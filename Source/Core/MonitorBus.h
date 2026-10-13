#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "PeakLimiter.h"

namespace mma {

/// §5: the live monitor mix. Unity sum (no per-channel attenuation with channel
/// count -- level is managed by trim and master volume only), then the
/// listener's master volume, then a mandatory zero-lookahead safety limiter at
/// -3 dBFS on what actually reaches the ears, a runaway cut if that limiter is
/// needed for >500ms without a break, and a global mute. Third-octave feedback
/// detection (§5.5) watches the summed bus from FeedbackGuard and cuts it the
/// same way. This bus's limiter is a *separate instance* from MixBusLimiter
/// (§6.1) so a monitor mute or runaway cut can never silence the recorded mix
/// file.
class MonitorBus
{
public:
    static constexpr float kLimiterCeilingDb = -3.0f;
    static constexpr double kLimiterHoldSeconds = PeakLimiter::kHoldSeconds;       // 10 ms
    static constexpr double kLimiterReleaseSeconds = PeakLimiter::kReleaseSeconds; // 50 ms

    /// §5: what "continuous" tolerates when timing a runaway. A stretch
    /// shorter than this with no sample over the ceiling is part of the same
    /// engagement; a longer one ends it.
    static constexpr double kRunawayGapSeconds = 0.001;
    static constexpr double kRunawayCutSeconds = 0.5;       // 500ms continuous engagement -> mute

    static constexpr double kFeedbackBandRiseDb = 10.0;
    static constexpr double kFeedbackWindowSeconds = 0.5;
    static constexpr double kFeedbackWithinBroadbandDb = 6.0;

    static constexpr double kDefaultMonitorVolume = 70.0; // 0-100
    static constexpr double kMinTrimDb = -20.0;
    static constexpr double kMaxTrimDb = 20.0;
    static constexpr double kTrimStepDb = 0.5;

    explicit MonitorBus (double sampleRate) noexcept;

    /// One sample of the headphone mix: the trimmed inputs summed at unity,
    /// turned to the listener's master volume, then through the -3 dBFS
    /// safety limiter. Returns what reaches the ears.
    ///
    /// The volume comes before the limiter. It used to come after, so the
    /// ceiling was tested on the bus at full level: three or four mics in one
    /// room reached -3 dBFS on ordinary speech and were squared off there,
    /// and turning the headphones down only made the same distortion quieter.
    /// Tested on what the listener hears, the ceiling now does what §5.5 says
    /// it is for -- protecting ears from a fault -- and the volume control
    /// takes the mix out of it.
    ///
    /// `busBeforeVolume`, when given, receives the summed bus before the
    /// volume: what the mix meter shows and the feedback guard listens to.
    /// Zero while the monitor is muted or cut, and for a sample carrying a
    /// faulty microphone's NaN or infinity. No EQ, no filtering, no lookahead -- only
    /// summing, trim (applied by the caller), the volume and this limiter
    /// belong on this bus.
    float processSample (const std::vector<float>& trimmedInputSamples,
                         float* busBeforeVolume = nullptr) noexcept;

    /// Maps 0-100 UI volume to a linear gain, logarithmically, per §5.1.
    static float monitorVolumeToLinearGain (double volume0to100) noexcept;

    /// §5.1 master monitor volume, 0-100, default 70. Affects only what reaches
    /// the headphones; recorded files are written from a separate path.
    void setMasterVolume (double volume0to100) noexcept;
    double getMasterVolume() const noexcept { return masterVolume.load (std::memory_order_relaxed); }

    /// The linear gain the master volume maps to (0..1), as the audio thread
    /// will next apply it.
    float getMasterGain() const noexcept { return masterGain.load (std::memory_order_acquire); }

    /// Times the safety limiter has started turning the headphones down,
    /// since this bus was made. One loud passage counts once, however many
    /// peaks it has. Read on any thread; the mix meter's CLIP is latched from
    /// it, so the user can see the mix is too hot instead of only hearing it.
    uint64_t getLimiterEngagementCount() const noexcept { return limiterEngagements.load (std::memory_order_relaxed); }

    /// Maps a -20..+20 dB trim value to a linear multiplier.
    static float trimDbToLinearGain (float trimDb) noexcept;

    /// True once the runaway cut has engaged; stays true until manuallyUnmute() is called.
    bool isRunawayMuted() const noexcept { return runawayMuted.load (std::memory_order_acquire); }
    void manuallyUnmute() noexcept;

    /// Latches the runaway cut on a bus that replaces one that was cut. A
    /// rebuild is not the user unmuting, and §5 holds the cut until they do.
    void engageRunawayCut() noexcept { runawayMuted.store (true, std::memory_order_release); }

    /// Global instantaneous mute (spacebar), independent of runaway cut.
    void setGlobalMute (bool shouldMute) noexcept { globallyMuted.store (shouldMute, std::memory_order_release); }
    bool isGloballyMuted() const noexcept { return globallyMuted.load (std::memory_order_acquire); }

    bool isMuted() const noexcept { return isGloballyMuted() || isRunawayMuted(); }

    /// The §5.5 growth rule for a single band's level, tumbling 500 ms windows.
    /// bandLevelDb is one band's level, broadbandPeakDb is the mix peak. The
    /// shipping path does not use this: FeedbackGuard (FeedbackDetector.h)
    /// analyses every third-octave band of the bus on its own thread and cuts
    /// through engageRunawayCut(). Kept for the UI walker, which trips a cut
    /// with it without needing a howl in the fixture. Not thread-safe against
    /// itself; call from one thread.
    bool processFeedbackCandidate (double bandLevelDb, double broadbandPeakDb, double blockSeconds) noexcept;

private:
    double sampleRate;
    bool limiterEngaged = false;
    double limiterEngagedSeconds = 0.0;

    // How long the limiter has had nothing over the ceiling within the
    // current engagement. The run is only broken once this passes
    // kRunawayGapSeconds; a gap shorter than that is part of the same
    // continuous engagement, which is what §5 means by "500 ms continuous".
    double limiterReleasedSeconds = 0.0;

    // The gain-riding limiter itself, audio-thread-owned like the counters.
    PeakLimiter limiter;
    std::atomic<uint64_t> limiterEngagements { 0 };

    // These values cross between the message thread and the audio callback.
    // Atomics keep UI changes lock-free and prevent undefined behaviour in the
    // callback. Limiter bookkeeping below remains audio-thread-owned.
    std::atomic<bool> runawayMuted { false };
    std::atomic<bool> globallyMuted { false };
    std::atomic<bool> limiterResetRequested { false };
    std::atomic<double> masterVolume { kDefaultMonitorVolume };

    // Precomputed so the audio callback never calls std::pow. processSample
    // runs once per sample per block, and a transcendental in there is time
    // taken directly out of the callback's deadline (§11). The ceiling is
    // likewise computed once, into the limiter.
    std::atomic<float> masterGain;

    void clearEngagement() noexcept;

    // Feedback detector state: level at the start of the current growth window.
    double feedbackWindowStartDb = -200.0;
    double feedbackWindowElapsed = 0.0;
};

} // namespace mma
