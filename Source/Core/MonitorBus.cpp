#include "MonitorBus.h"
#include <algorithm>
#include <cmath>

namespace mma {

static_assert (std::atomic<bool>::is_always_lock_free,
               "MonitorBus requires lock-free bool atomics on the audio thread");
static_assert (std::atomic<float>::is_always_lock_free,
               "MonitorBus requires lock-free float atomics on the audio thread");
static_assert (std::atomic<double>::is_always_lock_free,
               "MonitorBus requires lock-free double atomics on the audio thread");
static_assert (std::atomic<uint64_t>::is_always_lock_free,
               "MonitorBus requires lock-free 64-bit atomics on the audio thread");

MonitorBus::MonitorBus (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn),
      limiter (sampleRateIn, std::pow (10.0f, kLimiterCeilingDb / 20.0f)),
      masterGain (monitorVolumeToLinearGain (kDefaultMonitorVolume))
{
}

void MonitorBus::clearEngagement() noexcept
{
    limiterEngaged = false;
    limiterEngagedSeconds = 0.0;
    limiterReleasedSeconds = 0.0;
    limiter.reset();
}

float MonitorBus::monitorVolumeToLinearGain (double volume0to100) noexcept
{
    const double v = std::clamp (volume0to100, 0.0, 100.0) / 100.0;
    if (v <= 0.0)
        return 0.0f;
    // Logarithmic (perceptual) mapping: -60dB at 0, 0dB at 100.
    constexpr double minDb = -60.0;
    const double db = minDb * (1.0 - v);
    return static_cast<float> (std::pow (10.0, db / 20.0));
}

float MonitorBus::trimDbToLinearGain (float trimDb) noexcept
{
    const float clamped = std::clamp (trimDb, static_cast<float> (kMinTrimDb), static_cast<float> (kMaxTrimDb));
    return std::pow (10.0f, clamped / 20.0f);
}

float MonitorBus::processSample (const std::vector<float>& trimmedInputSamples,
                                 float* busBeforeVolume) noexcept
{
    if (busBeforeVolume != nullptr)
        *busBeforeVolume = 0.0f;

    // manuallyUnmute() is called from the message thread. Reset the remaining
    // limiter state here so those non-atomic counters stay audio-thread-owned.
    if (limiterResetRequested.load (std::memory_order_acquire)
        && limiterResetRequested.exchange (false, std::memory_order_acq_rel))
        clearEngagement();

    if (isMuted())
    {
        // No sound is reaching the room, so there is nothing for the limiter
        // to be protecting anyone from and no runaway in progress: whatever
        // engagement had been building is evidence about a signal that has
        // stopped. These counters used to freeze here instead, so a mute
        // pressed 400 ms into a loud burst carried that 400 ms across the
        // mute -- and the first 100 ms of ordinary loud audio after unmuting,
        // possibly minutes later, cut the headphones as a "runaway". The run
        // starts again from nothing when the sound comes back, and so does
        // the limiter's gain.
        clearEngagement();
        return 0.0f;
    }

    float sum = 0.0f;
    for (auto s : trimmedInputSamples)
        sum += s; // unity sum, no per-channel attenuation with channel count

    // A NaN or infinity from any microphone is a device fault. NaN failed the
    // ceiling comparison and went to the headphones as NaN, which an output
    // driver may turn into a full-scale burst; infinity was clipped to the
    // ceiling, a click at maximum level. Silence for that sample, and it is
    // not counted as limiting, so a faulty microphone cannot trip the runaway
    // cut and leave everyone's headphones muted after it recovers.
    if (! std::isfinite (sum))
        return 0.0f;

    // §5.1/§5.5: the listener's volume first, then the ceiling, so the limiter
    // judges what reaches the ears. The ceiling is a hearing-safety limit, and
    // a bus at +3 dBFS played at the default volume reaches nobody's ears
    // above -15 dBFS: there is nothing to protect anyone from, and squaring
    // it off there was distortion the volume knob could not take away.
    const float atTheEars = sum * masterGain.load (std::memory_order_acquire);

    const auto engagementsBefore = limiter.getEngagements();
    const float output = limiter.process (atTheEars);

    if (limiter.getEngagements() != engagementsBefore)
        limiterEngagements.fetch_add (1, std::memory_order_relaxed);

    // §5: the runaway cut times how long the signal at the ears keeps needing
    // the limiter -- samples over the ceiling before it acts, as it always
    // was, now measured after the volume. Not the gain reduction itself:
    // that is held and released over tens of milliseconds on purpose, and
    // would make every loud phrase look "continuous".
    const double sampleSeconds = (sampleRate > 0.0) ? (1.0 / sampleRate) : 0.0;

    if (limiter.wasOverCeiling())
    {
        limiterEngaged = true;
        limiterReleasedSeconds = 0.0;
        limiterEngagedSeconds += sampleSeconds;

        if (limiterEngagedSeconds >= kRunawayCutSeconds)
            runawayMuted.store (true, std::memory_order_release);
    }
    else if (limiterEngaged)
    {
        // §5: the runaway cut is for 500 ms of *continuous* engagement, and
        // kRunawayGapSeconds is what "continuous" tolerates -- a gap shorter
        // than 1 ms is the same engagement, a longer one ends it.
        //
        // This previously drained the accumulator one sample-time per quiet
        // sample and never consulted the gap length at all, so unwinding an
        // engagement took as long as building it. An engagement that had
        // ended seconds ago still carried most of its credit into the next,
        // unrelated one: two 300 ms bursts 10 ms apart -- neither close to the
        // threshold -- combined past 500 ms and cut the monitor. That is a
        // false alarm that silences someone's headphones mid-take and needs a
        // manual unmute, so a gap longer than this ends the run outright.
        limiterReleasedSeconds += sampleSeconds;

        if (limiterReleasedSeconds >= kRunawayGapSeconds)
        {
            limiterEngaged = false;
            limiterEngagedSeconds = 0.0;
            limiterReleasedSeconds = 0.0;
        }
    }

    if (isRunawayMuted())
        return 0.0f;

    if (busBeforeVolume != nullptr)
        *busBeforeVolume = sum;

    return output;
}

void MonitorBus::setMasterVolume (double volume0to100) noexcept
{
    const auto clamped = std::max (0.0, std::min (100.0, volume0to100));
    masterVolume.store (clamped, std::memory_order_relaxed);

    // One aligned float store the callback picks up on its next sample, instead
    // of a std::pow per sample inside processSample.
    masterGain.store (monitorVolumeToLinearGain (clamped), std::memory_order_release);
}

void MonitorBus::manuallyUnmute() noexcept
{
    // The counters are owned by the audio callback. Ask it to reset them on
    // its next sample instead of racing it from the message thread.
    limiterResetRequested.store (true, std::memory_order_release);
    runawayMuted.store (false, std::memory_order_release);
}

bool MonitorBus::processFeedbackCandidate (double bandLevelDb, double broadbandPeakDb, double blockSeconds) noexcept
{
    const bool withinBroadband = (broadbandPeakDb - bandLevelDb) <= kFeedbackWithinBroadbandDb;

    if (! withinBroadband)
    {
        feedbackWindowStartDb = bandLevelDb;
        feedbackWindowElapsed = 0.0;
        return false;
    }

    feedbackWindowElapsed += blockSeconds;

    if (feedbackWindowElapsed >= kFeedbackWindowSeconds)
    {
        const bool grew = (bandLevelDb - feedbackWindowStartDb) > kFeedbackBandRiseDb;
        // Slide the window forward regardless, using this block as the new baseline.
        feedbackWindowStartDb = bandLevelDb;
        feedbackWindowElapsed = 0.0;

        if (grew)
        {
            runawayMuted.store (true, std::memory_order_release); // visible reason is supplied by the UI layer
            return true;
        }
    }

    return false;
}

} // namespace mma
