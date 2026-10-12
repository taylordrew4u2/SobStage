#pragma once
#include <atomic>
#include <cstdint>
#include "RingBuffer.h"
#include "DriftCompensator.h"

namespace mma {

/// One microphone's capture path (§3.2). Separate USB devices each run on their
/// own crystal, so their callbacks arrive on independent clocks and cannot be
/// treated as one aligned block. Each device therefore writes into its own
/// lock-free ring on its own audio thread, and the output clock pulls from every
/// ring at a resample ratio the PI loop keeps adjusting.
///
/// Every channel is steered that way, the §3.1 clock master included. The clock
/// this stream is pulled by belongs to the output device, so exempting a
/// microphone from correction does not make it the timebase -- it only leaves
/// that one channel uncorrected against a clock it has no relationship to.
///
/// Both ends are audio threads, so nothing here allocates, locks or blocks
/// after prepare() (§11). It is SPSC: the device callback is the only producer,
/// the output callback the only consumer.
class DeviceInputStream
{
public:
    /// How deep the platform layer asks the driver's own capture ring to be,
    /// in blocks. A reader thread that wakes late finds its audio still
    /// there rather than dropped in the kernel, and then hands it over all at
    /// once: this many blocks can land in this ring between two pulls.
    static constexpr int kSourceBufferBlocks = 8;

    /// Jitter headroom. Sized to take a whole driver ring's burst on top of
    /// the target fill with room to spare: with eight blocks here and eight
    /// in the driver, a stall the driver rode out overflowed this ring by two
    /// blocks -- audio the hardware had kept, thrown away one layer up. This
    /// costs no latency (the target fill below is what the monitor path pays)
    /// and 32 KB a channel at the largest block.
    static constexpr int kRingBlocks = 16;

    /// How full the ring must be before playout starts, and what the PI loop
    /// steers back to. This is the latency the drift buffer costs the
    /// headphones (§5.4), and the only thing standing between a device that
    /// hands its audio over late and a gap in what the output takes.
    ///
    /// One pull of it the output takes at once. The rest is how late a
    /// delivery can be before the ring runs dry, and lateness is time, not
    /// blocks: a CoreAudio IOProc a few milliseconds late on a busy Mac, a
    /// Windows DPC holding a 3 ms period back by 6 ms. Two pulls -- what
    /// this used to be, 2.7 ms at 64 -- left one pull of that, 1.3 ms, and
    /// every delivery later than that wrote silence into the channel: a
    /// click in the headphones and a gap in the stem. Measured through the
    /// real coordinator with six simulated microphones on drifting clocks:
    /// 30 clicks a minute at 64 frames with 0.5% of callbacks up to 2 ms
    /// late, 216 a minute at 3 ms, and 38 a minute at 144-frame Windows
    /// periods with 0.2% up to 6 ms late; none in any of them with this.
    ///
    /// So the ring holds one pull plus kLateDeliveryMs, but never less than
    /// kPreRollBlocks pulls (a pull's own jitter is a pull's worth at any
    /// size) and never more than kMaxPreRollBlocks: at 64 samples four
    /// pulls is 5.3 ms, which with the input and output blocks still fits
    /// §5.4's 10 ms, and a cushion of a full 6 ms there would not. At 480 or
    /// 512, two pulls already ride out more than 6 ms and nothing changes.
    /// See cushionSamples(), which CaptureCoordinator::monitorPathMs uses
    /// too, so the headphone delay the app names includes exactly this.
    ///
    /// Sizing this from the ring capacity instead -- half full, say -- ties the
    /// latency to the headroom, so making the ring safer would silently make
    /// the monitor path slower.
    static constexpr int kPreRollBlocks = 2;
    static constexpr int kMaxPreRollBlocks = 4;
    static constexpr double kLateDeliveryMs = 6.0;

    /// The target fill, in samples, for a stream the output pulls
    /// `pullSamples` at a time: one pull plus kLateDeliveryMs, kept between
    /// kPreRollBlocks and kMaxPreRollBlocks pulls.
    static size_t cushionSamples (size_t pullSamples, double sampleRate) noexcept
    {
        const size_t pull = pullSamples > 0 ? pullSamples : 1;
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        const auto late = static_cast<size_t> (kLateDeliveryMs * rate / 1000.0 + 0.999999);
        const size_t least = pull * static_cast<size_t> (kPreRollBlocks - 1);
        const size_t most = pull * static_cast<size_t> (kMaxPreRollBlocks - 1);
        return pull + (late < least ? least : late > most ? most : late);
    }

