#include "WritePipeline.h"
#include "Utf8Path.h"
#include <filesystem>
#include <system_error>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace mma {

namespace {

float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

/// How long the writer sleeps when the ring is empty. Short enough that stopping
/// is not perceptibly slow, long enough not to spin a core.
constexpr int kWriterIdleMs = 5;

/// Closes writers a failed start had already opened, and removes their files.
///
/// §6.5 and the comment this replaces both say a take that failed to start must
/// not leave half-written headers on the card "for the recovery pass to find" --
/// but only the close was ever done. A card with room for two stems and not the
/// third kept two header-only .wav files, which read as tracks that recorded
/// nothing rather than as a take that never began.
void discardPartiallyOpenedWriters (std::vector<std::unique_ptr<SessionWriter>>& writers)
{
    for (auto& w : writers)
    {
        if (w == nullptr)
            continue;

        const auto path = w->getCurrentFilePath();
        w->close();

        std::error_code ignored;
        std::filesystem::remove (pathFromUtf8 (path), ignored);
    }

    writers.clear();
}

} // namespace

WritePipeline::~WritePipeline()
{
    stop();
}

bool WritePipeline::start (const std::string& sessionFolder,
                           const std::vector<WriteChannelSpec>& channels,
                           double rate, int bitDepth,
                           const std::string& originTimestamp,
                           const std::string& mirrorFolder)
{
    // A fresh take starts with a clean slate: a failure from a previous one
    // must never make this one look doomed before it has written a byte.
    cardWriteFailed.store (false, std::memory_order_release);
    mirrorOutOfSpace.store (false, std::memory_order_release);
    mirrorWriteFailed.store (false, std::memory_order_release);
    mixOnly.store (false, std::memory_order_release);

    if (running.load (std::memory_order_acquire) || channels.empty())
        return false;

    numChannels = static_cast<int> (channels.size());
    sampleRate = rate;

    {
        const std::lock_guard<std::mutex> lock (liveFolderLock);
        liveSessionFolder = sessionFolder;
    }
    openedSessionFolder = sessionFolder;
    secondsSinceFolderCheck = 0.0;

    // §6.3: 30 seconds at the current channel count and rate, minimum 64 MB.
    ring.reset (RingBuffer::minimumCapacitySamples (rate, numChannels));

    stemWriters.clear();
    trimGains.clear();
    startProblem.clear();
    mirrorFailedToOpen.store (false, std::memory_order_release);

    for (const auto& spec : channels)
    {
        auto writer = std::make_unique<SessionWriter>();

        // §6.1: stems are mono, one file per microphone, at unity gain.
        // §2.3: this stem's own depth when the device behind it named one,
        // and the take's otherwise.
        const int stemDepth = spec.bitDepth > 0 ? spec.bitDepth : bitDepth;

        if (! writer->open (sessionFolder + "/" + spec.fileName, rate, 1, stemDepth, originTimestamp))
        {
            // Close AND remove what did open. A take that failed to start must
            // not leave half-written headers on the card for the recovery pass
            // to find -- which this said and did not do.
            discardPartiallyOpenedWriters (stemWriters);

            // §10.6: what happened, then what to do. The file is named because
            // "recording could not start" is not something a user can act on,
            // and the folder is what they need to look at.
            // The writer's own account when it has one -- it knows whether the
            // drive was out of room, and "check the card is plugged in" sends
            // someone to the wrong place when it is merely full.
            startProblem = ! writer->getWriteProblem().empty()
                             ? writer->getWriteProblem()
                             : "Couldn't start writing to " + sessionFolder + ". The file "
                                 + spec.fileName + " couldn't be created -- check the card is "
                                   "still plugged in and has room, and isn't locked.";
            return false;
        }

        stemWriters.push_back (std::move (writer));
        trimGains.push_back (dbToGain (spec.trimDb));
    }

    mixWriter = std::make_unique<SessionWriter>();

    if (! mixWriter->open (sessionFolder + "/MIX", rate, 1, bitDepth, originTimestamp))
    {
        for (auto& opened : stemWriters)
            opened->close();

        // Read BEFORE the writer is released. Asking a reset unique_ptr for its
        // account dereferences null, and the only reason that did not crash on
        // the way in was that a stem's open fails first on a full card, so this
        // branch was never reached by the case it was written for.
        auto why = mixWriter->getWriteProblem();

        discardPartiallyOpenedWriters (stemWriters);

        {
            const auto mixPath = mixWriter->getCurrentFilePath();
            mixWriter->close();
            mixWriter.reset();

            std::error_code ignored;
            std::filesystem::remove (pathFromUtf8 (mixPath), ignored);
        }

        startProblem = ! why.empty()
                         ? std::move (why)
                         : "Couldn't start writing to " + sessionFolder + ". The mixed file "
                             "couldn't be created -- check the card is still plugged in and has "
                             "room, and isn't locked.";
        return false;
    }

    // §6.3: the mirror is a safety net, so failing to open it degrades to
    // card-only rather than failing a take the user is waiting to start.
    mirrorStemWriters.clear();
    mirrorMixWriter.reset();
    mirroring.store (false, std::memory_order_release);

    if (! mirrorFolder.empty())
    {
        if (openMirrorWriters (mirrorFolder, channels, rate, bitDepth, originTimestamp))
        {
            mirroring.store (true, std::memory_order_release);
        }
        else
        {
            // Still not an error -- §6.3 will not fail a take over the safety
            // net -- but no longer a secret. Nothing raised anything here, so
            // the take ran card-only while every caller that asked went on
            // being told a mirror was active.
            mirrorFailedToOpen.store (true, std::memory_order_release);
        }
    }

    // std::atomic is not copyable, so the vector is built in place.
    std::vector<std::atomic<bool>> live (static_cast<size_t> (numChannels));
    for (auto& l : live)
        l.store (true, std::memory_order_relaxed);
    channelLive = std::move (live);

    // Audio-thread scratch, sized once here so pushBlock never allocates (§11).
    interleaveScratch.assign (kInterleaveChunkFrames * static_cast<size_t> (numChannels), 0.0f);

    // Writer-thread scratch, sized once here so the drain loop never allocates.
    const size_t drainFrames = 4096;
    drainBuffer.assign (drainFrames * static_cast<size_t> (numChannels), 0.0f);
    stemScratch.assign (drainFrames, 0.0f);
    mixScratch.assign (drainFrames, 0.0f);

    // §6.1: a fresh MIX.wav limiter per take, at unity. Its gain carries from
    // one drained chunk to the next, so a peak at a chunk's edge is turned
    // down as smoothly as one in the middle.
    mixLimiter = MixBusLimiter (rate);

    // Every channel's alignment line, and the hand-off for changes to it.
    // Offsets start unpublished, so the first block the audio thread hands
    // over queues each channel's starting offset at frame zero.
    aligner.prepare (numChannels);
    offsetQueue.clear();
    publishedOffsets.assign (static_cast<size_t> (numChannels), -1);
    alignmentDeferred.store (false, std::memory_order_relaxed);
    framesDrained = 0;
    chunkEvents.clear();
    chunkEvents.reserve (StemOffsetQueue::kCapacity);

    {
        // Constructed here because this is where the rate is known, and the
        // K-weighting filter has to be built for the rate it will actually see.
        const std::lock_guard<std::mutex> lock (loudnessLock);
        loudness = std::make_unique<LoudnessMeter> (rate);
    }

    framesAccepted.store (0, std::memory_order_relaxed);
    framesWritten.store (0, std::memory_order_relaxed);
    framesDropped.store (0, std::memory_order_relaxed);
    peakWritten.store (0.0f, std::memory_order_relaxed);

    running.store (true, std::memory_order_release);
    writerThread = std::thread ([this] { runWriterThread(); });
    return true;
}

