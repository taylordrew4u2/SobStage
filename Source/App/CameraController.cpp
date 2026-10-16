#include "CameraController.h"
#include <algorithm>
#include <chrono>
#include <set>

#if JUCE_WINDOWS && JUCE_USE_CAMERA
#include <objbase.h>
#endif

namespace mma {

namespace {

#if JUCE_USE_CAMERA
constexpr double kCameraSignalTimeoutMs = 5000.0;

/// macOS camera privacy that must hold an open back: not yet answered, or no.
bool cameraAccessBlocks (PermissionState state)
{
    return state == PermissionState::NotYetRequested || state == PermissionState::Denied;
}

// How long a camera must stay open before a crash is no longer blamed on
// starting it (the macOS 27 start race took the app down within a second).
constexpr double kStartupGuardMs = 10000.0;
constexpr double kCameraFinalizationTimeoutMs = 15000.0;

#if JUCE_MAC || JUCE_WINDOWS || defined(SOBSTAGE_CAMERA_SIMULATION)
constexpr bool kCameraBackendReportsFinalization = true;
#else
constexpr bool kCameraBackendReportsFinalization = false;
#endif

/// CameraDevice::Listener may run on a platform capture thread. It publishes
/// one tiny generation-tagged fact and never reaches back into the controller
/// or UI. That makes an already-running callback harmless after a close/reopen:
/// the message-thread drain rejects it unless the exact device generation is
/// still current.
class FrameHeartbeatListener final : public juce::CameraDevice::Listener
{
public:
    explicit FrameHeartbeatListener (std::function<void (bool black)> publishIn)
        : publish (std::move (publishIn))
    {
    }

    void imageReceived (const juce::Image& image) override
    {
        if (image.isValid())
            publish (isBlack (image));
    }

private:
    std::function<void (bool black)> publish;

    /// A "USB2 Video" HDMI dongle with no signal, an unsupported input mode or
    /// an HDCP-protected source keeps streaming perfectly valid frames that
    /// are entirely black. Sample a coarse grid: a dark stage still has some
    /// pixel above near-black, a dead input has none.
    static bool isBlack (const juce::Image& image)
    {
        constexpr int kGrid = 16;
        constexpr juce::uint8 kNearBlack = 24; // above video-range black (16)
        const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);

        for (int gy = 0; gy < kGrid; ++gy)
            for (int gx = 0; gx < kGrid; ++gx)
            {
                const int x = juce::jmin (pixels.width - 1, (2 * gx + 1) * pixels.width / (2 * kGrid));
                const int y = juce::jmin (pixels.height - 1, (2 * gy + 1) * pixels.height / (2 * kGrid));
                const auto colour = pixels.getPixelColour (x, y);

                if (juce::jmax (colour.getRed(), colour.getGreen(), colour.getBlue()) > kNearBlack)
                    return false;
            }

        return true;
    }
};
#endif

/// A disposable UI owner for the one native preview component created when a
/// camera opens. Adding the native child here automatically removes it from a
/// previous host, so moving between the Cameras panel and the main screen does
/// not destroy/recreate AVFoundation's preview layer or restart its session.
#if JUCE_USE_CAMERA
class CameraViewerHost final : public juce::Component
{
public:
    explicit CameraViewerHost (std::shared_ptr<std::atomic<juce::Component*>> targetIn)
        : target (std::move (targetIn))
    {
        if (auto* viewer = target->load())
            addAndMakeVisible (*viewer);
    }

    void resized() override
    {
        if (auto* viewer = target->load(); viewer != nullptr && viewer->getParentComponent() == this)
            viewer->setBounds (getLocalBounds());
    }

private:
    std::shared_ptr<std::atomic<juce::Component*>> target;
};

/// Seconds after the audio's t=0 that a camera's movie begins.
///
/// `firstFrameMs` is the backend's estimate of when the movie's first frame
/// was captured, on the same high-resolution counter as `audioStartMs`. The
/// macOS backend takes it from the movie output itself (the callback's own
/// time less what the writer had already recorded), so a didStart delivered
/// late -- a busy delegate queue, a slow USB start -- no longer pushes the
/// picture later than it really is. Zero means the backend cannot tell; the
/// callback's arrival is then the best evidence there is. An estimate from the
/// future, or from before the movie could have been asked for, is not
/// believed either.
double cameraStartOffsetSeconds (double audioStartMs, double firstFrameMs, double callbackMs)
{
    if (audioStartMs <= 0.0)
        return 0.0;

    const bool plausible = firstFrameMs > 0.0 && firstFrameMs <= callbackMs
                        && callbackMs - firstFrameMs < kStartupGuardMs;
    const auto startedMs = plausible ? firstFrameMs : callbackMs;
    return juce::jmax (0.0, (startedMs - audioStartMs) / 1000.0);
}
#endif

} // namespace

CameraController::CameraController() = default;

CameraController::~CameraController()
{
#if JUCE_USE_CAMERA
    // Suppress a late platform result before touching message-thread-owned
    // camera state. Enumeration itself is deliberately not joined: an OS
    // camera service can remain wedged forever, and the worker owns no part of
    // this controller which could become dangling after destruction.
    const auto discovery = discoveryState;
    {
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        discovery->cancelled = true;
        discovery->pendingDeviceNames.clear();
        discovery->pendingDeviceIds.clear();
    }
    discovery->condition.notify_all();

    stopRecordingInternal (false);

    for (auto& [id, entry] : open)
    {
        juce::ignoreUnused (id);
        if (entry.device != nullptr && entry.frameListener != nullptr)
            entry.device->removeListener (entry.frameListener.get());
        entry.viewerTarget->store (nullptr);
    }

    open.clear();

    // A clean quit: nothing was left half-started. A camera still held from a
    // crashed launch stays named until the user switches it back on.
    startingGuardIds.clear();
    writeStartupGuard();
#else
    stopRecording();
#endif
}

void CameraController::setStartupGuardFile (const juce::File& file)
{
#if JUCE_USE_CAMERA
    startupGuardFile = file;
    crashedWhileStartingIds.clear();

    if (file.existsAsFile())
    {
        juce::StringArray lines;
        lines.addLines (file.loadFileAsString());

        for (const auto& line : lines)
            if (line.trim().isNotEmpty())
                crashedWhileStartingIds.insert (line.trim().toStdString());

        // Left on disk: the hold lasts until the user switches that camera back
        // on, not just for this one launch. writeStartupGuard() keeps it there.
    }
#else
    juce::ignoreUnused (file);
#endif
}

void CameraController::setCameraPermission (std::function<PermissionState()> probe,
                                            std::function<void()> request,
                                            std::function<bool()> restricted)
{
#if JUCE_USE_CAMERA
    cameraPermissionProbe = std::move (probe);
    requestCameraPermission = std::move (request);
    cameraAccessRestrictedProbe = std::move (restricted);
    cameraPermissionRequested = false;
#else
    juce::ignoreUnused (probe, request, restricted);
#endif
}

void CameraController::setCameraEnabledByUser (const std::string& id, bool enabled)
{
    selection.setEnabled (id, enabled);

#if JUCE_USE_CAMERA
    // Switching it on is the user's own retry of a camera the last launch went
    // down on. The applySelection (true) that follows opens it, and openCamera()
    // guards that start afresh.
    if (enabled && crashedWhileStartingIds.erase (id) > 0)
        writeStartupGuard();
#endif
}

#if JUCE_USE_CAMERA
void CameraController::setStartingGuard (const std::string& id, bool starting)
{
    if (startupGuardFile == juce::File())
        return;

    const bool changed = starting ? startingGuardIds.insert (id).second
                                  : startingGuardIds.erase (id) > 0;
    if (changed)
        writeStartupGuard();
}

