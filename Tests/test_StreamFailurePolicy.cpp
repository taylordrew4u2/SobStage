#include "TestFramework.h"
#include "Platform/IAudioBackend.h"

using namespace mma;

TEST_CASE (StreamFailurePolicy_OnlyALiveRateChangeStopsTheTake)
{
    REQUIRE (streamFailureRequiresRecordingStop (StreamFailureKind::sampleRateChanged));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::deviceUnavailable));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::processorOverload));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::safetyMonitoringUnavailable));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::unknown));
}

TEST_CASE (StreamFailurePolicy_RecoverableSafetyReportsAreWarnings)
{
    REQUIRE (streamFailureIsWarning (StreamFailureKind::processorOverload));
    REQUIRE (streamFailureIsWarning (StreamFailureKind::safetyMonitoringUnavailable));
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::sampleRateChanged));
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::deviceUnavailable));
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::unknown));
}

TEST_CASE (StreamFailurePolicy_ADeadInputStreamIsReopenedOnceThenLeftDead)
{
    // Idle: reopen now, or the next take records silence for that mic.
    REQUIRE (deadInputStreamAction (false, false) == DeadInputStreamAction::reopenNow);

    // Mid-take the channels are fixed; Stop owes the reopen.
    REQUIRE (deadInputStreamAction (true, false) == DeadInputStreamAction::reopenAtStop);

    // Died again straight after a reopen: no restart loop, it stays dead.
    REQUIRE (deadInputStreamAction (false, true) == DeadInputStreamAction::leaveDead);
    REQUIRE (deadInputStreamAction (true, true) == DeadInputStreamAction::leaveDead);
}

TEST_CASE (StreamFailurePolicy_AResumedStreamIsNeitherAWarningNorAStop)
{
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::resumed));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::resumed));
}

TEST_CASE (StreamFailurePolicy_AStreamResumesOnlyOnACallbackAfterItWasCalledDead)
{
    // Never called dead: nothing to take back.
    REQUIRE_FALSE (inputStreamHasResumed (0.0, 100.0, false));
    // Called dead at 50 s, last callback before that: still dead.
    REQUIRE_FALSE (inputStreamHasResumed (50.0, 44.0, false));
    REQUIRE_FALSE (inputStreamHasResumed (50.0, 50.0, false));
    // A callback after the report: it is delivering again.
    REQUIRE (inputStreamHasResumed (50.0, 50.5, false));
    // A device the OS says is gone is not revived by a stray callback.
    REQUIRE_FALSE (inputStreamHasResumed (50.0, 50.5, true));
}

TEST_CASE (StreamFailurePolicy_OnlyTheOutputsOwnRateChangeIsAnOutputRateChange)
{
    // The backend names no device for the headphone output.
    REQUIRE (streamFailureIsOutputRateChange ({ "", "changed to 44.1 kHz", "Your sound output",
                                                StreamFailureKind::sampleRateChanged }));

    // A microphone's rate change is the take's own format changing.
    REQUIRE_FALSE (streamFailureIsOutputRateChange ({ "uid-mic", "changed to 44.1 kHz", {},
                                                      StreamFailureKind::sampleRateChanged }));

    // Other news about the output is not a rate change.
    REQUIRE_FALSE (streamFailureIsOutputRateChange ({ "", "is no longer available", "Your sound output",
                                                      StreamFailureKind::deviceUnavailable }));
    REQUIRE_FALSE (streamFailureIsOutputRateChange ({ "", "is back at 48 kHz.", "Your sound output",
                                                      StreamFailureKind::resumed }));
}

TEST_CASE (StreamFailurePolicy_AnOutputRateChangeMidTakeDoesNotStopATakeItIsNotPartOf)
{
    // Headphones on an interface whose inputs are switched off, switched to
    // 44.1 kHz by another app mid-take: every microphone file is still true,
    // so the take carries on without the output as its clock.
    REQUIRE (outputRateChangeAction (true, false, false) == OutputRateChangeAction::carryOnWithoutIt);
    REQUIRE (outputRateChangeAction (true, false, true) == OutputRateChangeAction::carryOnWithoutIt);

    // The combined device (its rate is the microphones') or a mixer that is
    // both: the take's own format changed, so it stops.
    REQUIRE (outputRateChangeAction (true, true, false) == OutputRateChangeAction::stopTake);
    REQUIRE (outputRateChangeAction (true, true, true) == OutputRateChangeAction::stopTake);
}

TEST_CASE (StreamFailurePolicy_AnOutputRateChangeBetweenTakesIsPutBackOnceThenSetAside)
{
    // Between takes the rig reopens at its own rate, which puts the output
    // back -- rather than running on with every ring overflowing until the
    // buffer ladder blames the computer.
    REQUIRE (outputRateChangeAction (false, false, false) == OutputRateChangeAction::reopenNow);
    REQUIRE (outputRateChangeAction (false, true, false) == OutputRateChangeAction::reopenNow);

    // Switched again straight after: no tug-of-war with whatever keeps
    // switching it.
    REQUIRE (outputRateChangeAction (false, false, true) == OutputRateChangeAction::setAside);
    REQUIRE (outputRateChangeAction (false, true, true) == OutputRateChangeAction::setAside);
}