bool WritePipeline::pushBlock (const float* const* channelData, int numChannels_, int numSamples,
                               const int* channelOffsets, const StemOffsetKind* channelOffsetKinds) noexcept
{
    if (! running.load (std::memory_order_acquire) || numSamples <= 0)
        return false;

    // A block whose channel count does not match the take's is still written.
    //
    // This used to reject the whole block. The file layout is fixed for the
    // take (§6.5) and cannot widen or narrow, which is true -- but throwing the
    // block away does not preserve the layout, it just loses every channel that
    // DID arrive, for as long as the mismatch lasts. When a device delivers
    // fewer channels than the take expects, that is the entire recording.
    //
    // It presented on a real Mac as silent files while the meters moved: the
    // metering loop reads the same buffer a moment later and has no equivalent
    // check, so the app showed live level for audio it was dropping. §0.1 makes
    // unreported loss the one unacceptable failure, and this was the largest
    // possible version of it.
    //
    // §6.5 already settles what to do, for the case where a mic is unplugged
    // mid-take: the layout holds and the absent channel writes silence. A short
    // block is the same problem and gets the same answer. A block WIDER than
    // the take is different -- those samples have nowhere to go, so they are
    // genuinely lost and are counted.
    const int usableChannels = std::min (numChannels_, numChannels);

    if (numChannels_ > numChannels)
        framesDropped.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);

    // Interleave straight into the ring. Writing per-frame keeps the audio
    // thread free of any temporary buffer, which §11 would not allow it to
    // allocate anyway.
    const size_t needed = static_cast<size_t> (numSamples) * static_cast<size_t> (numChannels);

    if (ring.availableForWrite() < needed)
    {
        // §0.1: a dropped sample is the one unacceptable failure, so this is
        // counted rather than quietly ignored.
        framesDropped.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
        return false;
    }

    // Alignment changes, queued before the block they apply from is written:
    // a writer that reads the block has then already been handed them. Only
    // the channels whose offset moved, so a take queues a handful in all. A
    // full queue -- the writer stalled on the card while offsets kept moving
    // -- leaves the change to be queued with a later block, and the take's
    // alignment is reported as not exact. Nothing here allocates (§11).
    if (channelOffsets != nullptr)
    {
        const auto frame = framesAccepted.load (std::memory_order_relaxed);

        // As many as the caller's block carries, like its audio.
        for (int ch = 0; ch < usableChannels && ch < static_cast<int> (publishedOffsets.size()); ++ch)
        {
            const int wanted = channelOffsets[ch];
            auto& published = publishedOffsets[static_cast<size_t> (ch)];

            if (wanted == published)
                continue;

            const auto kind = channelOffsetKinds != nullptr ? channelOffsetKinds[ch] : StemOffsetKind::settled;

            if (offsetQueue.push ({ frame, ch, wanted, kind }))
                published = wanted;
            else
                alignmentDeferred.store (true, std::memory_order_relaxed);
        }
    }

    // Interleave in chunks. The whole block is known to fit -- that was checked
    // above, and this is the ring's only producer -- so every chunk lands whole.
    //
    // The per-sample ring.write this replaces cost a modulo and an atomic
    // release-store for every sample of every channel: at 8 mics and 48 kHz that
    // is 384,000 release-stores a second, on the one thread that must never miss
    // its deadline.
    int frameOffset = 0;

    while (frameOffset < numSamples)
    {
        const int chunkFrames = std::min (static_cast<int> (kInterleaveChunkFrames),
                                          numSamples - frameOffset);
        float* destination = interleaveScratch.data();

        // The loudest sample this take has written, tracked as the samples go
        // past rather than by reading the files back afterwards. A take whose
        // stems are full length and completely flat is indistinguishable from a
        // good one by size alone, and that is what let the app hand over
        // silence saying "Saved." -- see judgeTakeAudio.
        //
        // A local float through the loop, one relaxed atomic store per chunk:
        // §11 allows no locking here, and this costs a compare per sample on a
        // value already in a register.
        float chunkPeak = 0.0f;

        for (int f = 0; f < chunkFrames; ++f)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                // §6.5: an unplugged mic writes silence into its existing channel
                // rather than the file layout changing mid-recording. A channel
                // the block does not carry at all is the same case.
                const bool live = ch < usableChannels
                               && channelLive[static_cast<size_t> (ch)].load (std::memory_order_relaxed);
                const float sample = (live && channelData[ch] != nullptr)
                                         ? channelData[ch][frameOffset + f]
                                         : 0.0f;

                *destination++ = sample;

                const float magnitude = sample < 0.0f ? -sample : sample;

                if (magnitude > chunkPeak)
                    chunkPeak = magnitude;
            }
        }

        // Monotonic max. This is the only producer, so a plain load/store pair
        // cannot lose an update to a racing writer.
        if (chunkPeak > peakWritten.load (std::memory_order_relaxed))
            peakWritten.store (chunkPeak, std::memory_order_relaxed);

        ring.write (interleaveScratch.data(),
                    static_cast<size_t> (chunkFrames) * static_cast<size_t> (numChannels));

        frameOffset += chunkFrames;
    }

    framesAccepted.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
    return true;
}

