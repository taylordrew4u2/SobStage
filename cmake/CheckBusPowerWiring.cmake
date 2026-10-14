# ctest -P script: §14.2's two inputs from the device list. The rules are
# BusPowerDetector's (unit-tested); this pins that Application feeds them.
#
#  - A drop is counted against the devices attached before it. Mid-take the
#    device list has already lost every mic that dropped with it, so
#    noteDeviceDropout() counts the take's own devices too.
#  - A device that leaves the list ends its failure, so one that is plugged
#    back in and fails again counts again: rescanDevices() tells the advisor
#    which devices left, before the list forgets them.
#  - Every refusal that could be power is counted on every rebuild, and a
#    wake from sleep is told to the advisor before the drops it causes.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

function (body_of signature out)
    string(FIND "${text}" "${signature}" start)
    if (start EQUAL -1)
        message(FATAL_ERROR "${signature} not found")
    endif()
    string(SUBSTRING "${text}" ${start} -1 rest)
    string(FIND "${rest}" "\n}\n" end)
    string(SUBSTRING "${rest}" 0 ${end} rest)
    set(${out} "${rest}" PARENT_SCOPE)
endfunction()

body_of ("void Application::noteDeviceDropout (const std::string& deviceId" dropout)

foreach (needed "devicesAttachedAtDrop (listedNow, takeDevices)" "capture->getChannels()")
    string(FIND "${dropout}" "${needed}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "noteDeviceDropout() no longer counts the devices attached before the drop ('${needed}')")
    endif()
endforeach()

body_of ("void Application::rescanDevices()" rescan)

string(FIND "${rescan}" "setupAdvisor.noteDeviceLeft (" left)
string(FIND "${rescan}" "deviceManager.syncToEnumeration (seen);" sync)
if (left EQUAL -1)
    message(FATAL_ERROR "rescanDevices() no longer tells the advisor which devices left the list")
endif()
if (sync EQUAL -1 OR NOT left LESS sync)
    message(FATAL_ERROR "the devices that left must be told before syncToEnumeration() forgets them")
endif()

# Every refusal that could be power is counted on every rebuild: not only one
# whose live and failed counts changed (another mic browning out where the
# last one opened again changes neither), and not only one where something
# opened (every mic behind a starved hub refusing is the case the advice is
# for). So the call comes after both the started and the not-started report,
# outside either.
body_of ("void Application::restartCapture()" restart)

string(FIND "${restart}" "noteDeviceDropout (failure.deviceId" counted)
string(FIND "${restart}" "else if (worthSaying)" notStarted)
string(FIND "${restart}" "if (failedCount > 0 && worthSaying)" worthALine)
if (counted EQUAL -1)
    message(FATAL_ERROR "restartCapture() no longer counts a refusal toward the power check")
endif()
if (notStarted EQUAL -1 OR worthALine EQUAL -1)
    message(FATAL_ERROR "restartCapture()'s reports are not where this check expects them")
endif()
if (NOT counted GREATER notStarted)
    message(FATAL_ERROR "a refusal is counted toward the power check only when the rebuild is worth a line in the log, or only when something opened")
endif()

# A wake from sleep is noticed on the status tick before any stream failure
# that tick reads, and the advisor told: the drops of waking are not power.
body_of ("juce::String Application::pollStatusAdvice (double sinceLastCallSeconds)" poll)

string(FIND "${poll}" "wakeDetector.justWoke" woke)
string(FIND "${poll}" "setupAdvisor.noteSystemWoke (" told)
string(FIND "${poll}" "noteDeviceDropout (" dropped)
if (woke EQUAL -1 OR told EQUAL -1)
    message(FATAL_ERROR "pollStatusAdvice() no longer tells the advisor the computer woke from sleep")
endif()
if (NOT dropped EQUAL -1 AND NOT told LESS dropped)
    message(FATAL_ERROR "the wake must be told before this tick's stream failures are counted")
endif()

message(STATUS "Drops are counted against the devices before them, and a device that left fails afresh")
