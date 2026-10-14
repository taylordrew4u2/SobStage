#include "Application.h"
#include "../Platform/ReducedMotion.h"
#include "../Platform/SystemPermissions.h"
#include "../Platform/VolumeCapacity.h"
#include "../Core/PreflightScratchFile.h"
#if JUCE_MAC
 #include <sys/mount.h>
#endif
#if JUCE_MAC || JUCE_LINUX
 #include <cerrno>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif
#include "../Core/TakeCompleteness.h"
#include "../Platform/SystemThermalState.h"
#include "../Core/CombinedTakePlan.h"
#include "../Core/LoudnessMeter.h"
#include "../Core/SampleRateNegotiator.h"
#include "../Core/WriteSafetyActions.h"
#include "../Core/UpdateCheck.h"
#include "../Core/ShowTemplate.h"
#include "../Core/SlowMicrophoneNotice.h"
#include "../Core/QuitQuestions.h"
#include "../Platform/NullBackend.h"
#include "CheckedTextWrite.h"
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#if JUCE_MAC || defined (__linux__)
#include <dirent.h>
#endif

#if JUCE_MAC
#include "../Platform/CoreAudioBackend.h"
#elif JUCE_WINDOWS
#include "../Platform/WasapiAsioBackend.h"
#elif defined(__linux__) && ! defined(MMA_NO_ALSA)
#include "../Platform/AlsaBackend.h"
#endif

namespace mma {

namespace {

std::vector<std::string> mutationAliasesForRoots (const std::vector<std::string>& roots)
{
    std::vector<std::string> aliases;
    aliases.reserve (roots.size() * 2);

    for (const auto& root : roots)
    {
        if (root.empty())
            continue;

        aliases.push_back (root);

        // weakly_canonical resolves every existing symlink/alias prefix while
        // still accepting a take folder which has not been created yet. This
        // may enter a stale removable volume, so this helper is called only by
        // detached workers, never by the message thread.
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical (
            std::filesystem::u8path (root), error);

        if (! error)
        {
            const auto resolved = canonical.u8string();
            if (! resolved.empty())
                aliases.push_back (resolved);
        }
    }

    return aliases;
}

DetachedPathMutationGate::LeasePtr waitForMutationLease (
    DetachedPathMutationGate gate,
    const std::vector<std::string>& roots,
    const std::atomic<bool>& cancelled)
{
    const auto aliases = mutationAliasesForRoots (roots);

    while (! cancelled.load (std::memory_order_acquire))
    {
        if (auto lease = gate.tryAcquire (aliases))
            return lease;

        // A prior detached syscall may never return. This worker is disposable
        // too, so polling here never blocks the message thread and selecting a
        // genuinely different root can abandon it immediately.
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    return {};
}

// Runs `work` on a disposable detached thread and waits at most `limit` for
// its answer. A removable volume pulled or wedged at the wrong moment can
// leave any filesystem call blocked forever, and on the message thread that is
// a frozen window. The worker owns only copies, never Application, so giving
// up on it is safe; it finishes (or never does) on its own.
template <typename Result, typename Work>
std::optional<Result> runWithDeadline (Work work, std::chrono::milliseconds limit)
{
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();

    std::thread ([promise, work = std::move (work)]() mutable
    {
        try { promise->set_value (work()); }
        catch (...) { promise->set_exception (std::current_exception()); }
    }).detach();

    if (future.wait_for (limit) != std::future_status::ready)
        return std::nullopt;

    try { return future.get(); }
    catch (...) { return std::nullopt; }
}

// Matches the production input-open deadline: long enough for a slow card to
// wake, short enough that nobody reaches for Force Quit.
constexpr std::chrono::milliseconds kRemovableVolumeDeadline { 5000 };

// §6.3: said when the backup copy is stopped because its disk is nearly full.
constexpr const char* kMirrorLowSpaceLine =
    "The local backup copy stopped -- this drive is low on space. The recording itself is unaffected.";

// The version CMake stamped in. JUCE_APP_VERSION is only defined for builds
// that go through JuceHeader.h, so stringifying it here wrote the literal
// token "JUCE_APP_VERSION" into every take's session.json and every
// diagnostics bundle -- the two places support would look to find out which
// build produced a recording.
juce::String appVersionString()
{
   #if defined (SOBSTAGE_VERSION_STRING)
    return SOBSTAGE_VERSION_STRING;
   #else
    return "dev";
   #endif
}

// replaceWithTextChecked (CheckedTextWrite.h): everything durable this app
// writes goes through it, so a write that did not survive is a write the user
// hears about -- and, on a full disk, one that leaves the previous file intact
// rather than renaming an empty one over it.

// replaceWithTextChecked for files on the card, from the message thread. A
// card pulled mid-write can hold the write forever; past the deadline this
// gives up, leaving the write to finish (or not) on its own worker. Empty means
// the deadline passed, which is different from a write that answered "failed".
std::optional<bool> replaceWithTextWithin (const juce::File& file, const juce::String& text)
{
    return runWithDeadline<bool> ([file, text] { return replaceWithTextChecked (file, text); },
                                  kRemovableVolumeDeadline);
}

std::vector<juce::File> directChildDirectories (const juce::File& root)
{
    std::vector<juce::File> result;

   #if JUCE_MAC || defined (__linux__)
    // JUCE's macOS File::findChildFiles path goes through
    // NSAllDescendantPathsEnumerator, which has been observed waiting forever
    // when /Volumes contains a disappearing capture disk. These are flat mount
    // roots, so use the native non-recursive directory API directly. The
    // caller validates each candidate on its background worker.
    const auto path = root.getFullPathName();
    std::unique_ptr<DIR, decltype (&::closedir)> directory (
        ::opendir (path.toRawUTF8()), &::closedir);

    if (directory == nullptr)
        return result;

    while (const auto* entry = ::readdir (directory.get()))
    {
        const juce::String name = juce::String::fromUTF8 (entry->d_name);

        if (name == "." || name == "..")
            continue;

       #if defined (DT_DIR) && defined (DT_LNK) && defined (DT_UNKNOWN)
        if (entry->d_type != DT_DIR
            && entry->d_type != DT_LNK
            && entry->d_type != DT_UNKNOWN)
            continue;
       #endif

        result.push_back (root.getChildFile (name));
    }

   #else
    for (const auto& child : root.findChildFiles (juce::File::findDirectories, false))
        result.push_back (child);
   #endif

    return result;
}

Application::StorageVolume homeStorageVolumeFallback()
{
    const auto destination = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getChildFile ("RECORDINGS");
    return { "Home folder", destination.getFullPathName(), false, false };
}

juce::String baseSessionFolderName (juce::Time now, const juce::String& name)
{
    auto cleaned = SessionFolderNaming::sanitizeName (name.toStdString());
    if (cleaned.empty())
        cleaned = SessionFolderNaming::kDefaultName;

    return juce::String (SessionFolderNaming::buildFolderName (
        now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
        now.getHours(), now.getMinutes(), cleaned));
}

// Whether this port records as more than one channel, and so keeps a trim per
// input the way it keeps a name per input.
bool keepsTrimPerInput (const MicDeviceState& d, const PersistedDeviceSettings& settings)
{
    const bool knownDuplicateStereo = settings.hasChannelLayoutDecision && settings.channelLayoutIsMono;
    return takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo) > 1;
}

// session.json's trim for a device: the box's for a single microphone, and one
// per recorded input for an interface, whose inputs are trimmed separately.
void recordTrims (DeviceRecord& record, const MicDeviceState& d, const PersistedDeviceSettings& settings)
{
    record.trimDb = settings.trimDb;
    record.inputTrimDb.clear();

    if (! keepsTrimPerInput (d, settings))
        return;

    for (int input = 0; input < d.inputChannelCount; ++input)
        if (std::find (settings.disabledInputs.begin(), settings.disabledInputs.end(), input)
                == settings.disabledInputs.end())
            record.inputTrimDb[input] = settings.trimDbForInput (input);
}

} // namespace

Application::Application()
    // §9.3, asked once. The setting does not change between repaints, and the
    // alternative -- querying the OS per frame at 60Hz -- would be absurd.
    : reducedMotionPreferred (prefersReducedMotionOnThisSystem())
{
}
Application::~Application() { shutdown(); }

std::weak_ptr<int> Application::getAliveToken() const
{
    const std::lock_guard<std::mutex> guard (aliveTokenMutex);
    return aliveToken;
}

void Application::invalidateAliveToken()
{
    const std::lock_guard<std::mutex> guard (aliveTokenMutex);
    aliveToken.reset();
}

std::unique_ptr<IAudioBackend> Application::createPlatformBackend()
{
#if JUCE_MAC
    return std::make_unique<CoreAudioBackend>();
#elif JUCE_WINDOWS
    return std::make_unique<WasapiAsioBackend>();
#elif defined(__linux__) && ! defined(MMA_NO_ALSA)
    return std::make_unique<AlsaBackend>();
#else
    return nullptr; // unsupported platform for real audio I/O; Core/ logic still runs
#endif
}

std::unique_ptr<VirtualDeviceBackend> Application::createDefaultVirtualDeviceBackend()
{
    // §7: backend A (none) is always available and ships immediately. B/C/D
    // are progressively richer but each gated on work outside this repo
    // (ASIO COM registration, driver licensing, EV cert + Partner Center).
    return std::make_unique<NullBackend>();
}

void Application::initialise()
{
    // Before anything that could be worth recording, so every entry this
    // session shares one origin.
    appStartMs = juce::Time::getMillisecondCounterHiRes();

    // Before anything reads the folders they sit in. Each settings, show or
    // camera-guard write goes to a hidden temporary file that is renamed over
    // the real one; a crash, force-quit or power cut between the two strands
    // the temporary file, and nothing removed them. This launch holds the
    // single-instance lock, so none of them can be a write in progress.
    sweepAbandonedSafeWriteTemps();

    // First, before anything reads a setting: the capture coordinator is built
    // with masterVolume a few lines down, and the destination is chosen below.
    loadSettings();

    // Said after loadSettings(), because that is what triggers the migration
    // and what would have come back empty if it failed.
    if (supportFolderMigrationFailed)
        noteActivity (ActivityLevel::Warning, "Settings",
                      "Couldn't move your saved settings over from the app's old name, so your "
                      "microphone names and destination may have been forgotten.");

    // §10.1: asked BEFORE the backend enumerates, because on macOS a denial is
    // what makes enumeration come back empty. Without this the app told a user
    // with a microphone plugged in to "plug in a USB microphone" -- advice for
    // a problem they did not have, about the one thing they had already done.
    microphonePermission = queryMicrophonePermission();
    microphoneRestricted = isMicrophoneAccessRestricted();

    // Still "not yet asked": ask through AVFoundation now, so the prompt (if
    // one is due) appears at launch and the status the Record gate reads gets
    // settled, rather than waiting on whatever the stream open raises.
    if (microphonePermission == PermissionState::NotYetRequested)
        requestMicrophoneAccess();

    audioBackend = createPlatformBackend();
    virtualDeviceBackend = createDefaultVirtualDeviceBackend();
    systemAggregate = createSystemAggregateDevice();

    if (audioBackend != nullptr)
    {
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        desiredBufferSize());
        capture->getMonitorBus().setMasterVolume (masterVolume);
        capture->getMonitorBus().setGlobalMute (monitorMuted);
        ++captureGeneration;
        captureRate = currentSampleRate;
        captureBufferSize = desiredBufferSize();

        // OS device-change notifications arrive on the backend's own thread --
        // a CoreAudio listener thread on macOS, the COM notification thread on
        // Windows. Everything onDeviceListChanged touches (DeviceManager, the
        // meters, the coordinator) belongs to the message thread, so the
        // callback only queues the work. The token keeps a callback that is
        // already queued at quit time from firing into a destroyed Application.
        const auto alive = getAliveToken();

        audioBackend->setDeviceChangeCallback ([this, alive]
        {
            juce::MessageManager::callAsync ([this, alive]
            {
                if (alive.lock() != nullptr)
                    onDeviceListChanged();
            });
        });
        onDeviceListChanged(); // initial enumeration, per §2 "at launch"

        // Said once, at launch, if the backend cannot watch the rig at all.
        // The watch is set up above and its two failure paths used to return in
        // silence -- leaving an app that never notices a microphone arriving,
        // and never reports one pulled out mid-take (§6.5), while looking
        // exactly like one that has nothing to report.
        if (const auto hotplug = audioBackend->getHotplugProblem(); ! hotplug.empty())
            noteActivity (ActivityLevel::Warning, "Microphones", juce::String (hotplug));
    }
    else
    {
        // A build with no platform backend has no monitoring and can record
        // nothing. It used to present as an ordinary launch with an enabled
        // record button, which is the most complete silence in the app.
        noteActivity (ActivityLevel::Failed, "Sound",
                      "This build has no way to reach your sound hardware, so nothing can be "
                      "heard or recorded.");
    }

    // §10.1's default, but only when there is nothing remembered to override
    // it -- otherwise the card the user chose last time is silently replaced by
    // the home folder on every launch.
    if (destinationFolder.empty())
        chooseInitialDestination();

    // A remembered destination bypasses chooseInitialDestination(), but the
    // Settings panel will still need the volume list. This only schedules the
    // mount/free-space scan; it immediately returns the home-folder fallback,
    // so a sick removable volume cannot hold the window closed at launch.
    (void) getStorageVolumes();

    // Start the filesystem worker before the window asks for its first status
    // line. Submitting this request performs no filesystem work here.
    filesystemStatusProbe.setRequest (currentFilesystemProbeRequest());

    // §6.4: benchmark before the user reaches for record, not at record time.
    beginPreflightForDestination();

    // §6.6: schedule the one-shot recovery work before the window opens.
    // The walks and repairs run on detached, worker-owned state; arming stays
    // gated until the roots this take would use have finished.
    scanForInterruptedSessions();

    // §5.1 monitoring was already opened by the initial
    // onDeviceListChanged() above. Reopening the identical CoreAudio IOProcs
    // here immediately after startup made some USB drivers stall in their HAL
    // teardown/recreate path before the window could appear.

    // Before anything can open a camera: a launch that went down while
    // starting one must not start it unattended again, and on macOS no camera
    // opens until camera access is granted.
    cameraController.setStartupGuardFile (getSupportFolder().getChildFile ("camera-starting.txt"));
#if JUCE_MAC
    cameraController.setCameraPermission ([] { return queryCameraPermission(); },
                                          [] { requestCameraAccess(); },
                                          [] { return isCameraAccessRestricted(); });
#endif

    // Enumeration only. Nothing is opened here, so a rig with no camera
    // switched on turns no camera light on and spends no privacy prompt.
    //
    // A camera the user has already switched on is opened shortly afterwards by
    // the UI tick, because the main screen shows the picture beside the meters
    // and cannot show one from a closed device. On macOS the camera prompt is
    // raised once, by CameraController, when that first open is held back for it.
    cameraController.refreshCameras();

    // A camera the user turned off last time stays off, and one they named
    // keeps its name -- applied after enumeration, since only then is there
    // anything to match the remembered answers against.
    applyingRememberedSettings = true;

    for (const auto& camera : cameraController.getSelection().getAvailableCameras())
        if (const auto* remembered = rememberedSettings.findCamera (camera.id))
        {
            cameraController.getSelection().setEnabled (camera.id, remembered->enabled);
            cameraController.getSelection().setQuality (camera.id, cameraQualityFromKey (remembered->quality));

            if (! remembered->assignedName.empty())
                cameraController.getSelection().setAssignedName (camera.id, remembered->assignedName);
        }

    applyingRememberedSettings = false;
}

void Application::setCameraEnabled (const std::string& id, bool enabled)
{
    cameraController.setCameraEnabledByUser (id, enabled);
    saveSettings();
}

void Application::setCameraName (const std::string& id, const juce::String& name)
{
    // §6.2 sanitizing at the point of entry, so what is remembered is what will
    // appear in the filename rather than something that still has to be cleaned.
    // A cleared name is empty, not "Session", so the camera's own name returns.
    cameraController.getSelection().setAssignedName (id, SessionFolderNaming::sanitizeNameOrEmpty (name.toStdString()));
    saveSettings();
}

void Application::setCameraQuality (const std::string& id, CameraQuality quality)
{
    cameraController.setCameraQuality (id, quality);
    saveSettings();
}

void Application::setCameraPreviewQuality (PreviewQuality quality)
{
    cameraController.setPreviewQuality (quality);
    saveSettings();
}

void Application::confirmSaveLocation()
{
    confirmedSaveLocation = destinationFolder;
    saveSettings();
}

void Application::setAskWhereToSaveEveryTime (bool ask)
{
    askWhereToSaveEveryTime = ask;
    saveSettings();
}

void Application::markSetupGuideDone()
{
    if (setupGuideDone)
        return;

    setupGuideDone = true;
    saveSettings();
}

void Application::setMirrorEnabled (bool enabled)
{
    mirrorPolicy.setEnabledByUser (enabled);

    // §6.3: unticking mid-take used to change only the setting. The copy went
    // on being written to the end of the take, while session.json said the
    // backup was off. Off means off: this take's copy stops here, and the
    // record says where. Ticking it back on waits for the next take -- a copy
    // with a hole in the middle is not a usable one.
    if (! enabled && capture != nullptr && capture->isRecording() && capture->isMirroring()
        && mirrorPolicy.noteStoppedByUser())
    {
        capture->stopMirroring();
        midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                     "Local backup copy turned off in Settings: the copy is "
                                     "incomplete from this point." });
        noteActivity (ActivityLevel::Stopped, "Local backup",
                      "Local backup copy turned off; this take's copy stops here.");
    }

    saveSettings();
}

void Application::setCameraTileScale (int step)
{
    if (step == cameraTileScale)
        return;

    cameraTileScale = step;
    saveSettings();
}

void Application::setCombineVideoAndAudio (bool shouldCombine)
{
    if (shouldCombine == combineVideoAndAudio)
        return;

    combineVideoAndAudio = shouldCombine;
    saveSettings();
}

juce::String Application::getCombineUnavailableReason()
{
    if (! combineVideoAndAudio)
        return {};

    if (takeCombiner.findFfmpeg().isNotEmpty())
        return {};

    // §10.6: named before a take rather than discovered after one. Finding out
    // that the combined file was never possible is worth knowing while there is
    // still time to install the thing, not once the recording is over.
    return "Combined video needs ffmpeg, which isn't installed. Your picture and "
           "sound will still both be recorded, as separate files. On a Mac: "
           "brew install ffmpeg.";
}

namespace {
// Wall-clock seconds, for the once-a-day spacing. Not the app's own clock: the
// interval has to hold across launches, and that clock starts again at each.
double wallClockSeconds()
{
    return std::floor (static_cast<double> (juce::Time::currentTimeMillis()) / 1000.0);
}

// A whole release page is tens of kilobytes. Anything far past that is not the
// answer being looked for, and is not worth holding in memory to find out.
constexpr int kMaxReleaseResponseBytes = 1 << 20;
} // namespace

void Application::setCheckForUpdates (bool on)
{
    if (on == checkForUpdates)
        return;

    checkForUpdates = on;
    saveSettings();

    // Saying yes is consent for today's check too, not only tomorrow's launch.
    // The tick picks it up, so a take that is running still comes first.
    automaticUpdateCheckWanted = on;
}

void Application::checkForUpdatesNow()
{
    if (isRecording() || updateCheckRunning->load())
        return;

    startUpdateCheck();
}

void Application::pollUpdateCheck()
{
    // Nothing during a take: not the request, and not the news. Both wait.
    if (isRecording())
        return;

    if (updateNoticePending)
    {
        updateNoticePending = false;
        noteActivity (ActivityLevel::Started, "Updates",
                      "SobStage " + availableUpdateVersion
                          + " is available. Settings has the link to download it.");
    }

    if (automaticUpdateCheckWanted && ! updateCheckRunning->load())
    {
        automaticUpdateCheckWanted = false;

        if (UpdateCheck::isCheckDue (checkForUpdates, wallClockSeconds(), lastUpdateCheckSeconds))
            startUpdateCheck();
    }
}

void Application::startUpdateCheck()
{
   #if JUCE_LINUX && ! JUCE_USE_CURL
    // Built without curl, JUCE's Linux HTTP client speaks plain http:// only,
    // and the releases API is https://. Every check failed before a byte was
    // sent and was reported as "Couldn't reach GitHub just now. Try again
    // later." -- every day, on a network that was working, with no later at
    // which it would. Say what is actually true instead.
    updateCheckSucceeded = false;
    updateCheckProblem = "This Linux build can't check for updates by itself. The newest "
                         "version is on the Releases page.";
    return;
   #endif

    updateCheckRunning->store (true);
    updateCheckProblem.clear();

    // Stamped when asked rather than when answered, so a check that fails --
    // no network on the train -- is not retried on every launch that day.
    lastUpdateCheckSeconds = wallClockSeconds();
    saveSettings();

    const auto running = updateCheckRunning;
    const auto alive = getAliveToken();

    // A detached worker, like the diagnostics export: a network that hangs
    // must never be able to hold up the window, and the worker owns nothing of
    // Application's, so it can be abandoned at quit.
    std::thread ([this, alive, running]
    {
        bool reached = false;
        juce::String text;
        int status = 0;

        // The request carries a generic User-Agent, which GitHub requires, and
        // nothing else: no version, no identifier, no settings.
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withExtraHeaders ("User-Agent: SobStage\r\n"
                                                    "Accept: application/vnd.github+json")
                                 .withConnectionTimeoutMs (10000)
                                 .withStatusCode (&status)
                                 .withNumRedirectsToFollow (3);

        if (auto stream = juce::URL (UpdateCheck::kLatestReleaseUrl).createInputStream (options))
        {
            juce::MemoryBlock body;
            stream->readIntoMemoryBlock (body, kMaxReleaseResponseBytes);

            reached = status == 200 && body.getSize() > 0;
            text = body.toString();
        }

        juce::MessageManager::callAsync ([this, alive, running, reached, text]
        {
            running->store (false);

            if (alive.lock() != nullptr)
                finishUpdateCheck (reached, text);
        });
    }).detach();
}

void Application::finishUpdateCheck (bool reached, const juce::String& responseText)
{
    // Failures go on the Settings line and nowhere else. The user may not have
    // pressed anything, and a launch that opens with "couldn't reach GitHub"
    // in the activity list is noise about a convenience, not news about the rig.
    if (! reached)
    {
        updateCheckSucceeded = false;
        updateCheckProblem = "Couldn't reach GitHub just now. Try again later.";
        return;
    }

    const auto release = UpdateCheck::parseLatestRelease (responseText.toStdString(),
                                                          UpdateCheck::currentPlatform());

    if (! release.has_value())
    {
        updateCheckSucceeded = false;
        updateCheckProblem = "GitHub's answer couldn't be read. Try again later.";
        return;
    }

    updateCheckSucceeded = true;

    if (! UpdateCheck::isNewer (release->tag, appVersionString().toStdString()))
    {
        availableUpdateVersion.clear();
        availableUpdatePage.clear();
        return;
    }

    // Shown as people say it, without the tag's "v".
    auto version = juce::String (release->tag).trim();
    if (version.startsWithIgnoreCase ("v"))
        version = version.substring (1);

    // Once per version, not once per check: pressing Check now twice is not
    // two pieces of news. Said through pollUpdateCheck(), which holds it
    // back while a take is running.
    if (version != availableUpdateVersion)
        updateNoticePending = true;

    availableUpdateVersion = version;
    availableUpdatePage = juce::String (release->pageUrl);
    pollUpdateCheck();
}

juce::String Application::getUpdateStatusText() const
{
    if (isRecording())
        return {};

    if (updateCheckRunning->load())
        return "Checking...";

    if (availableUpdateVersion.isNotEmpty())
        return "Version " + availableUpdateVersion + " is available.";

    if (updateCheckProblem.isNotEmpty())
        return updateCheckProblem;

    if (updateCheckSucceeded)
        return "You have the latest version (" + appVersionString() + ").";

    return {};
}

bool Application::isUpdateAvailable() const
{
    return ! isRecording() && availableUpdateVersion.isNotEmpty();
}

void Application::openUpdatePage() const
{
    // Only ever a github.com address: parseLatestRelease refuses to keep
    // anything else and falls back to the releases page.
    if (availableUpdatePage.isNotEmpty())
        juce::URL (availableUpdatePage).launchInDefaultBrowser();
}

void Application::setDeliveryTarget (const juce::String& name)
{
    if (name == deliveryTarget)
        return;

    deliveryTarget = name;
    saveSettings();
}

juce::StringArray Application::getDeliveryTargetNames()
{
    juce::StringArray names;
    for (const auto& target : streamingTargets())
        names.add (juce::String (target.name));
    return names;
}

juce::String Application::getLoudnessReading() const
{
    if (capture == nullptr || capture->getLoudnessBlockCount() < kMinimumBlocksToJudge)
        return {};

    const double lufs = capture->getIntegratedLufs();

    if (lufs <= LoudnessMeter::kAbsoluteGateLufs)
        return {};

    return juce::String (lufs, 1) + " LUFS";
}

juce::String Application::getLoudnessAdvice() const
{
    if (deliveryTarget.isEmpty() || capture == nullptr)
        return {};

    const auto* target = findStreamingTarget (deliveryTarget.toStdString());

    if (target == nullptr)
        return {};

    const auto advice = adviseForTarget (*target,
                                         capture->getIntegratedLufs(),
                                         capture->getTruePeakDbtp(),
                                         capture->getLoudnessBlockCount());

    return juce::String (advice.summary);
}

void Application::openEnabledCameras (bool retryFailures)
{
    cameraController.applySelection (retryFailures);

    // CameraController has always set this string, and it was only ever
    // rendered by the camera panel -- which is a panel, so it is usually shut.
    // Starting a take with it shut meant "two of your cameras couldn't start
    // recording" was written to a field nobody was looking at.
    const auto problem = cameraController.getProblem();

    if (problem.isNotEmpty() && problem != reportedCameraProblem)
    {
        reportedCameraProblem = problem;
        noteActivity (ActivityLevel::Failed, "Cameras", problem);
    }
    else if (problem.isEmpty())
    {
        reportedCameraProblem.clear();
    }
}

std::vector<ChannelPlanDevice> Application::planDevices() const
{
    std::vector<ChannelPlanDevice> out;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        // §4: trim and the assigned name are persisted against the physical
        // port, so they follow the mic across replug rather than across slot.
        const auto persisted = portIdentityStore.get (d.identity);

        ChannelPlanDevice p;
        p.deviceKey = d.identity.key();
        p.productName = d.displayName;
        p.inputChannelCount = d.inputChannelCount;

        if (persisted.has_value())
        {
            p.assignedName = persisted->assignedName;
            p.disabledInputs = persisted->disabledInputs;
            p.inputNames = persisted->inputNames;
            p.hasChannelLayoutDecision = persisted->hasChannelLayoutDecision;

            // §2.4 remembers §2.1's verdict per port. Only a decision that was
            // actually made collapses a two-input device; the default of "no
            // decision yet" keeps both sides.
            p.knownDuplicateStereo = persisted->hasChannelLayoutDecision
                                  && persisted->channelLayoutIsMono;
            p.monoSourceChannel = persisted->channelLayoutMonoSource;
        }

        out.push_back (std::move (p));
    }

    return out;
}

std::vector<CaptureChannel> Application::buildCaptureChannels() const
{
    // One channel per input the device presents, decided in exactly one place.
    //
    // A device with one input is one microphone. A device with several is an
    // interface with several microphones plugged into it, and each of them is a
    // person who expects their own track. Taking one channel from any device --
    // which is what this did -- silently discarded everybody but the first, and
    // if their microphone was on one of the discarded inputs they got a silent
    // recording with no explanation.
    std::vector<CaptureChannel> channels;
    int index = 0;

    for (const auto& planned : planChannels (planDevices()))
    {
        ++index;

        CaptureChannel c;
        c.deviceId = planned.deviceKey;
        c.deviceChannel = planned.deviceChannel;
        c.collapseStereoPair = planned.collapseStereoPair;
        c.analyzeStereoPair = planned.analyzeStereoPair;
        c.monoSourceChannel = planned.monoSourceChannel;
        c.displayName = planned.displayName;

        // §6.2: "01_Yeti-Kitchen" -- ordinal prefix plus the sanitized name,
        // so the stems sort in channel order in any file browser.
        char prefix[4] = {};
        std::snprintf (prefix, sizeof (prefix), "%02d", index);
        c.fileName = std::string (prefix) + "_" + SessionFolderNaming::sanitizeName (c.displayName);

        for (const auto& d : deviceManager.getDevices())
        {
            if (d.identity.key() != planned.deviceKey)
                continue;

            // Per input: on an interface each socket is trimmed on its own.
            if (const auto persisted = portIdentityStore.get (d.identity))
                c.trimDb = persisted->trimDbForInput (planned.deviceChannel);

            // §2.3: this channel is written at its own device's depth. A rig
            // can hold a 16-bit microphone and a 24-bit interface at once and
            // there is no single answer that serves both -- 24 pads the first,
            // 16 discards from the second.
            //
            // currentBitDepth is the fallback, so a backend that reports
            // nothing keeps exactly today's behaviour, and a user who has set
            // the depth by hand keeps their choice.
            c.bitDepth = SampleFormat::chooseRecordingBitDepth (d.supportedBitDepths,
                                                                currentBitDepth);
        }

        channels.push_back (std::move (c));
    }

    return channels;
}

void Application::applyChannelLayoutDecisions()
{
    if (capture == nullptr || capture->isRecording())
        return;

    bool changed = false;
    const auto& captureChannels = capture->getChannels();

    for (size_t i = 0; i < captureChannels.size(); ++i)
    {
        const auto& channel = captureChannels[i];
        const bool learningLayout = channel.analyzeStereoPair;
        const bool refreshingCollapsedSource = channel.collapseStereoPair;
        if (! learningLayout && ! refreshingCollapsedSource)
            continue;

        // The sixty-second all-silent Mono fallback is intentionally not a
        // port classification. Persisting it would permanently hide input 2
        // of an interface that merely opened in a quiet room.
        if (! capture->isChannelLayoutDecisionPersistable (static_cast<int> (i)))
            continue;

        const auto decision = capture->getChannelLayoutDecision (static_cast<int> (i));

        for (const auto& device : deviceManager.getDevices())
        {
            if (device.identity.key() != channel.deviceId)
                continue;

            auto settings = portIdentityStore.get (device.identity).value_or (
                PersistedDeviceSettings {});

            if (learningLayout && ! settings.hasChannelLayoutDecision)
            {
                // Another UI action may already have saved a verdict between
                // the callback publishing it and this slow tick. Never
                // overwrite an explicit answer with a stale observation.
                settings.hasChannelLayoutDecision = true;
                settings.channelLayoutIsMono = decision == ChannelLayoutDecision::Mono;
                const int source = capture->getChannelLayoutSource (static_cast<int> (i));
                settings.channelLayoutMonoSource = decision == ChannelLayoutDecision::Mono
                                                 && source == 1 ? 1 : 0;
                portIdentityStore.put (device.identity, settings);
                changed = true;
            }
            else if (refreshingCollapsedSource
                     && settings.hasChannelLayoutDecision
                     && settings.channelLayoutIsMono
                     && decision == ChannelLayoutDecision::Mono)
            {
                // A known mono device can reveal that its live capsule is on
                // the opposite side this connection. Keep that valid idle
                // re-evaluation across the next launch as well as the next
                // take; silence alone never reaches this persistable branch.
                const int source = capture->getChannelLayoutSource (static_cast<int> (i)) == 1
                                     ? 1 : 0;
                if (settings.channelLayoutMonoSource != source)
                {
                    settings.channelLayoutMonoSource = source;
                    portIdentityStore.put (device.identity, settings);
                    changed = true;
                }
            }

            break;
        }
    }

    if (! changed)
        return;

    // Disk I/O and stream reconstruction stay on the message thread. The
    // analyzer callback itself performs only arithmetic and atomic stores.
    saveSettings();
    restartCapture();
}

void Application::restartCapture()
{
    if (capture == nullptr)
        return;

    // §5.4 fixes the buffer size for the duration of a take, and reopening the
    // streams would tear down the writer mid-file. A device change during a
    // recording is handled by §6.5 instead: the channel stays and goes silent.
    // The restart is owed, though, and the stop path pays it.
    if (capture->isRecording())
    {
        captureRestartDeferred = true;
        return;
    }

    // A combined device that changed during the take is replaced now, before
    // the streams reopen onto it.
    if (aggregateRepublishDeferred)
    {
        aggregateRepublishDeferred = false;

        // Chosen again whether or not the re-create worked: a refused one has
        // already destroyed the device the headphones were on.
        publishAggregateDevice();
        reselectOutputDevice();
    }

    // Every stream is reopened below, so none is dead any more. Stamped, so
    // one that dies again straight away is left dead instead of looping here.
    const auto reopenedAtMs = juce::Time::getMillisecondCounterHiRes();
    for (const auto& deviceId : deadInputStreams)
        deadInputReopenedAtMs[deviceId] = reopenedAtMs;
    deadInputStreams.clear();

    // §2.2 can settle on a different rate once the mics are enumerated, and
    // §5.4 can move the buffer up a rung. Both are fixed at construction, so a
    // change means a new coordinator -- carrying the listening level across,
    // since the user did not ask for it to jump.
    if (audioBackend != nullptr
        && (captureRate != currentSampleRate || captureBufferSize != desiredBufferSize()))
    {
        capture->stopMonitoring();

        // §5: a runaway cut holds until the user presses Unmute, but it lives
        // on the bus being replaced and a fresh bus starts uncut. Read after
        // the old streams stop, so a cut from their last block is kept too.
        const bool wasRunawayCut = capture->getMonitorBus().isRunawayMuted();

        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        desiredBufferSize());
        capture->getMonitorBus().setMasterVolume (masterVolume);
        capture->getMonitorBus().setGlobalMute (monitorMuted);
        if (wasRunawayCut)
            capture->getMonitorBus().engageRunawayCut();

        // And whose headphones are switched off: a fresh coordinator starts
        // with the mix on every channel.
        headphoneJacks.lay();
        ++captureGeneration;

        captureRate = currentSampleRate;
        captureBufferSize = desiredBufferSize();
    }

    auto channels = buildCaptureChannels();

    if (channels.empty())
    {
        const bool wasMonitoring = capture->isMonitoring();

        capture->stopMonitoring();

        if (wasMonitoring)
        {
            noteActivity (ActivityLevel::Stopped, "Monitoring",
                          "Listening stopped -- there are no microphones switched on.");

            journalledMonitorCount = 0;
            journalledMonitorFailedCount = 0;
            journalledMonitorOk = false;
        }

        if (onCaptureRebuilt)
            onCaptureRebuilt();

        return;
    }

    // AlarmSpeaker.h: the default output and the monitor output are never
    // open together. alarmTarget() closes the speaker only once the monitor
    // already HAS its stream -- but a speaker left open by a chirp is what can
    // keep that stream from opening. When the default output is the device
    // about to be monitored on, dmix or PipeWire (or WASAPI shared mode with
    // exclusive priority off) still holds it, the exclusive open is refused as
    // "in use", monitoring falls back to input-only, and nothing closed the
    // speaker again for the life of the app. Let go of it before the open.
    // A sound server that keeps the card suspended-but-held for a few seconds
    // after its last client leaves (PipeWire's idle suspend) can still refuse
    // this first open; the next rebuild then succeeds.
    const bool speakerWasActive = alarmSpeaker.isActive();
    if (! selectedOutputDeviceId.empty() && speakerWasActive)
    {
        alarmSpeaker.setActive (false);
        alarmSpeakerRetryAtMs = 0.0;
        ++speakerHandoversForMonitorOpen;
    }

    // The per-jack headphone gains need nothing here: the coordinator lays
    // the combined device's own on its stream before it opens, and the mix on
    // every channel of any other output.
    const bool started = capture->startMonitoring (channels, selectedOutputDeviceId);

    // What a later device-list notification compares against: only a rig
    // that came up whole is worth not reopening.
    lastStartedRigSignature = started && capture->getDevicesThatFailedToOpen().empty()
                            ? describeRig() : std::string();

    // The monitor did not come up, so nothing else will play a chirp or siren
    // that was sounding a moment ago: give the speaker straight back rather
    // than leaving it silent until the next alarm asks for it.
    if (speakerWasActive && ! alarmSpeaker.isActive() && ! capture->hasOutputStream())
        alarmSpeaker.setActive (true);

    // §5.4: what the monitor path actually costs, taken from the backend that
    // opened it.
    //
    // This member was declared and never assigned by anything, so the Advanced
    // panel reported monitoring latency as "0.0 ms" and every take's
    // session.json recorded 0.0 as a permanent fact about how the take was
    // made. Zero is not a small latency; it is an impossible one. Every backend
    // had worked the figure out all along and CaptureCoordinator dropped it.
    refreshMonitoringLatency();

    // The microphones that actually OPENED, not the ones that were selected.
    //
    // A device that refuses to open no longer takes the whole rig down with it,
    // so "started" is now true in cases where part of the rig is missing --
    // and counting the selection here then announced "3 microphones are live"
    // for a rig with a dead one. A count that is wrong in the user's favour is
    // worse than no count.
    const auto& failedToOpen = capture->getDevicesThatFailedToOpen();

    int liveCount = 0;
    for (const auto& ch : channels)
        if (std::find (failedToOpen.begin(), failedToOpen.end(), ch.deviceId) == failedToOpen.end())
            ++liveCount;

    const int failedCount = static_cast<int> (failedToOpen.size());

    // Only a change is news. This runs on every rename, hot-plug and output
    // change, and an entry per call would bury everything else under a repeated
    // "2 microphones are live". The failure count is part of "a change": a
    // microphone dropping out of a rig that still has the same number selected
    // is exactly the event worth saying.
    const bool worthSaying = started != journalledMonitorOk
                          || liveCount != journalledMonitorCount
                          || failedCount != journalledMonitorFailedCount;

    journalledMonitorOk = started;
    journalledMonitorCount = liveCount;
    journalledMonitorFailedCount = failedCount;

    if (started)
    {
        applyClockMaster();
        driftMeasuredSeconds = 0.0;

        if (worthSaying)
            noteActivity (ActivityLevel::Started, "Monitoring",
                          juce::String (liveCount)
                          + (liveCount == 1 ? " microphone is live." : " microphones are live."));

        // §0.1: a microphone that would not open is no longer fatal, and that
        // is precisely why it has to be said out loud here. Before, the failure
        // reached the journal because it killed monitoring outright and landed
        // in the [failed] branch below; carrying on without it must not also
        // mean carrying on quietly.
        if (failedCount > 0 && worthSaying)
        {
            noteActivity (ActivityLevel::Warning, "Microphones",
                          juce::String (capture->getMonitorProblem()));

            const auto& openFailures = capture->getOpenFailures();

            // The first device's reason is in the line above. Every other
            // one's was dropped, so the log named one cause for several mics
            // that may each have had a different one.
            for (size_t i = 1; i < openFailures.size(); ++i)
            {
                const auto& failure = openFailures[i];
                noteActivity (ActivityLevel::Warning, juce::String (failure.displayName),
                              juce::String (failure.displayName) + " couldn't be opened for recording."
                                  + (failure.reason.empty() ? juce::String()
                                                            : " " + juce::String (failure.reason)));
            }
        }

        // A device that opened this time ends its failure episode, so a later
        // failure of it counts again.
        for (const auto& ch : channels)
            if (std::find (failedToOpen.begin(), failedToOpen.end(), ch.deviceId) == failedToOpen.end())
                noteDeviceHealthy (ch.deviceId);
    }
    else if (worthSaying)
    {
        // The sticky problem string already existed and is already shown. It is
        // recorded here as well because it is a thing that happened at a
        // moment, and the sticky string only ever says what is true now -- so a
        // monitor that failed and was then fixed left nothing behind.
        const auto why = juce::String (capture->getMonitorProblem());

        noteActivity (ActivityLevel::Failed, "Monitoring",
                      why.isNotEmpty() ? why
                                       : juce::String ("Couldn't start listening to your microphones."));
    }

    // §14.2 names this exact event first: "bus power exhaustion doesn't
    // produce a clean error; it shows up as ENUMERATION FAILURES and device
    // drops". Only refusals that could be a device losing power: a mic another
    // app holds, a refused rate, a denied permission or the app's own earlier
    // open still finishing all read as "needs more power" once.
    //
    // On every rebuild, not only one worth a line in the log: another mic
    // browning out where the last one has opened again leaves the live and
    // failed counts as they were, and was never counted. And whether or not
    // anything opened: every mic behind a starved hub refusing at once is the
    // case the advice is for. The detector counts each device once until it
    // opens again, so a rebuild that changes nothing cannot stack one bad
    // cable into a power problem.
    for (const auto& failure : capture->getOpenFailures())
        if (countsAsPossiblePowerLoss (failure.refusal))
            noteDeviceDropout (failure.deviceId, juce::String (failure.displayName));

    // Fired on success AND failure: either way the old meters are gone, and a
    // UI still holding pointers into them would read freed memory on its very
    // next timer tick. This runs on the message thread, in the same call
    // stack as the rebuild, so no timer can interleave.
    if (onCaptureRebuilt)
        onCaptureRebuilt();
}

bool Application::publishAggregateDevice()
{
    if (systemAggregate == nullptr)
        return false;

    // A platform with no combined device is not a failure to report. publish()
    // answers false there by definition, and once its result started being
    // read, every Windows and Linux launch filed "Couldn't make the combined
    // device" as a FAILURE -- about a feature that platform has never had.
    // What other apps see is already explained accurately, in the Advanced
    // panel, by getStatus().
    if (! systemAggregate->isSupported())
        return false;

    std::vector<std::string> uids;
    for (const auto& d : deviceManager.getDevices())
        if (d.included)
            uids.push_back (d.identity.locationId); // the CoreAudio device UID on macOS

    // The clock master is this computer, so the aggregate is left on the
    // system's own clock rather than pinned to one microphone's crystal --
    // the same reference the in-app capture path corrects onto.
    const std::string master;

    const auto name = aggregateName.toStdString();

    // Republishing destroys the device other apps may be recording from, so it
    // happens only when something real changed.
    if (uids == publishedUids && master == publishedMaster && name == publishedNameStd)
        return false;

    // Mid-take, the headphone mix may be playing through this very device.
    // Replacing it would cut everyone's headphones for the rest of the take;
    // the change waits for Stop, like every other rig change during a take.
    //
    // Whether it is playing there is asked of the stream that is open, not
    // the output chosen. Mid-take the choice can move off the combined device
    // (picked in Settings, or one layout read that found no jacks) while the
    // stream stays on it until Stop, and a microphone plugged in or pulled
    // then destroyed the device under the running take: everyone's
    // headphones went silent for the rest of it.
    const auto step = combinedDeviceRemake();

    if (step == CombinedDeviceRemake::AfterTheTake)
    {
        aggregateRepublishDeferred = true;
        captureRestartDeferred = true;
        return false;
    }

    // Close our own stream on it first. Destroying a device under a running
    // IOProc leaves the stop and the teardown to fail against an object that
    // no longer exists; the rebuild that follows reopens onto the new one.
    if (step == CombinedDeviceRemake::CloseOurStreamFirst)
    {
        capture->stopMonitoring();
        lastStartedRigSignature.clear();
    }

    // The result is read, and the cache is only updated on success.
    //
    // publish() has always returned bool and the return was discarded, so a
    // failed creation cached itself as published: the guard above then matched
    // on every later call and no retry ever happened. getStatus() went on
    // saying "no microphones connected, so other apps see nothing yet", which
    // is a wrong explanation for a device that failed to be created -- the
    // user hunts for a cable while the app has already given up.
    if (! systemAggregate->publish (name, uids, master))
    {
        noteActivity (ActivityLevel::Failed, "Combined device",
                      "Couldn't make the combined device other apps record from, so they won't see "
                      "your microphones. Everything is still being recorded here.");
        return false;
    }

    // publish() waits briefly for the new device to list its microphones, so
    // the output choice that follows can see their headphone jacks. One that
    // is slower still would leave monitoring on another output until some
    // later device-list change; look once more shortly, through the same pass
    // a hot-plug takes, so the combined device is picked up when it is ready.
    if (! uids.empty() && ! combinedLayoutIsComplete (systemAggregate->getOutputLayout(), uids))
    {
        const auto alive = getAliveToken();
        juce::Timer::callAfterDelay (750, [this, alive]
        {
            if (alive.lock() != nullptr)
                onDeviceListChanged();
        });
    }

    publishedUids = std::move (uids);
    publishedMaster = std::move (master);
    publishedNameStd = name;
    return true;
}

CombinedDeviceRemake Application::combinedDeviceRemake() const
{
    return combinedDeviceRemakeStep (capture != nullptr && capture->isOutputOpenOn (kOurAggregateUid),
                                     capture != nullptr && capture->isRecording());
}

void Application::setAggregateDeviceName (const juce::String& name)
{
    const auto trimmed = name.trim();
    const auto newName = trimmed.isEmpty() ? juce::String ("SobStage") : trimmed;

    // Applied on focus loss as well as Return, so an untouched field must not
    // rebuild anything.
    if (newName == aggregateName)
        return;

    aggregateName = newName;

    // A rename replaces the device, and the headphone mix with it when that
    // is where it plays. Whatever the outcome -- replaced, deferred to Stop,
    // or refused -- the output is chosen again and the rig reopened: a
    // refused re-create has already destroyed the old device and stopped the
    // monitor on it, and leaving it there kept everyone's headphones silent.
    publishAggregateDevice();
    reselectOutputDevice();
    requestCaptureRestart();

    saveSettings();
}

juce::String Application::getAggregateStatus() const
{
    return systemAggregate != nullptr ? juce::String (systemAggregate->getStatus()) : juce::String();
}

void Application::applyClockMaster()
{
    if (capture == nullptr)
        return;

    // The clock master is this computer, always.
    //
    // §3.2 already corrects every microphone onto the output clock -- the
    // timebase the headphones run on, which is the machine's own. Naming one
    // microphone as "master" changed nothing about that path; it only moved
    // which crystal the drift figures were quoted against, and handed the user
    // a picker for a choice with no audible consequence. Measured against the
    // computer instead, every microphone's figure means the same thing, no
    // master can be unplugged mid-take, and there is nothing to choose.
    capture->setMasterChannel (-1);
}

AlarmTone& Application::alarmTarget()
{
    const bool monitorCarriesIt = capture != nullptr && capture->hasOutputStream();

    if (monitorCarriesIt)
    {
        // Never both: on a machine where the default output is the monitor
        // device, the speaker would be fighting the exclusive stream for it.
        alarmSpeaker.setActive (false);
        return capture->getAlarm();
    }

    if (! alarmSpeaker.isActive())
    {
        // Opening a device that is not there costs real time; ask again every
        // few seconds rather than on every tick.
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (now >= alarmSpeakerRetryAtMs)
        {
            alarmSpeakerRetryAtMs = now + 5000.0;

            if (! alarmSpeaker.setActive (true))
                juce::Logger::writeToLog ("Alarm sounds have no output: " + alarmSpeaker.getProblem());
        }
    }

    return alarmSpeaker.getAlarm();
}

const AlarmTone* Application::currentAlarm() const
{
    if (capture != nullptr && capture->hasOutputStream())
        return &capture->getAlarm();

    return alarmSpeaker.isActive() ? &alarmSpeaker.getAlarm() : nullptr;
}

void Application::announceRecordingStarted()
{
    alarmTarget().trigger (AlarmTone::Kind::Started);
}

void Application::announceRecordingStopped()
{
    alarmTarget().trigger (AlarmTone::Kind::Stopped);
}

void Application::setFaultAlarm (bool on)
{
    // Off is applied to both, so a route change mid-alarm never strands a
    // siren on the path that is no longer playing.
    if (! on)
    {
        if (capture != nullptr)
            capture->getAlarm().setFault (false);
        alarmSpeaker.getAlarm().setFault (false);
        return;
    }

    alarmTarget().setFault (true);
}

bool Application::isFaultAlarmOn() const
{
    const auto* alarm = currentAlarm();
    return alarm != nullptr && alarm->getKind() == AlarmTone::Kind::Fault;
}

bool Application::isAlarmChirping() const
{
    const auto* alarm = currentAlarm();

    if (alarm == nullptr)
        return false;

    const auto kind = alarm->getKind();
    return (kind == AlarmTone::Kind::Started || kind == AlarmTone::Kind::Stopped) && alarm->isSounding();
}

uint64_t Application::getAlarmSamplesRendered() const
{
    // Both paths, so a count taken before a route change still compares.
    return (capture != nullptr ? capture->getAlarm().getSamplesRendered() : 0)
         + alarmSpeaker.getAlarm().getSamplesRendered();
}

bool Application::isAlarmAudible() const
{
    return (capture != nullptr && capture->hasOutputStream()) || alarmSpeaker.isActive();
}

MonitorBus* Application::getMonitorBus()
{
    // The coordinator owns the bus the audio callback actually runs, so this is
    // the only bus there is. A second one kept here as a "fallback" is what let
    // the volume slider write to a bus nothing was listening to.
    return capture != nullptr ? &capture->getMonitorBus() : nullptr;
}

juce::String Application::getMonitorProblem() const
{
    if (capture == nullptr)
        return {};

    return juce::String (capture->getMonitorProblem());
}

void Application::syncSleepInhibitor()
{
    // A copy to the card under way keeps the computer awake too: one cut off
    // by sleep starts its file again, and a lid closed straight after the
    // show is the usual moment.
    sleepInhibitor.setHeld (isRecording() || cardCopier.hasWorkToFinish());
}

void Application::syncAfterTakeHolds()
{
    // The card is the take's until its last movie is finished: a combine
    // copying gigabytes beside it starved the take's writer and filled the
    // card under it.
    const bool held = isRecording()
                   || pendingStoppedTakeCompletion != nullptr
                   || cameraController.isFinalizingRecording();
    takeCombiner.setHeldForRecording (held);

    // The copy to the card waits for the same things, and for the launch's
    // interrupted-take checks too: one of them may be repairing a take on
    // the card, or still deciding whether a take on this computer is finished.
    cardCopier.setHeld (held || destinationRecoveryTask.isRunning() || mirrorRecoveryTask.isRunning());
}

bool Application::refreshMicrophonePermission()
{
    secondsSinceMicrophonePermissionCheck = 0.0;

    const auto current = queryMicrophonePermission();
    const auto refresh = PermissionRefresh::decide (microphonePermission, current);

    // Restricted and Denied are the same state to everything but the wording,
    // so a move between them (a profile installed or removed) is not a
    // permission change; it only re-journals the sentence.
    if (const bool restricted = isMicrophoneAccessRestricted(); restricted != microphoneRestricted)
    {
        microphoneRestricted = restricted;
        journalledPermissionProblems = false;
    }

    if (! refresh.changed)
        return false;

    microphonePermission = current;
    // Re-armed so a denial found now is journalled (and a stale one is not
    // repeated); getRecordDisabledReason() reads microphonePermission directly.
    journalledPermissionProblems = false;

    if (refresh.restartCapture)
    {
        // Mid-take the reopen this grant needs is deferred to Stop, so the take
        // in progress stays silent. "Allowed now" would read as an all-clear.
        const bool takeRunning = isRecording();
        noteActivity (takeRunning ? ActivityLevel::Failed : ActivityLevel::Recovered, "Microphones",
                      juce::String (PermissionGuidance::grantArrivedMessage (takeRunning)));
    }

    return refresh.restartCapture;
}

void Application::onDeviceListChanged()
{
    if (audioBackend == nullptr)
        return;

    // A device-list change is also when a TCC answer tends to land. The
    // restart a new grant needs is the one rescanDevices() ends with anyway.
    (void) refreshMicrophonePermission();

    rescanDevices();
}

void Application::rescanDevices()
{
    if (audioBackend == nullptr)
        return;

    auto inputDevices = audioBackend->enumerateInputDevices();

    // Rates per device, kept by identity so the §2.2 vote can be taken AFTER
    // inclusion is known. Taken here, over every device the OS lists, it
    // included microphones nobody is recording -- and a MacBook's built-in mic
    // sitting at 48 kHz outvoted the interface the take actually uses, which is
    // exactly how a rig at 44.1 kHz was told to be 48.
    std::vector<EnumeratedDeviceRates> enumeratedRates;
    enumeratedRates.reserve (inputDevices.size());

    std::vector<MicDeviceState> seen;
    seen.reserve (inputDevices.size());

    int order = 0;
    for (const auto& d : inputDevices)
    {
        MicDeviceState state;
        state.identity.locationId = d.usbLocationId;
        if (! d.serialNumber.empty())
            state.identity.serial = d.serialNumber;
        state.displayName = d.name;
        state.isBuiltIn = d.isBuiltIn;
        // 0 passes through as "not reported": syncToEnumeration keeps the
        // count it already has, and a newcomer is one microphone. Clamping it
        // to 1 here told a four-input interface being recorded from -- which
        // the ALSA probe cannot open -- that it had one input.
        state.inputChannelCount = std::max (0, d.maxInputChannels);

        // §2.3: carried through rather than dropped here, which is where it was
        // being dropped. The field existed on the descriptor, one backend
        // filled it, and nothing downstream ever saw it.
        state.supportedBitDepths = d.supportedBitDepths;

        // §2.2 prefers a rate the hardware is already on. Advertising a rate is
        // not the same as being willing to switch to it, and the switch is what
        // fails.
        enumeratedRates.push_back ({ state.identity.key(), d.supportedSampleRates, d.currentSampleRate });

        seen.push_back (std::move (state));
        ++order;
    }

    // §14.2: a device that has left the list has ended whatever failure it
    // was in. Plugged back in -- by hand, or by USB itself after a brownout --
    // and failing again, it is failing again, and counts. Only opening ended
    // a failure, so a mic that failed, was replugged and failed again counted
    // once for the life of the app.
    {
        std::set<std::string> listedNow;
        for (const auto& state : seen)
            listedNow.insert (state.identity.key());

        for (const auto& d : deviceManager.getDevices())
            if (listedNow.count (d.identity.key()) == 0)
                setupAdvisor.noteDeviceLeft (d.identity.key());
    }

    // Reconcile against what the OS reports rather than appending each device
    // again. macOS fires its device-list listener several times while a USB
    // microphone initialises, and this handler previously called addDevice()
    // per device per firing -- which is why one Yeti showed up five times.
    deviceManager.syncToEnumeration (seen);

    // §5.4: the ladder re-evaluates on a DEVICE CHANGE (and, by construction,
    // on next launch). resetToLowest() had no caller at all, so once it had
    // stepped up it stayed up for the life of the process -- a rig that was
    // struggling with one bad interface kept the bigger buffer, and its added
    // delay, after that interface was unplugged.
    //
    // Gated on the device set genuinely differing, not merely on this handler
    // running. It is also the path used to reopen streams after a step UP, and
    // resetting there would undo the step the moment it was taken.
    {
        std::string signature;
        for (const auto& state : seen)
            signature += state.identity.key() + "\n";

        if (haveEnumeratedDevicesOnce && signature != lastDeviceSignature)
            bufferLadder.resetToLowest();

        lastDeviceSignature = signature;
    }

    // §2.4: a microphone remembered from last week is only matchable once it is
    // actually plugged in, so this runs after every enumeration rather than
    // once at launch.
    applyRememberedDeviceSettings();

    // Which microphone arrived, and which one left. Run after the remembered
    // settings so each is called by the name the user gave it.
    announceDeviceChanges (seen);

    // §2.2: only the microphones actually being recorded get a vote. A device
    // the take does not use cannot be made to resample, cannot go out of sync,
    // and cannot be harmed by the choice -- so letting it constrain the rate
    // only ever costs the microphones that ARE being recorded.
    std::vector<std::string> includedKeys;

    for (const auto& d : deviceManager.getDevices())
        if (d.included)
            includedKeys.push_back (d.identity.key());

    auto rateCapabilities = SampleRateNegotiator::votingDevices (includedKeys, enumeratedRates);

    // Microphones are included but none of them matched what the OS listed.
    // That is a key mismatch somewhere upstream, and the honest response is to
    // let every enumerated device vote rather than let negotiate() see an empty
    // list and hand back its 48 kHz default -- which would be the very "demand
    // a rate the hardware refuses" failure this rule exists to end, arriving
    // silently through a side door.
    if (rateCapabilities.empty() && ! includedKeys.empty())
    {
        jassertfalse; // a device in the take that the OS did not list?
        int index = 0;

        for (const auto& e : enumeratedRates)
        {
            DeviceRateCapability cap;
            cap.deviceIndex = index++;
            cap.supportedRates = e.supportedRates;
            cap.currentRate = e.currentRate;
            rateCapabilities.push_back (std::move (cap));
        }
    }

    // The rate the rig is already on where they agree, else highest common,
    // capped at 48kHz. Never rejects a device.
    auto rateResult = SampleRateNegotiator::negotiate (rateCapabilities);

    // What Settings can offer: every rate any recorded microphone reports,
    // plus whatever each is running at now. Like Audio MIDI Setup, the whole
    // list, not a pre-filtered one -- the user is choosing for their hardware,
    // and a device that cannot follow is resampled by §3 or, if it refuses to
    // open, says so on the main screen by name.
    {
        std::set<uint32_t> offered;

        for (const auto& c : rateCapabilities)
        {
            for (auto rate : c.supportedRates)
                offered.insert (rate);

            if (c.currentRate != 0)
                offered.insert (c.currentRate);
        }

        availableSampleRates.assign (offered.begin(), offered.end());
    }

    // A pinned rate is honoured, full stop. The earlier rule quietly dropped a
    // pin the rig "could not reach" and fell back to automatic, which from the
    // user's side is a control that does nothing. §0.1's spirit: if the choice
    // cannot be met, say so -- the open path names the device and both rates.
    currentSampleRate = sampleRateOverride != 0 ? sampleRateOverride : rateResult.chosenRate;

    // The combined device other apps see tracks the rig -- §2: on the OS
    // notification, never a timer. Before the output is chosen, because the
    // combined device IS the headphone output when everyone hears everyone.
    //
    // First, whether it is still there at all. coreaudiod restarting (or the
    // device being deleted in Audio MIDI Setup) takes it away without the rig
    // changing, and the "nothing changed" guard below then never made it
    // again. Forgetting what was published sends it back through the normal
    // path, mid-take rules included.
    if (systemAggregate != nullptr && systemAggregate->forgetIfLost())
    {
        publishedUids.clear();
        publishedMaster.clear();
        publishedNameStd.clear();

        // publishAggregateDevice() holds the re-create until Stop when the
        // take's headphone mix was playing through it, by the same test.
        const bool waitsForStop = combinedDeviceRemake() == CombinedDeviceRemake::AfterTheTake;

        noteActivity (ActivityLevel::Warning, "Combined device",
                      waitsForStop
                          ? juce::String ("macOS removed the combined device other apps record from "
                                          "(its audio service restarted, or it was deleted). It will be "
                                          "made again when this take stops.")
                          : juce::String ("macOS removed the combined device other apps record from "
                                          "(its audio service restarted, or it was deleted). Making it "
                                          "again."));
    }

    publishAggregateDevice();

    // A device change can add or remove an output too, so §5.3 is re-run here
    // rather than only at launch (§6.5: output device disappears -> re-select).
    reselectOutputDevice();

    // §10.5 guidance follows the device list: names so advice can say "Kitchen"
    // rather than "channel 2", and topology so §14.3 contention is re-judged
    // when the card reader moves.
    std::vector<std::string> names;
    std::vector<ControllerContentionDetector::DeviceControllerInfo> topology;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        ControllerContentionDetector::DeviceControllerInfo info;
        info.deviceId = d.identity.key();
        // controllerId is left empty: no backend reports USB host-controller
        // topology yet, and §14.3 only judges co-location "where the OS exposes
        // controller topology". The detector treats unknown as unjudgeable and
        // stays silent, which is right -- guessing would warn people whose card
        // reader is fine.
        info.isMicrophone = true;
        topology.push_back (std::move (info));
    }

    // The advisor is fed peaks per *channel* (see pollStatusAdvice), so its
    // names have to be in that same space or every piece of §10.5 advice ends
    // up addressed to the wrong person. Built from the take's channel list
    // while one is running -- taking them from the device list meant that
    // unplugging a microphone mid-take renamed everyone after it.
    for (int i = 0, n = getIncludedMicCount(); i < n; ++i)
        names.push_back (getMicDisplayName (i).toStdString());

    setupAdvisor.setChannelNames (std::move (names));
    setupAdvisor.updateControllerTopology (topology);

    if (capture != nullptr && capture->isRecording())
    {
        // §6.5: mid-take, a mic that has gone away keeps its channel and writes
        // silence. Dropping or renumbering the channel would corrupt the take,
        // so the take's channel list is fixed and only its liveness moves.
        // Present means enumerated -- not `included`, which the Settings tick
        // box also clears. Unticking a mic mid-take used to read as an unplug
        // on the next device notification and silence that person's channel.
        std::set<std::string> present;
        for (const auto& d : deviceManager.getDevices())
            present.insert (d.identity.key());

        std::set<std::string> takeChannels;
        for (const auto& ch : capture->getChannels())
            takeChannels.insert (ch.deviceId);

        // A dead stream whose device has now left the list is an unplug, and
        // from here on the ordinary unplug and replug handling applies. Without
        // this a macOS unplug -- reported both as a stream failure and as a
        // device-list change -- would never be read as plugged back in.
        for (auto it = deadInputStreams.begin(); it != deadInputStreams.end();)
            it = present.count (*it) == 0 ? deadInputStreams.erase (it) : std::next (it);

        for (const auto& ch : capture->getChannels())
        {
            // The decision is RecordingEngine's: a device that left the list
            // this take stays silent until Stop even once it is back, because
            // its stream went with it (§6.5 below).
            //
            // A mic whose audio is still arriving is listed, whatever this
            // pass of the device list said. macOS can answer one enumeration
            // short -- a read that fails returns no devices at all, and a
            // device's alive flag can dip while a USB hub re-settles -- and
            // "left the list" is latched for the rest of the take, so one bad
            // pass silenced a working microphone until Stop. A real unplug
            // stops its stream, and the stream-failure notice that follows
            // runs this again with the audio gone.
            const bool streamDead = deadInputStreams.count (ch.deviceId) > 0;
            const bool enumerated = present.count (ch.deviceId) > 0;
            const bool listed = enumerated
                             || (! streamDead && capture->isDeviceDelivering (ch.deviceId));

            // Given the benefit of the doubt: look again shortly. A real
            // unplug's stream-failure notice is not handed to this function
            // once the device has left the list, so without a second look a
            // mic pulled within that window was never noticed at all.
            if (! enumerated && listed)
                recheckDeviceListSoon = true;
            const auto change = recordingEngine.onDeviceListSeen (ch.deviceId, listed, streamDead);
            const bool live = listed && ! streamDead && ! recordingEngine.isWritingSilence (ch.deviceId);
            capture->setChannelLive (ch.deviceId, live);

            // §6.5: "Log the dropout" on an unplug, and log the reconnection
            // too. RecordingEngine has always tracked both and nothing had ever
            // told it anything, so a mic could fall out of a four-hour take and
            // leave no trace anywhere.
            if (change == MidTakeMicChange::Unplugged)
            {
                midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                             "Microphone unplugged: writing silence to its channel." });

                noteActivity (ActivityLevel::Failed, juce::String (ch.displayName),
                              "Unplugged mid-take. Its track keeps its place and is being written "
                              "as silence.");

                // The other half of §14.2's heuristic: a "device drop". Already
                // edge-triggered by RecordingEngine, so one unplug counts once
                // however many times the device list is re-read while it is
                // gone.
                noteDeviceDropout (ch.deviceId, juce::String (ch.displayName));
            }
            else if (change == MidTakeMicChange::Reconnected)
            {
                midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                             "Microphone reconnected: its channel is live again." });

                noteActivity (ActivityLevel::Recovered, juce::String (ch.displayName),
                              "Plugged back in. Its track is live again.");
                noteDeviceHealthy (ch.deviceId);
            }
            else if (change == MidTakeMicChange::BackNextTake)
            {
                // Plugged back in, but its stream is still bound to the device
                // that left: on macOS the one that came back is a new
                // AudioObjectID, and streams are only reopened at Stop. This
                // used to mark the channel live and log "live again" while its
                // track stayed silence for the rest of the take -- so nobody
                // knew to press Stop and Record, the one thing that brings it
                // back.
                midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                             "Microphone plugged back in: it can't rejoin a take in "
                                             "progress, so its channel stays silent until the next take." });

                noteActivity (ActivityLevel::Warning, juce::String (ch.displayName),
                              "Plugged back in, but it can't rejoin this take. Its track stays "
                              "silent until you press Stop, then Record.");

                midTakeNotice = juce::String (ch.displayName)
                              + " is back but can't rejoin this take. Press Stop, then Record to include it.";
                midTakeNoticeSeconds = 12.0;
            }
        }

        // §6.5: a microphone plugged in during a take joins nothing -- the
        // channel list is fixed for the duration, and renumbering it would
        // corrupt the take. What the spec asks for is that the user be told,
        // in one line, rather than left believing it is being recorded.
        for (const auto& d : deviceManager.getDevices())
        {
            if (! d.included || takeChannels.count (d.identity.key()) > 0)
                continue;

            midTakeNotice = juce::String (recordingEngine.onNewMicPluggedMidTake (d.identity.key(), true));
            midTakeNoticeSeconds = 12.0;

            // The line lasts twelve seconds. Someone who looked away for
            // fifteen used to have no way of learning that the mic they just
            // plugged in is not in the take.
            noteActivity (ActivityLevel::Warning, juce::String (d.displayName),
                          "Plugged in. It isn't in this take or the headphones yet. It'll be "
                          "recorded starting with your next take.");
            break;
        }

        // §6.5 "clock master unplugged": failover per §3.3, without touching
        // the channel set.
        //
        // applyClockMaster() already resolves against the take's frozen channel
        // list and skips a candidate that is writing silence, so calling it
        // here is the whole of the failover. It runs on the status poll too;
        // doing it on the device-list notification as well is what makes the
        // switch happen when the microphone actually leaves rather than up to a
        // poll later.
        //
        // Only the reference moves. Every channel is corrected onto the output
        // stream's clock (§3.2), master included, so no channel's resampling
        // changes and the file layout is untouched (§6.5). What the move buys is
        // §3.3's figures: they are quoted relative to the master, and a master
        // that has gone silent stops updating, so leaving it there would quote
        // every surviving microphone against a frozen number.
        applyClockMaster();

        // Whatever this notification carried -- a rate or buffer size picked
        // in Settings, a microphone plugged in, an output that appeared -- is
        // already written into currentSampleRate, desiredBufferSize(), the
        // device list and selectedOutputDeviceId above, and none of it may
        // reach the streams mid-take. The reopen is owed, and the stop path
        // pays it, the same bargain requestCaptureRestart() strikes. Without
        // this the change was shown in Settings and never applied: the next
        // take ran at the old rate and size, and left out the new microphone.
        captureRestartDeferred = true;
        scheduleDeviceListRecheckIfDue();
        return;
    }

    // Nothing this rebuild would open has changed, and what is open is whole.
    // The app's own combined device appearing fires this very notification,
    // and reopening every stream for it -- a few hundred milliseconds after
    // they first opened -- is the immediate identical reopen some USB drivers
    // stall on. A dead stream, a lost output or a failed mic still reopens.
    //
    // An output set aside for changing its rate is not lost: it was put down
    // on purpose, because something keeps switching it (pollStatusAdvice).
    // The rate change itself fires this notification, and reopening here --
    // which sets the rate back -- is the very tug-of-war setting it aside
    // ended. It comes back when it is back at the rig's rate, or the rig
    // changes. Nor is one held off the clock on the platform's word of a rate
    // change that the poll has yet to read: the poll decides what happens to
    // it.
    if (capture != nullptr && capture->isMonitoring()
        && ! lastStartedRigSignature.empty()
        && deadInputStreams.empty()
        && capture->getDevicesThatFailedToOpen().empty()
        && (selectedOutputDeviceId.empty() || capture->hasOutputStream() || capture->isOutputAbandoned()
            || capture->isOutputOffRate())
        && describeRig() == lastStartedRigSignature)
        return;

    restartCapture();
}

void Application::scheduleDeviceListRecheckIfDue()
{
    if (! recheckDeviceListSoon)
        return;

    recheckDeviceListSoon = false;

    const auto alive = getAliveToken();
    juce::Timer::callAfterDelay (400, [this, alive]
    {
        if (alive.lock() != nullptr)
            onDeviceListChanged();
    });
}

std::string Application::describeRig() const
{
    std::string rig = std::to_string (currentSampleRate) + "|" + std::to_string (desiredBufferSize())
                    + "|" + selectedOutputDeviceId
                    + "|" + std::to_string (static_cast<int> (microphonePermission)) + "\n";

    for (const auto& c : buildCaptureChannels())
        rig += c.deviceId + "|" + std::to_string (c.deviceChannel) + "|"
             + std::to_string (c.collapseStereoPair) + std::to_string (c.analyzeStereoPair) + "|"
             + std::to_string (c.monoSourceChannel) + "|" + std::to_string (c.bitDepth) + "|"
             + c.fileName + "|" + c.displayName + "\n";

    return rig;
}

std::vector<Application::HeadphoneChoice> Application::getHeadphoneChoices() const
{
    std::vector<HeadphoneChoice> out;

    if (! isMonitoringThroughCombinedDevice())
        return out;

    for (const auto& sub : headphoneJacks.getLayout())
    {
        if (sub.outputChannels <= 0)
            continue;

        for (const auto& d : deviceManager.getDevices())
            if (d.identity.locationId == sub.deviceId)
            {
                out.push_back ({ juce::String (d.identity.key()), juce::String (d.displayName),
                                 headphonesOffKeys.count (d.identity.key()) == 0 });
                break;
            }
    }

    return out;
}

void Application::setHeadphonesOn (const juce::String& deviceKey, bool on)
{
    const auto key = deviceKey.toStdString();

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != key)
            continue;

        const bool currentlyOn = headphonesOffKeys.count (key) == 0;

        if (currentlyOn == on)
            return;

        if (on) headphonesOffKeys.erase (key);
        else    headphonesOffKeys.insert (key);

        headphoneJacks.lay();
        saveSettings();

        noteActivity (ActivityLevel::Started, juce::String (d.displayName),
                      on ? juce::String ("Headphones on: hearing everyone.")
                         : juce::String ("Headphones off: this jack no longer gets the mix. "
                                         "Alarm sounds still play in it."));
        return;
    }
}

void Application::reselectOutputDevice()
{
    if (audioBackend == nullptr)
        return;

    std::vector<OutputDeviceCandidate> snapshot;
    outputDeviceNames.clear();
    outputDeviceIdByLabel.clear();

    // Every microphone the device manager lists, recorded or not. A duplex
    // mic's output shares its id on CoreAudio, so both states matter (§5.2,
    // §5.5); the mapping itself is portable and unit-tested.
    std::vector<KnownMicrophone> microphones;
    for (const auto& mic : deviceManager.getDevices())
        microphones.push_back ({ mic.identity.locationId, mic.included });

    const auto recordingRate = static_cast<uint32_t> (currentSampleRate + 0.5);

    for (const auto& d : audioBackend->enumerateOutputDevices())
        snapshot.push_back (OutputDeviceSelector::candidateFromDescriptor (d, recordingRate, microphones));

    // A later snapshot is not itself an arrival. The tracker remembers what
    // was present before and marks only ids that actually appeared, preserving
    // §5.3's newly-connected priority across unrelated device notifications.
    auto candidates = outputDeviceTracker.observe (std::move (snapshot));

    // Labels shown in Settings map back to stable ids without asking the OS to
    // enumerate again on click. Duplicate product names are numbered rather
    // than made indistinguishable in the picker.
    std::map<std::string, int> nameTotals, nameOccurrence;
    for (const auto& c : candidates)
        if (OutputDeviceSelector::isEligible (c))
            ++nameTotals[c.displayName];

    for (const auto& c : candidates)
    {
        if (! OutputDeviceSelector::isEligible (c))
            continue;

        auto label = c.displayName;
        if (nameTotals[c.displayName] > 1)
            label += " (" + std::to_string (++nameOccurrence[c.displayName]) + ")";

        outputDeviceNames.push_back (label);
        outputDeviceIdByLabel[label] = c.id;
    }

    // The combined device, first in the list: every microphone's own
    // headphone jack, all fed the one mix, so everyone hears everyone. Offered
    // only when it exists and at least one microphone has a jack to play into.
    std::string combinedLabel;

    // The per-jack gains are by channel number, and the combined device
    // numbers its channels over the sub-devices that are active now. One
    // leaving (a cable bumped mid-take) or joining late (slower than the
    // publish wait) moves every later jack, so the read lays them again
    // (HeadphoneJacks): mid-take the reopen waits for Stop, and between takes
    // an unchanged rig is not reopened. Atomics, so changing them under a
    // running stream is safe.
    headphoneJacks.layoutRead (systemAggregate != nullptr && systemAggregate->isPublished()
                                   ? systemAggregate->getOutputLayout()
                                   : std::vector<CombinedDeviceOutputs> {});

    if (combinedDeviceHasHeadphones (headphoneJacks.getLayout()))
    {
        combinedLabel = aggregateName.toStdString();
        while (outputDeviceIdByLabel.count (combinedLabel) > 0)
            combinedLabel += " (every mic's headphones)";

        outputDeviceNames.insert (outputDeviceNames.begin(), combinedLabel);
        outputDeviceIdByLabel[combinedLabel] = kOurAggregateUid;
    }

    // Headphones arriving or leaving is a change to the rig and was said only
    // when it left the user with nothing to listen on at all. A microphone's
    // own playback endpoint is skipped: it is the same physical thing the
    // microphone list already announced, and saying it twice under two names
    // is noise rather than news. A switched-off mic's jack is the same case.
    {
        std::map<std::string, std::string> outputs;

        for (const auto& c : candidates)
            if (! c.isMicrophonePlaybackEndpoint && ! c.belongsToUnrecordedMicrophone)
                outputs[c.id] = c.displayName;

        announceOutputChanges (outputs);
    }

    // The previous pick is passed in so a moved system default alone does not
    // move the monitor; arrivals, the jack and an explicit choice still do.
    // Not when it was a headphone jack that has just been unplugged without
    // leaving the list (an Intel Mac's built-in output, back on its speakers).
    auto selection = OutputDeviceSelector::select (
        candidates, rememberedOutputDeviceId,
        OutputDeviceSelector::currentIdToKeep (candidates, selectedOutputDeviceId,
                                               selectedOutputWasHeadphoneJack));

    if (! combinedLabel.empty() && monitorThroughCombinedDevice)
    {
        selection = OutputSelection {};
        selection.found = true;
        selection.id = kOurAggregateUid;
        selection.displayName = combinedLabel;
    }

    selectedOutputDeviceId = selection.id;
    selectedOutputWasHeadphoneJack = std::any_of (
        candidates.begin(), candidates.end(),
        [&selection] (const OutputDeviceCandidate& c)
        {
            return c.id == selection.id && c.hasPhysicalHeadphoneJack;
        });
    selectedOutputDeviceName.clear();

    for (const auto& [label, id] : outputDeviceIdByLabel)
        if (id == selectedOutputDeviceId)
        {
            selectedOutputDeviceName = label;
            break;
        }

    outputSelectionProblem = selection.explanation;

    // Into the record, not just onto the screen. Having nothing to listen on
    // is a fact about the session -- someone singing to a rig that cannot play
    // them back is the thing they will ask about afterwards -- and it was shown
    // in the panel and written down nowhere. Only on a change, so a session
    // with no headphones does not repeat itself.
    if (outputSelectionProblem != reportedOutputProblem)
    {
        reportedOutputProblem = outputSelectionProblem;

        if (! outputSelectionProblem.empty())
            noteActivity (ActivityLevel::Warning, "Monitoring",
                          juce::String (outputSelectionProblem));
        else
            noteActivity (ActivityLevel::Recovered, "Monitoring",
                          "You can hear yourself again.");
    }
}

bool Application::noteCallbackOverrun()
{
    // §5.4 requires every step logged. The ladder keeps that log itself and
    // getBufferSizeChanges() hands it to whoever writes session.json, so the
    // history is not duplicated into a second place that could disagree.
    return bufferLadder.noteOverrun (juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

juce::String Application::applyBufferLadderStep (const juce::String& cause)
{
    const int size = bufferLadder.getCurrentSize();
    const bool midTake = capture != nullptr && capture->isRecording();

    // What the headphones will cost at this size, from the coordinator's own
    // sum (getMonitoringLatencyMsAt): "slightly more delay" was the old
    // wording at every rung, and at 512 samples that is over 40 ms, which
    // §5.4 says is never to be shipped without saying so. The quickest
    // microphone's own path -- no channel waits for any other in the
    // headphones, the stems are lined up on the writer -- so a device that
    // refuses this size and runs at 1156 frames delays its own channel, not
    // this figure. It is the figure the Advanced panel shows once the rig is
    // rebuilt; this notice worked its own out and read two blocks slower.
    const auto spoken = [] (double ms)
    {
        return ms < 10.0 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms));
    };

    const double delayMs = capture != nullptr ? capture->getMonitoringLatencyMsAt (size) : 0.0;

    // And a slower device's own path, where there is one: that microphone is
    // that much later in the headphones, and §5.4 does not let it go unsaid.
    const double slowestMs = capture != nullptr ? capture->getSlowestMonitoringLatencyMsAt (size) : 0.0;

    auto line = juce::String ("This computer could not keep up") + cause
              + ", so the audio buffer has been increased to " + juce::String (size) + " samples";

    // Nothing is monitoring, so there is no headphone delay to name.
    if (delayMs > 0.0)
    {
        line += ". The headphone delay is now about " + spoken (delayMs) + " ms";

        if (slowestMs - delayMs >= 1.0)
            line += " (about " + spoken (slowestMs) + " ms for the slowest microphone)";
    }

    // Fixed for the life of a stream, so the streams are reopened through the
    // same path a hot-plug takes -- the same one setBufferSizeOverride uses.
    // Not during a take: §5.4 forbids a buffer change mid-recording, so the
    // reopen is owed and happens when the take stops, and the sentence says
    // so rather than describing a change that has not happened.
    if (midTake)
    {
        line += ", from the next take. This take continues at the size it started with.";
        requestCaptureRestart();
    }
    else
    {
        line += ".";
        onDeviceListChanged();
    }

    noteActivity (ActivityLevel::Warning, "Performance", line);
    return line;
}

PerformanceWarning Application::updatePerformance (double cpuLoad, bool thermallyThrottled)
{
    return cpuPressureMonitor.update (cpuLoad, thermallyThrottled,
                                      juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

std::vector<SetupAdvice> Application::getSetupAdvice() const
{
    return setupAdvisor.getActiveAdvice (juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

ProofReading Application::snapshotProof() const
{
    ProofReading reading;
    reading.elapsedSeconds = getElapsedRecordingSeconds();

    // The UI walk's take that writes nothing. ProofReading's defaults are
    // exactly that: no frames, no bytes, a disk that did answer.
    if (proofStarvedForTesting)
        return reading;

    if (capture != nullptr)
    {
        reading.framesAccepted = capture->getFramesAccepted();
        reading.peakArrived = capture->getPeakArrived();
    }

    if (currentSessionFolder.isNotEmpty())
    {
        bool snapshotAvailable = false;
        // Only the WAVs: the camera's movie in the same folder keeps growing
        // when the audio writer has stalled.
        for (const auto& file : getCurrentSessionFiles (&snapshotAvailable))
            if (countsAsRecordedAudio (file.name.toStdString()))
                reading.bytesOnDisk += static_cast<uint64_t> (std::max<int64_t> (0, file.sizeBytes));
        reading.diskObservationAvailable = snapshotAvailable;
    }

    return reading;
}

TakeHealth Application::snapshotTakeHealth() const
{
    TakeHealth health;

    const int micCount = getIncludedMicCount();
    for (int i = 0; i < micCount; ++i)
        health.mics.push_back ({ getMicDisplayName (i).toStdString(), isMicLive (i) });

    // The controller freezes the cameras that actually began this take. An
    // unplugged writer stays in that roster as absent; a preview cannot be
    // reopened and mistaken for resumed recording during the same take.
    for (const auto& camera : cameraController.getTakeCameraStates())
        // `starting` counts as present. REC is confirmed only by
        // AVFoundation's didStart, so a camera sits in STARTING for a moment
        // after the take begins -- and `present` fed from `recording` alone
        // read that as "not there". If the watchdog's baseline landed inside
        // that window, the next observation saw the camera appear and
        // announced "Camera X is back." for a camera that had never gone
        // anywhere. What `present` means here is "in the take, not lost".
        // The id goes with it: twins share a display name, and the watchdog
        // must not read one's state as the other's.
        health.cameras.push_back ({ camera.displayName,
                                    camera.recording || camera.starting,
                                    camera.id });

    health.cameraProblem = cameraController.getProblem().toStdString();
    health.monitorProblem = getMonitorProblem().toStdString();

    if (capture != nullptr)
    {
        health.framesDropped = capture->getFramesDropped();
        // The take's own overruns, not the monitoring session's. TakeWatchdog
        // divides this by the sample rate and says "about N seconds lost so
        // far" about the CURRENT take, so a session-lifetime number made that
        // sentence describe audio lost before the take began.
        // The worst single channel, not the sum. TakeWatchdog turns this into
        // "about N seconds lost so far", and four rings overflowing together
        // for a second lose a second of recording, not four.
        health.samplesOverrun = capture->getWorstChannelOverrunThisTake();
        health.sampleRate = currentSampleRate;
        health.outputClockLost = capture->isOutputClockLost();
        health.outputRateChanged = capture->isOutputAbandoned() || capture->isOutputOffRate();
        health.writerBehind = capture->getRingFillFraction() >= CapacityMonitor::kFillWarningFraction;
        health.mixOnly = capture->isMixOnly();
    }

    health.remainingSeconds = getRemainingRecordingSeconds();
    health.elapsedSeconds = getElapsedRecordingSeconds();
    return health;
}

bool Application::isMicLive (int index) const
{
    if (capture == nullptr)
        return true;

    const auto& channels = capture->getChannels();

    if (index < 0 || index >= static_cast<int> (channels.size()))
        return true;

    const auto& deviceId = channels[static_cast<size_t> (index)].deviceId;

    // A stream that died and was not reopened is dead with or without a take.
    // Outside one this used to say live by definition, so a microphone that
    // died between takes kept a lit strip and was recorded as silence.
    if (deadInputStreams.count (deviceId) > 0)
        return false;

    // Otherwise, outside a take every included mic is live: §6.5's silence
    // only applies to a channel already fixed into a recording.
    if (! capture->isRecording())
        return true;

    return ! recordingEngine.isWritingSilence (deviceId);
}

void Application::noteDeviceDropout (const std::string& deviceId, const juce::String& displayName)
{
    // §14.2 is about three or more separate boxes drawing power from the bus,
    // so this counts DEVICES. It passed getIncludedMicCount(), which counts
    // take CHANNELS -- so one four-input interface on one port reported four
    // and tripped a heuristic written for four microphones on four ports,
    // telling someone to buy a powered hub for a rig that does not need one.
    int listedNow = 0;
    for (const auto& d : deviceManager.getDevices())
        if (d.included)
            ++listedNow;

    // Mid-take, the device list has already lost the mics that dropped --
    // a hub browning out takes all of its mics off it at once, before any of
    // them is counted here. The take's own devices were all attached when it
    // started.
    std::vector<std::string> takeDevices;
    if (capture != nullptr && capture->isRecording())
        for (const auto& ch : capture->getChannels())
            takeDevices.push_back (ch.deviceId);

    setupAdvisor.noteDeviceDropout (juce::Time::getMillisecondCounterHiRes() / 1000.0,
                                    deviceId, displayName.toStdString(),
                                    devicesAttachedAtDrop (listedNow, takeDevices));
}

void Application::noteDeviceHealthy (const std::string& deviceId)
{
    setupAdvisor.noteDeviceHealthy (deviceId);
}

void Application::updateSetupAdvisorLevels (const std::vector<float>& peaksDb, double blockSeconds)
{
    setupAdvisor.updateChannelLevels (peaksDb, blockSeconds);
}

RemainingTimeWarning Application::pollCapacityWarning()
{
    if (recordingEngine.getState() != RecordingState::Recording)
        return RemainingTimeWarning::None;

    // Negative is "could not be determined", which is not the same as none
    // left -- it used to read as Exhausted and announce a full drive.
    const auto remaining = getRemainingRecordingSeconds();
    if (remaining < 0.0)
        return RemainingTimeWarning::None;

    return capacityMonitor.evaluateRemaining (remaining);
}

Metering* Application::getChannelMetering (int index)
{
    // Same reasoning as getMonitorBus(): the meters the audio thread feeds live
    // in the coordinator, so the UI has to read those and not a second set.
    return capture != nullptr ? capture->getChannelMetering (index) : nullptr;
}

Metering* Application::getMixMetering()
{
    return capture != nullptr ? &capture->getMixMetering() : nullptr;
}

juce::String Application::nameForChannel (const std::string& identityKey, int deviceChannel) const
{
    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != identityKey)
            continue;

        const auto persisted = portIdentityStore.get (d.identity);

        // The name the user gave this port wins over the product string --
        // otherwise the meter says "Blue Yeti" while the files say "Kitchen".
        std::string base = d.displayName;
        bool knownDuplicateStereo = false;

        if (persisted.has_value())
        {
            // A name given to this particular input is who is on it, and
            // needs no socket number after it.
            const auto named = persisted->inputNames.find (deviceChannel);
            if (named != persisted->inputNames.end() && ! named->second.empty())
                return juce::String (named->second);

            if (! persisted->assignedName.empty())
                base = persisted->assignedName;

            knownDuplicateStereo = persisted->hasChannelLayoutDecision
                                && persisted->channelLayoutIsMono;
        }

        const int inputs = takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo);

        return juce::String (plannedChannelName (base, deviceChannel, inputs));
    }

    return {};
}

juce::String Application::getMicProductName (int index) const
{
    // §14.6: with four identical microphones on a desk, the name the user gave
    // a port is what identifies it -- but the hardware's own name is what tells
    // them which *kind* of thing it is.
    //
    // Resolved in channel space, not device space, for the same reason
    // getMicDisplayName is: an interface contributes several channels, and
    // after a mid-take unplug the two lists are different lengths.
    std::string key;
    juce::String fallback;

    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();

        if (index < 0 || index >= static_cast<int> (channels.size()))
            return {};

        key = channels[static_cast<size_t> (index)].deviceId;
        fallback = juce::String (channels[static_cast<size_t> (index)].displayName);
    }
    else
    {
        const auto plan = planChannels (planDevices());

        if (index < 0 || index >= static_cast<int> (plan.size()))
            return {};

        key = plan[static_cast<size_t> (index)].deviceKey;
        fallback = juce::String (plan[static_cast<size_t> (index)].displayName);
    }

    juce::String product = fallback;

    for (const auto& d : deviceManager.getDevices())
        if (d.identity.key() == key)
            product = juce::String (d.displayName);

    // Nothing to add when the strip is already showing this exact text: an
    // unnamed microphone would otherwise print its product string twice, once
    // bold and once faint underneath.
    return product == getMicDisplayName (index) ? juce::String() : product;
}

juce::String Application::getMicDisplayName (int index) const
{
    // Mid-take, resolve through the take's frozen channel list: the device the
    // caller means is the one recording into channel `index`, which after an
    // unplug is no longer "the index-th included device". The channel keeps the
    // name it was opened with, so an unplugged microphone's strip still says
    // whose it is rather than going blank.
    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();

        if (index < 0 || index >= static_cast<int> (channels.size()))
            return {};

        const auto& ch = channels[static_cast<size_t> (index)];
        const auto live = nameForChannel (ch.deviceId, ch.deviceChannel);

        // Empty means the device is no longer enumerated -- it was unplugged
        // mid-take. The name the channel opened with is the honest answer.
        return live.isNotEmpty() ? live : juce::String (ch.displayName);
    }

    // Outside a take, the same plan the take would use. Walking the device list
    // instead is what made a two-input interface show one microphone until the
    // moment recording began -- which reads as an app that cannot see the
    // second microphone at all, and was reported as exactly that.
    const auto plan = planChannels (planDevices());

    if (index < 0 || index >= static_cast<int> (plan.size()))
        return {};

    return juce::String (plan[static_cast<size_t> (index)].displayName);
}

Application::MicRenameTarget Application::getMicRenameTarget (int index) const
{
    // `index` is a strip, and a strip is one INPUT of a device -- resolved in
    // channel space, like every other strip accessor, rather than by walking
    // devices. Walking devices named the wrong port on any interface, and
    // could not name one input of it at all.
    MicRenameTarget target;

    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();
        if (index < 0 || index >= static_cast<int> (channels.size()))
            return target;
        target.deviceKey = channels[static_cast<size_t> (index)].deviceId;
        target.deviceChannel = channels[static_cast<size_t> (index)].deviceChannel;
    }
    else
    {
        const auto plan = planChannels (planDevices());
        if (index < 0 || index >= static_cast<int> (plan.size()))
            return target;
        target.deviceKey = plan[static_cast<size_t> (index)].deviceKey;
        target.deviceChannel = plan[static_cast<size_t> (index)].deviceChannel;
    }

    return target;
}

void Application::setMicAssignedName (int index, const juce::String& name)
{
    setMicAssignedName (getMicRenameTarget (index), name);
}

void Application::setMicAssignedName (const MicRenameTarget& target, const juce::String& name)
{
    if (! target.isValid())
        return;

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != target.deviceKey)
            continue;

        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});

        // One microphone is one box, so the box takes the name. On an interface
        // each socket is a person, so the socket does -- and the box's own name
        // is left alone for the other inputs. A cleared name clears the override.
        const bool knownDuplicateStereo = settings.hasChannelLayoutDecision && settings.channelLayoutIsMono;
        settings.setNameForInput (target.deviceChannel,
                                  takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo) > 1,
                                  name.toStdString());

        portIdentityStore.put (d.identity, settings);

        saveSettings();

        // The capture channels carry the display name into the stem filenames
        // (§6.2), so they are rebuilt -- but never mid-take, where §6.5 fixes
        // the channel list for the duration of the recording.
        requestCaptureRestart();

        return;
    }
}

void Application::requestCaptureRestart()
{
    if (capture != nullptr && capture->isRecording())
    {
        captureRestartDeferred = true;
        return;
    }

    restartCapture();
}

std::vector<Application::StorageVolume> Application::scanStorageVolumes()
{
    std::vector<StorageVolume> volumes;
    juce::StringArray seen;

    const auto add = [&] (const juce::File& root, bool forceRemovable)
    {
        if (! root.isDirectory() || ! root.hasWriteAccess())
            return;

        const auto full = root.getFullPathName();

        if (seen.contains (full))
            return;

        seen.add (full);

        // The space a recording can claim: on APFS that includes purgeable
        // space, which getBytesFreeOnVolume leaves out.
        const auto available = availableBytesForRecording (full.toStdString());
        const auto freeBytes = available.has_value()
            ? static_cast<juce::int64> (std::min<uint64_t> (*available, static_cast<uint64_t> (INT64_MAX)))
            : root.getBytesFreeOnVolume();
        const bool removable = forceRemovable || root.isOnRemovableDrive();

        auto name = root.getVolumeLabel();
        if (name.isEmpty())
            name = root.getFileName();
        if (name.isEmpty())
            name = full;

        juce::String label = name;
        if (removable)
            label += " (removable)";
        if (freeBytes > 0)
            label += " - " + juce::File::descriptionOfSizeInBytes (freeBytes) + " free";

        // Recordings land in a folder on the volume rather than loose at its
        // root, which is what someone expects when they hand a card to an
        // editor and what keeps a card usable for anything else.
        const auto destination = root.getChildFile ("RECORDINGS");

        volumes.push_back ({ label, destination.getFullPathName(), removable, false });
    };

    // Mounted volumes. On macOS every attached card and disk appears under
    // /Volumes; on Windows the roots are the drive letters; on Linux the
    // common mount points are covered by the roots plus /media and /mnt.
    const juce::File volumesDir ("/Volumes");
    for (const auto& child : directChildDirectories (volumesDir))
        add (child, false);

    for (const auto& dir : { juce::File ("/media"), juce::File ("/mnt") })
        for (const auto& child : directChildDirectories (dir))
            add (child, true);

    juce::Array<juce::File> roots;
    juce::File::findFileSystemRoots (roots);
    for (const auto& root : roots)
        add (root, false);

    // Always last, and always present: the one destination that cannot be
    // unplugged mid-take.
    add (juce::File::getSpecialLocation (juce::File::userHomeDirectory), false);

    return volumes;
}

std::vector<Application::StorageVolume> Application::getStorageVolumes() const
{
    auto result = storageVolumeCache.getAndRefresh (std::chrono::milliseconds (2000), []
    {
        return scanStorageVolumes();
    });

    // This is deliberately constructed without asking the filesystem whether
    // it exists, is writable or has free space. The very first call therefore
    // has a useful answer even while the detached full scan is blocked in the
    // OS on a stale mount. A completed scan normally replaces it immediately.
    if (result.empty())
        result.push_back (homeStorageVolumeFallback());

    // The selected marker is cheap application state rather than part of the
    // filesystem scan, so it remains current while the volume list is cached.
    for (auto& volume : result)
        volume.isCurrent = volume.path == juce::String (destinationFolder);

    return result;
}

void Application::setDestinationByPath (const juce::String& path)
{
    const juce::File target (path);

    if (path.trim().isEmpty())
        return;

    // The selected mount may disappear while the Settings row is being
    // clicked. Do not ask that path anything on the message thread: accepting
    // the inert string schedules detached recovery and write-speed checks,
    // and their fail-closed result decides whether Record can arm.
    setDestinationFolder (target);

    if (recordingEngine.getState() != RecordingState::Recording)
        noteActivity (ActivityLevel::Started, "Save location",
                      "Checking " + target.getFullPathName()
                      + " before it can be used for a take.");
}

void Application::chooseInitialDestination()
{
    // §10.1: a first launch must not wait for a stale card mount. The first
    // getStorageVolumes() result is therefore the immediate ~/RECORDINGS
    // fallback while full platform discovery continues off the message thread.
    // Settings picks up connected removable volumes from the completed cache.
    for (const auto& volume : getStorageVolumes())
    {
        if (! volume.isRemovable)
            continue;

        const juce::File recordings (volume.path);
        if ((! recordings.exists() && recordings.createDirectory().wasOk())
            || (recordings.isDirectory() && recordings.hasWriteAccess()))
        {
            destinationFolder = recordings.getFullPathName().toStdString();
            return;
        }
    }

    destinationFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("RECORDINGS")
                            .getFullPathName()
                            .toStdString();
}

void Application::toggleRecording()
{
    if (recordingEngine.getState() == RecordingState::Idle)
    {
        // Keep the safety gates in the composition root as well as on the UI
        // button. A keyboard/action callback must not be able to create a take
        // while a recovery worker could still enumerate and repair that same
        // destination or mirror.
        //
        // The permission is re-sampled first rather than trusted from the last
        // 2 s poll: a grant that has just landed reopens the streams here, so
        // the take starts on live inputs, and a fresh revoke blocks the gate.
        if (refreshMicrophonePermission())
            rescanDevices(); // the permission was just read; not again

        if (const auto blocked = getRecordDisabledReason(); blocked.isNotEmpty())
        {
            recordStartProblem = blocked;
            return;
        }

        // Where this take goes, asked of the disks themselves now rather than
        // read from the probe: on this computer and copied to the card after
        // Stop, or straight to the destination.
        const auto route = plannedTakeRoute (true);
        const bool onComputer = route == TakeRoute::ComputerThenCard;

        // Straight to the card: everything that refuses that is checked
        // against the route just decided, in case it differs from the one the
        // button was enabled for. Recorded on this computer, the card is not
        // asked anything at all until the copy.
        if (! onComputer)
        {
            if (const auto blocked = directRecordingBlockedReason(); blocked.isNotEmpty())
            {
                recordStartProblem = blocked;
                return;
            }

            // The drive check above may be up to one probe tick old; the card
            // could have been swapped in that time.
            if (const auto changed = confirmDestinationIsTheCheckedDrive(); changed.isNotEmpty())
            {
                recordStartProblem = changed;
                return;
            }
        }

        std::vector<RecordingChannel> channels;
        for (const auto& d : deviceManager.getDevices())
        {
            if (! d.included)
                continue;
            RecordingChannel c;
            c.deviceUsbId = d.identity.key();
            c.name = d.displayName;
            channels.push_back (c);
        }
        if (recordingEngine.start (std::move (channels)))
        {
            // Before any file of the take is opened: a combine still copying
            // the last take stops now, and the card is this take's.
            syncAfterTakeHolds();

            // recordingStartMs is NOT taken here. It is the take's audio t=0,
            // and no audio exists yet: a capacity probe, one or two mkdirs
            // (including the mirror, possibly on a slow card) and the WAV
            // headers all happen below before the writer thread has a sample.
            //
            // It used to be stamped here, and CameraController measures every
            // camera's start offset against it, which becomes audioLeadSeconds
            // and then ffmpeg's -ss on MIX.wav. Too early a t=0 makes the lead
            // too big, so the combine trimmed MORE off the front of the audio
            // than it should and the sound in the combined file ran LATE
            // against the picture. Single-digit milliseconds on an SSD, tens to
            // low hundreds on the removable cards this app is for -- small
            // enough to look like nothing, large enough to look wrong.

            // §6.3: the mirror decision is taken HERE, before the folders are
            // made, against the free space now. It used to be evaluated after
            // the mirror folder had already been decided from the previous
            // take's state, so the first take of every launch had no backup
            // and later takes inherited the verdict of the one before.
            capacityMonitor.reset();
            mirrorPolicy.reset();

            // Recorded on this computer, the take is the copy here: a backup
            // beside it on the same disk would protect nothing.
            if (onComputer)
            {
                mirrorPolicy.noteTakeRecordedOnComputer();
            }
            else
            {
                const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

                // Purgeable space counted: on a Mac with iCloud "Optimize Mac
                // storage" on, the plain free figure can be a small fraction
                // of what a write can actually claim, and the backup copy was
                // refused for want of room the disk had.
                const auto homeFree = availableBytesForRecording (home.getFullPathName().toStdString());
                const auto freeBytes = homeFree.has_value()
                    ? static_cast<int64_t> (std::min<uint64_t> (*homeFree, static_cast<uint64_t> (INT64_MAX)))
                    : static_cast<int64_t> (home.getBytesFreeOnVolume());

                // A backup on the disk the take is already going to doubles
                // the space and protects nothing: one failure takes both. The
                // usual case is recording to the computer's own drive.
                const bool backupSharesDisk = mirrorPolicy.isEnabledByUser()
                                              && backupWouldShareDestinationDisk();
                mirrorPolicy.evaluateAtArm (freeBytes, projectedSessionBytes(), backupSharesDisk);
            }

            // §6: this is what actually opens the stem files and starts the
            // writer thread. Without it the record button only changes state.
            if (capture != nullptr)
            {
                const auto now = juce::Time::getCurrentTime();
                juce::String folderProblem;

                // Collisions are settled here, where the take is written; the
                // card's folder name is settled when the copy is made.
                const auto takeRoot = onComputer ? backupRootFolder().getFullPathName()
                                                 : juce::String (destinationFolder);
                const auto folder = createSessionFolder (now, folderProblem, takeRoot, onComputer);

                // §6.3: the mirror decision was made just above, at arm time.
                const auto mirror = mirrorPolicy.isMirroring()
                                        ? createMirrorFolder (juce::File (folder).getFileName())
                                        : juce::String();

                if (folder.isEmpty()
                    || ! capture->startRecording (folder.toStdString(), currentBitDepth,
                                                  now.toISO8601 (true).toStdString(),
                                                  mirror.toStdString()))
                {
                    // Nothing was written, so the engine must not claim a take.
                    recordingEngine.stop();
                    recordingStartMs = 0.0;
                    syncAfterTakeHolds();

                    // And the user is told. This path used to return here in
                    // silence: the button un-latched, no message appeared
                    // anywhere, and the only difference between "the card is
                    // full" and "the app is broken" was that neither was said.
                    recordStartProblem = folder.isEmpty()
                        ? folderProblem
                        : juce::String (capture->getRecordingProblem());

                    if (recordStartProblem.isEmpty())
                        recordStartProblem = onComputer
                            ? juce::String ("Couldn't start recording on this computer. Check it has room.")
                            : juce::String ("Couldn't start recording. Check the card is plugged "
                                            "in, has room, and isn't locked.");

                    noteActivity (ActivityLevel::Failed, "Recording", recordStartProblem);
                    return;
                }

                // Cleared only once a take is genuinely under way, so the
                // reason for the last failure stays on screen until it is
                // replaced by a success rather than by the next click.
                // Audio t=0, taken where the audio actually begins: the
                // writer thread is up and the stem files are open. Every other
                // reader of this stamp -- the take clock, the dropout and drift
                // timestamps that reach session.json, the watchdog's repeat
                // throttle -- wants "seconds since audio began" too, so all of
                // them get more accurate here, not just the camera offsets.
                recordingStartMs = juce::Time::getMillisecondCounterHiRes();
                takeBufferChangeStart = bufferLadder.getChangeLog().size();

                recordStartProblem.clear();
                takeCardUnresponsive = false;
                lastTakeFinishedWhileQuitting = false;
                mirrorMissingReported = false;
                mirrorFinalizeFailureReported = false;
                mirrorLowSpaceNoticePending = false;
                backendDropsAtTakeStart = audioBackend != nullptr
                                              ? audioBackend->getFramesDroppedByBackend() : 0;
                takeStopSnapshot.clear();

                // The coordinator zeroes the counter itself when a take begins,
                // so the watermark has to follow it down or the first take's
                // losses would silence every later one.
                reportedLayoutMisses = 0;

                currentSessionFolder = folder;
                currentMirrorFolder = mirror;
                sessionStartIso = now.toISO8601 (true);
                currentTakeRoute = route;
                currentCopyRoot = juce::String (destinationFolder);
                // The card in now, or with it out, the one last seen here: a
                // card first mounted at its place after the take -- a camera
                // card also called "NO NAME" -- is then not this take's.
                currentCopyCardIdentity = onComputer ? cardcopy::cardForTake (cardIdentityAtDestination(),
                                                                              cardAtSaveLocation,
                                                                              destinationFolder)
                                                     : std::string();

                // Beside the take, on this computer: it still has to go to the
                // card, and where to. What lets a quit or a crash before the
                // copy is checked pick it up at the next launch. Nothing is
                // refused over it -- a take that records with no note is
                // still copied when this one stops.
                // Not an earlier take to the launch's look for marked ones,
                // which may still be held behind this take.
                if (onComputer)
                    cardCopier.expect (folder.toStdString());

                if (onComputer
                    && ! replaceWithTextChecked (juce::File (folder).getChildFile (cardcopy::kMarkerFileName),
                                                 juce::String (cardcopy::markerToJson (
                                                     { destinationFolder, {}, currentCopyCardIdentity }))))
                    noteActivity (ActivityLevel::Warning, "Card copy",
                                  "Couldn't make a note on this computer that this take still has to go "
                                  "to the card. It will still be copied when you stop, but not after a "
                                  "restart.");

                if (route == TakeRoute::StraightToCardComputerFull)
                {
                    // Only takes already on the card are safe to delete: the
                    // rest of that folder may be the only copy of a take whose
                    // copy is still to go.
                    // Named, not counted: in Finder the folder that is not on
                    // the card looks like every one that is.
                    lastTakeRouteNote = juce::String (cardcopy::computerFullNote (
                        backupRootFolder().getFullPathName().toStdString(),
                        cardCopier.summary().foldersStillToCopy));
                }
                else
                {
                    lastTakeRouteNote.clear();
                }

                // Held until the take's last movie has finished, so a rename
                // after the audio stops is still followed. See
                // followTakeFolderMovedAfterAudioStopped().
                // Opened on a disposable worker: the folder is on the card, and
                // a card that stops answering just after the folder was made
                // must not hold the window here.
                {
                    const auto path = folder.toStdString();
                    auto abandoned = std::make_shared<std::atomic<bool>> (false);
                    const auto descriptor = runWithDeadline<int> ([path, abandoned]
                    {
                        const int fd = TakeFolderTracker::openDescriptor (path);
                        if (abandoned->load() && fd >= 0)
                            TakeFolderTracker::closeDescriptor (fd);
                        return fd;
                    }, std::chrono::milliseconds (1500));

                    if (descriptor.has_value())
                        takeFolderTracker.adopt (*descriptor, path);
                    else
                        abandoned->store (true);
                }

                // What this take's files were opened with, fixed here. The
                // Settings pickers stay live during a take and move
                // currentBitDepth / currentSampleRate for the NEXT one, and a
                // replug or a tick box changes who is included, so the
                // stop-time session.json and the combined file read these.
                // captureRate, not currentSampleRate: it is the rate the running
                // coordinator writes into every header, and currentSampleRate
                // runs ahead of it whenever a reopen is still owed.
                takeSampleRate = captureRate;
                takeBitDepth = currentBitDepth;
                takeDevices.clear();

                for (const auto& d : deviceManager.getDevices())
                {
                    if (! d.included)
                        continue;

                    DeviceRecord record;
                    record.name = d.displayName;
                    record.usbId = d.identity.key();

                    if (const auto persisted = portIdentityStore.get (d.identity))
                        recordTrims (record, d, *persisted);

                    takeDevices.push_back (std::move (record));
                }

                // Do not attempt a fresh platform camera open after the audio
                // writer has started. JUCE's desktop open is synchronous and a
                // broken capture-card driver can wait inside it indefinitely;
                // doing that here would leave a take running behind a frozen
                // interface. Enabled previews are opened before arming by the
                // normal camera/UI reconciliation. Any camera which is not
                // already open is reported below while the sound take remains
                // controllable and complete.

                // recordingStartMs is the audio take's t=0. Handing it over is
                // what lets each camera record how far into the take its own
                // first frame lands -- the one number the combining step
                // cannot work out afterwards, since a camera file carries no
                // timestamp tying it to the session.
                cameraController.startRecording (juce::File (folder), recordingStartMs);

                // Same field, the other moment it is set. A camera that failed
                // to start recording is not in the take, and the take goes on
                // regardless -- so if this is not said now it is never said.
                // Only when it has changed. getProblem() now carries the
                // open-time failure as well, and that one persists across
                // takes -- so re-reading it at every take start re-logged the
                // same "Couldn't open X" for every take of the session.
                if (const auto cameraProblem = cameraController.getProblem();
                    cameraProblem.isNotEmpty() && cameraProblem != reportedCameraProblem)
                {
                    reportedCameraProblem = cameraProblem;
                    noteActivity (ActivityLevel::Failed, "Cameras", cameraProblem);
                }

                // This take's record starts with none of the last one's
                // unplugs in it. The list is reset for the take further down,
                // after this write -- so the start-time session.json, which is
                // what a crash in the take's first half-minute leaves behind,
                // carried the previous take's dropouts as this one's.
                midTakeDropouts.clear();

                // How the stems are being lined up, read just before the
                // record that carries it -- and that the first refresh's
                // signature is taken against -- so nothing that changed while
                // the cameras started is missed until the next period.
                recordTakeAlignment();

                // §6.2: session.json is written at the start so a crash mid-take
                // still leaves a record of what the rig was, and rewritten on
                // stop to add the stop time and everything logged since.
                writeSessionMetadata (false);

                // And kept current in between, so a crash part way through
                // leaves an account of the take up to then: the unplugs, the
                // cameras, the backup -- not just its first instant.
                startSessionRecordRefreshes();

                noteActivity (ActivityLevel::Started, "Recording",
                              onComputer
                                  ? "Recording started into " + juce::File (folder).getFileName()
                                        + " on this computer. It will be copied to the card when you stop."
                                  : "Recording started into " + juce::File (folder).getFileName()
                                        + (mirror.isNotEmpty() ? ", with a backup copy." : "."));

                if (lastTakeRouteNote.isNotEmpty())
                    noteActivity (ActivityLevel::Warning, "Recording", lastTakeRouteNote);

                // Said once per destination rather than every take: it is a
                // fact about where recordings go, not about this take. Not
                // about a take recorded on this computer, which has no backup
                // by design rather than for sharing a disk.
                if (! onComputer && mirrorPolicy.wasSkippedForSameDisk())
                {
                    if (sameDiskBackupNoticeFor != juce::String (destinationFolder))
                    {
                        sameDiskBackupNoticeFor = juce::String (destinationFolder);
                        noteActivity (ActivityLevel::Warning, "Local backup",
                                      mirrorplacement::sameDiskExplanation());
                    }
                }
                else if (! onComputer)
                {
                    sameDiskBackupNoticeFor.clear();
                }
            }

            // Each take gets its own warnings; a previous one must not leave the
            // ten-minute warning already spent.
            mirrorActiveAtStop = -1;
            midTakeDropouts.clear();
            midTakeNotice.clear();
            midTakeNoticeSeconds = 0.0;

            // §0.1: a stem that is silent for the whole take has to say why.
            //
            // A microphone that would not open is no longer allowed to stop the
            // rest of the rig recording, so its track is written as silence
            // beside working ones -- and silence nobody warned about is the
            // whole failure this app is built against. "Never opened" is a
            // different answer from "unplugged part way through", so it is
            // recorded at zero seconds, before any of the mid-take entries.
            if (capture != nullptr)
            {
                for (const auto& deviceId : capture->getDevicesThatFailedToOpen())
                    midTakeDropouts.push_back ({ 0.0, deviceId,
                                                 "This microphone could not be opened when the take "
                                                 "started, so its track is silent for the whole "
                                                 "take. The other microphones recorded normally." });

                // One whose stream died before the take and could not be
                // reopened. deadInputStreams used to be cleared here, which
                // made it live again on paper while its stream stayed dead:
                // the take wrote silence for it with no card and no dropout.
                for (const auto& deviceId : deadInputStreams)
                {
                    capture->setChannelLive (deviceId, false);

                    if (! recordingEngine.isWritingSilence (deviceId))
                        recordingEngine.onMicUnplugged (deviceId);

                    midTakeDropouts.push_back ({ 0.0, deviceId,
                                                 "Microphone stopped sending audio before the take "
                                                 "started and could not be reopened, so its track is "
                                                 "silent. Unplug it and plug it back in." });

                    auto subject = juce::String (deviceId);
                    for (const auto& d : deviceManager.getDevices())
                        if (d.identity.key() == deviceId)
                            subject = juce::String (d.displayName);

                    noteActivity (ActivityLevel::Failed, subject,
                                  subject + " is not sending audio, so its track in this take is "
                                  "silent. Unplug it and plug it back in.");
                }
            }

            // §5.4: buffer size is fixed for the duration of a take.
            bufferLadder.setRecording (true);
            syncSleepInhibitor();
        }
    }
    else
    {
        // Before stopRecording(), which moves the pipeline out and destroys it.
        // Read after, this is always -1 and no take could ever be reported as
        // silent -- the measurement has to be taken while the thing that made
        // it still exists.
        const float takePeak = capture != nullptr ? capture->getPeakWritten() : -1.0f;
        const float arrivedPeak = capture != nullptr ? capture->getPeakArrived() : -1.0f;
        mirrorActiveAtStop = (capture != nullptr && capture->isMirroring()) ? 1 : 0;

        // §6.1: stop the writer first so every buffered frame reaches the files
        // before the engine reports the take finished.
        if (capture != nullptr)
        {
            capture->stopRecording();

            // Where the files were finished, which is where session.json, the
            // activity log and the saved-take card must look.
            followRenamedTakeFolder();

            // Bounded inside the coordinator; said here, because a Stop that
            // could not finish the files must not read like a clean one.
            if (capture->didLastStopTimeOut())
            {
                takeCardUnresponsive = true;
                noteActivity (ActivityLevel::Failed, "Card",
                              juce::String (capture->getCardWriteProblem()));
            }
        }

        // After the writer's final drain, so its drops are all counted, and
        // while the engine is still Recording, so the clock is still running.
        // The stop-time session.json is written from this, not from whatever
        // the live state says by the time a camera finishes its movie.
        takeStopSnapshot.capture (liveTakeFigures (true));
        recordTakeAlignment();

        // Before the folder is listed for the panel that shows what was saved,
        // so the video files are closed and their real sizes are on disk by the
        // time anyone reads them.
        cameraController.stopRecording();

        // AVFoundation's stopRecording is intentionally asynchronous. Keep
        // all claims which consume or describe the movie files together, and
        // run them only after each camera's didFinish callback (or the
        // controller's bounded fail-closed timeout) has been observed.
        auto completeStoppedTake = [this, takePeak, arrivedPeak]
        {
        // A combined video or podcast copy started for this take: its copy to
        // the card keeps its marker until that file has been copied too.
        bool moreFilesComing = false;

        // The movies finished after the audio did, possibly seconds after. A
        // folder renamed or moved in that gap must not send session.json, the
        // file list and the combined video looking in the old place.
        followTakeFolderMovedAfterAudioStopped();

        // Written after stopRecording() so the frame counts and buffer log it
        // records are the take's final ones -- and before recordingStartMs is
        // cleared, or every timestamp inside it would read as zero.
        writeSessionMetadata (true);

        // §10.6: the outcome is stated, not implied. Ten seconds is enough to
        // read without becoming furniture.
        lastSessionFolder = currentSessionFolder;
        lastMirrorFolder = currentMirrorFolder;
        lastTakeBackupSkippedForSameDisk = mirrorPolicy.wasSkippedForSameDisk();
        savedNoticeSeconds = 10.0;

        // Judged here, against the files as finalized, so the status line and
        // the saved-take card cannot disagree. They used to: the card warned
        // that every file was empty while this line said "Saved to ..." beside
        // it, and the line is the one a user reads on their way out of the room.
        {
            // Bounded: a card pulled during Stop must not freeze the window
            // while its folder is listed. Listed once and kept, so the
            // saved-take card shows the same files this verdict was drawn from.
            if (takeCardUnresponsive)
            {
                lastSessionFiles.clear();
                lastSessionFilesListed = false;
            }
            else
            {
                lastSessionFiles = listSessionFilesWithin (lastSessionFolder,
                                                           static_cast<int> (kRemovableVolumeDeadline.count()),
                                                           lastSessionFilesListed);
                takeCardUnresponsive = ! lastSessionFilesListed && lastSessionFolder.isNotEmpty();
            }

            if (! lastSessionFilesListed && lastSessionFolder.isNotEmpty())
                noteActivity (ActivityLevel::Failed, "Recording",
                              currentTakeRoute == TakeRoute::ComputerThenCard
                                  ? "This computer's drive stopped answering after the take, so the files in "
                                        + juce::File (lastSessionFolder).getFileName() + " couldn't be checked."
                                  : "The card stopped answering after the take, so the files in "
                                        + juce::File (lastSessionFolder).getFileName()
                                        + " couldn't be checked. Check the card is plugged in.");

            std::vector<TakeFile> written;
            for (const auto& f : lastSessionFiles)
                written.push_back ({ f.name.toStdString(), f.sizeBytes });

            // Nothing was read, so nothing can be concluded about the audio:
            // "no audio" over a folder that simply did not answer is its own lie.
            lastTakeVerdict = lastSessionFilesListed
                                  ? judgeTakeAudio (written, takePeak, arrivedPeak)
                                  : TakeAudioVerdict::NotJudged;

            // Both failures mean the same thing to anyone deciding whether to
            // record it again: there is no audio in that folder.
            lastTakeHeldNoAudio = lastTakeVerdict == TakeAudioVerdict::NothingWritten
                               || lastTakeVerdict == TakeAudioVerdict::OnlySilence
                               || lastTakeVerdict == TakeAudioVerdict::DroppedByApp;
        }

        // The combined file, if it was asked for. Started only once every input
        // is closed and complete, and run on its own thread: copying a
        // four-hour picture is minutes of work, and none of it may happen on
        // the thread drawing the meters.
        //
        // After the bounded listing above, not before it: that listing is the
        // first post-take question the card is asked, and a card that fails
        // it is not given a combine to hang on.
        //
        // Nothing here can cost anyone the take. The inputs are finished files
        // that this only reads, and a failure leaves the folder exactly as it
        // was -- separate, complete, and playable.
        if (combineVideoAndAudio && currentSessionFolder.isNotEmpty())
        {
            // The take's own bit depth goes with it, so the combined file's
            // audio is written at the depth it was recorded at rather than
            // being quietly narrowed on the way out.
            const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined,
                                                     cameraController.getCombinedTakeInputs(),
                                                     "MIX.wav",
                                                     takeBitDepth);

            if (plan.hasWork() && takeCardUnresponsive)
            {
                // A card that stopped answering -- at Stop, or to the listing
                // just above -- is not asked to be read again, as with the
                // podcast copy below. The combine used to be: its worker hung
                // listing the folder, so it never finished, every later take's
                // combine queued behind it and silently never ran, and
                // quitting asked to wait for it.
                noteActivity (ActivityLevel::Warning, "Combined video",
                              "No combined video was made: the card stopped answering. "
                              "Whatever reached the card is still in its separate files.");
            }
            else if (plan.hasWork())
            {
                moreFilesComing = takeCombiner.start (juce::File (currentSessionFolder), plan,
                                                      followTakeFolderAfterTheTake())
                               || moreFilesComing;
            }
            else if (! plan.problem.empty())
            {
                // The user asked for one file with the sound on it and is not
                // getting one. buildCombinedTakePlan has always written a
                // plain-language reason -- "None of the cameras wrote a file,
                // so there is nothing to combine." -- and nothing in Source/
                // ever read it, so the plan was dropped in silence and the
                // user went looking for a file that was never attempted.
                //
                // TakeCombiner's own failures were already surfaced; this was
                // the remaining hole in that chain, and it is the half that
                // fires when the cameras failed rather than ffmpeg.
                noteActivity (ActivityLevel::Warning, "Combined video",
                              juce::String (plan.problem));
            }
        }

        // A copy of the mix set to the delivery target's loudness, when one is
        // chosen: the advice in Settings, carried out. Read from the take's own
        // finished MIX.wav (and any split parts) on a worker of its own, so the
        // stop is never kept waiting for it; the take's files are only read.
        // A card that stopped answering is not asked to be read again.
        if (deliveryTarget.isNotEmpty() && currentSessionFolder.isNotEmpty())
        {
            if (takeCardUnresponsive)
                noteActivity (ActivityLevel::Warning, "Podcast copy",
                              "No podcast-ready copy was made: the card stopped answering.");
            else if (podcastExporter.start (juce::File (currentSessionFolder), "MIX.wav",
                                            deliveryTarget.toStdString(),
                                            followTakeFolderAfterTheTake()))
                moreFilesComing = true;
            else
                noteActivity (ActivityLevel::Warning, "Podcast copy",
                              "No podcast-ready copy was made for " + deliveryTarget + ".");
        }

        // §6.2: the take is on disk and the UI has not shown where yet. Only
        // raised when a folder was actually opened -- a start that failed
        // preflight never got one, and "saved" would be a lie.
        savedTakePending = currentSessionFolder.isNotEmpty();

        // The stop goes in the record with its outcome attached. A take that
        // ended is a thing that happened, and whether it holds audio is the one
        // fact the user most needs carried past the ten seconds the notice
        // lasts -- particularly the empty ones, which look identical on disk to
        // a folder nobody has opened yet.
        // Whether the app ended this take, and why. A take stopped because the
        // card went away or the drive filled up used to write the same
        // "Recording stopped. Saved to X." a user-initiated stop writes -- so
        // reading the log afterwards, a take the app killed was
        // indistinguishable from one someone chose to end. The reason lived on
        // the advice line for a few seconds and nowhere else.
        const auto because = stopReason.isEmpty() ? juce::String (".")
                                                  : " -- " + stopReason + ".";
        const auto endedBy = stopReason.isEmpty() ? juce::String ("Recording stopped")
                                                  : juce::String ("Recording was stopped");

        // A card that stopped answering during the stop cannot be said to
        // hold anything: "Saved to" there is the one claim nobody can check.
        // The same account goes on the saved-take card, beside the backup's
        // path -- which is offered as a place to look, not as a complete copy,
        // because the backup is written by the same stalled writer.
        // A take recorded on this computer for a card never touched the card:
        // it is this computer's drive that stopped, and "check the card" would
        // send the user to the one thing that was fine.
        const bool recordedHere = currentTakeRoute == TakeRoute::ComputerThenCard;

        if (takeCardUnresponsive && currentSessionFolder.isNotEmpty())
        {
            CardRemovalNotice stalled;
            stalled.message = cardcopy::stoppedAnsweringAtStop (recordedHere, currentSessionFolder.toStdString(),
                                                                currentMirrorFolder.toStdString());
            cardRemovalNotice = stalled;
            cardRemovalPending = true;
        }

        noteActivity (stopReason.isNotEmpty() || lastTakeHeldNoAudio || takeCardUnresponsive
                          ? ActivityLevel::Failed : ActivityLevel::Stopped,
                      "Recording",
                      takeCardUnresponsive
                          ? endedBy + because + (recordedHere ? " This computer's drive" : " The card")
                                + " stopped answering, so the end of "
                                + juce::File (lastSessionFolder).getFileName()
                                + " may be missing."
                      : lastTakeHeldNoAudio
                          ? endedBy + because + " There is no audio in "
                                + juce::File (lastSessionFolder).getFileName()
                                + ". Check your microphones aren't muted."
                          : currentTakeRoute == TakeRoute::ComputerThenCard
                          ? endedBy + because + " Saved on this computer in "
                                + juce::File (lastSessionFolder).getFileName() + ". Copying it to the card."
                          : endedBy + because + " Saved to "
                                + juce::File (lastSessionFolder).getFileName() + ".");

        // One take, one reason. Cleared here so the next stop cannot inherit
        // this one's.
        stopReason.clear();

        // Written AGAIN, now that the stop itself is in the journal. The first
        // write happens inside writeSessionMetadata above, before this note
        // exists -- so the log filed beside every take ended mid-recording and
        // never said the take stopped, let alone why. A take the app killed
        // because the drive filled or the card left is exactly the one whose
        // log has to carry the reason, and that was the one it dropped.
        writeActivityLog (juce::File (currentSessionFolder));

        if (currentMirrorFolder.isNotEmpty())
            writeActivityLog (juce::File (currentMirrorFolder));

        // Recorded on this computer: the copy to the card, queued once every
        // file of the take is closed and its record has its stop time. The
        // folder itself goes with it, so a rename in Finder is followed.
        std::optional<CardCopier::Job> copyJob;
        lastTakeCopiesToCard = currentTakeRoute == TakeRoute::ComputerThenCard
                            && currentSessionFolder.isNotEmpty();
        lastCopyRoot = lastTakeCopiesToCard ? currentCopyRoot : juce::String();

        // Stopped here, every file closed: finished, even if its stop-time
        // record could not be written -- a full disk, the reason it stopped.
        if (lastTakeCopiesToCard)
            copyJob = CardCopier::Job { currentSessionFolder.toStdString(),
                                        { currentCopyRoot.toStdString(), {}, currentCopyCardIdentity },
                                        followTakeFolderAfterTheTake(),
                                        moreFilesComing,
                                        true };

        recordingEngine.stop();
        recordingStartMs = 0.0;
        bufferLadder.setRecording (false);
        syncSleepInhibitor();

        currentSessionFolder.clear();
        currentMirrorFolder.clear();
        currentTakeRoute = TakeRoute::StraightToDestination;
        currentCopyRoot.clear();
        currentCopyCardIdentity.clear();
        takeFolderTracker.close();

        // After the folder is let go, so no later line of this take's activity
        // log is written into it while the copy reads it. Any earlier take
        // still waiting on a card that was out or full goes again with it.
        if (copyJob.has_value())
        {
            cardCopier.start (std::move (*copyJob));
            cardCopier.retry ([] (const cardcopy::Result& r)
            {
                return cardcopy::isFailure (r.outcome) && r.outcome != cardcopy::Outcome::ComputerCopyMissing;
            });
        }

        // Every movie is finished and the take's combine is queued: whatever
        // combining is waiting can have the card now. After the copy is
        // queued, not before: a launch's look for marked takes held behind
        // this take would otherwise find it first, under whatever name its
        // folder has now, and copy it a second time as "an earlier take".
        syncAfterTakeHolds();

        // A destination the user chose mid-take, applied now that applying it
        // cannot move a running take's files out from under it.
        //
        // Placed here, AFTER recordingEngine.stop(), and that is the whole
        // point: run before it, the state is still Recording, so the apply fell
        // into its own deferral branch, re-armed the pending value and
        // re-emitted the promise -- forever. The user was told at every stop
        // that their new folder took effect from the next take, and it never
        // did, for the life of the process. applyDestinationFolder is used
        // rather than setDestinationFolder so that cannot happen again: it does
        // not consult the recording state at all.
        if (pendingDestinationFolder.isNotEmpty())
        {
            const juce::File chosen (pendingDestinationFolder);
            pendingDestinationFolder.clear();

            // Never stat a removable path on the message thread. The detached
            // recovery and preflight workers now validate this requested root;
            // recording remains fail-closed until both have answered.
            applyDestinationFolder (chosen);

            noteActivity (ActivityLevel::Started, "Save location",
                          "Checking " + chosen.getFullPathName()
                          + " before it can be used for the next take.");
        }

        // Everything refused during the take -- a mic plugged in, an unplug,
        // a rename, a rate change -- is applied now, so the next take's plan
        // and its streams agree.
        if (captureRestartDeferred)
        {
            captureRestartDeferred = false;
            restartCapture();
        }

        // A camera unplugged while recording is deliberately held absent for
        // the rest of that take because JUCE cannot append its reconnect to the
        // finalized movie. Discovery resumes only after metadata and the
        // combined-take inputs have captured the finished take, so a remembered
        // preview may safely return for the next one.
        cameraController.refreshCameras();
        };

        if (cameraController.isFinalizingRecording())
        {
            pendingStoppedTakeCompletion = std::move (completeStoppedTake);

            // Sound has already been drained above. Drop the global REC state
            // immediately so Stop stays responsive, while retaining the take
            // paths and t=0 until the movie completion closes its metadata.
            recordingEngine.stop();
            bufferLadder.setRecording (false);
            syncSleepInhibitor();
            syncAfterTakeHolds(); // still held: the movies are still being written
            return;
        }

        // Finalization already finished, synchronously, inside stopRecording.
        //
        // That is the COMMON case for the failure this sentence exists to
        // report: a camera unplugged mid-take has its writer finalize with an
        // error before Stop is even pressed, so every didFinish has already
        // arrived and isFinalizingRecording() is false here. Only the
        // asynchronous path below read the problem, so on this one the app
        // said "Saved to ..." while "X could not finish its video file. The
        // audio is safe; do not use that movie." was produced and thrown away.
        //
        // The string is deliberately not part of getProblem(), so nothing else
        // -- not the camera panel, not the journal, not the watchdog -- could
        // pick it up either.
        reportCameraFinalizationProblem();

        completeStoppedTake();
    }
}

void Application::reportFinishedCardCopies()
{
    // Reported here, on the message thread: the worker may not touch the
    // journal. A retry that found the card still out is not said again.
    for (const auto& done : cardCopier.collectFinished())
    {
        stallReportedFor.erase (juce::String (done.localFolder));
        stallReportedFor.erase (juce::String (done.folderNow));

        if (done.result.outcome == cardcopy::Outcome::Copied && done.result.cardFirstSeen)
        {
            // Not the card it was armed for -- it knew none -- so not simply
            // "the card": the one in now, which may be someone else's.
            noteActivity (ActivityLevel::Warning, "Card copy",
                          "Copied " + juce::File (juce::String (done.folderNow)).getFileName()
                              + " to the card that's in now. SobStage didn't know which card this take was "
                                "for, so check this is the one you meant.",
                          true);
        }
        else if (done.result.outcome == cardcopy::Outcome::Copied)
        {
            noteActivity (ActivityLevel::Stopped, "Card copy",
                          "Copied " + juce::File (juce::String (done.folderNow)).getFileName() + " to the card.",
                          true);
        }
        else if (! done.repeat)
        {
            noteActivity (ActivityLevel::Warning, "Card copy",
                          juce::String (cardcopy::describe (done.result, done.folderNow)), true);
        }

        // A repeat too, which the log leaves out: a quit that waited for the
        // retry of a card put back must not go without a word when that
        // retry fails as the one before it did. And one copied after all
        // drops what was kept for it.
        copyFailuresWhileQuitting.noteFinished (done.localFolder, done.folderNow, done.result);
    }
}

juce::String Application::cardCopyFailedWhileQuitting()
{
    reportFinishedCardCopies();
    return juce::String (cardcopy::copyFailedBeforeQuitting (copyFailuresWhileQuitting.untold()));
}

juce::String Application::problemToTellBeforeQuitting()
{
    const auto copy = cardCopyFailedWhileQuitting();
    const auto take = unseenProblemWithLastTake();

    if (take.isEmpty() || copy.isEmpty())
        return take + copy;

    return take + "\n\n" + copy;
}

void Application::problemToldBeforeQuitting()
{
    copyFailuresWhileQuitting.told();
}

void Application::tickCardCopies()
{
    reportFinishedCardCopies();

    // At launch, once both checks for interrupted takes are done -- one of
    // them may still be finishing a take that is waiting to be copied --
    // whatever a quit or a crash left on this computer, still to go to the
    // card, is picked up.
    const auto finished = [] (RecoveryScanStatus s)
    {
        return s == RecoveryScanStatus::Succeeded || s == RecoveryScanStatus::Failed;
    };

    if (! pendingCopiesResumed && finished (destinationRecoveryStatus)
        && (finished (mirrorRecoveryStatus) || mirrorRecoveryRoot == destinationRecoveryRoot))
    {
        pendingCopiesResumed = true;
        cardCopier.resumePendingIn (backupRootFolder().getFullPathName().toStdString());
    }

    if (const auto resumed = cardCopier.takeResumedCount(); resumed.has_value() && *resumed > 0)
        noteActivity (ActivityLevel::Started, "Card copy",
                      *resumed == 1 ? juce::String ("Copying 1 earlier take to the card.")
                                    : "Copying " + juce::String (*resumed) + " earlier takes to the card.");

    // A take whose file was too big for its card took the next one, as the
    // user was told; the takes that went to the card it replaced -- erased,
    // if it was reformatted -- go to this one too, and are said to.
    if (const auto again = cardCopier.takeRecopiedCount(); again.has_value() && *again > 0)
        noteActivity (ActivityLevel::Warning, "Card copy",
                      *again == 1
                          ? juce::String ("1 earlier take was for the card that was in before (replaced, or "
                                          "reformatted), so it's being copied to this one too.")
                          : juce::String (*again) + " earlier takes were for the card that was in before "
                                "(replaced, or reformatted), so they're being copied to this one too.",
                      true);

    // A card put back in, and an interrupted take whose stop time the
    // recovered-takes card has since written, are tried again on their own.
    // Every other failure waits for the next Stop or "Copy again".
    const auto now = juce::Time::getMillisecondCounterHiRes();

    // The card at the save location, remembered with the settings, so a take
    // armed with it out is still for it.
    if (const auto seen = cardIdentityAtDestination(); ! seen.empty())
    {
        if (auto& known = cardAtSaveLocation[destinationFolder]; known != seen)
        {
            known = seen;
            saveSettings();
        }
    }

    // A card seen back in, where it was seen out: the takes waiting for it
    // go now, not at the next timed retry, and a quit meanwhile waits for
    // them. Someone who plugs the card in as the line under Record asks, and
    // quits, otherwise left with a card that did not hold the take.
    if (const auto seen = cardSeenAtDestination(); seen.has_value())
    {
        if (*seen && cardWasSeenAtDestination == false)
            cardCopier.retryWithCardIn (destinationFolder, cardIdentityAtDestination(), false);

        cardWasSeenAtDestination = *seen;
    }

    if (now >= nextCopyRetryMs)
    {
        nextCopyRetryMs = now + cardCopyRetryIntervalMs();

        if (recordingEngine.getState() == RecordingState::Idle && ! cardCopier.isRunning())
            cardCopier.retry ([] (const cardcopy::Result& r)
            {
                return r.outcome == cardcopy::Outcome::CardMissing
                    || r.outcome == cardcopy::Outcome::DifferentCard
                    || r.outcome == cardcopy::Outcome::NotFinishedYet
                    || r.outcome == cardcopy::Outcome::ComputerReadFailed;
            });
    }

    // Held behind the card's check for interrupted takes, which the card has
    // stopped answering: said once, as a stall is.
    if (cardCheckIsHoldingCopies())
    {
        if (! copiesHeldByCardReported)
        {
            copiesHeldByCardReported = true;
            const auto waiting = cardCopier.summary().waiting;

            if (waiting > 0)
                noteActivity (ActivityLevel::Warning, "Card copy",
                              juce::String (cardcopy::notAnsweringLine (
                                  waiting, backupRootFolder().getFullPathName().toStdString())),
                              true);
        }
    }
    else if (cardCopier.msHeld() <= 0.0)
    {
        copiesHeldByCardReported = false;
    }

    // Stuck inside one call to the card. Said once per stall; the copy carries
    // on by itself if the card answers again.
    if (cardCopier.isCopying() && cardCopier.msSinceProgress() > CardCopier::kStalledAfterMs)
    {
        const auto copying = cardCopier.summary().copyingFolder;

        if (! copying.empty() && stallReportedFor.insert (juce::String (copying)).second)
            noteActivity (ActivityLevel::Warning, "Card copy",
                          juce::String (cardcopy::stalledLine (copying)), true);
    }
}

void Application::reportCameraFinalizationProblem()
{
    // One reader for both stop paths. Keeping the read inline in each was how
    // the synchronous one came to be missing it.
    if (const auto problem = cameraController.getRecordingFinalizationProblem();
        problem.isNotEmpty())
        noteActivity (ActivityLevel::Failed, "Cameras", problem);
}

bool Application::pollCameraFinalization()
{
    cameraController.pollRecordingFinalization();

    if (pendingStoppedTakeCompletion == nullptr
        || cameraController.isFinalizingRecording())
        return pendingStoppedTakeCompletion == nullptr;

    reportCameraFinalizationProblem();

    // Clear the member before invoking it: completion refreshes cameras and
    // may synchronously publish callbacks, but can never execute this take a
    // second time through re-entrancy.
    auto completion = std::move (pendingStoppedTakeCompletion);
    pendingStoppedTakeCompletion = {};
    completion();
    return true;
}

bool Application::prepareToQuit()
{
    // A take this quit stops, or one whose movies are still being finished,
    // reports how it went while the app is on its way out. The saved-take
    // card may open for it in that moment -- the screen keeps refreshing
    // while the quit waits -- but nobody has had a chance to read it.
    if (isRecording() || pendingStoppedTakeCompletion != nullptr)
        lastTakeFinishedWhileQuitting = true;

    // From here, a copy to the card that fails is asked about before the app
    // goes: the quit may be waiting for exactly that copy.
    if (! copyFailuresWhileQuitting.isWatching())
    {
        reportFinishedCardCopies();
        copyFailuresWhileQuitting.startWatching();

        // A take waiting for its card, with the card seen in its place now:
        // copied now, and waited for, rather than left for the next timed
        // retry the quit would cut off.
        if (cardSeenAtDestination() == true)
            cardCopier.retryWithCardIn (destinationFolder, cardIdentityAtDestination(), true);
    }

    if (isRecording())
    {
        if (stopReason.isEmpty())
            stopReason = "the app was asked to quit";
        toggleRecording();
    }

    pollCameraFinalization();
    return pendingStoppedTakeCompletion == nullptr
        && ! cameraController.isFinalizingRecording();
}

void Application::releaseFinishedFollowUps()
{
    // Once nothing more is being made, every take that was waiting for its
    // combined video or podcast copy is copied again with it. Asked here as
    // well as on the status tick, so a quit that comes between the two still
    // finds that copy to wait for.
    if (! takeCombiner.isRunning() && ! podcastExporter.isRunning())
        cardCopier.followUpsFinished();
}

namespace {

/// "A", "A and B", "A, B and C".
juce::String inWords (juce::StringArray items)
{
    if (items.size() <= 1)
        return items.joinIntoString ({});

    const auto last = items[items.size() - 1];
    items.remove (items.size() - 1);
    return items.joinIntoString (", ") + " and " + last;
}

} // namespace

juce::String Application::unfinishedAfterTakeWork()
{
    // Not a plain getter: a combined video or podcast copy that has just
    // finished starts its take's copy here, so the quit asks about that copy
    // rather than quitting before the status tick has queued it.
    releaseFinishedFollowUps();

    juce::StringArray work;

    if (podcastExporter.isRunning())
        work.add ("the podcast copy");

    if (takeCombiner.isRunning())
        work.add ("the combined video");

    // A copy under way, held, or queued a moment ago at Stop -- not one
    // waiting to retry a card that is out, which keeps its note on this
    // computer and goes at the next launch anyway -- or one queued because
    // its card was seen back in.
    if (cardCopier.hasWorkToFinish())
        work.add ("the copy to the card");

    return inWords (work);
}

juce::String Application::stillSavingMessage() const
{
    StillSaving work;
    juce::StringArray other;

    if (podcastExporter.isRunning())
        other.add ("the podcast copy");

    if (takeCombiner.isRunning())
        other.add ("the combined video");

    work.otherWork = inWords (other).toStdString();
    work.copyingToCard = cardCopier.hasWorkToFinish();
    work.takesCopying = cardCopier.takesToFinish();
    work.takesAddingFiles = cardCopier.takesAddingFilesToFinish();
    work.takesNotOnCard = cardCopier.takesNotOnCard();
    work.computerFolder = backupRootFolder().getFullPathName().toStdString();
    return juce::String (describeStillSaving (work));
}

juce::String Application::unseenProblemWithLastTake() const
{
    StoppedTakeOutcome outcome;

    // The two notices are consumed together, by the saved-take card as it
    // opens. A take that finished during the quit counts as unseen even so:
    // the card can open in the moment before the app closes over it.
    outcome.alreadyShown = ! savedTakePending && ! cardRemovalPending
                        && ! lastTakeFinishedWhileQuitting;

    if (cardRemovalPending)
        outcome.cardNotice = cardRemovalNotice.message;

    outcome.cardStoppedAnswering = takeCardUnresponsive;
    outcome.verdict = lastTakeVerdict;
    outcome.folderName = juce::File (lastSessionFolder).getFileName().toStdString();

    // Kept by the camera side until the next take starts, so it is this one's.
    outcome.cameraProblem = cameraController.getRecordingFinalizationProblem().toStdString();

    return juce::String (describeUnseenTakeProblem (outcome));
}

void Application::quitCalledOff()
{
    // Staying open: the saved-take card shows the take, as for any other stop.
    lastTakeFinishedWhileQuitting = false;

    // Its saved-take card and the line under Record say how the copies went.
    copyFailuresWhileQuitting.stopWatching();
}

juce::String Application::resolveSessionFolderName (juce::Time now, const juce::String& name) const
{
    const juce::File root (destinationFolder);
    const auto desired = baseSessionFolderName (now, name).toStdString();

    // §6.2: never overwrite, never prompt -- collisions get _2, _3, ...
    return juce::String (SessionFolderNaming::resolveCollision (desired,
        [&root] (const std::string& candidate)
        {
            return root.getChildFile (juce::String (candidate)).exists();
        }));
}

juce::String Application::createSessionFolder (juce::Time now, juce::String& problem,
                                               const juce::String& root, bool onThisComputer) const
{
    const auto desired = baseSessionFolderName (now, sessionName).toStdString();

    struct Made { juce::String path; bool ok = false; };

    // The first write a take makes to the card, so the first place a card
    // pulled or wedged during Record start would hang. Bounded, so Record
    // reports it rather than freezing the window.
    const auto made = runWithDeadline<Made> ([root, desired]
    {
        const juce::File rootDir (root);

        // §6.2: never overwrite, never prompt -- collisions get _2, _3, ...
        const auto name = SessionFolderNaming::resolveCollision (desired,
            [&rootDir] (const std::string& candidate)
            {
                return rootDir.getChildFile (juce::String (candidate)).exists();
            });

        const auto folder = rootDir.getChildFile (juce::String (name));
        return Made { folder.getFullPathName(), folder.createDirectory().wasOk() };
    }, kRemovableVolumeDeadline);

    // Returned rather than logged here: the caller logs the one reason, and
    // two messages for one failure -- the second vaguer than the first -- left
    // the user reading "couldn't make a folder" as the last word.
    if (! made.has_value())
    {
        problem = onThisComputer
            ? "This computer's drive stopped answering at " + root + ", so the take couldn't start."
            : "The card at " + root + " stopped answering, so the take couldn't start. "
              "Check the card is plugged in, or choose somewhere else.";
        return {};
    }

    if (! made->ok)
    {
        problem = onThisComputer
            ? "Couldn't make a folder on this computer at " + made->path + ". Check it has room."
            : "Couldn't make a folder at " + made->path
                  + ". Check the card is plugged in, has room, and isn't locked.";
        return {};
    }

    return made->path;
}

Application::PlannedSave Application::planSave (const juce::String& proposedSessionName) const
{
    PlannedSave plan;
    plan.parentFolder = juce::String (destinationFolder);
    // This is a preview, not the creation step. Asking whether every candidate
    // exists can wedge the prompt on a stale removable mount before the async
    // recovery/preflight gates get a chance to explain the problem. At arm
    // time createSessionFolder() performs the authoritative collision check
    // and adds _2, _3, ... rather than overwriting an existing take.
    plan.folderName = baseSessionFolderName (juce::Time::getCurrentTime(), proposedSessionName);
    plan.fullPath = juce::File (plan.parentFolder).getChildFile (plan.folderName).getFullPathName();

    // §6.3: the mirror decision is only actually taken at arm time, against the
    // free space then. Showing where it *would* go is still worth doing -- a
    // second copy the user does not know about is a second copy they will not
    // find -- so this reports the path whenever the setting is on, and the
    // panel shown at stop reports what really happened.
    //
    // Except where it is already known that it would share the destination's
    // disk, which arm time will refuse: then the prompt says so instead. Read
    // from the probe's last snapshot -- this preview never asks the disk.
    // Recorded on this computer and copied to the card after Stop: the take
    // itself is the copy here, so there is no backup line to show.
    if (wouldRecordOnComputer())
    {
        plan.recordsOnComputerFirst = true;
        plan.computerFolder = backupRootFolder().getChildFile (plan.folderName).getFullPathName();
    }
    else if (mirrorPolicy.isEnabledByUser())
    {
        const auto request = currentFilesystemProbeRequest();
        filesystemStatusProbe.setRequest (request);
        const auto snapshot = filesystemStatusProbe.getSnapshot();
        const bool sharesDisk = snapshot.ready && snapshot.request.destinationPath == destinationFolder
                             && snapshot.request.backupRootPath == request.backupRootPath
                             && snapshot.backupSharesDisk == 1;

        if (sharesDisk && ! keepSameDiskBackupForTesting())
            plan.mirrorNote = mirrorplacement::sameDiskShortNote();
        else
            plan.mirrorFolder = backupRootFolder().getChildFile (plan.folderName).getFullPathName();
    }

    // §6.1: one file per microphone plus the mix, exactly as WritePipeline
    // opens them, plus the §6.2 session.json.
    plan.fileNames.add ("MIX.wav");

    for (const auto& c : buildCaptureChannels())
        plan.fileNames.add (juce::String (c.fileName) + ".wav");

    // One file per camera in the take, in the same folder. Listed with the rest
    // because "where are my files" has one answer, not two.
    plan.fileNames.addArray (cameraController.getPlannedFileNames());

    plan.fileNames.add ("session.json");

    return plan;
}

bool Application::isSaveLocationConfirmed() const
{
    if (askWhereToSaveEveryTime)
        return false;

    return ! confirmedSaveLocation.empty() && confirmedSaveLocation == destinationFolder;
}

std::vector<Application::SavedFile> Application::listSessionFiles (const juce::String& folder)
{
    std::vector<SavedFile> files;

    if (folder.isEmpty())
        return files;

    const juce::File dir (folder);

    if (! dir.isDirectory())
        return files;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
    {
        const auto file = entry.getFile();

        // Finder's .DS_Store and an ExFAT card's "._MIX.wav" twins are not
        // files of this take, and the card would list and count them as such.
        if (isSystemClutterFile (file.getFileName().toStdString()))
            continue;

        files.push_back ({ file.getFileName(), file.getSize() });
    }

    // MIX first, session.json last, the stems in between in the channel order
    // their "01_", "02_" prefixes already encode. A user reading this list is
    // looking for "is everyone here", and the mix is the file they will play.
    const auto rank = [] (const juce::String& name)
    {
        if (name.startsWithIgnoreCase ("MIX")) return 0;
        if (name.equalsIgnoreCase ("session.json")) return 2;
        return 1;
    };

    std::sort (files.begin(), files.end(), [&rank] (const SavedFile& a, const SavedFile& b)
    {
        const int ra = rank (a.name), rb = rank (b.name);
        return ra != rb ? ra < rb : a.name.compareNatural (b.name) < 0;
    });

    return files;
}

std::vector<Application::SavedFile> Application::listSessionFilesWithin (const juce::String& folder,
                                                                      int limitMs,
                                                                      bool& completed)
{
    auto listed = runWithDeadline<std::vector<SavedFile>> ([folder] { return listSessionFiles (folder); },
                                                           std::chrono::milliseconds (limitMs));
    completed = listed.has_value();
    return completed ? std::move (*listed) : std::vector<SavedFile> {};
}

std::vector<Application::SavedFile> Application::getCurrentSessionFiles (bool* snapshotAvailable) const
{
    if (snapshotAvailable != nullptr)
        *snapshotAvailable = false;

    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    if (! snapshot.ready || snapshot.request != request || ! snapshot.filesObservationReady)
        return {};

    if (snapshotAvailable != nullptr)
        *snapshotAvailable = true;

    std::vector<SavedFile> files;
    files.reserve (snapshot.files.size());
    for (const auto& file : snapshot.files)
        files.push_back ({ juce::String (file.name), file.sizeBytes });
    return files;
}

bool Application::consumeCardRemovalNotice (CardRemovalNotice& out)
{
    if (! cardRemovalPending)
        return false;

    cardRemovalPending = false;
    out = cardRemovalNotice;

    return true;
}

bool Application::consumeSavedTake (SavedTake& out)
{
    if (! savedTakePending)
        return false;

    savedTakePending = false;
    out.folder = lastSessionFolder;
    out.mirrorFolder = lastMirrorFolder;
    out.mirrorNote = lastMirrorFolder.isEmpty() && lastTakeBackupSkippedForSameDisk
                         ? juce::String (mirrorplacement::sameDiskShortNote())
                         : juce::String();
    // Listed at stop, with a deadline. Reading the card again here would be a
    // second chance for a card pulled at that moment to freeze the window.
    out.files = lastSessionFiles;
    out.filesListed = lastSessionFilesListed;
    out.verdict = lastTakeVerdict;
    out.copiesToCard = lastTakeCopiesToCard;
    out.copyRoot = lastCopyRoot;
    out.routeNote = lastTakeRouteNote;

    return true;
}

Application::CardCopyView Application::getCardCopyView (const juce::String& localFolder) const
{
    CardCopyView out;
    const auto v = cardCopier.view (localFolder.toStdString());
    const auto where = v.folderNow.empty() ? localFolder.toStdString() : v.folderNow;

    switch (v.phase)
    {
        case CardCopier::Phase::None:
        case CardCopier::Phase::Waiting:
            out.line = cardCheckIsHoldingCopies() ? juce::String (cardcopy::stalledLine (where))
                                                  : juce::String ("Waiting to copy to the card.");
            break;

        case CardCopier::Phase::Copying:
            out.line = cardCopier.msSinceProgress() > CardCopier::kStalledAfterMs
                ? juce::String (cardcopy::stalledLine (where))
                : juce::String (cardcopy::copyingLine (v.done, v.total));
            break;

        case CardCopier::Phase::Copied:
            out.line = juce::String (cardcopy::describe (v.result, where));
            out.cardFolderIfCopied = juce::String (v.result.cardFolder);
            break;

        case CardCopier::Phase::StillOnComputer:
            out.line = juce::String (cardcopy::describe (v.result, where));
            out.canCopyAgain = v.result.outcome != cardcopy::Outcome::ComputerCopyMissing;
            break;
    }

    return out;
}

bool Application::cardCheckIsHoldingCopies() const
{
    // Held only by the card's own check for interrupted takes -- not by a
    // take, or its movies being finished -- and for as long as a copy is
    // given before the card is said to have stopped answering.
    return ! isRecording() && pendingStoppedTakeCompletion == nullptr
        && ! cameraController.isFinalizingRecording()
        && destinationRecoveryTask.isRunning()
        && cardCopier.msHeld() > CardCopier::kStalledAfterMs;
}

juce::String Application::getCardCopySummary() const
{
    const auto s = cardCopier.summary();

    if (! s.copyingFolder.empty())
        return cardCopier.msSinceProgress() > CardCopier::kStalledAfterMs
            ? juce::String (cardcopy::stalledLine (s.copyingFolder))
            : "Copying " + juce::File (juce::String (s.copyingFolder)).getFileName() + " to the card: "
                  + juce::String (s.total > 0 ? std::min<uint64_t> (100, s.done * 100 / s.total) : 0) + "%.";

    // Queued and not started: said, rather than an empty line that reads as
    // "nothing left to copy".
    if (s.waiting > 0)
        return cardCheckIsHoldingCopies()
            ? juce::String (cardcopy::notAnsweringLine (s.waiting, backupRootFolder().getFullPathName().toStdString()))
            : juce::String (cardcopy::waitingLine (s.waiting));

    return juce::String (cardcopy::tallyLine (s));
}

void Application::copyAgain (const juce::String& localFolder)
{
    if (cardCopier.copyAgain (localFolder.toStdString()))
        stallReportedFor.erase (localFolder);
}

void Application::setRecordOnComputerThenCopy (bool enabled)
{
    if (recordOnComputerThenCopy == enabled)
        return;

    recordOnComputerThenCopy = enabled;
    saveSettings();

    // Off: takes go straight to the card again, so the card has to pass the
    // speed check before the next one -- it may never have been asked. Not
    // during a take: the check writes 200 MB to the card, the load this
    // setting keeps away from a take. The Record gate starts it after Stop.
    if (! enabled && ! isRecording())
        beginPreflightForDestination();
}

double Application::getElapsedRecordingSeconds() const
{
    if (recordingEngine.getState() != RecordingState::Recording || recordingStartMs <= 0.0)
        return 0.0;

    return (juce::Time::getMillisecondCounterHiRes() - recordingStartMs) / 1000.0;
}

int Application::getIncludedMicCount() const
{
    // During a take this is the count the meters, the drift reports, the
    // advisor and the channel strips are all indexed by, and every one of those
    // reads out of capture. The device list is not the same length once a
    // microphone is unplugged mid-take -- counting it there left the last
    // channel's meter unread and shifted every name past the gap onto the
    // wrong strip.
    if (capture != nullptr && capture->isRecording())
        return static_cast<int> (capture->getChannels().size());

    // Outside a take, the count the take WOULD produce. Counting included
    // devices instead under-counted every interface: a two-input interface
    // showed one strip and reserved disk for one track, then recorded two.
    // §6.4's remaining-time figure was wrong by the input multiplier -- double
    // on a 2-input box, quadruple on a 4-input one -- which is a promise of
    // recording time the disk cannot keep.
    int count = 0;
    for (const auto& d : planDevices())
        count += takeChannelsForDevice (d.inputChannelCount, d.knownDuplicateStereo);

    return count;
}

double Application::bytesPerSecondOfAudio() const
{
    const int channels = std::max (1, getIncludedMicCount());
    const int bytesPerSample = std::max (1, currentBitDepth / 8);

    // Stems plus the mix file, matching the pre-flight required-rate figure (§6.4).
    return static_cast<double> (channels) * currentSampleRate
         * static_cast<double> (bytesPerSample) * 2.0;
}

int64_t Application::projectedSessionBytes() const
{
    // §6.3 needs a size to reserve for the mirror before the take starts, and
    // nothing knows how long the user will record. An hour is the planning
    // figure: long enough that a typical session fits, short enough that the
    // mirror is not refused on a drive that could hold it.
    constexpr double kProjectedSessionSeconds = 60.0 * 60.0;
    return static_cast<int64_t> (bytesPerSecondOfAudio() * kProjectedSessionSeconds);
}

double Application::bytesPerSecondOfRecording() const
{
    return bytesPerSecondOfAudio()
         + static_cast<double> (cameraController.getSelection().getEstimatedBytesPerSecond());
}

FilesystemStatusProbe::Request Application::currentFilesystemProbeRequest() const
{
    FilesystemStatusProbe::Request request;
    const bool isRecording = recordingEngine.getState() == RecordingState::Recording;

    // While a take is running, measure the folder actually being written, not
    // a destination setting that may have changed for the next take.
    request.destinationPath = (isRecording && currentSessionFolder.isNotEmpty()
                                ? currentSessionFolder
                                : juce::String (destinationFolder)).toStdString();
    request.sessionFolder = isRecording ? currentSessionFolder.toStdString() : std::string {};
    request.bytesPerSecond = bytesPerSecondOfRecording();

    const bool mirroring = capture != nullptr && capture->isMirroring();

    // Between takes with copying on, this computer's free space as well:
    // whether the next take fits here decides where it records.
    if (mirroring || (recordOnComputerThenCopy && ! isRecording))
        request.mirrorPath = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getFullPathName().toStdString();

    // §6.3: whether the backup shares the destination's disk, for the save
    // prompt's preview -- and, for a backup that is running there anyway,
    // the stems and mix it writes come out of the same free space. The same
    // answer says whether the destination is on this computer at all, which
    // decides whether a take is recorded here and copied to it.
    if (mirroring || mirrorPolicy.isEnabledByUser() || recordOnComputerThenCopy)
        request.backupRootPath = backupRootFolder().getFullPathName().toStdString();

    if (mirroring)
        request.backupBytesPerSecond = bytesPerSecondOfAudio();

    return request;
}

juce::File Application::backupRootFolder()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("RECORDINGS-MIRROR");
}

bool Application::keepSameDiskBackupForTesting()
{
   #if defined (MMA_UI_WALK)
    // Test builds only. The end-to-end walks record into a scratch home on one
    // disk and still need the backup machinery to run so it can be checked.
    if (const char* keep = std::getenv ("MMA_KEEP_SAME_DISK_BACKUP"))
        return juce::String (keep) == "1";
   #endif
    return false;
}

bool Application::backupWouldShareDestinationDisk() const
{
    if (keepSameDiskBackupForTesting())
        return false;

    // Unknown keeps the backup, as a disk that cannot be identified does.
    return destinationOnThisComputer (true) == 1;
}

bool Application::treatAsCardForTesting (const std::string& destination)
{
   #if defined (MMA_UI_WALK)
    // Test builds only. The end-to-end gates record into a scratch home on one
    // disk and still need a destination that counts as a card.
    if (const char* prefix = std::getenv ("MMA_TREAT_AS_CARD"); prefix != nullptr && *prefix != '\0')
    {
        const juce::File card { juce::String::fromUTF8 (prefix) };
        const juce::File folder { juce::String (destination) };
        return folder == card || folder.isAChildOf (card);
    }
   #else
    juce::ignoreUnused (destination);
   #endif
    return false;
}

int Application::destinationOnThisComputer (bool mayWait) const
{
    if (treatAsCardForTesting (destinationFolder))
        return 0;

    // The background probe has usually answered already; asking it costs the
    // message thread nothing.
    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    if (snapshot.ready && snapshot.request.destinationPath == destinationFolder
        && snapshot.request.backupRootPath == request.backupRootPath
        && snapshot.backupSharesDisk >= 0)
    {
        placementByDestination[destinationFolder] = snapshot.backupSharesDisk;
        return snapshot.backupSharesDisk;
    }

    if (const auto known = placementByDestination.find (destinationFolder);
        known != placementByDestination.end())
        return known->second;

    if (! mayWait)
        return -1;

    // Otherwise ask on a disposable worker: the destination is often a card,
    // and a card that stops answering as Record is pressed would hold the
    // window for as long as it hangs.
    const std::string destination = destinationFolder;
    const std::string backup = backupRootFolder().getFullPathName().toStdString();
    const auto answer = runWithDeadline<int> ([destination, backup]
    {
        const auto there = volumeIdentityOf (destination);
        const auto here = volumeIdentityOf (backup);

        if (mirrorplacement::sharesDisk (there, here))
            return 1;

        return there.device.empty() || here.device.empty() ? -1 : 0;
    }, std::chrono::milliseconds (1500));

    return answer.value_or (-1);
}

bool Application::destinationPlacementKnown() const
{
    if (treatAsCardForTesting (destinationFolder) || isRecording())
        return true;

    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    const bool answered = (snapshot.ready && snapshot.request.destinationPath == destinationFolder
                           && snapshot.request.backupRootPath == request.backupRootPath)
                       || placementByDestination.count (destinationFolder) > 0;

    // A card that hangs before the probe's first answer holds the probe in
    // that call for good. After a short wait, not knowing is an answer: the
    // take is recorded on this computer, as the arm's own look concludes.
    return placementWait.givenUp (destinationFolder, answered, juce::Time::getMillisecondCounterHiRes())
        || answered;
}

TakeRoute Application::plannedTakeRoute (bool authoritative) const
{
    TakeRouteInputs in;
    in.copyAfterTakeEnabled = recordOnComputerThenCopy;

    if (! in.copyAfterTakeEnabled)
        return TakeRoute::StraightToDestination;

    in.destinationOnThisComputer = destinationOnThisComputer (authoritative);

    if (in.destinationOnThisComputer == 1)
        return TakeRoute::StraightToDestination;

    // Room for audio AND cameras, for as long as the card could have held
    // them: the take writes both here. The card's room is the probe's last
    // look between takes, so arming asks the card nothing.
    in.bytesPerSecond = bytesPerSecondOfRecording();

    if (const auto cardRoom = destinationRoomFromProbe(); cardRoom >= 0.0)
        in.destinationRemainingSeconds = cardRoom;

    if (authoritative)
    {
        // Purgeable space counted, as for the backup copy.
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        if (const auto free = availableBytesForRecording (home.getFullPathName().toStdString()))
            in.computerFreeBytes = static_cast<int64_t> (std::min<uint64_t> (*free, static_cast<uint64_t> (INT64_MAX)));
    }
    else if (const auto free = getMirrorFreeBytes(); free >= 0)
    {
        in.computerFreeBytes = free;
    }

    return decideTakeRoute (in);
}

double Application::destinationRoomFromProbe() const
{
    // Between takes the probe measures the save location itself; during a
    // take it measures the take's own folder, which is not the card's room.
    if (isRecording())
        return -1.0;

    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    if (! snapshot.ready || ! (snapshot.request == request) || snapshot.request.destinationPath != destinationFolder)
        return -1.0;

    return snapshot.remainingSeconds;
}

bool Application::wouldRecordOnComputer() const
{
    return plannedTakeRoute (false) == TakeRoute::ComputerThenCard;
}

int64_t Application::getMirrorFreeBytes() const
{
    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();
    return snapshot.ready && snapshot.request == request ? snapshot.mirrorFreeBytes : -1;
}

double Application::getRemainingRecordingSeconds() const
{
    const auto request = currentFilesystemProbeRequest();
    if (request.bytesPerSecond <= 0.0)
        return -1.0;

    // Between takes, a take that will record on this computer has this
    // computer's room, not the card's. During one the probe already measures
    // the take's own folder, which is here.
    if (! isRecording() && wouldRecordOnComputer())
    {
        const auto free = getMirrorFreeBytes();
        return free >= 0 ? static_cast<double> (free) / request.bytesPerSecond : -1.0;
    }

    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();
    return snapshot.ready && snapshot.request == request ? snapshot.remainingSeconds : -1.0;
}

juce::String Application::formatDuration (double seconds)
{
    if (seconds < 0.0)
        return "--";

    const auto total = static_cast<int64_t> (seconds);
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto secs = total % 60;

    if (hours > 0)
        return juce::String (hours) + "h " + juce::String (minutes).paddedLeft ('0', 2) + "m";

    return juce::String (minutes) + "m " + juce::String (secs).paddedLeft ('0', 2) + "s";
}

juce::String Application::getRecordDisabledReason() const
{
    // Nothing disables the button while a take is running, because the button
    // IS the stop control and it is the only thing in the app that can end a
    // take.
    //
    // Every reason below is about whether it is sensible to START. Applied
    // during a take they take the stop away: unplug every microphone mid-take
    // and the no-microphone message disabled the one control that could
    // have ended the recording, leaving the clock running with no way out but
    // quitting the app. A user who cannot stop their own take has been failed
    // more completely than by any silence.
    if (isRecording())
        return {};

    if (pendingStoppedTakeCompletion != nullptr
        || cameraController.isFinalizingRecording())
        return "Finishing the camera files from the last take. Record will be ready when they are safely closed.";

    // Ahead of the microphone count on purpose. A denied microphone permission
    // is invisible to enumeration: the count is zero for the same reason it
    // would be with nothing plugged in, and the two need different fixes.
    // A take recorded on this computer writes only under the home folder, so
    // the card's write permission is not this take's to refuse; a card that
    // refuses writes is said by the copy, with the take safe here.
    for (const auto& problem : PermissionGuidance::evaluate (microphonePermission,
                                                             wouldRecordOnComputer()
                                                                 ? PermissionState::Granted
                                                                 : destinationWritePermission,
                                                             ! destinationFolder.empty(),
                                                             microphoneRestricted))
        if (problem.blocksRecording)
            return juce::String (problem.message);

    if (getIncludedMicCount() == 0)
        return "Plug in a USB microphone or audio interface first.";

    // The microphones have to be OPEN, not merely plugged in. A rig whose
    // recording-input streams failed to open -- for example, a mic held by
    // another app -- used to leave this button live, and pressing it produced
    // a take of empty files with the clock running. Output failure falls back
    // to input-only recording and is shown separately as a monitoring warning.
    if (capture == nullptr || ! capture->isMonitoring())
    {
        const auto problem = capture != nullptr ? capture->getMonitorProblem() : std::string();
        return problem.empty() ? juce::String ("The microphones aren't open yet.")
                               : "The microphones aren't open: " + juce::String (problem);
    }

    // macOS raises the microphone prompt when the streams above open, and
    // feeds them silence until it is answered. Only now is the prompt really
    // on screen, so this sits after the open check: earlier, a Mac with nothing
    // plugged in would be told to answer a question that was never asked. The
    // 2 s permission poll reopens the streams on Allow and Record turns on.
#if JUCE_MAC
    // About -120 dBFS: above the exact zeros an unanswered or refused macOS
    // microphone prompt delivers, below any real microphone's noise floor.
    constexpr float kRealSoundThreshold = 1.0e-6f;

    if (const auto promptReason = PermissionGuidance::pendingPromptReason (
            microphonePermission, true, capture->getPeakArrived() > kRealSoundThreshold);
        ! promptReason.empty())
        return juce::String (promptReason);
#endif

    // Recovery can repair WAV headers. Do not let a new writer create files
    // under either active write root until the corresponding one-shot scan has
    // finished. In particular, a mirror scan that was blocked in the OS must
    // never resume later and mistake this process's current take for a crashed
    // one. Finished results are consumed here, on the message thread, before
    // the button can become enabled.
    if (const auto recoveryReason = recoveryBlockingReason(); recoveryReason.isNotEmpty())
        return recoveryReason;

    // Where the take goes is decided against the probe's answer for this
    // destination, so wait the moment it takes to give one rather than arm on
    // a guess. Only when copying is on: off, every take goes straight there.
    if (recordOnComputerThenCopy && ! destinationPlacementKnown())
        return "Checking this save location...";

    // A take recorded on this computer and copied to the card afterwards is
    // not written to the card at all, so nothing about the card can refuse it:
    // not its speed, its room or its format. Those are the copy's to report,
    // after the take, with the take safe here. Only a computer too full to
    // hold the take sends it straight to the card, and then every check below
    // applies.
    if (wouldRecordOnComputer())
        return {};

    return directRecordingBlockedReason();
}

juce::String Application::directRecordingBlockedReason() const
{
    // §6.4 blocks arming on a drive that is too SLOW. A drive with no room at
    // all was not checked here at all: the button stayed live, the take started
    // and was stopped by the capacity check a moment later. Refusing before the
    // take is the same principle, and it is the answer to "what tells the user
    // when the drive is full and nothing is recording".
    if (getRemainingRecordingSeconds() == 0.0)
        return "This drive is full. Free some space, or choose another drive.";

    publishCompletedPreflight();
    startPreflightIfNeeded();

    // §6.4: pre-flight blocks arming rather than degrading mid-take.
    const bool preflightRunning = publishCompletedPreflight();
    const auto it = preflightResults.find (destinationFolder);

    // A quiet re-check of a card that could not be written keeps saying so
    // until the new verdict lands, rather than flashing "checking" every few
    // seconds while the card is still missing.
    if (preflightTaskDestination == destinationFolder && preflightRunning
        && (it == preflightResults.end() || ! it->second.couldNotWrite))
        return "Checking this drive is fast enough...";

    // There is no safe third state between "benchmark is running" and "a
    // verdict exists". In particular, a completion which lands between two
    // separate result/running reads must not open a one-frame window where a
    // keyboard action can arm without ever evaluating the drive.
    if (it == preflightResults.end())
        return recoveryMutationGate.isActive (destinationFolder)
            ? juce::String ("A previous check is still finishing on this save location. "
                            "Choose another location, or wait for the drive to respond.")
            : juce::String ("Couldn't confirm this drive is fast enough yet. Choose another "
                            "location, or reconnect this drive and try again.");

    // The verdict is for the card that was measured, and the probe has since
    // found another one -- or none -- at this path. The status tick is about
    // to throw it away and check the drive that is there; until then it arms
    // nothing.
    if (probeSeesAnotherDriveThanChecked (it->second))
        return "Checking this drive is fast enough...";

    if (it != preflightResults.end())
    {
        // The benchmark measured the card; the gate is about this take. They
        // are applied apart so that switching a camera or a microphone on
        // re-answers the question here, rather than leaving the verdict
        // frozen at whatever the rig was when the 200 MB test last ran --
        // which would let a card pass for the audio and then fail mid-take
        // once a camera started, the exact outcome §6.4 exists to prevent.
        //
        // A card that could not be written is refused as that: with no
        // windows it read "too slow, 0 MB/s", and a run cut short by the
        // failure could even have measured fast enough to pass.
        const auto verdict = PreflightThroughputTest::evaluateCached (
            it->second,
            std::max (1, getIncludedMicCount()),
            currentSampleRate,
            std::max (1, currentBitDepth / 8),
            static_cast<double> (cameraController.getSelection().getEstimatedBytesPerSecond()));

        if (! verdict.passed)
            return juce::String (verdict.reason);

        if (const auto refusal = PreflightThroughputTest::fileSizeRefusal (
                verdict.limitedTo4GiBFiles, cameraController.getSelection().getEnabledCount());
            ! refusal.empty())
            return juce::String (refusal);
    }

    return {};
}

void Application::beginPreflightForDestination()
{
    publishCompletedPreflight();
    startPreflightIfNeeded();
}

bool Application::isPreflightRunning() const
{
    const bool running = publishCompletedPreflight();
    return preflightTaskDestination == destinationFolder && running;
}

bool Application::publishCompletedPreflight() const
{
    auto snapshot = preflightTask.poll();
    auto completed = std::move (snapshot.result);
    if (! completed.has_value())
        return snapshot.running;

    // A re-check of a card that still cannot be written is not news; saying it
    // again every retry interval would bury the rest of the activity list.
    const auto previous = preflightResults.find (completed->destination);
    const bool alreadyReported = completed->couldNotWrite && previous != preflightResults.end()
                              && previous->second.couldNotWrite;

    preflightResults[completed->destination] = completed->result;
    preflightVerdictAtMs[completed->destination] = juce::Time::getMillisecondCounterHiRes();

    if (completed->couldNotWrite && ! alreadyReported)
        noteActivity (ActivityLevel::Failed, "Save location",
                      juce::String (completed->result.reason));

    return snapshot.running;
}

void Application::startPreflightIfNeeded() const
{
    if (destinationFolder.empty())
        return;

    // No 200 MB test write to a card nothing will record onto: the take is
    // recorded on this computer and copied there after Stop. If this computer
    // fills up, the next take goes straight to the card, and the Record gate
    // comes back through here to check it then.
    if (wouldRecordOnComputer())
        return;

    if (const auto cached = preflightResults.find (destinationFolder); cached != preflightResults.end())
    {
        // A card that could not be written is usually fixed by the user --
        // space freed, the card reinserted or unlocked -- without choosing the
        // location again, which used to be the only thing that cleared this
        // verdict. Try again now and then while idle. The failed verdict stays
        // in place until the new one lands, so Record stays refused meanwhile.
        //
        // Published and polled in one step, before the age is read: a worker
        // that finished since the caller's last publish must land first, or its
        // verdict is thrown away and the card checked again at once.
        const bool workerRunning = publishCompletedPreflight();
        const auto verdictAt = preflightVerdictAtMs.find (destinationFolder);
        const double secondsSince = verdictAt == preflightVerdictAtMs.end()
            ? 0.0
            : (juce::Time::getMillisecondCounterHiRes() - verdictAt->second) / 1000.0;

        if (! PreflightThroughputTest::shouldRetryCached (
                cached->second, secondsSince,
                recordingEngine.getState() == RecordingState::Idle,
                workerRunning))
            return;

        // The previous worker finished; nothing of it is left to abandon.
        preflightTaskDestination.clear();
    }

    const auto target = destinationFolder;

    if (preflightTaskDestination == target)
    {
        if (publishCompletedPreflight())
            return;

        if (preflightResults.count (target) > 0)
            return;

        // A completed worker always publishes a result. Reaching this branch
        // means its callable threw (or its thread could not be created). Hold a
        // safe failed verdict instead of launching a new worker on every UI
        // tick or silently allowing recording without a benchmark.
        PreflightResult failed;
        failed.passed = false;
        failed.reason = "Couldn't check whether this drive is fast enough. Choose another drive, or reconnect this one and try again.";
        preflightResults[target] = failed;
        noteActivity (ActivityLevel::Failed, "Save location", juce::String (failed.reason));
        return;
    }

    // A syscall against the previous destination may never return. Abandoning
    // replaces only the shared result state; the old worker keeps ownership of
    // its files and cancellation flag, while the new safe destination starts
    // immediately and cannot receive a late old result.
    if (! preflightTaskDestination.empty())
        preflightTask.abandon();

    preflightTaskDestination = target;
    const int channelCount = std::max (1, getIncludedMicCount());
    const double sampleRate = currentSampleRate;
    const int bytesPerSample = std::max (1, currentBitDepth / 8);
    const auto mutationGate = recoveryMutationGate;

    const bool launched = preflightTask.start (
        [target, channelCount, sampleRate, bytesPerSample,
         mutationGate]
        (const std::atomic<bool>& cancelled) mutable
        {
            // Preflight creates, flushes and removes a 200 MB file.
            // Cancellation suppresses publication but cannot pull a wedged
            // syscall off the kernel. Resolve aliases and acquire the shared
            // recovery/preflight lease on this detached worker, so reselecting
            // the same physical volume under another spelling cannot race it.
            auto mutationLease = waitForMutationLease (
                mutationGate, { target }, cancelled);
            if (mutationLease == nullptr)
                return PreflightBackgroundResult { target, {}, false };

            auto result = Application::runPreflight (target, channelCount, sampleRate,
                                                     bytesPerSample, cancelled);
            // Release the path before DetachedResultTask publishes completion.
            // A caller which atomically sees running=false plus this result can
            // therefore authorize the measured root immediately and safely.
            mutationLease.reset();
            return result;
        });

    if (! launched)
    {
        PreflightResult failed;
        failed.passed = false;
        failed.reason = "Couldn't start the drive speed check. Choose another drive, or restart SobStage and try again.";
        preflightResults[target] = failed;
        noteActivity (ActivityLevel::Failed, "Save location", juce::String (failed.reason));
    }
}

// The error the OS gives for creating a file in this folder (or the folder
// itself, when it could not be made), read straight from errno -- JUCE's
// stream keeps only a message. 0 when the probe succeeded or can't tell.
int Application::probeWriteErrno (const juce::File& folder)
{
   #if JUCE_MAC || JUCE_LINUX
    if (! folder.isDirectory())
    {
        errno = 0;
        if (::mkdir (folder.getFullPathName().toRawUTF8(), 0755) != 0)
            return errno;
    }

    const auto probe = folder.getChildFile (".sobstage-write-probe");
    errno = 0;
    const int fd = ::open (probe.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return errno;

    ::close (fd);
    ::unlink (probe.getFullPathName().toRawUTF8());
   #else
    juce::ignoreUnused (folder);
   #endif
    return 0;
}

// The OS's name for the format of the drive holding this folder, or empty.
std::string Application::filesystemTypeName (const juce::File& folder)
{
   #if JUCE_MAC
    // The nearest folder that exists. On a read-only drive the RECORDINGS
    // folder could not be made, so asking about it failed and the format came
    // back unknown -- exactly when it decides what the user is told to do.
    for (auto candidate = folder;; candidate = candidate.getParentDirectory())
    {
        struct statfs info {};
        if (::statfs (candidate.getFullPathName().toRawUTF8(), &info) == 0)
            return info.f_fstypename;

        if (errno != ENOENT || candidate.getParentDirectory() == candidate)
            break;
    }
   #else
    juce::ignoreUnused (folder);
   #endif
    return {};
}

Application::PreflightBackgroundResult Application::runPreflight (
    std::string destination, int channelCount, double sampleRate,
    int bytesPerSample, const std::atomic<bool>& cancelled)
{
    PreflightBackgroundResult completed;
    completed.destination = std::move (destination);
    PreflightResult result;

    const juce::File folder { juce::String (completed.destination) };
    folder.createDirectory();

    if (cancelled.load (std::memory_order_acquire))
        return completed;

    // Which card this measures, read before the test file goes on it. The
    // verdict is kept for the path, and the same path can later hold another
    // card; the status probe and Record compare what is there with this, so a
    // swapped card is checked again rather than armed on the old one's speed
    // and format.
    const auto measuredVolume = volumeFingerprintOf (completed.destination);

    // Whatever an interrupted earlier check left here goes first. This worker
    // holds the destination's lease, so no other check can be writing one.
    for (const auto& f : folder.findChildFiles (juce::File::findFiles,
                                                false, "*.tmp"))
        if (PreflightScratchFile::isLeftover (f.getFileName().toStdString()))
            f.deleteFile();

    const auto testFile = folder.getChildFile (PreflightScratchFile::kName);

    // JUCE opens an existing file to append; a leftover that would not delete
    // must not make this run's file start 200 MB in.
    testFile.deleteFile();

    std::vector<double> rollingWindows;

    // A card that will not take the test file at all is not a slow card, and
    // must not be reported as one. With no windows measured the gate below
    // reads 0 MB/s and says "this card is too slow", which sends someone
    // shopping for a faster card when the card is read-only, full, or gone.
    bool couldNotWrite = false;
    int writeErrno = 0;

    {
        // §6.4: 200 MB, written the way a take writes -- steadily, measuring the
        // sustained floor rather than a burst into the OS cache.
        juce::FileOutputStream out (testFile);

        if (! out.openedOk())
        {
            couldNotWrite = true;
            writeErrno = probeWriteErrno (folder);
        }
        else
        {
            constexpr size_t kChunkBytes = 1024 * 1024;
            const std::vector<char> chunk (kChunkBytes, 0);

            size_t written = 0;
            size_t writtenThisWindow = 0;
            auto windowStart = std::chrono::steady_clock::now();

            while (written < PreflightThroughputTest::kTestFileBytes)
            {
                // Quit must not wait for a slow card to swallow 200 MB.
                if (cancelled.load (std::memory_order_acquire))
                    break;

                if (! out.write (chunk.data(), kChunkBytes))
                {
                    // Stopped taking writes part way. Whatever windows were
                    // measured before that describe a card that is no longer
                    // accepting audio, so they are not a verdict either.
                    couldNotWrite = true;
                    break;
                }

                written += kChunkBytes;
                writtenThisWindow += kChunkBytes;

                const auto elapsedBeforeFlush = std::chrono::duration<double> (
                    std::chrono::steady_clock::now() - windowStart).count();

                if (elapsedBeforeFlush >= 1.0)
                {
                    // FileOutputStream::flush() reaches the platform durability
                    // primitive (fsync/FlushFileBuffers). Include that latency in
                    // every rolling window so a large OS cache cannot make a slow
                    // card look safe for a long recording.
                    out.flush();

                    if (out.getStatus().failed())
                    {
                        couldNotWrite = true;
                        break;
                    }

                    const auto durableElapsed = std::chrono::duration<double> (
                        std::chrono::steady_clock::now() - windowStart).count();
                    rollingWindows.push_back (static_cast<double> (writtenThisWindow) / durableElapsed);
                    writtenThisWindow = 0;
                    windowStart = std::chrono::steady_clock::now();
                }
            }

            if (! cancelled.load (std::memory_order_acquire))
                out.flush();

            // FileOutputStream::flush() returns void; the stream carries the
            // outcome instead. A flush that failed means the bytes counted as
            // written above never reached the card, so the windows measured
            // from them describe nothing.
            if (! cancelled.load (std::memory_order_acquire) && out.getStatus().failed())
                couldNotWrite = true;

            // Include the final partial window too. This gives a fast card its
            // only sample and makes the last durability flush part of the
            // sustained-floor verdict for every other card.
            if (! cancelled.load (std::memory_order_acquire)
                && ! couldNotWrite && writtenThisWindow > 0)
            {
                const auto elapsed = std::chrono::duration<double> (
                    std::chrono::steady_clock::now() - windowStart).count();

                if (elapsed > 0.0)
                    rollingWindows.push_back (static_cast<double> (writtenThisWindow) / elapsed);
            }
        }
    }

    testFile.deleteFile();

    // An abandoned run proved nothing. DetachedResultTask suppresses this
    // return after cancellation, but avoiding the calculation makes that
    // ownership contract explicit and keeps the worker independent of the
    // Application that launched it.
    if (cancelled.load (std::memory_order_acquire))
        return completed;

    // What is kept from this is the measurement. The pass/fail and the wording
    // alongside it are a snapshot of the rig as it was during the benchmark;
    // getRecordDisabledReason() reapplies the gate to the current rig.
    result = PreflightThroughputTest::evaluate (rollingWindows, channelCount,
                                                sampleRate, bytesPerSample);

    // The format, read here on the worker: a statfs on a card that has stopped
    // answering must not stall the message thread. Read before the refusal is
    // worded, because a read-only NTFS drive needs different advice from a
    // locked card.
    const auto filesystemKind =
        PreflightThroughputTest::filesystemKindFromTypeName (filesystemTypeName (folder));

    if (couldNotWrite)
    {
        result.passed = false;
        result.couldNotWrite = true;
        result.reason = PreflightThroughputTest::writeFailureReason (writeErrno, filesystemKind);
    }

    result.limitedTo4GiBFiles = PreflightThroughputTest::needsReformat (filesystemKind);

    // Stamped once the test file is gone. A card swapped while it was being
    // measured is still described by the card it began on, so the probe's
    // first look after this finds a different one there and has it checked
    // from the start -- and Record compares again before it arms.
    result.measuredVolume = measuredVolume;
    result.measuredVolumeConfirmedAt = std::chrono::steady_clock::now();

    completed.result = std::move (result);
    completed.couldNotWrite = couldNotWrite;
    return completed;
}

void Application::setMasterVolume (double volume0to100)
{
    // Held here rather than only on the bus: §2.2 and §5.4 can both rebuild the
    // coordinator underneath us, and the listening level must not jump back to
    // the default when they do.
    masterVolume = volume0to100;

    if (auto* bus = getMonitorBus())
        bus->setMasterVolume (masterVolume);

    // Deliberately not saved here. This is the one control that moves
    // continuously while someone listens, and it is comfort rather than setup:
    // losing it costs a second to reset, where losing a trim costs the ear-work
    // that found it. It goes to disk with everything else at shutdown.
}

void Application::toggleMonitorMute()
{
    auto* bus = getMonitorBus();

    // §5: the runaway cut stays muted until the user says otherwise. The mute
    // button is that path -- without this, a runaway cut is a dead end the
    // user can only escape by restarting the app.
    if (bus != nullptr && bus->isRunawayMuted())
    {
        bus->manuallyUnmute();
        monitorMuted = false;
        bus->setGlobalMute (false);
        return;
    }

    // Recorded even with no bus up (mid-rebuild), so the press is not lost:
    // the next coordinator picks it up when it is built.
    monitorMuted = ! monitorMuted;
    if (bus != nullptr)
        bus->setGlobalMute (monitorMuted);
}

double Application::getMasterVolume() const
{
    return masterVolume;
}

void Application::setChannelTrimDb (int index, float trimDb)
{
    // §4: clamped to the stated range and quantised to the stated step, so the
    // value that gets persisted is one the UI can round-trip exactly.
    const auto clamped = juce::jlimit (static_cast<float> (MonitorBus::kMinTrimDb),
                                       static_cast<float> (MonitorBus::kMaxTrimDb), trimDb);
    const auto step = static_cast<float> (MonitorBus::kTrimStepDb);
    const auto quantised = std::round (clamped / step) * step;

    // `index` is a strip, resolved in channel space like every other strip
    // accessor. Walking included devices put the trim on the wrong device on
    // any interface with more than one socket, and the slider snapped back.
    const auto target = getMicRenameTarget (index);

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != target.deviceKey)
            continue;

        // §4 persists trim against the physical port, not the slot, so it
        // follows the mic when it is unplugged and moved. On an interface it
        // is kept per input: one box-wide value came back on every socket.
        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        settings.setTrimDbForInput (target.deviceChannel, keepsTrimPerInput (d, settings), quantised);
        portIdentityStore.put (d.identity, settings);
        break;
    }

    if (capture != nullptr)
        capture->setChannelTrimDb (index, quantised);

    // Written on each step rather than only at quit: a trim is setup the user
    // did by ear and would have to redo, and §4 quantises it to 0.5 dB, so a
    // drag across the whole range is a few dozen writes of a small file in the
    // application-data folder -- never on the card a take is being written to.
    saveSettings();
}

float Application::getChannelTrimDb (int index) const
{
    const auto target = getMicRenameTarget (index);

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != target.deviceKey)
            continue;

        if (const auto settings = portIdentityStore.get (d.identity))
            return settings->trimDbForInput (target.deviceChannel);

        return 0.0f;
    }

    return 0.0f;
}

juce::String Application::getActiveBackendDescription() const
{
    if (virtualDeviceBackend == nullptr)
        return "None";

    const auto status = virtualDeviceBackend->getStatus();

    // §7/§10.3: say what other apps can see, not which driver model is in use.
    if (! status.reachDescription.empty())
        return juce::String (status.reachDescription);

    return "No. Only SobStage hears these mics.";
}

const std::vector<std::string>& Application::getOutputDeviceNames() const
{
    // §2: enumeration happens on the OS device-change notification, never on a
    // timer. The Advanced panel repaints at 2 Hz and must read this cache
    // rather than go back to the driver each time.
    return outputDeviceNames;
}

juce::String Application::getDriftReport() const
{
    juce::StringArray lines;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        // §3.1: drift is not claimed until 60 seconds of measurement exist.
        // Showing a number before then would be showing noise.
        lines.add (juce::String (d.displayName) + ": "
                   + (d.hasDriftMeasurement
                          ? juce::String (d.measuredDriftPpm, 1) + " PPM"
                          : juce::String ("measuring...")));
    }

    if (lines.isEmpty())
        return "No microphones connected.";

    return lines.joinIntoString ("\n");
}

void Application::setOutputDeviceByName (const juce::String& displayName)
{
    if (audioBackend == nullptr)
        return;

    const auto found = outputDeviceIdByLabel.find (displayName.toStdString());
    if (found == outputDeviceIdByLabel.end())
        return;

    // §5.3: an explicit choice is remembered and outranks the automatic
    // priority order from then on, including after the app is relaunched.
    if (found->second == kOurAggregateUid)
    {
        monitorThroughCombinedDevice = true;
    }
    else
    {
        monitorThroughCombinedDevice = false;
        rememberedOutputDeviceId = found->second;
    }

    saveSettings();
    reselectOutputDevice();
    restartCapture();
}

std::vector<Application::MicSelection> Application::getMicSelections() const
{
    std::vector<MicSelection> out;

    for (const auto& d : deviceManager.getDevices())
    {
        const auto persisted = portIdentityStore.get (d.identity);
        const bool knownDuplicateStereo = persisted.has_value()
                                       && persisted->hasChannelLayoutDecision
                                       && persisted->channelLayoutIsMono;

        MicSelection m;
        m.deviceKey = juce::String (d.identity.key());
        m.displayName = juce::String (d.displayName);
        m.enabled = d.userEnabled;
        m.isBuiltIn = d.isBuiltIn;
        m.channelCount = takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo);

        // One row per socket on an interface, each switchable and each showing
        // the name its person has been given, so the list reads as who is
        // being recorded rather than which boxes are plugged in.
        if (m.channelCount > 1)
        {
            for (int input = 0; input < m.channelCount; ++input)
            {
                MicSelection::Input in;
                in.index = input;
                in.label = nameForChannel (d.identity.key(), input);
                in.enabled = ! persisted.has_value()
                          || std::find (persisted->disabledInputs.begin(),
                                        persisted->disabledInputs.end(), input)
                             == persisted->disabledInputs.end();
                m.inputs.push_back (std::move (in));
            }
        }

        out.push_back (std::move (m));
    }

    return out;
}

void Application::setInputEnabled (const juce::String& deviceKey, int input, bool enabled)
{
    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != deviceKey.toStdString())
            continue;

        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        auto& off = settings.disabledInputs;
        const auto it = std::find (off.begin(), off.end(), input);
        const bool currentlyOff = it != off.end();

        if (enabled == ! currentlyOff)
            return;

        if (enabled) off.erase (it);
        else         off.push_back (input);

        portIdentityStore.put (d.identity, settings);
        saveSettings();

        // The channel set changed, so the streams are reopened -- never
        // mid-take, where §6.5 fixes the channel list for the recording.
        requestCaptureRestart();

        return;
    }
}

void Application::setMicEnabledByKey (const juce::String& deviceKey, bool enabled)
{
    for (const auto& d : deviceManager.getDevices())
    {
        // Matched on identity, never on the name: four identical mics share
        // one, and every row switched the first of them. Not on `included`
        // either: a deselected microphone is excluded, and looking only at
        // included ones would make it impossible to tick back on.
        if (d.identity.key() != deviceKey.toStdString())
            continue;

        if (deviceManager.setUserEnabled (d.identity.key(), enabled))
            restartCapture(); // the channel set changed, so the streams must be reopened

        saveSettings();
        return;
    }
}

void Application::setDestinationFolder (const juce::File& folder)
{
    if (folder.getFullPathName().trim().isEmpty())
        return;

    // §6.5 fixes where a take is going for its duration, and nothing in the UI
    // stopped this being changed in the middle of one. The take kept writing to
    // the folder it opened with, so the setting and the recording disagreed --
    // and the change also cleared the agreed save location and kicked off a
    // 200 MB benchmark write while a take was running, neither of which was
    // announced.
    //
    // Deferred rather than refused, the same way a mic change mid-take is
    // (requestCaptureRestart): the user asked for it, so it happens, at the
    // first moment it can happen without touching a take.
    if (isRecording())
    {
        pendingDestinationFolder = folder.getFullPathName();

        noteActivity (ActivityLevel::Warning, "Save location",
                      "This take is still being written to where it started. Takes will be saved "
                      "to " + folder.getFullPathName() + " from the next one.");
        return;
    }

    applyDestinationFolder (folder);
}

void Application::applyDestinationFolder (const juce::File& folder)
{
    destinationFolder = folder.getFullPathName().toStdString();

    // §10.1: the user agreed to a place, not to a setting. Somewhere else has
    // not been agreed to, so it gets asked about before the next take.
    if (confirmedSaveLocation != destinationFolder)
        confirmedSaveLocation.clear();

    // Re-selecting the same mount path can still mean a different physical
    // card now occupies it. Retire that path's previous safety fact so the new
    // medium gets its own recovery scan; a stuck old worker keeps only its old
    // cancelled shared state and is never joined.
    if (destinationRecoveryRoot == destinationFolder)
    {
        destinationRecoveryTask.abandon();
        destinationRecoveryRoot.clear();
        destinationRecoveryStatus = RecoveryScanStatus::NotStarted;
    }

    // Replace a possibly stuck old-volume scan with a fresh shared state. The
    // old worker owns its old state until its syscall returns, so it cannot
    // publish against this destination or hold the new one behind its gate.
    startDestinationRecoveryScan();

    // §6.4: selecting a volume is a fresh claim about what is mounted at this
    // path now. A different card can later reuse the same /Volumes name, so a
    // throughput pass cached for the previous occupant must not arm it. A
    // worker already stuck on this exact path is abandoned, never joined.
    preflightResults.erase (destinationFolder);
    preflightVerdictAtMs.erase (destinationFolder);
    if (preflightTaskDestination == destinationFolder)
    {
        preflightTask.abandon();
        preflightTaskDestination.clear();
    }

    beginPreflightForDestination();

    // §10.4. There is no query API for this on macOS; the only truthful answer
    // comes from trying, so it is asked here -- once, when the location
    // changes -- rather than anywhere near arming or the audio path.
    //
    // Bounded, and short: this runs on the message thread at every location
    // click and right after Stop, and a card that has stopped answering held
    // it inside fopen() indefinitely -- a frozen window and a quit that never
    // finished. The answer is only guidance (the drive check keeps Record
    // closed on its own worker), and a real privacy refusal returns in
    // milliseconds. A probe that runs out of time owns only its copy of the
    // path and finishes, or not, on its own.
    {
        const auto probeTarget = destinationFolder;
        destinationWritePermission = runWithDeadline<PermissionState> (
                [probeTarget] { return queryVolumeWritePermission (probeTarget); },
                std::chrono::milliseconds (250))
            .value_or (PermissionState::NotApplicable);
    }
    journalledPermissionProblems = false;

    saveSettings();
}

juce::String Application::createMirrorFolder (const juce::String& sessionFolderName) const
{
    // §6.3: the mirror lives on the internal drive, which is the whole point --
    // a card failure must not take both copies with it.
    const auto mirrorRoot = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("RECORDINGS-MIRROR");
    auto root = mirrorRoot.getChildFile (sessionFolderName);

    // §6.2 never overwrites, and that holds for the copy too. Collisions were
    // resolved against the card only, so a take restarted on a new card with
    // the same name -- the card-removal flow exactly -- reopened the mirror
    // folder it had just promised was safe and truncated every file in it.
    for (int attempt = 2; root.exists() && attempt < 1000; ++attempt)
        root = mirrorRoot.getChildFile (sessionFolderName + "_" + juce::String (attempt));

    if (root.exists() || ! root.createDirectory().wasOk())
    {
        // The caller degrades to card-only, which is right (§6.3). What it
        // could not do was tell anyone, because an empty string is not a
        // reason. pollStatusAdvice catches the resulting state and says so;
        // this puts the cause in the record.
        noteActivity (ActivityLevel::Failed, "Local backup",
                      "Couldn't make the backup folder at " + root.getFullPathName()
                      + ". This take is going to the card only.");
        return {};
    }

    return root.getFullPathName();
}

void Application::refreshMonitoringLatency()
{
    if (capture == nullptr)
        return;

    measuredLatencyMs = capture->getMonitoringLatencyMs();
    slowestMicLatencyMs = std::max (measuredLatencyMs, capture->getSlowestMonitoringLatencyMs());
}

void Application::noteSlowMicrophone()
{
    // The headline figure is the quickest microphone's own path, since no
    // channel waits for any other (§5.4). That is what most of the rig
    // hears -- but one interface that another app holds at a 2048-frame
    // buffer is 40 ms late in the headphones, in its own channel, while the
    // figure reads 5 ms. Past the ceiling while the rest are within it, or
    // several milliseconds behind them, it is said once, naming the
    // microphone. Not for being a block behind a rig the buffer ladder has
    // taken past the ceiling as a whole: the ladder's notice has said that.
    if (capture == nullptr || ! capture->isMonitoring())
        return;

    std::string slowId;

    if (SlowMicrophoneNotice::isFarBehind (measuredLatencyMs, slowestMicLatencyMs))
        slowId = capture->getSlowestMonitoringDeviceId();

    if (slowId == reportedSlowMicId)
        return;

    const bool wasStanding = ! reportedSlowMicId.empty();
    reportedSlowMicId = slowId;

    if (slowId.empty())
    {
        if (wasStanding)
            noteActivity (ActivityLevel::Recovered, "Monitoring",
                          "No microphone is far behind the others in the headphones any more (about "
                              + juce::String (juce::roundToInt (slowestMicLatencyMs)) + " ms for the slowest).");
        return;
    }

    juce::String name ("One microphone");

    for (const auto& d : deviceManager.getDevices())
        if (d.identity.key() == slowId)
        {
            name = juce::String (d.displayName);
            break;
        }

    noteActivity (ActivityLevel::Warning, "Monitoring",
                  name + " reaches the headphones about " + juce::String (juce::roundToInt (slowestMicLatencyMs))
                      + " ms late: its interface runs at a larger buffer than the others (another app may "
                        "have set it) or reports a longer delay. The other microphones are not held back for "
                        "it, and its recording is lined up with theirs. If another audio app is open, "
                        "closing it and then unplugging and replugging this microphone may bring it back.");
}

void Application::recordTakeAlignment()
{
    if (capture == nullptr)
    {
        takeMeasuredLatencyMs = measuredLatencyMs;
        takeSlowestMicLatencyMs = slowestMicLatencyMs;
        takeStemsAligned = false;
        return;
    }

    takeAlignedInputLatencyFrames = capture->getAlignedInputLatencyFrames();

    // What the headphones cost now that the devices have said what IO size
    // they really run at: the quickest microphone's own path, which no other
    // device's latency or block is added to (the stems are lined up on the
    // writer instead). Fixed here for the take's record, so a stop-time
    // session.json written after the engine has moved on still describes it,
    // with the slowest microphone's own path beside it.
    if (capture->isMonitoring())
        refreshMonitoringLatency();

    takeMeasuredLatencyMs = measuredLatencyMs;
    takeSlowestMicLatencyMs = slowestMicLatencyMs;

    // Whether the writer lined the stems up exactly; with each device's
    // offset below, whatever it could not do can be done in an editor.
    takeStemsAligned = capture->areStemsAligned();

    for (auto& record : takeDevices)
    {
        const int latency = capture->getDeviceInputLatencyFrames (record.usbId);
        record.inputLatencyFrames = latency;

        // Kept only where the driver's own figure was not believed, beside
        // the bound the stems were lined up by.
        record.reportedInputLatencyFrames.reset();
        if (const auto reported = capture->getDeviceReportedInputLatencyFrames (record.usbId);
            latency >= 0 && reported.has_value() && *reported != latency)
            record.reportedInputLatencyFrames = *reported;

        record.ioBlockFrames = latency >= 0 ? capture->getDeviceIoBlockFrames (record.usbId) : 0;
        record.alignmentDelayFrames = latency >= 0 ? capture->getDeviceAlignmentDelayFrames (record.usbId) : 0;
        record.alignmentStartFrames = latency >= 0 ? capture->getDeviceAlignmentStartFrames (record.usbId) : 0;
        record.alignmentSilenceFrames = latency >= 0 ? capture->getDeviceAlignmentSilenceFramesThisTake (record.usbId) : 0;
        record.alignmentDroppedFrames = latency >= 0 ? capture->getDeviceAlignmentDroppedFramesThisTake (record.usbId) : 0;
        record.ioShiftFrames = latency >= 0 ? capture->getDeviceIoShiftFramesThisTake (record.usbId) : 0;
    }
}

TakeFigures Application::liveTakeFigures (bool sessionHasStopped) const
{
    TakeFigures f;
    f.elapsedSeconds = getElapsedRecordingSeconds();
    f.stopTimestampIso = sessionHasStopped
                             ? juce::Time::getCurrentTime().toISO8601 (true).toStdString()
                             : std::string();

    // The size the take's streams actually ran at. The ladder can have stepped
    // on during the take; that step applies to the next one.
    f.bufferSizeSamples = capture != nullptr ? capture->getBufferSizeSamples() : bufferLadder.getCurrentSize();

    if (capture != nullptr)
    {
        // Each loaded once. Read twice, the number reported can differ from
        // the one that passed the test.
        f.framesDropped = capture->getFramesDropped();
        f.overrunSamples = capture->getOverrunSamplesThisTake();
        f.underrunSamples = capture->getUnderrunSamplesThisTake();
        f.framesMissedByLayout = capture->getFramesMissedByLayout();
    }

    // Measured from the start of THIS take. The backend's counter runs for as
    // long as its streams do, which is across takes. A total below the
    // baseline means the streams were rebuilt mid-take and the count
    // restarted; everything since is then this take's.
    if (audioBackend != nullptr)
    {
        const auto total = audioBackend->getFramesDroppedByBackend();
        f.backendFramesDropped = total >= backendDropsAtTakeStart ? total - backendDropsAtTakeStart : total;
    }

    return f;
}

std::string Application::buildSessionMetadataJson (bool sessionHasStopped)
{
    // Every figure below that describes the moment the take ended comes from
    // here -- see TakeStopSnapshot.
    const auto figures = takeStopSnapshot.resolve (sessionHasStopped, liveTakeFigures (sessionHasStopped));

    SessionMetadata meta;
    meta.appVersion = appVersionString().toStdString();
    meta.startTimestampIso = sessionStartIso.toStdString();
    meta.stopTimestampIso = figures.stopTimestampIso;
    // The take's own format (see toggleRecording), not the live setting,
    // which Settings can change mid-take for the next take.
    meta.sampleRate = takeSampleRate;
    meta.bitDepth = takeBitDepth;
    // The size the take's streams actually ran at. The ladder can have stepped
    // on during the take; that step applies to the next one.
    meta.bufferSizeSamples = figures.bufferSizeSamples;
    meta.measuredLatencyMs = takeMeasuredLatencyMs;
    meta.slowestMicLatencyMs = takeSlowestMicLatencyMs;
    meta.alignedInputLatencyFrames = takeAlignedInputLatencyFrames;
    meta.stemsAligned = takeStemsAligned;

    // The take's roster, fixed when it started, not whoever is ticked and
    // plugged in now. A mic unplugged mid-take still has its stem in the
    // folder, and one plugged in mid-take has none. Trim and drift still come
    // from the live device while it is there, so a trim moved mid-take is
    // recorded as it ends.
    for (auto record : takeDevices)
    {
        for (const auto& d : deviceManager.getDevices())
        {
            if (d.identity.key() != record.usbId)
                continue;

            if (const auto persisted = portIdentityStore.get (d.identity))
                recordTrims (record, d, *persisted);

            // §3.2: drift is only claimed once 60 seconds of measurement exist.
            if (d.hasDriftMeasurement)
                meta.driftLog.push_back ({ figures.elapsedSeconds, d.identity.key(), d.measuredDriftPpm });

            break;
        }

        meta.devices.push_back (std::move (record));
    }

    // §5.4 requires every buffer step logged -- this take's steps, timed from
    // its audio t=0 like the drift entries. The ladder's log spans the whole
    // launch on the uptime clock, and copying it whole put earlier takes' and
    // idle steps into every session.json at times no editor could place.
    for (const auto& change : selectTakeBufferChanges (bufferLadder.getChangeLog(),
                                                       takeBufferChangeStart,
                                                       recordingStartMs / 1000.0))
        meta.bufferChanges.push_back ({ change.atSeconds, change.fromSamples, change.toSamples });

    // Only camera writers which actually started belong in session.json. The
    // watchdog separately keeps the complete intended roster so it can report
    // a missing capture card, but inventing that card's movie filename here
    // would make an editor look for a file which never existed. At Stop, only
    // the movies that finished; before it -- the record a crash leaves -- the
    // ones being written as well, which are in the folder either way.
    for (const auto& video : sessionHasStopped ? cameraController.getTakeVideoRecords()
                                               : cameraController.getTakeVideoRecordsSoFar())
        meta.videos.push_back ({ video.displayName, video.fileName, false });

    // A take recorded on this computer has no backup by design, not because
    // one was switched off or cut short.
    meta.mirrorEnabled = mirrorPolicy.getState() != MirrorState::DisabledByUser
                      && mirrorPolicy.getState() != MirrorState::NotNeededTakeOnComputer;
    meta.mirrorActive = sessionHasStopped && mirrorActiveAtStop >= 0
                            ? mirrorActiveAtStop == 1
                            : (capture != nullptr && capture->isMirroring());
    meta.mirrorPath = currentMirrorFolder.toStdString();

    // §6.5's mid-recording row: every unplug and reconnection during this take.
    for (const auto& entry : midTakeDropouts)
        meta.dropouts.push_back (entry);

    // §6.5: "log the exact sample position of degradation." Recorded as a
    // dropout entry rather than a new field, because that is what it is: the
    // moment the stems stopped receiving audio they should have had.
    if (const auto degradedAt = capacityMonitor.getDegradationSamplePosition(); degradedAt >= 0)
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Fell back to writing the mix only at sample "
                                       + std::to_string (degradedAt) });

    // §6.3: a mirror that stopped mid-take must be visible in the record --
    // otherwise the copy looks complete and is not.
    if (mirrorPolicy.wasStoppedForSpace())
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Local backup copy stopped: the internal drive ran low on space." });

    // §6.3 again, for the other way a mirror can stop. Without this line a
    // backup copy truncated by a failed drive looks exactly like a complete
    // one -- the file is simply shorter, and nothing says why.
    if (mirrorPolicy.wasStoppedForWriteFailure())
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   mirrorMissingReported
                                       ? std::string ("Local backup copy was never made: its folder or "
                                                      "files could not be opened. There is no second "
                                                      "copy of this take.")
                                       : std::string ("Local backup copy stopped: that drive stopped "
                                                      "accepting writes. The copy is incomplete from "
                                                      "this point.") });

    // Not conditional on capture: at Stop these are the snapshot's, which
    // outlives a coordinator rebuilt while a camera finished its movie.
    if (figures.framesDropped > 0)
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Dropped " + std::to_string (figures.framesDropped)
                                       + " frames: the drive could not keep up." });

    // The other end of the same loss. The count above is what the writer could
    // not put on disk; this is what never reached it -- audio the device handed
    // over and the backend could not carry.
    // §0.1: audio this app received and threw away because the ring was full.
    // The take's record carried the writer's drops, the layout's and the
    // backend's, and not this one -- so the one loss the app inflicts on itself
    // was the one the record did not mention.
    if (figures.overrunSamples > 0)
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Dropped " + std::to_string (figures.overrunSamples)
                                       + " samples: audio arrived faster than it could be "
                                         "taken away, so the buffer overflowed." });

    // §0.1: the pull came and the microphone's block had not, so its stem got
    // silence. The stream has counted this since the first day; nothing wrote
    // it down. A take whose microphone ran dry every other block -- a machine
    // whose scheduling is wider than the cushion -- was recorded as lossless,
    // with half its blocks holding a step down to zero.
    if (figures.underrunSamples > 0)
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Dropped " + std::to_string (figures.underrunSamples)
                                       + " samples: a microphone's audio did not arrive in time, "
                                         "so silence was written in its place." });

    // §0.1: audio the device delivered that did not fit the take's layout, so
    // a channel wrote silence instead. Recorded beside the other two losses.
    if (figures.framesMissedByLayout > 0)
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Dropped " + std::to_string (figures.framesMissedByLayout)
                                       + " frames that didn't fit this take's channel layout: a "
                                         "microphone delivered a different number of channels "
                                         "than it was opened with." });

    // Measured from the start of THIS take. The backend's counter runs for as
    // long as its streams do, which is across takes, so writing it raw put
    // take one's losses into take three's record -- beside a card-side figure
    // on a completely different clock. See liveTakeFigures.
    if (figures.backendFramesDropped > 0)
        meta.dropouts.push_back ({ figures.elapsedSeconds, std::string(),
                                   "Dropped " + std::to_string (figures.backendFramesDropped)
                                       + " frames before recording: the sound hardware delivered "
                                         "more audio than it could hand over." });

    return meta.toJsonString();
}

void Application::writeSessionMetadata (bool sessionHasStopped)
{
    if (currentSessionFolder.isEmpty())
        return;

    // The Stop boundary for the mid-take refreshes: none of them may land
    // after the record written here, or a finished take's session.json would
    // go back to having no stop time and be offered as interrupted.
    if (sessionHasStopped)
        retireSessionRecordRefreshes();

    // Written to the card copy and the mirror alike, so either one stands alone.
    const auto json = buildSessionMetadataJson (sessionHasStopped);

    // The result is checked. session.json is the durable record of everything
    // above -- every dropout, every buffer change, why the backup stopped --
    // and it was written with the return value discarded, so the one file that
    // explains a difficult take could fail to appear and nothing would say so.
    // Not said once the card has stopped answering: "the audio itself is
    // saved" is exactly what nobody can know then, and the card's own message
    // has already told the user what happened.
    if (! writeTakeText (juce::File (currentSessionFolder).getChildFile ("session.json"), juce::String (json))
        && ! takeCardUnresponsive)
        noteActivity (ActivityLevel::Warning, "Recording",
                      "Couldn't write the details file for this take. The audio itself is saved.");

    // The mirror's copy is checked too. §6.3 makes each copy stand alone, and a
    // backup whose record failed to write is a folder of audio with no account
    // of how the take went -- which is the half of the pair the user reaches
    // for precisely when the card's copy is the one that went wrong.
    if (currentMirrorFolder.isNotEmpty())
    {
        const auto mirrorRecord = juce::File (currentMirrorFolder).getChildFile ("session.json");

        // At Stop, through the refreshes' own writer, so one still in flight
        // to a slow backup drive can never be put in place over this record:
        // refused at its rename if it gets there later, and waited for on the
        // writer's worker -- not here -- if it is inside its rename now. The
        // same deadline as any other write here; past it this record still
        // lands, and still lands last.
        const bool mirrorWritten =
            sessionHasStopped && sessionRecordMirror != nullptr
                ? sessionRecordMirror->writeLast (mirrorRecord.getFullPathName().toStdString(), json,
                                                  kRemovableVolumeDeadline).value_or (false)
                : writeTakeText (mirrorRecord, juce::String (json));

        if (! mirrorWritten)
            noteActivity (ActivityLevel::Warning, "Local backup",
                          "Couldn't write the details file into the backup copy. The backed-up audio "
                          "itself is there.");
    }

    if (sessionHasStopped)
        sessionRecordMirror.reset();

    writeActivityLog (juce::File (currentSessionFolder));

    if (currentMirrorFolder.isNotEmpty())
        writeActivityLog (juce::File (currentMirrorFolder));
}

std::string Application::sessionRecordEventSignature() const
{
    // What a crash would otherwise take with it: each of these changes the
    // record in a way someone reading it afterwards needs, and none of them
    // changes often. The figures that move all the time -- the length, drift,
    // how much was dropped -- are carried by the periodic refresh, with only
    // the first loss counted here: the one that makes the take a lossy one.
    const auto figures = liveTakeFigures (false);
    const bool lossCounted = figures.framesDropped > 0 || figures.overrunSamples > 0
                          || figures.underrunSamples > 0 || figures.framesMissedByLayout > 0
                          || figures.backendFramesDropped > 0;

    auto signature = std::to_string (midTakeDropouts.size())
                   + "|" + std::to_string (cameraController.getTakeVideoRecordsSoFar().size())
                   + "|" + (capture != nullptr && capture->isMirroring() ? "1" : "0")
                   + "|" + (mirrorPolicy.wasStoppedForSpace() ? "1" : "0")
                   + "|" + (mirrorPolicy.wasStoppedForWriteFailure() ? "1" : "0")
                   + "|" + (capacityMonitor.getDegradationSamplePosition() >= 0 ? "1" : "0")
                   + "|" + std::to_string (bufferLadder.getChangeLog().size())
                   + "|" + (lossCounted ? "1" : "0");

    // How the stems are being lined up: a device that has only now said its
    // input latency, an IO block that grew and moved every other stem, an
    // offset that had to be clamped. Each changes the offsets an editor needs
    // to put right whatever the writer could not, and they change a handful
    // of times a take -- so a crash just after one still leaves the offsets
    // the files were written with.
    if (capture != nullptr)
    {
        signature += capture->areStemsAligned() ? "|A" : "|a";

        for (const auto& record : takeDevices)
            signature += "|" + std::to_string (capture->getDeviceInputLatencyFrames (record.usbId))
                       + "," + std::to_string (capture->getDeviceIoBlockFrames (record.usbId))
                       + "," + std::to_string (capture->getDeviceAlignmentStartFrames (record.usbId))
                       + "," + std::to_string (capture->getDeviceAlignmentDelayFrames (record.usbId))
                       + "," + std::to_string (capture->getDeviceAlignmentSilenceFramesThisTake (record.usbId))
                       + "," + std::to_string (capture->getDeviceAlignmentDroppedFramesThisTake (record.usbId))
                       + "," + std::to_string (capture->getDeviceIoShiftFramesThisTake (record.usbId));
    }

    return signature;
}

void Application::refreshSessionRecordIfDue()
{
    if (! isRecording() || currentSessionFolder.isEmpty() || sessionRecordCard == nullptr)
        return;

    const auto nowSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    auto signature = sessionRecordEventSignature();

    if (! sessionRecordSchedule.isDue (nowSeconds, signature))
        return;

    sessionRecordSchedule.written (nowSeconds, std::move (signature));

    // Each device's offsets, the silence and cuts the writer has made since,
    // and whether the stems are still lined up exactly, as they stand now:
    // copied at the take's start and its Stop only, a crash in between left
    // the start's figures beside the latest of everything else.
    recordTakeAlignment();

    // Built here, written elsewhere: the message thread assembles a few
    // hundred bytes from state it already owns and hands them over. The write
    // is the same checked temp-then-rename every record uses, on a worker
    // that may wait on the card for as long as the card likes -- and a card
    // already known to have stopped answering is not asked again.
    const auto json = buildSessionMetadataJson (false);

    if (! takeCardUnresponsive)
        sessionRecordCard->submit (juce::File (currentSessionFolder).getChildFile ("session.json")
                                       .getFullPathName().toStdString(), json);

    if (currentMirrorFolder.isNotEmpty() && sessionRecordMirror != nullptr)
        sessionRecordMirror->submit (juce::File (currentMirrorFolder).getChildFile ("session.json")
                                         .getFullPathName().toStdString(), json);
}

void Application::startSessionRecordRefreshes()
{
    // A free function and two strings: the worker never reaches back into
    // Application, so one still stuck on a dead card when the app quits holds
    // nothing that is going away.
    // The rename goes through the writer's commit, which refuses a refresh
    // that reaches it after Stop -- however long the drive held it.
    const auto write = [] (const std::string& path, const std::string& text,
                           const BackgroundRecordWriter::Commit& commit)
    {
        return replaceWithTextChecked (juce::File (juce::String::fromUTF8 (path.data(), (int) path.size())),
                                       juce::String::fromUTF8 (text.data(), (int) text.size()),
                                       commit);
    };

    sessionRecordCard = std::make_unique<BackgroundRecordWriter> (write);
    sessionRecordMirror = std::make_unique<BackgroundRecordWriter> (write);

    // What writeSessionMetadata (false) has just put on disk, so the first
    // refresh comes with the first change -- the microphones that would not
    // open, a camera's start confirmed -- or in thirty seconds.
    sessionRecordSchedule.written (juce::Time::getMillisecondCounterHiRes() / 1000.0,
                                   sessionRecordEventSignature());
}

void Application::retireSessionRecordRefreshes()
{
    sessionRecordSchedule.stop();

    // No refresh may land after the stop-time record, or a finished take's
    // session.json goes back to having no stop time and is offered as
    // interrupted at the next launch. A refresh still writing its temporary
    // copy is refused at its rename; one inside the rename on the card is
    // waited for within the same deadline as every other card step, and a card
    // that does not answer in it is treated as every other step treats it:
    // gone, for the rest of the take, so its stop-time write is skipped and
    // the steps after this one do not wait again. A card already known to be
    // gone is not waited on at all.
    if (sessionRecordCard != nullptr
        && ! sessionRecordCard->retire (takeCardUnresponsive ? std::chrono::milliseconds (0)
                                                             : kRemovableVolumeDeadline))
        takeCardUnresponsive = true;

    sessionRecordCard.reset();

    // The backup's refreshes are retired by its stop-time record itself
    // (writeSessionMetadata), written through the same writer: the backup is
    // on this computer and gets that record whatever its drive is doing, so it
    // is ordered after any refresh still in flight rather than skipped.
}

void Application::followRenamedTakeFolder()
{
    if (capture == nullptr || currentSessionFolder.isEmpty())
        return;

    const auto live = juce::String (capture->getLiveSessionFolder());

    if (live.isEmpty() || live == currentSessionFolder)
        return;

    currentSessionFolder = live;

    noteActivity (ActivityLevel::Warning, "Recording",
                  "This take's folder was renamed or moved. It is still recording, into "
                  + live + ".");
}

std::shared_ptr<TakeFolderTracker> Application::followTakeFolderAfterTheTake() const
{
    auto tracker = std::make_shared<TakeFolderTracker>();

    if (! tracker->adopt (takeFolderTracker.duplicateDescriptor(), currentSessionFolder.toStdString()))
        return nullptr;

    return tracker;
}

void Application::followTakeFolderMovedAfterAudioStopped()
{
    // A card that has stopped answering is not asked anything more.
    if (currentSessionFolder.isEmpty() || takeCardUnresponsive || ! takeFolderTracker.isOpen())
        return;

    const auto now = juce::String (takeFolderTracker.resolve());

    if (now.isEmpty() || now == currentSessionFolder)
        return;

    currentSessionFolder = now;

    noteActivity (ActivityLevel::Warning, "Recording",
                  "This take's folder was renamed or moved while it was being finished. Its files "
                  "are in " + now + ".");
}

bool Application::isOnCard (const juce::File& file) const
{
    // Path arithmetic only: asking the filesystem would be the very call that
    // hangs on a dead card. A take recorded on this computer for a card has
    // nothing on the card: a "card" that stopped answering is no reason to
    // skip its stop-time record, which its copy to the card waits for.
    return cardcopy::isOnTheSaveLocationWhileRecording (file.getFullPathName().toStdString(), currentTakeRoute,
                                                        currentMirrorFolder.isEmpty() ? std::string()
                                                            : juce::File (currentMirrorFolder).getFullPathName().toStdString());
}

bool Application::writeTakeText (const juce::File& file, const juce::String& text) const
{
    // The backup copy lives on this computer, not the card, so a card that has
    // stopped answering is no reason to skip it.
    const bool onCard = isOnCard (file);

    // Each card step is bounded, but they run one after another. Once one has
    // timed out, waiting the full deadline again on every later step turned a
    // dead card at Stop into half a minute of frozen window. The card has
    // already said it is gone; believe it for the rest of the take.
    if (onCard && takeCardUnresponsive)
        return false;

    const auto written = replaceWithTextWithin (file, text);

    if (! written.has_value())
    {
        if (onCard)
            takeCardUnresponsive = true;

        return false;
    }

    return *written;
}

void Application::writeActivityLog (const juce::File& folder) const
{
    // Plain text beside the take, because this one is for the person and not
    // for a parser: it is what they show someone else when a take went wrong.
    // A take that carries its own history can be explained months later; one
    // that does not is a folder of audio and a memory of something going amber.
    juce::String text;
    text << "SobStage -- what happened during this session" << juce::newLine << juce::newLine;

    // Said, rather than left as a log that mysteriously starts part way through.
    if (const auto dropped = activity.getDroppedCount(); dropped > 0)
        text << "(" << juce::String (static_cast<int> (dropped))
             << " earlier entries are not listed -- this log keeps the most recent "
             << juce::String (static_cast<int> (ActivityJournal::kMaxEntries)) << ".)"
             << juce::newLine << juce::newLine;

    // Oldest first: a log is read forwards.
    auto lines = getRecentActivityLines (static_cast<int> (ActivityJournal::kMaxEntries));

    for (int i = lines.size(); --i >= 0;)
        text << lines[i] << juce::newLine;

    // Checked like everything else here. A log that failed to write is exactly
    // the sort of thing this file exists to stop happening quietly.
    if (! writeTakeText (folder.getChildFile ("activity.log"), text)
        && ! (takeCardUnresponsive && isOnCard (folder)))
        noteActivity (ActivityLevel::Warning, "Recording",
                      "Couldn't write the activity log into " + folder.getFileName()
                      + ". The audio itself is saved.");
}

double Application::activityClockSeconds() const
{
    if (appStartMs <= 0.0)
        return 0.0;

    return (juce::Time::getMillisecondCounterHiRes() - appStartMs) / 1000.0;
}

void Application::announceArrivalsAndDepartures (const std::map<std::string, std::string>& current,
                                                 std::map<std::string, std::string>& known,
                                                 bool& seeded,
                                                 const std::set<std::string>& saidElsewhere) const
{
    // The first enumeration is the rig as the user set it up, not a series of
    // arrivals. "N microphones are live" covers the launch.
    if (! seeded)
    {
        seeded = true;
        known = current;
        return;
    }

    for (const auto& [key, name] : current)
        if (known.find (key) == known.end())
            noteActivity (ActivityLevel::Started, juce::String (name),
                          juce::String (name) + " is connected.", true);

    for (const auto& [key, name] : known)
        if (current.find (key) == current.end() && saidElsewhere.count (key) == 0)
            noteActivity (ActivityLevel::Warning, juce::String (name),
                          juce::String (name) + " was unplugged.", true);

    known = current;
}

void Application::announceDeviceChanges (const std::vector<MicDeviceState>& seen) const
{
    std::map<std::string, std::string> current;

    for (const auto& d : seen)
        current[d.identity.key()] = d.displayName;

    // Whether a take is running decides only ONE thing here: a microphone that
    // is part of the take gets §6.5's own sentence when it goes, which says
    // what happens to its track, so saying "unplugged" beside it would be the
    // same news twice. Everything else is announced either way -- a
    // microphone arriving or leaving between takes is exactly as much a fact
    // about the rig as one arriving or leaving during one.
    std::set<std::string> takeChannels;

    if (capture != nullptr && capture->isRecording())
        for (const auto& ch : capture->getChannels())
            takeChannels.insert (ch.deviceId);

    announceArrivalsAndDepartures (current, knownDeviceNames, haveEnumeratedDevicesOnce, takeChannels);
}

void Application::announceOutputChanges (const std::map<std::string, std::string>& current) const
{
    // A microphone with a headphone jack (a Yeti, a capture dongle's audio) is
    // listed as an output too. Its arrival is already announced as a
    // microphone; saying it again here showed as "is connected. (x2)".
    std::set<std::string> micNames;
    for (const auto& [key, name] : knownDeviceNames)
        micNames.insert (name);

    std::map<std::string, std::string> outputsOnly;
    for (const auto& [key, name] : current)
        if (micNames.count (name) == 0)
            outputsOnly.emplace (key, name);

    announceArrivalsAndDepartures (outputsOnly, knownOutputNames, haveAnnouncedOutputsOnce, {});
}

void Application::announceCameraChanges()
{
    // Choices remembered under the old name-based ids were just carried over
    // to cameras' own per-device ids: save them under those now, so the next
    // launch reads them directly rather than matching names again.
    if (! applyingRememberedSettings && cameraController.getSelection().takeAdoptedLegacyChoices())
        saveSettings();

    std::map<std::string, std::string> current;

    for (const auto& cam : cameraController.getSelection().getAvailableCameras())
        current[cam.id] = cam.displayName;

    // A camera that began the running take gets TakeWatchdog's own sentence
    // when it goes, so it is not also announced here. Use the frozen take
    // roster rather than the live OS list: the lost camera is absent from that
    // list at exactly the moment duplicate suppression matters.
    std::set<std::string> saidByTheWatchdog;

    if (isRecording())
        for (const auto& cam : cameraController.getTakeCameraStates())
            saidByTheWatchdog.insert (cam.id);

    announceArrivalsAndDepartures (current, knownCameraNames, haveAnnouncedCamerasOnce,
                                   saidByTheWatchdog);
}

void Application::noteActivity (ActivityLevel level, const juce::String& subject,
                                const juce::String& message, bool onTheLine) const
{
    // A repeat that only bumps a count is not news for the two append-only
    // places below. Without this, one failure repeating twice a second wrote
    // its sentence to log.txt every time -- 207 identical lines in 40 seconds
    // when a destination went away -- and rewrote the take's log just as often.
    if (! activity.note (activityClockSeconds(), level, subject.toStdString(), message.toStdString(),
                         onTheLine))
        return;

    // Into the app's own log as well, which until now held one line per launch
    // -- "Starting up." -- and nothing else, for the whole life of the app. It
    // is the file the diagnostics bundle ships to whoever is helping, and the
    // only account that survives when the take folder does not: pull the card
    // mid-take and the take's own log goes with it, leaving the one file that
    // was still readable saying nothing at all.
    juce::Logger::writeToLog (juce::Time::getCurrentTime().toString (true, true)
                              + "  [" + juce::String (ActivityJournal::levelName (level)) + "] "
                              + subject + ": " + message);

    // Straight to disk while a take is running. The journal lives in memory,
    // and the only writes were at the start of the take -- before the "started"
    // note even existed -- and at the stop. So a take that ended in a crash or
    // a power cut, the take this log exists for, was recovered next to a log
    // that said nothing had happened: not that recording started, not the
    // drive-space warning that came ten minutes before the end.
    flushActivityLogToTake();
}

void Application::flushActivityLogToTake() const
{
    // The message thread owns the take's folder paths, while some device
    // notifications can record activity from OS threads. Reading those
    // juce::Strings there, and writing the same two files from two threads at
    // once, is a race; the journal itself is locked, so the entry is already
    // safely recorded and only the file write is deferred.
    if (! juce::MessageManager::existsAndIsCurrentThread())
    {
        const auto alive = getAliveToken();

        juce::MessageManager::callAsync ([this, alive]
        {
            if (alive.lock() != nullptr)
                flushActivityLogToTake();
        });

        return;
    }

    // writeActivityLog reports its own failure through noteActivity, which
    // lands back here. Once is a report; twice is a loop.
    if (writingActivityLog || currentSessionFolder.isEmpty())
        return;

    writingActivityLog = true;
    writeActivityLog (juce::File (currentSessionFolder));

    if (currentMirrorFolder.isNotEmpty())
        writeActivityLog (juce::File (currentMirrorFolder));

    writingActivityLog = false;
}

juce::StringArray Application::getRecentActivityLines (int limit) const
{
    juce::StringArray lines;

    for (const auto& e : activity.getEntries())
    {
        if (lines.size() >= limit)
            break;

        // Minutes and seconds on the app's clock. A user comparing this against
        // a take needs an ordering and a rough distance, not a wall clock.
        const int totalSeconds = static_cast<int> (e.atSeconds);
        auto stamp = juce::String::formatted ("%d:%02d", totalSeconds / 60, totalSeconds % 60);

        // Most messages name their device so they read alone on the main
        // screen; don't say the name twice here ("Yeti -- Yeti is connected").
        const juce::String subject (e.subject), message (e.message);
        auto line = stamp + "  "
                  + (message.startsWith (subject + " ")
                         ? message
                         : subject + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + message);

        if (e.repeats > 1)
            line += " (" + juce::String (e.repeats) + " times)";

        lines.add (line);
    }

    return lines;
}

juce::String Application::pollStatusAdvice (double sinceLastCallSeconds)
{
    // First, before any stream failure this tick reads: a wake from sleep.
    // Every device drops as the computer sleeps and comes back as it wakes,
    // which is no sign of power (§14.2). A Mac says so itself; on Windows each
    // stream simply dies, and the drops read as "may need more power".
    if (const auto clocks = WakeDetector::read(); clocks.has_value() && wakeDetector.justWoke (*clocks))
        setupAdvisor.noteSystemWoke (juce::Time::getMillisecondCounterHiRes() / 1000.0);

    // A fresh two-channel device is monitored as two exact physical inputs
    // until its analyzer reaches a verdict. Applying that answer here keeps
    // persistence and stream rebuilding off the audio thread; the helper also
    // refuses to do either while a take is running.
    applyChannelLayoutDecisions();

    // §6.6: backstop for every way a take starts or ends (the record button,
    // a full disk, a pulled card): mirror the take state into the sleep
    // assertion. Idempotent, so this costs nothing on an unchanged tick.
    syncSleepInhibitor();
    syncAfterTakeHolds();

    // A save location that was not mounted when it was checked for an
    // interrupted take is checked again once it is.
    rescanDestinationIfItHasAppeared();

    // A different card put in at the save location's path -- or the card
    // taken out -- is checked from scratch rather than armed on the old one's
    // speed and format.
    recheckDestinationIfItsDriveChanged();

    // Before anything reads the take's files: a folder renamed in Finder
    // mid-take is followed here, well inside the growth check's six seconds.
    if (isRecording())
        followRenamedTakeFolder();

    // Every 30 s, and soon after anything worth recording happens.
    refreshSessionRecordIfDue();

    // The assertion above holds a plugged-in Mac up with its lid shut; on
    // battery nothing can, so the performer hears it while the lid is open.
    if (batteryRecordingNotice.tick (isRecording(), sinceLastCallSeconds, &SleepInhibitor::queryPowerSource))
        noteActivity (ActivityLevel::Warning, "Recording", BatteryRecordingNotice::kMessage, true);

    // Podcast-ready copies finished since the last tick. Reported here, on
    // the message thread, because the worker that made them may not touch the
    // journal -- and up here, above every early return, so a warning holding
    // the line cannot hold back the news that the copy was saved.
    for (const auto& exported : podcastExporter.collectFinished())
        noteActivity (exported.written ? ActivityLevel::Stopped : ActivityLevel::Warning,
                      "Podcast copy", exported.message, exported.written);

    // The combined videos, likewise, once their run has finished. Failures
    // have their own line further down; this is the news that the file the
    // user asked for is there, which used to go unsaid -- the saved-take card
    // is listed at Stop, minutes before the combine is done, so nothing ever
    // named the file. Once per run, naming every file it wrote.
    {
        const auto combine = takeCombiner.getStatus();

        if (! combine.running && combine.run != 0 && combine.run != announcedCombineRun)
        {
            announcedCombineRun = combine.run;

            if (! combine.written.isEmpty())
                noteActivity (ActivityLevel::Stopped, "Combined video",
                              (combine.written.size() == 1 ? "Combined video saved: "
                                                           : "Combined videos saved: ")
                                  + combine.written.joinIntoString (", ") + ".",
                              true);
        }
    }

    // A podcast copy or combined video made after a take was copied to the
    // card goes to the card too. Only what is new is written or read back.
    releaseFinishedFollowUps();

    tickCardCopies();

    // The microphone answer read at launch goes stale the moment the user
    // answers the first-run prompt or revokes access in System Settings. Kept
    // at launch-only, "Don't Allow" left Record enabled over microphones the
    // OS feeds silence. Re-read every couple of seconds; it never prompts.
    secondsSinceMicrophonePermissionCheck += sinceLastCallSeconds;
    if (secondsSinceMicrophonePermissionCheck >= 2.0 && refreshMicrophonePermission())
    {
        // Access just arrived: re-enumerate (a Mac lists no microphones while
        // access is missing) and reopen the streams once. Mid-take the reopen
        // is deferred to the stop, as for any other device change. The
        // permission was read a moment ago, so it is not read again.
        rescanDevices();
    }

    // Record says "The microphones aren't open: ..." and tells the user what
    // to fix -- the interface's rate, another app holding it, macOS still
    // finishing. Nothing reports that fix: a rate or hog-mode change is not a
    // device-list change, and a device that never opened has no stream to
    // listen on. So while NOTHING is open, try again, backing off. Never while
    // anything is open: the reopen closes every stream first, and on a rig
    // that is partly up that would cut the headphones on every attempt.
    {
        FailedOpenRetry::Situation situation;
        situation.idle = recordingEngine.getState() == RecordingState::Idle
                      && pendingStoppedTakeCompletion == nullptr
                      && ! cameraController.isFinalizingRecording();
        situation.includedMicCount = getIncludedMicCount();
        situation.monitoring = capture != nullptr && capture->isMonitoring();
        situation.permissionDenied = microphonePermission == PermissionState::Denied;

        // The full device-list pass rather than restartCapture(): it re-reads
        // each device's current rate and re-takes the vote, which is exactly
        // what a rate changed in Audio MIDI Setup needs.
        if (capture != nullptr && failedOpenRetry.tick (sinceLastCallSeconds, situation))
            onDeviceListChanged();
    }

    // Cleared first, not inside the tap block: a capacity or performance
    // warning returns early below, and a stale index would leave one meter
    // lit indefinitely.
    tappedChannel = -1;

    // §10.6: the blocking problems already gate the record button, but a
    // save-location denial does not block anything -- it just means the take
    // will fail later -- so it has to reach the user some other way. The
    // journal is that way, and it also puts the reason in the take's own log.
    if (! journalledPermissionProblems)
    {
        const auto permissionProblems =
            PermissionGuidance::evaluate (microphonePermission, destinationWritePermission,
                                          ! destinationFolder.empty(), microphoneRestricted);

        journalledPermissionProblems = true;

        for (const auto& problem : permissionProblems)
            noteActivity (problem.blocksRecording ? ActivityLevel::Failed : ActivityLevel::Warning,
                          problem.kind == PermissionKind::Microphone ? "Microphones"
                                                                     : "Save location",
                          juce::String (problem.message));
    }

    // §8.1: the detectors only see anything if the per-block peaks reach them,
    // so this is where the §10.5 advice actually gets its input.
    const int micCount = getIncludedMicCount();
    std::vector<float> peaksDb;
    peaksDb.reserve (static_cast<size_t> (micCount));

    for (int i = 0; i < micCount; ++i)
    {
        auto* meter = getChannelMetering (i);
        peaksDb.push_back (meter != nullptr ? static_cast<float> (meter->getPeakHoldDb())
                                            : static_cast<float> (Metering::kMinDb));
    }

    updateSetupAdvisorLevels (peaksDb, sinceLastCallSeconds);

    // §14.4, "the sleeper failure": several microphones in omni or stereo in one
    // room produce heavy bleed, unusable stems and comb filtering, and the
    // detector for it has never been fed -- so the one piece of advice that
    // names the fix ("turn its pattern knob to the single-heart setting") could
    // not fire. Correlation is a sample-level quantity, so it comes from the
    // capture path rather than from the per-channel peaks above.
    if (capture != nullptr)
        setupAdvisor.updatePolarPattern (capture->getPolarPairCorrelation(),
                                         capture->getPolarThirdChannelPeakDb(),
                                         sinceLastCallSeconds);

    // §3.2 / §3.3: the loop runs in the audio callback; reporting it does not.
    if (capture != nullptr)
    {
        capture->tickDriftReporting (sinceLastCallSeconds);
        driftMeasuredSeconds += sinceLastCallSeconds;

        // §5.4's headphone figures, kept current for the Advanced panel: each
        // microphone's own path is only known once every device has said
        // what IO size it really runs at, and can grow while it runs.
        if (capture->isMonitoring())
        {
            refreshMonitoringLatency();
            noteSlowMicrophone();
        }

        // Walked over capture's channels rather than the device list: the two
        // agree only until a microphone is unplugged mid-take, and after that
        // this loop was attributing each channel's drift to whichever device
        // had shifted into its position -- so the Advanced panel showed one
        // microphone's PPM under another's name, and §3.3's 100 PPM flag could
        // land on a device that was keeping perfect time.
        {
            // The measured figure, not the loop's state. The loop's own PPM
            // slews from zero, overshoots to its clamp draining
            // what it accumulated on the way, and is knocked sideways by
            // every lost block -- so read at sixty seconds it was a snapshot
            // of a transient, and every microphone in the rig was "measured"
            // at +200. Each stream now fits its device's delivered samples
            // against time and reports that; it is within a couple of PPM at
            // the minute whatever the loop is doing.
            const auto& channels = capture->getChannels();

            for (size_t i = 0; i < channels.size(); ++i)
            {
                const auto index = static_cast<int> (i);
                deviceManager.updateMeasuredDrift (channels[i].deviceId,
                                                   capture->getChannelMeasuredDriftPpm (index),
                                                   capture->hasChannelDriftMeasurement (index)
                                                       ? std::max (driftMeasuredSeconds,
                                                                   capture->getChannelMeasurementSeconds (index))
                                                       : 0.0);
            }
        }

        // §3.1: the master is re-picked as measurements arrive, so a rig that
        // started on enumeration order settles onto the steadiest clock.
        applyClockMaster();

        // §3.3: a device this far out is not drifting, it is failing.
        //
        // Recorded rather than returned. This used to return, which put it
        // ahead of the card-removal check below -- the branch that actually
        // stops the take -- so a card failing at the same moment as a drift
        // warning kept recording into a dead handle for another poll. The
        // journal carries it, and the advice line picks it up once nothing
        // more urgent is holding the line.
        const auto& driftChannels = capture->getChannels();

        for (int i = 0; i < micCount; ++i)
        {
            if (! capture->hasSustainedExcessDrift (i))
                continue;

            // Not about a microphone that has been unplugged. Its stream is
            // gone, so whatever the drift figure still says is about a device
            // that is not there -- and "try a different USB port" is advice
            // for a microphone the user has already pulled out. Seen for real:
            // a mic unplugged mid-take went on filing sync warnings under its
            // own name for the rest of the take.
            if (static_cast<size_t> (i) < driftChannels.size()
                && recordingEngine.isWritingSilence (driftChannels[static_cast<size_t> (i)].deviceId))
                continue;

            noteActivity (ActivityLevel::Warning, getMicDisplayName (i),
                          juce::String (getMicDisplayName (i))
                          + " can't keep steady time with the others. Try a different USB port.");
        }

        // §10.5: a channel that stays silent while the others are working is
        // the single most common failure on a live rig -- usually the mute
        // switch on the microphone itself -- and it reached the screen and
        // nowhere else. A take where someone's microphone was muted right
        // through it left nothing in the record to say so, which is the same
        // silence-nobody-warned-about this app exists to prevent. noteActivity
        // already collapses the repeat, so this can run on the timer beside the
        // drift check above.
        for (const auto& advice : getSetupAdvice())
        {
            // Two of the advisor's findings are about something being WRONG
            // with the rig for the whole take rather than about how to set it
            // up better, and both reached the screen and nowhere else. A take
            // recorded on an underpowered hub, or with someone's microphone
            // muted right through it, should carry the reason in its own
            // record; the rest of the advice is guidance and would only be
            // noise in a log.
            if (advice.issue != SetupIssue::SilentChannel
                && advice.issue != SetupIssue::BusPowerExhausted
                && advice.issue != SetupIssue::DeviceKeepsDropping)
                continue;

            const auto who = advice.channelIndex >= 0
                           ? juce::String (getMicDisplayName (advice.channelIndex))
                           : juce::String ("Microphones");

            noteActivity (ActivityLevel::Warning, who, juce::String (advice.message));
        }
    }

    // §6.5 "target card removed" outranks even running out of room: the drive
    // is not merely full, it is gone, and every further block would be written
    // into nothing. Checked before anything else because the take has to stop
    // now rather than at the end of this function.
    if (capture != nullptr && capture->hasCardWriteFailed()
        && isRecording())
    {
        // Built before the take is torn down, while the mirror's path and
        // whether it was still running are both still known.
        cardRemovalNotice = CardRemovalNotice::build (currentMirrorFolder.toStdString(),
                                                      capture->isMirroring());
        cardRemovalPending = true;


        // The ordinary stop path: it finalizes every open file (§6.5 "finalize
        // every open file"), writes session.json and raises the saved-take
        // notice, which is what shows the user whatever did survive.
        stopReason = "the card stopped accepting writes";
        toggleRecording();

        // The writer's own account when it has one -- a roll-over past 3.9 GB
        // that could not create the next file, or a drive that is simply full
        // -- is not "the card was removed", and sending someone to check a
        // cable for a card that is simply full wastes the one moment they are
        // still next to the rig.
        //
        // It now LEADS rather than being journalled underneath the removal
        // notice and then contradicted by it. The headline is the sentence the
        // user acts on, and it went on saying the drive had stopped responding
        // and to check it was plugged in properly even when the writer knew
        // perfectly well that the drive was merely full. The guard above was
        // written for exactly this and could not fire, because an ordinary
        // failed write left no account at all -- only the roll-over path set
        // one.
        auto headline = juce::String (cardRemovalNotice.message);

        // Safe to read AFTER the stop now: the coordinator keeps the take's
        // account past the pipeline's destruction, and reading it here rather
        // than before stop() also picks up a final close() that failed.
        if (const auto why = capture->getCardWriteProblem(); ! why.empty())
        {
            headline = juce::String (why);

            // §6.5: pointing at the surviving copy is the whole point of the
            // notice's second sentence, so it outlives the swap.
            if (cardRemovalNotice.aCompleteCopySurvives)
                headline += " A complete backup copy is safe on this computer: "
                          + juce::String (cardRemovalNotice.survivingFolder);
        }

        noteActivity (ActivityLevel::Failed, "Card", headline);
        return headline;
    }

    // §6.5: the drive is full, so the take stops -- here, before any of the
    // warning branches below.
    //
    // The stop used to sit at the bottom, beneath five branches that return as
    // soon as their condition holds and keep holding it: backend drops, layout
    // losses, monitor glitches, mirror failures, ring fill. A rig with any one
    // of those persisting therefore never reached the capacity check, and the
    // take was NOT stopped on a full drive -- which is precisely the failure
    // the "said AND done" fix was written to prevent. The card-removal stop was
    // already hoisted for the same reason; this one was left behind.
    //
    // Only the stop is hoisted, and it is decided from the raw remaining figure
    // rather than from CapacityMonitor -- because evaluateRemaining LATCHES.
    // Evaluating it here and reusing the answer below swapped one bug for its
    // mirror image: the latch would be spent on every poll, including the ones
    // where a branch further down returns first, so a ring sitting at 50% fill
    // -- which returns on every single poll -- would permanently swallow the
    // ten- and two-minute warnings. Silencing the two warnings whose whole job
    // is to give the user time to act, inside a change about not being silent.
    //
    // The stop does not need the latch: it needs to know whether the room ran
    // out, which getRemainingRecordingSeconds answers as often as it is asked.
    // The latching call stays at the bottom, where its answer is delivered.
    if (isRecording())
    {
        const auto remaining = getRemainingRecordingSeconds();

        // Zero means the volume was asked and has nothing left. Negative means
        // it could not be asked, which is not the same thing -- reading that as
        // empty would stop a take over a drive whose free space the OS declined
        // to report. Same test CapacityMonitor applies, on a figure that can
        // now actually take the value.
        const bool roomRanOut = remaining == 0.0;

        if (roomRanOut)
        {
            stopReason = "the drive ran out of room";
            toggleRecording();

            const auto line = juce::String ("The drive is full. Recording has stopped -- free some "
                                            "space or choose another drive.");

            noteActivity (ActivityLevel::Failed, "Drive", line);
            return line;
        }
    }

    // §6.5 / §6.3: the two write protections that must ACT on every poll --
    // the 90% mix-only fallback and the backup's 1 GB low-space stop. Their
    // lines keep their place further down; only the actions are hoisted.
    //
    // They used to run only where their lines are returned, beneath the stream
    // failure, backend drop, layout miss and output glitch branches -- each of
    // which returns whenever its counter rose since the last poll. A loaded
    // Mac raises a processor overload in nearly every poll, and a layout miss
    // rises on every callback once it starts, so neither action was reached:
    // the ring filled past 90% and overflowed into whole-block drops from
    // every stem AND the mix, and the backup kept eating the computer's disk
    // past the 1 GB stop. The same shape as the full-drive stop above.
    if (capture != nullptr)
    {
        WriteSafetyInputs safetyIn;
        safetyIn.recording = isRecording();
        safetyIn.alreadyMixOnly = capture->isMixOnly();
        safetyIn.ringFillFraction = capture->getRingFillFraction();
        safetyIn.mirroring = capture->isMirroring();
        safetyIn.mirrorFreeBytes = safetyIn.mirroring ? getMirrorFreeBytes() : -1;

        const auto safety = decideWriteSafetyActions (safetyIn, capacityMonitor, mirrorPolicy);

        if (safety.fallBackToMixOnly)
        {
            capture->fallBackToMixOnly();
            // §6.5: "log the exact sample position of degradation."
            capacityMonitor.noteDegradationAt (static_cast<long long> (capture->getFramesAccepted()));
            noteActivity (ActivityLevel::Failed, "Drive",
                          juce::String (CapacityMonitor::fillStatusLine (
                              WritePipelineState::DegradedToMixOnly, true)));
        }

        // §6.3: once the mirror stops it never restarts within the same take.
        if (safety.stopMirroring)
        {
            capture->stopMirroring();
            noteActivity (ActivityLevel::Warning, "Local backup", kMirrorLowSpaceLine);
            mirrorLowSpaceNoticePending = true;
        }
    }

    // §0.1: a stream that opened and has since stopped. Taken here, on the
    // message thread, from wherever the backend's worker thread left it.
    //
    // Every backend has a loop that gives up on a dead device and exits, and
    // none of them used to tell anyone: monitoring went quiet, or a
    // microphone's track went on being written as silence for the rest of the
    // take, and the only evidence was in the file afterwards.
    if (audioBackend != nullptr)
    {
        juce::String firstFailure;
        juce::String firstWarning;
        bool bufferLadderStepped = false;
        bool reopenStreams = false;
        bool outputRateChanged = false;

        // §5.3/§5.4: the headphone output held off the clock on the
        // platform's word of a rate change, since the last poll -- read
        // before takeStreamFailures() reads the rate and may let it go. The
        // software clock takes the rig over a moment after the output stops
        // pulling, and what the rings lose in that moment, here or just after
        // this poll, is the hand-over and not the computer missing a
        // deadline. Counted, it was a third of the way to blaming the
        // computer and raising the buffer.
        {
            const bool heldNow = capture != nullptr && capture->isOutputOffRate();

            if (heldNow && ! outputWasOffRate)
                outputRateHoldGraceUntilMs = juce::Time::getMillisecondCounterHiRes()
                                           + TakeWatchdog::kOutputSwitchSeconds * 1000.0;

            outputWasOffRate = heldNow;
        }

        // §2.2: a take stopped for a rate change is stopped once, after the
        // whole batch has been read. Stopped inside the loop, Stop reopened
        // the rig there and then, and the rest of the batch was applied to the
        // streams it had just opened at the right rate: on the combined
        // device one change is the output's report and each microphone's
        // together, and each microphone was then called dead, told to set
        // back an interface that had just been set back, and the rig reopened
        // a second time straight after the first.
        const auto failures = audioBackend->takeStreamFailures();
        const bool microphoneStopsTheTake = microphoneRateChangeStopsTheTake (failures, isRecording());
        bool stopTakeForRateChange = false;

        for (const auto& failure : failures)
        {
            // §6.6: the Mac slept with the rig open. Every stream stopped
            // together and none of them failed, so no microphone is marked
            // dead and nothing is reopened. A take running across it has a
            // gap there, which used to go unrecorded: its record and the
            // activity log say where. Between takes nothing was lost.
            if (failure.kind == StreamFailureKind::systemSlept)
            {
                if (systemSleptMarksTheTake (failure.kind, isRecording()))
                {
                    midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                                 kSystemSleptTakeNote });
                    noteActivity (ActivityLevel::Warning, "Recording", kSystemSleptActivityLine);
                }
                continue;
            }

            // The backend knows an id; the user knows a name. A backend that
            // has a better subject than either -- a report that is not about
            // one device -- supplies it.
            auto subject = failure.subject.empty() ? juce::String ("Your headphones")
                                                   : juce::String (failure.subject);

            if (failure.subject.empty() && ! failure.deviceId.empty())
            {
                subject = juce::String (failure.deviceId);

                for (const auto& d : deviceManager.getDevices())
                    if (d.identity.key() == failure.deviceId)
                        subject = juce::String (d.displayName);
            }

            if (failure.kind == StreamFailureKind::resumed && failure.deviceId.empty())
            {
                // The headphone output back at the rig's rate, after a switch
                // set it aside (below). Between takes the rig is reopened,
                // which makes it the clock again. Mid-take the take's streams
                // stay as they are, and Stop reopens it anyway.
                if (capture != nullptr && capture->isOutputAbandoned())
                {
                    const auto line = subject + " " + juce::String (failure.reason);

                    if (isRecording() || capture->isRecording())
                    {
                        noteActivity (ActivityLevel::Warning, subject,
                                      line + " You'll hear the mix again when you stop.");
                    }
                    else
                    {
                        noteActivity (ActivityLevel::Recovered, subject,
                                      line + " You can hear the mix again.");
                        reopenStreams = true;
                    }
                }
                continue;
            }

            // §6.5: a still-listed mic called dead below whose stream is
            // delivering again. It is the same stream -- a macOS IOProc the HAL
            // paused and resumed, across system sleep or another app's hog-mode
            // grab -- so its audio is arriving, and nothing else would ever let
            // its channel go: mid-take deadInputStreams is only cleared by the
            // device leaving the list. The mic's track stayed silent for the
            // rest of the take. The reopen at Stop, if owed, stays owed; it is
            // harmless. Only a mic this owner silenced is taken back.
            if (failure.kind == StreamFailureKind::resumed)
            {
                if (capture != nullptr && ! failure.deviceId.empty()
                    && deadInputStreams.erase (failure.deviceId) > 0)
                {
                    // setChannelLive (true) restarts the channel from an empty
                    // ring, so no audio from before the gap is replayed.
                    capture->setChannelLive (failure.deviceId, true);

                    if (recordingEngine.onStreamResumed (failure.deviceId))
                        midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), failure.deviceId,
                                                     "Microphone started sending audio again: its "
                                                     "channel is live again." });

                    noteActivity (ActivityLevel::Recovered, subject,
                                  "Sending audio again. Its track is live again.");
                    noteDeviceHealthy (failure.deviceId);
                }
                continue;
            }

            // §5.3: the headphone output, not a microphone, changed sample
            // rate -- Audio MIDI Setup, another app, a music player switching
            // the device to its own rate. (An output that has inputs of its
            // own is not hogged, so any app may.) Its callback is the rig's
            // clock, and left alone it went on pulling every microphone at the
            // wrong rate. Mid-take the take was stopped as if a microphone's
            // file format had changed, though none had, and the rest of the
            // show went unrecorded. Between takes nothing reopened it: every
            // ring overflowed, and three polls later the buffer ladder blamed
            // the computer and raised the buffer -- or, at its top rung, the
            // next take recorded with a piece of every track missing.
            if (streamFailureIsOutputRateChange (failure) && capture != nullptr)
            {
                outputRateChanged = true;

                // The combined device's rate is the microphones' own (they are
                // its sub-devices), and a mixer that is both the headphones and
                // some of the microphones carries them on the output callback:
                // either way the take's format did change, whatever the
                // microphones' own reports say or when they arrive. The open
                // stream is asked as well as the selection, which can move
                // mid-take ahead of the reopen at Stop.
                const bool carriesTakeAudio = selectedOutputDeviceId == kOurAggregateUid
                                           || capture->getOutputDeviceId() == kOurAggregateUid
                                           || capture->outputCarriesTakeChannels();

                // Put back once; switched again straight after, something is
                // fighting for it, and a reopen per switch is a tug-of-war
                // with the headphones dropping out at every turn.
                const auto nowMs = juce::Time::getMillisecondCounterHiRes();
                const bool reopenedRecently = outputRateReopenedFor == selectedOutputDeviceId
                                           && nowMs - outputRateReopenedAtMs < kDeadInputReopenBackoffMs;

                auto action = outputRateChangeAction (isRecording() || capture->isRecording(),
                                                      carriesTakeAudio, reopenedRecently,
                                                      microphoneStopsTheTake);
                const auto changed = subject + " " + juce::String (failure.reason);
                juce::String said;

                switch (action)
                {
                    case OutputRateChangeAction::carryOnWithoutIt:
                        said = changed + " Recording carries on, but you won't hear the mix in your "
                                         "headphones until you stop.";

                        // Stop reopens it at the take's rate. With no software
                        // clock to take over, nothing else could clock the
                        // take, and it stops as before.
                        if (capture->abandonOutputClock (said.toStdString()))
                            captureRestartDeferred = true;
                        else
                            action = OutputRateChangeAction::stopTake;
                        break;

                    case OutputRateChangeAction::reopenNow:
                        said = changed + " SobStage is switching it back.";
                        outputRateReopenedAtMs = nowMs;
                        outputRateReopenedFor = selectedOutputDeviceId;
                        reopenStreams = true;
                        break;

                    case OutputRateChangeAction::reopenAtStop:
                        // A microphone's change in this batch stops the take
                        // (below), and Stop's reopen sets the output back too.
                        // Setting it aside instead would tell the user the
                        // take carries on without it, a line before it stops.
                        said = changed + " SobStage is switching it back.";
                        outputRateReopenedAtMs = nowMs;
                        outputRateReopenedFor = selectedOutputDeviceId;
                        captureRestartDeferred = true;
                        break;

                    case OutputRateChangeAction::setAside:
                        said = changed + " Something keeps switching it, so the mix is off in your "
                                         "headphones; recording still works. Set it back in Audio "
                                         "MIDI Setup to hear the mix again.";

                        if (! capture->abandonOutputClock (said.toStdString()))
                            said = changed + " Set it back in Audio MIDI Setup.";
                        break;

                    case OutputRateChangeAction::stopTake:
                        break;
                }

                // Whichever way the take goes on or ends, Stop reopens the
                // rig at its own rate.
                if (action == OutputRateChangeAction::stopTake)
                    captureRestartDeferred = true;

                // stopTake goes on to the ordinary path below, which stops the
                // take as it does for a microphone's rate change.
                if (action != OutputRateChangeAction::stopTake)
                {
                    noteActivity (ActivityLevel::Warning, subject, said);
                    if (firstWarning.isEmpty())
                        firstWarning = said;
                    continue;
                }
            }

            // §5.4's buffer ladder lives or dies here. noteCallbackOverrun()
            // existed, was correct, and was called from NOWHERE -- so the
            // ladder never counted an overrun, never stepped, and the user
            // stayed pinned at 64 samples on a machine that was dropping audio
            // with no automatic relief. session.json's buffer-change log was
            // always empty for the same reason, so the take's own record could
            // not say why it glitched.
            //
            // A processor overload IS the callback missing its deadline, which
            // is exactly what the ladder is counting. Three inside 30 seconds
            // steps it up.
            if (failure.kind == StreamFailureKind::processorOverload
                && noteCallbackOverrun())
                bufferLadderStepped = true;

            auto line = subject + " " + juce::String (failure.reason);
            const bool stopForSafety = streamFailureRequiresRecordingStop (failure.kind)
                                    && isRecording();

            if (stopForSafety)
                line += " Recording stopped before its file format could become untrue.";

            if (streamFailureIsWarning (failure.kind))
            {
                noteActivity (ActivityLevel::Warning, subject, line);
                if (firstWarning.isEmpty())
                    firstWarning = line;
            }
            else
            {
                // Journal the hardware failure while the take folder is still
                // active, then use the ordinary stop/finalize path. This puts
                // both the cause and the stop in activity.log and ensures a
                // changed CoreAudio rate cannot leave a WAV header describing
                // samples captured under another clock.
                noteActivity (ActivityLevel::Failed, subject, line);
                if (firstFailure.isEmpty())
                    firstFailure = line;

                // §6.5: a microphone whose stream died is gone from the take as
                // surely as one that was unplugged, and has to be treated the
                // same way. It was only written to the log: its meter stayed
                // lit, the mid-take card never came up, and the silence its
                // track filled with was counted as the computer being slow.
                // Only while the device is still listed: one that has already
                // left the list is an unplug, which onDeviceListChanged() owns,
                // replug included.
                bool stillListed = false;
                for (const auto& d : deviceManager.getDevices())
                    stillListed = stillListed || d.identity.key() == failure.deviceId;

                //
                // Nothing ever reopened the stream, either: the device stayed
                // listed, so no replug came, and the next take recorded it as
                // silence. Between takes the failure was dropped outright.
                if (! stopForSafety && ! failure.deviceId.empty() && stillListed
                    && capture != nullptr && deadInputStreams.count (failure.deviceId) == 0)
                {
                    const bool takeRunning = capture->isRecording();
                    bool isInput = false;

                    for (const auto& ch : capture->getChannels())
                    {
                        if (ch.deviceId != failure.deviceId)
                            continue;

                        isInput = true;
                        deadInputStreams.insert (ch.deviceId);
                        capture->setChannelLive (ch.deviceId, false);

                        if (takeRunning && ! recordingEngine.isWritingSilence (ch.deviceId))
                        {
                            recordingEngine.onMicUnplugged (ch.deviceId);
                            midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                                         "Microphone stopped sending audio: writing silence "
                                                         "to its channel." });
                            noteDeviceDropout (ch.deviceId, juce::String (ch.displayName));
                        }
                        break;
                    }

                    // Reopened once. One that dies again straight after its
                    // reopen stays dead -- its strip asleep, the next take
                    // reporting it -- rather than holding the rig in a loop.
                    const auto reopened = deadInputReopenedAtMs.find (failure.deviceId);
                    const bool reopenedRecently = reopened != deadInputReopenedAtMs.end()
                        && juce::Time::getMillisecondCounterHiRes() - reopened->second < kDeadInputReopenBackoffMs;

                    if (isInput)
                    {
                        switch (deadInputStreamAction (takeRunning, reopenedRecently))
                        {
                            case DeadInputStreamAction::reopenAtStop: captureRestartDeferred = true; break;
                            case DeadInputStreamAction::reopenNow:    reopenStreams = true; break;
                            case DeadInputStreamAction::leaveDead:    break;
                        }
                    }
                }
            }

            if (stopForSafety)
            {
                // Stop reopens the rig at its own rate, which sets the device
                // back. Without that the device-list notification this same
                // change queued -- run after the stop -- found the rig
                // unchanged and reopened nothing, and the next take ran with
                // the device still at the other rate and its one report spent.
                captureRestartDeferred = true;
                stopTakeForRateChange = true;
            }
        }

        // After the loop, so the rest of the batch was read against the
        // take's own streams (isRecording() still true: no microphone in it
        // was called dead, and nothing was reopened for it).
        bool stoppedTake = false;

        if (stopTakeForRateChange && isRecording())
        {
            stopReason = "an audio device changed sample rate";
            toggleRecording();
            stoppedTake = true;
        }

        // After the loop, so several streams dying together reopen once. A
        // stop above owes its own reopen (captureRestartDeferred), done by
        // Stop or, with a camera still finishing, once it has.
        if (reopenStreams && ! stoppedTake)
            restartCapture();

        // What the rings lost before this poll heard of an output rate
        // change was that output pulling them at its new rate, not the
        // computer missing a deadline. Counted by the ladder below on the next
        // poll, it was a third of the way to blaming the computer and raising
        // the buffer for a fault that has just been dealt with.
        if (outputRateChanged && capture != nullptr)
            reportedRingLossEvents = capture->getRingLossEvents();

        // Acted on after the loop, so one batch of overloads produces one step
        // rather than one per report.
        //
        // §5.4 forbids a change mid-take: a buffer change during a recording is
        // itself a dropout risk, and §0.1 puts not losing audio first. The
        // ladder still counts and steps during a take -- stepping *up* is what
        // it forbids stepping *down* -- but the reopen waits for the take to
        // end. It used to be handed to onDeviceListChanged(), which does not
        // reopen anything mid-take and did not owe the reopen either, so a
        // step taken during a take was announced and then never applied.
        if (bufferLadderStepped)
            return applyBufferLadderStep ("");

        if (firstFailure.isNotEmpty())
            return firstFailure;
        if (firstWarning.isNotEmpty())
            return firstWarning;
    }

    // §5.4's trigger, from the app's own rings. The ladder was fed only by
    // CoreAudio's processor-overload property, which the other two backends
    // never raise and which says nothing about the one place this app
    // actually loses audio to timing: a device ring that ran dry because its
    // block came late, or filled because the pull came late. Those are the
    // callback missing its deadline in every sense the spec means, and a
    // machine whose scheduling jitter is wider than a two-block cushion sat
    // at 64 samples losing a block every few seconds with the ladder never
    // hearing about it. Three inside thirty seconds steps it up; the streams
    // reopen at the new size through the same path a hot-plug takes.
    if (capture != nullptr)
    {
        const auto eventsNow = capture->getRingLossEvents();

        // The counters live in the streams and go back to zero whenever the
        // streams are rebuilt, which every step does.
        if (eventsNow < reportedRingLossEvents)
            reportedRingLossEvents = 0;

        // One overrun per poll in which any ring lost audio, however many
        // events the streams counted: a single stall dries every ring at
        // once and every tick it lasts, and counted per event -- even capped
        // at three -- one stall was the whole trigger, and the ladder stepped
        // twice inside a second. §5.4's three are three separate occasions.
        //
        // Not while the headphone output is being handed over for a rate
        // change (above): those losses are the hand-over's.
        const bool handingOverForRate = juce::Time::getMillisecondCounterHiRes() < outputRateHoldGraceUntilMs;
        const bool stepped = eventsNow > reportedRingLossEvents && ! handingOverForRate
                          && noteCallbackOverrun();
        reportedRingLossEvents = eventsNow;

        if (stepped)
            return applyBufferLadderStep (" with the microphones");
    }

    // §0.1: audio lost between the device and the app. Counted by the backends
    // and, until now, reported by nobody -- the take's own dropped-frame figure
    // only ever covered what the writer could not put on disk.
    if (audioBackend != nullptr)
    {
        const auto droppedNow = audioBackend->getFramesDroppedByBackend();

        // The counters live in the stream objects, and every rename, hot-plug
        // or output change tears those down and builds new ones -- so this
        // total goes back to zero regularly. Compared against a high-water mark
        // that never reset, one noisy USB port early in a session silently
        // blinded the report for the rest of it: every later loss sat below the
        // old mark and was never mentioned again.
        if (droppedNow < reportedBackendDrops)
            reportedBackendDrops = 0;

        if (droppedNow > reportedBackendDrops)
        {
            reportedBackendDrops = droppedNow;

            const auto line = juce::String ("Your sound hardware is delivering more audio than it "
                                            "can hand over, and some has been lost. Try a different "
                                            "USB port, and close other apps using audio.");

            noteActivity (ActivityLevel::Warning, "Sound hardware", line);
            return line;
        }
    }

    // §0.1: a microphone that changed its channel count mid-take. Reported
    // live, like the drive-side and backend-side losses -- this one only ever
    // reached session.json, so audio was being lost during a take with nothing
    // on screen saying so, which is the silence this whole thing is about.
    if (capture != nullptr && capture->isRecording())
    {
        const auto missed = capture->getFramesMissedByLayout();

        // The same backwards guard its siblings carry. The counter is only
        // zeroed by startRecording today, in the same call that zeroes this --
        // but being the one member of the family without the guard is how the
        // next person introduces the bug the others already had.
        if (missed < reportedLayoutMisses)
            reportedLayoutMisses = 0;

        if (missed > reportedLayoutMisses)
        {
            reportedLayoutMisses = missed;

            // Worded for what every path here has in common -- a device is
            // delivering audio this take cannot fit -- rather than asserting
            // the channel-count diagnosis, which is true of one of the four
            // ways this counter rises and wrong advice for the other three.
            const auto line = juce::String ("A microphone is sending audio this take can't fit, so "
                                            "some of it isn't being recorded. Unplug it and plug it "
                                            "back in after this take.");

            noteActivity (ActivityLevel::Failed, "Recording", line);
            return line;
        }
    }

    // The monitor path breaking up. Not lost recording -- §6.1 keeps the two
    // apart, and the take is unaffected -- but a machine that cannot keep the
    // headphones fed is one that will start losing the recording next, and the
    // user hearing clicks deserves to know it is the machine rather than a
    // microphone. Reported on a rising count, once per rise.
    if (audioBackend != nullptr)
    {
        const auto glitchesNow = audioBackend->getOutputGlitchCount();

        if (glitchesNow < reportedOutputGlitches)
            reportedOutputGlitches = 0;

        if (glitchesNow > reportedOutputGlitches)
        {
            reportedOutputGlitches = glitchesNow;

            const auto line = juce::String ("The sound you're hearing is breaking up -- this "
                                            "machine is struggling to keep up. The recording "
                                            "itself is unaffected. Close other apps.");

            noteActivity (ActivityLevel::Warning, "Monitoring", line);
            return line;
        }
    }

    // A take that could not start. Reported here rather than only at the click,
    // because the click's own moment passes and this line is where the user
    // looks. Held until the next take starts, not until the next poll.
    if (recordStartProblem.isNotEmpty() && recordingEngine.getState() == RecordingState::Idle)
        return recordStartProblem;

    // §6.3: a mirror was asked for and this take does not have one -- either
    // its folder could not be made, or its files could not be opened. Both used
    // to leave the policy saying "Active" over a backup that did not exist, so
    // both are caught here, by comparing what was asked for against what is
    // actually being written. Said once per take.
    // Asks the pipeline directly as well as inferring it from the policy. The
    // inference alone was the whole test, which made the pipeline's own flag
    // dead code and left the report resting on a proxy: it only fires while a
    // take is running AND the policy still says Active, so a mirror that failed
    // to open in a take the policy had already given up on said nothing.
    // A mirror that BROKE is not a mirror that never opened, and this branch
    // was catching both.
    //
    // WritePipeline sets mirrorWriteFailed and calls stopMirroring() in the
    // same pass, so a mid-take write failure leaves capture->isMirroring()
    // false while the policy still says Active -- which matched the second
    // disjunct below and produced "that folder couldn't be opened" for a
    // backup drive that had filled up ninety minutes into a take, with a
    // ninety-minute copy sitting on disk.
    //
    // It also consumed mirrorPolicy.noteWriteFailure(), which is one-shot, so
    // the branch further down written for exactly this case could never fire
    // -- taking its out-of-space wording with it, the only consumer of the
    // flag WritePipeline latches for it.
    //
    // Write failures are excluded here and fall through to that branch.
    if (capture != nullptr && capture->isRecording() && ! mirrorMissingReported
        && ! capture->hasMirrorWriteFailed()
        && (capture->hasMirrorFailedToOpen()
            || (mirrorPolicy.isMirroring() && ! capture->isMirroring())))
    {
        mirrorMissingReported = true;

        // The policy is caught up too, so isMirroring() and session.json stop
        // claiming a second copy that does not exist.
        mirrorPolicy.noteWriteFailure();

        const auto line = juce::String ("There is no backup copy of this take -- that folder "
                                        "couldn't be opened. The recording itself is fine and is "
                                        "going to the card.");

        noteActivity (ActivityLevel::Failed, "Local backup", line);
        return line;
    }

    // §6.3: the mirror's destination failed -- unplugged, read-only, or dead.
    //
    // WritePipeline has always stopped mirroring on a failed write, and rightly
    // leaves the card write alone (§6.3: the mirror must never take the
    // recording down with it). What nothing ever did was *notice*, so pulling
    // the backup drive mid-take stopped the copy in silence: no message, and
    // nothing in session.json, because only the low-space stop was ever
    // recorded. The user kept a truncated backup they had every reason to
    // believe was complete.
    //
    // Checked outside the isMirroring() guard below, and deliberately so: the
    // pipeline raises this flag and stops mirroring in the same breath, on its
    // own writer thread. By the time this poll runs isMirroring() is already
    // false, so nesting the check inside that guard would make it unreachable
    // -- policy that exists and never runs, which is the whole shape of bug
    // this series keeps finding. noteWriteFailure() is the idempotence: it
    // fires only on the transition out of Active, so a mirror that never
    // started, or one already stopped for space, produces nothing.
    //
    // capture->stopMirroring() is not called here because the pipeline has
    // already done it; this only catches the policy up so session.json can say
    // why the copy is short.
    if (capture != nullptr && capture->hasMirrorWriteFailed()
        && mirrorPolicy.noteWriteFailure())
    {
        // The mirror lives on the computer's own disk, so "that drive stopped
        // accepting writes" is almost always "that disk is full" -- and the
        // difference is the one thing the user can act on. The generic sentence
        // stays for anything the writer could not account for.
        //
        // Worded here rather than borrowed from the writer: the writer's
        // sentence is written for the card and says recording has stopped and
        // every file has been closed, which is true of the card and alarming
        // nonsense about a backup, whose failure stops no recording at all.
        const auto line = capture->mirrorRanOutOfSpace()
            ? juce::String ("The local backup copy stopped -- this computer's disk is full. "
                            "The recording itself is unaffected and is still going to the "
                            "card. Free up space to have a backup again.")
            : juce::String ("The local backup copy stopped -- that drive stopped accepting "
                            "writes. The recording itself is unaffected and is still going "
                            "to the card.");

        noteActivity (ActivityLevel::Failed, "Local backup", line);
        return line;
    }

    // §6.3: a backup that stopped for space and then could not close its files.
    //
    // The low-space stop (the write safety step near the top) calls
    // stopMirroring(), which makes the writer thread finalize the mirror --
    // and if one of those closes fails, the
    // pipeline latches mirrorWriteFailed a poll or two later. By then the
    // policy is StoppedLowSpace, so noteWriteFailure() returns false and the
    // branch above says nothing. The user was told the backup stopped early,
    // which sounds like a short file that plays; what they actually have is
    // one whose header says it holds no audio.
    if (capture != nullptr && ! mirrorFinalizeFailureReported
        && (mirrorPolicy.wasStoppedForSpace() || mirrorPolicy.wasStoppedByUser())
        && capture->hasMirrorWriteFailed())
    {
        mirrorFinalizeFailureReported = true;

        // Turning the backup off in Settings stops it the same way, so its
        // close can fail the same way.
        const auto line = juce::String (mirrorPolicy.wasStoppedByUser()
                                            ? "The local backup copy was turned off and its files "
                                            : "The local backup copy stopped for space and its files ")
                        + "could not be closed properly, so they may not open. The "
                          "recording on the card is unaffected.";

        noteActivity (ActivityLevel::Failed, "Local backup", line);
        return line;
    }

    // §6.3: the mirror is re-judged during the take, and once it stops it never
    // restarts within the same recording. The stop itself is made near the top
    // of this function so no warning can starve it; its line is said here, once,
    // when nothing above is holding the advice line.
    if (std::exchange (mirrorLowSpaceNoticePending, false))
        return kMirrorLowSpaceLine;


    // The notice's clock runs from here, not from the branch that shows it:
    // decremented where it is returned, a warning outranking it for a few
    // seconds would silently extend how long it lingers afterwards.
    if (midTakeNoticeSeconds > 0.0)
        midTakeNoticeSeconds -= sinceLastCallSeconds;

    // §6.5 buffer back-pressure, before the remaining-time warnings: the ring
    // filling up is audio about to be lost right now, where running low on
    // room is audio that will stop being recorded later.
    //
    // CapacityMonitor has always had this policy, and always had tests for it.
    // Nothing had ever called it, so the 50% warning was never shown and the
    // 90% fallback never happened -- §0.1's "never silently drop" was exactly
    // what the app did.
    if (capture != nullptr && recordingEngine.getState() == RecordingState::Recording)
    {
        // The mirror is fed by this same ring and writer thread. It protects a
        // take from one destination failing, not from this queue overflowing,
        // so its presence must never suppress the 90% mix-only safety step.
        switch (capacityMonitor.evaluateFill (capture->getRingFillFraction()))
        {
            case WritePipelineState::DegradedToMixOnly:
                // The fallback itself was made near the top of this function,
                // where no warning can return ahead of it; this is only its line.
                {
                    const auto line = juce::String (CapacityMonitor::fillStatusLine (
                        WritePipelineState::DegradedToMixOnly, true));

                    noteActivity (ActivityLevel::Failed, "Drive", line);
                    return line;
                }

            case WritePipelineState::FillWarning:
                {
                    // After the fallback the stems are gone for the rest of the
                    // take, so "nothing has been lost" would no longer be true.
                    const auto line = juce::String (CapacityMonitor::fillStatusLine (
                        WritePipelineState::FillWarning, capture->isMixOnly()));

                    noteActivity (ActivityLevel::Warning, "Drive", line);
                    return line;
                }

            case WritePipelineState::Healthy:
                break;
        }
    }

    // §6.5 next: running out of room stops the take, which outranks everything
    // else that is merely a warning.
    switch (pollCapacityWarning())
    {
        case RemainingTimeWarning::Exhausted:
            // Kept only so the switch stays exhaustive. A running take that is
            // out of room is stopped and returned from at the top of this
            // function, and pollCapacityWarning returns None when no take is
            // running -- so nothing reaches here. A full drive with no take
            // running is the record button's job, not this line's.
            break;
        case RemainingTimeWarning::TwoMinutes:
            return "About two minutes of room left. Wrap up or switch drives now.";
        case RemainingTimeWarning::TenMinutes:
            return "About ten minutes of room left on this drive.";
        case RemainingTimeWarning::None:
            break;
    }

    // §6.6 next: warned before it causes dropouts, not after.
    const auto load = capture != nullptr ? capture->getAudioCallbackLoad() : 0.0;

    switch (updatePerformance (load, isSystemThermallyThrottled()))
    {
        case PerformanceWarning::ThermalThrottling:
        {
            const auto line = juce::String (
                "This machine is overheating and is about to drop audio. Close other apps.");
            noteActivity (ActivityLevel::Warning, "Performance", line);
            return line;
        }
        case PerformanceWarning::SustainedCpuPressure:
        {
            const auto line = juce::String (
                "This machine is working hard. Close other apps before it starts dropping audio.");
            noteActivity (ActivityLevel::Warning, "Performance", line);
            return line;
        }
        case PerformanceWarning::None:
            break;
    }

    // §6.5: a microphone plugged in mid-take joins nothing, and the user has to
    // be told rather than left assuming it is being recorded. Ranked here, below
    // everything that means audio is being lost or the take is about to end: it
    // is information, not a problem, and it must never sit on top of a warning
    // that the drive is failing.
    if (midTakeNoticeSeconds > 0.0 && midTakeNotice.isNotEmpty())
        return midTakeNotice;

    // §14.6: which mic was just heard alone. Not an error and not a message --
    // the UI highlights that meter, which is how a user with four identical
    // mics learns which meter is which person. Runs only outside a take, when
    // naming actually happens.
    if (capture == nullptr || ! capture->isRecording())
    {
        if (tapDetector == nullptr || tapDetectorChannels != micCount)
        {
            tapDetector = micCount > 0 ? std::make_unique<TapToNameDetector> (micCount) : nullptr;
            tapDetectorChannels = micCount;
        }

        if (tapDetector != nullptr)
        {
            const auto result = tapDetector->processBlock (peaksDb, sinceLastCallSeconds);

            if (result == TapResult::ChannelIdentified)
                tappedChannel = tapDetector->getTappedChannel();

            // §14.6's other half. Ambiguous was produced, latched, reset and
            // shown to nobody: the sentence it exists for lived only in a
            // comment on the enumerator. So a novice with four identical Yetis
            // taps one, two mics hear it, and the app does nothing at all --
            // which is indistinguishable from tap-to-name being broken, and
            // tap-to-name is the answer to the first-run blocker of not being
            // able to tell four identical microphones apart.
            //
            // onTheLine because the user just did something physical and is
            // waiting to see what it did. noteActivity collapses the repeat, so
            // tapping again while it is still up does not stack.
            // At most once every ten minutes as well: a room where people keep
            // talking goes quiet between sentences, and each sentence is not a
            // tap -- once a minute still filled the log during a sound check.
            const auto nowMs = juce::Time::getMillisecondCounterHiRes();
            if (result == TapResult::Ambiguous
                && nowMs - lastAmbiguousTapNoticeMs >= kAmbiguousTapNoticeIntervalMs)
            {
                lastAmbiguousTapNoticeMs = nowMs;
                noteActivity (ActivityLevel::Started, "Microphones",
                              "Two mics heard that. Tap closer to the one you want to name.", true);
            }

            // Latch consumed; listen for the next tap. The meter's 2-second
            // peak hold keeps re-identifying while the sound decays, which is
            // what makes the highlight linger long enough to see.
            if (result != TapResult::Listening)
                tapDetector->reset();
        }
    }

    // §10.6: after a take, say where it went. Outranks ambient advice because
    // it answers the question the user actually has right now.
    if (savedNoticeSeconds > 0.0)
    {
        savedNoticeSeconds -= sinceLastCallSeconds;

        // §0.1: never claim audio that is not there. A take whose files hold
        // nothing but headers is where the take went, not what it saved, and
        // saying "Saved" would be the one word the user needed to be false.
        // A stream that ran for the whole take and carried nothing is a
        // different fault from one that never arrived, and sending someone to
        // check the drive when the problem is a muted microphone wastes the
        // one moment they are still standing next to the rig.
        // Not the user's rig. Say so plainly rather than sending them to check
        // hardware that was working the whole time.
        if (lastTakeVerdict == TakeAudioVerdict::DroppedByApp)
            return "Recording stopped, and the sound did not reach the files in "
                   + lastSessionFolder
                   + " -- your microphones were working. This is a fault in SobStage.";

        if (lastTakeVerdict == TakeAudioVerdict::OnlySilence)
            return "Recording stopped, but the files in " + lastSessionFolder
                   + " are silent -- the microphones were connected but sent no sound.";

        if (lastTakeHeldNoAudio)
            return "Recording stopped, but the files in " + lastSessionFolder
                   + " are empty -- no audio reached the drive.";

        if (takeCardUnresponsive)
            return "Recording stopped, but the card stopped answering, so the end of the take "
                   "may be missing from " + lastSessionFolder
                   + (lastMirrorFolder.isNotEmpty()
                          ? ". The backup copy is at " + lastMirrorFolder + "."
                          : juce::String (". Check the card before recording again."));

        if (lastTakeCopiesToCard)
            return "Saved on this computer. Copying to the card.";

        return "Saved to " + lastSessionFolder;
    }

    // The post-take combine, which nothing had ever asked about. TakeCombiner
    // has always set a problem string and Application has always had a getter
    // for it, and no code anywhere in the app called that getter -- so a
    // combine that failed after the take reported nothing, and the user was
    // left looking for a file that was never written.
    {
        const auto combine = takeCombiner.getStatus();
        const auto problem = juce::String (combine.problem);

        if (! combine.running && problem.isNotEmpty() && combine.run != reportedCombineRun)
        {
            reportedCombineRun = combine.run;

            noteActivity (ActivityLevel::Warning, "Combined video", problem);
            return problem;
        }
    }

    // §10.5: hardware guidance, most serious first.
    const auto advice = getSetupAdvice();

    if (! advice.empty())
        return juce::String (advice.front().message);

    // Last, and only when nothing above had anything to say: the most serious
    // thing in the journal the user has not been shown.
    //
    // This is what closes the gap the journal was written for. Every branch
    // above holds the line for as long as its condition is true, so anything
    // that happened underneath one of them -- a mic that dropped and came back
    // during a disk warning, a backup that never opened while the drive was
    // busy -- reached the record and never reached the screen. Here it does,
    // once the loud thing has finished being loud.
    if (activityLineSeconds > 0.0)
    {
        activityLineSeconds -= sinceLastCallSeconds;
        return activityLine;
    }

    if (ActivityEntry unseen; activity.getMostSeriousUnseen (unseen))
    {
        // That one entry, not all of them. The line can only show one sentence,
        // so marking the rest seen because this one was shown would lose every
        // other piece of unread news -- which is the silence this whole thing
        // is here to remove.
        activity.markSeen (unseen.id);

        // Ordinary starts and stops are not worth interrupting a quiet screen
        // for -- they are in the panel and in the take's own log. The line is
        // for the things that need acting on, plus the few things the journal
        // marks as belonging on the screen: a microphone arriving is not a
        // warning, and the user still wants it confirmed the moment it lands.
        if (unseen.onTheLine || unseen.level == ActivityLevel::Warning
            || unseen.level == ActivityLevel::Failed)
        {
            activityLine = juce::String (unseen.message);
            activityLineSeconds = 8.0;
            return activityLine;
        }
    }

    return {};
}

void Application::exportDiagnostics (const juce::File& destinationZip)
{
    // §11: logs + last 5 session.json + device inventory. NEVER audio -- the
    // point of a diagnostics bundle is that it can be sent to a stranger.
    //
    // A second press while one is still being written would race it for the
    // same kind of file; say so rather than start another.
    if (diagnosticsExportRunning->exchange (true))
    {
        noteActivity (ActivityLevel::Warning, "Diagnostics",
                      "Still saving the last diagnostics file. Give it a moment.", true);
        return;
    }

    juce::Array<juce::var> deviceArray;
    for (const auto& d : deviceManager.getDevices())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name", juce::String (d.displayName));
        obj->setProperty ("usbId", juce::String (d.identity.key()));
        obj->setProperty ("included", d.included);
        obj->setProperty ("exclusionReason", juce::String (d.exclusionReason));
        obj->setProperty ("driftPpm", d.measuredDriftPpm);
        obj->setProperty ("driftMeasured", d.hasDriftMeasurement);
        deviceArray.add (juce::var (obj));
    }

    auto* summary = new juce::DynamicObject();
    summary->setProperty ("appVersion", appVersionString());
    summary->setProperty ("sampleRate", currentSampleRate);
    summary->setProperty ("bitDepth", currentBitDepth);
    summary->setProperty ("bufferSize", capture != nullptr ? capture->getBufferSizeSamples() : bufferLadder.getCurrentSize());
    summary->setProperty ("bufferSizeNext", bufferLadder.getCurrentSize());
    summary->setProperty ("backend", audioBackend != nullptr
                                         ? juce::String (audioBackend->getBackendName())
                                         : juce::String ("none"));
    summary->setProperty ("outputDevice", juce::String (selectedOutputDeviceId));
    summary->setProperty ("outputProblem", juce::String (outputSelectionProblem));
    summary->setProperty ("monitorProblem", getMonitorProblem());
    summary->setProperty ("destination", juce::String (destinationFolder));
    summary->setProperty ("devices", juce::var (deviceArray));

    // Everything above is in memory. Everything below reads the recordings
    // folder, which is usually a card -- and a card pulled or wedged while the
    // button is pressed used to freeze the window inside the directory scan.
    // So the rest runs on a detached worker holding only copies; the result
    // comes back through the alive token like every other OS-thread callback.
    const auto inventoryText = juce::JSON::toString (juce::var (summary), true);
    const juce::String root (destinationFolder);
    const auto logFile = getLogFile();
    const auto running = diagnosticsExportRunning;
    const auto alive = getAliveToken();

    std::thread ([this, alive, running, inventoryText, root, logFile, destinationZip]
    {
        juce::ZipFile::Builder builder;
        juce::TemporaryFile tempInventory;

        // §11: a bundle missing the device inventory is the bundle that cannot
        // answer "what was plugged in", which is the first question anyone
        // reading it asks. Silently shipping one without it wastes a round trip.
        const bool inventoryWritten = replaceWithTextChecked (tempInventory.getFile(), inventoryText);
        builder.addFile (tempInventory.getFile(), 9, "device_inventory.json");

        // §11: the last five sessions. Newest first, because the one being
        // asked about is almost always the most recent.
        auto sessions = findRecentSessionMetadata (root, 5);
        int index = 0;

        for (const auto& file : sessions)
            builder.addFile (file, 9, "sessions/" + juce::String (++index) + "_"
                                          + file.getParentDirectory().getFileName() + ".json");

        if (logFile.existsAsFile())
            builder.addFile (logFile, 9, "log.txt");

        // Built in memory first, so the same bytes can go to a second place:
        // the builder's sources are read once and are spent afterwards. A
        // bundle is the log (capped) plus a few small JSON files.
        juce::MemoryOutputStream zipped;
        const bool built = builder.writeToStream (zipped, nullptr);

        const auto writeZipTo = [&zipped] (const juce::File& target)
        {
            bool ok = false;
            {
                juce::FileOutputStream out (target);
                ok = out.openedOk() && out.write (zipped.getData(), zipped.getDataSize());
                if (ok)
                {
                    out.flush();
                    ok = out.getStatus().wasOk();
                }
            }

            // Only ever a name getNonexistentChildFile picked, so whatever is
            // there is this attempt's own partial file.
            if (! ok)
                target.deleteFile();

            return ok;
        };

        // The Desktop is the place people look, but on macOS it is a protected
        // folder: the first export raises a "would like to access files in
        // your Desktop folder" prompt, and once that is answered Don't Allow
        // (or a managed Mac forbids it) every export failed, with advice to
        // "try somewhere else" and no way to choose anywhere. A Linux account
        // with no ~/Desktop failed the same way. The home folder itself is
        // never protected, so the bundle goes there instead and the message
        // says where.
        juce::File savedTo;
        if (built)
        {
            if (writeZipTo (destinationZip))
            {
                savedTo = destinationZip;
            }
            else
            {
                const auto fallback = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                          .getNonexistentChildFile (destinationZip.getFileNameWithoutExtension(),
                                                                    destinationZip.getFileExtension());
                if (fallback.getParentDirectory() != destinationZip.getParentDirectory()
                    && writeZipTo (fallback))
                    savedTo = fallback;
            }
        }

        const bool saved = savedTo != juce::File();

        running->store (false);

        juce::MessageManager::callAsync ([this, alive, inventoryWritten, saved, savedTo, destinationZip]
        {
            if (alive.lock() == nullptr)
                return;

            if (! inventoryWritten)
                noteActivity (ActivityLevel::Warning, "Diagnostics",
                              "Couldn't list the connected devices, so the diagnostics file won't "
                              "include them.");

            if (saved)
            {
                // onTheLine, because the user pressed a button and nothing else
                // happens: the level alone would have kept this out of the
                // advice line, so a successful export looked identical to
                // nothing happening and the path to the file was never shown.
                noteActivity (ActivityLevel::Stopped, "Diagnostics",
                              "Saved a diagnostics file to " + savedTo.getFullPathName() + ".",
                              true);
            }
            else
            {
                // A failed export must not look like a successful one right up
                // until the user goes to attach it to an email. There is no
                // picker to "try somewhere else" with, so it says what was tried.
                noteActivity (ActivityLevel::Failed, "Diagnostics",
                              "Couldn't write the diagnostics file to "
                              + destinationZip.getParentDirectory().getFullPathName()
                              + " or to your home folder. Check that there is free space on "
                                "this computer's disk.");
            }
        });
    }).detach();
}

juce::File Application::getSettingsFile()
{
    return getLogFile().getSiblingFile ("settings.json");
}

void Application::loadSettings()
{
    const auto file = getSettingsFile();

    if (! file.existsAsFile())
        return;

    // Anything unreadable comes back as defaults rather than as a failure:
    // §10.1 says the app launches to a working state, and a preferences file
    // truncated by a power cut is not a reason to refuse to start.
    rememberedSettings = AppSettings::fromJsonString (file.loadFileAsString().toStdString());

    // Defaults instead of a failure is right; defaults instead of a failure
    // AND without a word is not. Everything §2.4 remembers about the rig has
    // just been replaced, and the user is about to meet an app that looks like
    // it forgot them.
    if (rememberedSettings.wasUnreadable)
        noteActivity (ActivityLevel::Warning, "Settings",
                      "Your saved settings couldn't be read, so everything is back to its "
                      "defaults -- check your microphone names and where takes are being saved.");

    applyingRememberedSettings = true;

    // §10.1: preserve the user's remembered destination without touching it
    // on the launch thread. Even a seemingly harmless isDirectory() can wait
    // indefinitely on a stale removable or network volume. The detached
    // recovery/preflight/status workers will establish its current condition,
    // and the user can switch to the always-available default immediately.
    if (! rememberedSettings.destinationFolder.empty())
    {
        destinationFolder = rememberedSettings.destinationFolder;
        confirmedSaveLocation = rememberedSettings.confirmedSaveLocation;
    }

    askWhereToSaveEveryTime = rememberedSettings.askWhereToSaveEveryTime;
    setupGuideDone = rememberedSettings.setupGuideDone;
    mirrorPolicy.setEnabledByUser (rememberedSettings.mirrorEnabled);
    recordOnComputerThenCopy = rememberedSettings.recordOnComputerThenCopy;
    cardAtSaveLocation = rememberedSettings.cardAtSaveLocation;
    // §7: what other apps see. A settings file written under the app's previous
    // name carries that name here, and carrying it forward would leave the
    // device in everyone's Zoom and OBS still called the old thing -- a rename
    // everywhere except the one place other software looks.
    //
    // Only the untouched default is moved. Someone who typed their own name
    // into this field meant it, and it is not ours to overwrite.
    aggregateName = rememberedSettings.aggregateName == "Multi-Mic Aggregator"
                        ? juce::String ("SobStage")
                        : juce::String (rememberedSettings.aggregateName);
    masterVolume = rememberedSettings.masterVolume;
    rememberedOutputDeviceId = rememberedSettings.rememberedOutputDeviceId;
    monitorThroughCombinedDevice = rememberedSettings.monitorThroughCombinedDevice;
    headphonesOffKeys = { rememberedSettings.headphonesOffKeys.begin(),
                          rememberedSettings.headphonesOffKeys.end() };
    cameraController.setPreviewQuality (rememberedSettings.cameraPreviewFullQuality
                                            ? PreviewQuality::Full : PreviewQuality::Low);

    // Camera discovery now runs away from the message thread, so the first OS
    // snapshot may land after initialise() has returned. Seed choices by id
    // now; CameraSelection intentionally keeps choices for unavailable cameras
    // and applies them when that id appears.
    for (const auto& camera : rememberedSettings.cameras)
    {
        cameraController.getSelection().setEnabled (camera.id, camera.enabled);
        cameraController.getSelection().setQuality (camera.id, cameraQualityFromKey (camera.quality));
        if (! camera.assignedName.empty())
            cameraController.getSelection().setAssignedName (camera.id, camera.assignedName);
    }
    // The size table these index into roughly doubled at every step, because
    // the old top of the range -- 456px in a 760px window -- was not a picture
    // anyone could judge focus on. A settings file written before that carries
    // the old default, 1, which was never a choice so much as the value nobody
    // had reason to change; lifting exactly that one to the new default is what
    // stops the enlargement from reaching only people installing fresh.
    //
    // Any other remembered value was somebody moving the arrows on purpose, and
    // it is kept: the same index is a much larger picture now anyway.
    // Restored as stored. It used to rewrite any 1 into 5, which made the
    // second-smallest tile the one setting a user could not keep: pick it,
    // relaunch, and the tiles came back at their largest. The rewrite was
    // guarding against an old parse fallback of 1 that is now the struct
    // default, so a missing or unreadable value already lands on 5 without
    // destroying a deliberate choice.
    cameraTileScale = rememberedSettings.cameraTileScale;
    combineVideoAndAudio = rememberedSettings.combineVideoAndAudio;
    checkForUpdates = rememberedSettings.checkForUpdates;
    lastUpdateCheckSeconds = rememberedSettings.lastUpdateCheckSeconds;
    // At most once a day, at launch, and only once the user has said yes;
    // pollUpdateCheck() makes the call once no take is running.
    automaticUpdateCheckWanted = checkForUpdates;
    deliveryTarget = juce::String (rememberedSettings.deliveryTarget);
    sampleRateOverride = rememberedSettings.sampleRateOverride;
    bufferSizeOverride = rememberedSettings.bufferSizeOverride;

    if (rememberedSettings.bitDepthOverride == 16
        || rememberedSettings.bitDepthOverride == 24
        || rememberedSettings.bitDepthOverride == 32)
        currentBitDepth = rememberedSettings.bitDepthOverride;

    // §2.4: the names and trims go back into the store they were taken from, so
    // every path that already reads it -- stem filenames, the monitor mix, the
    // trim sliders -- picks them up without knowing a file was involved.
    for (const auto& port : rememberedSettings.ports)
    {
        PortIdentity id;
        id.locationId = port.key;
        portIdentityStore.put (id, port.settings);
    }

    applyingRememberedSettings = false;
}

void Application::applyRememberedDeviceSettings()
{
    if (rememberedSettings.ports.empty() && rememberedSettings.disabledMicKeys.empty())
        return;

    applyingRememberedSettings = true;

    for (const auto& device : deviceManager.getDevices())
    {
        const auto key = device.identity.key();

        // The store is keyed by PortIdentity::key(), which is what was written,
        // so a device only matches the entry that was actually about it.
        if (const auto* port = rememberedSettings.findPort (key); port != nullptr
                && ! portIdentityStore.contains (device.identity))
            portIdentityStore.put (device.identity, port->settings);

        if (rememberedSettings.isMicDisabled (key) && device.userEnabled)
            deviceManager.setUserEnabled (key, false);
    }

    applyingRememberedSettings = false;
}

void Application::setSampleRateOverride (uint32_t rate)
{
    if (sampleRateOverride == rate)
        return;

    sampleRateOverride = rate;
    saveSettings();

    // The rate is fixed for the life of a stream (§5.4), so the streams are
    // reopened rather than nudged. handleDeviceChange re-runs §2.2 and applies
    // the new choice through exactly the path a hot-plug takes.
    onDeviceListChanged();
}

void Application::setBitDepthOverride (int bits)
{
    if (bits != 16 && bits != 24 && bits != 32)
        return;

    if (currentBitDepth == bits)
        return;

    // Bit depth is a property of the files, chosen when a take starts, so this
    // needs no stream reopened -- it simply applies to the next press of record.
    currentBitDepth = bits;
    saveSettings();
}

void Application::setBufferSizeOverride (int samples)
{
    if (samples < 0)
        return;

    if (bufferSizeOverride == samples)
        return;

    bufferSizeOverride = samples;
    saveSettings();

    // Fixed for the life of a stream (§5.4), so the streams are reopened
    // through the same path a hot-plug takes.
    onDeviceListChanged();
}

void Application::saveSettings()
{
    // Guarded so applying a loaded file cannot write a half-applied rig back
    // over the complete one it came from.
    if (applyingRememberedSettings)
        return;

    AppSettings settings;
    // Whatever a newer version wrote that this one cannot read goes back out
    // as it came in, so running an older copy once does not erase it.
    settings.keepUnknownFieldsFrom (rememberedSettings);
    settings.destinationFolder = destinationFolder;
    settings.confirmedSaveLocation = confirmedSaveLocation;
    settings.askWhereToSaveEveryTime = askWhereToSaveEveryTime;
    settings.setupGuideDone = setupGuideDone;
    settings.mirrorEnabled = mirrorPolicy.isEnabledByUser();
    settings.recordOnComputerThenCopy = recordOnComputerThenCopy;
    settings.cardAtSaveLocation = cardAtSaveLocation;
    settings.aggregateName = aggregateName.toStdString();
    settings.masterVolume = masterVolume;
    settings.rememberedOutputDeviceId = rememberedOutputDeviceId;
    settings.monitorThroughCombinedDevice = monitorThroughCombinedDevice;
    settings.headphonesOffKeys.assign (headphonesOffKeys.begin(), headphonesOffKeys.end());
    settings.cameraPreviewFullQuality = cameraController.getPreviewQuality() == PreviewQuality::Full;
    settings.cameraTileScale = cameraTileScale;
    settings.combineVideoAndAudio = combineVideoAndAudio;
    settings.checkForUpdates = checkForUpdates;
    settings.lastUpdateCheckSeconds = lastUpdateCheckSeconds;
    settings.deliveryTarget = deliveryTarget.toStdString();
    settings.sampleRateOverride = sampleRateOverride;
    settings.bitDepthOverride = currentBitDepth == 24 ? 0 : currentBitDepth;
    settings.bufferSizeOverride = bufferSizeOverride;

    for (const auto& entry : portIdentityStore.all())
        settings.ports.push_back ({ entry.first, entry.second });

    for (const auto& device : deviceManager.getDevices())
        if (! device.userEnabled)
            settings.disabledMicKeys.push_back (device.identity.key());

    // And every mic the user switched off that is not plugged in right now,
    // as the cameras below already do: §2.4 port memory, for the one setting
    // that used to be forgotten the moment the mic was unplugged.
    for (const auto& key : rememberedSettings.disabledMicKeys)
    {
        bool enumerated = false;
        for (const auto& device : deviceManager.getDevices())
            if (device.identity.key() == key) { enumerated = true; break; }

        if (! enumerated)
            settings.disabledMicKeys.push_back (key);
    }

    // Every known camera, not only the connected ones: a camera unplugged
    // today should come back tomorrow as it was left, while changing the
    // toggle on its unavailable row must replace (not resurrect) old state.
    const auto& selection = cameraController.getSelection();
    for (const auto& camera : selection.getKnownCameras())
        settings.cameras.push_back ({ camera.id,
                                      selection.isEnabled (camera.id),
                                      selection.getDisplayName (camera.id),
                                      cameraQualityKey (selection.getQuality (camera.id)) });

    const auto file = getSettingsFile();
    file.getParentDirectory().createDirectory();

    // Checked, because silently failing here presents next launch as an app
    // that forgot the user's microphone names, trims and destination -- and
    // there is no moment at which that is explained.
    if (! replaceWithTextChecked (file, juce::String (settings.toJsonString())))
        noteActivity (ActivityLevel::Warning, "Settings",
                      "Couldn't save your settings, so they may not be remembered next time.");

    rememberedSettings = settings;
}

juce::File Application::getTemplatesFolder()
{
    return getSupportFolder().getChildFile ("Templates");
}

void Application::sweepAbandonedSafeWriteTemps()
{
    const auto sweep = [] (const juce::File& folder, const std::string& stem, const std::string& extension)
    {
        if (! folder.isDirectory())
            return;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, ".*", juce::File::findFiles))
            if (isAbandonedSafeWriteTemp (entry.getFile().getFileName().toStdString(), stem, extension))
                entry.getFile().deleteFile();
    };

    const auto support = getSupportFolder();
    sweep (support, getSettingsFile().getFileNameWithoutExtension().toStdString(),
           getSettingsFile().getFileExtension().toStdString());
    sweep (support, "camera-starting", ".txt");

    // Every show is written this way, under its own name.
    sweep (getTemplatesFolder(), std::string(), ".json");
}

std::vector<ShowTemplate::StoredFile> Application::scanTemplateFiles()
{
    std::vector<ShowTemplate::StoredFile> files;

    for (const auto& entry : juce::RangedDirectoryIterator (getTemplatesFolder(), false, "*.json",
                                                            juce::File::findFiles))
    {
        const auto file = entry.getFile();

        // A hidden file is not a show this app saved. The one that turned up
        // here was a save's own temporary file, stranded by a crash before its
        // rename: read back, it was offered in the show list as a second copy
        // of the show, or under its temporary name.
        if (isSystemClutterFile (file.getFileName().toStdString()))
            continue;
        const auto loaded = ShowTemplate::fromJsonString (file.loadFileAsString().toStdString());

        // A file that is not a template is still recorded, so saving never
        // picks its name; it is just never offered as a show.
        files.push_back ({ file.getFileName().toStdString(),
                           loaded.has_value() ? std::optional<std::string> (loaded->name) : std::nullopt });
    }

    return files;
}

juce::File Application::getTemplateFile (const juce::String& name)
{
    // Found by the name stored in the file, not by sanitizing `name`: "Live
    // stage" and "Live-stage" sanitize alike, and the second lives in
    // "Live-stage-2.json".
    const auto fileName = ShowTemplate::findFileFor (name.toStdString(), scanTemplateFiles());
    return fileName.empty() ? juce::File() : getTemplatesFolder().getChildFile (juce::String (fileName));
}

juce::StringArray Application::listTemplates() const
{
    juce::StringArray names;

    for (const auto& show : ShowTemplate::listShows (scanTemplateFiles()))
        names.add (juce::String (show.first));

    names.sortNatural();
    return names;
}

bool Application::saveTemplate (const juce::String& name)
{
    const auto clean = juce::String (ShowTemplate::cleanName (name.toStdString()));

    // Its own file when the show was saved before, so it is replaced; never
    // the file of a different show whose name sanitizes the same way. Sanitized
    // by the same §6.2 rule as a session folder, so a typed name can never
    // point outside Templates/ or carry a character the disk refuses.
    const auto fileName = ShowTemplate::fileNameForSaving (clean.toStdString(), scanTemplateFiles());
    const auto file = fileName.empty() ? juce::File()
                                       : getTemplatesFolder().getChildFile (juce::String (fileName));

    if (file == juce::File())
    {
        noteActivity (ActivityLevel::Warning, "Shows",
                      "A show needs a name with at least one letter or number in it.");
        return false;
    }

    // Brings rememberedSettings up to the rig as it stands, so what is saved
    // is what the user is looking at rather than what was true at launch.
    saveSettings();

    std::vector<std::string> connected;
    for (const auto& device : deviceManager.getDevices())
        connected.push_back (device.identity.key());

    const auto show = ShowTemplate::extract (clean.toStdString(), rememberedSettings, connected);

    file.getParentDirectory().createDirectory();

    if (! replaceWithTextChecked (file, juce::String (show.toJsonString())))
    {
        noteActivity (ActivityLevel::Failed, "Shows",
                      "Couldn't save the show \"" + clean + "\", so it won't be in the list.");
        return false;
    }

    noteActivity (ActivityLevel::Started, "Shows",
                  "Saved this setup as \"" + clean + "\".");
    return true;
}

bool Application::applyTemplate (const juce::String& name)
{
    // §6.5 fixes a take's microphones, names and destination until it stops,
    // and half of a show applying now and half at Stop is not a show at all.
    if (isRecording() || (capture != nullptr && capture->isRecording()))
    {
        noteActivity (ActivityLevel::Warning, "Shows",
                      "Can't load \"" + name + "\" while a take is being recorded. Stop first, "
                      "then load it.");
        return false;
    }

    const auto file = getTemplateFile (name);
    const auto loaded = file.existsAsFile()
                            ? ShowTemplate::fromJsonString (file.loadFileAsString().toStdString())
                            : std::nullopt;

    if (! loaded.has_value())
    {
        noteActivity (ActivityLevel::Failed, "Shows",
                      "Couldn't read the saved show \"" + name + "\", so nothing was changed.");
        return false;
    }

    // Laid over the rig as it stands right now, not as it was at launch, so
    // anything the show does not mention stays exactly as the user left it.
    saveSettings();
    rememberedSettings = loaded->applyTo (rememberedSettings);
    const auto& s = rememberedSettings;

    const bool rateChanged = s.sampleRateOverride != sampleRateOverride;
    const bool destinationChanged = s.destinationFolder != destinationFolder;

    // The same assignments loadSettings() makes at launch, guarded the same
    // way so no intermediate state is written back over the file.
    applyingRememberedSettings = true;

    askWhereToSaveEveryTime = s.askWhereToSaveEveryTime;
    mirrorPolicy.setEnabledByUser (s.mirrorEnabled);
    masterVolume = s.masterVolume;
    rememberedOutputDeviceId = s.rememberedOutputDeviceId;
    monitorThroughCombinedDevice = s.monitorThroughCombinedDevice;
    headphonesOffKeys = { s.headphonesOffKeys.begin(), s.headphonesOffKeys.end() };
    combineVideoAndAudio = s.combineVideoAndAudio;
    deliveryTarget = juce::String (s.deliveryTarget);
    sampleRateOverride = s.sampleRateOverride;

    // 0 is the default depth, which is 24 -- saveSettings() writes 24 as 0.
    if (s.bitDepthOverride == 16 || s.bitDepthOverride == 24 || s.bitDepthOverride == 32)
        currentBitDepth = s.bitDepthOverride;
    else if (s.bitDepthOverride == 0)
        currentBitDepth = 24;

    if (auto* bus = getMonitorBus())
        bus->setMasterVolume (masterVolume);

    // §2.4: back into the store every path already reads names and trims from.
    for (const auto& port : s.ports)
    {
        PortIdentity id;
        id.locationId = port.key;
        portIdentityStore.put (id, port.settings);
    }

    // Both ways, unlike applyRememberedDeviceSettings(): at launch nothing is
    // switched off yet, but a show can have someone back on tonight.
    for (const auto& device : deviceManager.getDevices())
    {
        const auto key = device.identity.key();
        const bool wantEnabled = ! s.isMicDisabled (key);

        if (device.userEnabled != wantEnabled)
            deviceManager.setUserEnabled (key, wantEnabled);
    }

    // Only the cameras the show names, and only where the answer differs:
    // switching one on through the user's path also clears a crash guard, and
    // that is not something to do to a camera the show never mentioned.
    for (const auto& saved : loaded->cameras)
    {
        // A show saved before cameras had per-device ids names them the old
        // way; follow those choices to the device that took them over.
        auto camera = saved;
        camera.id = cameraController.getSelection().resolveId (saved.id);

        if (cameraController.getSelection().isEnabled (camera.id) != camera.enabled)
            cameraController.setCameraEnabledByUser (camera.id, camera.enabled);

        cameraController.setCameraQuality (camera.id, cameraQualityFromKey (camera.quality));

        if (! camera.assignedName.empty())
            cameraController.getSelection().setAssignedName (camera.id, camera.assignedName);
    }

    applyingRememberedSettings = false;

    // Then the live rig catches up, through the same paths the Settings
    // controls take. The destination goes through its own checks rather than
    // being assigned, so a card that has gone away is found before a take.
    if (destinationChanged && ! s.destinationFolder.empty())
        setDestinationFolder (juce::File (juce::String (s.destinationFolder)));

    headphoneJacks.lay();
    reselectOutputDevice();

    // A new rate reopens the streams through the hot-plug path, as the rate
    // picker does; otherwise a rebuild is enough to pick up names, trims and
    // which microphones are on.
    if (rateChanged)
        onDeviceListChanged();
    else
        restartCapture();

    saveSettings();

    noteActivity (ActivityLevel::Started, "Shows", "Loaded \"" + name + "\".");
    return true;
}

bool Application::deleteTemplate (const juce::String& name)
{
    const auto file = getTemplateFile (name);

    if (! file.existsAsFile())
        return false;

    if (! file.deleteFile())
    {
        noteActivity (ActivityLevel::Failed, "Shows",
                      "Couldn't delete the saved show \"" + name + "\".");
        return false;
    }

    noteActivity (ActivityLevel::Stopped, "Shows", "Deleted the saved show \"" + name + "\".");
    return true;
}

juce::File Application::getLogFile()
{
    return getSupportFolder().getChildFile ("log.txt");
}

bool Application::supportFolderMigrationFailed = false;

juce::File Application::getSupportFolder()
{
    const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    const auto current = appData.getChildFile ("SobStage");

    // The app was called something else before, and everything §2.4 remembers
    // about a rig -- every microphone's name and trim, which ones are switched
    // off, the destination folder, the backup setting -- lives in this folder
    // under that old name. A rename that simply looked somewhere new would
    // present as every one of those being forgotten, which is precisely the
    // §10.1 failure the settings file exists to prevent: being asked the same
    // questions again.
    //
    // So the old folder is moved across, once, the first time the new name is
    // used. Moved rather than copied: two folders would then drift, and the
    // one being written to is not the one a user going looking would find.
    if (! current.isDirectory())
    {
        const auto previous = appData.getChildFile ("MultiMicAggregator");

        // The result is kept. A failed move presents as every remembered
        // microphone name, trim, disabled mic and destination folder being
        // forgotten at once -- the exact §10.1 failure this migration exists to
        // prevent -- and it used to happen without a word.
        if (previous.isDirectory() && ! previous.moveFileTo (current))
            supportFolderMigrationFailed = true;
    }

    return current;
}

namespace {
struct NewestFirst
{
    static int compareElements (const juce::File& a, const juce::File& b)
    {
        const auto ta = a.getLastModificationTime();
        const auto tb = b.getLastModificationTime();
        return ta > tb ? -1 : (ta < tb ? 1 : 0);
    }
};
} // namespace

void Application::clearRecoveredSessions()
{
    publishCompletedRecoveryAcknowledgement();

    std::vector<std::string> sessionFolders;
    sessionFolders.reserve (recoveredSessions.size() + hiddenRecoveredFolders.size());

    for (const auto& session : recoveredSessions)
        sessionFolders.push_back (session.folder);

    // The same-named copies the card never showed are just as interrupted.
    for (const auto& folder : hiddenRecoveredFolders)
        sessionFolders.push_back (folder);

    std::vector<std::string> mutationRoots;
    mutationRoots.reserve (sessionFolders.size());
    for (const auto& folder : sessionFolders)
        mutationRoots.push_back (
            juce::File (juce::String (folder)).getParentDirectory()
                .getFullPathName().toStdString());

    // Dismiss the card before touching any path it names. A recovered take may
    // live on the same removable volume whose disappearance interrupted it;
    // even existsAsFile() or loadFileAsString() can then wait indefinitely.
    recoveredSessions.clear();
    hiddenRecoveredFolders.clear();

    if (sessionFolders.empty())
        return;

    // The rule for "interrupted" is an empty stop timestamp, and repairing
    // the headers never wrote one -- so the same takes came back at every
    // launch. The worker stamps them with this acknowledgement time. It owns
    // every folder string and never captures Application; a stuck OS call can
    // therefore outlive this object without holding up the UI or shutdown.
    const auto recoveredAt = juce::Time::getCurrentTime().toISO8601 (true).toStdString();
    const auto mutationGate = recoveryMutationGate;
    const bool launched = recoveryAcknowledgementTask.start (
        [sessionFolders = std::move (sessionFolders), recoveredAt,
         mutationRoots = std::move (mutationRoots), mutationGate]
        (const std::atomic<bool>& cancelled) mutable
        {
            auto mutationLease = waitForMutationLease (
                mutationGate, mutationRoots, cancelled);
            if (mutationLease == nullptr)
                return RecoveryAcknowledgementResult {};

            auto result = Application::runRecoveryAcknowledgement (
                std::move (sessionFolders), recoveredAt, cancelled);
            mutationLease.reset();
            return result;
        });

    if (! launched)
        noteActivity (ActivityLevel::Warning, "Interrupted take",
                      "Couldn't mark the recovered takes as dealt with. No recording was "
                      "changed, so they may be offered again next time SobStage opens.");
}

Application::RecoveryAcknowledgementResult Application::runRecoveryAcknowledgement (
    std::vector<std::string> sessionFolders, std::string recoveredAt,
    const std::atomic<bool>& cancelled)
{
    RecoveryAcknowledgementResult result;

    const auto wasCancelled = [&cancelled]
    {
        return cancelled.load (std::memory_order_acquire);
    };

    for (const auto& folderPath : sessionFolders)
    {
        if (wasCancelled())
            break;

        const auto file = juce::File (juce::String (folderPath)).getChildFile ("session.json");

        // Every filesystem call stays on this detached worker. Check
        // cancellation after each one so a worker released from a stale mount
        // after shutdown cannot proceed to the next read or mutate a file.
        if (! file.existsAsFile() || wasCancelled())
            continue;

        try
        {
            // A read that failed is not an empty record. Parsed as one, it had
            // no stop time, was stamped, and a near-empty record was renamed
            // over the real one -- which a rename can do to an iCloud
            // placeholder this Mac could not read offline. Left exactly as it
            // is, and offered again. A file that WAS read and is empty or cut
            // short is still stamped below, or a take whose record a power
            // cut emptied would come back at every launch.
            const auto bytes = SessionRecovery::readWholeFileChecked (
                file.getFullPathName().toStdString());

            if (wasCancelled())
                break;

            if (! bytes.has_value())
            {
                result.activity.push_back (
                    { ActivityLevel::Warning,
                      "Interrupted take",
                      (file.getParentDirectory().getFileName()
                       + " wasn't marked as dealt with, because its details file couldn't be "
                         "read just now. Nothing was changed, so it will be offered again next "
                         "time.").toStdString() });
                continue;
            }

            const auto contents = juce::String::createStringFromData (
                                      bytes->data(), static_cast<int> (bytes->size())).toStdString();

            auto meta = SessionMetadata::fromJsonString (contents);

            if (meta.stopTimestampIso.empty())
            {
                meta.stopTimestampIso = recoveredAt;

                if (wasCancelled())
                    break;

                // Checked. This is the write that stops a recovered take being
                // offered again at every launch. A read-only/full/disappearing
                // card leaves the metadata untouched and the take is safely
                // offered again next launch.
                if (! replaceWithTextChecked (file, juce::String (meta.toJsonString()))
                    && ! wasCancelled())
                {
                    result.activity.push_back (
                        { ActivityLevel::Warning,
                          "Interrupted take",
                          (file.getParentDirectory().getFileName()
                           + " can't be marked as dealt with -- this card won't accept the "
                             "change, so it will be offered again next time.").toStdString() });
                }
            }
        }
        catch (...)
        {
            // Unreadable metadata stays as it is and is offered again. Do not
            // risk replacing a file whose contents could not be understood.
        }
    }

    return result;
}

void Application::publishCompletedRecoveryAcknowledgement() const
{
    auto completed = recoveryAcknowledgementTask.takeResult();

    if (! completed.has_value())
        return;

    // Only the message thread calls this publisher. The detached worker owns
    // plain result values and never reaches into the journal or Application.
    for (const auto& entry : completed->activity)
        noteActivity (entry.level, juce::String (entry.subject), juce::String (entry.message));
}

void Application::scanForInterruptedSessions()
{
    recoveredSessions.clear();
    hiddenRecoveredFolders.clear();

    // Separate tasks matter here. A destination can be replaced while its OS
    // call is stuck, and mirroring can be switched off to escape a stuck local
    // mirror probe. Neither root is allowed to hold the other behind one
    // worker, and neither detached worker ever captures Application.
    startDestinationRecoveryScan();
    startMirrorRecoveryScan();
}

void Application::startDestinationRecoveryScan()
{
    // A shared destination/mirror root may currently be represented by the
    // destination task alone. Consume a just-finished result before changing
    // either root so that safety fact can follow the mirror when the roots
    // split again.
    publishCompletedRecoveryScans();

    if (destinationFolder.empty())
    {
        destinationRecoveryStatus = RecoveryScanStatus::Failed;
        return;
    }

    const auto target = juce::File (juce::String (destinationFolder))
                            .getFullPathName().toStdString();

    if (target == destinationRecoveryRoot)
        return;

    const bool destinationOwnedSharedRoot = ! destinationRecoveryRoot.empty()
        && destinationRecoveryRoot == mirrorRecoveryRoot
        && destinationRecoveryStatus != RecoveryScanStatus::NotStarted
        && mirrorRecoveryStatus == RecoveryScanStatus::NotStarted;

    if (destinationOwnedSharedRoot)
    {
        // A finished scan proves the same root safe (or unsafe) for the mirror.
        // A running scan is about to be abandoned, so the mirror must launch
        // its own replacement below rather than waiting on a task it no longer
        // owns.
        mirrorRecoveryStatus = destinationRecoveryStatus == RecoveryScanStatus::Running
            ? RecoveryScanStatus::NotStarted
            : destinationRecoveryStatus;
    }

    if (! destinationRecoveryRoot.empty())
        destinationRecoveryTask.abandon();

    destinationRecoveryRoot = target;
    destinationRecoveryStatus = RecoveryScanStatus::NotStarted;
    destinationRecoveryRootWasMissing = false;

    // If the user's destination is the mirror root itself, the existing scan
    // already establishes the same safety fact. Do not race two header repairs
    // over one directory.
    if (target == mirrorRecoveryRoot
        && (mirrorRecoveryStatus != RecoveryScanStatus::NotStarted
            || mirrorRecoveryTask.isRunning()))
        return;

    const auto mutationGate = recoveryMutationGate;
    const bool sweepWorkingFiles = nothingIsWritingAfterATake();
    const bool launched = destinationRecoveryTask.start (
        [target, mutationGate, sweepWorkingFiles] (const std::atomic<bool>& cancelled) mutable
        {
            auto mutationLease = waitForMutationLease (
                mutationGate, { target }, cancelled);
            if (mutationLease == nullptr)
                return RecoveryBackgroundResult { target, true, {}, {} };

            auto result = Application::runRecoveryScan (target, true, sweepWorkingFiles, cancelled);
            mutationLease.reset();
            return result;
        });

    if (! launched)
    {
        destinationRecoveryStatus = RecoveryScanStatus::Failed;
        noteActivity (ActivityLevel::Warning, "Interrupted take",
                      "Couldn't start the interrupted-take check for this save location. "
                      "No files were changed; restart SobStage before recording there.");
    }
    else
    {
        destinationRecoveryStatus = RecoveryScanStatus::Running;
    }

    // If changing the destination split a once-shared root while its only scan
    // was still running, immediately give the mirror an independent task. The
    // old detached worker is cancelled and cannot publish, but is never joined.
    if (! mirrorRecoveryRoot.empty()
        && mirrorRecoveryRoot != destinationRecoveryRoot
        && mirrorRecoveryStatus == RecoveryScanStatus::NotStarted)
        startMirrorRecoveryScan();
}

bool Application::nothingIsWritingAfterATake() const
{
    // A take, a combined video and a podcast-ready copy are the only things
    // that write those working files. None of them may be running when a
    // scan is told it may remove one.
    return recordingEngine.getState() == RecordingState::Idle
        && ! takeCombiner.isRunning()
        && ! podcastExporter.isRunning()
        && ! cardCopier.isRunning()
        && ! cameraController.isFinalizingRecording();
}

void Application::rescanDestinationIfItHasAppeared()
{
    publishCompletedRecoveryScans();

    if (! destinationRecoveryRootWasMissing
        || destinationRecoveryStatus != RecoveryScanStatus::Succeeded
        || recordingEngine.getState() != RecordingState::Idle)
        return;

    // One task serves both when the destination is the backup root itself;
    // re-running only one side of that would leave the other waiting on it.
    if (destinationRecoveryRoot == mirrorRecoveryRoot)
        return;

    if (juce::File (juce::String (destinationFolder)).getFullPathName().toStdString()
            != destinationRecoveryRoot)
        return;

    // Evidence from the worker that is allowed to touch the drive, never a
    // stat on this thread: the drive check wrote its test file there, so the
    // location exists now. It re-checks a location it could not write on its
    // own, which is how a card that mounts late gets here.
    //
    // Only a verdict reached after the scan found nothing there counts. One
    // from before -- the card was there for the drive check and gone by the
    // scan -- proves nothing, and would rescan on every tick.
    publishCompletedPreflight();
    const auto verdict = preflightResults.find (destinationFolder);
    const auto verdictAt = preflightVerdictAtMs.find (destinationFolder);

    const bool drivesCheckSawIt = verdict != preflightResults.end() && ! verdict->second.couldNotWrite
                               && verdictAt != preflightVerdictAtMs.end()
                               && verdictAt->second > destinationRecoveryMissingAtMs;

    // With takes recorded on this computer and copied to the card, the drive
    // check never runs, so it can never say the card is back. The status
    // probe's own look between takes, begun after the miss, says it instead.
    const bool probeSawIt = FilesystemStatusProbe::showsDestinationAfter (
        filesystemStatusProbe.getSnapshot(), destinationFolder, destinationRecoveryMissingAt);

    if (! drivesCheckSawIt && ! probeSawIt)
        return;

    destinationRecoveryRootWasMissing = false;
    destinationRecoveryRoot.clear();
    destinationRecoveryStatus = RecoveryScanStatus::NotStarted;
    startDestinationRecoveryScan();
}

std::string Application::cardIdentityAtDestination() const
{
    // The probe's last look, between takes, at this destination: never a
    // filesystem call here, on the message thread, while Record is pressed.
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    if (! snapshot.ready || snapshot.request.destinationPath != destinationFolder
        || ! snapshot.request.sessionFolder.empty())
        return {};

    return snapshot.destinationCardIdentity;
}

std::optional<bool> Application::cardSeenAtDestination() const
{
    // The probe's last look between takes, at this destination: never a
    // filesystem call on the message thread. A card is there when it can be
    // told which card it is, or when the save location's folder is there
    // (only then is a room figure worked out).
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    if (! snapshot.ready || destinationFolder.empty() || snapshot.request.destinationPath != destinationFolder
        || ! snapshot.request.sessionFolder.empty())
        return std::nullopt;

    return ! snapshot.destinationCardIdentity.empty() || snapshot.remainingSeconds >= 0.0;
}

double Application::cardCopyRetryIntervalMs()
{
   #if defined (MMA_UI_WALK)
    // Test builds only: a gate that must tell a card seen back in from the
    // timed retry sets the timed one far off.
    if (const char* seconds = std::getenv ("MMA_CARD_RETRY_SECONDS"); seconds != nullptr && *seconds != '\0')
        return std::max (1.0, std::atof (seconds)) * 1000.0;
   #endif
    return 10000.0;
}

bool Application::probeSeesAnotherDriveThanChecked (const PreflightResult& verdict) const
{
    // Whatever the probe saw last; never a filesystem call here, because this
    // is asked by the record button's state on every status tick.
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    return snapshot.ready
        && snapshot.request.destinationPath == destinationFolder
        && snapshot.request.sessionFolder.empty()
        && PreflightThroughputTest::measuredAnotherDrive (verdict, snapshot.destinationVolume,
                                                          snapshot.sampleStartedAt);
}

void Application::recheckDestinationIfItsDriveChanged()
{
    if (destinationFolder.empty() || recordingEngine.getState() != RecordingState::Idle)
        return;

    publishCompletedPreflight();
    const auto verdict = preflightResults.find (destinationFolder);

    if (verdict == preflightResults.end())
        return;

    // Keeps the probe looking at the save location between takes even when
    // nothing else on screen is asking about it.
    filesystemStatusProbe.setRequest (currentFilesystemProbeRequest());

    if (! probeSeesAnotherDriveThanChecked (verdict->second))
        return;

    forgetCheckedDriveAtDestination();
}

void Application::forgetCheckedDriveAtDestination()
{
    noteActivity (ActivityLevel::Warning, "Save location",
                  "The drive at " + juce::String (destinationFolder) + " isn't the one that was "
                  "checked (it was taken out, or another was put in), so it is being checked "
                  "again before the next take.");

    // Exactly what choosing the location again does (applyDestinationFolder):
    // the card now at this path has its own interrupted takes, speed and
    // format. Workers stuck on the old card are abandoned, never joined.
    if (destinationRecoveryRoot == destinationFolder)
    {
        destinationRecoveryTask.abandon();
        destinationRecoveryRoot.clear();
        destinationRecoveryStatus = RecoveryScanStatus::NotStarted;
    }

    startDestinationRecoveryScan();

    preflightResults.erase (destinationFolder);
    preflightVerdictAtMs.erase (destinationFolder);
    if (preflightTaskDestination == destinationFolder)
    {
        preflightTask.abandon();
        preflightTaskDestination.clear();
    }

    beginPreflightForDestination();
}

juce::String Application::confirmDestinationIsTheCheckedDrive()
{
    const auto verdict = preflightResults.find (destinationFolder);

    // A platform that can't tell one volume from another (Windows), or a
    // verdict that measured nothing, has nothing to compare.
    if (verdict == preflightResults.end() || ! verdict->second.measuredVolume.known)
        return {};

    // Under the same deadline as the other card steps at Record: a card that
    // has stopped answering holds a statfs as long as it holds anything else.
    const std::string destination = destinationFolder;
    const auto now = runWithDeadline<VolumeFingerprint> ([destination]
    {
        return volumeFingerprintOf (destination);
    }, kRemovableVolumeDeadline);

    if (! now.has_value())
    {
        // The same account, and the only one, that the folder step below
        // would have given after the same wait.
        const auto problem = "The card at " + juce::String (destinationFolder) + " stopped answering, "
                             "so the take couldn't start. Check the card is plugged in, or choose "
                             "somewhere else.";
        noteActivity (ActivityLevel::Failed, "Recording", problem);
        return problem;
    }

    if (! volumesDiffer (verdict->second.measuredVolume, *now))
        return {};

    forgetCheckedDriveAtDestination();
    return "A different drive is at this save location now. Checking it is fast enough...";
}

void Application::startMirrorRecoveryScan()
{
    const auto target = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("RECORDINGS-MIRROR")
                            .getFullPathName().toStdString();

    if (target == mirrorRecoveryRoot)
    {
        if (mirrorRecoveryStatus != RecoveryScanStatus::NotStarted
            || target == destinationRecoveryRoot)
            return;
    }
    else
    {
        if (! mirrorRecoveryRoot.empty())
            mirrorRecoveryTask.abandon();

        mirrorRecoveryRoot = target;
        mirrorRecoveryStatus = RecoveryScanStatus::NotStarted;
    }

    if (target == destinationRecoveryRoot)
        return;

    const auto mutationGate = recoveryMutationGate;
    const bool sweepWorkingFiles = nothingIsWritingAfterATake();
    const bool launched = mirrorRecoveryTask.start (
        [target, mutationGate, sweepWorkingFiles] (const std::atomic<bool>& cancelled) mutable
        {
            auto mutationLease = waitForMutationLease (
                mutationGate, { target }, cancelled);
            if (mutationLease == nullptr)
                return RecoveryBackgroundResult { target, false, {}, {} };

            auto result = Application::runRecoveryScan (target, false, sweepWorkingFiles, cancelled);
            mutationLease.reset();
            return result;
        });

    if (! launched)
    {
        mirrorRecoveryStatus = RecoveryScanStatus::Failed;
        noteActivity (ActivityLevel::Warning, "Interrupted take",
                      "Couldn't start the interrupted-take check for the local backup folder. "
                      "No files were changed; restart SobStage before using local backup.");
    }
    else
    {
        mirrorRecoveryStatus = RecoveryScanStatus::Running;
    }
}

Application::RecoveryBackgroundResult Application::runRecoveryScan (
    std::string rootPath, bool isDestinationCopy, bool sweepWorkingFiles,
    const std::atomic<bool>& cancelled)
{
    RecoveryBackgroundResult result;
    result.root = std::move (rootPath);
    result.isDestinationCopy = isDestinationCopy;

    const auto wasCancelled = [&cancelled]
    {
        return cancelled.load (std::memory_order_acquire);
    };

    const auto report = [&result] (ActivityLevel level, juce::String message)
    {
        result.activity.push_back ({ level, "Interrupted take", message.toStdString() });
    };

    // A constant of the platform's camera code: no device is asked.
    const auto movieExtension = CameraController::getMovieFileExtension().toStdString();

    if (wasCancelled())
        return result;

    const juce::File root { juce::String (result.root) };
    if (wasCancelled())
        return result;

    // Not there is not the same as checked. After a power cut or a kernel
    // panic macOS checks a card before mounting it, and that can take minutes;
    // a scan that ran first found nothing and called the card clear, so the
    // take the crash interrupted was never repaired or offered, and new takes
    // were allowed in beside it. Said, so the scan can be run again once the
    // location really is there.
    if (! root.isDirectory())
    {
        result.rootMissing = true;
        return result;
    }

    if (wasCancelled())
        return result;

    // One level down and newest first. A card can hold hundreds of takes; an
    // interrupted take is, by definition, among the most recent events.
    juce::Array<juce::File> folders;
    root.findChildFiles (folders, juce::File::findDirectories, false);

    if (wasCancelled())
        return result;

    NewestFirst comparator;
    folders.sort (comparator);

    if (wasCancelled())
        return result;

    constexpr int kMaxFoldersExamined = 20;
    const int examine = juce::jmin (folders.size(), kMaxFoldersExamined);

    // Takes whose details file is there but could not be read just now.
    int couldNotReadNow = 0;

    for (int i = 0; i < examine && ! wasCancelled(); ++i)
    {
        const auto folder = folders[i];

        const auto metadataFile = folder.getChildFile ("session.json");

        if (! metadataFile.existsAsFile() || wasCancelled())
            continue;

        // Read with a check, because a failed read and an empty file used to
        // look the same here. An iCloud "Optimize Mac Storage" placeholder
        // opened offline (or a card that hiccups) gave an empty string, which
        // made a finished take look interrupted -- and then its real record was
        // overwritten when it was dismissed, or straight away when its audio
        // could not be read either. Nothing in such a folder is touched: it is
        // skipped and looked at again next launch.
        const auto bytes = SessionRecovery::readWholeFileChecked (
            metadataFile.getFullPathName().toStdString());

        if (wasCancelled())
            return result;

        if (! bytes.has_value())
        {
            ++couldNotReadNow;
            continue;
        }

        SessionMetadata meta;

        // A session.json truncated by the same power cut that interrupted the
        // take is not a reason to fail: the audio beside it is still worth
        // recovering, so one that was read but says nothing is treated as
        // interrupted.
        bool metadataUnreadable = false;

        try
        {
            // Through juce::String as loadFileAsString did, so a byte-order
            // mark is handled the same way it always was.
            const auto text = juce::String::createStringFromData (
                                  bytes->data(), static_cast<int> (bytes->size())).toStdString();

            const auto parsed = JsonValue::parse (text);

            // The parser is lenient by design and hardly ever throws: a file
            // cut short mid-key comes back with that member dropped, possibly
            // leaving none. Judge it by whether a field carried a value.
            if (parsed.getType() != JsonValue::Type::Object
                || parsed.getValuedMemberCount() == 0)
            {
                meta = {};
                metadataUnreadable = true;
            }
            else
            {
                meta = SessionMetadata::fromJson (parsed);
            }
        }
        catch (...)
        {
            meta = {};
            metadataUnreadable = true;
        }

        if (wasCancelled())
            return result;

        if (metadataUnreadable)
            report (ActivityLevel::Warning,
                    folder.getFileName()
                    + " has a details file this app can't read, so what it says about "
                      "that take is gone. Its audio is still in that folder.");

        // What a hard kill left of the combined movie or the podcast-ready
        // copy being made after this take. Those are made after the take has
        // stopped, so this folder is usually NOT an interrupted one. Only
        // when nothing could be writing them now -- the caller decides that
        // on the message thread -- and only by their own working names.
        if (sweepWorkingFiles)
        {
            int removed = 0;

            for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFiles))
            {
                if (wasCancelled())
                    return result;

                if (isAbandonedTakeWorkingFile (entry.getFile().getFileName().toStdString())
                    && entry.getFile().deleteFile())
                    ++removed;
            }

            if (removed > 0)
                report (ActivityLevel::Warning,
                        folder.getFileName()
                        + ": SobStage stopped while it was still making the combined video or "
                          "podcast-ready copy of this take, and the unfinished file has been "
                          "removed. The take itself is untouched.");
        }

        if (! SessionRecovery::sessionWasInterrupted (meta))
            continue;

        RecoveredSession session;
        session.folder = folder.getFullPathName().toStdString();
        session.startedIso = meta.startTimestampIso;
        session.mirrorFolder = meta.mirrorPath;
        session.modifiedMs = folder.getLastModificationTime().toMilliseconds();
        // The backup was on and had stopped being written by the time the
        // record was last refreshed: that copy ends early, so it is never
        // offered in place of a card copy that could not be repaired.
        session.backupCopyCutShort = meta.mirrorEnabled && ! meta.mirrorActive
                                  && ! meta.mirrorPath.empty();

        // The hidden half-written copy a crash left mid-replace of the take's
        // session.json or activity.log. Its target still holds the previous
        // version, so it is never the only copy of anything -- and on a card
        // later opened on Windows it is a stray file in the take.
        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, ".*", juce::File::findFiles))
        {
            const auto name = entry.getFile().getFileName().toStdString();

            if (isAbandonedSafeWriteTemp (name, "session", ".json")
                || isAbandonedSafeWriteTemp (name, "activity", ".log"))
                entry.getFile().deleteFile();
        }

        if (wasCancelled())
            return result;

        // Camera movies are not repaired -- there is nothing in them this app
        // can fix -- but the card must not leave them out of its account. In
        // the container this computer's cameras write as well as the others:
        // a Windows camera's .wmv was never counted, so a Windows take's
        // recovery row never mentioned its movies.
        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFiles))
            SessionRecovery::countCameraMovie (session, entry.getFile().getFileName().toStdString(),
                                               movieExtension);

        if (wasCancelled())
            return result;

        for (const auto& entry : juce::RangedDirectoryIterator (
                 folder, false, "*.wav", juce::File::findFiles))
        {
            if (wasCancelled())
                return result;

            // "._01_Alice.wav" on an ExFAT card is macOS's AppleDouble twin of
            // the stem, not a recording: it has no RIFF header, so it came back
            // as an "empty file left alone" -- one phantom lost track per stem.
            if (isSystemClutterFile (entry.getFile().getFileName().toStdString()))
                continue;

            session.files.push_back (SessionRecovery::repairWavFile (
                entry.getFile().getFullPathName().toStdString()));

            if (wasCancelled())
                return result;
        }

        std::sort (session.files.begin(), session.files.end(),
                   [] (const RecoveredFile& a, const RecoveredFile& b)
                   {
                       return a.fileName < b.fileName;
                   });

        for (const auto& file : session.files)
            if (file.repairFailed)
                report (ActivityLevel::Warning,
                        juce::String (file.fileName)
                        + " couldn't be opened or repaired -- this card wouldn't accept "
                          "the fix. Copy it somewhere else before playing it.");

        if (session.isWorthPresenting())
        {
            // The playable count, not every file in the folder. The headline
            // used to announce files the warning directly above it had just
            // said could not be opened.
            const int playable = session.playableFileCount();

            if (playable > 0)
                report (ActivityLevel::Recovered,
                        folder.getFileName()
                        + " was interrupted, and "
                        + juce::String (playable)
                        + (playable == 1 ? " of its files has" : " of its files have")
                        + " been repaired and can be played.");
            else
                report (ActivityLevel::Warning,
                        folder.getFileName()
                        + " was interrupted, and none of its files could be repaired. "
                          "Copy them somewhere else before trying to play them.");
            result.sessions.push_back (std::move (session));
        }
        else
        {
            report (ActivityLevel::Failed,
                    folder.getFileName()
                    + " was interrupted and nothing playable survived in it. There is "
                      "nothing to recover from that folder.");

            // Marked as dealt with now. Nothing is offered for it, so there is
            // no card whose Done could do it -- and unmarked, this same failure
            // was announced again at every launch for as long as the folder
            // stayed among the newest. A card that refuses the write leaves it
            // as it was, to be said again next time, which is no worse.
            meta.stopTimestampIso = juce::Time::getCurrentTime().toISO8601 (true).toStdString();

            if (! wasCancelled())
                (void) replaceWithTextChecked (metadataFile, juce::String (meta.toJsonString()));
        }
    }

    // One line for all of them: a folder of takes iCloud has moved off this
    // Mac would otherwise be twenty warnings at every offline launch.
    if (couldNotReadNow > 0 && ! wasCancelled())
    {
        const bool one = couldNotReadNow == 1;

        report (ActivityLevel::Warning,
                (one ? juce::String ("One recent take") : juce::String (couldNotReadNow) + " recent takes")
                + " in " + root.getFileName()
                + (one ? " wasn't" : " weren't")
                + " checked for an interruption, because "
                + (one ? "its details file" : "their details files")
                + " couldn't be read just now. Nothing was changed. If "
                + (one ? "it's" : "they're")
                + " kept in iCloud or another online storage service, connect to the internet. "
                  "SobStage will check again next time it opens.");
    }

    return result;
}

void Application::publishCompletedRecoveryScans() const
{
    const auto publish = [this] (DetachedResultTask<RecoveryBackgroundResult>& task,
                                 const std::string& expectedRoot,
                                 RecoveryScanStatus& status,
                                 const juce::String& failureMessage)
    {
        if (status != RecoveryScanStatus::Running)
            return;

        auto completed = task.takeResult();

        // The worker publishes its result and clears `running` under the same
        // mutex. If the first read arrived just before that publication and
        // the second sees the worker finished, one final result read closes
        // that narrow race before a missing result is treated as failure.
        if (! completed.has_value())
        {
            if (task.isRunning())
                return;

            completed = task.takeResult();

            if (! completed.has_value())
            {
                status = RecoveryScanStatus::Failed;
                noteActivity (ActivityLevel::Warning, "Interrupted take", failureMessage);
                return;
            }
        }

        if (completed->root != expectedRoot)
        {
            status = RecoveryScanStatus::Failed;
            noteActivity (ActivityLevel::Warning, "Interrupted take", failureMessage);
            return;
        }

        status = RecoveryScanStatus::Succeeded;

        if (&status == &destinationRecoveryStatus)
        {
            destinationRecoveryRootWasMissing = completed->rootMissing;
            destinationRecoveryMissingAtMs = juce::Time::getMillisecondCounterHiRes();
            destinationRecoveryMissingAt = std::chrono::steady_clock::now();
        }

        for (const auto& entry : completed->activity)
            noteActivity (entry.level, juce::String (entry.subject),
                          juce::String (entry.message));

        // A take mirrored to the local backup is found twice. It is shown
        // once, and the hidden copy's folder is kept so that dismissing the
        // card stamps it too -- otherwise it came back at every launch.
        std::set<std::string> standingInBefore;
        for (const auto& session : recoveredSessions)
            if (session.shownBecauseCardCopyUnrepairable)
                standingInBefore.insert (session.folder);

        auto merged = SessionRecovery::mergeScan (
            { std::move (recoveredSessions), std::move (hiddenRecoveredFolders) },
            std::move (completed->sessions), completed->isDestinationCopy);
        recoveredSessions = std::move (merged.shown);
        hiddenRecoveredFolders = std::move (merged.hiddenFolders);

        // The card offers the backup copy of a take whose card copy the card
        // would not let be repaired. Said in the log as well as on the card,
        // with where it is: the card is gone once dismissed, and the log is
        // what someone helping reads afterwards.
        for (const auto& session : recoveredSessions)
            if (session.shownBecauseCardCopyUnrepairable && standingInBefore.count (session.folder) == 0)
                noteActivity (ActivityLevel::Warning, "Interrupted take",
                              juce::File (juce::String (session.folder)).getFileName()
                              + ": the card's copy couldn't be repaired, so the local backup copy is "
                                "the one offered -- it is in " + juce::String (session.folder) + ".");
    };

    publish (mirrorRecoveryTask, mirrorRecoveryRoot, mirrorRecoveryStatus,
             "The interrupted-take check for the local backup folder stopped unexpectedly. "
             "No files were changed; turn off local backup or restart SobStage before recording.");
    publish (destinationRecoveryTask, destinationRecoveryRoot, destinationRecoveryStatus,
             "The interrupted-take check for this save location stopped unexpectedly. "
             "No files were changed; choose another save location or restart SobStage before recording there.");
}

juce::String Application::recoveryBlockingReason() const
{
    publishCompletedRecoveryScans();

    const auto hasNonPreflightMutation = [this] (const std::string& root)
    {
        if (! recoveryMutationGate.isActive (root))
            return false;

        // The speed benchmark deliberately shares this mutation gate with
        // recovery. Once recovery itself has succeeded, that current worker
        // is reported by the preflight-specific status below rather than as a
        // misleading interrupted-take scan. An abandoned preflight has a fresh
        // task state (not running) and therefore still blocks here until its
        // old worker and lease really return.
        return ! (preflightTaskDestination == root && preflightTask.isRunning());
    };

    const bool rootsAreShared = ! destinationRecoveryRoot.empty()
        && destinationRecoveryRoot == mirrorRecoveryRoot;

    const auto sharedStatus = [&]
    {
        // Exactly one task is launched when the save location and local backup
        // are the same directory. Whichever side owns that task establishes
        // the safety fact for both roots.
        if (destinationRecoveryStatus != RecoveryScanStatus::NotStarted)
            return destinationRecoveryStatus;

        return mirrorRecoveryStatus;
    };

    // A take recorded on this computer is written under RECORDINGS-MIRROR
    // only, so that is the one check it waits for. The card's own check goes
    // on beside it and blocks nothing: the copy to the card is held until it
    // is done (syncAfterTakeHolds).
    if (wouldRecordOnComputer())
    {
        const auto computerStatus = rootsAreShared ? sharedStatus() : mirrorRecoveryStatus;

        if (computerStatus == RecoveryScanStatus::Failed)
            return "Couldn't check the recordings folder on this computer for interrupted takes. "
                   "Restart SobStage before recording.";

        if (computerStatus != RecoveryScanStatus::Succeeded)
            return "Checking the recordings folder on this computer for an interrupted take...";

        if (hasNonPreflightMutation (mirrorRecoveryRoot))
            return "Waiting for earlier work on the recordings folder on this computer to finish.";

        return {};
    }

    const auto destinationStatus = rootsAreShared ? sharedStatus()
                                                   : destinationRecoveryStatus;

    if (destinationStatus == RecoveryScanStatus::Failed)
        return "Couldn't check this save location for interrupted takes. Choose another save "
               "location or restart SobStage before recording here.";

    if (destinationStatus != RecoveryScanStatus::Succeeded)
        return "Checking this save location for an interrupted take...";

    if (hasNonPreflightMutation (destinationRecoveryRoot))
        return "Waiting for earlier work on this save location to finish. Choose another "
               "location if this drive is no longer responding.";

    if (! mirrorPolicy.isEnabledByUser())
        return {};

    const auto enabledMirrorStatus = rootsAreShared ? sharedStatus()
                                                    : mirrorRecoveryStatus;

    if (enabledMirrorStatus == RecoveryScanStatus::Failed)
        return "Couldn't check the local backup folder for interrupted takes. Turn off local "
               "backup or restart SobStage before recording.";

    if (enabledMirrorStatus != RecoveryScanStatus::Succeeded)
        return "Checking the local backup folder for an interrupted take...";

    if (hasNonPreflightMutation (mirrorRecoveryRoot))
        return "Waiting for earlier work on the local backup folder to finish. Turn off local "
               "backup if that drive is no longer responding.";

    return {};
}

bool Application::isRecoveryScanPending() const
{
    publishCompletedRecoveryAcknowledgement();
    publishCompletedRecoveryScans();
    return destinationRecoveryTask.isRunning() || mirrorRecoveryTask.isRunning();
}

const std::vector<RecoveredSession>& Application::getRecoveredSessions() const
{
    publishCompletedRecoveryAcknowledgement();
    publishCompletedRecoveryScans();
    return recoveredSessions;
}

juce::Array<juce::File> Application::findRecentSessionMetadata (const juce::String& rootPath, int maximum)
{
    juce::Array<juce::File> found;

    const juce::File root { rootPath };

    if (! root.isDirectory())
        return found;

    juce::Array<juce::File> folders;
    root.findChildFiles (folders, juce::File::findDirectories, false);

    // Newest first: a diagnostics bundle is nearly always about the take that
    // just went wrong.
    NewestFirst comparator;
    folders.sort (comparator);

    for (const auto& folder : folders)
    {
        if (found.size() >= maximum)
            break;

        const auto metadata = folder.getChildFile ("session.json");

        if (metadata.existsAsFile())
            found.add (metadata);
    }

    return found;
}

void Application::shutdown()
{
    if (shutdownStarted)
        return;

    shutdownStarted = true;

    // A device/preflight callback may already be queued on the message thread.
    // Make it harmless before any owned subsystem begins disappearing. This
    // also covers the explicit JUCE shutdown followed by ~Application's second
    // call into this function.
    invalidateAliveToken();

    // Combining is post-take convenience work. Ask its detached worker and
    // child process to stop before any hardware or filesystem finalizer gets a
    // chance to stall; this request never joins the worker.
    takeCombiner.cancel();

    // Likewise the podcast-ready copy: told now, waited for (briefly) below,
    // by which time it has usually cleared away its unfinished file.
    podcastExporter.cancel();

    // And the copy to the card. Its note stays on this computer, so the next
    // launch finishes it.
    cardCopier.cancel();

    // The drive check, early: its worker deletes its 200 MB scratch file on
    // the way out, and the sooner it is told the more of teardown it has to
    // get there. The next run sweeps up anything it still leaves.
    preflightTask.cancel();

    // Finalize user media before auxiliary teardown. The detached storage
    // workers never join here; their cancellation flags only suppress late
    // publication, and worker-owned state survives any OS call that does not
    // return. Finalization itself performs filesystem I/O and is not
    // cancellable: force-terminating while either writer is blocked can still
    // leave an incomplete file, which no in-process ordering can prevent.
    // Camera first: CaptureCoordinator::stopRecording drains and joins a writer
    // that may itself be stuck in removable-volume I/O. Putting the movie stop
    // behind that join could leave its header open forever. JUCE camera teardown
    // is message-thread-affine, so these two finalizers cannot safely be raced.
    cameraController.stopRecordingForShutdown();

    if (capture != nullptr)
        capture->stopRecording();

    // The writer is drained above while its callbacks still have a valid rig.
    // Now stop the clock/streams that can enter those callbacks.
    if (capture != nullptr)
        capture->stopMonitoring();

    if (isRecording())
        recordingEngine.stop();
    sleepInhibitor.setHeld (false);
    if (audioBackend != nullptr)
        audioBackend->closeAllStreams();

    // Other apps must not be left holding a combined device whose owner is gone.
    if (systemAggregate != nullptr)
        systemAggregate->remove();

    destinationRecoveryTask.cancel();
    mirrorRecoveryTask.cancel();
    recoveryAcknowledgementTask.cancel();
    filesystemStatusProbe.stop();

    // Bounded: a worker stuck on a card that went away is left to the OS.
    podcastExporter.waitUntilIdle (PodcastExporter::kShutdownWaitMs);
    cardCopier.waitUntilIdle (CardCopier::kShutdownWaitMs);

    // The rig as the user is leaving it, so tomorrow starts where today ended.
    saveSettings();
}

} // namespace mma
