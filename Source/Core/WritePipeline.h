#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "RingBuffer.h"
#include "SessionWriter.h"
#include "MixBusLimiter.h"
#include "LoudnessMeter.h"
#include "StemAligner.h"

namespace mma {

struct WriteChannelSpec
{
    std::string fileName; // "01_Yeti-Kitchen", already sanitized per §6.2
    float trimDb = 0.0f;  // §4: affects the mix file only, never the stem

    /// §2.3: the depth THIS stem is written at, which follows the capability of
    /// the device behind it -- "do not upconvert; it adds file size and no
    /// information". Per channel, because a rig can mix a 16-bit microphone
    /// with a 24-bit interface and there is no one answer that serves both:
    /// picking 24 pads the first, picking 16 discards from the second.
    ///
    /// Zero means "use the take's depth", which is what every caller that has
    /// no per-device answer passes -- the manual override, and the two backends
    /// that do not report capability yet.
    int bitDepth = 0;
};

/// §6.3 write pipeline: the audio callback hands blocks to a lock-free ring
/// buffer and a dedicated writer thread drains it to disk. §11 forbids
/// allocation, locking, logging and file I/O in the callback, so everything the
/// audio thread touches here is sized up front and lock-free.
class WritePipeline
{
public:
    ~WritePipeline();

    /// Opens one stem per channel plus MIX, all sharing a session origin so a
    /// DAW aligns them on import (§6.1).
    /// mirrorFolder is §6.3's redundant local copy. Empty means no mirror.
    /// A mirror that cannot be opened is not an error: §6.3 makes the mirror a
    /// safety net, so losing it degrades to card-only rather than failing the
    /// take.
    bool start (const std::string& sessionFolder,
                const std::vector<WriteChannelSpec>& channels,
                double sampleRate, int bitDepth,
                const std::string& originTimestamp,
                const std::string& mirrorFolder = {});

    /// §6.3: the mirror stops when the internal drive runs low, and never
    /// restarts within the same take -- a copy with a hole in it is not a copy.
    void stopMirroring();
    bool isMirroring() const noexcept { return mirroring.load (std::memory_order_acquire); }

    /// Audio-thread entry point. Real-time safe: no allocation, no locking, no
    /// file I/O. Returns false when the ring buffer could not take the whole
    /// block, which is a dropout and must be reported (§0.1, §6.5).
    ///
    /// `channelOffsets`, when given, is how far each channel is to be held
    /// back in its stem (and so in the mix) to line up with the slowest
    /// device, from this block on: the audio arrives as early as each device
    /// allows, because the headphones are fed from the same samples and must
    /// not wait (§5.4), and the writer lines it up instead (StemAligner). A
    /// change is handed to the writer tagged with the frame it applies from,
    /// so it lands sample-exactly. Null leaves every offset where it is.
    /// `channelOffsetKinds`, when given beside them, says for each channel
    /// whether a change is for a block its device has not confirmed, takes
    /// such a change back, or is room the channel's own stream gave back
    /// (StemOffsetKind) -- `channelReleased` samples of it; null means every
    /// change is settled.
    bool pushBlock (const float* const* channelData, int numChannels, int numSamples,
                    const int* channelOffsets = nullptr,
                    const StemOffsetKind* channelOffsetKinds = nullptr,
                    const int* channelReleased = nullptr) noexcept;

    /// How the stems were lined up, per channel: the offset in force now and
    /// the one the take started with, the silence written mid-take to hold a
    /// channel back further when another device's IO block grew, and the
    /// samples taken out to bring one forward when its own did. Any thread;
    /// still answers after stop().
    int getChannelAlignmentOffset (int channel) const noexcept { return aligner.getOffset (channel); }
    int getChannelStartAlignmentOffset (int channel) const noexcept { return aligner.getStartOffset (channel); }
    bool hasChannelAlignmentStarted (int channel) const noexcept { return aligner.hasStarted (channel); }
    uint64_t getChannelAlignmentSilence (int channel) const noexcept { return aligner.getSilenceInserted (channel); }
    uint64_t getChannelAlignmentDropped (int channel) const noexcept { return aligner.getSamplesDropped (channel); }

    /// True when every offset asked for was applied exactly, at the frame it
    /// was asked for. False when one had to be clamped, or a change could not
    /// be queued at once and landed late.
    bool isAlignmentExact() const noexcept
    {
        return aligner.isExact() && ! alignmentDeferred.load (std::memory_order_relaxed);
    }

    /// §6.5: at 90% ring fill, the stems stop on both destinations and the
    /// mix keeps going. A mirror is fed from the same ring and writer thread, so
    /// it cannot protect stems from a ring overflow. A complete mix is worth
    /// more than eight stems that all have the same hole in them, and the ring
    /// draining at a fraction of the byte rate is what stops the hole growing.
    ///
    /// Never restarts within a take, for the same reason the mirror does not
    /// (§6.3): stems that resume mid-file are worse than stems that stopped,
    /// because the gap is invisible in the waveform.
    void fallBackToMixOnly() noexcept;
    bool isMixOnly() const noexcept { return mixOnly.load (std::memory_order_acquire); }

