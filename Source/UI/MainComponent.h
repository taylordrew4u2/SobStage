#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainScreen.h"
#include "../Core/ElapsedClock.h"
#include "AdvancedPanel.h"
#include "CameraPanel.h"
#include "HelpPanel.h"
#include "SetupGuidePanel.h"
#include "SaveLocationPrompt.h"
#include "SavedTakePanel.h"
#include "RecoveredTakesPanel.h"
#include "TakeAlertCard.h"
#include "TakeBanner.h"
#include "CameraPreviewCover.h"
#include "../Core/TakeWatchdog.h"
#include "../Core/RecordingProof.h"
#include <functional>

namespace mma {

class Application; // App/Application.h

/// Top-level component: hosts MainScreen always, and opens Settings, Help,
/// the setup guide or the Cameras page beside it on demand, one at a time
/// (§10.3: "one door, one click").
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int kUiRefreshHz = 60;     // §8.2 meter polling
    static constexpr int kStatusRefreshHz = 2;  // background-sampled status; see refreshStatus()

    explicit MainComponent (Application& app);
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    /// While the app waits to quit until the combined video or podcast copy
    /// is finished, the line under the record button says so, and so does
    /// the foot of every card (the saved-take card above all), since a card
    /// covers that line; empty clears both. Without it a pending quit looked
    /// like nothing had happened, and quitting again was the natural next
    /// step.
    void setQuitPendingNote (const juce::String& note);

private:
    /// §8.2: the UI polls, the audio thread never pushes. Dropping a frame here
    /// is acceptable; it can never stall the audio callback.
    void timerCallback() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void refreshStatus();
    /// Rebinds every Metering pointer the meters hold. Called from
    /// Application::onCaptureRebuilt in the same call stack that destroyed the
    /// old ones, so no timer can dereference a freed Metering in between.
    void rebindMeters();
    Application& application;
    MainScreen mainScreen;
    AdvancedPanel advancedPanel;

    // Both screens are taller than a small window -- Settings especially, once
    // a few microphones are listed. Without these the overflow was simply
    // clipped, with no scrollbar and no indication anything was missing.
    int lastAdvancedHeight = 0;

    juce::Viewport mainViewport;
    juce::Viewport advancedViewport;
    juce::Viewport cameraViewport;
    bool advancedVisible = false;
    bool cameraVisible = false;

    /// Whether a drawer is taking the right-hand side of the window.
    bool anyDrawerOpen() const { return advancedVisible || helpVisible || guideVisible || cameraVisible; }

    /// A click on the main screen ends typing in a drawer's text box -- a
    /// camera's name, the combined device's -- as one on the main screen's
    /// own name box does (MainScreen::ClickAwayWatcher). Nothing there takes
    /// the keyboard on a click, so the box kept it beside the live main
    /// screen, and the next Space, the room's mute, typed into the name.
    struct DrawerTypingWatcher final : juce::MouseListener
    {
        explicit DrawerTypingWatcher (MainComponent& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent&) override;
        MainComponent& owner;
    };
    DrawerTypingWatcher drawerTypingWatcher { *this };

    // The third door, beside Settings: the answers to "why is it silent?",
    // in the app rather than in a README. Same viewport arrangement as the
    // other two, and like them only one is open at a time.
    juce::Viewport helpViewport;
    bool helpVisible = false;
    HelpPanel helpPanel;
    void toggleHelp();

    // The first-run setup guide: a fourth drawer, beside the main screen like
    // Settings and Help rather than a card over it, because two of its pages
    // send the reader to the main screen -- to tap a microphone and watch its
    // strip light, and to press Record. Opened by itself once, on a first
    // launch, and from Settings or Help after that.
    juce::Viewport guideViewport;
    bool guideVisible = false;
    /// Put aside while another drawer is up -- the guide itself sends people
    /// to Settings and Cameras -- and brought back when that closes, on the
    /// page they were on.
    bool guideSuspended = false;
    bool guideAutoOpenConsidered = false;
    /// A take has started, and then a take has finished, since the guide was
    /// opened: what its last page reports back.
    bool guideSawRecording = false;
    bool guideTakeSaved = false;
    int lastGuideHeight = 0;
    SetupGuidePanel guidePanel;
    SetupGuide setupGuide;
    void openSetupGuide();
    /// Finished, skipped or escaped: all three close it for good, until it is
    /// asked for again.
    void closeSetupGuide();
    void suspendSetupGuide();
    void resumeSetupGuideIfSuspended();
    void refreshSetupGuide();
    SetupGuideRig readSetupGuideRig() const;
    void openSetupGuideOnFirstLaunch();

    /// §11: logs, recent session.json files and the device inventory to a
    /// zip on the desktop. Never audio. Reachable from Settings and Help.
    void exportDiagnostics();
    int lastCameraHeight = 0;

