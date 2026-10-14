# ctest -P script: the mid-take write protections (90% mix-only fallback and
# the backup's 1 GB low-space stop) must run on every status poll. They used
# to sit beneath warning branches that return whenever a counter rose -- the
# stream-failure, backend-drop, layout-miss and output-glitch lines -- and on
# a loaded Mac one of those rises in nearly every poll, so neither ever ran.
# Pin the order in Application::pollStatusAdvice so it cannot drift back.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

string(FIND "${text}" "juce::String Application::pollStatusAdvice (double sinceLastCallSeconds)" start)
if (start EQUAL -1)
    message(FATAL_ERROR "pollStatusAdvice() not found")
endif()
string(SUBSTRING "${text}" ${start} -1 body)
string(FIND "${body}" "\n}\n" end)
string(SUBSTRING "${body}" 0 ${end} body)

set(failed FALSE)

string(FIND "${body}" "decideWriteSafetyActions (" safety)
string(FIND "${body}" "capture->fallBackToMixOnly()" fallback)
string(FIND "${body}" "capture->stopMirroring()" stopMirror)
if (safety EQUAL -1 OR fallback EQUAL -1 OR stopMirror EQUAL -1)
    message(SEND_ERROR "pollStatusAdvice() does not apply decideWriteSafetyActions()")
    set(failed TRUE)
else()
    foreach (marker
             "audioBackend->takeStreamFailures()"
             "return firstWarning;"
             "getFramesDroppedByBackend()"
             "getFramesMissedByLayout()"
             "getOutputGlitchCount()"
             "recordStartProblem.isNotEmpty()"
             "! mirrorMissingReported")
        string(FIND "${body}" "${marker}" at)
        if (at EQUAL -1)
            message(SEND_ERROR "pollStatusAdvice() no longer contains '${marker}'; update this check")
            set(failed TRUE)
        elseif (NOT (safety LESS at AND fallback LESS at AND stopMirror LESS at))
            message(SEND_ERROR "write safety actions must run before '${marker}'")
            set(failed TRUE)
        endif()
    endforeach()

    # Still after the two stops that end the take outright. The card-removal
    # stop's reason is worded for the take's route (the card, or this
    # computer's drive for a take recorded here).
    string(FIND "${body}" "stoppedAcceptingWritesReason (" cardStop)
    string(FIND "${body}" "the drive ran out of room" roomStop)
    if (cardStop EQUAL -1 OR roomStop EQUAL -1
        OR NOT (cardStop LESS safety AND roomStop LESS safety))
        message(SEND_ERROR "write safety actions must follow the card-removal and full-drive stops")
        set(failed TRUE)
    endif()
endif()

if (failed)
    message(FATAL_ERROR "Write safety order check failed")
endif()
message(STATUS "Write safety actions run before every warning branch")