void CameraController::writeStartupGuard()
{
    if (startupGuardFile == juce::File())
        return;

    // Both the cameras being started now and the ones still held from a crashed
    // launch: a crash (or a clean quit) before the user retries a held camera
    // must not let the launch after it start that camera unattended.
    std::set<std::string> guarded (startingGuardIds);
    guarded.insert (crashedWhileStartingIds.begin(), crashedWhileStartingIds.end());

    if (guarded.empty())
    {
        startupGuardFile.deleteFile();
        return;
    }

    juce::StringArray lines;
    for (const auto& id : guarded)
        lines.add (juce::String (id));

    // Written before the OS call it guards, so a crash inside that call still
    // leaves the name behind. Best effort: a read-only disk only loses the guard.
    startupGuardFile.getParentDirectory().createDirectory();
    startupGuardFile.replaceWithText (lines.joinIntoString ("\n") + "\n");
}
#endif

bool CameraController::isSupported() const
{
#if JUCE_USE_CAMERA
    return true;
#else
    return false;
#endif
}

juce::String CameraController::getUnavailableReason() const
{
    if (isSupported())
        return {};

    // §10.6: name what happened and what to do about it. There is nothing to
    // do about it here, so the sentence says that rather than implying the
    // user has a setting to find.
    return "This build can't use cameras. Camera recording is available in the "
           "macOS and Windows builds; the sound recording works either way.";
}

void CameraController::refreshCameras()
{
#if JUCE_USE_CAMERA
    applyPendingCameraList();
    requestDiscovery (true);
#else
    // Keep the unsupported build's selection consistently empty.
    selection.setAvailableCameras ({});
#endif
}

void CameraController::refreshCamerasIfIdle()
{
#if JUCE_USE_CAMERA
    applyPendingCameraList();
    requestDiscovery (false);
#else
    // Keep the unsupported build's selection consistently empty.
    selection.setAvailableCameras ({});
#endif
}

bool CameraController::applyPendingCameraList()
{
#if JUCE_USE_CAMERA
    const bool runtimeStateChanged = applyPendingRuntimeEvents();
    const bool signalStateChanged = applySignalTimeouts();
    bool finalizationStateChanged = applyRecordingFinalizationTimeout();
    finalizationStateChanged = finishRecordingFinalizationIfReady()
                            || finalizationStateChanged;
    juce::StringArray names, ids;
    bool hasNewDeviceList = false;
    const auto discovery = discoveryState;
    {
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        if (! discovery->cancelled && discovery->completed > discoveryApplied)
        {
            names = discovery->pendingDeviceNames;
            ids = discovery->pendingDeviceIds;
            discoveryApplied = discovery->completed;
            hasNewDeviceList = true;
        }
    }

    if (hasNewDeviceList)
    {
        hasAppliedDeviceList = true;
        applyDeviceNames (names, ids);
    }

    // The user answered the camera prompt (or allowed SobStage in System
    // Settings): open the cameras that were held back for it.
    const bool permissionArrived = waitingForCameraPermission && cameraPermissionProbe != nullptr
        && ! cameraAccessBlocks (cameraPermissionProbe());

    // Applying a topology snapshot and reconciling the devices it owns are one
    // operation. In particular, the Cameras panel may be visible, in which case
    // the outer UI deliberately does not call applySelection(). Leaving cleanup
    // to that caller kept an unplugged CameraDevice alive and allowed a same-name
    // reconnect to reuse the stale object indefinitely.
    if (hasNewDeviceList || runtimeStateChanged || signalStateChanged || permissionArrived)
        applySelection (false);

    return hasNewDeviceList || runtimeStateChanged || signalStateChanged
        || finalizationStateChanged || permissionArrived;
#else
    return false;
#endif
}

bool CameraController::waitForCameraRefresh (int timeoutMilliseconds)
{
#if JUCE_USE_CAMERA
    const auto discovery = discoveryState;
    std::unique_lock<std::mutex> lock (discovery->mutex);
    const auto requested = discovery->requested;
    if (requested == 0)
        return false;

    const bool signalled = discovery->condition.wait_for (
        lock, std::chrono::milliseconds (juce::jmax (0, timeoutMilliseconds)),
        [discovery, requested]
        {
            return discovery->cancelled
                || discovery->completed >= requested
                || ! discovery->workerRunning;
        });
    const bool completed = discovery->completed >= requested;
    const bool cancelled = discovery->cancelled;
    lock.unlock();

    return signalled && completed && ! cancelled && applyPendingCameraList();
#else
    juce::ignoreUnused (timeoutMilliseconds);
    return false;
#endif
}

bool CameraController::hasLookedForCameras() const noexcept
{
#if JUCE_USE_CAMERA
    return hasAppliedDeviceList;
#else
    return true;
#endif
}

bool CameraController::isInitialDiscoveryPending() const noexcept
{
#if JUCE_USE_CAMERA
    if (hasAppliedDeviceList)
        return false;

    // Discovery is deliberately off-thread because AVFoundation/DirectShow
    // can hang. Give the ordinary first snapshot a short chance to protect a
    // remembered camera from an empty take, but never let one wedged driver
    // block audio recording forever. The remembered row stays visible and can
    // also be switched off during this grace period.
    constexpr double kInitialDiscoveryGraceMs = 3000.0;
    return initialDiscoveryRequestedAtMs <= 0.0
        || juce::Time::getMillisecondCounterHiRes() - initialDiscoveryRequestedAtMs
               < kInitialDiscoveryGraceMs;
#else
    return false;
#endif
}