void WritePipeline::setChannelTrimDb (int channelIndex, float trimDb) noexcept
{
    if (channelIndex < 0 || channelIndex >= static_cast<int> (trimGains.size()))
        return;

    // Single float store into an already-sized vector: the writer thread reads
    // either the old gain or the new one, never a torn or reallocated value.
    trimGains[static_cast<size_t> (channelIndex)] = dbToGain (trimDb);
}

void WritePipeline::setChannelLive (int channelIndex, bool live) noexcept
{
    if (channelIndex >= 0 && channelIndex < static_cast<int> (channelLive.size()))
        channelLive[static_cast<size_t> (channelIndex)].store (live, std::memory_order_relaxed);
}

bool WritePipeline::openMirrorWriters (const std::string& mirrorFolder,
                                       const std::vector<WriteChannelSpec>& channels,
                                       double rate, int bitDepth,
                                       const std::string& originTimestamp)
{
    for (const auto& spec : channels)
    {
        auto writer = std::make_unique<SessionWriter>();

        // The mirror is a second copy of the same take, so it is written at
        // the same per-stem depth -- a backup in a different format is not a
        // backup of what was recorded (§6.3).
        const int stemDepth = spec.bitDepth > 0 ? spec.bitDepth : bitDepth;

        if (! writer->open (mirrorFolder + "/" + spec.fileName, rate, 1, stemDepth, originTimestamp))
        {
            // Discarded, not merely released. Letting the unique_ptrs die runs
            // ~SessionWriter -> close(), which FINALIZES each file and leaves
            // it -- so a mirror that failed to open left a full set of
            // header-only .wav stubs in the backup folder. A backup that looks
            // present and holds nothing is worse than an absent one, which is
            // why the card path has used this helper all along.
            discardPartiallyOpenedWriters (mirrorStemWriters);
            return false;
        }

        mirrorStemWriters.push_back (std::move (writer));
    }

    auto mix = std::make_unique<SessionWriter>();

    if (! mix->open (mirrorFolder + "/MIX", rate, 1, bitDepth, originTimestamp))
    {
        // The stems opened; only the mix did not. Both go, and so does the
        // half-opened mix itself, which the old code let fall out of scope
        // still holding whatever it had managed to create.
        discardPartiallyOpenedWriters (mirrorStemWriters);

        if (mix != nullptr)
        {
            const auto path = mix->getCurrentFilePath();
            mix->close();

            std::error_code ignored;
            std::filesystem::remove (pathFromUtf8 (path), ignored);
        }

        return false;
    }

    mirrorMixWriter = std::move (mix);
    return true;
}

