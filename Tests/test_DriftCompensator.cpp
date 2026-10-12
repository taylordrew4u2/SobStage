#include "TestFramework.h"
#include "Core/DriftCompensator.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace mma;

TEST_CASE (DriftCompensator_StartsAtUnity)
{
    DriftCompensator dc (48000.0);
    REQUIRE_NEAR (dc.getRatio(), 1.0, 1e-12);
    REQUIRE_NEAR (dc.getPpm(), 0.0, 1e-12);
}

TEST_CASE (DriftCompensator_PositiveFillErrorPushesRatioUp)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 1000; ++i)
        dc.update (1000.0, 64);
    REQUIRE (dc.getPpm() > 0.0);
}

TEST_CASE (DriftCompensator_ClampsToMax200Ppm)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64);
    REQUIRE (dc.getPpm() <= DriftCompensator::kMaxRatioDeviationPpm + 1e-6);
    REQUIRE (dc.getPpm() >= DriftCompensator::kMaxRatioDeviationPpm - 1e-6);
}

TEST_CASE (DriftCompensator_ClampsToMinNegative200Ppm)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (-1.0e9, 64);
    REQUIRE (dc.getPpm() <= -DriftCompensator::kMaxRatioDeviationPpm + 1e-6);
    REQUIRE (dc.getPpm() >= -DriftCompensator::kMaxRatioDeviationPpm - 1e-6);
}

TEST_CASE (DriftCompensator_NeverJumpsFasterThanSlewLimit)
{
    DriftCompensator dc (48000.0);
    const int blockSize = 64;
    const double blockSeconds = blockSize / 48000.0;
    const double maxStepPpm = DriftCompensator::kMaxSlewPpmPerSecond * blockSeconds;

    double previousPpm = dc.getPpm();
    for (int i = 0; i < 5000; ++i)
    {
        dc.update (1.0e9, blockSize); // huge error, forces the slew limiter to be the binding constraint
        double newPpm = dc.getPpm();
        REQUIRE (std::abs (newPpm - previousPpm) <= maxStepPpm + 1e-9);
        previousPpm = newPpm;
    }
}

TEST_CASE (DriftCompensator_ResetClearsState)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 1000; ++i)
        dc.update (1000.0, 64);
    REQUIRE (dc.getPpm() != 0.0);
    dc.reset();
    REQUIRE_NEAR (dc.getPpm(), 0.0, 1e-12);
}

TEST_CASE (DriftCompensator_SustainedExcessDriftFlagsAfterThreshold)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64); // drive ppm above the 100ppm excess-drift threshold
    REQUIRE (dc.getPpm() > 100.0);

    REQUIRE_FALSE (dc.isSustainedExcessDrift());
    dc.updateSustainedDriftFlag (5.0);
    REQUIRE_FALSE (dc.isSustainedExcessDrift());
    dc.updateSustainedDriftFlag (5.0);
    REQUIRE (dc.isSustainedExcessDrift());
}

TEST_CASE (DriftCompensator_SustainedExcessDriftIsJudgedAgainstTheClockMaster)
{
    // §3.3 flags a device that is far from the *master*, not from whatever
    // clock happens to be pulling it.
    //
    // Every channel is corrected onto the output stream now (§3.2), so an output
    // device a long way off nominal shows up in every channel's PPM at once. Judged
    // absolutely that would flag the entire rig as unreliable the moment the
    // listener plugged in an unusual interface -- every mic reported broken
    // because the headphones are unusual. Relative to the master it cancels.
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64);
    REQUIRE (dc.getPpm() > 100.0);

    // The master sits at the same offset: this device is not drifting from it.
    const double masterPpm = dc.getPpm();
    dc.updateSustainedDriftFlag (10.0, masterPpm);
    REQUIRE_FALSE (dc.isSustainedExcessDrift());

    // A master 150 PPM the other way is a genuine disagreement, and is flagged.
    dc.updateSustainedDriftFlag (10.0, masterPpm - 150.0);
    REQUIRE (dc.isSustainedExcessDrift());
}

TEST_CASE (DriftCompensator_SustainedExcessDriftFlagClearsWhenBackInRange)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64);
    dc.updateSustainedDriftFlag (10.0);
    REQUIRE (dc.isSustainedExcessDrift());

    dc.reset();
    dc.updateSustainedDriftFlag (10.0);
    REQUIRE_FALSE (dc.isSustainedExcessDrift());
}

