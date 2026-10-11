#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <memory>
#include "../Core/DeviceManager.h"
#include "../Core/ChannelPlan.h"
#include "../Core/TakeCompleteness.h"
#include "../Core/RecordingEngine.h"
#include "../Core/MonitorBus.h"
#include "../Core/MixBusLimiter.h"
#include "../Core/DriftCompensator.h"
#include "../Core/Metering.h"
#include "../Core/SessionMetadata.h"
#include "../Core/TakeStopSnapshot.h"
#include "../Core/SessionFolderNaming.h"
#include "../Core/AppSettings.h"
#include "../Core/ShowTemplate.h"
#include "../Core/SessionRecovery.h"
#include "../Core/CardRemovalNotice.h"
#include "../Core/PreflightThroughputTest.h"
#include "../Core/StreamingTargets.h"
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include "../Core/PortIdentity.h"
#include "../Core/OutputDeviceSelector.h"
#include "../Core/ActivityJournal.h"
#include "../Core/BackgroundRecordWriter.h"
#include "../Core/SessionRecordSchedule.h"
#include "../Core/CapacityMonitor.h"
#include "../Core/TakeWatchdog.h"
#include "../Core/RecordingProof.h"
#include "../Core/FilesystemStatusProbe.h"
#include "../Core/DetachedRefreshCache.h"
#include "../Core/BufferLadder.h"
#include "../Core/CpuPressureMonitor.h"
#include "../Core/MirrorPolicy.h"
#include "../Core/TakeFolderTracker.h"
#include "../Core/SampleFormat.h"
#include "../Core/SetupAdvisor.h"
#include "../Core/CaptureCoordinator.h"
#include "../Core/TapToNameDetector.h"
#include "../Core/FailedOpenRetry.h"
#include "../Core/BatteryRecordingNotice.h"
#include "../Core/PermissionGuidance.h"
#include "../Platform/SleepInhibitor.h"
#include "CameraController.h"
#include "PodcastExport.h"
#include "TakeCombiner.h"
#include "AlarmSpeaker.h"
#include "../Platform/IAudioBackend.h"
#include "../Platform/VirtualDeviceBackend.h"
#include "../Platform/SystemAggregateDevice.h"

namespace mma {

/// §10.1 first-run flow and overall wiring: ties DeviceManager +
/// RecordingEngine + MonitorBus + the platform audio backend + UI together.
/// This class is the composition root; it owns the long-lived engine objects
/// and drives them from OS device-change notifications and UI actions.
class Application
{
public:
    Application();
    ~Application();

    /// Called once at startup: picks the platform backend, enumerates
    /// devices, negotiates rate/depth/master, opens the monitor stream, and
    /// starts monitoring immediately (§5.1: "live from launch").
    void initialise();
    void shutdown();

    DeviceManager& getDeviceManager() { return deviceManager; }
    RecordingEngine& getRecordingEngine() { return recordingEngine; }
    /// Null only on a build with no platform audio backend.
    MonitorBus* getMonitorBus();

    /// Bumped each time the capture coordinator -- and with it the monitor bus
    /// -- is built anew (§2.2 rate change, §5.4 buffer change). Lets the UI
    /// walk see a rebuild happen without comparing pointers a new allocation
    /// could reuse.
    int getCaptureGeneration() const noexcept { return captureGeneration; }

    /// The buffer size the live coordinator's streams were opened with. It
    /// trails getCurrentBufferSize() only while a take is running (§5.4
    /// forbids a reopen mid-recording); once the take stops the two agree
    /// again. Lets the UI walk see that the stop path paid the owed reopen.
    int getOpenBufferSize() const noexcept { return captureBufferSize; }

    /// The sounds the app makes about itself: a chirp when a take starts and
    /// stops, a siren while something is wrong mid-take. They go through the
    /// headphone output when one is open, and through the computer's default
    /// output when none is (AlarmSpeaker).
    void announceRecordingStarted();
    void announceRecordingStopped();
    /// Idempotent, so the UI can re-assert it every tick while its alarm card
    /// is up and a capture rebuilt mid-alarm keeps sounding.
    void setFaultAlarm (bool on);
    bool isFaultAlarmOn() const;
    /// True while a start or stop chirp is still playing.
    bool isAlarmChirping() const;
    /// Samples of alarm tone that have reached the output so far.
    uint64_t getAlarmSamplesRendered() const;
    /// Whether any output is open to carry them: the monitor stream, or the
    /// computer's default output standing in for it.
    bool isAlarmAudible() const;
    /// Whether the computer's default output is open for those sounds
    /// (AlarmSpeaker). Never true while the monitor output is being opened.
    bool isAlarmSpeakerOpen() const noexcept { return alarmSpeaker.isActive(); }

    /// How many times a rebuild let go of the default output before opening
    /// the monitor output (AlarmSpeaker.h: never both). For the UI walk, which
    /// cannot see the moment itself: when the monitor then fails to open, the
    /// speaker is given straight back.
    int getSpeakerHandoversForMonitorOpen() const noexcept { return speakerHandoversForMonitorOpen; }

    /// §5.4: empty while the low-latency monitor path is healthy; otherwise the
    /// plain-language reason it isn't, which the UI must show rather than
    /// silently delivering a 40 ms mix.
    juce::String getMonitorProblem() const;

    /// §10.4: record button behavior. Starts/stops immediately, no
    /// confirmation either direction.
    void toggleRecording();

    /// Drains camera start/finish callbacks without blocking the message
    /// thread and completes the stopped take only after every movie is closed
    /// (or the controller's bounded fail-closed timeout expires).
    /// Journals the camera finalization failure, if there is one. Called from
    /// both stop paths -- the synchronous one used to skip it.
    void reportCameraFinalizationProblem();
    bool pollCameraFinalization();

    /// Starts the ordinary stop path when necessary and returns true once it
    /// is safe for JUCE to destroy the application. Repeated calls are the
    /// nonblocking quit poll; no device wait occurs on this thread.
    bool prepareToQuit();

    /// The after-take work still running, in words for a quit prompt
    /// ("the podcast copy", "the combined video", or both). Empty when none.
    /// Quitting cancels it, so the user is asked before it is thrown away.
    juce::String unfinishedAfterTakeWork() const;

    /// What went wrong with the take that just stopped, in words for a quit
    /// question: the card stopped answering, the files hold no sound, a
    /// camera could not finish its movie. Empty when nothing did, or when the
    /// saved-take card already showed it before the quit was asked for. A
    /// take stopped on the way out used to quit before that card could open.
    juce::String unseenProblemWithLastTake() const;

    /// The user chose to stay open after asking to quit. A take the quit
    /// stopped is the saved-take card's to report from here on, so a later
    /// quit does not ask about it again.
    void quitCalledOff();

    /// A take is being written right now.
    bool isRecording() const { return recordingEngine.getState() == RecordingState::Recording; }

    /// §10.2 status the main screen shows by default. All plain language --
    /// no sample rates, buffers or backends leak into these.
    juce::String getDestinationFolder() const { return juce::String (destinationFolder); }
    double getElapsedRecordingSeconds() const;
    /// Remaining recording time at the current channel count and format, from
    /// free space on the destination volume. Negative if it cannot be determined.
    double getRemainingRecordingSeconds() const;
    /// Formats a duration as "2h 14m" / "14m 03s" (§6.4: hours and minutes, never bytes).
    static juce::String formatDuration (double seconds);

    /// §10.4: the record button is disabled only for these two reasons, and the
    /// reason is shown next to it. Empty string means enabled.
    juce::String getRecordDisabledReason() const;

    /// §6.4: benchmarks the destination volume by writing a 200 MB file and
    /// measuring the sustained -- never average -- throughput. Runs on its own
    /// thread; the record button stays disabled until it passes. Cached per
    /// volume for 30 days, so a card is benchmarked once and not on every launch.
    void beginPreflightForDestination();
    bool isPreflightRunning() const;

    /// §5.1 master monitor volume, 0-100. Recorded files are unaffected.
    void setMasterVolume (double volume0to100);
    double getMasterVolume() const;

    /// §5.1: the room's mute. Held here, like the volume, because §2.2 and
    /// §5.4 rebuild the coordinator -- and the bus with it -- underneath the
    /// user, and a fresh bus starts unmuted. Toggling also clears a §5 runaway
    /// cut, which the mute button is the way out of.
    void toggleMonitorMute();
    bool isMonitorMuted() const noexcept { return monitorMuted; }