#if JUCE_USE_CAMERA
bool CameraController::applyPendingRuntimeEvents()
{
    std::vector<RuntimeCameraError> errors;
    std::vector<FrameNotification> frames;
    std::vector<RecordingStartedNotification> recordingsStarted;
    std::vector<RecordingFinishedNotification> recordingsFinished;
    std::vector<FormatNotification> formats;
    {
        const std::lock_guard<std::mutex> guard (runtimeErrorMailbox->mutex);
        formats.swap (runtimeErrorMailbox->formats);
        errors.swap (runtimeErrorMailbox->pending);
        frames.swap (runtimeErrorMailbox->frames);
        recordingsStarted.swap (runtimeErrorMailbox->recordingsStarted);
        recordingsFinished.swap (runtimeErrorMailbox->recordingsFinished);
    }

    bool changed = false;

    for (const auto& format : formats)
    {
        const auto entry = open.find (format.id);
        if (entry == open.end() || entry->second.viewerRevision != format.viewerRevision)
            continue;

        juce::String text;
        if (format.width > 0 && format.height > 0)
        {
            text = juce::String (format.width) + " x " + juce::String (format.height);
            if (format.fps > 0.0)
                text += ", " + juce::String (juce::roundToInt (format.fps)) + " fps";
        }

        if (text != entry->second.activeFormatText)
        {
            entry->second.activeFormatText = text;
            changed = true;
        }
    }

    // A take has at most one writer per camera, and the generation check below
    // ties the callback to this take, so the camera id is the whole match. The
    // path is deliberately not compared: a take folder renamed or moved in
    // Finder mid-take leaves the movie writing on into the moved folder, and
    // a backend that reports where the file now is must not be mistaken for
    // one that never finished (which waited out the 15 s timeout and then
    // called a good movie unusable).
    for (const auto& started : recordingsStarted)
    {
        const auto take = std::find_if (takeRecordings.begin(), takeRecordings.end(),
            [&] (const TakeRecording& recording)
            {
                return recording.deviceId == started.id;
            });

        if (take == takeRecordings.end() || started.takeGeneration != takeGeneration
            || take->finalizationComplete || take->started)
            continue;

        take->started = true;
        take->startOffsetSeconds = started.startOffsetSeconds;
        startingCameraIds.erase (started.id);

        // didStart may legitimately arrive after Stop was pressed. It still
        // proves that this movie must be awaited and can be used if didFinish
        // succeeds, but it must not put the stopped take back into REC.
        if (takeActive)
        {
            recordingCameraIds.insert (started.id);
            recording = true;
        }

        const auto entry = open.find (started.id);
        if (entry != open.end() && entry->second.viewerRevision == started.viewerRevision)
        {
            entry->second.startingThisTake = false;
            entry->second.recordingThisTake = takeActive;
        }

        changed = true;
    }

    for (const auto& frame : frames)
    {
        const auto entry = open.find (frame.id);

        // A frame from the device generation which just closed must never make
        // a same-id replacement look live. This is especially important for
        // capture cards, whose product string and therefore app id rarely
        // changes across a USB reconnect.
        if (entry == open.end() || entry->second.viewerRevision != frame.viewerRevision)
            continue;

        const bool becameLive = ! entry->second.firstFrameReceived
                             || entry->second.signalTimedOut;
        entry->second.firstFrameReceived = true;
        entry->second.signalTimedOut = false;
        setStartingGuard (frame.id, false);
        entry->second.lastFrameAtMs = signalClockMs();
        openFailures.erase (frame.id);

        // Black proofs keep the signal alive (the device IS streaming) but a
        // black rectangle with no words is what the user saw. After a short
        // run of them, swap the preview for a placeholder that says why; the
        // first real picture brings the preview straight back.
        entry->second.blackFramesInARow = frame.black ? entry->second.blackFramesInARow + 1 : 0;
        const bool pictureBlack = entry->second.blackFramesInARow >= kBlackFramesBeforeWarning;
        const bool blackChanged = pictureBlack != entry->second.pictureBlack;
        entry->second.pictureBlack = pictureBlack;

        // Keep this generation's listener attached. SobStage's pinned-JUCE
        // patch throttles the proof capture to two frames per second, which is
        // cheap enough to detect HDMI loss/recovery without the old unbounded
        // still-photo loop contending with the movie writer.
        if (becameLive || blackChanged)
        {
            // Device generation stays fixed for callback validation. The public
            // revision is only the UI cache token for placeholder/live changes.
            ++viewerRevisions[frame.id];
            changed = true;
        }
    }

    for (const auto& finished : recordingsFinished)
    {
        const auto take = std::find_if (takeRecordings.begin(), takeRecordings.end(),
            [&] (const TakeRecording& recording)
            {
                return recording.deviceId == finished.id; // see the didStart match above
            });

        if (take == takeRecordings.end() || finished.takeGeneration != takeGeneration
            || take->finalizationComplete)
            continue;

        take->finalizationComplete = true;
        take->finalizationError = finished.error;

        // didFinish is required even for a start which AVFoundation rejected.
        // A backend may provide no NSError in that edge case; the missing
        // didStart is itself enough evidence that no usable movie began.
        if (! take->started)
        {
            if (take->finalizationError.isEmpty())
                take->finalizationError = "The camera ended before it confirmed recording started";

            recordProblem = "Couldn't start recording video from "
                          + juce::String (take->displayName) + ": "
                          + take->finalizationError + ".";
        }

        const auto openEntry = open.find (finished.id);
        if (openEntry != open.end()
            && openEntry->second.viewerRevision == finished.viewerRevision)
        {
            openEntry->second.recordingThisTake = false;
            openEntry->second.startingThisTake = false;
            openEntry->second.device->onRecordingStarted = nullptr;
            openEntry->second.device->onRecordingFinished = nullptr;
        }

        finalizingDevices.erase (
            std::remove_if (finalizingDevices.begin(), finalizingDevices.end(),
                [&] (const FinalizingDevice& device)
                {
                    return device.id == finished.id
                        && device.viewerRevision == finished.viewerRevision
                        && device.takeGeneration == finished.takeGeneration;
                }),
            finalizingDevices.end());

        recordingCameraIds.erase (finished.id);
        startingCameraIds.erase (finished.id);
        recording = ! recordingCameraIds.empty();
        changed = true;
    }

    for (const auto& error : errors)
    {
        const auto entry = open.find (error.id);

        // A callback can arrive after an intentional close/reopen. Only the
        // exact CameraDevice generation which emitted it may invalidate itself.
        if (entry == open.end() || entry->second.viewerRevision != error.viewerRevision)
            continue;

        const auto name = juce::String (selection.getDisplayName (error.id));
        closeCamera (error.id);
        openFailures[error.id] = "Lost video from " + name
                               + (error.message.isNotEmpty() ? ": " + error.message : juce::String())
                               + ". Check the camera or capture-device signal and cables, close any "
                                 "other app using it, then turn this camera off and back on.";
        changed = true;
    }

    return changed;
}

double CameraController::signalClockMs() const noexcept
{
    auto now = juce::Time::getMillisecondCounterHiRes();
#if defined(SOBSTAGE_CAMERA_SIMULATION)
    now += signalClockOffsetForTesting;
#endif
    return now;
}

bool CameraController::applySignalTimeouts()
{
    bool changed = false;
    const auto now = signalClockMs();

    for (auto& [id, entry] : open)
    {
        // Open this long without taking the app down: it started. (An HDMI
        // card with no signal never sends a frame, and that is not a crash.)
        if (now - entry.openedAtMs >= kStartupGuardMs)
            setStartingGuard (id, false);

        const auto lastProofAt = entry.firstFrameReceived ? entry.lastFrameAtMs
                                                          : entry.openedAtMs;

        if (entry.signalTimedOut || now - lastProofAt < kCameraSignalTimeoutMs)
            continue;

        entry.signalTimedOut = true;
        openFailures[id] = "No video frames arrived from "
                         + juce::String (selection.getDisplayName (id))
                         + ". Check that the camera or capture device is on. For HDMI, check its "
                           "output plus the capture card's HDMI and USB cables. Then turn this "
                           "camera off and back on.";
        ++viewerRevisions[id];
        changed = true;
    }

    return changed;
}

bool CameraController::applyRecordingFinalizationTimeout()
{
    if (recordingFinalizationState != RecordingFinalizationState::Waiting
        || signalClockMs() < recordingFinalizationDeadlineMs)
        return false;

    juce::StringArray timedOutNames;

    std::vector<std::string> timedOutIds;

    for (auto& recording : takeRecordings)
    {
        if (recording.finalizationComplete)
            continue;

        recording.finalizationComplete = true;
        recording.finalizationError = "The camera did not finish writing its movie within 15 seconds.";
        timedOutNames.add (juce::String (recording.displayName));
        timedOutIds.push_back (recording.deviceId);

        const auto entry = open.find (recording.deviceId);
        if (entry != open.end())
        {
            entry->second.device->onRecordingFinished = nullptr;
            entry->second.device->onRecordingStarted = nullptr;
            entry->second.recordingThisTake = false;
            entry->second.startingThisTake = false;
        }

        // A writer which missed its required completion callback is not a
        // safe device generation to reuse. Keep this durable until the user
        // explicitly retries the camera; periodic discovery/reconciliation
        // must not silently open it for the next take.
        openFailures[recording.deviceId] =
            "Video file finalization timed out for "
            + juce::String (recording.displayName)
            + ". Turn this camera off and back on before recording with it again.";
        finalizationRetryRequiredIds.insert (recording.deviceId);
    }

    if (! timedOutNames.isEmpty())
        recordingFinalizationProblem = timedOutNames.size() == 1
            ? timedOutNames[0] + " did not finish its video file. The audio is safe; do not use that movie."
            : juce::String (timedOutNames.size())
                + " cameras did not finish their video files. The audio is safe; do not use those movies.";

    finalizingDevices.clear();

    // A backend that missed its required completion callback is not safe to
    // reuse for another file. Closing is asynchronous in the pinned JUCE patch,
    // so this remains bounded even when the driver itself is wedged.
    for (const auto& id : timedOutIds)
        closeCamera (id);

    return true;
}

