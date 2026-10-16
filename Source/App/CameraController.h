#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <map>
#include <memory>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include "../Core/CameraSelection.h"
#include "../Core/CombinedTakePlan.h"
#include "../Core/PermissionGuidance.h"
#include <vector>

#if JUCE_USE_CAMERA
#include <juce_video/juce_video.h>
#endif

namespace mma {

/// The picture half of a take.
///
/// There is no audio path in this class at all, and that is the point. The
/// microphones are the sound: a camera's own microphone folded into a take
/// would put a room mic nobody asked for into a recording whose whole purpose
/// is one clean track per person. On macOS and Windows -- the two desktop
/// targets §11 names -- the platform camera capture JUCE drives is video-only,
/// so the files this writes carry no sound track of any kind. The sound lives
/// beside them in the WAVs, two separate files, aligned by the shared session
/// start §6.1 stamps into every stem.
///
/// Recording requests JUCE's high-quality mode; the OS/driver chooses the
/// actual capture format. The live view is the only thing the preview setting
/// touches -- see PreviewQuality.
class CameraController
{
public:
    enum class SignalState
    {
        NotOpen,
        Waiting,
        Live,
        TimedOut
    };

    enum class RecordingFinalizationState
    {
        Idle,
        Waiting,
        Succeeded,
        Failed
    };

    struct TakeCameraState
    {
        std::string id;
        std::string displayName;
        bool recording = false;
        bool starting = false;
    };

    /// One camera writer which actually started for the current/most recent
    /// take. Missing intended cameras deliberately have no record here: a
    /// session manifest must never claim a movie file that was not started.
    struct TakeVideoRecord
    {
        std::string displayName;
        std::string fileName;
    };

    CameraController();
    ~CameraController();

    /// False on a build or an OS where the app cannot open a camera at all.
    /// Everything else then becomes a no-op and getUnavailableReason() says why
    /// in one sentence (§10.6), rather than the panel simply staying empty.
    bool isSupported() const;
    juce::String getUnavailableReason() const;

    /// Requests a background re-read of whatever the OS is offering and
    /// applies the previous completed result, if any. Device discovery can
    /// enter AVFoundation/DirectShow and must not stall the message thread.
    void refreshCameras();

    /// Periodic topology poll which consumes a completed snapshot but does not
    /// supersede a scan already inside the OS. This prevents a consistently
    /// slow (but healthy) enumerator from being restarted faster than it can
    /// ever publish. Explicit user/topology actions use refreshCameras().
    void refreshCamerasIfIdle();

    /// Applies a completed background discovery result on the message thread.
    /// Returns true only when a new result was consumed.
    bool applyPendingCameraList();

    /// Used by the platform simulator to deterministically await the request
    /// it just made. The shipping UI never waits; it polls applyPendingCameraList.
    bool waitForCameraRefresh (int timeoutMilliseconds);

    /// True for the bounded grace period before the first OS camera snapshot
    /// reaches the message thread. A remembered capture card is labelled as
    /// still being checked rather than falsely declared unplugged.
    bool isInitialDiscoveryPending() const noexcept;

    /// True once a look for cameras has finished and its list been applied:
    /// only then does an empty list mean no camera is plugged in. Stays false
    /// for as long as a driver keeps the first look from finishing.
    bool hasLookedForCameras() const noexcept;

    /// Crash-loop guard. While a camera is being started, its id is kept in
    /// this file; it leaves once the camera delivers a frame, has been open for
    /// a while, is closed, or the app quits normally. If the file still names a
    /// camera at the next launch, the app went down while starting it, so that
    /// camera is not started automatically again, on this or any later launch:
    /// the show goes on with sound, and turning the camera off and on
    /// (setCameraEnabledByUser) tries it again. Call once, at launch, before the
    /// first applySelection().
    void setStartupGuardFile (const juce::File& file);

