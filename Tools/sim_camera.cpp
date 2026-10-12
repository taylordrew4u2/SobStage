// Runs the real CameraController against a virtual camera layer.
//
// JUCE implements CameraDevice on macOS and Windows only, so on Linux the whole
// camera path is #if'd out and nothing has ever exercised it. This compiles
// CameraController.cpp unmodified with JUCE_USE_CAMERA=1 against
// Simulation/Camera's stand-in juce_video, and DRIVES it: cameras arriving,
// cameras going away, and two of the same model staying apart.
//
// The stand-in also records open attempts. It deliberately does not create a
// viewer component, but that is enough to verify the controller's failed-open
// retry policy without needing a GUI message loop.

#include "../Simulation/Camera/juce_video/juce_video.h"
#include "../Source/App/CameraController.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check (bool condition, const std::string& what)
{
    ++checks;

    if (! condition)
        ++failures;

    std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
}

std::vector<std::string> namesFrom (const mma::CameraController& controller)
{
    std::vector<std::string> out;

    for (const auto& cam : controller.getSelection().getAvailableCameras())
        out.push_back (cam.displayName);

    return out;
}

void refreshNow (mma::CameraController& controller)
{
    controller.refreshCameras();
    check (controller.waitForCameraRefresh (2000),
           "camera discovery completes and publishes its result");
}

/// The list follows the OS, in both directions. Everything the app says about a
/// camera arriving or leaving is a diff of this list, so a list that does not
/// move is a rig change nobody can be told about.
void aCameraArrivingAndLeavingMovesTheList()
{
    std::printf ("\nA camera plugged in, then pulled out\n");

    fakecamera::setDevices ({});
    mma::CameraController controller;
    refreshNow (controller);
    check (namesFrom (controller).empty(), "no cameras to begin with");
    check (! fakecamera::wasLastEnumerationOnThisThread(),
           "camera discovery runs off the caller/message thread");

    fakecamera::setDevices ({ "Logitech C920" });
    refreshNow (controller);

    const auto after = namesFrom (controller);
    check (after.size() == 1, "the camera appears once it is listed");
    check (! after.empty() && after.front() == "Logitech C920", "and is called what the OS calls it");

    fakecamera::setDevices ({});
    refreshNow (controller);
    check (namesFrom (controller).empty(), "and is gone once the OS stops listing it");
}

/// AVFoundation and DirectShow enumeration are unbounded platform calls. A
/// driver which never returns must not turn ordinary app shutdown into an
/// equally unbounded join, and its eventual result must remain detached from a
/// later controller instance.
void aStuckDiscoveryCannotHoldControllerLifetime()
{
    std::printf ("\nA camera discovery call stuck while the controller is destroyed\n");

    fakecamera::setDevices ({ "Late Camera" });
    fakecamera::pauseNextEnumerations (1);

    auto controller = std::make_unique<mma::CameraController>();
    controller->refreshCameras();
    check (fakecamera::waitForPausedEnumerationCount (1, 2000),
           "the simulated platform discovery is held inside its worker");

    // The delayed release keeps this test finite if a blocking join is ever
    // reintroduced. Correct teardown returns long before the safety release.
    std::thread safetyRelease ([]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (750));
        fakecamera::releaseOnePausedEnumeration();
    });

    const auto before = std::chrono::steady_clock::now();
    controller.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);

    check (elapsed < std::chrono::milliseconds (250),
           "controller destruction never waits for the stuck discovery call");
    safetyRelease.join();
    check (fakecamera::waitForNoPausedEnumerations (2000),
           "the late platform result is released after its owner is gone");

    fakecamera::setDevices ({ "Replacement Camera" });
    mma::CameraController replacement;
    refreshNow (replacement);
    check (namesFrom (replacement) == std::vector<std::string> { "Replacement Camera" },
           "a late result from the destroyed owner cannot enter a replacement controller");
}

/// A refresh requested while an earlier OS call is blocked supersedes that
/// call. Hold the replacement call too so the message thread gets a reliable
/// chance to prove the stale first snapshot was never published.
void aSupersededDiscoveryCannotPublishItsStaleSnapshot()
{
    std::printf ("\nA newer refresh supersedes a blocked camera snapshot\n");

    fakecamera::setDevices ({ "Old Camera" });
    fakecamera::pauseNextEnumerations (2);
    mma::CameraController controller;
    controller.refreshCameras();
    check (fakecamera::waitForPausedEnumerationCount (1, 2000),
           "the old snapshot is paused after being captured");

    fakecamera::setDevices ({ "New Camera" });
    controller.refreshCameras();
    fakecamera::releaseOnePausedEnumeration();
    check (fakecamera::waitForPausedEnumerationCount (2, 2000),
           "the worker discards the old generation and starts the replacement scan");

    check (! controller.applyPendingCameraList() && namesFrom (controller).empty(),
           "the superseded old snapshot is never exposed while the fresh scan is pending");

    fakecamera::releaseOnePausedEnumeration();
    check (controller.waitForCameraRefresh (2000),
           "the newest discovery generation completes and is published");
    check (namesFrom (controller) == std::vector<std::string> { "New Camera" },
           "only the newest camera snapshot reaches controller state");
    check (fakecamera::waitForNoPausedEnumerations (2000),
           "no simulated camera discovery remains paused");
}

/// The UI asks for topology every two seconds. Those observational polls must
/// not continually supersede an OS enumeration which is slow but will return,
/// or a scan taking just over two seconds could leave the camera list empty
/// forever despite completing successfully each time.
void periodicPollingCannotStarveASlowDiscovery()
{
    std::printf ("\nPeriodic polling while one camera discovery is slow\n");

    fakecamera::setDevices ({ "Slow Camera" });
    fakecamera::pauseNextEnumerations (1);
    mma::CameraController controller;
    controller.refreshCameras();
    check (fakecamera::waitForPausedEnumerationCount (1, 2000),
           "the slow camera snapshot remains in flight");

    for (int i = 0; i < 8; ++i)
        controller.refreshCamerasIfIdle();

    fakecamera::releaseOnePausedEnumeration();
    check (controller.waitForCameraRefresh (2000),
           "periodic polls do not invalidate the in-flight generation");
    check (namesFrom (controller) == std::vector<std::string> { "Slow Camera" },
           "the slow successful snapshot is eventually visible");
    check (fakecamera::waitForNoPausedEnumerations (2000),
           "the slow simulated enumeration is fully released");
}

/// §14.6, applied to pictures: two cameras of the same model enumerate with the
/// same product string. If they collapse into one entry, the second camera is
/// silently missing from the rig -- and from anything said about it.
void twoOfTheSameModelStayApart()
{
    std::printf ("\nTwo cameras of the same model\n");

    fakecamera::setDevices ({ "HD Webcam", "HD Webcam" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();
    mma::CameraController controller;
    refreshNow (controller);

    const auto& cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both are listed");

    if (cameras.size() == 2)
    {
        check (cameras[0].id != cameras[1].id, "and they are told apart by id, not by name");

        controller.getSelection().setEnabled (cameras[0].id, true);
        controller.getSelection().setEnabled (cameras[1].id, true);
        controller.applySelection (true);
        check (fakecamera::getOpenedDeviceIndices() == std::vector<int> ({ 0, 1 }),
               "each duplicate id opens its own OS occurrence instead of the first matching name");
    }
}

/// A camera that is unplugged and plugged back in is the same camera. Its id
/// has to come back the same, or every remembered choice about it is lost.
void aCameraThatComesBackKeepsItsIdentity()
{
    std::printf ("\nA camera unplugged and plugged back in\n");

    fakecamera::setDevices ({ "Studio Cam" });
    mma::CameraController controller;
    refreshNow (controller);

    const auto before = controller.getSelection().getAvailableCameras();
    check (before.size() == 1, "listed to begin with");

    fakecamera::setDevices ({});
    refreshNow (controller);

    fakecamera::setDevices ({ "Studio Cam" });
    refreshNow (controller);

    const auto after = controller.getSelection().getAvailableCameras();
    check (after.size() == 1, "listed again after coming back");

    if (! before.empty() && ! after.empty())
        check (before.front().id == after.front().id, "and is the same camera, by id");
}

/// A privacy denial or a camera held by another app can make openDevice block
/// and fail. The status timer still applies the selection twice a second; that
/// timer must retain the explanation without retrying the OS call forever.
void aFailedOpenWaitsForAnExplicitRetry()
{
    std::printf ("\nA camera that will not open\n");

    fakecamera::setDevices ({ "Busy Camera" });
    fakecamera::setOpenSucceeds (false);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    const auto& cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the busy camera is listed");

    if (cameras.empty())
        return;

    controller.getSelection().setEnabled (cameras.front().id, true);
    controller.applySelection();
    check (fakecamera::getOpenCallCount() == 1, "the first selection attempts one open");
    check (controller.getProblem().isNotEmpty(), "the failed open remains explained");

    controller.applySelection();
    controller.applySelection();
    check (fakecamera::getOpenCallCount() == 1,
           "periodic selection refreshes do not retry the failed OS call");
    check (controller.getProblem().isNotEmpty(), "the explanation survives those refreshes");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-busy", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a busy-camera take folder is available");
    check (! controller.startRecording (takeFolder),
           "a connected camera which failed to open is not claimed as recording");
    check (controller.getTakePlans().size() == 1
               && controller.getTakeVideoRecords().empty(),
           "the watchdog retains it but the manifest invents no movie");

    refreshNow (controller);
    check (namesFrom (controller).size() == 1,
           "a topology refresh keeps the connected failed camera listed during the take");
    check (controller.getProblem().containsIgnoreCase ("Couldn't open Busy Camera")
               && ! controller.getProblem().containsIgnoreCase ("system isn't listing"),
           "its busy/privacy error is not replaced by a false disconnected diagnosis");

    controller.stopRecording();

    fakecamera::setOpenSucceeds (true);
    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2, "an explicit action retries once");
    check (! controller.getProblem().containsIgnoreCase ("Couldn't open Busy Camera"),
           "a successful retry clears the open failure while retaining the prior take result");
    check (controller.startRecording (takeFolder),
           "the recovered camera can join the next take");
    check (controller.getProblem().isEmpty(),
           "the successful next take replaces the prior failed-start result");
    controller.stopRecording();

    takeFolder.deleteRecursively();
}