bool CameraController::finishRecordingFinalizationIfReady()
{
    if (recordingFinalizationState != RecordingFinalizationState::Waiting)
        return false;

    if (std::any_of (takeRecordings.begin(), takeRecordings.end(),
                     [] (const TakeRecording& recording) { return ! recording.finalizationComplete; }))
        return false;

    const bool failed = std::any_of (takeRecordings.begin(), takeRecordings.end(),
        [] (const TakeRecording& recording) { return recording.finalizationError.isNotEmpty(); });

    if (failed && recordingFinalizationProblem.isEmpty())
    {
        juce::StringArray failedNames;
        for (const auto& recording : takeRecordings)
            if (recording.finalizationError.isNotEmpty())
                failedNames.add (juce::String (recording.displayName));

        recordingFinalizationProblem = failedNames.size() == 1
            ? failedNames[0] + " could not finish its video file. The audio is safe; do not use that movie."
            : juce::String (failedNames.size())
                + " cameras could not finish their video files. The audio is safe; do not use those movies.";
    }

    recordingFinalizationState = failed ? RecordingFinalizationState::Failed
                                        : RecordingFinalizationState::Succeeded;
    finalizingDevices.clear();

    if (reconcileWhenFinalized)
        reconcileAfterTake();

    reconcileWhenFinalized = false;
    return true;
}

void CameraController::requestDiscovery (bool supersedePending)
{
    const auto discovery = discoveryState;
    bool launchWorker = false;

    {
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        if (discovery->cancelled)
            return;

        // The status timer is observation, not a new user request. Let the
        // current OS call finish so a merely slow enumerator can eventually
        // publish; explicit actions are allowed to invalidate that snapshot.
        if (discovery->workerRunning && ! supersedePending)
            return;

        if (discovery->requested == 0)
            initialDiscoveryRequestedAtMs = juce::Time::getMillisecondCounterHiRes();

        ++discovery->requested;
        if (! discovery->workerRunning)
        {
            discovery->workerRunning = true;
            launchWorker = true;
        }
    }

    if (! launchWorker)
        return;

    try
    {
        std::thread ([discovery]
        {
            runDiscoveryWorker (discovery);
        }).detach();
    }
    catch (...)
    {
        // Thread construction can fail under severe resource pressure. Leave
        // the request pending and reopen the one-flight gate so a later UI
        // refresh can retry without blocking this caller.
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        discovery->workerRunning = false;
        discovery->condition.notify_all();
    }
}

void CameraController::runDiscoveryWorker (std::shared_ptr<DiscoveryState> discovery)
{
   #if JUCE_WINDOWS
    const auto comResult = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool uninitialiseCom = SUCCEEDED (comResult);
   #endif

    for (;;)
    {
        uint64_t requested = 0;
        {
            const std::lock_guard<std::mutex> guard (discovery->mutex);
            if (discovery->cancelled)
            {
                discovery->workerRunning = false;
                discovery->condition.notify_all();
                break;
            }

            requested = discovery->requested;
        }

        juce::StringArray names, ids;
        bool succeeded = false;
        try
        {
            // One enumeration for both, so ids[i] is the device names[i] is,
            // at the index openDevice() will be given.
            juce::CameraDevice::getAvailableDevicesWithIds (names, ids);
            succeeded = true;
        }
        catch (...)
        {
            // A platform exception must not escape a detached worker. Keep the
            // previous applied snapshot and allow a later refresh to retry.
        }

        bool finished = false;
        {
            const std::lock_guard<std::mutex> guard (discovery->mutex);

            if (discovery->cancelled || ! succeeded)
            {
                discovery->workerRunning = false;
                finished = true;
            }
            else if (discovery->requested == requested)
            {
                // Only the newest requested generation may become visible.
                // If another refresh arrived while the OS was blocked, this
                // snapshot is stale and the loop immediately enumerates again.
                discovery->pendingDeviceNames = std::move (names);
                discovery->pendingDeviceIds = std::move (ids);
                discovery->completed = requested;
                discovery->workerRunning = false;
                finished = true;
            }

            discovery->condition.notify_all();
        }

        if (finished)
            break;
    }

   #if JUCE_WINDOWS
    if (uninitialiseCom)
        CoUninitialize();
   #endif
}

void CameraController::applyDeviceNames (const juce::StringArray& names, const juce::StringArray& ids)
{
    // Keep the raw OS answer, not the take-filtered Selection produced below.
    // It is message-thread state and can be replayed synchronously when the
    // take ends so a camera which already reconnected is eligible immediately.
    lastAppliedDeviceNames = names;
    lastAppliedDeviceIds = ids;

    const auto deviceIdAt = [&ids] (int index)
    {
        return juce::isPositiveAndBelow (index, ids.size()) ? ids[index].toStdString() : std::string();
    };

    std::vector<CameraDeviceInfo> cameras;
    std::map<std::string, int> currentDeviceNameCounts;
    // Names every listed device of which carries its own per-device id. Their
    // count changing says nothing about which unit is which: each still has
    // its own identity, so none of the same-name ambiguity below applies.
    std::map<std::string, bool> nameHasPerDeviceIds;
    for (int i = 0; i < names.size(); ++i)
    {
        const auto name = names[i].toStdString();
        ++currentDeviceNameCounts[name];
        const bool hasId = ! deviceIdAt (i).empty();
        const auto known = nameHasPerDeviceIds.find (name);
        nameHasPerDeviceIds[name] = (known == nameHasPerDeviceIds.end() || known->second) && hasId;
    }

    if (takeActive)
    {
        // Occurrence numbers are the only identity JUCE exposes for cameras
        // with the same OS name. If that group's count changes, there is no
        // honest way to tell which physical unit kept which id. Once ambiguous,
        // hold the whole group out until the next take rather than attach a
        // live preview to the wrong recording identity.
        for (const auto& [name, baselineCount] : takeDeviceNameCounts)
        {
            const auto current = currentDeviceNameCounts.find (name);
            const int currentCount = current != currentDeviceNameCounts.end() ? current->second : 0;

            const auto stable = nameHasPerDeviceIds.find (name);
            const bool identifiable = stable != nameHasPerDeviceIds.end() && stable->second;

            if (! identifiable && currentCount != baselineCount && (baselineCount > 1 || currentCount > 1))
                ambiguousTakeDeviceNames.insert (name);

            // Below the take's own count, a camera that began the take has
            // left. A twin that arrived and was pulled again (1 -> 2 -> 1)
            // never takes the group below it.
            if (currentCount < baselineCount)
                shrunkTakeDeviceNames.insert (name);
        }
    }

    std::set<std::string> previousIds;
    for (const auto& camera : selection.getAvailableCameras())
        previousIds.insert (camera.id);
    osIndexById.clear();
    perDeviceIds.clear();

    // These failures mean only that JUCE's index-to-device mapping changed
    // between discovery and open. The result being applied was requested after
    // that mismatch, so it is the fresh identity/index mapping that earns one
    // automatic retry. Ordinary privacy/busy failures remain explicit-retry
    // only and are intentionally not cleared here.
    for (const auto& id : topologyRetryIds)
        openFailures.erase (id);
    topologyRetryIds.clear();

    // Whatever the OS lists, whatever it is plugged into: a USB webcam, a
    // built-in camera, a capture card presenting an HDMI feed as a camera. The
    // app does not vet the source, the same way §2 does not vet a microphone's.
    std::map<juce::String, int> seen;

    for (int i = 0; i < names.size(); ++i)
    {
        const auto name = names[i];
        const int occurrence = ++seen[name];

        // Two cameras of the same model enumerate with the same product string,
        // exactly as §14.6's four identical microphones do. Where the platform
        // gives each device its own id (macOS's uniqueID) that is the camera's
        // identity, so its choices stay with it across a replug, a reboot, or
        // its twin being unplugged. Otherwise the occurrence keeps them apart
        // for as long as they stay connected in the same order.
        const auto nameBasedId = (occurrence == 1 ? name : name + " #" + juce::String (occurrence)).toStdString();
        const auto deviceId = deviceIdAt (i);
        const auto id = deviceId.empty() ? nameBasedId : deviceId;

        // A camera whose writer was interrupted cannot be joined back onto the
        // same movie by JUCE. Keep it out of the live list until this take is
        // over rather than reopening a preview that looks like recording has
        // resumed when the rest of the file is in fact being lost.
        if (takeActive
            && (ambiguousTakeDeviceNames.count (name.toStdString()) > 0
                || camerasDeferredUntilTakeEnds.count (id) > 0))
            continue;

        osIndexById[id] = i;
        cameras.push_back ({ id, name.toStdString(), deviceId.empty() ? std::string() : nameBasedId });

        if (! deviceId.empty())
            perDeviceIds.insert (id);

        // Reconnecting is a fresh opportunity to open the camera. A failure
        // belonging to the prior connection must not suppress that one.
        if (previousIds.count (id) == 0
            && finalizationRetryRequiredIds.count (id) == 0)
            openFailures.erase (id);
    }
    selection.setAvailableCameras (std::move (cameras));
}
#endif