    /// macOS camera privacy. Opening a camera while the system prompt is still
    /// up (or after a denial) gives a session that delivers black frames and
    /// never recovers on its own. With a probe set, no camera opens until it
    /// reads Granted or NotApplicable: "not yet asked" calls `request` once,
    /// and the cameras open on the first poll after the user clicks Allow.
    ///
    /// `restricted`, when set, says whether a Denied answer is really macOS's
    /// Restricted (an administrator's policy). It changes only the wording of
    /// the problem line; the cameras are held back exactly as for Denied.
    void setCameraPermission (std::function<PermissionState()> probe,
                              std::function<void()> request,
                              std::function<bool()> restricted = nullptr);

    /// The user switched one camera on or off in the Cameras panel. Switching
    /// a camera on is the explicit retry the crash-loop guard waits for: it is
    /// the only thing that releases that camera's hold. Opening the panel, a
    /// take, or switching some other camera on never does.
    void setCameraEnabledByUser (const std::string& id, bool enabled);

    CameraSelection& getSelection() { return selection; }
    const CameraSelection& getSelection() const { return selection; }

    /// Opens the enabled cameras for viewing, and closes the ones that are no
    /// longer enabled. §5.1 makes the sound live from launch rather than from
    /// record, and a picture is worth even less after the fact: a camera you
    /// cannot see until you press record is a camera you aim afterwards.
    /// `retryFailures` is reserved for an explicit user action (opening the
    /// camera panel, toggling a camera, or starting a take). Periodic UI
    /// refreshes pass false so a camera held by another app is not reopened
    /// twice a second forever on the message thread.
    void applySelection (bool retryFailures = false);

    /// A live view of one open camera, or nullptr when it is not open. The
    /// caller owns what comes back.
    std::unique_ptr<juce::Component> createViewer (const std::string& deviceId);

    /// Changes whenever the backing camera/view changes or an open attempt
    /// fails. UI caches include this value so a successful retry for the same
    /// device id replaces its old "no picture" placeholder.
    uint64_t getViewerRevision (const std::string& deviceId) const;

    /// A non-null CameraDevice/native preview is only an opened handle, not
    /// proof that an HDMI capture card is delivering pictures. A camera is
    /// eligible for a take only after its current device generation has
    /// delivered an actual image callback.
    SignalState getSignalState (const std::string& deviceId) const;

    /// Plain-language status for a tile which cannot yet show a proven live
    /// picture. Empty once a frame has arrived.
    juce::String getSignalStatusText (const std::string& deviceId) const;

    /// "3840 x 2160, 30 fps": what this camera reported it is actually
    /// delivering, once its session started. Empty until then, and on
    /// platforms that do not report it.
    juce::String getActiveFormatText (const std::string& deviceId) const;

    /// The user picked a recording quality for one camera. Remembered, and
    /// applied by reopening the camera -- never during a take, whose cameras
    /// are fixed until Stop.
    void setCameraQuality (const std::string& id, CameraQuality quality);

#if defined(SOBSTAGE_CAMERA_SIMULATION)
    /// Advances only the signal-timeout clock used by the deterministic camera
    /// simulator. Shipping builds always use the monotonic system clock.
    void advanceSignalClockForTesting (double milliseconds);
#endif

    /// §6.2: one file per camera, in the session folder next to the audio.
    /// Returns false only when nothing could be started at all.
    ///
    /// `audioStartMs` is the high-resolution millisecond counter read as the
    /// audio take began. The sound is always started first -- the stem files
    /// and the writer thread are opened before any camera is asked to record --
    /// so each camera's file begins some way into the take, and how far in is
    /// the one number the combining step cannot work out for itself. It is
    /// measured rather than assumed: the backend's estimate of the movie's
    /// first frame on the same counter (macOS: the didStart time less what the
    /// writer had already recorded), or, when it cannot give one, the counter
    /// read again the moment the OS confirms the start.
    bool startRecording (const juce::File& sessionFolder, double audioStartMs = 0.0);
    void stopRecording();
    /// Finalizes camera files without reopening previews. Used only while the
    /// application is quitting, when a normal post-take recovery would light
    /// hardware just before it is destroyed.
    void stopRecordingForShutdown();
    bool isRecording() const { return recording; }