void WritePipeline::fallBackToMixOnly() noexcept
{
    mixOnly.store (true, std::memory_order_release);
}

void WritePipeline::stopMirroring()
{
    // Only flips the flag. The writer thread owns the files, so it closes them
    // on its next pass rather than this being a cross-thread close.
    mirroring.store (false, std::memory_order_release);
}

void WritePipeline::drainOnce (bool finalFlush)
{
    const size_t framesCapacity = stemScratch.size();

    // Read once per pass: if the mirror is stopped mid-drain, this pass still
    // completes coherently and the next one closes the files.
    const bool mirrorActiveForThisPass = mirroring.load (std::memory_order_acquire);

    if (! mirrorActiveForThisPass && ! mirrorStemWriters.empty())
    {
        // §6.3: stopped for space, or stopped by the user. Close the partial
        // copy properly so its headers are valid rather than truncated.
        //
        // Judged, like every other close. This is the one place mirror files
        // are finalized outside stop(), and its results were the two in this
        // file still thrown away -- so a partial copy whose headers failed to
        // land was left looking like a partial copy that closed cleanly, which
        // is the difference between a short file that plays and one that does
        // not open at all.
        bool finalizeFailed = false;

        // Asked before the writers are released, which is the only moment the
        // answer still exists.
        for (const auto& w : mirrorStemWriters)
            if (w != nullptr && w->ranOutOfSpace())
                mirrorOutOfSpace.store (true, std::memory_order_release);

        if (mirrorMixWriter != nullptr && mirrorMixWriter->ranOutOfSpace())
            mirrorOutOfSpace.store (true, std::memory_order_release);

        for (auto& w : mirrorStemWriters)
            if (! w->close())
                finalizeFailed = true;

        mirrorStemWriters.clear();

        if (mirrorMixWriter != nullptr)
        {
            if (! mirrorMixWriter->close())
                finalizeFailed = true;

            mirrorMixWriter.reset();
        }

        if (finalizeFailed)
            mirrorWriteFailed.store (true, std::memory_order_release);
    }

    for (;;)
    {
        const size_t availableSamples = ring.availableForRead();
        const size_t availableFrames = availableSamples / static_cast<size_t> (numChannels);

        if (availableFrames == 0)
            return;

        const size_t frames = std::min (availableFrames, framesCapacity);
        const size_t samples = frames * static_cast<size_t> (numChannels);

        if (ring.read (drainBuffer.data(), samples) != samples)
        {
            // Unreachable while this is the ring's only consumer, which is
            // exactly why it is counted rather than returned from in silence:
            // if a second consumer ever appears, the loss shows up in the
            // take's own dropped-frame figure instead of nowhere.
            framesDropped.fetch_add (static_cast<uint64_t> (samples / std::max (1, numChannels)),
                                     std::memory_order_relaxed);
            return;
        }

        std::fill (mixScratch.begin(), mixScratch.begin() + static_cast<long> (frames), 0.0f);

        // The alignment changes that fall inside these frames. The audio
        // thread queued each one before the block it applies from, so every
        // one up to the last frame read is here to be seen; any later ones
        // stay queued for the chunk they fall in.
        const uint64_t chunkStart = framesDrained;
        chunkEvents.clear();
        {
            StemOffsetEvent event;
            while (chunkEvents.size() < chunkEvents.capacity()
                   && offsetQueue.peek (event) && event.frame < chunkStart + frames)
            {
                chunkEvents.push_back (event);
                offsetQueue.pop();
            }
        }
        framesDrained += frames;

        // §6.5: once degraded, the stems stop but the mix is still summed from
        // every channel -- the point is to shed write bandwidth, not to lose
        // anyone from the recording that survives.
        const bool writeStems = ! mixOnly.load (std::memory_order_acquire);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float gain = trimGains[static_cast<size_t> (ch)];

            for (size_t f = 0; f < frames; ++f)
            {
                // A NaN or infinity is a driver fault, not audio. Left alone,
                // infinity became a full-scale click in the stem and a click at
                // the ceiling in the mix, and one NaN wiped out the take's
                // loudness figure for good. Silence is the only honest value.
                const float raw = drainBuffer[f * static_cast<size_t> (numChannels) + static_cast<size_t> (ch)];

                // §4: the stem is unity gain, always. A bad trim decision must
                // not be baked into the raw material.
                stemScratch[f] = std::isfinite (raw) ? raw : 0.0f;
            }

            // Lined up with the slowest device before anything is written or
            // summed, so the stems and the mix agree with each other. Each
            // change takes effect at the frame it was queued for.
            {
                size_t done = 0;

                for (const auto& event : chunkEvents)
                {
                    if (event.channel != ch)
                        continue;

                    const auto at = static_cast<size_t> (std::min<uint64_t> (
                        event.frame > chunkStart ? event.frame - chunkStart : 0, frames));

                    if (at > done)
                    {
                        aligner.process (ch, stemScratch.data() + done, at - done);
                        done = at;
                    }

                    aligner.setOffset (ch, event.offset, event.kind);
                }

                aligner.process (ch, stemScratch.data() + done, frames - done);
            }

            // §5.1: unity summing, no attenuation with channel count. Trim
            // applies here and only here -- to the aligned samples, so the
            // mix carries a clap once, not once per device's latency.
            for (size_t f = 0; f < frames; ++f)
                mixScratch[f] += stemScratch[f] * gain;

            if (! writeStems)
                continue;

            // §6.5: the return value is the card telling us it has gone. It was
            // discarded here, which is why pulling a card mid-take used to look
            // exactly like a healthy recording.
            if (! stemWriters[static_cast<size_t> (ch)]->writeInterleaved (stemScratch.data(), frames))
                cardWriteFailed.store (true, std::memory_order_release);

            // §6.3: same frames, second destination. Written from the same
            // scratch so the copy cannot diverge from the original.
            if (mirrorActiveForThisPass && ch < static_cast<int> (mirrorStemWriters.size()))
                if (! mirrorStemWriters[static_cast<size_t> (ch)]->writeInterleaved (stemScratch.data(), frames))
                {
                    if (mirrorStemWriters[static_cast<size_t> (ch)]->ranOutOfSpace())
                        mirrorOutOfSpace.store (true, std::memory_order_release);

                    mirrorWriteFailed.store (true, std::memory_order_release);
                }
        }

        // §6.1: the mix file gets its own limiter instance at -1 dBFS, separate
        // from the monitor limiter, so a monitor mute never silences the file.
        for (size_t f = 0; f < frames; ++f)
            mixScratch[f] = mixLimiter.processSample (mixScratch[f]);

        // Measured from the same buffer that is about to be written, after the
        // limiter, so the loudness reported is the loudness of the file.
        {
            const std::lock_guard<std::mutex> lock (loudnessLock);

            if (loudness != nullptr)
                loudness->process (mixScratch.data(), frames);
        }

        if (! mixWriter->writeInterleaved (mixScratch.data(), frames))
            cardWriteFailed.store (true, std::memory_order_release);
        else
            // This is also the writer-completion boundary: every destination
            // stem for these frames was handled above. Ring emptiness cannot
            // provide that fact because read() publishes space before the file
            // writes have completed.
            framesWritten.fetch_add (static_cast<uint64_t> (frames),
                                     std::memory_order_release);

        progress.fetch_add (1, std::memory_order_release);

        if (chunkHookForTesting)
            chunkHookForTesting();

        if (mirrorActiveForThisPass && mirrorMixWriter != nullptr)
            if (! mirrorMixWriter->writeInterleaved (mixScratch.data(), frames))
            {
                if (mirrorMixWriter->ranOutOfSpace())
                    mirrorOutOfSpace.store (true, std::memory_order_release);

                mirrorWriteFailed.store (true, std::memory_order_release);
            }

        const double elapsed = static_cast<double> (frames) / sampleRate;

        // §6.6 rewrites each header every 5 seconds so an interrupted file stays
        // playable. Its result was thrown away, so the write that keeps a
        // four-hour take recoverable could start failing on a dying card and
        // nothing would notice until the stop -- by which point the header
        // being wrong is the whole loss. Judged the same way the audio writes
        // above are, and kept on the same side of the card/mirror line.
        if (writeStems)
            for (auto& w : stemWriters)
                if (! w->tick (elapsed))
                    cardWriteFailed.store (true, std::memory_order_release);

        if (! mixWriter->tick (elapsed))
            cardWriteFailed.store (true, std::memory_order_release);

        if (mirrorActiveForThisPass)
        {
            // Once pressure has forced mix-only, the mirror's stems stop too.
            // Continuing their periodic durable header rewrites would retain
            // avoidable I/O precisely when the emergency mode is trying to shed
            // it. They are finalized normally at stop().
            if (writeStems)
                for (auto& w : mirrorStemWriters)
                    if (! w->tick (elapsed))
                        mirrorWriteFailed.store (true, std::memory_order_release);

            if (mirrorMixWriter != nullptr && ! mirrorMixWriter->tick (elapsed))
                mirrorWriteFailed.store (true, std::memory_order_release);
        }

        // About once a second of audio: has the take's folder moved? Cheap
        // (one fcntl), and on the writer thread, which owns the descriptor.
        secondsSinceFolderCheck += elapsed;
        if (secondsSinceFolderCheck >= 1.0)
        {
            secondsSinceFolderCheck = 0.0;
            refreshLiveSessionFolder();
        }

        // §6.3: a mirror that has failed stops for the rest of the take, the
        // same way one that ran out of room does. The card write is untouched:
        // the mirror must never take the recording down with it.
        if (mirrorWriteFailed.load (std::memory_order_acquire) && mirroring.load (std::memory_order_acquire))
            stopMirroring();

        // §6.5: the destination is gone. Draining further would loop writing
        // into a dead handle; the owner stops and finalizes the take.
        if (cardWriteFailed.load (std::memory_order_acquire))
            return;

        if (! finalFlush && frames < framesCapacity)
            return;
    }
}