/// JUCE accepts an array index, then enumerates the OS again inside openDevice.
/// If the list moves in between, blindly trusting that index records the wrong
/// camera under the selected camera's name. The controller must reject the
/// mismatched instance and retry only after its worker publishes a fresh map.
void aReorderedListCannotOpenTheWrongCamera()
{
    std::printf ("\nA camera list that reorders between discovery and open\n");

    fakecamera::setDevices ({ "Wide Camera", "Close Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;

    // Off before discovery, so nothing opens on the refresh itself: the open
    // under test has to be the one made against the stale index below.
    controller.getSelection().setEnabled ("Wide Camera", false);
    controller.getSelection().setEnabled ("Close Camera", false);
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both cameras are in the discovered snapshot");
    if (cameras.size() != 2)
        return;

    controller.getSelection().setEnabled ("Wide Camera", true);

    // Do not refresh the controller yet: this is the exact race window between
    // its completed background snapshot and JUCE's internal re-enumeration.
    fakecamera::setDevices ({ "Close Camera", "Wide Camera" });
    controller.applySelection (true);

    check (fakecamera::getOpenCallCount() == 1, "the stale index is attempted once");
    check (fakecamera::getLastOpenedDeviceName() == "Close Camera",
           "the fake exposes that JUCE's index now names the other camera");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the mismatched camera is immediately discarded rather than retained");
    check (controller.getProblem().isNotEmpty(), "the topology retry is visible while pending");

    check (controller.waitForCameraRefresh (2000),
           "the mismatch automatically requests and applies a fresh discovery");
    check (fakecamera::getOpenCallCount() == 2, "the selected camera is retried once with the fresh map");
    check (fakecamera::getLastOpenedDeviceName() == "Wide Camera",
           "the retry opens the camera the user actually selected");
    check (fakecamera::getLiveDeviceCount() == 1, "only that selected camera remains open");
    check (controller.getProblem().isEmpty(), "the transient topology problem clears after recovery");
}

/// Pending topology is consumed while the camera drawer is visible too. That
/// path has no outer applySelection call, so the controller itself must release
/// an unplugged device and must not reuse it when the same camera comes back.
void topologyConsumptionOwnsOpenDeviceReconciliation()
{
    std::printf ("\nUnplug and reconnect while only topology refreshes run\n");

    fakecamera::setDevices ({ "Panel Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();

    {
        mma::CameraController controller;
        refreshNow (controller);
        controller.getSelection().setEnabled ("Panel Camera", true);
        controller.applySelection (true);

        check (fakecamera::getLiveDeviceCount() == 1, "the enabled camera begins open");

        fakecamera::setDevices ({});
        refreshNow (controller);
        check (fakecamera::getLiveDeviceCount() == 0,
               "consuming the unplug snapshot closes the device without an outer selection pass");

        fakecamera::setDevices ({ "Panel Camera" });
        refreshNow (controller);
        check (fakecamera::getOpenCallCount() == 2, "the remembered enabled camera is opened afresh");
        check (fakecamera::getLiveDeviceCount() == 1,
               "the reconnect owns one new device rather than the stale pre-unplug object");
    }

    check (fakecamera::getLiveDeviceCount() == 0, "controller teardown releases the reconnected device");
}

/// A capture card remembered as enabled must not disappear just because the
/// latest OS snapshot omits it. The UI needs an unavailable row and a concrete
/// reason before the user trusts a take that cannot contain that picture.
void anEnabledMissingCaptureCardStaysVisibleAsAProblem()
{
    std::printf ("\nA remembered capture card missing from the OS list\n");

    fakecamera::setDevices ({});
    mma::CameraController controller;
    controller.getSelection().setEnabled ("USB2 Video", true);
    controller.getSelection().setAssignedName ("USB2 Video", "HDMI wide");
    refreshNow (controller);

    const auto missing = controller.getSelection().getUnavailableEnabledCameras();
    check (missing.size() == 1, "the enabled missing camera remains in controller state");
    check (! missing.empty() && missing.front().displayName == "HDMI wide",
           "its remembered name remains available to the UI");
    check (controller.getProblem().containsIgnoreCase ("system isn't listing HDMI wide"),
           "the problem names the missing camera and the OS boundary");
    check (controller.getProblem().containsIgnoreCase ("HDMI signal"),
           "the recovery text covers a capture card's video source");
    check (fakecamera::getLiveDeviceCount() == 0,
           "a missing camera never creates a phantom open device");
}

/// Remembered choices are loaded synchronously but camera discovery is not.
/// The empty pre-discovery state must not be presented as an unplug or allowed
/// to become an empty frozen take plan.
void aRememberedCameraWaitsForTheFirstSnapshot()
{
    std::printf ("\nA remembered camera before the first OS snapshot\n");

    fakecamera::setDevices ({});
    fakecamera::setOpenSucceeds (true);

    mma::CameraController controller;
    controller.getSelection().setEnabled ("USB2 Video", true);
    controller.applySelection (true);

    check (controller.isInitialDiscoveryPending(),
           "the controller distinguishes pending discovery from an empty snapshot");
    check (controller.getProblem().isEmpty(),
           "a remembered camera is not falsely called missing before discovery");

    refreshNow (controller);
    check (! controller.isInitialDiscoveryPending(),
           "the first completed OS snapshot ends the pending state");
    check (controller.getProblem().containsIgnoreCase ("system isn't listing USB2 Video"),
           "an actually empty snapshot reports the remembered camera as missing");
}

/// Audio recording must never wait forever on a platform camera enumeration.
/// Starting before the first snapshot keeps the remembered camera in the
/// frozen plan as missing, and a late first snapshot is held for the next take.
void aTakeBeforeFirstDiscoveryKeepsTheCameraPlanHonest()
{
    std::printf ("\nA take starts before the first camera snapshot\n");

    fakecamera::setDevices ({ "Slow HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    controller.getSelection().setEnabled ("Slow HDMI", true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-first-scan", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a pre-discovery take folder is available");
    check (! controller.startRecording (takeFolder),
           "the missing pre-snapshot writer is not claimed as recording");
    check (controller.getTakePlans().size() == 1,
           "the remembered camera is retained in the frozen take plan");
    check (controller.getTakeVideoRecords().empty(),
           "the take manifest cannot claim a movie which never started");
    const auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && ! states.front().recording,
           "the frozen roster reports the remembered camera as missing");

    refreshNow (controller);
    check (namesFrom (controller).empty()
               && fakecamera::getOpenCallCount() == 0
               && fakecamera::getStartRecordingCallCount() == 0,
           "the late first snapshot cannot join the active take");

    controller.stopRecording();
    check (namesFrom (controller).size() == 1
               && fakecamera::getOpenCallCount() == 1
               && fakecamera::getLiveDeviceCount() == 1,
           "the camera from the delayed snapshot opens immediately for the next take");

    takeFolder.deleteRecursively();
}

/// A camera absent at t=0 is part of the intended/frozen roster, but JUCE
/// cannot append it when it arrives later. Keep it unavailable for this take,
/// record the missing writer, and open it only for the next one.
void aCameraMissingAtTakeStartCannotJoinMidTake()
{
    std::printf ("\nA camera missing at take start arrives mid-take\n");

    fakecamera::setDevices ({});
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    controller.getSelection().setEnabled ("Late HDMI", true);
    refreshNow (controller);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-late-arrival", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a late-arrival take folder is available");
    check (! controller.startRecording (takeFolder),
           "the controller does not claim the missing camera started");
    check (controller.getTakePlans().size() == 1,
           "the missing enabled camera remains in the frozen take plan");
    check (controller.getTakeVideoRecords().empty(),
           "the missing camera is not written as a fictional movie in session metadata");

    auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && ! states.front().recording,
           "the watchdog sees the expected camera as not recording");

    fakecamera::setDevices ({ "Late HDMI" });
    refreshNow (controller);
    check (namesFrom (controller).empty() && fakecamera::getLiveDeviceCount() == 0,
           "a mid-take arrival is not advertised or opened as part of this take");
    check (fakecamera::getStartRecordingCallCount() == 0,
           "the late camera never creates a misleading partial writer");

    controller.stopRecording();
    check (namesFrom (controller).size() == 1
               && fakecamera::getOpenCallCount() == 1
               && fakecamera::getLiveDeviceCount() == 1,
           "the remembered camera opens before an immediate next take");
    check (controller.startRecording (takeFolder)
               && fakecamera::getStartRecordingCallCount() == 1,
           "an immediate second take records the camera without waiting for another scan");
    controller.stopRecording();

    takeFolder.deleteRecursively();
}

/// AVFoundation's preview layer is tied to the capture session which created
/// it. Screen changes move one persistent layer between disposable hosts; they
/// must never ask JUCE to create a second preview for the running session.
void oneNativeViewerMovesBetweenScreens()
{
    std::printf ("\nOne native preview moves between UI hosts\n");

    fakecamera::setDevices ({ "HDMI Capture" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetViewerCreateCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("HDMI Capture", true);
    controller.applySelection (true);

    check (fakecamera::getViewerCreateCallCount() == 1,
           "opening creates the native preview exactly once");

    auto mainHost = controller.createViewer ("HDMI Capture");
    check (mainHost != nullptr && mainHost->getNumChildComponents() == 1,
           "the first screen hosts that native preview");

    auto panelHost = controller.createViewer ("HDMI Capture");
    check (panelHost != nullptr && panelHost->getNumChildComponents() == 1,
           "the next screen reparents the same preview");
    check (mainHost != nullptr && mainHost->getNumChildComponents() == 0,
           "the previous host no longer retains the preview");
    check (fakecamera::getViewerCreateCallCount() == 1,
           "moving screens never asks JUCE for another preview layer");
}

/// A CameraDevice pointer is not proof that a preview was created. A failed
/// native layer must remain retryable, and its revision must invalidate the
/// same-id placeholder cached by both camera screens.
void aFailedViewerCanRecoverWithTheSameId()
{
    std::printf ("\nA failed native preview recovers under the same camera id\n");

    fakecamera::setDevices ({ "Capture Card" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (false);
    fakecamera::resetViewerCreateCallCount();

    mma::CameraController controller;
    // Off before discovery, so the one preview attempt counted below is the
    // explicit open and not the refresh's.
    controller.getSelection().setEnabled ("Capture Card", false);
    refreshNow (controller);
    controller.getSelection().setEnabled ("Capture Card", true);
    controller.applySelection (true);

    const auto failedRevision = controller.getViewerRevision ("Capture Card");
    check (controller.createViewer ("Capture Card") == nullptr,
           "a missing native preview is never treated as a usable camera");
    check (controller.getProblem().isNotEmpty(), "the missing preview is explained");
    check (fakecamera::getViewerCreateCallCount() == 1,
           "the failed open made one native preview attempt");

    fakecamera::setViewerSucceeds (true);
    controller.applySelection (true);

    check (controller.getViewerRevision ("Capture Card") > failedRevision,
           "the successful same-id retry changes the UI cache revision");
    check (controller.createViewer ("Capture Card") != nullptr,
           "the retry now supplies a live preview host");
    check (fakecamera::getViewerCreateCallCount() == 2,
           "the explicit retry makes exactly one fresh native preview");
    check (controller.getProblem().isEmpty(), "successful preview recovery clears the problem");
}

/// A CameraDevice and native viewer can both exist while a capture card has no
/// HDMI input. That black handle is not a recordable camera until this exact
/// open generation has delivered a valid image callback.
void anOpenedCaptureCardWithoutAFrameIsNotRecordable()
{
    std::printf ("\nAn opened capture card with no video frames\n");

    fakecamera::setDevices ({ "Signal-less HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Signal-less HDMI", true);
    controller.applySelection (true);

    check (controller.getSignalState ("Signal-less HDMI")
               == mma::CameraController::SignalState::Waiting,
           "a non-null device/viewer begins in waiting, not live");
    check (controller.createViewer ("Signal-less HDMI") == nullptr,
           "the black native viewer is hidden until an image proves it live");
    check (controller.getSignalStatusText ("Signal-less HDMI")
               .containsIgnoreCase ("Waiting for video"),
           "the waiting tile names the missing video signal plainly");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-no-frame", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a no-frame take folder is available");
    check (! controller.startRecording (takeFolder),
           "a camera without first-frame proof is not reported as recording");
    check (fakecamera::getStartRecordingCallCount() == 0,
           "no movie writer is started for the unproved source");

    const auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && ! states.front().recording,
           "the frozen take roster exposes that omission to the UI/watchdog");

    controller.stopRecording();
    takeFolder.deleteRecursively();
    fakecamera::setAutoFrameOnListener (true);
}

/// When some cameras start and some do not, the problem has to say WHICH.
///
/// It used to say "1 of your cameras couldn't start recording." -- a count, to
/// someone looking at a rig of several, with the roster of display names
/// sitting in takePlans the whole time. Knowing which one is the difference
/// between checking one cable and checking all of them.
void aCameraThatCannotStartIsNamedNotCounted()
{
    std::printf ("\nOne camera of two cannot start\n");

    fakecamera::setDevices ({ "Good Camera", "Signal-less HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Good Camera", true);
    controller.getSelection().setEnabled ("Signal-less HDMI", true);
    controller.applySelection (true);

    // Take the frame away from one of them only, then re-open it so it sits
    // there with no first frame while its neighbour is fine.
    fakecamera::setAutoFrameOnListener (false);
    controller.getSelection().setEnabled ("Signal-less HDMI", false);
    controller.applySelection (true);
    controller.getSelection().setEnabled ("Signal-less HDMI", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-partial-start", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a partial-start take folder is available");
    check (controller.startRecording (takeFolder), "the take begins on the camera that works");

    const auto problem = controller.getProblem();
    check (problem.contains ("Signal-less HDMI"),
           "the problem names the camera that could not start");
    check (! problem.contains ("Good Camera"),
           "and does not name the one that did");
    check (! problem.contains ("1 of your cameras"),
           "it is a name, not a count");
    check (problem.contains ("sound is recording"),
           "and it still says the sound is safe");

    controller.stopRecording();
    takeFolder.deleteRecursively();
    fakecamera::setAutoFrameOnListener (true);
}

/// No-frame timeout is actionable, but not terminal. Some HDMI sources become
/// valid only after a resolution/HDCP handshake; their first late frame should
/// promote the same open generation without a destructive reopen.
void aLateFirstFrameRecoversAfterTheSignalTimeout()
{
    std::printf ("\nA late HDMI frame after the waiting timeout\n");

    fakecamera::setDevices ({ "Late Signal HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetOpenCallCount();
    fakecamera::resetListenerCallCounts();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Late Signal HDMI", true);
    controller.applySelection (true);

    const auto waitingRevision = controller.getViewerRevision ("Late Signal HDMI");
    controller.advanceSignalClockForTesting (5001.0);
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::TimedOut,
           "five seconds without a frame becomes a signal timeout");
    check (controller.getProblem().containsIgnoreCase ("For HDMI")
               && controller.getProblem().containsIgnoreCase ("USB cables"),
           "the timeout explains the camera and capture-card checks to make");
    check (controller.getViewerRevision ("Late Signal HDMI") > waitingRevision,
           "the timeout invalidates the cached waiting tile");

    fakecamera::emitFrame ("Late Signal HDMI");
    check (controller.applyPendingCameraList(),
           "a late frame is consumed without reopening the camera");
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::Live,
           "the late current-generation frame proves the source live");
    check (controller.createViewer ("Late Signal HDMI") != nullptr,
           "the native preview is exposed only after that proof");
    check (controller.getProblem().isEmpty(),
           "the recovered signal clears its timeout explanation");
    check (fakecamera::getOpenCallCount() == 1,
           "late-signal recovery does not churn the platform device");
    check (fakecamera::getAddListenerCallCount() == 1
               && fakecamera::getRemoveListenerCallCount() == 0,
           "the throttled proof listener remains attached to detect later signal loss");

    controller.advanceSignalClockForTesting (5001.0);
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::TimedOut,
           "an already-live source becomes SIGNAL LOST when its heartbeat stops");
    fakecamera::emitFrame ("Late Signal HDMI");
    controller.applyPendingCameraList();
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::Live,
           "a later heartbeat restores that same source without reopening it");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-late-frame", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a recovered-signal take folder is available");
    check (controller.startRecording (takeFolder)
               && fakecamera::getStartRecordingCallCount() == 1,
           "the now-proved camera is eligible for the next frozen take");
    controller.stopRecording();
    takeFolder.deleteRecursively();
    fakecamera::setAutoFrameOnListener (true);
}

/// A capture dongle with no HDMI signal or an HDCP-protected source streams
/// valid but all-black frames. The tile must say so in words instead of
/// showing a silent black rectangle, and come back on the first real picture.
void anAllBlackPictureIsExplainedNotShown()
{
    std::printf ("\nA capture card streaming all-black frames\n");

    fakecamera::setDevices ({ "USB2 Video" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("USB2 Video", true);
    controller.applySelection (true);

    fakecamera::emitFrame ("USB2 Video", true);
    controller.applyPendingCameraList();
    check (controller.createViewer ("USB2 Video") != nullptr
               && controller.getSignalStatusText ("USB2 Video").isEmpty(),
           "one dark proof (a camera still exposing) is not yet a warning");

    const auto liveRevision = controller.getViewerRevision ("USB2 Video");
    fakecamera::emitFrame ("USB2 Video", true);
    fakecamera::emitFrame ("USB2 Video", true);
    controller.applyPendingCameraList();
    check (controller.getSignalState ("USB2 Video") == mma::CameraController::SignalState::Live,
           "black frames still prove the device is streaming");
    check (controller.createViewer ("USB2 Video") == nullptr,
           "a run of black frames swaps the black preview for a placeholder");
    check (controller.getSignalStatusText ("USB2 Video").containsIgnoreCase ("all black")
               && controller.getSignalStatusText ("USB2 Video").containsIgnoreCase ("HDCP"),
           "the placeholder says the picture is black and why");
    check (controller.getViewerRevision ("USB2 Video") > liveRevision,
           "the black warning invalidates the cached tile");

    fakecamera::emitFrame ("USB2 Video");
    controller.applyPendingCameraList();
    check (controller.createViewer ("USB2 Video") != nullptr
               && controller.getSignalStatusText ("USB2 Video").isEmpty(),
           "the first real picture brings the preview straight back");
    check (fakecamera::getOpenCallCount() == 1, "none of this reopens the device");

    fakecamera::setAutoFrameOnListener (true);
}

/// macOS: opening a camera while the privacy prompt is still up gives a
/// session that streams black and never recovers. Hold the open until the
/// answer is in, ask once, and open on the first poll after Allow.
void aCameraWaitsForPrivacyPermission()
{
    std::printf ("\nA camera switched on before camera access is granted\n");

    fakecamera::setDevices ({ "FaceTime HD Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();

    auto permission = std::make_shared<mma::PermissionState> (mma::PermissionState::NotYetRequested);
    auto requests = std::make_shared<int> (0);

    mma::CameraController controller;
    controller.setCameraPermission ([permission] { return *permission; },
                                    [requests] { ++*requests; });
    refreshNow (controller);
    controller.getSelection().setEnabled ("FaceTime HD Camera", true);
    controller.applySelection (true);
    controller.applySelection (false);

    check (fakecamera::getOpenCallCount() == 0, "no camera opens before the user answers");
    check (*requests == 1, "the system prompt is asked for exactly once");
    check (controller.getProblem().containsIgnoreCase ("allow camera access"),
           "the problem line says what the user needs to do");
    check (controller.getSignalStatusText ("FaceTime HD Camera").containsIgnoreCase ("camera access"),
           "the tile says it is waiting for camera access, not to close other apps");

    *permission = mma::PermissionState::Denied;
    controller.applyPendingCameraList();
    check (fakecamera::getOpenCallCount() == 0
               && controller.getProblem().containsIgnoreCase ("Privacy & Security"),
           "a denial keeps the camera closed and points at System Settings");

    *permission = mma::PermissionState::Granted;
    check (controller.applyPendingCameraList(), "the grant is noticed on the next poll");
    check (fakecamera::getOpenCallCount() == 1
               && controller.getSignalState ("FaceTime HD Camera")
                      == mma::CameraController::SignalState::Live,
           "the camera opens and goes live without the user toggling it");
    check (controller.getProblem().isEmpty(), "the permission message clears");
}

/// The per-camera quality caps what the camera is asked for, survives as a
/// choice, reopens the camera to take effect, and the panel can say what the
/// camera is actually running at once it reports.
void aCameraQualityCapsTheOpenAndReportsTheFormat()
{
    std::printf ("\nPer-camera recording quality\n");

    fakecamera::setDevices ({ "Cam Link 4K" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Cam Link 4K", true);
    controller.applySelection (true);

    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLastOpenMaxHeight() == 2160,
           "Best asks for up to 4K");
    check (controller.getActiveFormatText ("Cam Link 4K").isEmpty(),
           "nothing is claimed before the camera reports its format");

    fakecamera::emitFormat ("Cam Link 4K", 3840, 2160, 29.97);
    check (controller.applyPendingCameraList()
               && controller.getActiveFormatText ("Cam Link 4K") == "3840 x 2160, 30 fps",
           "the reported format is shown as size and rounded frame rate");

    controller.setCameraQuality ("Cam Link 4K", mma::CameraQuality::HD1080);
    controller.applyPendingCameraList();
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLastOpenMaxHeight() == 1080,
           "choosing 1080p reopens the camera capped at 1080");
    check (controller.getActiveFormatText ("Cam Link 4K").isEmpty(),
           "the old format is not shown for the reopened camera");

    controller.setCameraQuality ("Cam Link 4K", mma::CameraQuality::HD1080);
    check (fakecamera::getOpenCallCount() == 2, "choosing the same quality again does nothing");
}

/// A callback already queued when an identical capture card reconnects belongs
/// to the old platform object. Product-name identity must not let that event
/// certify the replacement generation.
void aStaleGenerationFrameCannotCertifyAReopenedCamera()
{
    std::printf ("\nA stale first frame after a same-id camera reopen\n");

    fakecamera::setDevices ({ "Same-id HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Same-id HDMI", true);
    controller.applySelection (true);
    fakecamera::emitFrame ("Same-id HDMI"); // queued for generation one

    controller.getSelection().setEnabled ("Same-id HDMI", false);
    controller.applySelection (true);
    controller.getSelection().setEnabled ("Same-id HDMI", true);
    controller.applySelection (true); // generation two, still without a frame

    check (fakecamera::getOpenCallCount() == 2,
           "the same app id now owns a fresh platform camera generation");
    controller.applyPendingCameraList();
    check (controller.getSignalState ("Same-id HDMI")
               == mma::CameraController::SignalState::Waiting,
           "the queued old-generation frame cannot certify the replacement");
    check (controller.createViewer ("Same-id HDMI") == nullptr,
           "the replacement remains a waiting tile rather than a false preview");

    fakecamera::emitFrame ("Same-id HDMI");
    check (controller.applyPendingCameraList()
               && controller.getSignalState ("Same-id HDMI")
                      == mma::CameraController::SignalState::Live,
           "a frame from the replacement generation certifies only that generation");

    fakecamera::setAutoFrameOnListener (true);
}

/// JUCE can return a non-null CameraDevice and only later report that its input
/// or capture session failed. The callback is drained on the message-thread
/// poll, invalidates the viewer, and leaves an explicit-retry failure behind.
void aRuntimeCameraErrorInvalidatesThePreview()
{
    std::printf ("\nA runtime camera error invalidates its preview\n");

    fakecamera::setDevices ({ "Flaky HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Flaky HDMI", true);
    controller.applySelection (true);

    const auto liveRevision = controller.getViewerRevision ("Flaky HDMI");
    check (controller.createViewer ("Flaky HDMI") != nullptr,
           "the non-null device initially has a live preview host");

    fakecamera::emitRuntimeError ("Flaky HDMI", "capture input stopped");
    check (controller.applyPendingCameraList(),
           "the UI poll consumes a runtime error even without a topology update");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the failed CameraDevice is closed instead of remaining black forever");
    check (controller.createViewer ("Flaky HDMI") == nullptr,
           "the invalid preview is no longer advertised as live");
    check (controller.getViewerRevision ("Flaky HDMI") > liveRevision,
           "runtime failure invalidates same-id UI caches");
    check (controller.getProblem().containsIgnoreCase ("capture input stopped"),
           "the platform's runtime reason is surfaced to the user");

    controller.applySelection (true);
    check (fakecamera::getLiveDeviceCount() == 1,
           "an explicit action can reopen the camera after the runtime failure");
}

/// Selection changes are for the next take. Applying one while a writer is
/// active must not finalize that movie early; the close happens immediately
/// after the take ends instead.
void switchingOffARecordingCameraWaitsForTheTakeToEnd()
{
    std::printf ("\nSwitching off a camera during a take is deferred\n");

    fakecamera::setDevices ({ "HDMI Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("HDMI Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-frozen-roster", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a temporary take folder is available");
    check (controller.startRecording (takeFolder), "the HDMI camera starts recording");

    controller.getSelection().setEnabled ("HDMI Camera", false);
    controller.applySelection (true);
    check (fakecamera::getActiveRecordingCount() == 1
               && fakecamera::getStopRecordingCallCount() == 0,
           "a mid-take setting change does not truncate the movie");

    controller.stopRecording();
    check (fakecamera::getActiveRecordingCount() == 0
               && fakecamera::getStopRecordingCallCount() == 1,
           "the writer is finalized exactly once when the take ends");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the deferred off setting is applied after finalization");

    takeFolder.deleteRecursively();
}

/// Runtime failure suppresses same-take reopening, but once the take ends an
/// enabled device which is still in the OS snapshot gets one fresh preview.
void aRuntimeFailureRetriesAfterTheTakeEnds()
{
    std::printf ("\nA runtime camera failure retries after the take\n");

    fakecamera::setDevices ({ "Recovering HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Recovering HDMI", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-runtime-retry", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a runtime-retry take folder is available");
    check (controller.startRecording (takeFolder), "the recovering camera starts recording");

    fakecamera::emitRuntimeError ("Recovering HDMI", "capture input stopped");
    check (controller.applyPendingCameraList(), "the in-take runtime error is consumed");
    controller.applyPendingCameraList(); // drain the simulator's synchronous didFinish
    check (fakecamera::getLiveDeviceCount() == 0,
           "the failed camera stays closed for the rest of the take");

    controller.stopRecording();
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 1,
           "the same listed camera is reopened automatically for the next take");
    check (controller.getProblem().isEmpty(),
           "a successful post-take retry clears the runtime failure");

    takeFolder.deleteRecursively();
}

/// JUCE cannot append a reconnected camera to the movie it finalized when the
/// device vanished. Reopening that camera's preview during the same take would
/// look like recovery while silently omitting every later frame. Keep the
/// camera absent, retain its partial file, and reopen the remembered preview
/// only after the take ends.
void aRecordedCameraThatReconnectsWaitsForTheNextTake()
{
    std::printf ("\nA recorded camera unplugged and replugged during a take\n");

    fakecamera::setDevices ({ "Recording Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the recording camera is discovered");
    if (cameras.empty())
        return;

    controller.getSelection().setEnabled (cameras.front().id, true);
    controller.applySelection (true);
    check (fakecamera::getLiveDeviceCount() == 1, "the selected preview is open before the take");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-continuity", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a temporary take folder is available");

    check (controller.startRecording (takeFolder), "the camera starts recording");
    check (controller.getTakeVideoRecords().empty(),
           "session metadata cannot claim the movie before its writer finalizes");

    // The record refreshed mid-take is what a crash leaves behind, and the
    // movie being written is in the folder beside the sound.
    {
        const auto soFar = controller.getTakeVideoRecordsSoFar();
        check (soFar.size() == 1 && ! soFar.front().fileName.empty()
                   && ! juce::String (soFar.front().fileName).containsAnyOf ("/\\"),
               "the mid-take record names the movie being written, by file name");
    }
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 1,
           "one camera writer is active");

    auto takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && takeStates.front().recording,
           "the frozen take roster reports that writer as recording");

    fakecamera::setDevices ({});
    refreshNow (controller);
    controller.applyPendingCameraList(); // consume the partial movie's didFinish
    check (fakecamera::getLiveDeviceCount() == 0, "unplug closes the camera device");
    check (! controller.isRecording(), "the controller no longer claims camera recording is live");

    takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && ! takeStates.front().recording,
           "the lost camera remains in the take roster as absent");

    const auto partialInputs = controller.getCombinedTakeInputs();
    check (partialInputs.size() == 1 && ! partialInputs.front().videoFile.empty(),
           "the finalized partial movie remains available to the combiner");
    check (controller.getTakeVideoRecordsSoFar().size() == 1
               && controller.getTakeVideoRecordsSoFar().front().fileName == partialInputs.front().videoFile,
           "and the mid-take record goes on naming it after the unplug");

    fakecamera::setDevices ({ "Recording Camera" });
    refreshNow (controller);
    check (namesFrom (controller).empty(),
           "a same-take reconnect is not advertised as an available live camera");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the reconnected camera preview stays closed for this take");
    check (fakecamera::getOpenCallCount() == 1,
           "the reconnect does not make a second OS open attempt during the take");
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 0,
           "the reconnect is never falsely counted as resumed recording");

    takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && ! takeStates.front().recording,
           "the watchdog-facing state stays lost after reconnect");

    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLiveDeviceCount() == 0,
           "even an explicit selection refresh cannot reopen it mid-take");

    controller.stopRecording();
    check (namesFrom (controller).size() == 1,
           "the remembered camera is advertised immediately after the take ends");
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 1,
           "its remembered preview reopens for the next take");
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 0,
           "reopening the preview does not retroactively restart recording");

    takeFolder.deleteRecursively();
}

/// The Cameras panel says "Recording to <file>" for each camera in a take. That
/// name was rebuilt from the live list, which keeps moving mid-take: unplug the
/// first camera and the second one's row read V01 while the movie it was
/// writing was V02. Someone checking which file holds which angle mid-show was
/// pointed at the wrong one. After Stop the row is about the NEXT take again.
void theRecordingToNameIsTheFileBeingWritten()
{
    std::printf ("\nThe file a camera is recording to, after the list moves mid-take\n");

    fakecamera::setDevices ({ "Wide Cam", "Close Cam" }, { "unit-wide", "unit-close" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("unit-wide", true);
    controller.getSelection().setEnabled ("unit-close", true);
    controller.applySelection (true);

    const auto planned = controller.getPlannedFileNameFor ("unit-close");
    check (planned.startsWith ("V02_"), "before the take the second camera writes V02");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-recording-to", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a recording-to take folder is available");
    check (controller.startRecording (takeFolder), "both cameras start recording");
    check (fakecamera::getActiveRecordingCount() == 2, "both writers are active");

    std::string writing;
    for (const auto& record : controller.getTakeVideoRecordsSoFar())
        if (record.displayName == "Close Cam")
            writing = record.fileName;

    check (! writing.empty() && controller.getPlannedFileNameFor ("unit-close") == juce::String (writing),
           "mid-take the row names the movie the camera is writing");

    // A camera arriving ahead of it in the OS list (a Continuity Camera iPhone,
    // say) renumbers the live plan.
    fakecamera::setDevices ({ "Phone Cam", "Wide Cam", "Close Cam" },
                            { "unit-phone", "unit-wide", "unit-close" });
    refreshNow (controller);
    controller.applyPendingCameraList();
    check (controller.getPlannedFileNames().joinIntoString (",").contains ("V03_"),
           "a camera arriving mid-take moves the live numbering");
    check (controller.getPlannedFileNameFor ("unit-close") == juce::String (writing),
           "the recording camera's row still names its own movie");

    // The first camera unplugged mid-take.
    fakecamera::setDevices ({ "Close Cam" }, { "unit-close" });
    refreshNow (controller);
    controller.applyPendingCameraList();
    check (fakecamera::getActiveRecordingCount() == 1, "the camera still plugged in keeps recording");
    check (controller.getPlannedFileNames().joinIntoString (",").startsWith ("V01_"),
           "unplugging the first camera moves the live numbering");
    check (controller.getPlannedFileNameFor ("unit-close") == juce::String (writing),
           "and the row still names the movie on the card, not V01");

    // Even while the movie is still finishing: the row then reads "Writes ...",
    // which is about the next take, and that take is numbered afresh.
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::DelayedSuccess);
    controller.stopRecording();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Waiting,
           "Stop leaves the movie finishing");
    check (controller.getPlannedFileNameFor ("unit-close").startsWith ("V01_"),
           "after Stop the row says what the next take will write");

    fakecamera::completePendingFinalizations();
    controller.pollRecordingFinalization();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
    fakecamera::setDevices ({});
    refreshNow (controller);
    takeFolder.deleteRecursively();
}

/// JUCE identifies two same-model cameras only by occurrence in the current
/// device list. Once that count changes, no occurrence can be safely mapped to
/// the physical camera that owned it at take start. Both writers must stop and
/// the whole ambiguous group must wait for the next take.
void aChangingSameNameGroupIsDeferredTogether()
{
    std::printf ("\nTwo same-name recording cameras become ambiguous\n");

    fakecamera::setDevices ({ "Twin Camera", "Twin Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both same-name cameras are discovered");
    if (cameras.size() != 2)
        return;

    for (const auto& camera : cameras)
        controller.getSelection().setEnabled (camera.id, true);

    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 2,
           "both selected previews are open before the take");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-ambiguity", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a second temporary take folder is available");
    check (controller.startRecording (takeFolder), "both same-name cameras start recording");
    check (fakecamera::getStartRecordingCallCount() == 2
               && fakecamera::getActiveRecordingCount() == 2,
           "both same-name camera writers are active");

    auto takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 2
               && takeStates[0].recording && takeStates[1].recording,
           "the frozen roster initially reports both writers");

    fakecamera::setDevices ({ "Twin Camera" });
    refreshNow (controller);
    controller.applyPendingCameraList(); // consume both synchronous didFinish callbacks
    check (namesFrom (controller).empty(),
           "a changed same-name count hides the entire ambiguous group");
    check (fakecamera::getLiveDeviceCount() == 0
               && fakecamera::getActiveRecordingCount() == 0,
           "both ambiguous camera devices and writers are closed");
    check (fakecamera::getStopRecordingCallCount() == 2,
           "each interrupted same-name movie is finalized exactly once");

    takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 2
               && ! takeStates[0].recording && ! takeStates[1].recording,
           "both watchdog-facing camera states remain lost");

    fakecamera::setDevices ({ "Twin Camera", "Twin Camera" });
    refreshNow (controller);
    check (namesFrom (controller).empty() && fakecamera::getOpenCallCount() == 2,
           "restoring the count cannot undo same-take identity ambiguity");

    controller.stopRecording();
    refreshNow (controller);
    check (namesFrom (controller).size() == 2,
           "the same-name group is advertised again after the take");
    check (fakecamera::getOpenCallCount() == 4 && fakecamera::getLiveDeviceCount() == 2,
           "both remembered previews reopen for the next take");
    check (fakecamera::getStartRecordingCallCount() == 2
               && fakecamera::getActiveRecordingCount() == 0,
           "neither reopened preview is misreported as recording");

    takeFolder.deleteRecursively();
}

