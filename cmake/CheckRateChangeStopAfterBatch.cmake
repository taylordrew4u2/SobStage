# ctest -P script: §2.2/§5.3, a take stopped for a rate change is stopped once,
# after the whole poll's batch of stream reports has been read. Stopped inside
# the loop, Stop reopened the rig there and then (it owes a reopen), and the
# rest of the batch landed on streams just opened at the right rate: one rate
# change on the combined device is its output's report and each microphone's,
# and each microphone was then called dead and the rig reopened a second time.
# The policy is unit-tested (microphoneRateChangeStopsTheTake,
# outputRateChangeAction); Application cannot be compiled into a test, so this
# pins that pollStatusAdvice() asks it before the loop, stops nowhere inside
# the loop, and stops once after it without reopening a second time.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

string(FIND "${text}" "const auto failures = audioBackend->takeStreamFailures();" taken)
if (taken EQUAL -1)
    message(FATAL_ERROR "pollStatusAdvice() must take the stream-failure batch whole before walking it")
endif()
string(SUBSTRING "${text}" ${taken} -1 body)

string(FIND "${body}" "for (const auto& failure : failures)" loop)
if (loop EQUAL -1)
    message(FATAL_ERROR "the stream-failure loop was not found")
endif()

# Asked of the whole batch before the loop, so a report read before a
# microphone's knows the take is ending.
string(SUBSTRING "${body}" 0 ${loop} beforeLoop)
string(FIND "${beforeLoop}" "microphoneRateChangeStopsTheTake (failures, isRecording())" asked)
if (asked EQUAL -1)
    message(FATAL_ERROR "the batch must be asked whether a microphone's rate change stops the take before the loop")
endif()

string(FIND "${body}" "if (stopTakeForRateChange && isRecording())" stop)
if (stop EQUAL -1)
    message(FATAL_ERROR "the rate-change stop after the loop was not found")
endif()
if (NOT loop LESS stop)
    message(FATAL_ERROR "the rate-change stop must come after the stream-failure loop")
endif()

# Nothing inside the loop stops the take.
math(EXPR loopLength "${stop} - ${loop}")
string(SUBSTRING "${body}" ${loop} ${loopLength} insideLoop)
string(FIND "${insideLoop}" "toggleRecording()" stopInLoop)
if (NOT stopInLoop EQUAL -1)
    message(FATAL_ERROR "the stream-failure loop must not stop the take; record it and stop after the loop")
endif()

# The output's report hears that the take is ending.
string(FIND "${insideLoop}" "outputRateChangeAction (" decide)
if (decide EQUAL -1)
    message(FATAL_ERROR "the output's rate change is no longer decided by outputRateChangeAction()")
endif()
string(SUBSTRING "${insideLoop}" ${decide} 400 decision)
string(FIND "${decision}" "microphoneStopsTheTake" told)
if (told EQUAL -1)
    message(FATAL_ERROR "outputRateChangeAction() must be told when a microphone's change stops the take")
endif()

# Stopped once after the loop, and the idle reopen is left to the stop's own.
string(SUBSTRING "${body}" ${stop} 700 afterLoop)
foreach (required "toggleRecording();" "reopenStreams && ! stoppedTake")
    string(FIND "${afterLoop}" "${required}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "after the stream-failure loop: '${required}' not found")
    endif()
endforeach()

message(STATUS "A rate-change stop waits for the whole batch of reports and happens once")