    /// The largest single delivery or pull the ring is allocated for.
    /// CoreAudio runs a device at its own IO size whenever it refuses the one
    /// asked for -- 512 by default, or anything the driver's range allows --
    /// and sizes its own callback scratch for up to 4096 frames, on either
    /// side. The ring used to be sixteen *nominal* blocks and its target fill
    /// two of them: 1024 and 128 samples at 64. A microphone running at 1156
    /// frames overflowed on every delivery, and an output pulling 1024 at a
    /// time drained a ring held at 128 dry on every callback -- either way
    /// half the audio gone, in the recording as well as the headphones.
    ///
    /// Storage is allocated for this many samples a block. What is *used* of
    /// it -- the overflow limit, kRingBlocks of a block -- and the target fill
    /// -- cushionSamples() of the pull -- follow the largest block actually
    /// seen on each side, so a device running at the size it was asked for
    /// keeps exactly the latency and the bound on staleness it always had.
    static constexpr int kLargestDeviceBlock = 4096;

    static_assert (kSourceBufferBlocks + kMaxPreRollBlocks + 2 <= kRingBlocks,
                   "the ring must hold a full driver burst above its target fill, with a block "
                   "of delivery jitter and one in flight");

    /// §3.1/§3.3: a drift figure is only claimed once this much of it has been
    /// measured. Before that the estimate's resolution is too coarse to name.
    static constexpr double kMeasurementSeconds = 60.0;

    /// The clock the loop reads to place a pull within the device's block. A
    /// test may substitute simulated time; production leaves it alone.
    using NowNsFn = int64_t (*)();
    static void setClockForTesting (NowNsFn fn) noexcept;

    /// Test and tuning hook: false drives the loop from the raw ring level at
    /// pull time -- the whole-block staircase -- instead of the de-quantized
    /// level. Exists so Tools/sim_drift_loop.cpp can show the difference.
    static void setVirtualFillForTesting (bool enabled) noexcept;

    /// Test and tuning hook: this stream's loop gains, after prepare().
    void setLoopGainsForTesting (double kp, double ki) noexcept { compensator.setGains (kp, ki); }
    void setLoopSlewForTesting (double ppmPerSecond) noexcept { compensator.setSlewForTesting (ppmPerSecond); }

    explicit DeviceInputStream (double sampleRate) noexcept;

    /// Sizes the ring and clears all loop state. Not real-time safe -- call
    /// before the streams open.
    void prepare (double sampleRate, int bufferSizeSamples);

    /// This stream's place in time: its audio reaches the output the target
    /// fill plus one device block after the device handed it over, since a
    /// sample waits up to a block in the device before it is delivered. Two
    /// microphones at different IO sizes -- 64 and the 1156 a Mac device
    /// runs at when it refuses 64 -- therefore play 1092 samples (23 ms)
    /// apart, and that is left alone here: the headphone mix hears each
    /// microphone as early as its own device allows (§5.4), and the stems
    /// are lined up afterwards, on the writer thread, where it costs the
    /// monitor path nothing (WritePipeline, StemAligner).
    ///
    /// What the stream does hold is room for its *own* device's largest
    /// block (see getDeviceBlockSamples), which is what lets the ring absorb
    /// an IO-size change: after a device has once run at a larger block, the
    /// room that block needs stays held while it runs smaller, so a return
    /// to the larger size finds it there and loses nothing, and the
    /// channel's place in time never moves. Only the first growth past
    /// anything held moves the channel -- the device itself now holds its
    /// audio that much longer -- and it moves once, exactly. When the ring ran
    /// dry for it, that gap is counted as lost audio; when the cushion
    /// covered it, the silence that moves the channel is counted as a shift
    /// (getAlignmentSilenceSamples). Its place in time is therefore always
    /// the cushion (cushionSamples) plus the block it holds room for
    /// (getHeldBlockSamples; the block asked
    /// for, before the device has settled on one), which is what the
    /// coordinator tells the writer.

