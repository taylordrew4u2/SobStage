#include "TestFramework.h"
#include "Platform/CallbackWatchdogSleepGuard.h"

using namespace mma;

// Times are seconds. The awake clock stops while the Mac sleeps; the
// sleep-counting clock does not, and sits at an unrelated offset, as the wall
// clock does on a Mac.
namespace {

constexpr double kWallOffset = 8.0e8;

/// What the Mac says about its audio: on, as when it is fully awake (or when it
/// cannot be asked), and off, as in a dark wake.
bool audioOn() noexcept { return false; }
bool audioOff() noexcept { return true; }

/// Polls with both clocks having moved by the same awake time.
void pollAwake (CallbackWatchdogSleepGuard& guard, double awake, double& wall, double step,
                double latestCallback, bool (*systemAudioIsOff)() noexcept = audioOn)
{
    wall += step;
    guard.poll (awake, kWallOffset + wall, latestCallback, systemAudioIsOff);
}

} // namespace

TEST_CASE (CallbackWatchdogSleepGuard_AwakeTheWatchdogMeasuresFromTheLastCallback)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 100.0;

    guard.poll (100.0, kWallOffset + wall, 99.0, audioOn);
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
    guard.poll (5.0, kWallOffset + 100000.0, 4.0, audioOn);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (4.0, 1.0) == 4.0);
}

TEST_CASE (CallbackWatchdogSleepGuard_ASleepHoldsTheWatchdogUntilAudioRunsAgain)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 100.0;

    guard.poll (100.0, kWallOffset + wall, 100.0, audioOn);

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

    guard.poll (10.0, kWallOffset + wall, 9.9, audioOn);
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

    guard.poll (1000.0, kWallOffset + wall, 1000.0, audioOn);
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

    guard.poll (50.0, kWallOffset + wall, 49.0, audioOn);
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

    guard.poll (10.0, kWallOffset + 10.0, 10.0, audioOn);

    // The wall clock gaining under a second on the awake clock (a time sync
    // nudging it) is not a sleep.
    guard.poll (10.5, kWallOffset + 11.4, 10.4, audioOn);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    // Nor is it stepping back.
    guard.poll (11.0, kWallOffset + 5.0, 10.9, audioOn);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    // Just over the line is.
    guard.poll (11.5, kWallOffset + 6.6, 10.9, audioOn);
    REQUIRE (guard.isWaitingForAudio());
}

TEST_CASE (CallbackWatchdogSleepGuard_ADarkWakeOfAnyLengthKeepsTheWatchdogHeld)
{
    // Power Nap, a Time Machine backup, iCloud: awake with the lid shut for
    // minutes, the app polling, the Mac's audio off. The minute's wait used to
    // run out a minute in, and five seconds later every device was dead.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;

    guard.poll (500.0, kWallOffset + wall, 500.0, audioOn);
    wall += 3600.0;

    double t = 500.5;
    pollAwake (guard, t, wall, 0.5, 500.0, audioOff);
    REQUIRE (guard.isWaitingForAudio());

    for (int second = 0; second < 600; ++second)
    {
        t += 1.0;
        pollAwake (guard, t, wall, 1.0, 500.0, audioOff);
        REQUIRE (guard.isWaitingForAudio());
        REQUIRE (guard.silenceBegan (500.0, 400.0) == 0.0);
    }

    // Properly awake, audio on, and a device running again: judged from now.
    t += 1.0;
    pollAwake (guard, t, wall, 1.0, t - 0.2);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (500.0, 400.0) == t);
}

TEST_CASE (CallbackWatchdogSleepGuard_TheWaitRunsOnlyWhileTheMacsAudioIsOn)
{
    // A rig that never comes back is still reported: a minute after the Mac's
    // audio is back on, however long the dark wake before it.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;
    constexpr double kWait = CallbackWatchdogSleepGuard::kMaxWaitForAudioSeconds;

    guard.poll (50.0, kWallOffset + wall, 49.0, audioOn);
    wall += 600.0;

    double t = 50.5;
    pollAwake (guard, t, wall, 0.5, 49.0, audioOff);

    for (int second = 0; second < 300; ++second)
    {
        t += 1.0;
        pollAwake (guard, t, wall, 1.0, 49.0, audioOff);
    }

    REQUIRE (guard.isWaitingForAudio());

    // Fully awake; the last poll with the audio off is where the minute starts.
    const double audioBack = t;

    for (double waited = 1.0; waited < kWait; waited += 1.0)
    {
        pollAwake (guard, audioBack + waited, wall, 1.0, 49.0);
        REQUIRE (guard.isWaitingForAudio());
    }

    pollAwake (guard, audioBack + kWait, wall, 1.0, 49.0);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (49.0, 10.0) == audioBack + kWait);
}

TEST_CASE (CallbackWatchdogSleepGuard_AudioOffAgainStartsTheMinuteAgain)
{
    // Awake with audio on for most of the minute, then a dark wake: the minute
    // is not carried over -- the Mac is not settled until its audio has been
    // on, and silent, for a whole one.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;
    constexpr double kWait = CallbackWatchdogSleepGuard::kMaxWaitForAudioSeconds;

    guard.poll (10.0, kWallOffset + wall, 10.0, audioOn);
    wall += 600.0;

    double t = 10.5;
    pollAwake (guard, t, wall, 0.5, 10.0);
    REQUIRE (guard.isWaitingForAudio());

    t += kWait - 5.0;
    pollAwake (guard, t, wall, kWait - 5.0, 10.0);
    REQUIRE (guard.isWaitingForAudio());

    t += 1.0;
    pollAwake (guard, t, wall, 1.0, 10.0, audioOff);
    REQUIRE (guard.isWaitingForAudio());

    t += 10.0;
    pollAwake (guard, t, wall, 10.0, 10.0);
    REQUIRE (guard.isWaitingForAudio());
}

