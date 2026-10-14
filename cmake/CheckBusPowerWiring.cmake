# ctest -P script: §14.2's two inputs from the device list. The rules are
# BusPowerDetector's (unit-tested); this pins that Application feeds them.
#
#  - A drop is counted against the devices attached before it. Mid-take the
#    device list has already lost every mic that dropped with it, so
#    noteDeviceDropout() counts the take's own devices too.
#  - A device that leaves the list ends its failure, so one that is plugged
#    back in and fails again counts again: rescanDevices() tells the advisor
#    which devices left, before the list forgets them.
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

message(STATUS "Drops are counted against the devices before them, and a device that left fails afresh")