TEST_CASE (DriftCompensator_SettlesInsteadOfOscillating)
{
    // The regression this exists for: the integral saturated at the +/-200 PPM
    // clamp long before the slew-limited output (5 PPM/s then) could reach it,
    // so every time the fill crossed target the loop had to unwind from
    // saturation at the same slew. Over hours it swung between +140 and -45
    // PPM instead of settling, and starved rings badly enough to underrun.
    DriftCompensator c (48000.0);

    // A device running 100 PPM fast: its ring fills, so the loop must raise the
    // playout ratio until the fill error it sees settles at a steady offset.
    double fill = 0.0;
    double lastPpm = 0.0;
    double maxLateSwing = 0.0;

    for (int block = 0; block < 1200000; ++block)
    {
        // Fill grows by the clock mismatch and shrinks by whatever correction
        // is currently applied -- the same feedback the real ring provides.
        fill += 64.0 * (100.0e-6 - c.getPpm() * 1.0e-6);
        c.update (fill, 64);

        // Once the loop has had ample time to settle, it must stay settled.
        if (block > 600000)
            maxLateSwing = std::max (maxLateSwing, std::abs (c.getPpm() - lastPpm));

        lastPpm = c.getPpm();
    }

    REQUIRE_NEAR (c.getPpm(), 100.0, 0.5);
    REQUIRE (maxLateSwing < 0.01);
}

TEST_CASE (DriftCompensator_DoesNotIntegrateWhileRateLimited)
{
    DriftCompensator a (48000.0);
    DriftCompensator b (48000.0);

    // A huge, sustained error drives the output hard against its slew limit.
    for (int i = 0; i < 50; ++i)
        a.update (1.0e6, 64);

    // The same loop reaching the same place gradually. If the integral had wound
    // up during the rate-limited climb, the first would now be carrying stored
    // correction the second is not, and would overshoot when the error clears.
    for (int i = 0; i < 50; ++i)
        b.update (200.0, 64);

    for (int i = 0; i < 2000; ++i) { a.update (0.0, 64); b.update (0.0, 64); }

    // With the error gone, both must return towards zero rather than one of them
    // continuing to push from a saturated integral.
    REQUIRE (a.getPpm() < 5.0);
    REQUIRE (b.getPpm() < 5.0);
}

TEST_CASE (DriftCompensator_TheIntegralStillWorksWhenTheLevelIsNoisy)
{
    // The level the loop reads always carries a sample or two of jitter:
    // a device delivers a little early or late, the output pulls a little
    // early or late. The proportional term turns that into several PPM of
    // request on every block, and the slew allows a few hundredths of a PPM
    // a block, so the output is rate-limited on every single block. The
    // integral used to pause whenever the output was limited at all, which
    // here meant forever: the loop ran on its proportional term alone and
    // held a device 100 PPM slow 20 samples below its target for the whole
    // session -- with gaps, not the loop, making up the difference.
    //
    // Pausing only when the error would push the integral further the way
    // the output is already straining keeps the protection against windup
    // and lets the integral carry the drift: the level settles on target.
    DriftCompensator c (48000.0);

    uint64_t seed = 0x9E3779B97F4A7C15ull;
    const auto noise = [&seed]
    {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return (static_cast<double> (seed >> 11) / 9007199254740992.0 - 0.5) * 4.0; // +/-2 samples
    };

    double fill = 0.0, sum = 0.0;
    int counted = 0;
    const int blocks = static_cast<int> (240.0 * 48000.0 / 64.0); // four minutes

    for (int block = 0; block < blocks; ++block)
    {
        // A device 100 PPM slow drains the ring; the loop's correction
        // slows the output to match.
        fill += 64.0 * (-100.0e-6 - c.getPpm() * 1.0e-6);
        c.update (fill + noise(), 64);

        if (block > blocks / 2)
        {
            sum += fill;
            ++counted;
        }
    }

    // On the clock, and on target: not parked at the -20 samples a
    // proportional-only loop needs for 100 PPM.
    REQUIRE_NEAR (c.getPpm(), -100.0, 5.0);
    REQUIRE (std::abs (sum / counted) < 3.0);
}

TEST_CASE (DriftCompensator_CatchesAFarDeviceBeforeItsRingRunsDown)
{
    // Every stream starts at zero PPM, and a device far off the output's
    // clock drains its ring until the loop arrives. At 5 PPM/s a device
    // 200 PPM slow took forty seconds to catch and ran its ring 190 samples
    // down -- past a 64-sample block's whole cushion, a gap at the top of
    // every take. At the slew the loop has now it is caught in ten seconds,
    // and the ring never goes a block down.
    DriftCompensator c (48000.0);

    double fill = 0.0, lowest = 0.0;

    for (int block = 0; block < static_cast<int> (60.0 * 48000.0 / 64.0); ++block)
    {
        fill += 64.0 * (-200.0e-6 - c.getPpm() * 1.0e-6);
        c.update (fill, 64);
        lowest = std::min (lowest, fill);
    }

    REQUIRE (lowest > -64.0);
    REQUIRE_NEAR (c.getPpm(), -200.0, 1.0);

    // And no faster than the slew, which keeps any change of pitch
    // gradual: 20 PPM a second is 0.035 of a cent a second.
    REQUIRE (DriftCompensator::kMaxSlewPpmPerSecond <= 20.0);
}
