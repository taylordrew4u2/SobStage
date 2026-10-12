# ctest -P script: §6.6, a Mac that sleeps mid-take leaves a gap in the take,
# and the take's record has to say where. The backend reports the sleep once
# (StreamFailureKind::systemSlept, checked by sim_coreaudio and
# sim_capture_mac) and the policy is unit-tested (systemSleptMarksTheTake);
# Application cannot be compiled into a test, so this pins that its stream-
# failure loop turns the report into a session.json dropout line and an
# activity line -- ahead of the path that marks microphones dead -- and does
# nothing else with it: no microphone silenced, nothing reopened.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

# The batch is taken whole and then walked (a rate-change stop waits for the
# end of it; see CheckRateChangeStopAfterBatch.cmake).
string(FIND "${text}" "const auto failures = audioBackend->takeStreamFailures();" taken)
if (taken EQUAL -1)
    message(FATAL_ERROR "the stream-failure batch was not found")
endif()
string(SUBSTRING "${text}" ${taken} -1 text)

string(FIND "${text}" "for (const auto& failure : failures)" loop)
if (loop EQUAL -1)
    message(FATAL_ERROR "the stream-failure loop was not found")
endif()
string(SUBSTRING "${text}" ${loop} -1 body)

string(FIND "${body}" "if (failure.kind == StreamFailureKind::systemSlept)" slept)
if (slept EQUAL -1)
    message(FATAL_ERROR "the stream-failure loop does not handle StreamFailureKind::systemSlept")
endif()

foreach (laterPath "deadInputStreams.insert" "streamFailureIsWarning (failure.kind)"
                   "failure.kind == StreamFailureKind::resumed")
    string(FIND "${body}" "${laterPath}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "'${laterPath}' not found in the stream-failure loop")
    endif()
    if (NOT slept LESS at)
        message(FATAL_ERROR "systemSlept must be handled before '${laterPath}'")
    endif()
endforeach()

# The branch: from its test to the first `continue;` after it.
string(SUBSTRING "${body}" ${slept} -1 branch)
string(FIND "${branch}" "continue;" end)
if (end EQUAL -1)
    message(FATAL_ERROR "the systemSlept branch does not end in continue")
endif()
string(SUBSTRING "${branch}" 0 ${end} branch)

foreach (required "systemSleptMarksTheTake (failure.kind, isRecording())"
                  "midTakeDropouts.push_back" "kSystemSleptTakeNote"
                  "noteActivity (ActivityLevel::Warning" "kSystemSleptActivityLine")
    string(FIND "${branch}" "${required}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "the systemSlept branch no longer has '${required}'")
    endif()
endforeach()

foreach (forbidden "setChannelLive" "deadInputStreams" "restartCapture" "reopenStreams"
                   "captureRestartDeferred" "noteDeviceDropout" "onMicUnplugged")
    string(FIND "${branch}" "${forbidden}" at)
    if (NOT at EQUAL -1)
        message(FATAL_ERROR "the Mac sleeping is no device's failure, but its branch has '${forbidden}'")
    endif()
endforeach()

message(STATUS "A Mac that sleeps mid-take is in the take's record, and no microphone is called dead for it")