    /// The largest IO block the device has run at since prepare(): its real
    /// IO size, which CoreAudio does not promise is the one asked for, and
    /// which can change mid-session when another app or a rate change moves
    /// it. A size counts once two deliveries in a row have been at least that
    /// large, so one odd delivery -- a test preloading the ring, a driver
    /// handing over a backlog in one piece -- is not taken for the device's
    /// block and does not hold the channel later for the rest of the session.
    /// Zero before that.
    size_t getDeviceBlockSamples() const noexcept { return deviceBlockSamples.load (std::memory_order_relaxed); }

    /// The device block this stream's place in time includes, as of its last
    /// pull: the largest it has settled at (the block asked for, before the
    /// device has settled on one) -- and, for getHeldBlockSamples, a larger
    /// one it has just moved for and is waiting for the next delivery to
    /// confirm or refuse. Published by the consumer, so the two always agree
    /// with each other and with what the stream has actually done.
    size_t getSettledBlockSamples() const noexcept { return settledBlockPublished.load (std::memory_order_relaxed); }
    size_t getHeldBlockSamples() const noexcept { return heldBlockPublished.load (std::memory_order_relaxed); }

    /// Silence written, without consuming, to move the channel later when its
    /// device's IO block grew by less than the ring's cushion. Not lost audio
    /// -- nothing that arrived is skipped, and the ring never ran dry -- but a
    /// gap in what this stream hands on all the same, so it is counted here
    /// rather than nowhere. (The writer allows for the move when it lines the
    /// stems up; see StemAligner.)
    uint64_t getAlignmentSilenceSamples() const noexcept { return alignmentSilence.load (std::memory_order_relaxed); }

    /// Producer: this device's audio callback. Real-time safe.
    void pushBlock (const float* samples, int numSamples) noexcept;

    /// Producer: the device produced these samples and the platform layer
    /// lost them before they could be handed over -- a driver ring that
    /// overflowed while the reader was not running. Nothing enters the ring;
    /// the loss is the backend's to count and report. But the measurement
    /// counts what the device's clock produced, and a step of a stall's
    /// worth in the delivered count read as the clock running slow for as
    /// long as the step sat in the window: a microphone at +150 PPM was
    /// reported at -330 after one xrun. Real-time safe.
    void noteSamplesLostBeforeDelivery (int numSamples) noexcept;

    /// Consumer: the output clock pulls numSamples of this device's audio,
    /// resampled by the current drift ratio so it lands on the master's
    /// timebase. Real-time safe.
    ///
    /// Until the ring has pre-rolled to its target fill this yields silence and
    /// counts nothing: at stream start the output callback runs before any
    /// input has arrived, and consuming an empty ring there would glitch the
    /// first block of every take and leave the loop chasing a fill error that
    /// only means "not buffered yet". Genuine underruns after that are counted.
    void pull (float* destination, int numSamples) noexcept;

    /// True once enough audio has arrived to start consuming (§3.2 pre-roll).
    bool hasStarted() const noexcept { return started.load (std::memory_order_relaxed); }

    /// §6.5: an unplugged mic keeps its channel and yields silence.
    ///
    /// Coming back is not the mirror image of going away. While the channel is
    /// dead this stream stops consuming, so its ring, its resampler phase and
    /// its PI loop all still hold the moment the microphone left. Resuming from
    /// that state replays audio from before the gap, or -- with a dry ring --
    /// holds the last sample before it as a DC offset for the rest of the take
    /// while counting every block as an underrun. So a false-to-true transition
    /// arms a restart, which the next pull() performs on the audio thread.
    void setLive (bool live) noexcept
    {
        if (live && ! channelLive.exchange (true, std::memory_order_relaxed))
            restartPending.store (true, std::memory_order_release);
        else
            channelLive.store (live, std::memory_order_relaxed);

        if (! live)
        {
            driftPpm.store (0.0, std::memory_order_relaxed);
            excessDrift.store (false, std::memory_order_relaxed);
            driftReportingResetEpoch.fetch_add (1, std::memory_order_release);
        }
    }

    bool isLive() const noexcept { return channelLive.load (std::memory_order_relaxed); }