bool CameraController::hiddenOnlyByGrowth (const std::string& id) const
{
    for (const auto& recordingEntry : takeRecordings)
        if (recordingEntry.deviceId == id)
            return ambiguousTakeDeviceNames.count (recordingEntry.deviceName) > 0
                && shrunkTakeDeviceNames.count (recordingEntry.deviceName) == 0;
    return false;
}

void CameraController::applySelection (bool retryFailures)
{
#if JUCE_USE_CAMERA
    // Names rather than a count, because "one of your cameras" helps nobody
    // standing in front of three of them. Declared inside the guard so a build
    // without camera support does not carry an unused local.
    juce::StringArray missingCameras;

    // Rebuilt from durable per-camera failures below. A periodic no-op must
    // keep the existing explanation visible even though it deliberately does
    // not spend another blocking open attempt.
    openProblem.clear();

    if (retryFailures)
        for (const auto& camera : selection.getAvailableCameras())
            if (selection.isEnabled (camera.id))
                finalizationRetryRequiredIds.erase (camera.id);

    // Close first, so a machine that can only hold one camera open at a time
    // has the old one released before the new one is asked for.
    std::vector<std::string> toClose;

    for (const auto& entry : open)
    {
        const bool switchedOff = ! selection.isEnabled (entry.first);
        const bool disappeared = osIndexById.count (entry.first) == 0;

        // Camera membership is frozen for a take. A settings/UI change must
        // not finalize a movie halfway through merely because the camera was
        // switched off; apply it as soon as stopRecording() ends the take.
        // A real unplug is different: the device is already gone, so close it
        // now and retain the partial movie for the watchdog/combiner.
        if (takeActive && entry.second.recordingThisTake && switchedOff && ! disappeared)
            continue;

        // Hidden only because a same-name twin arrived: nothing left, and the
        // open device is still bound to the camera it was recording. Keep it.
        if (takeActive && disappeared
            && (entry.second.recordingThisTake || entry.second.startingThisTake)
            && hiddenOnlyByGrowth (entry.first))
            continue;

        if (switchedOff || disappeared)
            toClose.push_back (entry.first);
    }

    for (const auto& id : toClose)
        closeCamera (id);

    for (auto it = openFailures.begin(); it != openFailures.end();)
    {
        const bool stillEnabled = selection.isEnabled (it->first);
        const bool stillAvailable = osIndexById.count (it->first) > 0;
        if (! stillEnabled || ! stillAvailable)
        {
            if (! stillEnabled)
                finalizationRetryRequiredIds.erase (it->first);
            it = openFailures.erase (it);
        }
        else
            ++it;
    }

    const auto cameraPermission = cameraPermissionProbe != nullptr ? cameraPermissionProbe()
                                                                    : PermissionState::NotApplicable;
    const bool cameraAccessBlocked = cameraAccessBlocks (cameraPermission);
    waitingForCameraPermission = false;

    for (const auto& camera : selection.getAvailableCameras())
    {
        if (! selection.isEnabled (camera.id) || open.count (camera.id) > 0)
            continue;

        // Camera membership and files are fixed when the take starts. A device
        // that appears (or finishes a topology retry) during the take may be
        // used again afterwards, but opening only a preview now would falsely
        // imply that its picture is being added to the running recording.
        if (takeActive)
            continue;

        if (cameraAccessBlocked)
        {
            waitingForCameraPermission = true;
            continue;
        }

        const auto index = osIndexById.find (camera.id);

        // The last launch went down while starting this camera. Starting it
        // again unattended could do the same on every launch; keep the app up
        // and say so, and let the user's own off/on be the retry. That is
        // setCameraEnabledByUser(), not retryFailures: opening the Cameras
        // panel (the only way to reach this camera's switch), starting a take
        // or switching another camera on must not start it.
        if (crashedWhileStartingIds.count (camera.id) > 0)
        {
            openFailures[camera.id] = "SobStage closed unexpectedly while starting "
                                    + juce::String (selection.getDisplayName (camera.id))
                                    + " last time, so it wasn't started automatically. Sound "
                                      "recording is unaffected. Turn this camera off and back on "
                                      "to try it again.";
            continue;
        }

        if (index != osIndexById.end()
            && (retryFailures
                || (openFailures.count (camera.id) == 0
                    && finalizationRetryRequiredIds.count (camera.id) == 0)))
            openCamera (camera.id, index->second, juce::String (camera.displayName));
    }

    if (waitingForCameraPermission)
    {
        if (cameraPermission == PermissionState::NotYetRequested
            && ! cameraPermissionRequested && requestCameraPermission != nullptr)
        {
            cameraPermissionRequested = true;
            requestCameraPermission();
        }

        openProblem = cameraPermission == PermissionState::Denied
            ? juce::String (PermissionGuidance::cameraDeniedMessage (
                  cameraAccessRestrictedProbe != nullptr && cameraAccessRestrictedProbe()))
            : juce::String ("Waiting for you to allow camera access. Click Allow in the macOS "
                            "prompt; if none appeared, turn SobStage on in System Settings > "
                            "Privacy & Security > Camera.");
    }

    // Selection's available list necessarily contains only the latest OS
    // snapshot. Look at its durable choices separately so a previously armed
    // capture card that vanishes remains a visible fault instead of silently
    // disappearing from the panel and from this problem line.
    if (hasAppliedDeviceList)
        for (const auto& camera : selection.getUnavailableEnabledCameras())
            missingCameras.add (juce::String (camera.displayName));

    for (const auto& [id, problem] : openFailures)
    {
        juce::ignoreUnused (id);
        if (openProblem.isNotEmpty())
            openProblem += " ";
        openProblem += problem;
    }

    // Appended, never assigned over: openCamera() may already have recorded a
    // camera that is connected and still would not open, and both facts matter.
    if (! missingCameras.isEmpty())
    {
        if (openProblem.isNotEmpty())
            openProblem += " ";

        openProblem += missingCameras.size() == 1
                       ? "The system isn't listing " + missingCameras[0]
                             + ". Reconnect it and check its cable and HDMI signal. Sound isn't affected."
                       : "The system isn't listing " + juce::String (missingCameras.size())
                             + " cameras. Reconnect them and check their cables and HDMI signal. "
                               "Sound isn't affected.";
    }
#endif
}