    int getIncludedMicCount() const;

    /// §0.1 / §6.5: everything the mid-take pop-up watches, in one reading:
    /// which microphones are live, which cameras are still listed, whether
    /// sound has been dropped, whether the drive is keeping up, room left.
    TakeHealth snapshotTakeHealth() const;

    /// §0.1: the evidence that the current take is really landing on the
    /// drive -- frames the writer accepted, bytes the session folder holds,
    /// the loudest sample that has arrived. Read on the slow tick.
    ProofReading snapshotProof() const;

    /// The UI walk only: while set, snapshotProof() reports a take whose files
    /// never grow -- nothing accepted, nothing on the drive -- so §0.1's
    /// automatic stop can be walked on a fixture whose drive always answers.
    /// The take itself records normally; only the evidence is withheld.
    void setProofStarvedForTesting (bool starved) noexcept { proofStarvedForTesting = starved; }

    /// §6.5: false while this channel's microphone is unplugged mid-take. The
    /// channel stays in the file writing silence; this is what the UI dashes
    /// its meter on. Index is into the included-mic list, as everywhere else.
    bool isMicLive (int index) const;

        /// §14.6: the mic whose tap/voice was just heard alone, or -1. The UI
    /// highlights that meter so a user can see which meter is which person --
    /// four identical USB mics enumerate with the same product string.
    int getTappedChannel() const noexcept { return tappedChannel; }

    /// §14.6 / §2.4: names a microphone. Persisted against the physical port so
    /// the name follows the mic across replug, shown on its meter, and used in
    /// its stem filename (§6.2).
    struct MicRenameTarget
    {
        std::string deviceKey;
        int deviceChannel = 0;

        bool isValid() const noexcept { return ! deviceKey.empty(); }
    };

    /// Resolve the strip while the rename action begins. The dialog may remain
    /// open across a hot-plug, so its callback must carry this physical target
    /// rather than looking up the same (now possibly different) strip index.
    MicRenameTarget getMicRenameTarget (int index) const;
    void setMicAssignedName (int index, const juce::String& name);
    void setMicAssignedName (const MicRenameTarget& target, const juce::String& name);

    /// §6.2: the user's name for the next take -- "2026-08-27_1030_<name>".
    /// Empty falls back to "Session". Sanitized by SessionFolderNaming.
    void setSessionName (const juce::String& name) { sessionName = name; }
    juce::String getSessionName() const { return sessionName; }

    /// §6.2/§10.1: everything the "where will this go?" question needs
    /// answered, worked out from the settings as they stand and without
    /// creating anything on disk. The pre-record prompt shows this, so a user
    /// knows where to look before there is anything to look for -- §6.2 calls
    /// a novice losing track of their recording a total product failure, and
    /// the cheapest place to prevent it is before the take, not after.
    struct PlannedSave
    {
        juce::String parentFolder;   // "/Users/sam/RECORDINGS"
        juce::String folderName;     // "2026-08-31_1432_Kitchen"
        juce::String fullPath;       // the two joined
        juce::String mirrorFolder;   // §6.3 second copy, empty when none will run
        juce::String mirrorNote;     // why there is no second copy, when that is known
        juce::StringArray fileNames; // "MIX.wav", "01_Alice.wav", ..., "session.json"
    };
    PlannedSave planSave (const juce::String& proposedSessionName) const;

    /// §10.1: whether the user has been shown, and has agreed to, where their
    /// recordings go. False until the pre-record prompt has been answered for
    /// the destination currently set -- changing the destination clears it,
    /// because the answer was about somewhere else.
    bool isSaveLocationConfirmed() const;
    void confirmSaveLocation();
    /// For a user who wants the question every time rather than once a run.
    void setAskWhereToSaveEveryTime (bool ask);
    bool getAskWhereToSaveEveryTime() const { return askWhereToSaveEveryTime; }

    /// The first-run setup guide: false until it has been finished or skipped
    /// once, which is what opens it by itself on a first launch. Finishing and
    /// skipping both count -- someone who skipped it knows where to find it
    /// again (Settings and Help), and asking twice is the §10.1 failure.
    bool isSetupGuideDone() const noexcept { return setupGuideDone; }
    void markSetupGuideDone();

    /// §6.2: the take in progress, and its §6.3 second copy. Empty when idle.
    juce::String getCurrentSessionFolder() const { return currentSessionFolder; }

    /// One file in a session folder, as it stands on disk right now.
    struct SavedFile
    {
        juce::String name;
        int64_t sizeBytes = 0;
    };

    /// What is actually in `folder` at this instant, MIX first, then the stems
    /// in channel order, then session.json. Read live during a take so the
    /// files can be watched growing, and again at stop so what is shown is
    /// what was written rather than what was meant to be.
    static std::vector<SavedFile> listSessionFiles (const juce::String& folder);

    /// listSessionFiles on a disposable detached worker, waited on for at most
    /// `limitMs`. A card pulled or wedged at the wrong moment can leave a
    /// directory read blocked forever; the message thread gives up instead.
    /// `completed` is false when the folder did not answer in time, which is
    /// different from a folder that answered with nothing in it.
    static std::vector<SavedFile> listSessionFilesWithin (const juce::String& folder,
                                                          int limitMs,
                                                          bool& completed);

    /// §6.2: "on stop, show the location and offer to open the containing
    /// folder." Set when a take finishes and consumed once by the UI that
    /// shows it, so the notice is driven by the stop rather than by the UI
    /// having to spot a state edge on a timer.
    struct SavedTake
    {
        juce::String folder;
        juce::String mirrorFolder;
        /// Why this take has no second copy, when the reason is one the user
        /// should hear (it would have shared the recording's disk).
        juce::String mirrorNote;
        std::vector<SavedFile> files;

        /// Decided by the engine, not by the panel. The panel can see file
        /// sizes and nothing else, so it cannot tell a real take from a
        /// full-length silent one -- and a panel reaching its own verdict is
        /// how it once came to warn that files were empty while the status
        /// line beside it said "Saved to ...".
        TakeAudioVerdict verdict = TakeAudioVerdict::NotJudged;

        /// False when the folder did not answer in time, so `files` being
        /// empty means "unknown", not "nothing was written".
        bool filesListed = true;
    };
    bool consumeSavedTake (SavedTake& out);

    /// §4 per-microphone trim, -20..+20 dB in 0.5 dB steps. Persisted against
    /// the physical port so it follows the mic across a replug, and applied
    /// live to the monitor mix and the mix file -- never to the stems.
    void setChannelTrimDb (int index, float trimDb);
    float getChannelTrimDb (int index) const;

    /// The combined device other apps see (macOS: a real CoreAudio aggregate).
    /// The name is the user's -- it is what shows up in Zoom's input list.
    void setAggregateDeviceName (const juce::String& name);
    juce::String getAggregateStatus() const;
    juce::String getAggregateDeviceName() const { return aggregateName; }

    /// §10.3 Advanced panel contents.
    double getSampleRate() const { return currentSampleRate; }

    /// The rates every microphone in the rig can record at, for the Settings
    /// picker. Ascending, capped at §2.2's 48 kHz.
    std::vector<uint32_t> getAvailableSampleRates() const { return availableSampleRates; }

    /// The rate the user pinned, or 0 for automatic.
    uint32_t getSampleRateOverride() const noexcept { return sampleRateOverride; }

    /// Pin a rate, or 0 to go back to automatic. Persists and reopens the
    /// streams, because the rate is fixed for the life of a stream (§5.4).
    void setSampleRateOverride (uint32_t rate);
    int getBitDepth() const { return currentBitDepth; }
    /// Pin the bit depth of every take from now on (16, 24 or 32). Persists.
    void setBitDepthOverride (int bits);

    /// The user's pinned buffer size, or 0 for automatic.
    int getBufferSizeOverride() const noexcept { return bufferSizeOverride; }
    /// Pin the buffer size, or 0 to hand it back to §5.4's ladder. Persists and
    /// reopens the streams, since the size is fixed for the life of a stream.
    void setBufferSizeOverride (int samples);
    double getMeasuredLatencyMs() const { return measuredLatencyMs; }

