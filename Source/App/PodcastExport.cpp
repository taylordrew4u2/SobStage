#include "PodcastExport.h"
#include "../Core/CombinedTakePlan.h"
#include "../Core/LoudnessMeter.h"
#include "../Core/SessionFolderNaming.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <thread>

#if defined (__APPLE__)
 #include <sys/resource.h>
#endif

namespace mma {

namespace {

/// Samples read and written at a time. Large enough that a four-hour take is
/// not millions of calls, small enough that a cancel is noticed promptly.
constexpr int kBlockSamples = 1 << 16;

constexpr double kPi = 3.14159265358979323846;

juce::String wholeLufs (double lufs)
{
    return juce::String (juce::roundToInt (lufs)) + " LUFS";
}

/// "MIX" for "MIX.wav".
juce::String stemOf (const juce::String& fileName)
{
    return fileName.containsChar ('.') ? fileName.upToLastOccurrenceOf (".", false, false)
                                       : fileName;
}

PodcastExportResult skipped (const juce::String& why)
{
    PodcastExportResult result;
    result.message = why;
    return result;
}

/// The peak of the signal around each sample, including between samples.
///
/// A file's samples are not its waveform: the reconstructed signal can pass
/// between two samples higher than either, and that is what a platform's
/// true-peak meter reads. This interpolates 4x with a windowed sinc -- the
/// same idea as BS.1770 Annex 2's filter, which LoudnessMeter's linear
/// estimate deliberately is not -- so the limiter below holds back the peaks
/// a platform will actually see.
class InterSamplePeak
{
public:
    static constexpr int kHalfTaps = 8;
    static constexpr int kTaps = 2 * kHalfTaps;

    /// push() answers for the sample this many pushes ago, because the
    /// interpolation needs to see that far past it.
    static constexpr int kDelay = kHalfTaps;

    InterSamplePeak()
    {
        // Taps i = 0..15 sit at offsets j = i - (kHalfTaps - 1) from the
        // sample whose following interval is being interpolated, at 1/4, 2/4
        // and 3/4 of the way to the next one.
        for (int phase = 0; phase < 3; ++phase)
        {
            const double t = (phase + 1) / 4.0;

            for (int i = 0; i < kTaps; ++i)
            {
                const double x = t - (i - (kHalfTaps - 1));
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
                const double window = 0.5 * (1.0 + std::cos (kPi * x / (kHalfTaps + 0.5)));
                coeffs[phase][i] = static_cast<float> (sinc * window);
            }
        }
    }

    float push (float x)
    {
        std::copy (history + 1, history + kTaps, history);
        history[kTaps - 1] = x;

        // The interval after the sample kDelay pushes ago...
        float following = 0.0f;
        for (int phase = 0; phase < 3; ++phase)
        {
            float y = 0.0f;
            for (int i = 0; i < kTaps; ++i)
                y += history[i] * coeffs[phase][i];

            following = std::max (following, std::abs (y));
        }

        // ...and the one before it, which was the last call's "following".
        const float peak = std::max ({ std::abs (history[kHalfTaps - 1]), following, previousFollowing });
        previousFollowing = following;
        return peak;
    }

private:
    float coeffs[3][kTaps] {};
    float history[kTaps] {};
    float previousFollowing = 0.0f;
};

/// A look-ahead peak limiter: turns the loudest moments down just far enough
/// that nothing -- between samples included -- goes over `ceilingDb`, and
/// leaves everything else alone.
///
/// The look-ahead is what makes it clean on speech. The gain starts falling a
/// few milliseconds before a peak arrives, so the peak is never chopped off --
/// it is turned down, and the turn-down is too short and too smooth to hear
/// as anything but a slightly softer consonant. Then it recovers over the
/// release, slowly enough not to pump.
///
/// The chain: required gain per sample, held at its minimum across the
/// look-ahead window, released smoothly, then averaged over that same window.
/// The hold and the average together guarantee the gain is already at or
/// below what a peak needs on the sample the peak lands on.
class LookAheadLimiter
{
public:
    LookAheadLimiter (double sampleRate, double ceilingDb)
        : ceiling (static_cast<float> (juce::Decibels::decibelsToGain (ceilingDb))),
          window (std::max (1, static_cast<int> (std::lround (sampleRate * kLookAheadSeconds)))),
          release (1.0 - std::exp (-1.0 / (sampleRate * kReleaseSeconds))),
          boxRing (static_cast<size_t> (window), 1.0f),
          delayLine (static_cast<size_t> (latency() + 1), 0.0f)
    {
        boxSum = static_cast<double> (window);
    }

