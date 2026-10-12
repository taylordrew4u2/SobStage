# ctest -P script: §5.2/§7, the app's combined device is never destroyed under
# the take's headphone mix. Re-making it (a microphone plugged in or pulled, a
# rename, coreaudiod having removed it) waits for Stop while the take's output
# stream is open on it, and between takes our stream on it is closed first.
# Both are judged by the stream that is open, never by the output chosen:
# mid-take the choice can move off the combined device (picked in Settings, or
# a layout read that found no jacks) while the stream stays on it until Stop,
# and a re-make then destroyed it under the running take.
#
# The decision is combinedDeviceRemakeStep() (unit-tested) and the open stream
# is CaptureCoordinator::isOutputOpenOn() (unit-tested); Application cannot be
# compiled into a test, so this pins that it feeds one from the other and that
# publishAggregateDevice() and the "removed by macOS" notice both go by it.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

function(function_body signature out)
    string(FIND "${text}" "${signature}" start)
    if (start EQUAL -1)
        message(FATAL_ERROR "${signature} not found")
    endif()
    string(SUBSTRING "${text}" ${start} -1 rest)
    string(FIND "${rest}" "\n}\n" end)
    string(SUBSTRING "${rest}" 0 ${end} body)
    set(${out} "${body}" PARENT_SCOPE)
endfunction()

function(require_absent body what where)
    string(FIND "${body}" "${what}" at)
    if (NOT at EQUAL -1)
        message(FATAL_ERROR "${where} must not go by the output chosen ('${what}')")
    endif()
endfunction()

# The decision, from the stream that is open.
function_body("CombinedDeviceRemake Application::combinedDeviceRemake() const" decide)
foreach (required "combinedDeviceRemakeStep (" "capture->isOutputOpenOn (kOurAggregateUid)"
                  "capture->isRecording()")
    string(FIND "${decide}" "${required}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "combinedDeviceRemake() must decide from the open stream: '${required}' not found")
    endif()
endforeach()
require_absent("${decide}" "selectedOutputDeviceId" "combinedDeviceRemake()")
require_absent("${decide}" "isMonitoringThroughCombinedDevice" "combinedDeviceRemake()")

# The republish asks it before destroying anything, and acts on both answers.
function_body("bool Application::publishAggregateDevice()" publish)
string(FIND "${publish}" "systemAggregate->publish (" destroys)
if (destroys EQUAL -1)
    message(FATAL_ERROR "publishAggregateDevice() no longer publishes")
endif()
string(SUBSTRING "${publish}" 0 ${destroys} beforePublish)
foreach (required "const auto step = combinedDeviceRemake()"
                  "step == CombinedDeviceRemake::AfterTheTake"
                  "aggregateRepublishDeferred = true"
                  "step == CombinedDeviceRemake::CloseOurStreamFirst"
                  "capture->stopMonitoring()")
    string(FIND "${beforePublish}" "${required}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "publishAggregateDevice() must settle what plays through the device before publish(): '${required}' not found")
    endif()
endforeach()
require_absent("${publish}" "isMonitoringThroughCombinedDevice" "publishAggregateDevice()")
require_absent("${publish}" "selectedOutputDeviceId" "publishAggregateDevice()")

# The notice that macOS removed it says "when this take stops" by the same
# test the republish waits on.
string(FIND "${text}" "systemAggregate->forgetIfLost()" lost)
if (lost EQUAL -1)
    message(FATAL_ERROR "the combined device's forgetIfLost() check was not found")
endif()
string(SUBSTRING "${text}" ${lost} 1500 lostBlock)
string(FIND "${lostBlock}" "const bool waitsForStop = combinedDeviceRemake() == CombinedDeviceRemake::AfterTheTake" waits)
string(FIND "${lostBlock}" "made again when this take stops." said)
if (waits EQUAL -1 OR said EQUAL -1 OR NOT waits LESS said)
    message(FATAL_ERROR "the removed-device notice must say it waits for Stop by combinedDeviceRemake()")
endif()

message(STATUS "The combined device is re-made by what plays through it, not by the output chosen")