    /// §0.1: samples this stream threw away because its ring was full -- the
    /// consumer stopped pulling, or pulled too slowly. Counted, never silent.
    uint64_t getOverrunSamples() const noexcept { return overrunSamples.load (std::memory_order_relaxed); }

    /// The loop's own correction against the clock that pulls this stream:
    /// what the resampler is doing right now. Positive means this device is
    /// being drained faster than nominal. This is a control state, not a
    /// measurement -- it slews at 5 PPM/s, overshoots, and is disturbed by
    /// every lost block -- so §3.3's reported figure comes from
    /// getMeasuredDriftPpm() instead.
    double getDriftPpm() const noexcept { return driftPpm.load (std::memory_order_relaxed); }
    bool hasSustainedExcessDrift() const noexcept { return excessDrift.load (std::memory_order_relaxed); }

    /// §3.3: this device's clock against the clock that pulls it, measured.
    /// A least-squares fit of samples-delivered against samples-pulled over
    /// the reporting window, so it does not depend on where the loop is in its
    /// transient, and a lost block moves it by a few PPM rather than sending
    /// it to a clamp. Zero, and hasDriftMeasurement() false, until
    /// kMeasurementSeconds of window exist. Positive means the device runs
    /// fast. The figure §3.3 shows is this minus the master's; the
    /// coordinator does that subtraction.
    double getMeasuredDriftPpm() const noexcept { return measuredPpm.load (std::memory_order_relaxed); }
    bool hasDriftMeasurement() const noexcept { return measured.load (std::memory_order_relaxed); }
    double getMeasurementSeconds() const noexcept { return measurementSeconds.load (std::memory_order_relaxed); }

    /// Samples the output clock asked for and the ring could not supply. Any
    /// value above zero is audio that was not there when it was needed.
    uint64_t getUnderrunSamples() const noexcept { return underruns.load (std::memory_order_relaxed); }

    /// §5.4: gaps in which this ring lost audio -- a run of pulls that ran
    /// dry, or a push that found the ring full -- as events rather than
    /// samples. The buffer ladder counts events: three inside thirty seconds
    /// is its trigger. A run of dry pulls with no audio between them is one
    /// gap: a device whose IO size grew from 64 to 1156 leaves fifteen 64-
    /// sample pulls dry in a row, and counted per pull that one change
    /// stepped the ladder and rebuilt the engine for the rest of the session.
    uint64_t getLossEvents() const noexcept { return lossEvents.load (std::memory_order_relaxed); }

    /// Diagnostics for the harnesses: how often the interpolator restarted
    /// its pair (a sub-sample step each time), held its last sample for a
    /// missing look-ahead, and skipped late audio. None of these is loss;
    /// all of them are a seam in the audio, and a take with one per block
    /// is a take with a click per block.
    uint64_t getPrimeCount() const noexcept { return primes.load (std::memory_order_relaxed); }
    uint64_t getHoldCount() const noexcept { return holds.load (std::memory_order_relaxed); }
    uint64_t getSkipCount() const noexcept { return skips.load (std::memory_order_relaxed); }

    /// Samples the device has delivered, dropped ones included, and samples
    /// the consumer has pulled since playout started. The measurement above
    /// is built from these; harnesses read them directly.
    uint64_t getPushedSamples() const noexcept { return pushedSamples.load (std::memory_order_relaxed); }

    /// The device handed over audio within the last `windowNs` nanoseconds.
    /// Stamped whether or not the channel is live, so a stream still running
    /// is visible even while its channel is being written as silence.
    bool deliveredWithin (int64_t windowNs) const noexcept;
    uint64_t getPulledSamples() const noexcept { return pulledSamples.load (std::memory_order_relaxed); }

    /// The two clocks against the wall clock, from the same fit: how fast the
    /// device delivers and how fast the consumer pulls, each in PPM against
    /// nominal. Diagnostics; zero until measured.
    double getMeasuredDeviceRatePpm() const noexcept { return deviceRatePpm.load (std::memory_order_relaxed); }
    double getMeasuredConsumerRatePpm() const noexcept { return consumerRatePpm.load (std::memory_order_relaxed); }

    double getFillFraction() const noexcept;