    static constexpr double kLookAheadSeconds = 0.003;
    static constexpr double kReleaseSeconds = 0.08;

    /// How many samples the output trails the input by.
    int latency() const { return InterSamplePeak::kDelay + window - 1; }

    /// One sample in, the sample `latency()` ago out, limited.
    float process (float x)
    {
        delayLine[writePos] = x;
        writePos = (writePos + 1) % delayLine.size();

        const float peak = detector.push (x);
        const float required = peak > ceiling ? ceiling / peak : 1.0f;

        // Hold: the lowest required gain in the last `window` samples.
        while (! minima.empty() && minima.back().second >= required)
            minima.pop_back();
        minima.emplace_back (counter, required);
        while (minima.front().first <= counter - window)
            minima.pop_front();
        ++counter;

        const float held = minima.front().second;

        // Release: falls instantly, recovers smoothly.
        released = std::min (held, static_cast<float> (released + (1.0 - released) * release));

        // Average over the window, so the fall into a peak is a ramp.
        boxSum += released - boxRing[boxPos];
        boxRing[boxPos] = released;
        boxPos = (boxPos + 1) % boxRing.size();

        const float gain = std::min (1.0f, static_cast<float> (boxSum / window));
        lowestGain = std::min (lowestGain, gain);

        // writePos now points at the oldest sample: the one `latency()` ago.
        return delayLine[writePos] * gain;
    }

    /// The most the limiter turned anything down, in dB. Zero if never.
    double deepestReductionDb() const
    {
        return -juce::Decibels::gainToDecibels (lowestGain, -200.0f);
    }

private:
    float ceiling;
    int window;
    double release;

    InterSamplePeak detector;
    std::deque<std::pair<long long, float>> minima;
    long long counter = 0;
    float released = 1.0f;

    std::vector<float> boxRing;
    size_t boxPos = 0;
    double boxSum = 0.0;

    std::vector<float> delayLine;
    size_t writePos = 0;

    float lowestGain = 1.0f;
};

/// The limiter aims this far under the platform's ceiling, so a meter with a
/// different interpolation filter than the one above still reads under it.
constexpr double kCeilingMarginDb = 0.5;

/// Limiting deeper than this on a good part of the take is no longer catching
/// peaks: it is squashing the whole recording, and it sounds it.
constexpr double kMostLimitingDb = 10.0;

/// "A good part": the share of the take's sound, in 100 ms windows, that may
/// need more than kMostLimitingDb before the copy settles for less loudness.
constexpr double kMostLimitedShare = 0.2;

/// Windows quieter than this (in mean square, about -70 dBFS) are silence and
/// say nothing about how hard the sound would have to be limited.
constexpr double kSilentWindowMeanSquare = 1.0e-7;

/// The most times the copy is written while its loudness is brought onto the
/// target.
constexpr int kMostRenders = 3;

/// What one pass through the mix with gain and limiter came out as.
struct Rendered
{
    juce::String problem; ///< empty unless the pass failed
    double lufs = 0.0;
    double truePeakDbtp = 0.0;
    double deepestReductionDb = 0.0;
};

} // namespace

juce::String podcastCopyFileName (const juce::String& mixFileName, const std::string& targetName)
{
    return stemOf (mixFileName) + " - " + juce::String (SessionFolderNaming::sanitizeName (targetName)) + ".wav";
}

PodcastExportResult exportPodcastCopy (const juce::File& sessionFolder,
                                       const juce::String& mixFileName,
                                       const StreamingTarget& target,
                                       const std::atomic<bool>& cancel,
                                       const std::function<juce::File()>& currentFolder)
{
    const juce::String noCopy ("No podcast-ready copy was made: ");

    // Asked afresh every time a file in the take folder is opened, checked or
    // renamed: the folder can be renamed in Finder during a long copy, and a
    // path worked out at the start then names a folder that is not there.
    // Files already open carry on regardless.
    const auto folderNow = [&sessionFolder, &currentFolder]
    {
        return currentFolder != nullptr ? currentFolder() : sessionFolder;
    };

    // The mix as it is on the card: the first file, then every split part that
    // follows it -- the same list the combined video is laid against. Kept as
    // names, so each pass opens them wherever the folder is by then.
    const auto listed = folderNow();
    std::vector<std::string> namesInFolder;
    for (const auto& file : listed.findChildFiles (juce::File::findFiles, false))
        namesInFolder.push_back (file.getFileName().toStdString());

    std::vector<juce::String> parts;
    for (const auto& part : splitPartsInOrder (mixFileName.toStdString(), namesInFolder))
        parts.push_back (juce::String::fromUTF8 (part.c_str()));

    if (parts.empty())
        return skipped (noCopy + mixFileName + " wasn't in " + listed.getFileName() + ".");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    auto openPart = [&formats, &folderNow] (const juce::String& name)
    {
        return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (folderNow().getChildFile (name)));
    };

