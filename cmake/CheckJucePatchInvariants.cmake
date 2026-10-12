# ctest -P script: static guard for the pinned JUCE camera patch.
#
# The macOS camera backend (juce_CameraDevice_mac.h, Objective-C++) cannot be
# compiled on non-Apple CI, so this only pins the text of the safety fix: the
# frame heartbeat must never ask AVFoundation to capture on an inactive
# connection, must catch an NSException from the capture call, and must keep
# rescheduling itself. Real behaviour can only be proven on a Mac.
if (NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "Pass -DPATCH_FILE=<path to juce-7.0.12-camera-lifecycle.patch>")
endif()

file(READ "${PATCH_FILE}" patch_text)

set(required_added_lines
    "+        if (videoConnection == nil || ! [videoConnection isActive] || ! [videoConnection isEnabled])"
    "+        @try"
    "+        @catch (NSException* exception)"
    "+    void scheduleImageCaptureRetry()"
    "+                        withOwner (self, [] (Pimpl& owner) { owner.scheduleImageCaptureRetry(); });"
    "+                            ownerLink->owner->scheduleImageCaptureRetry();"
    # MAC-CAM-2: macOS 27 aborted on launch when the preview layer's
    # configuration commit raced the queued startRunning. The layer is
    # attached before the start, the start is caught and retried, and no
    # reconfiguration runs while a start is queued.
    "+                        [retainedSession startRunning];"
    "+                            weakRef->sessionStartFinished (started, raised);"
    "+        if (sessionStartQueued)"
    "+        return view;"
    # MAC-CAM-3: a chosen smooth format stays locked through startRunning
    # (Apple's documented macOS pattern), is unlocked on the session queue on
    # every path, and is re-asserted once after the start if it was replaced.
    "+        heldDevice = [device retain];"
    "+                        [deviceToUnlock unlockForConfiguration];"
    "+        releaseHeldDevice();"
    "+        reassertChosenFormat();"
    # MAC-CAM-4: didStart reports the movie's first-frame estimate on the
    # host clock, not just its own arrival.
    "+                const CMTime recorded = [output recordedDuration];"
    "+            withOwner (self, [&] (Pimpl& owner) { owner.recordingStarted (file, firstFrameMs); });"
    # MAC-CAM-5: the preview view is allocated (+1) and setView retains it, so
    # the viewer must drop the allocation reference or every closed camera
    # leaks its NSView, preview layer and capture session.
    "+        [view release];"
    "+       #if ! __has_feature (objc_arc)"
    # MAC-CAM-6: cameras are told apart by AVCaptureDevice.uniqueID, listed
    # in the same enumeration (and so the same order) as their names, and an
    # opened device reports the id of the device it actually opened.
    "+    static void getAvailableDevicesWithIds (StringArray& names, StringArray& identifiers);"
    "+        Pimpl::getAvailableDevicesWithIds (names, identifiers);"
    "+    return pimpl->getDeviceIdentifier();"
    # MAC-CAM-7: the smooth-format check before the start reads the format the
    # device had before, not the one the preset will pick. A device once
    # switched is switched again on every open (a reopen used to see the
    # smooth format the last open left and skip the hold), and a running
    # session's slow preset format is judged and switched after the start.
    "+        const bool neededBefore = devicesNeedingSmoothFormat().count (deviceIdentifier) > 0;"
    "+        if (! neededBefore && maxRateOf (device.activeFormat) >= smoothRate)"
    "+            devicesNeedingSmoothFormat().insert (deviceIdentifier);"
    "+        if (chosenFormat != nil || currentInput == nil || isRecording.load())"
    "+        chooseSmoothFormatAfterStart();")

set(failed FALSE)
foreach (line IN LISTS required_added_lines)
    string(FIND "${patch_text}" "${line}" position)
    if (position EQUAL -1)
        message(SEND_ERROR "JUCE camera patch is missing required line: ${line}")
        set(failed TRUE)
    endif()
endforeach()

if (failed)
    message(FATAL_ERROR "MAC-CAM-1/2 camera guards are not in the JUCE patch")