    /// §5.4: the slowest open microphone's own headphone delay. The figure
    /// above is the quickest's, and on a rig of like devices the two agree;
    /// a device held at a larger buffer, or reporting a longer delay, is that
    /// much later in the headphones (its own channel only), and this says by
    /// how much -- beside the headline figure, and in the take's record.
    double getSlowestMicLatencyMs() const { return slowestMicLatencyMs; }
    juce::String getActiveBackendDescription() const;
    /// Display names of every candidate monitor output, and of every mic that
    /// could serve as §3.1 clock master.
    const std::vector<std::string>& getOutputDeviceNames() const;
    /// §3.2 per-device drift, one line per mic, or a plain line saying the
    /// 60-second measurement window has not elapsed yet (§3.1).
    juce::String getDriftReport() const;
    void setOutputDeviceByName (const juce::String& displayName);

    /// The live mix goes to the combined device: every microphone's headphone
    /// jack at once, so everyone hears everyone.
    bool isMonitoringThroughCombinedDevice() const { return selectedOutputDeviceId == kOurAggregateUid; }

    /// One row per microphone with a headphone jack on the combined device:
    /// whether its wearer hears the mix. Empty unless the mix is going there.
    struct HeadphoneChoice
    {
        juce::String key; // §2.4 identity: two identical mics share a name
        juce::String displayName;
        bool on = true;
    };
    std::vector<HeadphoneChoice> getHeadphoneChoices() const;

    /// Switch one person's headphones on or off. Applies at once, mid-take
    /// too: it changes only what reaches that jack, never the recording.
    void setHeadphonesOn (const juce::String& deviceKey, bool on);

    /// Every microphone the OS reports, in enumeration order, with whether the
    /// user currently has it selected. Includes deselected ones -- the point of
    /// the list is to let them be turned back on.
    struct MicSelection
    {
        /// §2.4 identity, which is what the switches act on. Two identical
        /// microphones share a display name, and matching on it switched the
        /// first one whichever row was clicked.
        juce::String deviceKey;
        juce::String displayName;
        bool enabled = true;
        bool isBuiltIn = false;
        /// How many microphones this one device records. An interface is a
        /// single row here -- it is switched on and off as one thing -- so
        /// without this the list looked identical whether the box had one
        /// microphone on it or four, and a user with two people plugged into an
        /// interface read the single row as the app refusing their second mic.
        int channelCount = 1;

        /// On an interface, one entry per socket: switchable on its own, and
        /// labelled with whatever its person has been named.
        struct Input
        {
            int index = 0;
            juce::String label;
            bool enabled = true;
        };
        std::vector<Input> inputs;
    };
    std::vector<MicSelection> getMicSelections() const;

    /// §10.1: somewhere to save to, chosen by pointing at it rather than by
    /// navigating a file browser to find it. One entry per mounted volume the
    /// app can actually write to, plus the home folder, so an SD card is a
    /// click rather than a path someone has to know.
    struct StorageVolume
    {
        juce::String displayName;   // "UNTITLED (SD card) - 29.8 GB free"
        juce::String path;          // where recordings would go
        bool isRemovable = false;
        bool isCurrent = false;
    };
    std::vector<StorageVolume> getStorageVolumes() const;

    /// The live take's directory snapshot, shared by the proof check and its
    /// on-screen byte count. Both run in the same slow tick; caching that one
    /// snapshot prevents a second per-file stat pass over the card.
    std::vector<SavedFile> getCurrentSessionFiles (bool* snapshotAvailable = nullptr) const;

    /// Point the destination at one of the volumes above, by its path.
    void setDestinationByPath (const juce::String& path);

    /// Ticking or clearing a microphone in Settings. Rebuilds the audio streams
    /// only when the flag actually changed.
    void setMicEnabledByKey (const juce::String& deviceKey, bool enabled);

    /// Switch one socket of an interface on or off. Port memory, so it follows
    /// the box across a replug; reopens the streams outside a take.
    void setInputEnabled (const juce::String& deviceKey, int input, bool enabled);
    void setDestinationFolder (const juce::File& folder);

    /// §5.3 output selection result for the Advanced panel, and the plain-language
    /// line to show when nothing could be selected.
    const std::string& getSelectedOutputDeviceId() const { return selectedOutputDeviceId; }
    const std::string& getSelectedOutputDeviceName() const { return selectedOutputDeviceName; }
    const std::string& getOutputSelectionProblem() const { return outputSelectionProblem; }

    /// §6.5 capacity warnings. Returns a warning the first time each threshold is
    /// crossed, so the caller can poll it every frame without re-warning.
    RemainingTimeWarning pollCapacityWarning();

    /// §5.4: report a callback overrun. Returns true when it pushed the buffer
    /// up a rung, so the caller can tell the user why latency just changed.
    bool noteCallbackOverrun();

    /// After the ladder has stepped: journals it, reopens the streams at the
    /// new size -- or, mid-take, owes that reopen to the take's end, since
    /// §5.4 forbids a change during a recording -- and returns the sentence
    /// the user is shown, with the delay the new size actually costs.
    juce::String applyBufferLadderStep (const juce::String& cause);
    int getCurrentBufferSize() const { return desiredBufferSize(); }

    /// §6.6: called with the current load so pressure is warned about before it
    /// causes dropouts rather than after.
    PerformanceWarning updatePerformance (double cpuLoad, bool thermallyThrottled);

    /// §6.3 redundant local mirror. Default on; turns most card failures from
    /// data loss into inconvenience.
    void setMirrorEnabled (bool enabled);
    /// What the USER set, not whether a copy is happening right now. The
    /// panel's toggle shows the setting, so it has to read the setting;
    /// isMirroring() is false between takes and would flip the box off.
    bool isMirrorEnabledByUser() const { return mirrorPolicy.isEnabledByUser(); }
    bool isMirroring() const { return mirrorPolicy.isMirroring(); }
    MirrorState getMirrorState() const { return mirrorPolicy.getState(); }

    /// Fired on the message thread whenever the capture coordinator's meters
    /// have been destroyed and rebuilt (rename, hot-plug, output change, rate
    /// change). The UI must rebind every Metering pointer inside this callback
    /// -- its own timers dereference them on the very next tick.
    std::function<void()> onCaptureRebuilt;

    /// §10.5 physical setup guidance: everything currently worth telling the
    /// user about their hardware, in plain language, most serious first.
    std::vector<SetupAdvice> getSetupAdvice() const;

    /// Runs the §6.5 capacity check, the §6.6 performance check and the §10.5
    /// level-based detectors from the UI tick, and returns the single most
    /// serious thing worth saying right now -- empty when there is nothing.
    /// §10.6: what happened then what to do, never a code.
    juce::String pollStatusAdvice (double sinceLastCallSeconds);

    /// The recent activity as lines for a panel, newest first, at most `limit`.
    juce::StringArray getRecentActivityLines (int limit = 12) const;

    /// How many entries the user has not been shown yet. What a badge counts.
    size_t getUnseenActivityCount() const { return activity.getUnseenCount(); }
    /// Informational only: a warning or failure that arrives while the panel
    /// is open still owes the user its line above the record button.
    void markActivitySeen() { activity.markInformationalSeen(); }

    /// Records one thing that happened, on the app's own clock. Public because
    /// the UI layer owns some of the events worth recording (a camera the user
    /// switched off, an export they asked for) and they belong in the same
    /// record as everything else.
    ///
    /// Const because recording what happened is observation, not a change to
    /// anything the rest of the class promises -- and because several of the
    /// places that most need to report a failure (working out where a take
    /// would go, planning a save) are const themselves. A journal that only
    /// non-const code could write to would have been silent in exactly those
    /// places.
    void noteActivity (ActivityLevel level, const juce::String& subject, const juce::String& message,
                       bool onTheLine = false) const;

    /// §6.5/§2: says which microphone arrived or left, every time it happens,
    /// whether or not a take is running. Seeded silently on the first
    /// enumeration -- everything present at launch is not news.
    void announceDeviceChanges (const std::vector<MicDeviceState>& seen) const;

    /// The same for cameras, which the take also records. Called after each
    /// re-listing rather than from a device notification, because neither
    /// macOS nor Windows offers one for cameras -- the OS simply stops listing
    /// a camera that has gone.
    void announceCameraChanges();

    /// The same for the things the user listens on. Plugging headphones in is
    /// as much a change to the rig as plugging a microphone in, and it was
    /// mentioned only when it left them with nothing to listen on at all.
    void announceOutputChanges (const std::map<std::string, std::string>& current) const;

    /// Arrivals and departures between two enumerations, said by name.
    /// `known` is updated to `current`, and `seeded` guards the first call:
    /// what was already there at launch is the rig, not news.
    void announceArrivalsAndDepartures (const std::map<std::string, std::string>& current,
                                        std::map<std::string, std::string>& known,
                                        bool& seeded,
                                        const std::set<std::string>& saidElsewhere) const;