    /// §6.5: a channel whose mic is unplugged writes silence rather than
    /// changing the file layout mid-recording.
    void setChannelLive (int channelIndex, bool live) noexcept;

    /// §4: trim moves the mix file, never the stems. Live, because the user can
    /// turn a mic down mid-take and the mix should follow.
    void setChannelTrimDb (int channelIndex, float trimDb) noexcept;

    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    /// BS.1770 loudness of the mix as written, measured on the writer thread.
    ///
    /// Measured here rather than in the audio callback because §11 forbids this
    /// much arithmetic per block there, and because this is the one place the
    /// mix exists exactly as it lands on disk -- summed, trimmed and through
    /// the limiter. A figure taken anywhere earlier would describe a mix nobody
    /// receives.
    double getIntegratedLufs() const;
    double getTruePeakDbtp() const;
    int getLoudnessBlockCount() const;

    /// §6.5: warn at 50% fill, degrade at 90%.
    double getFillFraction() const noexcept { return ring.fillFraction(); }

    /// Total frames accepted from the audio thread, for session.json.
    uint64_t getFramesAccepted() const noexcept { return framesAccepted.load (std::memory_order_relaxed); }

    /// Frames successfully handed to the destination MIX writer. Unlike ring
    /// fill, this advances only after the writer thread has finished every stem
    /// write for the same frames, so status/tests can observe a real completed
    /// writer boundary rather than a dequeue that is still being processed.
    uint64_t getFramesWritten() const noexcept { return framesWritten.load (std::memory_order_acquire); }

    /// Ticks whenever the writer gets something onto the disk -- a chunk
    /// drained, a file closed. Stop watches it: a slow card still working
    /// through its backlog is not a card that has stopped answering.
    uint64_t getProgress() const noexcept { return progress.load (std::memory_order_acquire); }

    /// Runs on the writer after every chunk it gets onto the disk. Tests use
    /// it to make a card slow but alive; nothing else sets it.
    void setChunkHookForTesting (std::function<void()> hook) { chunkHookForTesting = std::move (hook); }

    /// Frames the ring buffer could not accept. Any value above zero means
    /// audio was lost, which §0.1 treats as the one unacceptable failure.
    uint64_t getFramesDropped() const noexcept { return framesDropped.load (std::memory_order_relaxed); }

    /// The largest absolute sample value written since start(), across every
    /// channel. Zero means this take wrote nothing but silence -- which size
    /// alone cannot tell you, because a dead stream still fills the files.
    float getPeakWritten() const noexcept { return peakWritten.load (std::memory_order_relaxed); }

    /// §6.5 "target card removed": true once a write to the destination itself
    /// has failed, which is what pulling the card mid-take looks like from here.
    ///
    /// SessionWriter has always returned false on an unrecoverable write and its
    /// own header says the caller handles it per §6.5. This pipeline was that
    /// caller and discarded every one of those returns, so a pulled card wrote
    /// into the void: no stop, no finalize, no alert, and the elapsed time still
    /// climbing. Once this is true the pipeline stops writing rather than
    /// looping on a dead handle, and the owner must stop and finalize the take.
    bool hasCardWriteFailed() const noexcept { return cardWriteFailed.load (std::memory_order_acquire); }

    /// §6.3: the mirror failing is survivable and must never stop the card --
    /// the mirror exists to turn a card failure into an inconvenience, so it
    /// cannot be allowed to become one itself. A mirror write that fails stops
    /// the mirror for the rest of the take, exactly as running out of internal
    /// space does, and the card write continues untouched.
    bool hasMirrorWriteFailed() const noexcept { return mirrorWriteFailed.load (std::memory_order_acquire); }

    /// §6.3: the mirror was asked for and could not be opened at all, so this
    /// take has no second copy from its very first sample.
    ///
    /// Kept apart from hasMirrorWriteFailed() because the two need different
    /// sentences and one of them used to have none. A mirror that stops
    /// mid-take leaves a partial copy; a mirror that never opened leaves
    /// nothing, and start() deliberately does not fail the take over it. That
    /// left the user with a backup that had never existed while the app went on
    /// reporting one -- exactly the silent loss §6.3 exists to prevent.
    bool hasMirrorFailedToOpen() const noexcept { return mirrorFailedToOpen.load (std::memory_order_acquire); }

    /// Why start() returned false, in the user's words: which file could not be
    /// opened, and where. Empty when start() succeeded.
    ///
    /// The bool alone was the whole report, so a take that could not open its
    /// files failed with no reason attached and the record button simply
    /// refused to latch. A full card, a read-only card and a card pulled
    /// between arming and pressing record were indistinguishable, and none of
    /// them said anything.
    const std::string& getStartProblem() const noexcept { return startProblem; }

    /// The card writer's own account of why it stopped, when it has one that
    /// is more specific than "the card stopped accepting writes" -- a
    /// roll-over past 3.9 GB that could not create the next file, say. Empty
    /// otherwise, and the general message stands.
    /// The card's own account of why writing stopped. Survives stop().
    ///
    /// It is assembled from the writers, and stop() destroys them -- so this
    /// used to go empty exactly when the take ended, which is when the app
    /// asks. Every full card therefore produced the generic "the drive stopped
    /// responding, check it is plugged in properly" instead of the sentence
    /// naming it as full. Latched in stop() before the writers go, the same
    /// way mirrorOutOfSpace is latched below and for the same reason.
    std::string getCardWriteProblem() const;