    double rate = 0.0;
    int channels = 0;

    for (const auto& part : parts)
    {
        auto reader = openPart (part);

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels < 1)
            return skipped (noCopy + part + " couldn't be read.");

        if (rate == 0.0)
        {
            rate = reader->sampleRate;
            channels = static_cast<int> (reader->numChannels);
        }
        else if (reader->sampleRate != rate || static_cast<int> (reader->numChannels) != channels)
        {
            return skipped (noCopy + "the parts of " + mixFileName + " don't match each other.");
        }
    }

    juce::AudioBuffer<float> buffer (channels, kBlockSamples);

    // Every sample of the mix, block by block, through `perSample`. False with
    // `problem` set when a part could not be read or the app is closing.
    auto forEachSample = [&] (const std::function<void (float)>& perSample, juce::String& problem)
    {
        for (const auto& part : parts)
        {
            auto reader = openPart (part);
            if (reader == nullptr)
            {
                problem = part + " couldn't be read.";
                return false;
            }

            for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += kBlockSamples)
            {
                if (cancel.load (std::memory_order_acquire))
                {
                    problem = "the app was closing.";
                    return false;
                }

                const auto count = static_cast<int> (juce::jmin<juce::int64> (kBlockSamples,
                                                                              reader->lengthInSamples - pos));
                if (! reader->read (&buffer, 0, count, pos, true, true))
                {
                    problem = part + " couldn't be read to the end.";
                    return false;
                }

                // The mix is mono, so the first channel is the mix, and the
                // copy is mono whatever else the file carries.
                const float* samples = buffer.getReadPointer (0);
                for (int i = 0; i < count; ++i)
                    perSample (samples[i]);
            }
        }

        return true;
    };

    // Pass one: measure -- the loudness, and the peak of each 100 ms, which is
    // how hard the limiter would have to work to reach the target.
    LoudnessMeter meter (rate);
    InterSamplePeak windowDetector;
    const int windowSamples = std::max (1, juce::roundToInt (rate * 0.1));
    std::vector<std::pair<float, double>> windows; // peak, mean square
    float windowPeak = 0.0f;
    double windowSquares = 0.0;
    int inWindow = 0;
    juce::int64 totalSamples = 0;

    auto closeWindow = [&]
    {
        if (inWindow > 0)
            windows.emplace_back (windowPeak, windowSquares / inWindow);

        windowPeak = 0.0f;
        windowSquares = 0.0;
        inWindow = 0;
    };

    auto intoWindow = [&] (float peak, float sample)
    {
        windowPeak = std::max (windowPeak, peak);
        windowSquares += static_cast<double> (sample) * sample;

        if (++inWindow == windowSamples)
            closeWindow();
    };

    {
        juce::String problem;
        std::vector<float> chunk;
        chunk.reserve (kBlockSamples);

        // The detector trails by kDelay, so its answers are paired with the
        // samples they are about, not the ones just pushed.
        std::deque<float> pending;

        const bool read = forEachSample ([&] (float x)
        {
            meter.process (&x, 1);
            ++totalSamples;
            pending.push_back (x);

            const float peak = windowDetector.push (x);
            if (static_cast<int> (pending.size()) > InterSamplePeak::kDelay)
            {
                intoWindow (peak, pending.front());
                pending.pop_front();
            }
        }, problem);

        if (! read)
            return skipped (noCopy + problem);

        while (! pending.empty())
        {
            intoWindow (windowDetector.push (0.0f), pending.front());
            pending.pop_front();
        }

        closeWindow();
    }

    const double lufs = meter.getIntegratedLufs();
    const double truePeak = meter.getTruePeakDbtp();
    const int blocks = meter.getBlockCount();

    if (blocks < kMinimumBlocksToJudge)
        return skipped (noCopy + "the take is too short to measure how loud it is.");

    if (lufs <= LoudnessMeter::kAbsoluteGateLufs)
        return skipped (noCopy + "the mix is silent, so there's nothing to set the loudness of.");

    const auto advice = adviseForTarget (target, lufs, truePeak, blocks);

    // adviseForTarget's own test for the two cases above. Both have been
    // answered already; this is here so a change to its rules cannot turn into
    // a copy written from a figure it would not stand behind.
    if (! advice.measurable)
        return skipped (noCopy + juce::String (advice.summary));

    // The gain the target wants, peaks or no peaks: the limiter takes care of
    // them. Unless reaching the target would mean limiting a good part of the
    // take by more than kMostLimitingDb -- then it is the recording that is too
    // peaky, not a few consonants, and the copy falls back to what the advice
    // has always offered: as loud as the peaks allow unlimited, and says so.
    const double wanted = monoTargetLufs (target);
    const double limiterCeilingDb = target.truePeakCeilingDbtp - kCeilingMarginDb;
    double gainDb = wanted - lufs;

    int sounding = 0;
    int squashed = 0;
    for (const auto& [peak, meanSquare] : windows)
    {
        if (meanSquare < kSilentWindowMeanSquare)
            continue;

        ++sounding;
        const double needed = juce::Decibels::gainToDecibels (peak, -200.0f) + gainDb - limiterCeilingDb;
        if (needed > kMostLimitingDb)
            ++squashed;
    }

    const bool fallBack = sounding > 0 && squashed > kMostLimitedShare * sounding;
    if (fallBack)
        gainDb = std::min (advice.gainDb, gainDb);

    // Never over anything: a copy from an earlier export, or a file someone
    // put there, keeps its name and this one takes the next free one.
    const auto wantedName = podcastCopyFileName (mixFileName, target.name);
    const auto stem = stemOf (wantedName).toStdString();
    const auto namingFolder = folderNow();
    const auto freeStem = SessionFolderNaming::resolveCollision (stem, [&] (const std::string& candidate)
    {
        return namingFolder.getChildFile (juce::String::fromUTF8 (candidate.c_str()) + ".wav").exists();
    });

    const auto outputName = juce::String::fromUTF8 (freeStem.c_str()) + ".wav";

    // Not .wav until it is finished, so nothing that lists the folder's audio
    // -- this app's own saved-take card included -- can count it before then.
    const auto workingName = outputName + ".part";
    const auto workingNow = [&folderNow, &workingName] { return folderNow().getChildFile (workingName); };

    // Pass two: the mix again, with the gain and the limiter, into the working
    // file -- measured on the way out, because the copy's loudness is the
    // claim, not the gain that was meant to produce it.
    auto render = [&] (double withGainDb) -> Rendered
    {
        Rendered rendered;
        const auto working = workingNow();
        working.deleteFile();

        auto stream = std::make_unique<juce::FileOutputStream> (working);

        if (stream->failedToOpen())
        {
            rendered.problem = "couldn't write to " + working.getParentDirectory().getFileName() + ".";
            return rendered;
        }

        // Ownership passes to the writer only when it is made; otherwise the
        // stream is still this function's to close.
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), rate, 1, 24, {}, 0));

        if (writer == nullptr)
        {
            stream.reset();
            workingNow().deleteFile();
            rendered.problem = "couldn't start a WAV file in " + working.getParentDirectory().getFileName() + ".";
            return rendered;
        }

        // Still reachable after the writer takes it, to see the end of it
        // reach the disk (below). The writer owns and deletes it.
        auto* const fileStream = stream.release();

        const float gain = juce::Decibels::decibelsToGain (static_cast<float> (withGainDb), -1000.0f);
        LookAheadLimiter limiter (rate, limiterCeilingDb);
        LoudnessMeter outMeter (rate);

        juce::AudioBuffer<float> out (1, kBlockSamples);
        int filled = 0;
        bool writeFailed = false;
        juce::int64 toSkip = limiter.latency();
        juce::int64 toWrite = totalSamples;

        auto emit = [&] (float y)
        {
            // The limiter's first `latency()` outputs are the silence it
            // started with, not the take.
            if (toSkip > 0) { --toSkip; return; }
            if (toWrite <= 0) return;
            --toWrite;

            out.setSample (0, filled++, y);

            if (filled == kBlockSamples)
            {
                outMeter.process (out.getReadPointer (0), static_cast<size_t> (filled));
                writeFailed = writeFailed || ! writer->writeFromAudioSampleBuffer (out, 0, filled);
                filled = 0;
            }
        };

        juce::String problem;
        const bool read = forEachSample ([&] (float x) { emit (limiter.process (x * gain)); }, problem);

        if (read)
        {
            for (int i = 0; i < limiter.latency(); ++i)
                emit (limiter.process (0.0f));

            if (filled > 0)
            {
                outMeter.process (out.getReadPointer (0), static_cast<size_t> (filled));
                writeFailed = writeFailed || ! writer->writeFromAudioSampleBuffer (out, 0, filled);
            }

            // Closing is what writes the header's final sizes; a writer that
            // cannot flush leaves a file that only looks finished.
            writeFailed = writeFailed || ! writer->flush();

            // The writer's flush only rewrites the header into the stream's
            // buffer: the tail of the audio and the header itself reached the
            // disk when the writer was deleted, where a failure is dropped.
            // So a card that filled on those last bytes gave a short file that
            // was renamed into place as finished. Pushed out and checked here.
            //
            // Measured where the folder is now: by the name it was opened
            // under, a folder renamed during the pass reads as an empty file,
            // and the copy was reported as the card running out of room.
            if (! writeFailed)
            {
                fileStream->flush();
                writeFailed = fileStream->getStatus().failed()
                           || workingNow().getSize() < fileStream->getPosition();
            }
        }

        writer.reset();

        if (! read || writeFailed)
        {
            workingNow().deleteFile();
            rendered.problem = read ? juce::String ("the card ran out of room or stopped answering "
                                                    "while it was being written.")
                                    : problem;
            return rendered;
        }

        rendered.lufs = outMeter.getIntegratedLufs();
        rendered.truePeakDbtp = outMeter.getTruePeakDbtp();
        rendered.deepestReductionDb = limiter.deepestReductionDb();
        return rendered;
    };

    auto rendered = render (gainDb);

    // Limiting takes some loudness with it -- a lot, when the peaks carry much
    // of the take's energy. When that leaves the copy off target, again with
    // the difference made up. Bounded: each pass is the whole take read and
    // written, and a take that has not landed in three is as close as it gets.
    for (int pass = 1; pass < kMostRenders && rendered.problem.isEmpty() && ! fallBack
                       && std::abs (rendered.lufs - wanted) > kOnTargetToleranceLu; ++pass)
    {
        gainDb += wanted - rendered.lufs;
        rendered = render (gainDb);
    }

    if (rendered.problem.isNotEmpty())
        return skipped (noCopy + rendered.problem);

    // Named where the folder is now. Named in the old place, a folder renamed
    // during the copy kept its finished copy hidden as ".part", and the next
    // launch deleted it as unfinished work.
    const auto finalFolder = folderNow();
    const auto working = finalFolder.getChildFile (workingName);
    const auto output = finalFolder.getChildFile (outputName);

    if (! working.moveFileTo (output))
    {
        working.deleteFile();
        return skipped (noCopy + "couldn't name the finished copy " + output.getFileName() + ".");
    }

    PodcastExportResult result;
    result.written = true;
    result.output = output;
    result.measuredLufs = lufs;
    result.measuredTruePeakDbtp = truePeak;
    result.gainDb = gainDb;
    result.limitedByTruePeak = fallBack;
    result.peakLimitingDb = rendered.deepestReductionDb;
    result.resultingLufs = rendered.lufs;
    result.resultingTruePeakDbtp = rendered.truePeakDbtp;

    result.message = "Podcast-ready copy saved: " + output.getFileName()
                   + " (" + wholeLufs (result.resultingLufs) + ")";

    // Short of the target is said, not left for someone to find on a meter:
    // the copy is as loud as it can be without squashing it, which is not as
    // loud as the platform asked.
    if (fallBack && result.resultingLufs < wanted - kOnTargetToleranceLu)
        result.message += ", quieter than " + juce::String (target.name) + " wants ("
                        + wholeLufs (wanted) + "): getting there would have meant squashing "
                          "much of the take by more than " + juce::String (juce::roundToInt (kMostLimitingDb))
                        + " dB.";
    else if (result.peakLimitingDb >= 1.0)
        result.message += ", with the loudest peaks turned down by up to "
                        + juce::String (juce::roundToInt (result.peakLimitingDb)) + " dB so they don't clip.";
    else
        result.message += ".";

    return result;
}