#if JUCE_USE_CAMERA
void CameraController::openCamera (const std::string& id, int osIndex,
                                   const juce::String& expectedDeviceName)
{
    const auto viewerRevision = ++viewerRevisions[id];

    // Kept until this camera proves it started (see setStartupGuardFile).
    setStartingGuard (id, true);

    // Ask JUCE/the platform for high-quality capture. The driver ultimately
    // chooses the actual format, so neither the UI nor documentation promises
    // a resolution which the device has not reported.
    //
    // highQuality=false is what JUCE calls preview mode, where the OS is free
    // to drop frames -- fine for a picture on screen, not fine for the file
    // that is the point of the exercise. Since one open device feeds both the
    // view and the recording, the safe request is high-quality capture, with
    // the view kept cheap by the size the main screen's arrows draw it at.
    // The user's per-camera picture size caps what is asked for: the Mac
    // picks the matching preset, and Windows the largest mode within it.
    const auto limit = CameraSelection::frameLimitFor (selection.getQuality (id));
    std::unique_ptr<juce::CameraDevice> device (
        juce::CameraDevice::openDevice (osIndex,
                                        640, 480,      // never settle below this
                                        limit.maxWidth, limit.maxHeight,
                                        true));        // highQuality

    if (device == nullptr)
    {
        // §10.6: what happened, then what to do. The overwhelmingly common
        // cause is the OS privacy prompt having been declined, or another app
        // holding the camera.
        // Appended. Assigning meant two cameras failing in one pass reported
        // only the last -- the same enumeration-order dependence the pass-level
        // clear above was added to remove, still live one function away.
        openFailures[id] = "Couldn't open " + juce::String (selection.getDisplayName (id))
                         + ". Close any other app using it, and check this app is allowed "
                           "to use cameras and capture devices in your system privacy settings.";
        setStartingGuard (id, false);
        return;
    }

    // JUCE's index overload enumerates the OS again inside openDevice(). A
    // hot-plug between our background snapshot and this call can therefore make
    // the same integer refer to a different camera. Never attach that picture to
    // the selected camera's id/name (or record it under that filename): discard
    // it, request a fresh mapping, and retry only after that mapping arrives.
    // Same for twins: two cameras of one model share a name, so only the
    // per-device id shows that the index now points at the other one.
    if (device->getName() != expectedDeviceName
        || (perDeviceIds.count (id) > 0 && device->getDeviceIdentifier().toStdString() != id))
    {
        device.reset();
        openFailures[id] = "The camera list changed while opening "
                         + juce::String (selection.getDisplayName (id))
                         + ". Checking the cameras again.";
        topologyRetryIds.insert (id);
        requestDiscovery();
        setStartingGuard (id, false);
        return;
    }

    OpenCamera entry;
    entry.device = std::move (device);
    entry.viewerRevision = viewerRevision;
    entry.viewerTarget = std::make_shared<std::atomic<juce::Component*>> (nullptr);
    entry.openedAtMs = signalClockMs();

    const auto mailbox = runtimeErrorMailbox;
    entry.device->onErrorOccurred = [mailbox, id, viewerRevision] (const juce::String& error)
    {
        const std::lock_guard<std::mutex> guard (mailbox->mutex);
        mailbox->pending.push_back ({ id, viewerRevision, error });
    };
    entry.device->onFormatChanged = [mailbox, id, viewerRevision] (int width, int height, double fps)
    {
        const std::lock_guard<std::mutex> guard (mailbox->mutex);
        mailbox->formats.push_back ({ id, viewerRevision, width, height, fps });
    };

    // JUCE documents that the macOS preview must be created before anything
    // starts the capture session. It is therefore part of opening the device,
    // not a transient UI object that can be created again on each screen.
    entry.nativeViewer.reset (entry.device->createViewerComponent());

    if (entry.nativeViewer == nullptr)
    {
        openFailures[id] = "Couldn't start video from "
                         + juce::String (selection.getDisplayName (id))
                         + ". Check the camera or capture-device signal and cables, close any other app using it, "
                           "then turn this camera off and back on.";
        setStartingGuard (id, false);
        return;
    }

    entry.viewerTarget->store (entry.nativeViewer.get());
    entry.osIndex = osIndex;
    open[id] = std::move (entry);
    openFailures.erase (id);
    finalizationRetryRequiredIds.erase (id);

    // A native preview component proves only that JUCE created a view. Capture
    // cards with no HDMI signal commonly satisfy that condition while drawing
    // a permanent black rectangle. Listen for an actual image from this exact
    // open generation and keep the UI in its explicit waiting state until one
    // arrives. The native viewer is deliberately created first: JUCE's macOS
    // backend requires that ordering before addListener starts the session.
    const auto frameMailbox = runtimeErrorMailbox;
    auto& stored = open.at (id);
    stored.frameListener = std::make_unique<FrameHeartbeatListener> (
        [frameMailbox, id, viewerRevision] (bool black)
        {
            const std::lock_guard<std::mutex> guard (frameMailbox->mutex);
            frameMailbox->frames.push_back ({ id, viewerRevision, black });
        });
    stored.device->addListener (stored.frameListener.get());

    // Some camera backends can synchronously deliver their first frame while
    // the listener is being attached. Consume that proof before returning so a
    // working camera does not flash a false waiting state for one UI tick.
    applyPendingRuntimeEvents();

    // Deliberately does NOT clear openProblem. It used to, which meant one
    // camera opening successfully erased the message about another that had
    // just failed -- with three cameras and one bad one, whether you were told
    // depended on enumeration order. The pass that starts a round of opens is
    // what clears it; see applySelection().
}

void CameraController::closeCamera (const std::string& id)
{
    setStartingGuard (id, false);

    const auto entry = open.find (id);

    if (entry == open.end())
        return;

    const bool wasRecordingThisTake = entry->second.recordingThisTake
                                   || entry->second.startingThisTake
                                   || recordingCameraIds.count (id) > 0
                                   || startingCameraIds.count (id) > 0;

    if (wasRecordingThisTake && entry->second.device != nullptr)
        entry->second.device->stopRecording();

    if (entry->second.device != nullptr && entry->second.frameListener != nullptr)
        entry->second.device->removeListener (entry->second.frameListener.get());

    if (takeActive && wasRecordingThisTake)
        camerasDeferredUntilTakeEnds.insert (id);

    recordingCameraIds.erase (id);
    startingCameraIds.erase (id);
    recording = ! recordingCameraIds.empty();

    entry->second.viewerTarget->store (nullptr);
    ++viewerRevisions[id];

    // After Stop the take is over but this device may still owe didFinish for
    // its movie. Destroying it (e.g. unplugged right after Stop) would drop
    // that callback and turn a good movie into a 15 s finalization timeout.
    const bool finalizationStillOwed =
        recordingFinalizationState == RecordingFinalizationState::Waiting
        && std::any_of (takeRecordings.begin(), takeRecordings.end(),
                        [&] (const TakeRecording& recording)
                        {
                            return recording.deviceId == id
                                && ! recording.finalizationComplete;
                        });

    if (((takeActive && wasRecordingThisTake) || finalizationStillOwed)
        && kCameraBackendReportsFinalization
        && entry->second.device != nullptr)
        finalizingDevices.push_back ({ id, entry->second.viewerRevision, takeGeneration,
                                       std::move (entry->second.device) });

    open.erase (entry);
}
#endif

std::unique_ptr<juce::Component> CameraController::createViewer (const std::string& deviceId)
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);

    if (entry != open.end() && entry->second.nativeViewer != nullptr
        && entry->second.firstFrameReceived && ! entry->second.signalTimedOut
        && ! entry->second.pictureBlack)
        return std::make_unique<CameraViewerHost> (entry->second.viewerTarget);
#else
    juce::ignoreUnused (deviceId);
#endif

    return nullptr;
}

CameraController::SignalState CameraController::getSignalState (const std::string& deviceId) const
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);

    if (entry == open.end())
        return SignalState::NotOpen;

    if (entry->second.signalTimedOut)
        return SignalState::TimedOut;

    return entry->second.firstFrameReceived ? SignalState::Live : SignalState::Waiting;