    /// Applies AVFoundation's file-finished callbacks and the bounded timeout.
    /// This never waits inside the OS. The ordinary UI camera poll calls it too;
    /// the explicit name lets Application gate metadata and combining on it.
    bool pollRecordingFinalization();
    RecordingFinalizationState getRecordingFinalizationState() const noexcept
    {
        return recordingFinalizationState;
    }
    bool isFinalizingRecording() const noexcept
    {
        return recordingFinalizationState == RecordingFinalizationState::Waiting;
    }
    juce::String getRecordingFinalizationProblem() const { return recordingFinalizationProblem; }

    /// Frozen camera membership for the current/most recent take. A camera that
    /// disappears remains here with recording=false, so the watchdog can report
    /// the loss without mistaking a preview reopened later for resumed video.
    std::vector<TakeCameraState> getTakeCameraStates() const;

    /// The complete intended roster is retained through stopRecording() for
    /// watchdog/history checks. It can include a missing camera; use
    /// getTakeVideoRecords() for the files that actually started.
    const std::vector<CameraPlan>& getTakePlans() const { return takePlans; }

    /// Actual started camera files for session.json, retained after stop and
    /// after an unplug finalizes a partial movie.
    std::vector<TakeVideoRecord> getTakeVideoRecords() const;

    /// The same list while the take is still running, for the session.json a
    /// crash leaves behind: every movie whose writer confirmed it started and
    /// has not failed, finished or not. getTakeVideoRecords() waits for the
    /// finish, which is right at Stop and meant that the record refreshed
    /// mid-take named no movie at all -- while the movie a crash cut short was
    /// in the folder beside the sound, playable up to its last fragment.
    std::vector<TakeVideoRecord> getTakeVideoRecordsSoFar() const;

    /// What each camera contributed to the take just finished: the file it
    /// wrote and how late it started. Empty when nothing recorded.
    std::vector<CombinedTakeInput> getCombinedTakeInputs() const;

    /// The container this computer's cameras write their movies in,
    /// juce::CameraDevice's own: ".mov" on a Mac, ".wmv" on Windows (and
    /// ".mov" where cameras are not supported, for the summaries).
    static juce::String getMovieFileExtension();

    /// The file names currently available enabled cameras would write,
    /// extension included, for the pre-take save summary.
    juce::StringArray getPlannedFileNames() const;

    /// The file one camera will write, extension included, or empty when that
    /// camera is not in the take. During a take, a camera in it gets the file
    /// it is actually writing, fixed at Record, however the list moves since.
    juce::String getPlannedFileNameFor (const std::string& deviceId) const;

    /// §10.6: whatever is currently wrong with the cameras, in plain language.
    /// Empty when nothing is.
    juce::String getProblem() const
    {
        if (openProblem.isEmpty())
            return recordProblem;

        if (recordProblem.isEmpty())
            return openProblem;

        return openProblem + " " + recordProblem;
    }

    void setPreviewQuality (PreviewQuality quality) { previewQuality = quality; }
    PreviewQuality getPreviewQuality() const { return previewQuality; }

private:
    CameraSelection selection;
    PreviewQuality previewQuality = PreviewQuality::Low;
    /// Two fields, not one, because two independent operations report through
    /// this and each used to assign over the other: opening the cameras
    /// (applySelection/openCamera) and starting a take (startRecording). One
    /// string meant whichever ran last won, so a camera that would not open was
    /// erased by the take starting, and a second camera failing erased the
    /// first. getProblem() joins whichever are set.
    juce::String openProblem;
    juce::String recordProblem;
    bool recording = false;

#if JUCE_USE_CAMERA
    struct OpenCamera
    {
        std::unique_ptr<juce::CameraDevice> device;
        // JUCE's macOS preview layer must be created before the capture session
        // starts. Keep that one native component for the whole device lifetime;
        // the UI receives lightweight hosts which reparent it between screens.
        std::unique_ptr<juce::Component> nativeViewer;
        std::shared_ptr<std::atomic<juce::Component*>> viewerTarget;
        std::unique_ptr<juce::CameraDevice::Listener> frameListener;
        uint64_t viewerRevision = 0;
        int osIndex = -1;
        juce::File recordingFile;
        bool recordingThisTake = false;
        bool startingThisTake = false;
        bool firstFrameReceived = false;
        bool signalTimedOut = false;
        int blackFramesInARow = 0;
        juce::String activeFormatText;
        bool pictureBlack = false;
        double openedAtMs = 0.0;
        double lastFrameAtMs = 0.0;

