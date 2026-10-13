#include "TestFramework.h"
#include "Core/PeakLimiter.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace mma;

namespace {

constexpr double kRate = 48000.0;
constexpr float kTwoPi = 6.2831853f;

float sineAt (float amplitude, float hz, int i)
{
    return amplitude * std::sin (kTwoPi * hz * static_cast<float> (i) / static_cast<float> (kRate));
}

} // namespace

TEST_CASE (PeakLimiter_PassesEverythingUnderTheCeilingUntouched)
{
    PeakLimiter limiter (kRate, 0.5f);

    for (int i = 0; i < 4800; ++i)
    {
        const float x = sineAt (0.49f, 440.0f, i);
        REQUIRE (limiter.process (x) == x);
    }

    REQUIRE (limiter.getEngagements() == 0u);
    REQUIRE (limiter.getGain() == 1.0f);
}

TEST_CASE (PeakLimiter_NeverLetsASampleOverTheCeiling)
{
    PeakLimiter limiter (kRate, 0.708f);
    uint64_t state = 12345;

    for (int i = 0; i < 48000; ++i)
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const float x = (static_cast<float> (state >> 40) / static_cast<float> (1u << 24) - 0.5f) * 20.0f;
        REQUIRE (std::abs (limiter.process (x)) <= 0.708f);
    }
}

TEST_CASE (PeakLimiter_AttacksOnTheSampleThatGoesOver)
{
    // Zero lookahead, nothing delayed: the first sample over the ceiling is
    // itself put on the ceiling, not the one after it.
    PeakLimiter limiter (kRate, 0.5f);
    REQUIRE (limiter.process (0.2f) == 0.2f);
    REQUIRE_NEAR (limiter.process (2.0f), 0.5f, 1e-7);
    REQUIRE (limiter.wasOverCeiling());
    REQUIRE_NEAR (limiter.getGain(), 0.25f, 1e-7);
    REQUIRE (limiter.getEngagements() == 1u);
}

TEST_CASE (PeakLimiter_HoldsThenReleasesOverFiftyMilliseconds)
{
    PeakLimiter limiter (kRate, 0.5f);
    limiter.process (1.0f); // gain 0.5

    const int holdSamples = static_cast<int> (PeakLimiter::kHoldSeconds * kRate);
    for (int i = 0; i < holdSamples; ++i)
        limiter.process (0.0f);
    REQUIRE_NEAR (limiter.getGain(), 0.5f, 1e-6); // held for the whole 10 ms

    // One release time constant later, 1/e of the reduction is left.
    const int releaseSamples = static_cast<int> (PeakLimiter::kReleaseSeconds * kRate);
    for (int i = 0; i < releaseSamples; ++i)
        limiter.process (0.0f);
    REQUIRE_NEAR (limiter.getGain(), 1.0f - 0.5f * std::exp (-1.0f), 2e-3);

    // And it does get all the way back.
    for (int i = 0; i < 48000; ++i)
        limiter.process (0.0f);
    REQUIRE (limiter.getGain() == 1.0f);
}

TEST_CASE (PeakLimiter_ASteadyLoudToneKeepsOneGainInsteadOfPumping)
{
    // Every crest of a steady tone lands exactly on the ceiling once the gain
    // is set. Those crests must keep the gain held: if it were let go 10 ms
    // after the first, each later crest would come in over the ceiling again
    // and have its rise squared off, every 10 ms.
    PeakLimiter limiter (kRate, 0.5f);
    float lowest = 1.0f, highest = 0.0f;
    int flatTopped = 0;
    float previous = 0.0f;

    for (int i = 0; i < 48000; ++i)
    {
        const float out = limiter.process (sineAt (1.0f, 150.0f, i));

        if (i > 960)
        {
            lowest = std::min (lowest, limiter.getGain());
            highest = std::max (highest, limiter.getGain());

            if (std::abs (out) >= 0.49999f && std::abs (previous) >= 0.49999f)
                ++flatTopped;
        }

        previous = out;
    }

    REQUIRE (highest - lowest < 1e-3f);
    REQUIRE (flatTopped == 0);
    REQUIRE (limiter.getEngagements() == 1u);
}

TEST_CASE (PeakLimiter_IsFarCleanerThanTheHardClipItReplaced)
{
    // The same 220 Hz tone, 6 dB over the ceiling, through each. The residual
    // after the best single gain is the distortion: for the hard clip it is
    // the squared-off third of every cycle; the limiter, once it has its
    // gain, adds none.
    const float ceiling = 0.708f;
    PeakLimiter limiter (kRate, ceiling);

    double inIn = 0.0, inLimited = 0.0, limitedLimited = 0.0;
    double inClipped = 0.0, clippedClipped = 0.0;

    for (int i = 0; i < 48000; ++i)
    {
        const float x = sineAt (2.0f * ceiling, 220.0f, i);
        const float limited = limiter.process (x);
        const float clipped = std::clamp (x, -ceiling, ceiling);

        // Compared once both are in their steady state, after the first
        // cycle: the limiter's one squared-off rise is covered elsewhere.
        if (i < 240)
            continue;

        inIn += static_cast<double> (x) * x;
        inLimited += static_cast<double> (x) * limited;
        limitedLimited += static_cast<double> (limited) * limited;
        inClipped += static_cast<double> (x) * clipped;
        clippedClipped += static_cast<double> (clipped) * clipped;
    }

    const auto distortionDb = [inIn] (double cross, double own)
    {
        const double a = cross / inIn;
        const double residual = own - 2.0 * a * cross + a * a * inIn;
        return 10.0 * std::log10 (a * a * inIn / std::max (1e-30, residual));
    };

    const double limiterDb = distortionDb (inLimited, limitedLimited);
    const double clipDb = distortionDb (inClipped, clippedClipped);

    REQUIRE (clipDb < 25.0);     // what the headphones used to get
    REQUIRE (limiterDb > 50.0);  // what they get now
}

TEST_CASE (PeakLimiter_AFaultySampleIsSilenceAndDoesNotMoveTheGain)
{
    PeakLimiter limiter (kRate, 0.5f);
    REQUIRE (limiter.process (std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    REQUIRE (limiter.process (std::numeric_limits<float>::infinity()) == 0.0f);
    REQUIRE (limiter.process (-std::numeric_limits<float>::infinity()) == 0.0f);
    REQUIRE (limiter.getGain() == 1.0f);
    REQUIRE (limiter.getEngagements() == 0u);
    REQUIRE (limiter.process (0.3f) == 0.3f);
}

TEST_CASE (PeakLimiter_ResetReturnsToUnityButKeepsTheCount)
{
    PeakLimiter limiter (kRate, 0.5f);
    limiter.process (2.0f);
    REQUIRE (limiter.getGain() < 1.0f);

    limiter.reset();
    REQUIRE (limiter.getGain() == 1.0f);
    REQUIRE (limiter.process (0.4f) == 0.4f);
    REQUIRE (limiter.getEngagements() == 1u);
}
