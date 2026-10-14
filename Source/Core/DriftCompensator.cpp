#include "DriftCompensator.h"
#include <algorithm>
#include <cmath>

namespace mma {

DriftCompensator::DriftCompensator (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn)
{
}

void DriftCompensator::reset() noexcept
{
    integralTerm = 0.0;
    currentPpm = 0.0;
    excessDriftSeconds = 0.0;
    sustainedExcessDrift = false;
}

void DriftCompensator::update (double fillError, int blockSizeSamples) noexcept
{
    const double maxDeviation = kMaxRatioDeviationPpm * 1.0e-6;

    const double proportional = kp * fillError;
    const double candidateIntegral = std::clamp (integralTerm + ki * fillError,
                                                 -maxDeviation, maxDeviation);

    const double target = std::clamp (proportional + candidateIntegral, -maxDeviation, maxDeviation);

    // Never correct instantaneously: limit slew to kMaxSlewPpmPerSecond, expressed per this block.
    const double blockSeconds = (sampleRate > 0.0) ? (static_cast<double> (blockSizeSamples) / sampleRate) : 0.0;
    const double maxStep = (slewPpmPerSecond * 1.0e-6) * blockSeconds;

    double currentRatioDeviation = currentPpm * 1.0e-6;
    const double wanted = target - currentRatioDeviation;
    const double delta = std::clamp (wanted, -maxStep, maxStep);

    // Anti-windup. The actuator is deliberately rate limited, so while it is
    // still slewing towards target the loop cannot deliver what the integral is
    // asking for. Integrating anyway stores up correction that has to be
    // unwound later -- at the same slew -- and the result is a loop that
    // swings past target every time instead of settling on it.
    //
    // So integration pauses while the output is rate- or clamp-limited AND
    // the error would push the integral further the way the output is already
    // straining. Pausing on every limited block, which this used to do, froze
    // the integral for good on real hardware: the level the loop reads
    // carries a sample or two of jitter, the proportional term turns that
    // into several PPM of request, and the slew step per block is a few
    // hundredths of a PPM -- so the output was limited on every block and
    // the integral never moved. The loop ran on its proportional term alone,
    // holding a microphone 100 PPM off the output 20 samples below its
    // target for the whole session, and the gaps that left room for did the
    // correcting instead: a click every few seconds. An error that pulls the
    // integral back towards the output is never windup, so it is always
    // taken.
    const bool limitedUp = delta < wanted;
    const bool limitedDown = delta > wanted;
    const bool deeper = (limitedUp && fillError > 0.0) || (limitedDown && fillError < 0.0);

    if (! deeper)
        integralTerm = candidateIntegral;

    currentRatioDeviation = std::clamp (currentRatioDeviation + delta, -maxDeviation, maxDeviation);
    currentPpm = currentRatioDeviation * 1.0e6;
}

void DriftCompensator::updateSustainedDriftFlag (double elapsedSeconds, double referencePpm) noexcept
{
    // §3.3 flags a device that is far from the *master*. Every channel's PPM
    // also carries the output device's own skew, identically, so subtracting
    // the master's figure removes it -- otherwise an output clock a long way
    // off nominal would flag every microphone in the rig at once.
    const double relativePpm = currentPpm - referencePpm;

    if (std::abs (relativePpm) > kExcessDriftThresholdPpm)
    {
        excessDriftSeconds += elapsedSeconds;
        if (excessDriftSeconds >= kExcessDriftSustainSeconds)
            sustainedExcessDrift = true;
    }
    else
    {
        excessDriftSeconds = 0.0;
        sustainedExcessDrift = false;
    }
}

} // namespace mma