    /// Whether the backup copy stopped because its drive ran out of room.
    ///
    /// The mirror is the computer's own disk, so "full" is the ordinary reason,
    /// and "that drive stopped accepting writes" leaves someone guessing at the
    /// one thing they can actually do about it. The fact rather than the card's
    /// sentence, which says recording has stopped -- true of the card, and
    /// alarming nonsense about a backup.
    ///
    /// Latched when the failure happens rather than read back from the
    /// writers: §6.3 closes and releases the mirror writers the moment
    /// mirroring stops, so by the time anyone asks, the one object that knew
    /// why is already gone. That is why there was never a way to ask.
    bool mirrorRanOutOfSpace() const noexcept
    { return mirrorOutOfSpace.load (std::memory_order_acquire); }

    /// The take's folder as it stands now, which is not always where it was
    /// opened: a folder renamed or moved in Finder mid-take takes the open
    /// files with it, and the audio goes on landing there. Read from the open
    /// mix file about once a second by the writer thread, and once more at
    /// stop() before the files close, so it survives stop(). The folder the
    /// take started in until the system has said otherwise.
    std::string getLiveSessionFolder() const;

private:
    mutable std::mutex liveFolderLock;
    std::string liveSessionFolder;
    std::string openedSessionFolder;
    double secondsSinceFolderCheck = 0.0;
    /// Writer thread (or stop(), after it has joined) only.
    void refreshLiveSessionFolder();

    // Constructed small and resized by start(), which is the only place the
    // real channel count and rate are known. RingBuffer::reset reallocates, so
    // it must never be called while the audio thread is running.
    std::atomic<bool> mixOnly { false };
    std::atomic<bool> cardWriteFailed { false };
    std::atomic<bool> mirrorWriteFailed { false };
    std::string cardWriteProblemAtStop;

    std::atomic<bool> mirrorOutOfSpace { false };
    std::atomic<bool> mirrorFailedToOpen { false };
    std::string startProblem;

    RingBuffer ring { 1 };
    std::vector<std::unique_ptr<SessionWriter>> stemWriters;
    std::unique_ptr<SessionWriter> mixWriter;

    // §6.3 mirror: a second, independent set of writers fed from the same
    // drained frames, so the copy is byte-identical without a second read.
    std::vector<std::unique_ptr<SessionWriter>> mirrorStemWriters;
    std::unique_ptr<SessionWriter> mirrorMixWriter;
    std::atomic<bool> mirroring { false };

    // Written by the writer thread, read by the UI. Guarded rather than atomic
    // because it is a whole object with vectors in it, and the read is a UI
    // tick rather than anything on a deadline.
    std::unique_ptr<LoudnessMeter> loudness;
    mutable std::mutex loudnessLock;

    std::vector<float> trimGains;      // linear, from WriteChannelSpec::trimDb
    std::vector<std::atomic<bool>> channelLive;

    std::thread writerThread;
    std::atomic<bool> running { false };
    std::atomic<uint64_t> framesAccepted { 0 };
    std::atomic<uint64_t> framesWritten { 0 };
    std::atomic<uint64_t> progress { 0 };
    std::function<void()> chunkHookForTesting;
    std::atomic<uint64_t> framesDropped { 0 };

    /// Loudest sample written this take; reset by start().
    std::atomic<float> peakWritten { 0.0f };

    int numChannels = 0;
    double sampleRate = 48000.0;

    // Audio-thread scratch, allocated at start(). pushBlock interleaves into
    // this and hands the ring whole chunks, so the callback pays two memcpys per
    // chunk instead of one indexed store, one modulo and one release-store per
    // sample per channel.
    static constexpr size_t kInterleaveChunkFrames = 512;
    std::vector<float> interleaveScratch;

    // Writer-thread scratch, allocated at start() so the drain loop does not.
    std::vector<float> drainBuffer;
    std::vector<float> stemScratch;
    std::vector<float> mixScratch;

    // §6.1's MIX.wav limiter. Writer-thread-owned, remade at start().
    MixBusLimiter mixLimiter;

    // Stem alignment. The audio thread queues each change to a channel's
    // offset (publishedOffsets is what it has queued, audio-thread-owned);
    // the writer applies them at the frames they were tagged with.
    StemAligner aligner;
    StemOffsetQueue offsetQueue;
    std::vector<int> publishedOffsets;
    std::atomic<bool> alignmentDeferred { false };
    uint64_t framesDrained = 0;                 // writer-owned
    std::vector<StemOffsetEvent> chunkEvents;   // writer scratch, reserved at start()

    void runWriterThread();
    void drainOnce (bool finalFlush);
    bool openMirrorWriters (const std::string& mirrorFolder,
                            const std::vector<WriteChannelSpec>& channels,
                            double rate, int bitDepth,
                            const std::string& originTimestamp);
};

} // namespace mma