    /// §3.3 drift reporting runs on a slower cadence than the audio callback,
    /// so the measurement and the sustained-excess flag are advanced from
    /// there. referencePpm is the clock master's own measured drift, since
    /// §3.3 judges each device against the master rather than against the
    /// output stream. Called from one thread only; never from an audio thread.
    void tickDriftReporting (double elapsedSeconds, double referencePpm = 0.0) noexcept;

private:
    RingBuffer ring;
    DriftCompensator compensator;
    double rate = 48000.0;

    // The block size prepare() was given, and the largest delivery and pull
    // seen since. Each is written by one side and read by both: the usable
    // part of the ring is kRingBlocks of the biggest of the three, never more
    // than the storage behind it. See kLargestDeviceBlock.
    size_t nominalBlockSamples = 64;
    std::atomic<size_t> largestPushSamples { 0 };

    // Producer-owned: the size of the previous delivery; and, published, the
    // largest size two consecutive deliveries have both reached. See
    // getDeviceBlockSamples().
    size_t previousPushSamples = 0;
    std::atomic<size_t> deviceBlockSamples { 0 };
    std::atomic<size_t> largestPullSamples { 0 };
    size_t usableCapacity() const noexcept;

    std::atomic<bool> channelLive { true };

    std::atomic<uint64_t> overrunSamples { 0 };

    // Set on the message thread when a channel comes back, consumed by the
    // audio thread in pull(). The reset itself has to happen there: it touches
    // the resampler state and the ring's read index, both of which belong to
    // the consumer.
    std::atomic<bool> restartPending { false };
    std::atomic<double> driftPpm { 0.0 };
    std::atomic<bool> excessDrift { false };
    std::atomic<uint64_t> driftReportingResetEpoch { 0 };
    std::atomic<uint64_t> underruns { 0 };
    std::atomic<uint64_t> lossEvents { 0 };
    std::atomic<uint64_t> primes { 0 };
    std::atomic<uint64_t> holds { 0 };
    std::atomic<uint64_t> skips { 0 };

    // Producer-published: when its last block landed and how big it was. The
    // consumer uses them to place its pull within the device's block, which
    // is what turns the ring level from a staircase in whole blocks into a
    // line the loop can follow. Sample counts feed the measurement.
    std::atomic<int64_t> lastPushNs { 0 };

    /// Real time of the last delivery, for liveness only. Separate from
    /// lastPushNs, which runs on the measurement clock harnesses simulate.
    std::atomic<int64_t> lastDeliveryWallNs { 0 };
    std::atomic<int> lastPushSamples { 0 };
    std::atomic<uint64_t> pushedSamples { 0 };
    std::atomic<int64_t> lastPullNs { 0 };
    std::atomic<uint64_t> pulledSamples { 0 };

    // Consumer-owned: the smoothed, de-quantized fill the loop is driven by.
    double fillAverage = 0.0;
    bool fillAverageValid = false;
    static constexpr double kFillSmoothing = 1.0 / 32.0;

    // Consumer-owned: silence this stream has written in place of audio that
    // had not arrived, not yet answered by the audio that arrives late for
    // the same span. A dry pull consumes nothing, so when the device's blocks
    // do land -- a late reader hands over everything the driver held at once
    // -- the ring holds that much more than before. The loop used to read
    // that as "device fast" and answer with the clamp, which on a slow device
    // drained the ring dry again: a limit cycle paid in one dropped block
    // every few seconds. Draining it slowly instead kept this channel that
    // far behind every other for as long as the drain took, minutes, with
    // the monitor path that much slower and the headroom that much smaller.
    //
    // The span the late audio covers already stands in the file as silence.
    // So it is skipped, up to the silence it answers for, at the first pull
    // that finds the ring above its target: the channel is back in step at
    // once, the loss was counted once, as the silence, and the loop never
    // sees it. Silence that nothing arrives to answer within a ring's worth
    // of pulls was audio lost outright -- a device that dropped it itself --
    // and is forgotten rather than left to eat the next block that runs a
    // few samples ahead.
    double silenceOwed = 0.0;
    int pullsSinceSilence = 0;

    // Consumer-owned: the target fill rose after playout had started, with
    // too little buffered to serve it -- a pull bigger than any before it,
    // or a device block bigger than the headroom held for it. The stream
    // writes silence until the de-quantized level reaches the new target,
    // landing on it exactly, so the channel's new place in time is set once
    // rather than left to the loop. Counted as lost only when the ring ran
    // dry for it; a growth the cushion covered is a shift (rebufferCounted
    // false, getAlignmentSilenceSamples).
    bool rebuffering = false;
    bool rebufferCounted = true;