        /// §14.2: report an enumeration failure or a device dropping off the bus,
    /// which is how bus-power exhaustion actually presents.
    void noteDeviceDropout();

    /// §8.1: per-block channel peaks, so silent channels are noticed.
    void updateSetupAdvisorLevels (const std::vector<float>& peaksDb, double blockSeconds);

    /// §8.1: one meter per microphone plus one for the shared mix. These are
    /// owned here so they outlive any UI rebuild when mics come and go.
    Metering* getChannelMetering (int index);
    Metering* getMixMetering();
    juce::String getMicDisplayName (int index) const;

    /// The hardware's own product string for that channel, ignoring any name
    /// the user gave the port -- the faint second line under the name. Empty
    /// when it would only repeat what the strip already says, so an unnamed
    /// microphone does not print its product string twice.
    juce::String getMicProductName (int index) const;

    /// §9.3: whether this machine's owner has asked for reduced motion. Read
    /// once at construction -- it is an accessibility preference, not something
    /// that changes between repaints.
    bool prefersReducedMotion() const noexcept { return reducedMotionPreferred; }

    /// The picture side of a take: which cameras are connected, which are in,
    /// and the live views. Video only -- see CameraController for why there is
    /// no audio anywhere near it.
    CameraController& getCameraController() { return cameraController; }
    const CameraController& getCameraController() const { return cameraController; }

    /// The camera choices go through here rather than straight into the
    /// selection, so that every one of them is remembered for next launch in
    /// the one place that knows how to write them down.
    void setCameraEnabled (const std::string& id, bool enabled);
    void setCameraName (const std::string& id, const juce::String& name);
    void setCameraPreviewQuality (PreviewQuality quality);
    void setCameraQuality (const std::string& id, CameraQuality quality);
    PreviewQuality getCameraPreviewQuality() const { return cameraController.getPreviewQuality(); }

    /// Opens cameras the user explicitly enabled. A newly discovered camera is
    /// off, so it cannot raise a privacy prompt on its own; that choice is then
    /// remembered and its main-screen preview may reopen on later launches.
    /// Explicit retries happen from the camera panel and at arm time.
    void openEnabledCameras (bool retryFailures = false);

    /// §10.2: how large the main screen draws the camera pictures, as a step
    /// into MainScreen's size table. Remembered across launches -- it is a
    /// decision about the room, not about this take.
    void setCameraTileScale (int step);
    int getCameraTileScale() const noexcept { return cameraTileScale; }

    /// Whether a finished take also writes one video-with-sound file per
    /// camera, beside the separate picture and sound rather than instead of
    /// them. Off by default: it costs disk and minutes of CPU after every take,
    /// and both files are already saved and already aligned.
    void setCombineVideoAndAudio (bool shouldCombine);
    bool getCombineVideoAndAudio() const noexcept { return combineVideoAndAudio; }

    /// Empty unless the machine has no ffmpeg and the setting is on -- the one
    /// thing this feature needs that the app cannot install for the user.
    juce::String getCombineUnavailableReason();

    /// The opt-in update check. Off by default: PRIVACY.md promises the app
    /// sends nothing anywhere, and this is the one request it can make, so it
    /// is made only once the user has said yes. Switched on, it asks GitHub at
    /// most once a day -- at launch, and when it is first switched on. Nothing
    /// is installed: the user is pointed at the release page and downloads it
    /// themselves.
    void setCheckForUpdates (bool on);
    bool getCheckForUpdates() const noexcept { return checkForUpdates; }

    /// Settings' Check now: asks GitHub immediately, whether or not the
    /// automatic check is on, because pressing the button is the user asking.
    /// Does nothing during a take or while a check is already out.
    void checkForUpdatesNow();

    /// From the status tick. Starts the automatic check once it is due and no
    /// take is running, and hands a result that arrived mid-take to the
    /// activity list once the take is over.
    void pollUpdateCheck();

    /// The line beside Check now: "Checking...", "You have the latest version",
    /// "Version X is available". Empty during a take, which is not the moment.
    juce::String getUpdateStatusText() const;

    /// True when a newer release is known and no take is running -- which is
    /// when Settings offers Download.
    bool isUpdateAvailable() const;
    bool isUpdateCheckRunning() const { return updateCheckRunning->load(); }

    /// Opens the newer release's page in the user's browser. Never downloads
    /// or installs anything itself.
    void openUpdatePage() const;

    /// Which platform the take is being aimed at, by name, or empty for none.
    ///
    /// Streaming services all normalise what they are given to one loudness
    /// figure, so how loud a take is decides what a listener hears -- and peak
    /// level, which is all this app showed before, says nothing about it.
    void setDeliveryTarget (const juce::String& name);
    juce::String getDeliveryTarget() const { return deliveryTarget; }
    static juce::StringArray getDeliveryTargetNames();

    /// What the mix measures, and what to do about it for the chosen platform.
    /// Empty when no platform is chosen.
    juce::String getLoudnessAdvice() const;

    /// The measured figure on its own, for the line that shows it while a take
    /// runs. Empty when there is nothing measurable yet.
    juce::String getLoudnessReading() const;

    /// Show templates: the whole rig's choices saved under a name and brought
    /// back with one click, so a show that recurs is set up once. Each is one
    /// .json file in the support folder's Templates/. See ShowTemplate.
    ///
    /// The names of every saved show, sorted, as the user typed them.
    juce::StringArray listTemplates() const;

    /// Saves the current setup as `name`, replacing a show of the same name.
    /// False, with the reason in the activity list, when it could not be.
    bool saveTemplate (const juce::String& name);

    /// Lays the named show over the current setup and applies it to the live
    /// rig. Refused while a take is running: §6.5 fixes a take's microphones,
    /// names and destination for its duration.
    bool applyTemplate (const juce::String& name);

    bool deleteTemplate (const juce::String& name);

    /// §6.5 "target card removed": set when a take was stopped because the
    /// destination stopped accepting writes, and consumed once by the UI that
    /// alerts about it. Empty the rest of the time.
    bool consumeCardRemovalNotice (CardRemovalNotice& out);

        /// §6.6: takes the app never got to finish -- a force-quit, a power cut, a
    /// pulled card. Found at launch on the destination volume and in the mirror
    /// folder, with their headers repaired, and presented before the main
    /// screen. Empty when the last run ended cleanly, which is the usual case.
    const std::vector<RecoveredSession>& getRecoveredSessions() const;
    /// Recovery touches files, so its launch-time scans run off the message
    /// thread. The UI waits for both one-shot scans before presenting their
    /// combined result; this also guarantees a dismissed card cannot be
    /// reopened by a late second result.
    bool isRecoveryScanPending() const;
    /// Called once the user has been shown them. Also marks each one as
    /// finished on disk, so it is offered once rather than at every launch
    /// until twenty newer takes push it out of the scan. The card disappears
    /// immediately; all removable-volume reads and writes happen on a detached
    /// worker so a card that vanished cannot freeze the interface.
    void clearRecoveredSessions();

    /// §11: diagnostics export -- logs, last 5 session.json files, device
    /// inventory. Never audio.
    void exportDiagnostics (const juce::File& destinationZip);

    /// §2.4/§10.1: the rig as the user last left it -- microphone names and
    /// trims, which mics are switched off, where recordings go, the mirror
    /// setting, the cameras. Beside the log, in the same folder.
    static juce::File getSettingsFile();

    /// §11: where the running log lives. Public so Main.cpp can install the
    /// logger before anything else has a chance to fail.
    static juce::File getLogFile();

    /// The folder the log and the settings share.
    ///
    /// Also where the app's previous name is dealt with: everything §2.4
    /// remembers about a rig was written under it, and the first call after
    /// the rename moves that folder across rather than starting empty.
    static juce::File getSupportFolder();

    /// True when the one-time move of the old settings folder onto the current
    /// name failed. Static because getSupportFolder() is, and it has to be: the
    /// settings file is located before any Application exists to tell. Read
    /// once at startup and turned into a sentence there.
    static bool supportFolderMigrationFailed;

private:
    std::unique_ptr<IAudioBackend> audioBackend;
    std::unique_ptr<CaptureCoordinator> capture;

