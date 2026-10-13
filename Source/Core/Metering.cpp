#include "Metering.h"
#include <algorithm>
#include <cmath>

namespace mma {

Metering::Metering ([[maybe_unused]] double sampleRateIn) noexcept
    : clipThresholdLinear (std::pow (10.0f, kClipThresholdDb / 20.0f))
{
}

float Metering::linearToDb (float linear) noexcept
{
    if (linear <= 0.0f)
        return kMinDb;
    return std::max (kMinDb, 20.0f * std::log10 (linear));
}

void Metering::processAudioBlock (const float* samples, int numSamples) noexcept
{
    float maxAbs = 0.0f;
    int consecutive = consecutiveClipSamples.load (std::memory_order_relaxed);
    const float clipLinear = clipThresholdLinear;

    for (int i = 0; i < numSamples; ++i)
    {
        const float a = std::abs (samples[i]);
        maxAbs = std::max (maxAbs, a);

        if (a >= clipLinear)
        {
            ++consecutive;
            if (consecutive >= kClipConsecutiveSamples)
            {
                clipLatched.store (true, std::memory_order_relaxed);
                clipCount.fetch_add (1, std::memory_order_relaxed);
                consecutive = 0; // start counting the next clip event independently
            }
        }
        else
        {
            consecutive = 0;
        }
    }

    consecutiveClipSamples.store (consecutive, std::memory_order_relaxed);
    publishPeak (linearToDb (maxAbs));
}

void Metering::processLevelOnly (const float* samples, int numSamples) noexcept
{
    float maxAbs = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        maxAbs = std::max (maxAbs, std::abs (samples[i]));

    publishPeak (linearToDb (maxAbs));
}

void Metering::noteClipEvents (int events) noexcept
{
    if (events <= 0)
        return;

    clipLatched.store (true, std::memory_order_relaxed);
    clipCount.fetch_add (events, std::memory_order_relaxed);
}

void Metering::publishPeak (float blockDb) noexcept
{
    // Keep the loudest block since the last tick. At 64-sample buffers about
    // twelve blocks land per UI tick; storing only the latest dropped a short
    // transient before the meter, peak hold or SetupAdvisor ever saw it.
    const float pending = pendingPeakDb.load (std::memory_order_relaxed);
    pendingPeakDb.store (std::max (pending, blockDb), std::memory_order_relaxed);
}

void Metering::pushBlockStats (float maxAbsLinear, int /*numSamplesInBlock*/) noexcept
{
    publishPeak (linearToDb (maxAbsLinear));
    if (maxAbsLinear >= clipThresholdLinear)
    {
        clipLatched.store (true, std::memory_order_relaxed);
        clipCount.fetch_add (1, std::memory_order_relaxed);
    }
}

float Metering::tick (double dtSeconds) noexcept
{
    const float pending = pendingPeakDb.exchange (kNothingNew, std::memory_order_relaxed);
    if (pending > kNothingNew)
        lastBlockDb = pending;
    const float blockDb = lastBlockDb;

    // Exponential attack/decay envelope toward the incoming block level.
    const bool rising = blockDb > displayedDb;
    const double timeConstant = rising ? kAttackSeconds : kDecaySeconds;
    const double coeff = (timeConstant > 0.0) ? std::exp (-dtSeconds / timeConstant) : 0.0;
    displayedDb = static_cast<float> (blockDb + (displayedDb - blockDb) * coeff);
    displayedDb = std::clamp (displayedDb, kMinDb, kMaxDb);

    // Peak hold: latch a new peak immediately, hold for kPeakHoldSeconds, then
    // decay at kPeakDecayDbPerSecond.
    if (blockDb >= peakHoldDb)
    {
        peakHoldDb = blockDb;
        peakHoldElapsed = 0.0;
    }
    else
    {
        peakHoldElapsed += dtSeconds;
        if (peakHoldElapsed > kPeakHoldSeconds)
        {
            // Only the part of this step past the hold decays. With steps of
            // real elapsed time -- a throttled timer can hand over a second
            // at once -- charging the whole step dropped a peak up to 20 dB
            // in the tick its hold ran out.
            const double decaying = std::min (dtSeconds, peakHoldElapsed - kPeakHoldSeconds);
            peakHoldDb = static_cast<float> (peakHoldDb - kPeakDecayDbPerSecond * decaying);
            peakHoldDb = std::max (peakHoldDb, blockDb);
            peakHoldDb = std::clamp (peakHoldDb, kMinDb, kMaxDb);
        }
    }

    return displayedDb;
}

void Metering::acknowledgeClip() noexcept
{
    clipLatched.store (false, std::memory_order_relaxed);
    consecutiveClipSamples.store (0, std::memory_order_relaxed);
}

} // namespace mma
