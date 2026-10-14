#pragma once
#include <atomic>

namespace mma {

/// §8.1/§8.2 meter ballistics: 10ms attack, 1.5s decay, peak hold 2s then 20dB/s
/// decay, clip latch at 3 consecutive samples >= -0.1dBFS. Audio thread calls
/// pushBlock() (writes only atomics, no locks/allocation); UI thread polls at 60Hz.
class Metering
{
public:
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 0.0f;
    static constexpr float kClipThresholdDb = -0.1f;
    static constexpr int kClipConsecutiveSamples = 3;
    static constexpr double kAttackSeconds = 0.010;
    static constexpr double kDecaySeconds = 1.5;
    static constexpr double kPeakHoldSeconds = 2.0;
    static constexpr double kPeakDecayDbPerSecond = 20.0;

    /// The ballistics advance on wall-clock dtSeconds in tick(), not on sample
    /// counts, so nothing here depends on the rate. The argument is kept
    /// because every caller has it and a meter that one day needs it should
    /// not change its construction; it is deliberately not stored.
    explicit Metering (double sampleRate) noexcept;

    /// Audio-thread call: pass this channel's raw samples for one block. Computes
    /// max-abs (and tracks a running count of consecutive samples at/above the
    /// clip threshold, needed for the §8.1 3-sample clip latch) then stores results
    /// into atomics only -- no allocation, locking, logging, or file I/O.
    void processAudioBlock (const float* samples, int numSamples) noexcept;

    /// Audio-thread call for a meter whose CLIP is not about its own samples:
    /// the level and peak exactly as processAudioBlock, of the samples times
    /// `gain`, without the -0.1 dBFS latch. The mix bar shows the summed bus
    /// at the headphone volume -- the signal the safety limiter judges -- and
    /// its CLIP comes from that limiter (noteClipEvents), not from its own
    /// samples. The gain is applied to the block's loudest sample, so the
    /// caller needs no scratch buffer for it. No allocation, locking or
    /// logging.
    void processLevelOnly (const float* samples, int numSamples, float gain = 1.0f) noexcept;

    /// Latches CLIP and adds `events` to its count, from a clip the caller
    /// detected itself (the headphone limiter turning the mix down). Any
    /// thread; atomics only.
    void noteClipEvents (int events) noexcept;

    /// Convenience overload when the caller has already reduced the block to a
    /// single max-abs value (e.g. a synthetic test, or a caller who doesn't need
    /// the sample-accurate clip latch).
    void pushBlockStats (float maxAbsLinear, int numSamplesInBlock) noexcept;

    /// UI-thread call at 60Hz: advances envelope/peak-hold ballistics by dtSeconds
    /// and returns the current displayed level in dBFS.
    float tick (double dtSeconds) noexcept;

    float getDisplayedLevelDb() const noexcept { return displayedDb; }
    float getPeakHoldDb() const noexcept { return peakHoldDb; }
    bool isClipped() const noexcept { return clipLatched.load (std::memory_order_relaxed); }
    int getClipCount() const noexcept { return clipCount.load (std::memory_order_relaxed); }

    /// UI calls this when the user taps the clip indicator to clear the latch.
    void acknowledgeClip() noexcept;

private:
    // Precomputed at construction rather than evaluated per block on the audio
    // thread. MonitorBus already hoists its ceiling out of the callback for the
    // same reason; this is the same std::pow of the same kind of constant.
    float clipThresholdLinear;

    // Loudest block since the UI last read it. The audio thread is the only
    // writer of a raised value (load, max, store -- no CAS needed); tick()
    // exchange()s it to kNothingNew. If the exchange lands between the audio
    // thread's load and store, that store republishes the already-consumed
    // peak: it is seen by one extra tick, never lost. kNothingNew (below
    // kMinDb) tells tick() that no block arrived, so it keeps the last level.
    static constexpr float kNothingNew = kMinDb - 1.0f;
    std::atomic<float> pendingPeakDb { kNothingNew };
    float lastBlockDb = kMinDb; // UI thread: level held while no block arrives
    std::atomic<int> consecutiveClipSamples { 0 };
    std::atomic<bool> clipLatched { false };
    std::atomic<int> clipCount { 0 };

    // UI-thread-owned ballistics state.
    float displayedDb = kMinDb;
    float peakHoldDb = kMinDb;
    double peakHoldElapsed = kPeakHoldSeconds; // starts "expired" so it decays immediately if no signal

    static float linearToDb (float linear) noexcept;
    void publishPeak (float blockDb) noexcept;
};

} // namespace mma