    // The app's own sounds go through the monitor output when one is open,
    // and through the computer's default output when none is. Which is
    // decided on every call, so a device coming or going switches the route.
    AlarmSpeaker alarmSpeaker;
    double alarmSpeakerRetryAtMs = 0.0;
    AlarmTone& alarmTarget();
    const AlarmTone* currentAlarm() const;
    // What the live coordinator was constructed with, so restartCapture() can
    // tell a plain channel-set change from one that needs a new coordinator.
    double captureRate = 0.0;
    int captureBufferSize = 0;
    double measuredLatencyMs = 0.0;
    double slowestMicLatencyMs = 0.0;
    double driftMeasuredSeconds = 0.0; // §3.1 60-second window

    /// Both headphone figures from the coordinator, as it stands now.
    void refreshMonitoringLatency();

    /// §5.4: "never ship a 40 ms mix silently". Says, once per device, when
    /// one microphone's own path through the headphones is past the 10 ms
    /// ceiling while the headline figure (the quickest's) is well below it,
    /// and again when that clears. reportedSlowMicId is the device last
    /// named, empty when nothing is standing.
    void noteSlowMicrophone();
    std::string reportedSlowMicId;

    // Lifetime token for callbacks marshalled from OS threads; see initialise().
    // Every access goes through the mutex-backed helpers so shutdown cannot
    // race a device callback copying the shared_ptr.
    mutable std::mutex aliveTokenMutex;
    std::shared_ptr<int> aliveToken = std::make_shared<int> (0);
    std::weak_ptr<int> getAliveToken() const;
    void invalidateAliveToken();
    // JUCE calls shutdown(), then destroying the owner calls our destructor,
    // which calls shutdown() again. Guard the external teardown and invalidate
    // queued callbacks on the first entry only.
    bool shutdownStarted = false;
    juce::String currentSessionFolder, currentMirrorFolder, sessionStartIso;
    juce::String sessionName;      // §6.2, set by the user before a take
    juce::String lastSessionFolder;
    // What the last take's folder held at stop, listed once with a deadline.
    // The saved-take card reuses it rather than reading the card a second time.
    std::vector<SavedFile> lastSessionFiles;
    bool lastSessionFilesListed = false;
    // Set the first time a card step of the current take times out, so the
    // steps after it skip the card instead of each waiting out the deadline.
    // Cleared when the next take starts. mutable: writeActivityLog is const.
    mutable bool takeCardUnresponsive = false;
    // One export at a time; the flag is shared with its detached worker.
    std::shared_ptr<std::atomic<bool>> diagnosticsExportRunning
        = std::make_shared<std::atomic<bool>> (false);
    // The update check (see setCheckForUpdates). The running flag is shared
    // with the detached request so a second press cannot start a second one.
    bool checkForUpdates = false;
    double lastUpdateCheckSeconds = 0.0;
    bool automaticUpdateCheckWanted = false;
    std::shared_ptr<std::atomic<bool>> updateCheckRunning
        = std::make_shared<std::atomic<bool>> (false);
    juce::String availableUpdateVersion, availableUpdatePage;
    juce::String updateCheckProblem;
    bool updateCheckSucceeded = false;
    // A newer release learned about mid-take, still owed its activity line.
    bool updateNoticePending = false;
    void startUpdateCheck();
    void finishUpdateCheck (bool reached, const juce::String& responseText);
    juce::String lastMirrorFolder;
    double savedNoticeSeconds = 0.0; // how long "Saved to ..." stays on screen

    // Whether the take that just finished wrote any audio at all, judged once
    // at stop against the finalized files. Keeps the §10.6 status line and the
    // saved-take card telling the same story.
    bool lastTakeHeldNoAudio = false;

    /// What the last take actually held. Distinguishes "nothing arrived" from
    /// "a stream ran and was flat" -- two different causes, two different
    /// things for the user to go and check.
    TakeAudioVerdict lastTakeVerdict = TakeAudioVerdict::NotJudged;

    // §6.2: a take has finished and the UI has not yet shown where it went.
    bool savedTakePending = false;

    // The last take was stopped by a quit, or finished while a quit waited
    // for it: its result counts as unseen even once the card has opened.
    // See prepareToQuit() and unseenProblemWithLastTake().
    bool lastTakeFinishedWhileQuitting = false;

    // setProofStarvedForTesting(): the UI walk's take that writes nothing.
    bool proofStarvedForTesting = false;

    // §6.5's mid-recording row: unplugs and reconnections during a take, kept
    // until the take ends so session.json carries them. RecordingEngine has
    // always tracked this and nothing ever told it anything.
    std::vector<DropoutEntry> midTakeDropouts;

    // Take channels whose input stream died while the device stayed plugged
    // in. The device is still listed, so without this the next device-list
    // refresh would read it as "plugged back in" and relabel a dead stream live.
    //
    // Kept across takes: only a reopen clears it. It used to be cleared at
    // take start while nothing had reopened the stream, so the next take
    // wrote that microphone as silence with its strip lit as live.
    std::set<std::string> deadInputStreams;

    // When restartCapture() last reopened a dead stream, by device, on the
    // millisecond counter. One that dies again inside
    // kDeadInputReopenBackoffMs is left dead rather than reopened in a loop.
    std::map<std::string, double> deadInputReopenedAtMs;

    // When the rig was last reopened to put the headphone output back at its
    // rate after something else switched it, and which output that was.
    // Switched again inside kDeadInputReopenBackoffMs, it is set aside rather
    // than reopened in a tug-of-war (OutputRateChangeAction::setAside).
    double outputRateReopenedAtMs = 0.0;
    std::string outputRateReopenedFor;

    // Whether the mirror was still writing when this take was stopped, sampled
    // before stopRecording() tears the pipeline down. Read after, isMirroring()
    // is always false, so every finished take claimed its backup never ran --
    // and verify_take.py skips the mirror comparison when it reads that, which
    // is exactly the check a mirror that died mid-take needs to fail. -1 means
    // no take has stopped yet, so a mid-take write uses the live flag.
    int mirrorActiveAtStop = -1;

    // The take as it stood when Stop was pressed, for the stop-time
    // session.json. With a camera still finishing its movie that write runs
    // later, when the engine has stopped -- the live clock reads zero then and
    // the counters have moved on. Cleared when a take starts.
    TakeStopSnapshot takeStopSnapshot;

    // §3.3: which device is currently holding the timebase, so a mid-take
    // switchover can be logged once rather than on every status poll.

    // §9.3, read once at construction. See prefersReducedMotion().
    bool reducedMotionPreferred = false;
    juce::String midTakeNotice;
    double midTakeNoticeSeconds = 0.0;

        // §6.5: the take was stopped by the card going away rather than by the
    // user, and that has not been said out loud yet.
    bool cardRemovalPending = false;
    CardRemovalNotice cardRemovalNotice;

    // §10.1: the destination the user has actually been shown and accepted.
    // Compared against destinationFolder rather than being a bare flag, so
    // pointing the app at a different card asks again instead of assuming the
    // old answer covered the new place.
    std::string confirmedSaveLocation;
    bool askWhereToSaveEveryTime = false;

    // Remembered with the rest of the settings; see isSetupGuideDone().
    bool setupGuideDone = false;

    // §14.6 tap-to-name. Rebuilt when the mic count changes, like the
    // fixed-width detectors in SetupAdvisor.
    CameraController cameraController;
    TakeCombiner takeCombiner;
    PodcastExporter podcastExporter;
    std::function<void()> pendingStoppedTakeCompletion;
    bool combineVideoAndAudio = false;
    juce::String deliveryTarget;

    std::unique_ptr<TapToNameDetector> tapDetector;
    int tapDetectorChannels = 0;
    // Negative so the first ambiguous tap after launch is always shown.
    double lastAmbiguousTapNoticeMs = -1.0e12;
    static constexpr double kAmbiguousTapNoticeIntervalMs = 600000.0;
    int tappedChannel = -1;

    struct PreflightBackgroundResult
    {
        std::string destination;
        PreflightResult result;
        bool couldNotWrite = false;
    };