/// A second camera of the same model plugged in mid-take. Nothing left: the
/// camera that was recording is still there and still recording, and closing
/// it loses the rest of a take that cannot be redone. The group may be held out
/// of the list for identity's sake, but the running writer must not be cut.
void aSameNameCameraArrivingMidTakeDoesNotStopTheOneRecording()
{
    std::printf ("\nA same-name camera plugged in while one is recording\n");

    fakecamera::setDevices ({ "Twin Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the one camera is discovered");
    if (cameras.size() != 1)
        return;

    controller.getSelection().setEnabled (cameras[0].id, true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-arrival", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a temporary take folder is available");
    check (controller.startRecording (takeFolder), "the camera starts recording");
    check (fakecamera::getActiveRecordingCount() == 1, "its writer is active");

    fakecamera::setDevices ({ "Twin Camera", "Twin Camera" });
    refreshNow (controller);
    controller.applyPendingCameraList();

    check (fakecamera::getActiveRecordingCount() == 1 && fakecamera::getStopRecordingCallCount() == 0,
           "the recording camera keeps recording when its twin arrives");
    const auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && states[0].recording,
           "and the watchdog still sees it recording");

    controller.stopRecording();
    controller.applyPendingCameraList();
    check (fakecamera::getStopRecordingCallCount() == 1, "its movie is finalized once, at Stop");

    takeFolder.deleteRecursively();
}