PodcastExporter::PodcastExporter()
    : state (std::make_shared<State>())
{
}

PodcastExporter::~PodcastExporter()
{
    cancel();
    waitUntilIdle (kShutdownWaitMs);
}

bool PodcastExporter::start (const juce::File& sessionFolder, const juce::String& mixFileName,
                             const std::string& targetName,
                             std::shared_ptr<TakeFolderTracker> followedFolder)
{
    const auto* target = findStreamingTarget (targetName);
    if (target == nullptr)
        return false;

    bool startWorker = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        // Quitting: nothing more may start, queued or otherwise.
        if (state->cancelling.load (std::memory_order_acquire))
            return false;

        state->queued.push_back ({ sessionFolder, mixFileName, *target, std::move (followedFolder) });

        if (! state->running)
        {
            state->running = true;
            startWorker = true;
        }
    }

    if (! startWorker)
        return true;

    // Detached, holding its own share of the state: the worker may be stuck
    // reading a card that went away, and nothing here may wait on that.
    try
    {
        std::thread ([s = state] { run (s); }).detach();
    }
    catch (...)
    {
        const std::lock_guard<std::mutex> lock (state->lock);
        state->running = false;
        state->queued.clear();
        state->finished.push_back ({});
        state->finished.back().message = "No podcast-ready copy was made: the app couldn't start "
                                         "the work. The take itself is saved.";
        state->idle.notify_all();
    }

    return true;
}