    // Consumer-owned: the rebuffer is filling a gap whose cause is not
    // settled -- a provisional block -- so the silence it writes is owed like
    // the gap's own (noteSilence) and late audio landing above the target
    // answers for it.
    bool rebufferOwed = false;

    // Consumer-owned: a delivery larger than any block held room for ended a
    // dry gap. The device may have grown its IO size, or a driver may have
    // handed a backlog over in one piece; the delivery after it tells which
    // (a size two in a row reach is the device's block). Until then the
    // stream holds room for it (heldLatency) and keeps the gap's silence
    // owed. Zero when nothing is pending.
    size_t provisionalBlock = 0;

    // Consumer-owned: the last pull ended in counted silence. A run of dry
    // pulls is one loss event, not one per pull.
    bool inGap = false;
    void countGap (int samples) noexcept;

    // Consumer-owned: the largest device block the consumer has accounted
    // for, and the shift silence it has written (getAlignmentSilenceSamples).
    size_t seenDeviceBlock = 0;
    std::atomic<uint64_t> alignmentSilence { 0 };
    std::atomic<size_t> settledBlockPublished { 0 };
    std::atomic<size_t> heldBlockPublished { 0 };
    void publishPlaceInTime() noexcept;

    /// What this stream holds beyond the base target, with the device's block
    /// included: the largest block the device has delivered, or a provisional
    /// one it has moved for.
    size_t heldLatency() const noexcept;
    void recomputeTarget() noexcept;

    // Reporting-thread-owned. The audio threads publish counters atomically;
    // they never touch anything below.
    double excessDriftSeconds = 0.0;
    uint64_t observedDriftReportingResetEpoch = 0;
    std::atomic<double> measuredPpm { 0.0 };
    std::atomic<bool> measured { false };
    std::atomic<double> measurementSeconds { 0.0 };
    std::atomic<double> deviceRatePpm { 0.0 };
    std::atomic<double> consumerRatePpm { 0.0 };

    // The measurement window: at each tick, each side's sample count paired
    // with the timestamp of the block that brought it there, in a fixed ring
    // so the reporting thread allocates nothing after construction either.
    // Each side is fitted against its own timestamps. Fitting one count
    // against the other would put the ring level into the residual, and the
    // level's slow swing through the loop's transient reads as slope over a
    // sixty-second window -- tens of PPM of bias at larger blocks.
    static constexpr int kMaxWindowPoints = 1024;
    struct RatePoint { double pushSeconds; double pushed; double pullSeconds; double pulled; };
    RatePoint window[kMaxWindowPoints] {};
    int windowStart = 0;
    int windowCount = 0;

    static constexpr double kExcessDriftThresholdPpm = 100.0;
    static constexpr double kExcessDriftSustainSeconds = 10.0;

    // Linear-interpolation resampler state. Two samples and a phase is all a
    // ratio this close to 1.0 needs, and it costs no allocation in the callback.
    float previousSample = 0.0f;
    float currentSample = 0.0f;
    double phase = 0.0;
    bool primed = false;

    // The sample a pull that ran dry had already read but not yet played: it
    // was only the interpolator's target. Dropping it with the pair lost one
    // captured sample at every gap; the next prime starts from it instead.
    // `heldLast` marks a pull that ended on a held sample, which was played.
    float carrySample = 0.0f;
    double carryPhase = 0.0;
    bool carryPending = false;
    bool heldLast = false;
    void resetInterpolator() noexcept;
    // Read by the reporting thread too, to know whether there is anything to
    // measure yet; the consumer alone writes it.
    std::atomic<bool> started { false };

    size_t targetFillSamples = 0;
    size_t baseTargetSamples = 0; // the pull-sized pre-roll, before block headroom

    bool readOne (float& out) noexcept;
    void resetMeasurementWindow() noexcept;
    double virtualFillNow (size_t available) const noexcept;
    double fillErrorNow (size_t available) noexcept;
    void noteSilence (int samples) noexcept;
    void skipLateAudio (int numSamples) noexcept;
};

} // namespace mma