void WritePipeline::refreshLiveSessionFolder()
{
    if (mixWriter == nullptr)
        return;

    const auto filePath = mixWriter->resolveCurrentFilePath();
    if (filePath.empty())
        return;

    const auto resolved = pathFromUtf8 (filePath).parent_path();
    if (resolved.empty())
        return;

    // The system answers with the resolved path, so a folder reached through
    // a symlink (/tmp on a Mac is /private/tmp) would read as moved when it
    // has not. Still the same folder: keep the spelling the take was given.
    std::error_code ec;
    const bool unmoved = std::filesystem::equivalent (pathFromUtf8 (openedSessionFolder), resolved, ec)
                      && ! ec;
    const auto folder = unmoved ? openedSessionFolder : utf8FromPath (resolved);

    const std::lock_guard<std::mutex> lock (liveFolderLock);
    liveSessionFolder = folder;
}

std::string WritePipeline::getLiveSessionFolder() const
{
    const std::lock_guard<std::mutex> lock (liveFolderLock);
    return liveSessionFolder;
}

double WritePipeline::getIntegratedLufs() const
{
    const std::lock_guard<std::mutex> lock (loudnessLock);
    return loudness != nullptr ? loudness->getIntegratedLufs() : LoudnessMeter::kSilenceLufs;
}