/// The twin plugged in mid-take and then unplugged again. The group went
/// 1 -> 2 -> 1: never below what the take began with, so the camera that was
/// recording is not shown to have left. Treating the drop back to one as a
/// departure closed the healthy recording the moment the twin was pulled.
void aSameNameTwinLeavingMidTakeDoesNotStopTheOneRecording()
{
    std::printf ("\nA same-name twin plugged in and pulled again while one is recording\n");

    fakecamera::setDevices ({ "Twin Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the one camera is discovered");
    if (cameras.size() != 1)
        return;

    controller.getSelection().setEnabled (cameras[0].id, true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-twin-leaves", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a temporary take folder is available");
    check (controller.startRecording (takeFolder), "the camera starts recording");

    fakecamera::setDevices ({ "Twin Camera", "Twin Camera" });
    refreshNow (controller);
    controller.applyPendingCameraList();
    check (fakecamera::getActiveRecordingCount() == 1 && fakecamera::getStopRecordingCallCount() == 0,
           "the recording camera keeps recording when its twin arrives");

    fakecamera::setDevices ({ "Twin Camera" });
    refreshNow (controller);
    controller.applyPendingCameraList();
    check (fakecamera::getActiveRecordingCount() == 1 && fakecamera::getStopRecordingCallCount() == 0,
           "and keeps recording when the twin is unplugged again");
    const auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && states[0].recording,
           "and the watchdog still sees it recording");

    controller.stopRecording();
    controller.applyPendingCameraList();
    check (fakecamera::getStopRecordingCallCount() == 1, "its movie is finalized once, at Stop");

    takeFolder.deleteRecursively();
}

