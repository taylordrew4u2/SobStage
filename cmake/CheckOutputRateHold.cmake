# ctest -P script: §5.3, the headphone output held off the clock on the
# platform's word of a rate change (IAudioBackend::isOutputOffRate), before the
# app's poll has read the report. The coordinator's side is unit-tested
# (CaptureCoordinator, TakeWatchdog) and the backend's in sim_coreaudio;
# Application cannot be compiled into a test, so this pins the three places it
# has to know about the hold:
#
#  - the take's health counts it as the output's rate change, so the watchdog
#    does not call the output unplugged or taken, nor the rings' loss over the
#    hand-over the computer's;
#  - a device-list rescan in that moment does not take the held output for a
#    lost one and reopen every stream before the poll has decided;
#  - the buffer ladder does not count what the rings lose over the hand-over,
#    from the poll that first sees the hold (before takeStreamFailures() can
#    let it go).
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

string(FIND "${text}" "health.outputRateChanged = capture->isOutputAbandoned() || capture->isOutputOffRate();" health)
if (health EQUAL -1)
    message(FATAL_ERROR "snapshotTakeHealth() must count an output held for a rate change as outputRateChanged")
endif()

# The rescan's "nothing changed, nothing lost" guard.
string(FIND "${text}" "void Application::rescanDevices()" rescan)
if (rescan EQUAL -1)
    message(FATAL_ERROR "rescanDevices() not found")
endif()
string(SUBSTRING "${text}" ${rescan} -1 rescanBody)
string(FIND "${rescanBody}" "&& describeRig() == lastStartedRigSignature)" guardEnd)
if (guardEnd EQUAL -1)
    message(FATAL_ERROR "the rescan's unchanged-rig guard was not found")
endif()
math(EXPR guardStart "${guardEnd} - 500")
string(SUBSTRING "${rescanBody}" ${guardStart} 500 guard)
string(FIND "${guard}" "capture->isOutputOffRate()" heldInGuard)
if (heldInGuard EQUAL -1)
    message(FATAL_ERROR "the rescan must not reopen the rig for an output held off the clock for a rate change")
endif()

# The ladder's grace, armed before the batch is taken.
string(FIND "${text}" "juce::String Application::pollStatusAdvice (double sinceLastCallSeconds)" poll)
if (poll EQUAL -1)
    message(FATAL_ERROR "pollStatusAdvice() not found")
endif()
string(SUBSTRING "${text}" ${poll} -1 pollBody)
string(FIND "${pollBody}" "const auto failures = audioBackend->takeStreamFailures();" taken)
if (taken EQUAL -1)
    message(FATAL_ERROR "the stream-failure batch was not found")
endif()
string(SUBSTRING "${pollBody}" 0 ${taken} beforeTaken)
foreach (required "capture->isOutputOffRate()" "outputRateHoldGraceUntilMs =")
    string(FIND "${beforeTaken}" "${required}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "pollStatusAdvice() must see the hold before takeStreamFailures(): '${required}' not found")
    endif()
endforeach()

string(FIND "${pollBody}" "eventsNow > reportedRingLossEvents && ! handingOverForRate" ladder)
if (ladder EQUAL -1)
    message(FATAL_ERROR "the buffer ladder must not count the rings' losses over a rate-change hand-over")
endif()
string(FIND "${pollBody}" "juce::Time::getMillisecondCounterHiRes() < outputRateHoldGraceUntilMs" grace)
if (grace EQUAL -1 OR NOT grace LESS ladder)
    message(FATAL_ERROR "the ladder's hand-over grace must be read from outputRateHoldGraceUntilMs")
endif()

message(STATUS "An output held off the clock for a rate change is neither lost nor the computer's fault")