#else
    juce::ignoreUnused (deviceId);
    return SignalState::NotOpen;
#endif
}

juce::String CameraController::getSignalStatusText (const std::string& deviceId) const
{
    const auto displayName = juce::String (selection.getDisplayName (deviceId));

    switch (getSignalState (deviceId))
    {
        case SignalState::Waiting:
            return "Waiting for video from " + displayName
                 + ". Turn the camera on; if it uses HDMI, check the HDMI and USB cables.";

        case SignalState::TimedOut:
            return "No video signal from " + displayName
                 + ". Check the camera or capture device; for HDMI, check the output and both cables, "
                   "then turn this camera off and back on.";

        case SignalState::NotOpen:
#if JUCE_USE_CAMERA
            if (waitingForCameraPermission)
                return "No picture from " + displayName
                     + " yet: SobStage needs camera access. Allow it in the macOS prompt or in "
                       "System Settings > Privacy & Security > Camera.";
#endif

            return "No picture from " + displayName
                 + ". Close any other app using it, then turn this camera off and back on.";

        case SignalState::Live:
            break;
    }

#if JUCE_USE_CAMERA
    if (const auto entry = open.find (deviceId); entry != open.end() && entry->second.pictureBlack)
        return "The picture from " + displayName
             + " is all black. Check the source is switched on and set to 1080p or 720p; "
               "a capture device shows black for HDCP-protected sources (Apple TV, iPad, many "
               "laptops and consoles).";
#endif

    return {};
}

#if defined(SOBSTAGE_CAMERA_SIMULATION)
void CameraController::advanceSignalClockForTesting (double milliseconds)
{
#if JUCE_USE_CAMERA
    signalClockOffsetForTesting += juce::jmax (0.0, milliseconds);
    if (applySignalTimeouts())
        applySelection (false);
#else
    juce::ignoreUnused (milliseconds);
#endif
}
#endif

juce::String CameraController::getActiveFormatText (const std::string& deviceId) const
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);
    return entry != open.end() ? entry->second.activeFormatText : juce::String();
#else
    juce::ignoreUnused (deviceId);
    return {};
#endif
}

void CameraController::setCameraQuality (const std::string& id, CameraQuality quality)
{
    if (selection.getQuality (id) == quality)
        return;

    selection.setQuality (id, quality);

#if JUCE_USE_CAMERA
    // Reopen now so the new size is what the preview shows and the next take
    // records. Never mid-take: the take's cameras are fixed until Stop, and
    // the UI keeps this control off while recording.
    if (! takeActive && open.count (id) > 0)
    {
        closeCamera (id);
        applySelection (false);
    }
#endif
}

uint64_t CameraController::getViewerRevision (const std::string& deviceId) const
{
#if JUCE_USE_CAMERA
    const auto revision = viewerRevisions.find (deviceId);
    return revision != viewerRevisions.end() ? revision->second : 0;
#else
    juce::ignoreUnused (deviceId);
    return 0;
#endif
}

juce::String CameraController::getMovieFileExtension()
{
#if JUCE_USE_CAMERA
    return juce::CameraDevice::getFileExtension();
#else
    return ".mov";
#endif
}

juce::StringArray CameraController::getPlannedFileNames() const
{
    juce::StringArray names;
    const auto extension = getMovieFileExtension();

    for (const auto& plan : selection.buildPlans())
        names.add (juce::String (plan.fileName) + extension);

    return names;
}

juce::String CameraController::getPlannedFileNameFor (const std::string& deviceId) const
{
    const auto extension = getMovieFileExtension();

    // During a take, the file this camera is actually writing. The numbering
    // below is rebuilt from the live list, which keeps moving mid-take: unplug
    // V01's camera and V02's row read "Recording to V01_....mov" while the
    // movie on the card was still V02. Gated on the take only, not on its
    // finalization: after Stop the row says "Writes ..." about the NEXT take,
    // which is numbered afresh from the live list.
    if (takeActive)
    {
        for (const auto& recording : takeRecordings)
            if (recording.deviceId == deviceId)
                return recording.file.getFileName();

        for (const auto& plan : takePlans)
            if (plan.deviceId == deviceId)
                return juce::String (plan.fileName) + extension;
    }

    for (const auto& plan : selection.buildPlans())
        if (plan.deviceId == deviceId)
            return juce::String (plan.fileName) + extension;

    return {};
}

bool CameraController::startRecording (const juce::File& sessionFolder, double audioStartMs)
{
#if JUCE_USE_CAMERA
    if (takeActive)
        return recording;

    if (recordingFinalizationState == RecordingFinalizationState::Waiting)
    {
        recordProblem = "The last camera files are still finishing. Wait before starting another take.";
        return false;
    }

    // A frame may have arrived since the last UI poll. Consume that bounded
    // mailbox now so the frozen take roster uses the freshest proof without
    // ever entering a platform camera call after audio has begun.
    const bool cameraStateChanged = applyPendingRuntimeEvents() || applySignalTimeouts();
    if (cameraStateChanged)
        applySelection (false);

    const auto extension = juce::CameraDevice::getFileExtension();
    int startRequests = 0;

    ++takeGeneration;
    takeActive = true;
    takePlans = selection.buildIntendedPlans();
    takeRecordings.clear();
    recordingCameraIds.clear();
    startingCameraIds.clear();
    finalizingDevices.clear();
    camerasDeferredUntilTakeEnds.clear();
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();
    shrunkTakeDeviceNames.clear();
    recordingFinalizationState = RecordingFinalizationState::Idle;
    recordingFinalizationProblem.clear();
    reconcileWhenFinalized = false;

    // Its own field, and cleared each time this runs. A callback-backed start
    // failure below must remain visible rather than being erased after the
    // mailbox is drained.
    recordProblem.clear();

    for (const auto& camera : selection.getAvailableCameras())
        ++takeDeviceNameCounts[camera.displayName];

    // Collected so the problem below can NAME the cameras that did not start.
    // It used to say "2 of your cameras couldn't start recording" -- a count,
    // to someone looking at a rig of four, with the roster of display names
    // sitting right here in takePlans. Knowing which two is the whole
    // difference between checking one cable and checking all of them.
    juce::StringArray couldNotStart;

    for (const auto& plan : takePlans)
    {
        const auto entry = open.find (plan.deviceId);

        if (entry == open.end() || entry->second.device == nullptr
            || ! entry->second.firstFrameReceived || entry->second.signalTimedOut)
        {
            couldNotStart.add (juce::String (plan.displayName));
            continue;
        }

        // §6.2: the picture goes in the same session folder as the sound, under
        // the same naming rules, so one folder is still the whole take.
        const auto file = sessionFolder.getChildFile (juce::String (plan.fileName) + extension);
        entry->second.recordingFile = file;

        const auto mailbox = runtimeErrorMailbox;
        const auto viewerRevision = entry->second.viewerRevision;
        const auto generation = takeGeneration;
        entry->second.device->onRecordingStarted =
            [mailbox, id = plan.deviceId, viewerRevision, generation, audioStartMs]
            (const juce::File& startedFile, double firstFrameMs)
            {
                const auto offset = cameraStartOffsetSeconds (
                    audioStartMs, firstFrameMs, juce::Time::getMillisecondCounterHiRes());
                const std::lock_guard<std::mutex> guard (mailbox->mutex);
                mailbox->recordingsStarted.push_back (
                    { id, viewerRevision, generation, startedFile, offset });
            };
        entry->second.device->onRecordingFinished =
            [mailbox, id = plan.deviceId, viewerRevision, generation]
            (const juce::File& finishedFile, const juce::String& error)
            {
                const std::lock_guard<std::mutex> guard (mailbox->mutex);
                mailbox->recordingsFinished.push_back (
                    { id, viewerRevision, generation, finishedFile, error });
            };

        entry->second.startingThisTake = true;
        entry->second.recordingThisTake = false;
        startingCameraIds.insert (plan.deviceId);
        takeRecordings.push_back ({ plan.deviceId, plan.displayName,
                                    entry->second.device->getName().toStdString(), file,
                                    0.0, false, false, {} });

        // Quality 2 is JUCE's highest. There is no setting for this and there
        // should not be: nobody wants the take they cannot redo in medium.
        entry->second.device->startRecordingToFile (file, 2);

        ++startRequests;
    }

    // A physically absent camera cannot join a movie after the take begins.
    // Hide only those absent-at-t=0 ids from later topology snapshots. A
    // connected camera which failed because it is busy or privacy-blocked must
    // remain listed so its precise open failure is not replaced by the false
    // "system isn't listing it" diagnosis. takeActive already prevents either
    // kind from being opened into the running take.
    for (const auto& camera : selection.getUnavailableEnabledCameras())
        camerasDeferredUntilTakeEnds.insert (camera.id);

    // REC is confirmed only by AVFoundation's didStart callback. A submitted
    // request remains STARTING until that generation-scoped callback arrives.
    recording = false;
    applyPendingRuntimeEvents();

    if (! takePlans.empty() && startRequests < static_cast<int> (takePlans.size()))
    {
        // Named, in the same voice as the finalization failure above. The
        // count alone sent someone round every camera on the rig.
        recordProblem = (couldNotStart.size() == 1
                             ? couldNotStart[0] + " couldn't start recording."
                             : couldNotStart.joinIntoString (", ")
                                   + " couldn't start recording.")
                      + " The sound is recording either way.";

        // Belt and braces: if the roster and the request count ever disagree
        // -- a plan that reached startRecordingToFile and still did not count
        // -- fall back to the number rather than naming the wrong cameras.
        if (couldNotStart.isEmpty())
            recordProblem = juce::String (static_cast<int> (takePlans.size()) - startRequests)
                            + " of your cameras couldn't start "
                            "recording. The sound is recording either way.";
    }

    return startRequests > 0;
#else
    juce::ignoreUnused (sessionFolder, audioStartMs);
    return false;
#endif
}

