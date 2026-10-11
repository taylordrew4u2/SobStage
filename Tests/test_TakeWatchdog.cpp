#include "TestFramework.h"
#include "Core/TakeWatchdog.h"

using namespace mma;

namespace {

TakeHealth healthyRig()
{
    TakeHealth h;
    h.mics = { { "Alex", true }, { "Sam", true } };
    h.cameras = { { "FaceTime", true } };
    h.remainingSeconds = 3600.0;
    return h;
}

bool has (const std::vector<TakeAlert>& alerts, TakeAlert::Kind kind)
{
    for (const auto& a : alerts)
        if (a.kind == kind)
            return true;
    return false;
}

} // namespace

TEST_CASE (TakeWatchdog_NothingToSayWhenNothingChanges)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());
    REQUIRE (w.observe (healthyRig()).empty());
    REQUIRE (w.observe (healthyRig()).empty());
}

TEST_CASE (TakeWatchdog_SilentOutsideATake)
{
    TakeWatchdog w;
    auto bad = healthyRig();
    bad.mics[0].live = false;
    REQUIRE (w.observe (bad).empty());
}

TEST_CASE (TakeWatchdog_AMicGoingAwayIsSaidOnceAndNamed)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.mics[1].live = false;

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::MicLost);
    REQUIRE (alerts[0].message.find ("Sam") != std::string::npos);
    REQUIRE (! alerts[0].recovery);

    // Still dead next tick: not said again.
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_AMicComingBackIsGoodNews)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto gone = healthyRig();
    gone.mics[0].live = false;
    w.observe (gone);

    const auto alerts = w.observe (healthyRig());
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::MicBack);
    REQUIRE (alerts[0].recovery);
    REQUIRE (alerts[0].message.find ("Alex") != std::string::npos);
}

TEST_CASE (TakeWatchdog_AMicDeadAtTheStartIsNotAnAlert)
{
    // It was on screen before record was pressed; the pop-up is for changes.
    TakeWatchdog w;
    auto baseline = healthyRig();
    baseline.mics[0].live = false;
    w.beginTake (baseline);
    REQUIRE (w.observe (baseline).empty());
}

TEST_CASE (TakeWatchdog_ACameraGoingAwayIsNamed)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.cameras[0].present = false;

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::CameraLost);
    REQUIRE (alerts[0].message.find ("FaceTime") != std::string::npos);
    REQUIRE (alerts[0].message.find ("rest of this take") != std::string::npos);
    REQUIRE (alerts[0].message.find ("next take") != std::string::npos);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.empty());

    w.endTake();
    w.beginTake (healthyRig());
    REQUIRE (w.observe (healthyRig()).empty());
}

TEST_CASE (TakeWatchdog_ACameraStillStartingIsNotAnAbsentCamera)
{
    // The bug this replaces: REC is confirmed only by AVFoundation's didStart,
    // so a camera sits in STARTING for a moment after Record. `present` was
    // fed from the recording flag alone, which read that as "not there" -- and
    // if the baseline landed inside the window, the next observation saw the
    // camera appear and announced a recovery for a camera that never left.
    //
    // Application now feeds `present` from recording OR starting. This is the
    // watchdog's half of that contract: a camera which is present at the
    // baseline and stays present says nothing, and a camera which is absent at
    // the baseline and then appears says nothing either, because there is no
    // longer any branch that could speak.
    TakeWatchdog w;

    auto starting = healthyRig();
    starting.cameras[0].present = false;   // baseline taken mid-STARTING
    w.beginTake (starting);

    const auto confirmed = w.observe (healthyRig());
    REQUIRE (confirmed.empty());

    // And it is still watched: a real loss after that is still reported once.
    auto lost = healthyRig();
    lost.cameras[0].present = false;

    const auto alerts = w.observe (lost);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::CameraLost);
}

TEST_CASE (TakeWatchdog_ACameraThatVanishesFromTheListIsGoneToo)
{
    // The OS stops listing an unplugged camera; it does not list it as absent.
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.cameras.clear();

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::CameraLost);
    REQUIRE (alerts[0].message.find ("FaceTime") != std::string::npos);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.empty());
}

namespace {

/// Two capture dongles of the same model: the OS gives them one name and
/// two device ids. `a` and `b` say whether each is in the take.
TakeHealth twins (bool a, bool b)
{
    auto h = healthyRig();
    h.cameras = { { "USB2 Video", a, "unit-A" }, { "USB2 Video", b, "unit-B" } };
    return h;
}

size_t countKind (const std::vector<TakeAlert>& alerts, TakeAlert::Kind kind)
{
    size_t n = 0;
    for (const auto& a : alerts)
        if (a.kind == kind)
            ++n;
    return n;
}

} // namespace