    // §6.4 preflight. The detached task owns every value it probes and only
    // publishes a result into shared state. A stale volume may retain that
    // worker, but neither Application destruction nor shutdown ever joins it.
    mutable DetachedResultTask<PreflightBackgroundResult> preflightTask;
    mutable std::string preflightTaskDestination;
    mutable std::map<std::string, PreflightResult> preflightResults;
    /// When each verdict above landed, so a "couldn't write" one can be re-run
    /// after PreflightThroughputTest::kWriteFailureRetrySeconds.
    mutable std::map<std::string, double> preflightVerdictAtMs;
    static std::string filesystemTypeName (const juce::File& folder);
    static int probeWriteErrno (const juce::File& folder);
    static PreflightBackgroundResult runPreflight (
        std::string destination, int channelCount, double sampleRate,
        int bytesPerSample, const std::atomic<bool>& cancelled);
    /// Consumes any completed verdict and atomically reports whether its
    /// worker is still running. This combined observation is what keeps Record
    /// fail-closed at the instant a background benchmark publishes.
    bool publishCompletedPreflight() const;
    void startPreflightIfNeeded() const;
    std::unique_ptr<VirtualDeviceBackend> virtualDeviceBackend;
    std::unique_ptr<SystemAggregateDevice> systemAggregate;
    juce::String aggregateName { "SobStage" };
    // What was last published, so device-change churn republishes only on a
    // real difference -- destroying a device another app is recording from is
    // justified when hardware changed, never as a side effect of a no-op.
    std::vector<std::string> publishedUids;
    std::string publishedMaster, publishedNameStd;

    /// Republishes when the rig changed. Returns true when the combined device
    /// was replaced, which takes away any stream open on the old one.
    bool publishAggregateDevice();

    /// A republish held back because the take's headphone mix is playing
    /// through the combined device; Stop pays it.
    bool aggregateRepublishDeferred = false;

    bool monitorThroughCombinedDevice = true;
    std::set<std::string> headphonesOffKeys;

    /// The combined device's jacks as of the last output selection, so the
    /// Settings refresh does not ask the HAL twice a second.
    std::vector<CombinedDeviceOutputs> combinedOutputLayout;
    /// Lays the per-jack headphone gains onto the output stream that is open
    /// now (applyHeadphoneGains) or about to open (applyHeadphoneGainsFor).
    void applyHeadphoneGains();
    void applyHeadphoneGainsFor (const std::string& outputDeviceId);

    /// Everything a rig rebuild opens, as text. A device-list notification
    /// that leaves this unchanged on a healthy rig -- the app's own combined
    /// device appearing is the usual one -- has nothing to reopen.
    std::string describeRig() const;
    std::string lastStartedRigSignature;

    /// A mid-take pass kept a missing mic because its audio was still
    /// arriving; one more pass shortly after settles whether it really left.
    bool recheckDeviceListSoon = false;
    void scheduleDeviceListRecheckIfDue();

    DeviceManager deviceManager;
    RecordingEngine recordingEngine;

    /// §6.6: keeps the Mac from idling to sleep (display or system) while a
    /// take records. Released on stop, in shutdown(), and by its destructor.
    SleepInhibitor sleepInhibitor;
    void syncSleepInhibitor();

    /// §6.6: on battery a closed lid still sleeps the Mac and ends the take,
    /// so a take recording on battery says so once.
    BatteryRecordingNotice batteryRecordingNotice;
    PortIdentityStore portIdentityStore;

    std::string selectedOutputDeviceId;

    /// Whether that output was a headphone jack when it was chosen, so an
    /// Intel Mac's built-in output that goes back to its speakers when the
    /// headphones come out is not kept as if nothing had changed. See
    /// OutputDeviceSelector::currentIdToKeep.
    bool selectedOutputWasHeadphoneJack = false;
    std::string selectedOutputDeviceName;
    std::string outputSelectionProblem;

    /// The last output problem written to the journal, so a session with no
    /// headphones says so once rather than at every device notification.
    std::string reportedOutputProblem;
    std::string rememberedOutputDeviceId;
    std::vector<std::string> outputDeviceNames; // §2: refreshed on device change only
    std::map<std::string, std::string> outputDeviceIdByLabel;
    OutputDeviceTracker outputDeviceTracker;
    CapacityMonitor capacityMonitor;
    BufferLadder bufferLadder;

    /// The device identity keys as of the last enumeration, so §5.4's ladder
    /// can tell a real hot-plug from this handler simply running again.
    std::string lastDeviceSignature;
    CpuPressureMonitor cpuPressureMonitor;

    double recordingStartMs = 0.0;
    /// The ladder's log size when the take began: its session.json records
    /// only the steps from here on.
    std::size_t takeBufferChangeStart = 0;
    double currentSampleRate = 48000.0;
    int currentBitDepth = 24;
    // Buffer size lives in bufferLadder, which is the only thing allowed to
    // change it (§5.4). Keeping a second copy here would let the two disagree.

    // The running take's own format and roster, fixed in toggleRecording once
    // its files are open. currentSampleRate/currentBitDepth and `included` are
    // the NEXT take's (Settings, a replug or a tick box can move them
    // mid-take), so session.json and the combined file read these instead.
    double takeSampleRate = 0.0;
    int takeBitDepth = 0;
    std::vector<DeviceRecord> takeDevices;
    int takeAlignedInputLatencyFrames = 0;
    double takeMeasuredLatencyMs = 0.0;
    double takeSlowestMicLatencyMs = 0.0;
    bool takeStemsAligned = false;

    /// Copies how the writer lined each take device's stems up -- input
    /// latency, IO block, the offset it was held back by, the silence written
    /// and samples taken out mid-take to keep it in step, and the silence its
    /// own stream left in its stems when its IO block grew -- and whether all of
    /// that was done exactly, into the take's record, and refreshes the
    /// headphone latency figure. At the take's start, before every mid-take
    /// refresh of its record, and again as it stops: a device whose IO size
    /// grew during the take moved every other stem's alignment with it (never
    /// the headphones), and a crash's record must carry that too.
    void recordTakeAlignment();

    std::string destinationFolder;

    // §10.1/§10.4. What the OS says about our privacy permissions, sampled off
    // the audio path: the microphone answer at launch and again every couple
    // of seconds from the status poll and on every device-list change (the
    // first-run prompt is usually answered AFTER launch, and access can be
    // revoked while running), the destination answer whenever the save
    // location changes.
    PermissionState microphonePermission = PermissionState::NotApplicable;
    // macOS Restricted (an administrator's policy) rather than the user's own
    // refusal. Read alongside microphonePermission; only the wording differs.
    bool microphoneRestricted = false;
    PermissionState destinationWritePermission = PermissionState::NotApplicable;
    /// Latched so the journal entry is written once, not on every poll.
    bool journalledPermissionProblems = false;
    double secondsSinceMicrophonePermissionCheck = 0.0;

    /// Reopens the microphones, with backoff, while none of them is open. A
    /// failed open was otherwise final: fixing the cause (the interface's
    /// rate, another app holding it) fires no notification the app hears.
    FailedOpenRetry failedOpenRetry;

    /// Re-reads the microphone permission (cheap; never prompts). On a change
    /// it stores the answer and re-arms the permission journal entry, and the
    /// record gate reads the new answer on its next evaluation. Returns true
    /// when access has just been granted, so the streams opened without it
    /// must be reopened once.
    bool refreshMicrophonePermission();
    MirrorPolicy mirrorPolicy;
    SetupAdvisor setupAdvisor;
    mutable ActivityJournal activity;

    // Free-space and per-file stat calls live on this worker; status getters
    // submit an in-memory request and consume its latest snapshot only.
    mutable FilesystemStatusProbe filesystemStatusProbe;
    FilesystemStatusProbe::Request currentFilesystemProbeRequest() const;
    int64_t getMirrorFreeBytes() const;

    static std::vector<StorageVolume> scanStorageVolumes();
    mutable DetachedRefreshCache<std::vector<StorageVolume>> storageVolumeCache {
        std::vector<StorageVolume> {}
    };

    /// What the last enumeration held, by identity key, with the name to call
    /// each one by. The diff against this is what makes an arrival or a
    /// departure sayable at all.
    mutable std::map<std::string, std::string> knownDeviceNames;
    mutable bool haveEnumeratedDevicesOnce = false;

    mutable std::map<std::string, std::string> knownOutputNames;
    mutable bool haveAnnouncedOutputsOnce = false;

    mutable std::map<std::string, std::string> knownCameraNames;
    mutable bool haveAnnouncedCamerasOnce = false;

    /// Seconds since the app started, for journal timestamps. One clock for
    /// every entry, so entries can be compared with each other and with the
    /// take's own elapsed time.
    double appStartMs = 0.0;
    double activityClockSeconds() const;

    /// Writes activity.log beside a take, so the folder carries its own story.
    void writeActivityLog (const juce::File& folder) const;