    /// Grows the window so the main screen's camera row gets the size the user
    /// asked for. Only ever grows: shrinking would undo a window the user sized
    /// themselves. Bounded by the display, so it cannot walk off the screen.
    static juce::Rectangle<int> usableAreaForWindow (juce::Component& window);
    void growWindowToFitMainScreen();

    /// The same, for any content height -- the panels that open over the main
    /// screen ask for their own on the way in, so none of them opens showing
    /// its first few rows with the rest behind a scrollbar.
    void growWindowToFit (int contentHeight);

    /// Settings and Help open as a drawer down the right-hand side, with the
    /// main screen -- meters, pictures, record button -- still live on the
    /// left. A full-screen panel meant every trip into Settings was a trip
    /// away from the thing being recorded.
    int drawerWidth() const;
    /// Widens the window (never narrows it) so the drawer and a usable main
    /// screen fit side by side.
    void growWindowToFitWidth (int contentWidth);
    /// While a drawer is open the window may not be dragged narrower than the
    /// drawer plus a usable main screen, nor, while a full-window card is up,
    /// shorter than the card; otherwise the window's own minimum is back.
    /// -1 until the window's own minimum has been read.
    void updateWindowMinimumSize();
    int windowBaseMinimumWidth = -1;
    int windowBaseMinimumHeight = -1;

    /// Pauses the meters' repainting while the window cannot be seen, and
    /// repaints everything once when it can again. Painting only.
    void updateWindowVisibility();
    bool windowVisible = true;
    /// One place that decides which viewports show, from the three flags.
    void applyPanelVisibility();
    int lastMicCount = -1;
    juce::StringArray lastAdvancedTrimRows; // identity and name of each trim row shown
    mma::ElapsedClock statusClock { 1.0 / kStatusRefreshHz };

    // What the live "files are appearing" line last reported, so it is only
    // rebuilt when the folder on disk has actually changed underneath it.
    juce::String lastSavingLine;
    // setQuitPendingNote(): shown on that line between takes.
    juce::String quitPendingNote;

    /// §10.1/§6.2: the record press, with the "where does this go" question in
    /// front of it the first time it is asked for a given destination.
    void beginRecording();
    /// Actually starts the take. Everything the prompt is for has already
    /// happened by the time this runs.
    void startRecordingNow();
    void showSaveLocationPrompt();
    void dismissSaveLocationPrompt();
    /// §6.2: called when a take has finished, with what landed on disk.
    void showSavedTake();
    void chooseDestinationFolder (std::function<void()> onChosen);

    /// §6.6: shown once at launch when the last run was interrupted.
    void showRecoveredTakes();

    SaveLocationPrompt saveLocationPrompt;
    SavedTakePanel savedTakePanel;
    RecoveredTakesPanel recoveredTakesPanel;

    // §0.1 / §6.5: the mid-take pop-up and the watcher that raises it. The
    // watcher is fed one reading per slow tick while a take runs and speaks
    // only when something changes; the card collects what it says.
    TakeAlertCard takeAlertCard;
    TakeBanner takeBanner;
    bool lastAnnouncedRecording = false;
    juce::String announcedTakeFolder;
    void announceTakeTransitions();
    TakeWatchdog takeWatchdog;
    RecordingProof recordingProof;
    bool takeStoppedByProof = false;
    void showTakeAlertCard();
    bool wasRecording = false;
    int ticksUntilCameraRecheck = 0;
    void watchTake (bool isRecording);
    // The folder the panel is currently showing, so "Open the folder" opens the
    // one on screen rather than whatever the app has moved on to since.
    juce::String savedTakeFolder;
    // The take on screen was recorded on this computer and is being copied to
    // the card: its line follows the copy while the panel is open.
    bool savedTakeCopiesToCard = false;
    void refreshSavedTakeCardCopy();

    void toggleAdvanced();
    void toggleCameras();
    /// The main screen's camera tiles, always, and the Cameras page while it
    /// is open. Each rebuilds only what has actually moved.
    void refreshCameras();
    CameraPanel cameraPanel;

    /// Hides the live camera previews while any card or the take banner is up.
    /// On macOS they are native views drawn above every card; see
    /// CameraPreviewCover. Declared after everything it watches or drives, so
    /// it is destroyed first and never calls into a half-destroyed window.
    CameraPreviewCover cameraPreviewCover;

    /// §10.3 panel contents. Refreshed on the slow tick and whenever the panel
    /// is opened, so it is never showing a stale rig.
    void refreshAdvanced();
    std::unique_ptr<juce::FileChooser> folderChooser;
    void promptRenameMic (int index);

    /// Show templates. The list is read from disk when Settings opens and after
    /// a save or delete, never on the refresh tick: it is a folder of files, and
    /// nothing else changes it while the app is running.
    void refreshShows (const juce::String& select = {});
    void promptSaveShow();
    void confirmDeleteShow (const juce::String& name);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace mma