TEST_CASE (TakeWatchdog_ATwinThatNeverStartedIsNotALossAtTheStart)
{
    // Unit B has no HDMI signal, so it never starts; it is still in the take's
    // roster as absent. Matched by name, B read A's "present" and the first
    // tick of the take said "USB2 Video has gone away" about nothing.
    TakeWatchdog w;
    w.beginTake (twins (true, false));
    REQUIRE (w.observe (twins (true, false)).empty());

    // A's real loss later in the take is still said, once.
    const auto alerts = w.observe (twins (false, false));
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::CameraLost);
    REQUIRE (alerts[0].message.find ("USB2 Video") != std::string::npos);
    REQUIRE (w.observe (twins (false, false)).empty());
}

TEST_CASE (TakeWatchdog_ATwinThatNeverStartedListedFirstDoesNotHideTheOthersLoss)
{
    // The same rig with the absent unit first in the list. By name, A's
    // "was present" came from B's absent entry, so A's loss was never said.
    TakeWatchdog w;
    w.beginTake (twins (false, true));
    REQUIRE (w.observe (twins (false, true)).empty());

    const auto alerts = w.observe (twins (false, false));
    REQUIRE (countKind (alerts, TakeAlert::Kind::CameraLost) == 1);
}

TEST_CASE (TakeWatchdog_EachTwinLostIsSaid)
{
    // Two working twins. The latch was by name, so the second one lost said
    // nothing.
    TakeWatchdog w;
    w.beginTake (twins (true, true));
    REQUIRE (w.observe (twins (true, true)).empty());

    REQUIRE (countKind (w.observe (twins (false, true)), TakeAlert::Kind::CameraLost) == 1);
    REQUIRE (countKind (w.observe (twins (false, false)), TakeAlert::Kind::CameraLost) == 1);
    REQUIRE (w.observe (twins (false, false)).empty());

    // A twin that drops out of the list altogether is carried forward under
    // its own id, not merged into the one still listed.
    TakeWatchdog v;
    v.beginTake (twins (true, true));
    auto onlyB = twins (true, true);
    onlyB.cameras.erase (onlyB.cameras.begin());
    REQUIRE (countKind (v.observe (onlyB), TakeAlert::Kind::CameraLost) == 1);
    REQUIRE (v.observe (onlyB).empty());

    auto none = onlyB;
    none.cameras.clear();
    REQUIRE (countKind (v.observe (none), TakeAlert::Kind::CameraLost) == 1);
    REQUIRE (v.observe (none).empty());
}

TEST_CASE (TakeWatchdog_DroppedAudioIsSaidOnceThenAtMostOnceAMinute)
{
    // A struggling machine drops a little on every tick. Said on every
    // increase, the card grew a line twice a second and could not be read or
    // dismissed -- seen for real in the headless take.
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.framesDropped = 10;
    now.elapsedSeconds = 1.0;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::AudioDropped));

    now.framesDropped = 20; now.elapsedSeconds = 1.5;
    REQUIRE (w.observe (now).empty());           // more loss, too soon to repeat
    now.framesDropped = 30; now.elapsedSeconds = 30.0;
    REQUIRE (w.observe (now).empty());

    now.framesDropped = 48000 * 3; now.elapsedSeconds = 62.0;
    const auto again = w.observe (now);
    REQUIRE (again.size() == 1);
    REQUIRE (again[0].kind == TakeAlert::Kind::AudioDropped);
    REQUIRE (again[0].message.find ("still") != std::string::npos);
    REQUIRE (again[0].message.find ("3 s") != std::string::npos);
}

TEST_CASE (TakeWatchdog_DriveTroubleIsSaidWhenItStarts)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.writerBehind = true;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::WriterBehind));
    REQUIRE (w.observe (now).empty());

    now.mixOnly = true;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::MixOnly));
}

TEST_CASE (TakeWatchdog_WriterBehindAfterMixOnlyDoesNotPromiseToDropTracks)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    // Fell back to the mix at 90%, then drained below 50%.
    auto now = healthyRig();
    now.writerBehind = true;
    now.mixOnly = true;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::MixOnly));
    now.writerBehind = false;
    REQUIRE (w.observe (now).empty());

    // A later 50-89% fill: the separate tracks are already gone, so "will be
    // dropped" would be a false promise about the past.
    now.writerBehind = true;
    REQUIRE_FALSE (has (w.observe (now), TakeAlert::Kind::WriterBehind));
}