void PodcastExporter::run (std::shared_ptr<State> state)
{
   #if defined (__APPLE__)
    // The lowest disk priority, for this thread only -- what taskpolicy gives
    // the combine's ffmpeg. The copy reads the whole mix up to four times and
    // writes it up to three, on the card the next take is usually already
    // recording to, and at normal priority that competed with the take's
    // writer: ring fill, a fall back to mix-only, dropped video frames. With
    // nothing else writing it still runs at full speed. Best effort: a Mac
    // that refuses it makes the copy exactly as before.
    (void) setiopolicy_np (IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
   #endif

    for (;;)
    {
        Job job;

        {
            const std::lock_guard<std::mutex> lock (state->lock);

            // Decided under the same lock start() queues under, so a take is
            // either picked up by this worker or starts its own. Never neither.
            if (state->queued.empty() || state->cancelling.load (std::memory_order_acquire))
            {
                state->queued.clear();
                state->running = false;
                state->idle.notify_all();
                return;
            }

            job = std::move (state->queued.front());
            state->queued.pop_front();
        }

        // Where the take folder is now, followed through a rename in Finder;
        // where it was last known to be once it can't be found.
        auto lastKnown = job.sessionFolder;
        const std::function<juce::File()> folderNow = [&job, &lastKnown]
        {
            if (job.followedFolder != nullptr)
                if (const auto now = job.followedFolder->resolve(); ! now.empty())
                    lastKnown = juce::File (juce::String::fromUTF8 (now.c_str()));

            return lastKnown;
        };

        auto result = exportPodcastCopy (job.sessionFolder, job.mixFileName, job.target,
                                         state->cancelling, folderNow);

        const std::lock_guard<std::mutex> lock (state->lock);
        state->finished.push_back (std::move (result));
    }
}

std::vector<PodcastExportResult> PodcastExporter::collectFinished()
{
    const std::lock_guard<std::mutex> lock (state->lock);
    std::vector<PodcastExportResult> out;
    out.swap (state->finished);
    return out;
}

bool PodcastExporter::isRunning() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    return state->running;
}

void PodcastExporter::cancel() noexcept
{
    state->cancelling.store (true, std::memory_order_release);
}

bool PodcastExporter::waitUntilIdle (int timeoutMs)
{
    std::unique_lock<std::mutex> lock (state->lock);
    return state->idle.wait_for (lock, std::chrono::milliseconds (juce::jmax (0, timeoutMs)),
                                 [this] { return ! state->running; });
}

} // namespace mma