/// Submitting startRecordingToFile is not proof that AVFoundation accepted the
/// movie. The UI stays STARTING until didStart, and the alignment offset is
/// measured at that callback rather than at the request call.
void recordingTruthWaitsForTheStartCallback()
{
    std::printf ("\nRecording state waits for the camera start callback\n");

    fakecamera::setDevices ({ "Slow-start Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setAutoConfirmRecordingStart (false);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Slow-start Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-slow-start", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a slow-start take folder is available");

    const auto audioStart = juce::Time::getMillisecondCounterHiRes() - 250.0;
    check (controller.startRecording (takeFolder, audioStart),
           "the camera start request is submitted");
    auto states = controller.getTakeCameraStates();
    check (! controller.isRecording() && states.size() == 1
               && states.front().starting && ! states.front().recording,
           "submitted but unconfirmed video reads STARTING, never REC");
    check (fakecamera::getPendingRecordingStartCount() == 1,
           "the simulator retains the delayed didStart fact");
    check (controller.getTakeVideoRecordsSoFar().empty(),
           "nor does the mid-take record name a movie no writer has confirmed");

    fakecamera::completePendingRecordingStarts();
    controller.applyPendingCameraList();
    states = controller.getTakeCameraStates();
    check (controller.isRecording() && states.size() == 1
               && ! states.front().starting && states.front().recording,
           "only didStart promotes the matching take generation to REC");
    check (controller.getTakeVideoRecordsSoFar().size() == 1,
           "and from didStart the mid-take record names its movie");

    controller.stopRecording();
    const auto inputs = controller.getCombinedTakeInputs();
    check (inputs.size() == 1 && inputs.front().videoStartOffsetSeconds >= 0.20,
           "camera alignment is measured at didStart, after the delayed request");

    takeFolder.deleteRecursively();
    fakecamera::setAutoConfirmRecordingStart (true);
}

/// didStart can arrive well after the movie's first frame. When the backend
/// says when that frame was, the offset is that time, not the callback's.
void startOffsetUsesTheBackendsFirstFrameTime()
{
    std::printf ("\nCamera start offset uses the reported first frame\n");

    fakecamera::setDevices ({ "Late-callback Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setAutoConfirmRecordingStart (false);

    const auto offsetFor = [] (double audioBeforeNowMs, double firstFrameAfterAudioMs)
    {
        mma::CameraController controller;
        refreshNow (controller);
        controller.getSelection().setEnabled ("Late-callback Camera", true);
        controller.applySelection (true);

        const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getNonexistentChildFile ("sobstage-camera-first-frame", {}, false);
        takeFolder.createDirectory();

        const auto audioStart = juce::Time::getMillisecondCounterHiRes() - audioBeforeNowMs;
        controller.startRecording (takeFolder, audioStart);
        fakecamera::completePendingRecordingStarts (firstFrameAfterAudioMs != 0.0
                                                        ? audioStart + firstFrameAfterAudioMs
                                                        : 0.0);
        controller.applyPendingCameraList();
        controller.stopRecording();
        const auto inputs = controller.getCombinedTakeInputs();
        takeFolder.deleteRecursively();
        return inputs.size() == 1 ? inputs.front().videoStartOffsetSeconds : -1.0;
    };

    // The callback lands 400 ms after audio; the first frame was at 120 ms.
    const auto reported = offsetFor (400.0, 120.0);
    check (std::abs (reported - 0.120) < 1.0e-6,
           "a reported first-frame time is used exactly, not the late callback");

    // No estimate (Windows): the callback's arrival is still the measure.
    const auto unreported = offsetFor (400.0, 0.0);
    check (unreported >= 0.39 && unreported < 5.0,
           "without an estimate the callback's arrival is used");

    // An estimate from the future is not believed.
    const auto future = offsetFor (400.0, 60000.0);
    check (future >= 0.39 && future < 5.0,
           "a first-frame time after the callback falls back to its arrival");

    // A first frame before the audio began clamps to the start of the take.
    const auto early = offsetFor (400.0, -50.0);
    check (early == 0.0, "a first frame before t=0 is clamped to zero, never negative");

    fakecamera::setAutoConfirmRecordingStart (true);
}

/// stopRecording must return immediately while AVFoundation closes the movie.
/// No manifest/combiner input is exposed and no second take is allowed until
/// the generation- and file-matched didFinish arrives.
void delayedFinalizationBlocksClaimsAndTheNextTake()
{
    std::printf ("\nA delayed camera movie finalization\n");

    fakecamera::setDevices ({ "Finishing Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::DelayedSuccess);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Finishing Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-delayed-finish", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a delayed-finalization folder is available");
    check (controller.startRecording (takeFolder), "the delayed writer starts");

    const auto before = std::chrono::steady_clock::now();
    controller.stopRecording();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);

    check (elapsed < std::chrono::milliseconds (250),
           "Stop returns without waiting for AVFoundation's movie finalizer");
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Waiting,
           "the controller explicitly waits for didFinish");
    check (controller.getTakeVideoRecords().empty()
               && controller.getCombinedTakeInputs().empty(),
           "metadata and combining cannot consume an unfinished movie");
    check (! controller.startRecording (takeFolder),
           "a rapid second take is rejected while the previous file is finishing");

    fakecamera::completePendingFinalizations();
    controller.pollRecordingFinalization();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Succeeded,
           "the matching didFinish releases the finalization gate");
    check (controller.getTakeVideoRecords().size() == 1
               && controller.getCombinedTakeInputs().size() == 1,
           "only the finalized movie becomes a manifest and combiner input");

    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
    check (controller.startRecording (takeFolder),
           "a new take may start once the prior generation is complete");
    controller.stopRecording();
    takeFolder.deleteRecursively();
}

/// A take folder renamed in Finder mid-take. AVFoundation's movie writer holds
/// its file open, so the movie goes on into the moved folder and finishes
/// there; the completion callback may name either the path it was started
/// with or where the file now is. Either must count as this camera's movie
/// finishing -- matching on the path made a moved movie look as if it never
/// finished, so the take waited out the 15 s timeout and then called a good
/// movie unusable. What the app consumes afterwards is the file NAME, joined
/// to the folder's current path, so nothing may carry the old folder.
void aTakeFolderRenamedMidTakeStillFinishesItsMovie()
{
    std::printf ("\nA take folder renamed in Finder while the camera records\n");

    for (const bool delayed : { false, true })
    {
        fakecamera::setDevices ({ "Moving Camera" });
        fakecamera::setOpenSucceeds (true);
        fakecamera::setViewerSucceeds (true);
        fakecamera::setAutoFrameOnListener (true);
        fakecamera::resetRecordingCallCounts();
        fakecamera::setFinalizationMode (delayed ? fakecamera::FinalizationMode::DelayedSuccess
                                                 : fakecamera::FinalizationMode::ImmediateSuccess);

        mma::CameraController controller;
        refreshNow (controller);
        controller.getSelection().setEnabled ("Moving Camera", true);
        controller.applySelection (true);

        const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getNonexistentChildFile ("sobstage-camera-renamed-take", {}, false);
        const auto takeFolder = scratch.getChildFile ("2026-10-06_1432_Kitchen");
        const auto movedFolder = scratch.getChildFile ("Kitchen (keep)");
        check (takeFolder.createDirectory().wasOk(), "a take folder is available");
        check (controller.startRecording (takeFolder), "the camera starts into the take folder");

        // Renamed while recording; the backend now reports the moved path.
        check (takeFolder.moveFileTo (movedFolder), "the folder is renamed mid-take");
        fakecamera::setReportedRecordingFolder (movedFolder);

        controller.stopRecording();
        if (delayed)
        {
            fakecamera::completePendingFinalizations();
            controller.pollRecordingFinalization();
        }

        const std::string how = delayed ? " (finished after Stop)" : " (finished during Stop)";
        check (controller.getRecordingFinalizationState()
                   == mma::CameraController::RecordingFinalizationState::Succeeded,
               "the moved movie's didFinish completes the take" + how);
        check (controller.getRecordingFinalizationProblem().isEmpty(),
               "and no camera is said to have failed to finish" + how);

        const auto records = controller.getTakeVideoRecords();
        const auto inputs = controller.getCombinedTakeInputs();
        check (records.size() == 1 && inputs.size() == 1,
               "the movie is in session.json's list and the combiner's inputs" + how);
        check (records.size() == 1 && juce::String (records.front().fileName).isNotEmpty()
                   && ! juce::String (records.front().fileName).containsAnyOf ("/\\")
                   && inputs.size() == 1
                   && inputs.front().videoFile == records.front().fileName
                   && movedFolder.getChildFile (juce::String (records.front().fileName)).existsAsFile(),
               "both name the file only, and it is found in the folder's new place" + how);

        scratch.deleteRecursively();
    }

    fakecamera::setReportedRecordingFolder ({});
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
}

/// Two cameras of one model. Settings were keyed "Name" / "Name #2" by OS
/// order, which is not a property of the camera: it changes across reboots
/// and replugs, and unplugging the first handed its settings to the second.
/// Where the platform gives each device its own id (macOS uniqueID), the
/// choices follow that, and choices remembered the old way are carried over.
void identicalCamerasKeepTheirOwnSettings()
{
    std::printf ("\nTwo identical cameras, told apart by their own ids\n");

    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();

    struct Remembered
    {
        std::string id;
        bool enabled;
        std::string name;
        mma::CameraQuality quality;
    };

    // What settings.json carries from one launch to the next.
    const auto rememberAll = [] (const mma::CameraController& c)
    {
        std::vector<Remembered> out;
        for (const auto& cam : c.getSelection().getKnownCameras())
            out.push_back ({ cam.id, c.getSelection().isEnabled (cam.id),
                             c.getSelection().getDisplayName (cam.id),
                             c.getSelection().getQuality (cam.id) });
        return out;
    };
    // How Application seeds a fresh launch before the first snapshot.
    const auto seed = [] (mma::CameraController& c, const std::vector<Remembered>& saved)
    {
        for (const auto& r : saved)
        {
            c.getSelection().setEnabled (r.id, r.enabled);
            c.getSelection().setQuality (r.id, r.quality);
            c.getSelection().setAssignedName (r.id, r.name);
        }
    };

    std::vector<Remembered> saved;

    {
        // Launch 1: Left is unit A, Right is unit B, and B is switched off.
        fakecamera::setDevices ({ "Twin Cam", "Twin Cam" }, { "unit-A", "unit-B" });
        mma::CameraController controller;
        refreshNow (controller);
        const auto cams = controller.getSelection().getAvailableCameras();
        check (cams.size() == 2 && cams[0].id == "unit-A" && cams[1].id == "unit-B",
               "each twin is known by its own device id");

        controller.getSelection().setAssignedName ("unit-A", "Left");
        controller.getSelection().setAssignedName ("unit-B", "Right");
        controller.getSelection().setQuality ("unit-B", mma::CameraQuality::HD720);
        controller.setCameraEnabledByUser ("unit-B", false);
        controller.applySelection (true);
        check (controller.getSignalState ("unit-A") != mma::CameraController::SignalState::NotOpen
                   && controller.getSignalState ("unit-B") == mma::CameraController::SignalState::NotOpen,
               "only the twin left on stays open");
        saved = rememberAll (controller);
    }

    {
        // Launch 2, after a reboot: the OS lists them the other way round.
        fakecamera::setDevices ({ "Twin Cam", "Twin Cam" }, { "unit-B", "unit-A" });
        fakecamera::resetOpenCallCount();
        mma::CameraController controller;
        seed (controller, saved);
        refreshNow (controller);
        controller.applySelection (true);

        const auto& sel = controller.getSelection();
        check (sel.getDisplayName ("unit-A") == "Left" && sel.isEnabled ("unit-A"),
               "reordered: unit A is still Left and still on");
        check (sel.getDisplayName ("unit-B") == "Right" && ! sel.isEnabled ("unit-B")
                   && sel.getQuality ("unit-B") == mma::CameraQuality::HD720,
               "reordered: unit B is still Right, off, at 720p");
        check (fakecamera::getOpenCallCount() == 1 && fakecamera::getOpenedDeviceIndices().size() == 1
                   && fakecamera::getOpenedDeviceIndices().front() == 1,
               "and the camera opened is unit A, now second in the list");

        // Unit A unplugged: B is now the only "Twin Cam" -- the very slot A had.
        fakecamera::setDevices ({ "Twin Cam" }, { "unit-B" });
        refreshNow (controller);
        controller.applySelection (false);
        check (sel.getDisplayName ("unit-B") == "Right" && ! sel.isEnabled ("unit-B")
                   && sel.getQuality ("unit-B") == mma::CameraQuality::HD720,
               "unplugging unit A hands none of its settings to unit B");
        check (sel.getDisplayName ("unit-A") == "Left" && sel.isEnabled ("unit-A"),
               "and unit A's are kept for when it comes back");
    }

    {
        // Settings from before per-device ids, for two twins: ambiguous, so
        // the OS order decides once, as it did; then the ids hold.
        fakecamera::setDevices ({ "Twin Cam", "Twin Cam" }, { "unit-B", "unit-A" });
        mma::CameraController controller;
        seed (controller, { { "Twin Cam", true, "Left", mma::CameraQuality::Best },
                            { "Twin Cam #2", false, "Right", mma::CameraQuality::HD720 } });
        refreshNow (controller);
        const auto& sel = controller.getSelection();
        check (sel.getDisplayName ("unit-B") == "Left" && sel.getDisplayName ("unit-A") == "Right"
                   && ! sel.isEnabled ("unit-A"),
               "old name-based settings for two twins are carried over in OS order");
        check (sel.getKnownCameras().size() == 2, "and the old entries do not linger as missing cameras");

        fakecamera::setDevices ({ "Twin Cam", "Twin Cam" }, { "unit-A", "unit-B" });
        refreshNow (controller);
        check (sel.getDisplayName ("unit-B") == "Left" && sel.getDisplayName ("unit-A") == "Right",
               "after which a reorder no longer swaps them");
    }

    {
        // One remembered camera of that name: no doubt which it was.
        fakecamera::setDevices ({ "Twin Cam" }, { "unit-B" });
        mma::CameraController controller;
        seed (controller, { { "Twin Cam #2", false, "Right", mma::CameraQuality::HD720 } });
        refreshNow (controller);
        const auto& sel = controller.getSelection();
        check (sel.getDisplayName ("unit-B") == "Right" && ! sel.isEnabled ("unit-B"),
               "a single old name-based entry is adopted by the camera of that name");
    }

    {
        // No per-device ids (Windows): the old name-and-order ids, unchanged.
        fakecamera::setDevices ({ "Twin Cam", "Twin Cam" });
        mma::CameraController controller;
        refreshNow (controller);
        const auto cams = controller.getSelection().getAvailableCameras();
        check (cams.size() == 2 && cams[0].id == "Twin Cam" && cams[1].id == "Twin Cam #2",
               "without device ids, twins are still told apart by order");
    }

    fakecamera::setDevices ({});
}

/// A twin opened by index must be the twin asked for. A hot-plug between the
/// background snapshot and the open can shift the index onto the other unit;
/// the names match, so only the device id can catch it.
void anIndexThatNowPointsAtTheOtherTwinIsNotUsed()
{
    std::printf ("\nA twin's index shifted onto the other twin before it opened\n");

    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::setDevices ({ "Twin Cam", "Twin Cam" }, { "unit-A", "unit-B" });

    mma::CameraController controller;
    controller.getSelection().setEnabled ("unit-A", false);
    controller.getSelection().setEnabled ("unit-B", true);

    // Held shut through the first snapshot, so the open below is the one
    // that meets the reordered list.
    fakecamera::setOpenSucceeds (false);
    refreshNow (controller);
    fakecamera::setOpenSucceeds (true);

    // The OS reorders after the snapshot the controller is using.
    fakecamera::setDevices ({ "Twin Cam", "Twin Cam" }, { "unit-B", "unit-A" });
    fakecamera::resetOpenCallCount();
    controller.applySelection (true);

    check (fakecamera::getOpenCallCount() == 1, "the stale index is tried once");
    check (controller.getSignalState ("unit-B") == mma::CameraController::SignalState::NotOpen,
           "but unit A's picture is not attached to unit B");

    // The fresh snapshot the mismatch asked for opens the right unit.
    check (controller.waitForCameraRefresh (2000), "a fresh camera list is requested and arrives");
    controller.applyPendingCameraList();
    controller.applySelection (false);
    check (controller.getSignalState ("unit-B") != mma::CameraController::SignalState::NotOpen
               && fakecamera::getOpenedDeviceIndices().back() == 0,
           "and unit B then opens at its new index");

    fakecamera::setDevices ({});
}

/// MAC-CAM-2: a camera unplugged right AFTER Stop, while AVFoundation still
/// owes its didFinish for the movie, must not be destroyed. Destroying it drops
/// the completion callback, so the take waited 15 s and then reported a movie
/// that was actually fine as failed. The device is parked like a mid-take unplug.
void anUnplugAfterStopStillReceivesItsFinalization()
{
    std::printf ("\nA camera unplugged right after Stop, before its movie finished\n");

    fakecamera::setDevices ({ "Unplugged Finisher" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::DelayedSuccess);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Unplugged Finisher", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-unplug-after-stop", {}, false);
    check (takeFolder.createDirectory().wasOk(), "an unplug-after-stop folder is available");
    check (controller.startRecording (takeFolder), "the writer starts");

    controller.stopRecording();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Waiting,
           "Stop leaves the movie finalization owed");

    fakecamera::setDevices ({});
    refreshNow (controller);
    controller.applyPendingCameraList();

    check (fakecamera::getPendingFinalizationCount() == 1,
           "the unplugged device is parked, so its owed didFinish is not dropped");

    fakecamera::completePendingFinalizations();
    controller.pollRecordingFinalization();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Succeeded,
           "the parked device's didFinish completes the take without a timeout");
    check (controller.getTakeVideoRecords().size() == 1,
           "the finished movie is kept");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the parked device is released once its movie is finished");

    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
    takeFolder.deleteRecursively();
}

/// A backend can report didFinish without ever reporting didStart. Even an
/// empty platform error cannot turn that into a successful zero-frame movie.
void finishWithoutStartIsAStartFailure()
{
    std::printf ("\nA camera finishes without ever confirming start\n");

    fakecamera::setDevices ({ "Rejected Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setAutoConfirmRecordingStart (false);
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Rejected Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-no-didstart", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a rejected-start folder is available");
    check (controller.startRecording (takeFolder), "the rejected start request is submitted");
    controller.stopRecording();

    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "didFinish without didStart fails closed even with no NSError");
    check (controller.getProblem().containsIgnoreCase ("confirmed recording started"),
           "the missing start confirmation is explained");
    check (controller.getTakeVideoRecords().empty()
               && controller.getCombinedTakeInputs().empty(),
           "the unconfirmed movie is never claimed as saved or combinable");

    takeFolder.deleteRecursively();
    fakecamera::setAutoConfirmRecordingStart (true);
}

/// DirectShow can fail while creating its file-capture graph even though
/// JUCE's public startRecordingToFile method returns void. The patched backend
/// publishes that rejection instead of letting the controller synthesize REC.
void aSynchronousWriterStartFailureNeverClaimsAFile()
{
    std::printf ("\nA synchronous camera writer fails to create its file graph\n");

    fakecamera::setDevices ({ "Failing DirectShow Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setStartRecordingSucceeds (false);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Failing DirectShow Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-sync-start-fail", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a synchronous-failure folder is available");
    check (controller.startRecording (takeFolder), "the camera writer request is submitted");

    const auto states = controller.getTakeCameraStates();
    check (! controller.isRecording() && states.size() == 1
               && ! states.front().recording && ! states.front().starting,
           "a rejected synchronous writer never displays REC or STARTING");
    check (controller.getProblem().containsIgnoreCase ("start recording video"),
           "the backend's start rejection is explained immediately");

    controller.stopRecording();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "the rejected writer remains failed at take finalization");
    check (controller.getTakeVideoRecords().empty()
               && controller.getCombinedTakeInputs().empty(),
           "no metadata or combine input is invented for the absent file");

    takeFolder.deleteRecursively();
    fakecamera::setStartRecordingSucceeds (true);
}

/// A writer that fails its finalization IMMEDIATELY, which is the common shape
/// of the failure and the one nothing here had ever produced.
///
/// FinalizationMode::ImmediateError existed in the camera stand-in and no
/// scenario used it. That matters beyond coverage: when every didFinish has
/// already arrived by the time stopRecording returns, isFinalizingRecording()
/// is false, and Application's stop took its SYNCHRONOUS branch -- which never
/// read getRecordingFinalizationProblem(). So the one sentence warning that a
/// movie is unusable was produced and dropped precisely when it was true, and
/// the app said "Saved to ..." over the top of it.
///
/// This pins the state that made that possible: finalization already finished,
/// and a problem waiting to be read. Application now reads it on both paths.
void anImmediateFinalizationErrorIsReadyBeforeTheStopReturns()
{
    std::printf ("\nA camera writer which fails its finalization at once\n");

    fakecamera::setDevices ({ "Failing Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateError);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Failing Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-immediate-error", {}, false);
    check (takeFolder.createDirectory().wasOk(), "an immediate-error folder is available");
    check (controller.startRecording (takeFolder), "the writer begins");

    controller.stopRecording();

    // The two halves of the hazard, asserted together. Either alone is
    // harmless; it is the combination that the old stop path threw away.
    check (! controller.isFinalizingRecording(),
           "finalization is already over when stopRecording returns");
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "and it ended in failure");
    check (controller.getRecordingFinalizationProblem().isNotEmpty(),
           "with a problem waiting to be read, on the path that used to drop it");
    check (controller.getRecordingFinalizationProblem().containsIgnoreCase ("do not use"),
           "and it tells the user not to trust the movie");
    check (controller.getTakeVideoRecords().empty(),
           "and no movie is claimed for the take");
    check (controller.getTakeVideoRecordsSoFar().empty(),
           "not even by the mid-take record: a failed writer is no more usable after a crash");

    takeFolder.deleteRecursively();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
}

/// A missing didFinish is bounded. Once timed out, the unsafe device generation
/// stays closed through periodic discovery and only an explicit retry can open
/// it again.
void aNeverFinishingWriterFailsClosedAndDoesNotAutoReopen()
{
    std::printf ("\nA camera writer which never finishes\n");

    fakecamera::setDevices ({ "Wedged Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::Never);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Wedged Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-never-finish", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a never-finalization folder is available");
    check (controller.startRecording (takeFolder), "the wedged writer begins");
    controller.stopRecording();
    check (controller.isFinalizingRecording()
               && fakecamera::getPendingFinalizationCount() == 1,
           "the missing callback leaves one bounded finalizer pending");

    controller.advanceSignalClockForTesting (15001.0);
    controller.pollRecordingFinalization();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "the 15-second deadline ends in an explicit failed state");
    check (controller.getRecordingFinalizationProblem().containsIgnoreCase ("did not finish")
               && controller.getTakeVideoRecords().empty()
               && controller.getTakeVideoRecordsSoFar().empty(),
           "the audio-safe/video-unsafe outcome is reported without a movie claim");
    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLiveDeviceCount() == 0,
           "the timed-out platform device is closed and not immediately reopened");

    refreshNow (controller);
    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLiveDeviceCount() == 0,
           "periodic topology refresh cannot reuse the unsafe generation");

    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 1,
           "an explicit retry can open a fresh camera generation");

    takeFolder.deleteRecursively();
}