endif()

# The preview layer must be attached before the session start is queued.
string(FIND "${patch_text}" "[view setLayer: [AVCaptureVideoPreviewLayer layerWithSession: getCaptureSession()]];" layer_at REVERSE)
string(FIND "${patch_text}" "+        startSession();\r\n+        return view;" start_at)
if (start_at EQUAL -1)
    string(FIND "${patch_text}" "+        startSession();\n+        return view;" start_at)
endif()
if (layer_at EQUAL -1 OR start_at EQUAL -1 OR start_at LESS layer_at)
    message(FATAL_ERROR "MAC-CAM-2 preview layer is not attached before the capture session starts")
endif()

# MAC-CAM-3: the held format is unlocked only after startRunning has returned.
string(FIND "${patch_text}" "+                        [retainedSession startRunning];" running_at)
string(FIND "${patch_text}" "+                        [deviceToUnlock unlockForConfiguration];" unlock_at)
if (running_at EQUAL -1 OR unlock_at EQUAL -1 OR unlock_at LESS running_at)
    message(FATAL_ERROR "MAC-CAM-3 the chosen format is unlocked before the session has started")
endif()

# B5: a movie start that raises must be reported through onRecordingFinished,
# exactly like a start AVFoundation rejects through the delegate. A catch that
# only cleared isRecording left the camera STARTING for the whole take and made
# Stop wait out the 15 s finalization timeout for a file that never existed.
string(FIND "${patch_text}" "Movie recording start raised" raised_at)
string(FIND "${patch_text}" "+    void stopRecording()" stop_at)
if (stop_at EQUAL -1)
    string(FIND "${patch_text}" " void stopRecording()" stop_at)
endif()
if (raised_at EQUAL -1 OR stop_at EQUAL -1)
    message(FATAL_ERROR "B5 movie start catch is missing from the JUCE patch")
endif()
string(SUBSTRING "${patch_text}" ${raised_at} -1 after_raised)
string(FIND "${after_raised}" "void stopRecording()" catch_len)
string(SUBSTRING "${after_raised}" 0 ${catch_len} catch_body)
string(FIND "${catch_body}" "+            recordingFinished (file, \"Recording could not start: \"" reported_at)
if (reported_at EQUAL -1)
    message(FATAL_ERROR "B5 a raised movie start is swallowed instead of reported through recordingFinished")
endif()

# MAC-CAM-6: a name and its id come from the same device in the same pass.
# Two enumerations (or two loops) could pair one twin's name with the other's
# id, which is the very swap the ids exist to prevent.
set(pair_lf "+            names.add (nsStringToJuce ([device localizedName]));\n+            identifiers.add (nsStringToJuce ([device uniqueID]));")
string(REPLACE "\n" "\r\n" pair_crlf "${pair_lf}")
string(FIND "${patch_text}" "${pair_crlf}" pair_at)
if (pair_at EQUAL -1)
    string(FIND "${patch_text}" "${pair_lf}" pair_at)
endif()
if (pair_at EQUAL -1)
    message(FATAL_ERROR "MAC-CAM-6 camera names and uniqueIDs are not listed together, device by device")
endif()

# MAC-CAM-7: the after-start check runs once the session has started and
# before the format is reported, so the row never says "5 fps" for a camera
# that is about to be switched.
set(after_start_lf "+        reassertChosenFormat();\n+        chooseSmoothFormatAfterStart();\n+        reportActiveFormat();")
string(REPLACE "\n" "\r\n" after_start_crlf "${after_start_lf}")
string(FIND "${patch_text}" "${after_start_crlf}" after_start_at)
if (after_start_at EQUAL -1)
    string(FIND "${patch_text}" "${after_start_lf}" after_start_at)
endif()
if (after_start_at EQUAL -1)
    message(FATAL_ERROR "MAC-CAM-7 the slow preset format is not checked after the session starts")
endif()

message(STATUS "JUCE camera patch keeps the guarded, self-rescheduling heartbeat")
