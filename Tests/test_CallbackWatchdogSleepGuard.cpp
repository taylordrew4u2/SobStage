#include "TestFramework.h"
#include "Platform/CallbackWatchdogSleepGuard.h"

using namespace mma;

// Times are seconds. The awake clock stops while the Mac sleeps; the
// sleep-counting clock does not, and sits at an unrelated offset, as the wall
// clock does on a Mac.
namespace {

constexpr double kWallOffset = 8.0e8;

/// Polls with both clocks having moved by the same awake time.
void pollAwake (CallbackWatchdogSleepGuard& guard, double awake, double& wall, double step,
                double latestCallback)
{
    wall += step;
    guard.poll (awake, kWallOffset + wall, latestCallback);
}

} // namespace

TEST_CASE (CallbackWatchdogSleepGuard_AwakeTheWatchdogMeasuresFromTheLastCallback)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 100.0;

    guard.poll (100.0, kWallOffset + wall, 99.0);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (99.0, 90.0) == 99.0);

    // Before the first callback, from the start.
    REQUIRE (guard.silenceBegan (0.0, 90.0) == 90.0);

    // Neither known: not judged.
    REQUIRE (guard.silenceBegan (0.0, 0.0) == 0.0);

    for (double t = 100.5; t < 120.0; t += 0.5)
    {
        pollAwake (guard, t, wall, 0.5, 99.0);
        REQUIRE_FALSE (guard.isWaitingForAudio());
    }

    // A device that really stopped is still measured from its last callback.
    REQUIRE (guard.silenceBegan (99.0, 90.0) == 99.0);
}

TEST_CASE (CallbackWatchdogSleepGuard_TheFirstPollHasNothingToCompareWith)
{
    // However far apart the two clocks start, that is not a sleep.
    CallbackWatchdogSleepGuard guard;
    guard.poll (5.0, kWallOffset + 100000.0, 4.0);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (4.0, 1.0) == 4.0);
}

TEST_CASE (CallbackWatchdogSleepGuard_ASleepHoldsTheWatchdogUntilAudioRunsAgain)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 100.0;

    guard.poll (100.0, kWallOffset + wall, 100.0);

    // IO stopped at 100; the Mac fell asleep 3 s later and slept an hour.
    pollAwake (guard, 103.0, wall, 3.0, 100.0);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    wall += 3600.0;
    pollAwake (guard, 103.5, wall, 0.5, 100.0);
    REQUIRE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (100.0, 50.0) == 0.0);
    REQUIRE (guard.silenceBegan (0.0, 50.0) == 0.0);

    // The devices take their time starting again. Still held.
    for (double t = 104.0; t <= 115.0; t += 0.5)
    {
        pollAwake (guard, t, wall, 0.5, 100.0);
        REQUIRE (guard.isWaitingForAudio());
        REQUIRE (guard.silenceBegan (100.0, 50.0) == 0.0);
    }

    // One device delivers again: the watchdog resumes, and every device's
    // silence is counted from now, not from before the sleep.
    pollAwake (guard, 115.5, wall, 0.5, 115.2);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (100.0, 50.0) == 115.5);
    REQUIRE (guard.silenceBegan (115.2, 50.0) == 115.5);

    // A callback after that is measured from itself; so is a stream opened
    // after the wake.
    REQUIRE (guard.silenceBegan (117.0, 50.0) == 117.0);
    REQUIRE (guard.silenceBegan (0.0, 116.0) == 116.0);
}

TEST_CASE (CallbackWatchdogSleepGuard_ACallbackFromBeforeTheWakeDoesNotEndTheWait)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 10.0;

    guard.poll (10.0, kWallOffset + wall, 9.9);
    wall += 600.0;
    pollAwake (guard, 10.5, wall, 0.5, 10.2); // stamped before this poll noticed the sleep
    REQUIRE (guard.isWaitingForAudio());

    pollAwake (guard, 11.0, wall, 0.5, 10.2);
    REQUIRE (guard.isWaitingForAudio());

    pollAwake (guard, 11.5, wall, 0.5, 10.6);
    REQUIRE_FALSE (guard.isWaitingForAudio());
}

TEST_CASE (CallbackWatchdogSleepGuard_ADarkWakeKeepsTheWatchdogHeld)
{
    // Awake for maintenance with the lid shut: the app polls, audio is off.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;

    guard.poll (1000.0, kWallOffset + wall, 1000.0);
    wall += 1800.0;

    double t = 1000.5;
    pollAwake (guard, t, wall, 0.5, 1000.0);

    for (int i = 0; i < 45; ++i)
    {
        t += 1.0;
        pollAwake (guard, t, wall, 1.0, 1000.0);
        REQUIRE (guard.isWaitingForAudio());
    }

    // Back to sleep, then woken for long enough that a wait counted from the
    // first wake would have run out: it is counted from the latest one.
    wall += 1800.0;
    t += 0.5;
    pollAwake (guard, t, wall, 0.5, 1000.0);
    REQUIRE (guard.isWaitingForAudio());

    for (int i = 0; i < 30; ++i)
    {
        t += 1.0;
        pollAwake (guard, t, wall, 1.0, 1000.0);
        REQUIRE (guard.isWaitingForAudio());
    }
}

TEST_CASE (CallbackWatchdogSleepGuard_TheWaitRunsOutIfNothingRunsAgain)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;

    guard.poll (50.0, kWallOffset + wall, 49.0);
    wall += 600.0;
    pollAwake (guard, 50.5, wall, 0.5, 49.0);
    REQUIRE (guard.isWaitingForAudio());

    const double wokeAt = 50.5;
    pollAwake (guard, wokeAt + CallbackWatchdogSleepGuard::kMaxWaitForAudioSeconds - 0.5, wall,
               CallbackWatchdogSleepGuard::kMaxWaitForAudioSeconds - 0.5, 49.0);
    REQUIRE (guard.isWaitingForAudio());

    const double gaveUpAt = wokeAt + CallbackWatchdogSleepGuard::kMaxWaitForAudioSeconds;
    pollAwake (guard, gaveUpAt, wall, 0.5, 49.0);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    // Judged again, with the usual grace from here.
    REQUIRE (guard.silenceBegan (49.0, 10.0) == gaveUpAt);
}

TEST_CASE (CallbackWatchdogSleepGuard_ASmallOrBackwardStepIsNotASleep)
{
    CallbackWatchdogSleepGuard guard;

    guard.poll (10.0, kWallOffset + 10.0, 10.0);

    // The wall clock gaining under a second on the awake clock (a time sync
    // nudging it) is not a sleep.
    guard.poll (10.5, kWallOffset + 11.4, 10.4);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    // Nor is it stepping back.
    guard.poll (11.0, kWallOffset + 5.0, 10.9);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    // Just over the line is.
    guard.poll (11.5, kWallOffset + 6.6, 10.9);
    REQUIRE (guard.isWaitingForAudio());
}