double WritePipeline::getTruePeakDbtp() const
{
    const std::lock_guard<std::mutex> lock (loudnessLock);
    return loudness != nullptr ? loudness->getTruePeakDbtp() : LoudnessMeter::kSilenceLufs;
}

int WritePipeline::getLoudnessBlockCount() const
{
    const std::lock_guard<std::mutex> lock (loudnessLock);
    return loudness != nullptr ? loudness->getBlockCount() : 0;
}

void WritePipeline::runWriterThread()
{
    while (running.load (std::memory_order_acquire))
    {
        drainOnce (false);
        std::this_thread::sleep_for (std::chrono::milliseconds (kWriterIdleMs));
    }
}

std::string WritePipeline::getCardWriteProblem() const
{
    // The mix writer first: it is the one every take has, and the one whose
    // failure stops the take. A stem's account is used only when the mix has
    // none of its own.
    if (mixWriter != nullptr && ! mixWriter->getWriteProblem().empty())
        return mixWriter->getWriteProblem();

    for (const auto& w : stemWriters)
        if (w != nullptr && ! w->getWriteProblem().empty())
            return w->getWriteProblem();

    // The writers are gone after stop(); what they said is not.
    return cardWriteProblemAtStop;
}

void WritePipeline::stop()
{
    if (! running.exchange (false, std::memory_order_acq_rel))
        return;

    if (writerThread.joinable())
        writerThread.join();

    // Everything still in the ring belongs to the take. §0.1: audio is never
    // lost, including the last block before stop.
    drainOnce (true);

    // Every close() is judged, and the card's and the mirror's are judged
    // separately -- §6.3 keeps the two apart everywhere else and the finalize
    // is no exception. A stop is the last chance to notice that the card went
    // away, and closing was the one write that had never been checked: a card
    // pulled during the stop left files whose headers say they hold no audio,
    // under a take the app had already reported as saved.
    // Latched before the writers are destroyed, because that is where the
    // account lives. Without this the sentence exists only while the take is
    // running and is gone by the time anyone asks what went wrong.
    cardWriteProblemAtStop = getCardWriteProblem();

    // The last word on where the take's folder is, while its files are still
    // open to be asked: the stop-time session.json goes there.
    refreshLiveSessionFolder();

    bool cardFinalizeFailed = false;

    for (auto& w : stemWriters)
    {
        if (! w->close())
            cardFinalizeFailed = true;
        progress.fetch_add (1, std::memory_order_release);
    }

    stemWriters.clear();

    if (mixWriter != nullptr)
    {
        if (! mixWriter->close())
            cardFinalizeFailed = true;
        progress.fetch_add (1, std::memory_order_release);

        mixWriter.reset();
    }

    if (cardFinalizeFailed)
        cardWriteFailed.store (true, std::memory_order_release);

    // §6.3: the mirror is closed last and the same way, so a take that ended
    // normally leaves two complete, independently valid copies.
    bool mirrorFinalizeFailed = false;

    for (auto& w : mirrorStemWriters)
    {
        if (! w->close())
            mirrorFinalizeFailed = true;
        progress.fetch_add (1, std::memory_order_release);
    }

    mirrorStemWriters.clear();

    if (mirrorMixWriter != nullptr)
    {
        if (! mirrorMixWriter->close())
            mirrorFinalizeFailed = true;

        mirrorMixWriter.reset();
    }

    // Only counted when a mirror was actually running: closing writers that
    // were never opened is not a mirror failure, and reporting one would tell
    // the user a backup broke when they never asked for one.
    if (mirrorFinalizeFailed && mirroring.load (std::memory_order_acquire))
        mirrorWriteFailed.store (true, std::memory_order_release);

    mirroring.store (false, std::memory_order_release);
}

} // namespace mma