/// Destruction cannot join a platform movie callback. The fake retains the
/// delayed callback by raw device pointer, so this also proves teardown removes
/// it rather than allowing a late use-after-free.
void shutdownDuringFinalizationIsBoundedAndLifetimeSafe()
{
    std::printf ("\nShutdown while a camera movie is still finalizing\n");

    fakecamera::setDevices ({ "Shutdown Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::DelayedSuccess);

    auto controller = std::make_unique<mma::CameraController>();
    refreshNow (*controller);
    controller->getSelection().setEnabled ("Shutdown Camera", true);
    controller->applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-shutdown-finish", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a shutdown-finalization folder is available");
    check (controller->startRecording (takeFolder), "the shutdown writer starts");
    controller->stopRecordingForShutdown();
    check (controller->isFinalizingRecording(), "shutdown observes the unfinished movie");

    const auto before = std::chrono::steady_clock::now();
    controller.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);
    check (elapsed < std::chrono::milliseconds (250),
           "controller destruction never joins a delayed movie callback");
    check (fakecamera::getPendingFinalizationCount() == 0,
           "device destruction removes the callback target before it can fire late");

    fakecamera::completePendingFinalizations();
    takeFolder.deleteRecursively();
}

} // namespace

/// macOS 27 took the app down about a second after launch while it started a
/// remembered camera, and since the camera was remembered, every launch did it
/// again. A launch that went down while starting a camera must leave that one
/// switched off-in-effect next time, with the sound still working.
void aCrashWhileStartingACameraDoesNotRepeatEveryLaunch()
{
    std::printf ("\nThe app went down while starting a camera\n");

    fakecamera::setDevices ({ "Crashy Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetOpenCallCount();

    const auto guard = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getNonexistentChildFile ("sobstage-camera-guard", ".txt", false);
    juce::String leftBehind;

    {
        mma::CameraController controller;
        controller.setStartupGuardFile (guard);
        refreshNow (controller);
        controller.getSelection().setEnabled ("Crashy Camera", true);
        controller.applySelection();
        check (fakecamera::getOpenCallCount() == 1, "the remembered camera is started at launch");
        check (guard.existsAsFile() && guard.loadFileAsString().contains ("Crashy Camera"),
               "while it is starting, the guard file names it");

        // What a crash leaves on disk; a clean quit (below) deletes it.
        leftBehind = guard.loadFileAsString();
    }

    check (! guard.existsAsFile(), "a clean quit clears the guard");
    guard.replaceWithText (leftBehind);

    {
        mma::CameraController controller;
        controller.setStartupGuardFile (guard);
        refreshNow (controller);
        controller.getSelection().setEnabled ("Crashy Camera", true);
        controller.applySelection();
        controller.applySelection();
        check (fakecamera::getOpenCallCount() == 1,
               "the next launch does not start the camera it went down on");
        check (controller.getProblem().containsIgnoreCase ("closed unexpectedly while starting Crashy Camera")
                   && controller.getProblem().containsIgnoreCase ("off and back on"),
               "and says so, with the way back");

        // Opening the Cameras panel is an explicit retry for every other
        // camera, but it is also the only way to reach this camera's switch.
        controller.applySelection (true);
        check (fakecamera::getOpenCallCount() == 1,
               "opening the Cameras panel does not start the held camera");
        check (controller.getProblem().containsIgnoreCase ("closed unexpectedly while starting Crashy Camera"),
               "and the panel still says why it is off");

        // Switching a different camera on is not a retry of this one.
        fakecamera::setDevices ({ "Crashy Camera", "Other" });
        refreshNow (controller);
        controller.setCameraEnabledByUser ("Other", true);
        controller.applySelection (true);
        check (fakecamera::getOpenCallCount() == 2,
               "switching another camera on starts only that camera");
        check (controller.getProblem().containsIgnoreCase ("closed unexpectedly while starting Crashy Camera"),
               "and the held camera stays held");

        // If this launch went down now, the next one must still hold it.
        check (guard.loadFileAsString().contains ("Crashy Camera"),
               "the hold is kept on disk while this launch runs");
    }

    // The user quit without trying the camera again.
    check (guard.existsAsFile() && guard.loadFileAsString().contains ("Crashy Camera")
               && ! guard.loadFileAsString().contains ("Other"),
           "a clean quit keeps the hold for a camera that was never retried");
    fakecamera::setDevices ({ "Crashy Camera" });

    {
        mma::CameraController controller;
        controller.setStartupGuardFile (guard);
        refreshNow (controller);
        controller.getSelection().setEnabled ("Crashy Camera", true);
        controller.applySelection();
        check (fakecamera::getOpenCallCount() == 2,
               "the launch after that still does not start it");
        check (controller.getProblem().containsIgnoreCase ("closed unexpectedly while starting Crashy Camera"),
               "and still says why");

        controller.setCameraEnabledByUser ("Crashy Camera", false);
        controller.applySelection (true);
        check (fakecamera::getOpenCallCount() == 2, "turning it off starts nothing");
        check (! controller.getProblem().containsIgnoreCase ("closed unexpectedly"),
               "and a camera that is off has nothing to explain");

        controller.setCameraEnabledByUser ("Crashy Camera", true);
        controller.applySelection (true);
        check (fakecamera::getOpenCallCount() == 3, "turning it back on starts it again");
        check (! controller.getProblem().containsIgnoreCase ("closed unexpectedly"),
               "and the explanation goes");

        fakecamera::emitFrame ("Crashy Camera");
        controller.applyPendingCameraList();
        check (! guard.existsAsFile(), "a first frame proves the start and clears the guard");
    }

    {
        mma::CameraController controller;
        controller.setStartupGuardFile (guard);
        refreshNow (controller);
        controller.getSelection().setEnabled ("Crashy Camera", true);
        controller.applySelection();
        check (fakecamera::getOpenCallCount() == 4, "after a clean launch it starts automatically again");
        check (guard.existsAsFile(), "an HDMI card with no signal is guarded while it starts");
        controller.advanceSignalClockForTesting (10001.0);
        check (! guard.existsAsFile(), "and cleared once it has stayed up ten seconds without a frame");
    }

    guard.deleteFile();
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("CameraController, driven against a virtual camera layer\n");
    std::printf ("======================================================\n");

    aCameraArrivingAndLeavingMovesTheList();
    aStuckDiscoveryCannotHoldControllerLifetime();
    aSupersededDiscoveryCannotPublishItsStaleSnapshot();
    periodicPollingCannotStarveASlowDiscovery();
    twoOfTheSameModelStayApart();
    aCameraThatComesBackKeepsItsIdentity();
    aFailedOpenWaitsForAnExplicitRetry();
    aReorderedListCannotOpenTheWrongCamera();
    topologyConsumptionOwnsOpenDeviceReconciliation();
    anEnabledMissingCaptureCardStaysVisibleAsAProblem();
    aRememberedCameraWaitsForTheFirstSnapshot();
    aTakeBeforeFirstDiscoveryKeepsTheCameraPlanHonest();
    aCameraMissingAtTakeStartCannotJoinMidTake();
    oneNativeViewerMovesBetweenScreens();
    aFailedViewerCanRecoverWithTheSameId();
    anOpenedCaptureCardWithoutAFrameIsNotRecordable();
    aCameraThatCannotStartIsNamedNotCounted();
    aLateFirstFrameRecoversAfterTheSignalTimeout();
    anAllBlackPictureIsExplainedNotShown();
    aCameraWaitsForPrivacyPermission();
    aCameraQualityCapsTheOpenAndReportsTheFormat();
    aStaleGenerationFrameCannotCertifyAReopenedCamera();
    aRuntimeCameraErrorInvalidatesThePreview();
    switchingOffARecordingCameraWaitsForTheTakeToEnd();
    aRuntimeFailureRetriesAfterTheTakeEnds();
    aRecordedCameraThatReconnectsWaitsForTheNextTake();
    theRecordingToNameIsTheFileBeingWritten();
    aChangingSameNameGroupIsDeferredTogether();
    aSameNameCameraArrivingMidTakeDoesNotStopTheOneRecording();
    aSameNameTwinLeavingMidTakeDoesNotStopTheOneRecording();
    recordingTruthWaitsForTheStartCallback();
    startOffsetUsesTheBackendsFirstFrameTime();
    delayedFinalizationBlocksClaimsAndTheNextTake();
    aTakeFolderRenamedMidTakeStillFinishesItsMovie();
    identicalCamerasKeepTheirOwnSettings();
    anIndexThatNowPointsAtTheOtherTwinIsNotUsed();
    anUnplugAfterStopStillReceivesItsFinalization();
    finishWithoutStartIsAStartFailure();
    aSynchronousWriterStartFailureNeverClaimsAFile();
    anImmediateFinalizationErrorIsReadyBeforeTheStopReturns();
    aNeverFinishingWriterFailsClosedAndDoesNotAutoReopen();
    shutdownDuringFinalizationIsBoundedAndLifetimeSafe();
    aCrashWhileStartingACameraDoesNotRepeatEveryLaunch();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