        /// Seconds after the audio's t=0 that this camera's first frame lands.
        double startOffsetSeconds = 0.0;
    };

    // Keyed by the id CameraSelection uses, so the two never have to agree on
    // an ordering -- the OS list reorders on a hot-plug and the choices must not
    // follow it onto a different camera.
    std::map<std::string, OpenCamera> open;
    std::map<std::string, juce::String> openFailures;
    // A movie writer missed didFinish. Discovery churn must never spend a new
    // open attempt for that same id until an explicit retry/switch cycle.
    std::set<std::string> finalizationRetryRequiredIds;
    // Crash-loop guard (setStartupGuardFile): cameras named by the file a
    // crashed launch left behind, and the ones this launch is still starting.
    juce::File startupGuardFile;
    std::set<std::string> crashedWhileStartingIds;
    std::set<std::string> startingGuardIds;
    void setStartingGuard (const std::string& id, bool starting);
    void writeStartupGuard();

    // macOS camera privacy (setCameraPermission).
    std::function<PermissionState()> cameraPermissionProbe;
    std::function<void()> requestCameraPermission;
    std::function<bool()> cameraAccessRestrictedProbe;
    bool cameraPermissionRequested = false;
    bool waitingForCameraPermission = false;
    std::map<std::string, uint64_t> viewerRevisions;

    struct RuntimeCameraError
    {
        std::string id;
        uint64_t viewerRevision = 0;
        juce::String message;
    };

    struct FrameNotification
    {
        std::string id;
        uint64_t viewerRevision = 0;
        bool black = false;
    };

    /// Proofs arrive twice a second, so about 1.5 s of black.
    static constexpr int kBlackFramesBeforeWarning = 3;

    struct RecordingFinishedNotification
    {
        std::string id;
        uint64_t viewerRevision = 0;
        uint64_t takeGeneration = 0;
        juce::File file;
        juce::String error;
    };

    struct RecordingStartedNotification
    {
        std::string id;
        uint64_t viewerRevision = 0;
        uint64_t takeGeneration = 0;
        juce::File file;
        double startOffsetSeconds = 0.0;
    };

    struct FormatNotification
    {
        std::string id;
        uint64_t viewerRevision = 0;
        int width = 0, height = 0;
        double fps = 0.0;
    };

    struct RuntimeErrorMailbox
    {
        std::mutex mutex;
        std::vector<FormatNotification> formats;
        std::vector<RuntimeCameraError> pending;
        std::vector<FrameNotification> frames;
        std::vector<RecordingStartedNotification> recordingsStarted;
        std::vector<RecordingFinishedNotification> recordingsFinished;
    };

    std::shared_ptr<RuntimeErrorMailbox> runtimeErrorMailbox =
        std::make_shared<RuntimeErrorMailbox>();
    std::set<std::string> topologyRetryIds;
    std::set<std::string> camerasDeferredUntilTakeEnds;
    void openCamera (const std::string& id, int osIndex,
                     const juce::String& expectedDeviceName);
    void closeCamera (const std::string& id);
    bool applyPendingRuntimeEvents();
    bool applySignalTimeouts();
    bool applyRecordingFinalizationTimeout();
    bool finishRecordingFinalizationIfReady();
    void reconcileAfterTake();
    double signalClockMs() const noexcept;
    void requestDiscovery (bool supersedePending = true);
#if defined(SOBSTAGE_CAMERA_SIMULATION)
    double signalClockOffsetForTesting = 0.0;
#endif
#endif