std::vector<CombinedTakeInput> CameraController::getCombinedTakeInputs() const
{
    std::vector<CombinedTakeInput> inputs;

#if JUCE_USE_CAMERA
    // Retained independently of the live/open map: unplug closes and erases a
    // CameraDevice, but the movie it finalized is still a valid partial input
    // and must remain available to the post-take combiner.
    for (const auto& takeRecording : takeRecordings)
    {
        const auto& file = takeRecording.file;

        if (takeRecording.started && takeRecording.finalizationComplete
            && takeRecording.finalizationError.isEmpty())
            inputs.push_back ({ file.getFileName().toStdString(),
                                takeRecording.startOffsetSeconds });
    }
#endif

    return inputs;
}

std::vector<CameraController::TakeVideoRecord> CameraController::getTakeVideoRecords() const
{
    std::vector<TakeVideoRecord> videos;

#if JUCE_USE_CAMERA
    videos.reserve (takeRecordings.size());

    // Unlike takePlans, this list contains only writers for which JUCE accepted
    // startRecordingToFile(). It therefore cannot invent a file for a missing
    // or failed-open camera. An interrupted writer remains here because its
    // finalized partial movie is still a real contribution to the take.
    for (const auto& takeRecording : takeRecordings)
        if (takeRecording.started && takeRecording.finalizationComplete
            && takeRecording.finalizationError.isEmpty())
            videos.push_back ({ takeRecording.displayName,
                                takeRecording.file.getFileName().toStdString() });
#endif

    return videos;
}

std::vector<CameraController::TakeVideoRecord> CameraController::getTakeVideoRecordsSoFar() const
{
    std::vector<TakeVideoRecord> videos;

#if JUCE_USE_CAMERA
    videos.reserve (takeRecordings.size());

    // didStart is the proof a file exists, as above. Not waiting for didFinish
    // is the whole difference: a movie still being written is listed, and one
    // whose writer has already reported a failure is not -- it is no more
    // usable after a crash than after a clean stop.
    for (const auto& takeRecording : takeRecordings)
        if (takeRecording.started && takeRecording.finalizationError.isEmpty())
            videos.push_back ({ takeRecording.displayName,
                                takeRecording.file.getFileName().toStdString() });
#endif

    return videos;
}

std::vector<CameraController::TakeCameraState> CameraController::getTakeCameraStates() const
{
    std::vector<TakeCameraState> states;
    states.reserve (takePlans.size());

    for (const auto& plan : takePlans)
        states.push_back ({ plan.deviceId, plan.displayName,
                            recordingCameraIds.count (plan.deviceId) > 0,
                            startingCameraIds.count (plan.deviceId) > 0 });

    return states;
}

void CameraController::stopRecording()
{
    stopRecordingInternal (true);
}

void CameraController::stopRecordingForShutdown()
{
    stopRecordingInternal (false);
}

bool CameraController::pollRecordingFinalization()
{
    return applyPendingCameraList();
}

void CameraController::stopRecordingInternal (bool reconcileForNextTake)
{
    const bool wasTakeActive = takeActive;

#if JUCE_USE_CAMERA
    for (auto& entry : open)
    {
        if ((entry.second.recordingThisTake || entry.second.startingThisTake)
            && entry.second.device != nullptr)
            entry.second.device->stopRecording();

        entry.second.recordingThisTake = false;
        entry.second.startingThisTake = false;
    }

    // The simulator can deliver both start and finish synchronously. Drain its
    // generation-tagged facts before deciding whether any file is still owed.
    applyPendingRuntimeEvents();
#endif

    recordingCameraIds.clear();
    startingCameraIds.clear();
    recording = false;
    takeActive = false;
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();
    shrunkTakeDeviceNames.clear();

#if JUCE_USE_CAMERA
    if (! wasTakeActive)
        return;

    reconcileWhenFinalized = reconcileForNextTake;
    recordingFinalizationState = RecordingFinalizationState::Waiting;
    recordingFinalizationDeadlineMs = signalClockMs() + kCameraFinalizationTimeoutMs;

    if (! kCameraBackendReportsFinalization)
    {
        // Unpatched/unsupported JUCE camera backends expose no completion
        // callback. Their legacy stop contract is the only available boundary;
        // the patched macOS and Windows backends never enter this fallback.
        for (auto& recording : takeRecordings)
            recording.finalizationComplete = true;

        for (auto& [id, entry] : open)
        {
            juce::ignoreUnused (id);
            entry.device->onRecordingStarted = nullptr;
            entry.device->onRecordingFinished = nullptr;
            entry.recordingThisTake = false;
            entry.startingThisTake = false;
        }
    }

    finishRecordingFinalizationIfReady();
#else
    juce::ignoreUnused (wasTakeActive, reconcileForNextTake);
#endif
}

#if JUCE_USE_CAMERA
void CameraController::reconcileAfterTake()
{
    const auto deferred = camerasDeferredUntilTakeEnds;
    camerasDeferredUntilTakeEnds.clear();

    // applyDeviceNames deliberately hid absent-at-start or interrupted cameras
    // from the running take. Restore the latest unfiltered OS list only after
    // every movie is complete, so a reconnect cannot reuse its writer early.
    applyDeviceNames (lastAppliedDeviceNames, lastAppliedDeviceIds);

    for (const auto& id : deferred)
    {
        const bool failedToFinalize = std::any_of (
            takeRecordings.begin(), takeRecordings.end(),
            [&] (const TakeRecording& recording)
            {
                return recording.deviceId == id
                    && recording.finalizationError.isNotEmpty();
            });

        if (! failedToFinalize && selection.isEnabled (id) && osIndexById.count (id) > 0)
            openFailures.erase (id);
    }

    applySelection (false);
}
#endif

} // namespace mma