    /// Keeps the take's log on disk in step with the journal, so a take that
    /// ends in a crash still explains itself. Does nothing outside a take.
    void flushActivityLogToTake() const;

    /// Stops writeActivityLog's own failure note from re-entering the flush.
    mutable bool writingActivityLog = false;

    /// §10.6: why the last attempt to start recording failed. Shown until the
    /// next attempt, because a record button that does nothing and says nothing
    /// is the worst failure in the app: the user believes they are recording.
    juce::String recordStartProblem;

    /// How many microphones the last journalled monitoring change was about,
    /// and whether that change was a success. restartCapture() runs on every
    /// rename, hot-plug and output change, so an entry per call would bury the
    /// journal in "2 microphones are live" -- only an actual change is news.
    int journalledMonitorCount = -1;
    int journalledMonitorFailedCount = -1;
    bool journalledMonitorOk = false;

    /// The backend drop count already reported, so a loss that is still growing
    /// is said again and one that has stopped is not repeated forever.
    uint64_t reportedBackendDrops = 0;
    // §5.4: ring loss events already handed to the buffer ladder, so each is
    // counted once. Falls back to zero with the streams' own counters.
    uint64_t reportedRingLossEvents = 0;

    /// The backend's drop total when the current take started, so the take's
    /// own record reports its own losses rather than the session's.
    uint64_t backendDropsAtTakeStart = 0;

    /// The monitor-glitch count already reported. Same shape as
    /// reportedBackendDrops, including the reset when the streams are rebuilt.
    uint64_t reportedOutputGlitches = 0;

    /// The camera problem already journalled, so an open failure that persists
    /// across takes is said once rather than at every take start.
    juce::String reportedCameraProblem;

    /// Layout losses already reported live. Reset with each take, because the
    /// counter behind it is.
    uint64_t reportedLayoutMisses = 0;

    /// Why the app stopped the current take, when the app is what stopped it.
    /// Empty means the user did, which is the ordinary case.
    juce::String stopReason;

    /// True once the mirror-never-opened line has been said for this take, so
    /// it is said once rather than on every poll.
    bool mirrorMissingReported = false;
    /// Said once per take: a backup that stopped for space and then failed to
    /// close its files, which the low-space notice alone does not convey.
    bool mirrorFinalizeFailureReported = false;
    /// The backup was stopped for space by this poll's safety step; its line is
    /// said once, at its old priority, when nothing more urgent holds the line.
    bool mirrorLowSpaceNoticePending = false;
    /// The destination for which "no backup: same disk" has been said, so it
    /// is said once per destination rather than at every take.
    juce::String sameDiskBackupNoticeFor;
    /// Whether the take just finished went without a backup for that reason,
    /// for the saved-take card.
    bool lastTakeBackupSkippedForSameDisk = false;

    /// §6.3: whether the backup folder is on the same disk as the current
    /// destination, asked of the system directly. Arm time only: it can block
    /// on a disappearing card the way creating the take's folder there can.
    bool backupWouldShareDestinationDisk() const;
    /// The backup folder's root, ~/RECORDINGS-MIRROR.
    static juce::File backupRootFolder();
    /// Test builds' MMA_KEEP_SAME_DISK_BACKUP=1: keep the backup on the same
    /// disk so the end-to-end walks, which record into one scratch home, still
    /// exercise it. Always false in a shipping build.
    static bool keepSameDiskBackupForTesting();

    /// The journal entry currently being shown on the advice line, and how much
    /// longer it stays there. This is what makes "nothing is silent" true on
    /// the main screen: when nothing more serious is happening, the line shows
    /// the most serious thing the user has not been shown yet, rather than
    /// going blank over a journal full of unread news.
    juce::String activityLine;
    double activityLineSeconds = 0.0;

    /// The last take's combine outcome that has already been reported, so a
    /// finished combine is announced once. Kept by run, not by wording: two
    /// takes in a row whose combine failed for the same reason are two
    /// failures, and matching the sentence left the second one unreported.
    juce::uint64 reportedCombineRun = 0;

    /// The last combine run whose finished files have been announced.
    juce::uint64 announcedCombineRun = 0;

    // §5.1 listening level. Owned here, not on the bus, because the coordinator
    // that owns the bus is rebuilt on a rate or buffer change.
    double masterVolume = MonitorBus::kDefaultMonitorVolume;
    int captureGeneration = 0;
    bool monitorMuted = false;
    int speakerHandoversForMonitorOpen = 0;

    /// (Re)opens the streams for the current mic set and output device. §5.1
    /// makes monitoring live from launch, and a hot-plug changes the channel
    /// set, so this runs at startup and on every device-list change.
    void restartCapture();

    /// restartCapture() unless a take is running, in which case the restart
    /// is owed and happens when the take stops. Every change refused mid-take
    /// (a mic plugged in, an unplug, a rename, a rate) used to stay unapplied
    /// forever: the next take planned N+1 files and wrote N.
    void requestCaptureRestart();
    bool captureRestartDeferred = false;

    /// A destination chosen while a take was running. §6.5 fixes where a take
    /// goes for its duration, so the change waits for the take to end rather
    /// than being dropped -- the same bargain requestCaptureRestart strikes.
    juce::String pendingDestinationFolder;

    /// Does what setDestinationFolder does, without asking whether a take is
    /// running. The deferral decision lives in setDestinationFolder alone, so
    /// the deferred apply cannot re-enter it and defer itself -- which is
    /// exactly what it did when both jobs lived in one function.
    void applyDestinationFolder (const juce::File& folder);
    void applyClockMaster();


    /// The name one INPUT of a device is known by -- the port's name resolved
    /// live, so a rename mid-take reaches the strip, plus the input number that
    /// tells an interface's microphones apart.
    ///
    /// Answering per device cannot do this: asked about four inputs of one
    /// interface it gave the same name four times, while the four files were
    /// correctly named apart. The screen and the disk disagreed about who was
    /// who. Empty when the device is no longer enumerated at all -- exactly the
    /// case mid-take, where the channel outlives the microphone unplugged from
    /// it, and the caller falls back to the name the channel opened with.
    juce::String nameForChannel (const std::string& identityKey, int deviceChannel) const;

    /// The rig as the channel planner sees it: every included device, with
    /// §2.4's remembered name and §2.1 verdict already resolved.
    std::vector<ChannelPlanDevice> planDevices() const;

    std::vector<CaptureChannel> buildCaptureChannels() const;

    /// Collects completed §2.1 decisions from the audio callback, remembers
    /// them per physical port, and rebuilds the channel plan. Called on the
    /// message thread and deliberately does nothing during a take, so a
    /// verdict can never change an in-progress file layout.
    void applyChannelLayoutDecisions();
    /// §6.2 folder name for a take started at `now` under `name`, including the
    /// collision suffix. Resolves against the disk but creates nothing, so the
    /// pre-record prompt and the take itself agree on the answer.
    juce::String resolveSessionFolderName (juce::Time now, const juce::String& name) const;
    /// §6.2 destination folder for a new take, created on disk. Empty on
    /// failure, with the reason in `problem`.
    juce::String createSessionFolder (juce::Time now, juce::String& problem) const;
    /// §6.3 local backup folder for a take, created on disk. Empty on failure.
    juce::String createMirrorFolder (const juce::String& sessionFolderName) const;
    /// §6.2 session.json, written at start and rewritten at stop.
    void writeSessionMetadata (bool sessionHasStopped);
    std::string buildSessionMetadataJson (bool sessionHasStopped);

    /// §6.2's record refreshed during the take -- every 30 s, and soon after a
    /// dropout, a camera starting, the backup stopping, a loss or a buffer
    /// step (SessionRecordSchedule) -- on workers, so a crash leaves an
    /// accurate session.json and the message thread never waits on the card
    /// for it. At Stop the card's are retired before its stop-time record;
    /// the backup's stop-time record goes through its writer (writeLast), so
    /// no refresh can land on top of either.
    std::unique_ptr<BackgroundRecordWriter> sessionRecordCard, sessionRecordMirror;
    SessionRecordSchedule sessionRecordSchedule;
    std::string sessionRecordEventSignature() const;
    void startSessionRecordRefreshes();
    void refreshSessionRecordIfDue();
    void retireSessionRecordRefreshes();