TEST_CASE (CallbackWatchdogSleepGuard_TheMacTurningItsAudioOffHoldsTheWatchdogBeforeAnySleep)
{
    // The lid closes, the HAL stops IO, and the Mac takes longer than the
    // watchdog's few seconds to actually go to sleep. No sleep has been seen;
    // the Mac says its audio is off.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;

    guard.poll (100.0, kWallOffset + wall, 100.0, audioOn);

    double t = 100.0;
    for (int i = 0; i < 12; ++i)
    {
        t += 0.5;
        pollAwake (guard, t, wall, 0.5, 100.0, audioOff);
        REQUIRE_FALSE (guard.isWaitingForAudio()); // not asked: nothing held yet
    }

    // Six seconds silent: the backend, about to call a device dead, asks.
    guard.holdWhileSystemAudioIsOff (t);
    REQUIRE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (100.0, 50.0) == 0.0);

    t += 0.5;
    pollAwake (guard, t, wall, 0.5, 100.0, audioOff);
    REQUIRE (guard.isWaitingForAudio());

    // Then sleeps, and wakes; held until audio runs.
    wall += 3600.0;
    t += 0.5;
    pollAwake (guard, t, wall, 0.5, 100.0);
    REQUIRE (guard.isWaitingForAudio());

    t += 2.0;
    pollAwake (guard, t, wall, 2.0, t - 0.1);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE (guard.silenceBegan (100.0, 50.0) == t);
}

TEST_CASE (CallbackWatchdogSleepGuard_ThePowerManagerIsAskedOnlyWhileHeld)
{
    // Asking is an IPC, not a clock read: never on an ordinary poll.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;
    int asked = 0;
    const auto counting = [&asked]() noexcept { ++asked; return false; };

    guard.poll (10.0, kWallOffset + wall, 10.0, counting);

    for (double t = 10.5; t < 30.0; t += 0.5)
    {
        wall += 0.5;
        guard.poll (t, kWallOffset + wall, t - 0.1, counting);
    }

    REQUIRE (asked == 0);

    wall += 600.0 + 0.5;
    guard.poll (30.0, kWallOffset + wall, 29.9, counting);
    REQUIRE (guard.isWaitingForAudio());
    REQUIRE (asked == 1);

    // Audio back: resumed without asking, and not asked again after.
    wall += 0.5;
    guard.poll (30.5, kWallOffset + wall, 30.2, counting);
    REQUIRE_FALSE (guard.isWaitingForAudio());

    for (double t = 31.0; t < 40.0; t += 0.5)
    {
        wall += 0.5;
        guard.poll (t, kWallOffset + wall, t - 0.1, counting);
    }

    REQUIRE (asked == 1);
}

TEST_CASE (CallbackWatchdogSleepGuard_EachSleepLeavesOneNotice)
{
    // Holding the watchdog back must not make a sleep invisible: a take
    // running across it has a gap, and the notice is how its record hears.
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;

    guard.poll (100.0, kWallOffset + wall, 100.0, audioOn);

    double t = 100.0;
    for (int i = 0; i < 20; ++i)
    {
        t += 0.5;
        pollAwake (guard, t, wall, 0.5, t - 0.1);
    }

    REQUIRE_FALSE (guard.takeSleepNotice()); // awake and running: nothing to say

    wall += 3600.0;
    t += 0.5;
    pollAwake (guard, t, wall, 0.5, 109.9, audioOff);
    REQUIRE (guard.takeSleepNotice());
    REQUIRE_FALSE (guard.takeSleepNotice()); // once

    // A dark wake that goes back to sleep, twice, before the Mac is properly
    // awake: still the one sleep as far as the take is concerned.
    for (int sleeps = 0; sleeps < 2; ++sleeps)
    {
        wall += 1800.0;
        for (int i = 0; i < 20; ++i)
        {
            t += 0.5;
            pollAwake (guard, t, wall, 0.5, 109.9, audioOff);
            REQUIRE_FALSE (guard.takeSleepNotice());
        }
    }

    t += 0.5;
    pollAwake (guard, t, wall, 0.5, t - 0.1);
    REQUIRE_FALSE (guard.isWaitingForAudio());
    REQUIRE_FALSE (guard.takeSleepNotice()); // audio back is not another sleep

    // The next sleep is news again.
    wall += 600.0;
    t += 0.5;
    pollAwake (guard, t, wall, 0.5, t - 0.6);
    REQUIRE (guard.takeSleepNotice());
}

TEST_CASE (CallbackWatchdogSleepGuard_TheMacTurningItsAudioOffToSleepLeavesOneNotice)
{
    CallbackWatchdogSleepGuard guard;
    double wall = 0.0;

    guard.poll (10.0, kWallOffset + wall, 10.0, audioOff);
    REQUIRE_FALSE (guard.takeSleepNotice());

    guard.holdWhileSystemAudioIsOff (16.0);
    REQUIRE (guard.takeSleepNotice());

    // Asked again before it sleeps, and then the sleep itself: the same gap.
    guard.holdWhileSystemAudioIsOff (16.5);
    wall += 3600.0 + 7.0;
    guard.poll (17.0, kWallOffset + wall, 10.0, audioOn);
    REQUIRE (guard.isWaitingForAudio());
    REQUIRE_FALSE (guard.takeSleepNotice());
}
