# ctest -P script: §5.2, the per-jack headphone gains on the combined device.
# They are by output channel number, and the combined device numbers its
# channels over the sub-devices active in it now, so a microphone leaving it
# mid-take or joining it late moves every later jack. Which stream they reach
# is CaptureCoordinator::setHeadphoneJackGains()'s to decide, from the stream
# actually open (unit-tested), and HeadphoneJacks lays them on every layout
# read (unit-tested). Application cannot be compiled into a test, so this pins
# that it uses both:
#
#  - the output choice reads the combined device's layout only through
#    headphoneJacks.layoutRead(), which lays the gains;
#  - the gains are handed to the coordinator as the combined device's own, not
#    tagged with the output chosen (mid-take the choice can move while the
#    stream stays on the combined device until Stop);
#  - a coordinator re-made for a new rate is given them before it opens, and
#    switching someone's headphones on or off lays them at once.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
#   -DAPPLICATION_H=<Source/App/Application.h>
foreach (var APPLICATION_CPP APPLICATION_H)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Pass -D${var}=...")
    endif()
endforeach()

file(READ "${APPLICATION_CPP}" text)
file(READ "${APPLICATION_H}" header)

# The body of one Application member function, from its signature to the
# first closing brace at column 0.
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

# The layout is read in one place, and that read lays the gains.
function_body("void Application::reselectOutputDevice()" reselect)
string(FIND "${reselect}" "headphoneJacks.layoutRead (" read)
if (read EQUAL -1)
    message(FATAL_ERROR "reselectOutputDevice() must read the combined device's layout through headphoneJacks.layoutRead()")
endif()
string(FIND "${reselect}" "combinedDeviceHasHeadphones (headphoneJacks.getLayout())" offered)
if (offered EQUAL -1 OR NOT read LESS offered)
    message(FATAL_ERROR "the combined device must be offered from the layout headphoneJacks.layoutRead() just stored")
endif()
string(FIND "${reselect}" "systemAggregate->getOutputLayout()" readInReselect)
if (readInReselect EQUAL -1)
    message(FATAL_ERROR "reselectOutputDevice() no longer reads the combined device's layout")
endif()

# Handed on as the combined device's own; the coordinator alone says which
# stream that is.
string(FIND "${header}" "HeadphoneJacks headphoneJacks {" member)
if (member EQUAL -1)
    message(FATAL_ERROR "Application.h must hold the combined device's jacks in a HeadphoneJacks")
endif()
string(SUBSTRING "${header}" ${member} 700 memberInit)
string(FIND "${memberInit}" "kOurAggregateUid," combinedId)
string(FIND "${memberInit}" "capture->setHeadphoneJackGains (outputDeviceId, gains)" handedOn)
if (combinedId EQUAL -1 OR handedOn EQUAL -1)
    message(FATAL_ERROR "headphoneJacks must hand its gains to the coordinator as the combined device's (kOurAggregateUid)")
endif()
string(FIND "${text}" "setHeadphoneJackGains" direct)
if (NOT direct EQUAL -1)
    message(FATAL_ERROR "Application.cpp must lay jack gains only through headphoneJacks, never naming a device itself")
endif()

# A coordinator made fresh starts with the mix on every channel.
function_body("void Application::restartCapture()" restart)
string(FIND "${restart}" "capture = std::make_unique<CaptureCoordinator>" remade)
string(FIND "${restart}" "capture->startMonitoring (channels, selectedOutputDeviceId)" opened)
if (remade EQUAL -1 OR opened EQUAL -1)
    message(FATAL_ERROR "restartCapture()'s re-made coordinator or its open was not found")
endif()
math(EXPR between "${opened} - ${remade}")
string(SUBSTRING "${restart}" ${remade} ${between} remadeToOpen)
string(FIND "${remadeToOpen}" "headphoneJacks.lay()" relaid)
if (relaid EQUAL -1)
    message(FATAL_ERROR "a coordinator re-made for a new rate must be given the jack gains before it opens")
endif()

function_body("void Application::setHeadphonesOn (const juce::String& deviceKey, bool on)" switched)
string(FIND "${switched}" "headphoneJacks.lay()" switchLays)
if (switchLays EQUAL -1)
    message(FATAL_ERROR "switching someone's headphones on or off must lay the jack gains at once")
endif()

message(STATUS "The combined device's jack gains follow every layout read, from the stream that is open")