    /// Points currentSessionFolder at wherever the take's open files now are,
    /// when the folder was renamed or moved in Finder mid-take. Without it the
    /// file-growth check watched a folder that no longer existed ("the files
    /// have stopped growing") and session.json and activity.log went to the
    /// old path at Stop and failed.
    void followRenamedTakeFolder();
    /// The same, after the audio has stopped: the camera movies finish later,
    /// and everything that consumes them (the stop-time session.json, the
    /// saved-take card's list, the combined video) runs only then. Asks the
    /// folder itself, held open for the take by takeFolderTracker.
    void followTakeFolderMovedAfterAudioStopped();
    TakeFolderTracker takeFolderTracker;
    /// The figures session.json reports, read from the live take now.
    TakeFigures liveTakeFigures (bool sessionHasStopped) const;
    /// Writes a take's text file with a deadline, skipping the card once it
    /// has stopped answering this take. The local backup is always written.
    bool writeTakeText (const juce::File& file, const juce::String& text) const;
    /// True unless `file` is inside the current take's local backup folder.
    bool isOnCard (const juce::File& file) const;
    /// §11: the newest session.json files under `root`, newest first. Static
    /// because it runs on the diagnostics export's detached worker.
    static juce::Array<juce::File> findRecentSessionMetadata (const juce::String& root, int maximum);
    /// Re-reads the microphone permission, then rescanDevices().
    void onDeviceListChanged();
    /// The device-list pass itself, for a caller that has just called
    /// refreshMicrophonePermission() and must not pay for a second read.
    void rescanDevices();
    void chooseInitialDestination();

    /// Read once at launch, written whenever a remembered setting changes and
    /// again at shutdown. Saving is not debounced: the file is a few hundred
    /// bytes in the user's application-data folder, never on the card a take is
    /// being written to, so §14.3's contention worry does not apply to it.
    void loadSettings();
    void saveSettings();

    /// Where show templates live, what is in that folder, and the file the
    /// show listed as `name` is kept in -- an invalid File when there is none.
    static juce::File getTemplatesFolder();
    static std::vector<ShowTemplate::StoredFile> scanTemplateFiles();
    /// Removes the hidden temporary files a crash strands mid-replace of the
    /// settings, a show, or the camera start guard. Launch only.
    static void sweepAbandonedSafeWriteTemps();
    static juce::File getTemplateFile (const juce::String& name);

    struct RecoveryActivity
    {
        ActivityLevel level = ActivityLevel::Started;
        std::string subject;
        std::string message;
    };

    struct RecoveryBackgroundResult
    {
        std::string root;
        bool isDestinationCopy = false;
        std::vector<RecoveredSession> sessions;
        std::vector<RecoveryActivity> activity;
        /// The root was not there to scan -- a card not mounted yet.
        bool rootMissing = false;
    };

    struct RecoveryAcknowledgementResult
    {
        std::vector<RecoveryActivity> activity;
    };

    enum class RecoveryScanStatus { NotStarted, Running, Succeeded, Failed };

    /// §6.6: schedules separate detached walks of the destination and mirror
    /// for takes with no stop timestamp. A take cannot start while the scans
    /// that could touch either of its write roots are pending. Each worker owns
    /// all paths/results and publication into Application is message-thread-only.
    void scanForInterruptedSessions();
    void startDestinationRecoveryScan();
    void startMirrorRecoveryScan();
    /// True when no take, combined video or podcast-ready copy is being
    /// written, so a recovery scan may remove their abandoned working files.
    bool nothingIsWritingAfterATake() const;
    static RecoveryBackgroundResult runRecoveryScan (
        std::string root, bool isDestinationCopy, bool sweepWorkingFiles,
        const std::atomic<bool>& cancelled);
    static RecoveryAcknowledgementResult runRecoveryAcknowledgement (
        std::vector<std::string> sessionFolders, std::string recoveredAt,
        const std::atomic<bool>& cancelled);
    void publishCompletedRecoveryScans() const;
    void publishCompletedRecoveryAcknowledgement() const;
    juce::String recoveryBlockingReason() const;
    mutable DetachedResultTask<RecoveryBackgroundResult> destinationRecoveryTask;
    mutable DetachedResultTask<RecoveryBackgroundResult> mirrorRecoveryTask;
    mutable DetachedResultTask<RecoveryAcknowledgementResult> recoveryAcknowledgementTask;
    /// A cancelled detached recovery or preflight worker can still be inside
    /// an OS write. Its lease keeps that root unavailable until the worker has
    /// truly returned, so a replacement check or new take can never race the
    /// late mutation.
    mutable DetachedPathMutationGate recoveryMutationGate;
    std::string destinationRecoveryRoot;
    std::string mirrorRecoveryRoot;
    mutable RecoveryScanStatus destinationRecoveryStatus = RecoveryScanStatus::NotStarted;
    mutable RecoveryScanStatus mirrorRecoveryStatus = RecoveryScanStatus::NotStarted;
    /// The destination scan finished because there was nothing at that path
    /// yet. Its "nothing interrupted here" holds only until the card mounts.
    mutable bool destinationRecoveryRootWasMissing = false;
    mutable double destinationRecoveryMissingAtMs = 0.0;
    /// Runs the destination scan again once the drive check has written to a
    /// destination that was missing when it was scanned.
    void rescanDestinationIfItHasAppeared();

    /// §6.4: a drive check describes the card it measured, and the save
    /// location is a path that another card can take over -- macOS mounts
    /// every card at /Volumes/<its label>. True when the status probe, in a
    /// look begun after `verdict` was reached, found a different volume there
    /// (or none). Reads only the probe's last answer, never the drive.
    bool probeSeesAnotherDriveThanChecked (const PreflightResult& verdict) const;
    /// Between takes, from the status tick: when the probe sees a different
    /// drive at the save location than the one checked, forgets that check and
    /// checks the drive that is there now, for speed, format and interrupted
    /// takes alike.
    void recheckDestinationIfItsDriveChanged();
    /// What applyDestinationFolder does for a fresh claim about the same
    /// path: drops the drive check and the interrupted-take scan, and starts
    /// both again for whatever is there now. Says so in the activity list.
    void forgetCheckedDriveAtDestination();
    /// At Record, the drive at the save location is looked at once more, under
    /// a deadline, and compared with the one checked: the probe's last look
    /// can be half a second old. Empty when it is the same drive (or nothing
    /// can be told); otherwise why the take can't start, with the check
    /// already restarted for the drive that is there.
    juce::String confirmDestinationIsTheCheckedDrive();
    mutable std::vector<RecoveredSession> recoveredSessions;
    /// Folders of same-named copies hidden behind a shown recovered take.
    mutable std::vector<std::string> hiddenRecoveredFolders;

    /// Pushes the remembered per-port names and trims, and the microphones the
    /// user switched off, onto the device list as it currently stands. Runs
    /// after every enumeration, not just the first: a microphone remembered
    /// from last week is only matchable once it has actually been plugged in.
    void applyRememberedDeviceSettings();

    /// What was read at launch. Kept because devices and cameras arrive later
    /// than the file does, so it has to stay around to be matched against them.
    int cameraTileScale = 5;
    AppSettings rememberedSettings;

    /// The rates every included microphone can reach, for the Settings picker.
    std::vector<uint32_t> availableSampleRates;

    /// A rate the user pinned, or 0 for automatic (§2.2 decides).
    uint32_t sampleRateOverride = 0;

    /// Pinned in Settings, or 0 for automatic. The ladder still runs underneath
    /// so handing control back later resumes from where it would have been.
    int bufferSizeOverride = 0;

    /// The buffer the streams are opened with: the user's pin, else the ladder.
    int desiredBufferSize() const noexcept
    {
        return bufferSizeOverride > 0 ? bufferSizeOverride : bufferLadder.getCurrentSize();
    }
    /// Suppresses saving while the loaded settings are still being applied, so
    /// a half-applied rig cannot be written back over a complete one.
    bool applyingRememberedSettings = false;
    void reselectOutputDevice();
    double bytesPerSecondOfAudio() const;
    /// Audio plus whatever the enabled cameras are estimated to add. This, not
    /// the audio figure, is what the destination volume actually has to hold --
    /// "Room for 8h" with two cameras running would be off by an order of
    /// magnitude, and §6.5's whole point is that a novice cannot act on a
    /// surprise part-way through.
    double bytesPerSecondOfRecording() const;
    int64_t projectedSessionBytes() const;
    std::unique_ptr<IAudioBackend> createPlatformBackend();
    std::unique_ptr<VirtualDeviceBackend> createDefaultVirtualDeviceBackend();
};

} // namespace mma