    struct TakeRecording
    {
        std::string deviceId;
        std::string displayName;
        std::string deviceName;
        juce::File file;
        double startOffsetSeconds = 0.0;
        bool started = false;
        bool finalizationComplete = false;
        juce::String finalizationError;
    };

#if JUCE_USE_CAMERA
    struct FinalizingDevice
    {
        std::string id;
        uint64_t viewerRevision = 0;
        uint64_t takeGeneration = 0;
        std::unique_ptr<juce::CameraDevice> device;
    };
#endif

    bool takeActive = false;
    std::vector<CameraPlan> takePlans;
    std::vector<TakeRecording> takeRecordings;
    std::set<std::string> recordingCameraIds;
    std::set<std::string> startingCameraIds;
    std::map<std::string, int> takeDeviceNameCounts;
    std::set<std::string> ambiguousTakeDeviceNames;
    /// Same-name groups whose count has fallen below the take's baseline at
    /// some point this take. Only that can mean a recording camera left; a
    /// group that has only grown (or grown and come back) is held out of the
    /// list for identity's sake, but its running writers are not cut (a twin
    /// plugged in, or plugged in and pulled, used to stop the one recording).
    std::set<std::string> shrunkTakeDeviceNames;
    void stopRecordingInternal (bool reconcileForNextTake);
    bool hiddenOnlyByGrowth (const std::string& id) const;
    uint64_t takeGeneration = 0;
    RecordingFinalizationState recordingFinalizationState = RecordingFinalizationState::Idle;
    juce::String recordingFinalizationProblem;
    double recordingFinalizationDeadlineMs = 0.0;
    bool reconcileWhenFinalized = false;
#if JUCE_USE_CAMERA
    std::vector<FinalizingDevice> finalizingDevices;
#endif

    // The OS list index for each id, refreshed with the list itself.
    std::map<std::string, int> osIndexById;

#if JUCE_USE_CAMERA
    struct DiscoveryState
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool cancelled = false;
        bool workerRunning = false;
        uint64_t requested = 0;
        uint64_t completed = 0;
        juce::StringArray pendingDeviceNames;
        /// Parallel to pendingDeviceNames: the platform's per-device ids
        /// (macOS uniqueID), empty where it has none.
        juce::StringArray pendingDeviceIds;
    };

    static void runDiscoveryWorker (std::shared_ptr<DiscoveryState> state);
    /// `ids` is parallel to `names`. A non-empty id is the camera's identity
    /// (settings follow the physical device); an empty one falls back to the
    /// name, "Name #2" and on for cameras sharing a name, by OS order.
    void applyDeviceNames (const juce::StringArray& names, const juce::StringArray& ids);
    /// Selection ids that are the platform's per-device ids, so an opened
    /// device can be checked to be that very device and not a same-name twin.
    std::set<std::string> perDeviceIds;

    // Camera enumeration can remain inside AVFoundation/DirectShow forever.
    // The detached worker owns only this shared mailbox, never the controller,
    // so destroying CameraController cancels publication without waiting for
    // that unbounded platform call to return.
    std::shared_ptr<DiscoveryState> discoveryState = std::make_shared<DiscoveryState>();
    uint64_t discoveryApplied = 0;
    bool hasAppliedDeviceList = false;
    double initialDiscoveryRequestedAtMs = 0.0;
    // The unfiltered OS snapshot most recently applied on the message thread.
    // During a take, deferred ids are hidden from Selection; replaying this at
    // stop restores a camera which already reconnected without waiting for a
    // second asynchronous scan before the next Record click.
    juce::StringArray lastAppliedDeviceNames;
    juce::StringArray lastAppliedDeviceIds;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraController)
};

} // namespace mma