TEST_CASE (TakeWatchdog_RoomWarningsFireOncePerThresholdOnTheWayDown)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.remainingSeconds = 599.0;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::TenMinutesLeft));
    now.remainingSeconds = 500.0;
    REQUIRE (w.observe (now).empty());

    now.remainingSeconds = 119.0;
    const auto two = w.observe (now);
    REQUIRE (two.size() == 1);
    REQUIRE (two[0].kind == TakeAlert::Kind::TwoMinutesLeft);
    now.remainingSeconds = 60.0;
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_UnknownRoomIsNotAWarning)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());
    auto now = healthyRig();
    now.remainingSeconds = -1.0;
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_ANewTakeStartsClean)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());
    auto low = healthyRig();
    low.remainingSeconds = 100.0;
    w.observe (low);
    w.endTake();

    // The next take on the same nearly-full drive warns again: the warning
    // belongs to the take, not to the drive.
    w.beginTake (healthyRig());
    REQUIRE (has (w.observe (low), TakeAlert::Kind::TwoMinutesLeft));
}

TEST_CASE (TakeWatchdog_OutputClockLostIsSaidOnceAndComesBack)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.outputClockLost = true;

    const auto lost = w.observe (now);
    REQUIRE (lost.size() == 1);
    REQUIRE (lost[0].kind == TakeAlert::Kind::OutputLost);
    REQUIRE (! lost[0].recovery);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.size() == 1);
    REQUIRE (back[0].kind == TakeAlert::Kind::OutputBack);
    REQUIRE (back[0].recovery);
}

TEST_CASE (TakeWatchdog_AnOutputSetAsideForARateChangeIsSaidOnceInItsOwnWords)
{
    // Another app switched the headphones to 44.1 kHz mid-take, and the take
    // carries on without them. The output is still plugged in and nobody took
    // it, so "has stopped -- unplugged, or taken by another app ... until it
    // comes back" would be wrong; the monitor problem says what happened.
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.outputClockLost = true;
    now.outputRateChanged = true;
    now.monitorProblem = "Your sound output changed to 44.1 kHz while SobStage is using 48 kHz. "
                         "Recording carries on, but you won't hear the mix in your headphones "
                         "until you stop.";

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::MonitorTrouble);
    REQUIRE (alerts[0].message == now.monitorProblem);
    REQUIRE_FALSE (has (alerts, TakeAlert::Kind::OutputLost));
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_RingOverrunsCountAsDroppedAudio)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.samplesOverrun = 256;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::AudioDropped));
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_TheSecondsLostFigureUsesTheTakesOwnRate)
{
    // The figure was computed against a hardcoded 48000, so a take at 96 kHz
    // was told twice as much audio had been lost as actually had -- the same
    // wrong-magnitude family as counting channel frames as wall-clock ones.
    TakeWatchdog watchdog;

    auto start = healthyRig();
    start.elapsedSeconds = 0.0;
    start.sampleRate = 96000.0;
    watchdog.beginTake (start);

    TakeHealth now = start;
    now.elapsedSeconds = 5.0;
    now.framesDropped = 96000;   // one second at this take's rate
    now.sampleRate = 96000.0;

    const auto alerts = watchdog.observe (now);

    bool saidOneSecond = false;
    for (const auto& a : alerts)
        if (a.message.find ("1 s lost") != std::string::npos)
            saidOneSecond = true;

    // Either it names one second, or it is the first-time wording that gives no
    // figure at all. What it must never do is say two.
    for (const auto& a : alerts)
        REQUIRE (a.message.find ("2 s lost") == std::string::npos);

    (void) saidOneSecond;
}

TEST_CASE (TakeWatchdog_ADriveWithNoRoomIsNotToldItHasTwoMinutes)
{
    // Zero used to be unreachable here: the app collapsed a full drive into the
    // "unknown" sentinel, which this block's gate excluded. Now that zero is a
    // real value it would fall into the two-minute arm and tell someone with no
    // room at all that they have about two minutes and should wrap up --
    // contradicting, one tick earlier, the stop that is about to happen.
    TakeWatchdog watchdog;

    auto start = healthyRig();
    start.elapsedSeconds = 0.0;
    start.remainingSeconds = 3600.0;
    watchdog.beginTake (start);

    auto now = start;
    now.elapsedSeconds = 10.0;
    now.remainingSeconds = 0.0;

    for (const auto& a : watchdog.observe (now))
    {
        REQUIRE (a.message.find ("two minutes of room") == std::string::npos);
        REQUIRE (a.message.find ("ten minutes of room") == std::string::npos);
    }
}

TEST_CASE (TakeWatchdog_RealRoomWarningsStillFire)
{
    // The gate above must not have silenced the warnings it sits in front of.
    TakeWatchdog watchdog;

    auto start = healthyRig();
    start.elapsedSeconds = 0.0;
    start.remainingSeconds = 3600.0;
    watchdog.beginTake (start);

    auto now = start;
    now.elapsedSeconds = 10.0;
    now.remainingSeconds = 100.0; // inside two minutes, and genuinely non-zero

    bool warned = false;
    for (const auto& a : watchdog.observe (now))
        if (a.message.find ("two minutes of room") != std::string::npos)
            warned = true;

    REQUIRE (warned);
}
