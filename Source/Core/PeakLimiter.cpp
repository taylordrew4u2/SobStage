#include "PeakLimiter.h"
#include <algorithm>
#include <cmath>

namespace mma {

namespace {

// Within 0.001 dB of unity counts as all the way back. An exponential
// release only approaches 1, so without a floor the gain would hover a hair
// under it for good and the next loud passage would never count as new.
constexpr float kUnityGainFloor = 0.9999f;

// A sample within 1 dB of the ceiling, at the gain being held, renews the
// hold: the signal is still at the level that set the gain.
constexpr float kHoldRefreshFraction = 0.891f;

} // namespace

PeakLimiter::PeakLimiter (double sampleRate, float ceilingLinear) noexcept
    : ceiling (ceilingLinear > 0.0f ? ceilingLinear : 1.0f)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // Computed here, once, so the audio thread never calls std::exp.
    releaseCoefficient = static_cast<float> (std::exp (-1.0 / (kReleaseSeconds * rate)));
    holdSamples = std::max (0, static_cast<int> (std::lround (kHoldSeconds * rate)));
}

void PeakLimiter::reset() noexcept
{
    gain = 1.0f;
    holdRemaining = 0;
    overCeiling = false;
}

float PeakLimiter::process (float sample) noexcept
{
    if (! std::isfinite (sample))
    {
        overCeiling = false;
        return 0.0f;
    }

    const float magnitude = std::abs (sample);

    // Release first, so a sample arriving as the gain recovers is judged
    // against the gain it would actually get.
    float g = gain;

    if (holdRemaining > 0)
    {
        --holdRemaining;
    }
    else if (g < 1.0f)
    {
        g = 1.0f - (1.0f - g) * releaseCoefficient;

        if (g >= kUnityGainFloor)
            g = 1.0f;
    }

    overCeiling = magnitude > ceiling;

    const float reached = magnitude * g;

    if (reached > ceiling)
    {
        // Instant attack: this very sample lands on the ceiling, and the gain
        // that put it there is held while the waveform comes back round.
        if (g >= 1.0f)
            ++engagements;

        g = ceiling / magnitude;
        holdRemaining = holdSamples;
    }
    else if (g < 1.0f && reached >= ceiling * kHoldRefreshFraction)
    {
        // Still as loud as the passage that set this gain: keep holding it.
        // Without this a steady loud vowel, whose every crest lands exactly
        // on the ceiling, let the gain go 10 ms after the first one -- and the
        // next crest came in over the ceiling again and was squared off,
        // every 10 ms for as long as the vowel lasted.
        holdRemaining = holdSamples;
    }

    gain = g;

    // The backstop. ceiling / magnitude * sample can round an ulp over the
    // ceiling; nothing else can reach this clamp.
    return std::clamp (sample * g, -ceiling, ceiling);
}

} // namespace mma
