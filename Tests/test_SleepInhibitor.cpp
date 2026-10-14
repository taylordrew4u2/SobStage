#include "TestFramework.h"
#include "Platform/SleepInhibitor.h"

#include <memory>

using namespace mma;

namespace {

struct Counts
{
    int acquires = 0;
    int releases = 0;
    bool acquireSucceeds = true;
    bool heldByOs = false;
};

class FakeBackend final : public SleepInhibitor::Backend
{
public:
    explicit FakeBackend (Counts& c) : counts (c) {}

    bool acquire() override
    {
        ++counts.acquires;
        counts.heldByOs = counts.acquireSucceeds;
        return counts.acquireSucceeds;
    }

    void release() override
    {
        ++counts.releases;
        counts.heldByOs = false;
    }

private:
    Counts& counts;
};

} // namespace

TEST_CASE (SleepInhibitor_HeldWhileRecordingReleasedOnStop)
{
    Counts counts;
    SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));

    REQUIRE_FALSE (inhibitor.isHeld());

    inhibitor.setHeld (true);   // take starts
    REQUIRE (inhibitor.isHeld());
    REQUIRE (counts.heldByOs);

    inhibitor.setHeld (false);  // take stops
    REQUIRE_FALSE (inhibitor.isHeld());
    REQUIRE_FALSE (counts.heldByOs);
    REQUIRE (counts.acquires == 1);
    REQUIRE (counts.releases == 1);
}

TEST_CASE (SleepInhibitor_RepeatedTicksDoNotStackAssertions)
{
    // The app mirrors "is recording" into it from its status tick, so the
    // same answer arrives many times; it must not acquire or release again.
    Counts counts;
    SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));

    for (int i = 0; i < 5; ++i)
        inhibitor.setHeld (true);
    for (int i = 0; i < 5; ++i)
        inhibitor.setHeld (false);

    REQUIRE (counts.acquires == 1);
    REQUIRE (counts.releases == 1);
}

TEST_CASE (SleepInhibitor_QuitMidTakeReleases)
{
    Counts counts;
    {
        SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));
        inhibitor.setHeld (true);
    }
    REQUIRE (counts.releases == 1);
    REQUIRE_FALSE (counts.heldByOs);
}

TEST_CASE (SleepInhibitor_FailedAcquireIsRetriedAndNeverReleased)
{
    Counts counts;
    counts.acquireSucceeds = false;
    {
        SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));
        inhibitor.setHeld (true);
        REQUIRE_FALSE (inhibitor.isHeld());

        counts.acquireSucceeds = true;
        inhibitor.setHeld (true);
        REQUIRE (inhibitor.isHeld());
        REQUIRE (counts.acquires == 2);
    }
    REQUIRE (counts.releases == 1);
}

TEST_CASE (SleepInhibitor_PlatformBackendIsSafeEverywhere)
{
    // Off the Mac this is a no-op; on the Mac it takes and drops a real
    // assertion. Either way it must not throw or leave anything held.
    SleepInhibitor inhibitor;
    inhibitor.setHeld (true);
#if ! defined(__APPLE__)
    REQUIRE_FALSE (inhibitor.isHeld());
#endif
    inhibitor.setHeld (false);
    REQUIRE_FALSE (inhibitor.isHeld());
}

TEST_CASE (SleepInhibitor_PowerSourceProbeIsSafeEverywhere)
{
    // Off the Mac there is nothing to ask, and Unknown is never a battery
    // warning. On the Mac it answers whatever is powering the runner.
    const auto source = SleepInhibitor::queryPowerSource();
#if ! defined(__APPLE__)
    REQUIRE (source == PowerSource::Unknown);
#else
    REQUIRE (source == PowerSource::Unknown || source == PowerSource::AC || source == PowerSource::Battery);
#endif
}

// Windows and Linux say nothing when the computer has slept: each audio
// stream simply dies on waking. Time on the clock that counts sleep and not
// on the one that does not is how it is noticed. Mutation: any difference
// between the clocks read as a sleep, or none ever.
TEST_CASE (WakeDetector_TimeAsleepIsWhatOneClockCountsAndTheOtherDoesNot)
{
    WakeDetector w;
    REQUIRE_FALSE (w.justWoke ({ 1000.0, 5000.0 })); // takes its bearings
    REQUIRE_FALSE (w.justWoke ({ 2000.0, 6000.0 })); // both moved alike
    REQUIRE_FALSE (w.justWoke ({ 3000.0, 7016.0 })); // a clock's own 16 ms step

    REQUIRE (w.justWoke ({ 3500.0, 7516.0 + 45.0 * 60.0 * 1000.0 })); // 45 minutes asleep
    REQUIRE_FALSE (w.justWoke ({ 4500.0, 8516.0 + 45.0 * 60.0 * 1000.0 }));

    // Just over the threshold.
    REQUIRE (w.justWoke ({ 5500.0, 9516.0 + 45.0 * 60.0 * 1000.0 + WakeDetector::kMinSleepMs }));

    // This computer's clocks can be read, and say nothing about a sleep
    // between two reads a moment apart.
    const auto first = WakeDetector::read();
    REQUIRE (first.has_value());
    WakeDetector live;
    REQUIRE_FALSE (live.justWoke (*first));
    REQUIRE_FALSE (live.justWoke (*WakeDetector::read()));
}
