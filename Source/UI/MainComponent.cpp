#include "MainComponent.h"
#include "AppLookAndFeel.h"
#include "../App/Application.h"
#include "../Core/ShowTemplate.h"
#include "../Platform/WindowOcclusion.h"
#include <set>

namespace mma {

namespace {
// The least the main screen can have beside an open drawer and still show a
// row of strips, the record button and the footer without wrapping into a
// column.
constexpr int kMainMinWidth = 720;
} // namespace

MainComponent::MainComponent (Application& app)
    : application (app)
{
    addAndMakeVisible (mainScreen);
    // Owned by this component, not by the viewports: `false` here means the
    // viewport does not take ownership and will not delete them.
    mainViewport.setViewedComponent (&mainScreen, false);
    mainViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (mainViewport);

    advancedViewport.setViewedComponent (&advancedPanel, false);
    advancedViewport.setScrollBarsShown (true, false);
    advancedViewport.setVisible (false);
    addChildComponent (advancedViewport);

    cameraViewport.setViewedComponent (&cameraPanel, false);
    cameraViewport.setScrollBarsShown (true, false);
    cameraViewport.setVisible (false);
    addChildComponent (cameraViewport);

    helpViewport.setViewedComponent (&helpPanel, false);
    helpViewport.setScrollBarsShown (true, false);
    helpViewport.setVisible (false);
    addChildComponent (helpViewport);

    helpPanel.onCloseClicked = [this] { toggleHelp(); };
    helpPanel.onOpenSettingsClicked = [this] { toggleAdvanced(); };
    helpPanel.onExportDiagnosticsClicked = [this] { exportDiagnostics(); };
    helpPanel.onSetupGuideClicked = [this] { openSetupGuide(); };

    guideViewport.setViewedComponent (&guidePanel, false);
    guideViewport.setScrollBarsShown (true, false);
    guideViewport.setVisible (false);
    addChildComponent (guideViewport);

    guidePanel.onSkipClicked = [this] { closeSetupGuide(); };
    guidePanel.onBackClicked = [this]
    {
        setupGuide.back (readSetupGuideRig());
        guideViewport.setViewPosition (0, 0);
        refreshSetupGuide();
    };
    guidePanel.onNextClicked = [this]
    {
        if (! setupGuide.next (readSetupGuideRig()))
        {
            closeSetupGuide();
            return;
        }

        guideViewport.setViewPosition (0, 0);
        refreshSetupGuide();
    };
    // The same rename the strips' own names open, so a name typed here is the
    // one on the strip and on the file.
    guidePanel.onNameMicClicked = [this] (int index) { promptRenameMic (index); };
    guidePanel.onHeadphonesToggled = [this] (const juce::String& key, bool on)
    {
        application.setHeadphonesOn (key, on);
        refreshSetupGuide();
    };
    guidePanel.onChangeFolderClicked = [this]
    {
        chooseDestinationFolder ([this] { refreshSetupGuide(); });
    };

    mainScreen.onRecordButtonClicked = [this] { beginRecording(); };

    mainScreen.onVolumeChanged = [this] (double volume0to100) {
        // Master monitor volume, mapped logarithmically per §5.1; MonitorBus
        // owns the actual mapping, this just forwards the UI value.
        application.setMasterVolume (volume0to100);
    };

    mainScreen.onMuteToggled = [this] { application.toggleMonitorMute(); };

    mainScreen.onMicNameClicked = [this] (int index) { promptRenameMic (index); };

    // The window is where Space means mute.
    mainScreen.onSessionNameFinished = [this] { grabKeyboardFocus(); };
    mainScreen.addMouseListener (&drawerTypingWatcher, true);

    // A reason under the record button, a no-microphones message or another
    // row of strips makes the screen taller. Re-fitted here, the viewport
    // scrolls to show it; left alone, the footer was squeezed into what was left.
    mainScreen.onRequiredHeightChanged = [this] { resized(); };

    // §5.1: spacebar is the instant mute, so the window has to take keys.
    setWantsKeyboardFocus (true);

    // A capture rebuild destroys the Metering objects the channel meters point
    // at, and each meter's own timer dereferences its pointer on the next
    // tick. Rebinding inside the rebuild's call stack closes that window.
    application.onCaptureRebuilt = [this] { rebindMeters(); };

    mainScreen.onAdvancedClicked = [this] { toggleAdvanced(); };
    mainScreen.onCamerasClicked = [this] { toggleCameras(); };
    mainScreen.onHelpClicked = [this] { toggleHelp(); };

    // The live views come from the controller, which is the only thing holding
    // the open devices. The main screen is the only thing that shows them --
    // it is always on screen, Cameras page or not -- so a camera never has two.
    mainScreen.makeViewer = [this] (const std::string& id) {
        return application.getCameraController().createViewer (id);
    };

    mainScreen.setVersionText ("v" + juce::JUCEApplication::getInstance()->getApplicationVersion());
    mainScreen.setCameraScale (application.getCameraTileScale());
    mainScreen.onCameraScaleChanged = [this] (int step) {
        application.setCameraTileScale (step);
        // The row's height changes with the tiles, so the window has to be
        // re-laid-out around it rather than letting the pictures grow off the
        // bottom of a viewport sized for the old ones.
        resized();
    };

    cameraPanel.onCameraEnabledChanged = [this] (const std::string& id, bool enabled) {
        application.setCameraEnabled (id, enabled);
        // Opening or releasing the camera immediately is what makes the toggle
        // mean something: the view appears or goes when it is clicked, not when
        // the next take starts.
        application.openEnabledCameras (true);
        refreshCameras();
    };

    cameraPanel.onCameraRenamed = [this] (const std::string& id, const juce::String& name) {
        application.setCameraName (id, name);
        refreshCameras(); // the name goes on the file, so the panel restates it
    };

    cameraPanel.onCameraQualityChanged = [this] (const std::string& id, CameraQuality quality) {
        application.setCameraQuality (id, quality);
        refreshCameras();
    };

    cameraPanel.onCombineVideoToggled = [this] (bool enabled) {
        application.setCombineVideoAndAudio (enabled);

        // Straight back, so switching it on says immediately whether the
        // machine can actually do it rather than leaving that to be discovered
        // after the next take.
        refreshCameras();
    };

    cameraPanel.onCloseClicked = [this] { toggleCameras(); };

    // §10.3: everything behind the one door is optional, but none of it is
    // decorative -- each control below reaches the engine it names.
    advancedPanel.onTrimChanged = [this] (int index, float trimDb) {
        application.setChannelTrimDb (index, trimDb);
    };


    advancedPanel.onAggregateNameChanged = [this] (const juce::String& name) {
        application.setAggregateDeviceName (name);
    };

    advancedPanel.onOutputDeviceChanged = [this] (const juce::String& name) {
        application.setOutputDeviceByName (name);
    };

    advancedPanel.onHeadphonesToggled = [this] (const juce::String& name, bool on) {
        application.setHeadphonesOn (name, on);
    };

    advancedPanel.onSampleRateChanged = [this] (uint32_t rate) {
        application.setSampleRateOverride (rate);
    };

    advancedPanel.onInputEnabledChanged = [this] (const juce::String& device, int input, bool enabled) {
        application.setInputEnabled (device, input, enabled);
    };

    advancedPanel.onBitDepthChanged = [this] (int bits) {
        application.setBitDepthOverride (bits);
    };

    advancedPanel.onBufferSizeChanged = [this] (int samples) {
        application.setBufferSizeOverride (samples);
    };

    advancedPanel.onMirrorToggled = [this] (bool enabled) {
        application.setMirrorEnabled (enabled);
    };

    advancedPanel.onCopyToCardToggled = [this] (bool enabled) {
        application.setRecordOnComputerThenCopy (enabled);
    };

    advancedPanel.onDeliveryTargetChanged = [this] (juce::String name) {
        application.setDeliveryTarget (name);
        refreshAdvanced();
    };

    advancedPanel.onDestinationFolderClicked = [this] {
        chooseDestinationFolder ([this] { refreshAdvanced(); });
    };

    advancedPanel.onStorageVolumeChosen = [this] (const juce::String& path) {
        application.setDestinationByPath (path);
        refreshAdvanced();
    };

    advancedPanel.onCloseClicked = [this] { toggleAdvanced(); };
    advancedPanel.onHelpClicked = [this] { toggleHelp(); };
    advancedPanel.onSetupGuideClicked = [this] { openSetupGuide(); };

    advancedPanel.onMicEnabledChanged = [this] (const juce::String& name, bool enabled) {
        application.setMicEnabledByKey (name, enabled);
    };

    advancedPanel.onDiagnosticsExportClicked = [this] { exportDiagnostics(); };

    advancedPanel.onCheckForUpdatesToggled = [this] (bool enabled) {
        application.setCheckForUpdates (enabled);
        refreshAdvanced();
    };

    advancedPanel.onCheckForUpdatesNowClicked = [this] {
        application.checkForUpdatesNow();
        refreshAdvanced();
    };

    advancedPanel.onDownloadUpdateClicked = [this] { application.openUpdatePage(); };

    advancedPanel.onLoadShowClicked = [this] (const juce::String& name) {
        // The rig is rebuilt by the load, which rebinds the strips through the
        // capture callback; the panel is restated now so it shows the show.
        application.applyTemplate (name);
        refreshAdvanced();
    };

    advancedPanel.onSaveShowClicked = [this] { promptSaveShow(); };
    advancedPanel.onDeleteShowClicked = [this] (const juce::String& name) { confirmDeleteShow (name); };

    // §10.1/§6.2: the question asked before the first take, and the answer
    // given after every one. Children of this component rather than
    // AlertWindows so they arrive in the app's own palette and spacing, and
    // addChildComponent (not addAndMakeVisible) so neither is up until it is
    // wanted.
    saveLocationPrompt.folderNameFor = [this] (const juce::String& name) {
        return application.planSave (name).folderName;
    };
    saveLocationPrompt.onStart = [this] {
        application.setAskWhereToSaveEveryTime (saveLocationPrompt.getAskEveryTime());
        // §10.1: the answer is about the folder they were just shown, so it is
        // recorded against that folder and every later press goes straight
        // through -- §10.4's "no confirmation" holds from here on.
        application.confirmSaveLocation();
        mainScreen.setSessionName (saveLocationPrompt.getSessionName());
        dismissSaveLocationPrompt();
        startRecordingNow();
    };
    saveLocationPrompt.onChooseFolder = [this] {
        // The card is rebuilt from the main screen's name field, so a name
        // typed into the card goes back there first -- otherwise picking a
        // folder silently throws away what the user had just typed.
        mainScreen.setSessionName (saveLocationPrompt.getSessionName());

        // Re-stated against the folder they just picked, rather than dismissed:
        // the whole card was the answer to "where does this go", and the answer
        // has just changed.
        chooseDestinationFolder ([this] { showSaveLocationPrompt(); });
    };
    saveLocationPrompt.onCancel = [this] {
        // Backing out of the card is not backing out of the name: it lands in
        // the box on the main screen, where the user can see it and where the
        // next press will pick it up.
        mainScreen.setSessionName (saveLocationPrompt.getSessionName());
        dismissSaveLocationPrompt();
    };
    addChildComponent (saveLocationPrompt);

    savedTakePanel.onOpenFolder = [this] {
        // §6.2: the offer to open the containing folder. Reveals the session
        // folder in the OS file browser rather than opening the files, which
        // is the difference between "here is your recording" and a media
        // player nobody asked for.
        //
        // A take copied to the card opens on the card once the copy is
        // checked -- that is where the user is taking it -- and on this
        // computer until then.
        juce::String folder = savedTakeFolder;

        if (savedTakeCopiesToCard)
            if (const auto copied = application.getCardCopyView (savedTakeFolder).cardFolderIfCopied;
                copied.isNotEmpty())
                folder = copied;

        juce::File (folder).revealToUser();
    };
    savedTakePanel.onCopyAgain = [this] {
        application.copyAgain (savedTakeFolder);
        refreshSavedTakeCardCopy();
    };
    mainScreen.onCopyToThisCardClicked = [this] {
        application.copyWaitingTakesToCardInNow();
        mainScreen.setCopyToThisCardVisible (false);
    };
    savedTakePanel.onDone = [this] {
        savedTakePanel.setVisible (false);
        grabKeyboardFocus();
    };
    addChildComponent (savedTakePanel);

    // The mid-take pop-up. Keep = dismiss; the take never stopped. Stop =
    // the same press as the record button, so the saved-take card follows.
    takeAlertCard.onKeepRecording = [this] {
        takeAlertCard.setVisible (false);
        grabKeyboardFocus();
    };
    takeAlertCard.onStopRecording = [this] {
        takeAlertCard.setVisible (false);

        // "OK" on a take the proof already stopped; "Stop recording" otherwise.
        if (application.getRecordingEngine().getState() == RecordingState::Recording)
        {
            startRecordingNow();
        }
        else
        {
            grabKeyboardFocus();

            // The saved-take card waited behind this one; it follows the OK.
            showSavedTake();
        }
    };
    addChildComponent (takeAlertCard);

    recoveredTakesPanel.onOpenFolder = [this] {
        juce::File (recoveredTakesPanel.getFolderToOpen()).revealToUser();
    };
    recoveredTakesPanel.onDone = [this] {
        recoveredTakesPanel.setVisible (false);
        application.clearRecoveredSessions();
        grabKeyboardFocus();
    };
    addChildComponent (recoveredTakesPanel);

    // Last, so it sits over every card: the three-second takeover that says a
    // take has started or stopped. It takes no input, so nothing behind it
    // is ever blocked.
    takeBanner.setReducedMotion (application.prefersReducedMotion());
    takeAlertCard.setReducedMotion (application.prefersReducedMotion());
    addChildComponent (takeBanner);

    // Every overlay above is a JUCE child of this window, but on macOS a live
    // camera preview is a native NSView composited above all JUCE painting:
    // it drew over the save prompt, the saved-take card, the red take alert
    // and the take banner, hiding their text and buttons. Hide the previews
    // -- only hide, never rebuild them or touch the capture session -- for
    // exactly as long as any of these is visible.
    cameraPreviewCover.onCoverChanged = [this] (bool covered)
    {
        mainScreen.setCameraPreviewsHidden (covered);
    };
    cameraPreviewCover.watch (saveLocationPrompt);
    cameraPreviewCover.watch (savedTakePanel);
    cameraPreviewCover.watch (recoveredTakesPanel);
    cameraPreviewCover.watch (takeAlertCard);
    cameraPreviewCover.watch (takeBanner);

    // Tall enough that the whole main screen -- monitor volume, mute and the
    // Settings button included -- is on screen at launch. At 480 the bottom
    // row sat below the fold, so the one door into Settings was reachable
    // only by scrolling. The viewport still scrolls on shorter displays.
    // A floor as well as the main screen's own ask. The main screen is the
    // most compact of the three -- Settings and Cameras both need more -- and
    // sizing the window to the smallest of them left the other two scrolling
    // from the moment they opened.
    //
    // The floor is a compromise rather than a fit: on a rig with a camera
    // switched on the pictures fill this height and more, and on an audio-only
    // rig it leaves room under the levels. Sizing to the audio-only case
    // instead would make the window jump every time a camera was switched on.
    // Sized to what the content wants, then clamped to what the screen has.
    //
    // Component::centreWithSize does not clamp -- it centres at exactly the
    // size it is given -- so a window taller than the display simply hangs off
    // both ends of it, taking the record button with it. That was survivable
    // when the camera pictures were thumbnails and stopped being so the moment
    // they got big enough to be worth looking at.
    //
    // The main screen is inside a viewport, so a window clamped smaller than
    // its content scrolls rather than clipping.
    const auto usable = juce::Desktop::getInstance().getDisplays()
                            .getPrimaryDisplay() != nullptr
                        ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                        : juce::Rectangle<int> (0, 0, 1180, 900);

    // The floor matches the window's own minimum height rather than sitting
    // 140px above it. At 560 an audio-only rig opened with a band of empty
    // background between the last status line and the footer, because the
    // content it has to show is barely 330px tall -- the space the pictures
    // would occupy, reserved on behalf of cameras nobody had switched on.
    //
    // Opening to the content is only safe because switching a camera on now
    // grows the window (growWindowToFitMainScreen); before that, a window this
    // size would have had nowhere to put the picture.
    setSize (juce::jmin (1180, usable.getWidth()),
             juce::jlimit (420, juce::jmax (420, usable.getHeight()),
                           mainScreen.getPreferredHeight() + 24));

    // §8.1: meters and status are live from launch, not from record.
    refreshStatus();
    startTimerHz (kUiRefreshHz);

    // §6.6: "present recovered recordings before the main screen." Last, so the
    // screen it sits in front of is already laid out behind it.
    showRecoveredTakes();
}

MainComponent::~MainComponent()
{
    mainScreen.removeMouseListener (&drawerTypingWatcher);

    // The window is torn down before Application::shutdown(), and a queued
    // device-change can still fire in between -- it must not call into a
    // destroyed component.
    application.onCaptureRebuilt = nullptr;
    stopTimer();
}

void MainComponent::DrawerTypingWatcher::mouseDown (const juce::MouseEvent&)
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();

    if (dynamic_cast<juce::TextEditor*> (focused) == nullptr)
        return;

    const juce::Component* drawers[] { &owner.advancedViewport, &owner.cameraViewport,
                                       &owner.helpViewport, &owner.guideViewport };

    for (const auto* drawer : drawers)
        if (drawer->isParentOf (focused))
        {
            // To the window, where Space mutes; the box commits what was
            // typed as it loses the keyboard (its onFocusLost).
            owner.grabKeyboardFocus();
            return;
        }
}

void MainComponent::timerCallback()
{
    refreshStatus();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // A card is up and owns the keyboard. Muting the room from behind one would
    // be a change the user cannot see the cause of.
    if (saveLocationPrompt.isVisible() || savedTakePanel.isVisible()
        || recoveredTakesPanel.isVisible() || takeAlertCard.isVisible())
        return false;

    // §5.1: spacebar mutes and unmutes the monitor instantly. A focused text
    // field never reaches here -- it consumes its own keys -- so typing a
    // space into the session name does not silence the room.
    if (key == juce::KeyPress::spaceKey)
    {
        if (mainScreen.onMuteToggled)
            mainScreen.onMuteToggled();

        return true;
    }

    // Cmd-, is where every Mac app keeps its settings; people press it before
    // they go looking for the button. A second press closes them again.
    if (key == juce::KeyPress (',', juce::ModifierKeys::commandModifier, 0))
    {
        toggleAdvanced();
        return true;
    }

   #if JUCE_MAC
    // Cmd-M minimises every Mac window; it comes from a Window menu, and JUCE's
    // stock menu bar has none, so here it only beeped. Minimising never
    // touches a take. (Cmd-W is deliberately left alone: closing this window
    // is quitting, and that is never one keystroke away during a show.)
    if (key == juce::KeyPress ('m', juce::ModifierKeys::commandModifier, 0))
    {
        if (auto* peer = getPeer())
            peer->setMinimised (true);

        return true;
    }
   #endif

    // Escape is the way back from any panel, so nobody has to find the Done
    // button at the top of a screen they have scrolled down.
    if (key == juce::KeyPress::escapeKey)
    {
        if (advancedVisible)     toggleAdvanced();
        else if (cameraVisible)  toggleCameras();
        else if (helpVisible)    toggleHelp();
        else if (guideVisible)   closeSetupGuide();
        else                     return false;

        return true;
    }

    // The arrows resize the camera pictures. The main screen is always on
    // screen now -- every drawer, Cameras included, opens beside it -- so they
    // work whenever no card is up (the cards returned above). A focused slider
    // or text field consumes its own arrows before they reach here, so this
    // cannot steal them from the volume.
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
    {
        const int step = mainScreen.getCameraScale() + (key == juce::KeyPress::upKey ? 1 : -1);

        mainScreen.setCameraScale (step);

        if (mainScreen.onCameraScaleChanged)
            mainScreen.onCameraScaleChanged (mainScreen.getCameraScale());

        return true;
    }

    return false;
}

void MainComponent::beginRecording()
{
    // Every card is full-window and swallows the mouse, so a record press
    // while one is up can only be a key (Return) on the button hidden behind
    // it -- e.g. focus handed back to it by ComponentPeer::handleFocusGain on
    // Cmd+Tab back. The card owns the decision; only its own Stop ends a take.
    if (saveLocationPrompt.isVisible() || savedTakePanel.isVisible()
        || recoveredTakesPanel.isVisible() || takeAlertCard.isVisible())
        return;

    // A click hands the keyboard to the record button. Never leave it on the
    // one control that ends a take (a stray Return would press it), nor in
    // the name box (Space would type instead of muting the room).
    grabKeyboardFocus();

    // §6.2: whatever is in the name box when record is pressed names the take.
    application.setSessionName (mainScreen.getSessionName());

    const bool stopping = application.getRecordingEngine().getState() == RecordingState::Recording;

    // §10.4: stopping is never confirmed and never delayed. Only the first
    // start against a destination the user has not been shown asks anything,
    // and it asks before a file exists rather than after one is lost.
    if (! stopping && ! application.isSaveLocationConfirmed())
    {
        showSaveLocationPrompt();
        return;
    }

    startRecordingNow();
}

void MainComponent::startRecordingNow()
{
    const bool stopping = application.getRecordingEngine().getState() == RecordingState::Recording;

    // §6.4: arming is blocked until the destination has passed its throughput
    // benchmark, and refusing before the take starts is the whole point --
    // "never degrade mid-take". The record button is disabled for this, but the
    // prompt's own start button is a second way in, and picking a new folder
    // from the prompt is exactly what sets a fresh benchmark running.
    if (! stopping && application.getRecordDisabledReason().isNotEmpty())
        return;

    application.setSessionName (mainScreen.getSessionName());
    application.toggleRecording();
    mainScreen.setRecording (application.getRecordingEngine().getState() == RecordingState::Recording);

    // CameraController freezes the writers which actually started inside the
    // toggle above. Rebuild the tile captions in this same click so an opened
    // camera which never delivered a frame reads NOT RECORDING immediately,
    // while only the proven writers receive a REC label.
    refreshCameras();

    // §6.2: a stop that wrote a take raises the notice; showing it here rather
    // than waiting for the next timer tick keeps the panel attached to the
    // press that caused it.
    showSavedTake();
}

void MainComponent::showSaveLocationPrompt()
{
    const auto plan = application.planSave (mainScreen.getSessionName());
    const auto& cameraController = application.getCameraController();
    const auto& cameraSelection = cameraController.getSelection();
    int armedCameraCount = 0;
    int readyCameraCount = 0;

    for (const auto& camera : cameraSelection.getKnownCameras())
        armedCameraCount += cameraSelection.isEnabled (camera.id) ? 1 : 0;

    for (const auto& camera : cameraSelection.getAvailableCameras())
        if (cameraSelection.isEnabled (camera.id)
            && cameraController.getSignalState (camera.id) == CameraController::SignalState::Live)
            ++readyCameraCount;

    saveLocationPrompt.setSessionName (mainScreen.getSessionName());
    saveLocationPrompt.setAskEveryTime (application.getAskWhereToSaveEveryTime());
    saveLocationPrompt.setPlan (plan.parentFolder, plan.folderName, plan.mirrorFolder, plan.fileNames,
                                armedCameraCount, readyCameraCount, plan.mirrorNote,
                                plan.recordsOnComputerFirst ? plan.computerFolder : juce::String(),
                                plan.recordsOnComputerFirst ? plan.roomNote : juce::String());

    saveLocationPrompt.setBlockedReason (application.getRecordDisabledReason());
    // Room for the whole card BEFORE it is shown. ModalCard clamps itself to
    // the window (getHeight() - 32) and truncates what will not fit -- it is
    // not inside a viewport, so there is nothing to scroll. In a short window
    // that put this card's buttons past the bottom edge, which does not look
    // broken; it looks like a prompt that cannot be answered.
    growWindowToFit (saveLocationPrompt.getRequiredHeight() + 32);

    saveLocationPrompt.setBounds (getLocalBounds());
    saveLocationPrompt.setVisible (true);
    saveLocationPrompt.toFront (true);
    saveLocationPrompt.prepareToShow();
}

void MainComponent::dismissSaveLocationPrompt()
{
    saveLocationPrompt.setVisible (false);
    grabKeyboardFocus();
}

void MainComponent::showRecoveredTakes()
{
    // Both one-shot scans must have settled before this card is populated. If
    // the first result were shown and dismissed while the second was still in
    // a blocked OS call, that late result could reopen the card. Waiting also
    // keeps recovery presentation out of the way of an active take or another
    // modal/panel.
    if (application.isRecoveryScanPending()
        || recoveredTakesPanel.isVisible()
        || application.getRecordingEngine().getState() == RecordingState::Recording
        || saveLocationPrompt.isVisible()
        || savedTakePanel.isVisible()
        || takeAlertCard.isVisible()
        || advancedVisible || cameraVisible || helpVisible)
        return;

    const auto& sessions = application.getRecoveredSessions();

    // The overwhelmingly common case: the last run quit cleanly and there is
    // nothing to say, so nothing is shown.
    if (sessions.empty())
        return;

    std::vector<RecoveredTakesPanel::TakeRow> rows;

    for (const auto& session : sessions)
        rows.push_back (recoveredTakeRow (session));

    recoveredTakesPanel.setTakes (rows);
    growWindowToFit (recoveredTakesPanel.getRequiredHeight() + 32);

    recoveredTakesPanel.setBounds (getLocalBounds());
    recoveredTakesPanel.setVisible (true);
    recoveredTakesPanel.toFront (true);
    recoveredTakesPanel.prepareToShow();
}

namespace {

/// §6.5: how loudly each piece of mid-take news should be drawn. Everything
/// bad used to share one amber, which told the user a camera going away and a
/// microphone going away were the same size of problem. They are not: one of
/// them cost recorded audio and the other did not.
TakeAlertCard::Tone toneFor (const TakeAlert& alert)
{
    using Kind = TakeAlert::Kind;

    if (alert.recovery)
        return TakeAlertCard::Tone::Fixed;

    switch (alert.kind)
    {
        case Kind::MicLost:
        case Kind::MixOnly:
        case Kind::TwoMinutesLeft:
            return TakeAlertCard::Tone::NeedsYou;

        // Real news, worth saying, but the recorded audio was never at risk:
        // the camera and the headphone output are both outside the take.
        case Kind::CameraLost:
        case Kind::CameraTrouble:
        case Kind::OutputLost:
        case Kind::MonitorTrouble:
            return TakeAlertCard::Tone::Quiet;

        case Kind::AudioDropped:
        case Kind::WriterBehind:
        case Kind::TenMinutesLeft:
        case Kind::MicBack:
        case Kind::OutputBack:
            break;
    }

    return TakeAlertCard::Tone::Watch;
}

} // namespace

void MainComponent::watchTake (bool isRecording)
{
    // Runs on the slow tick. The first tick of a take is the baseline --
    // whatever was already wrong was on screen before record was pressed --
    // and every tick after it is compared against the one before.
    if (isRecording && ! wasRecording)
    {
        takeAlertCard.clear();
        takeAlertCard.setVisible (false);
        takeStoppedByProof = false;
        takeWatchdog.beginTake (application.snapshotTakeHealth());
        recordingProof.begin (application.snapshotProof());
        ticksUntilCameraRecheck = 0;
    }
    else if (! isRecording && wasRecording)
    {
        takeWatchdog.endTake();

        // A take the proof stopped keeps its red card up: that card IS the
        // news, and hiding it with the take would be the old silence again.
        if (! takeStoppedByProof)
            takeAlertCard.setVisible (false);
    }

    wasRecording = isRecording;

    // The OS does not announce a camera going away; it just stops listing it.
    // Re-listed every couple of seconds on CameraController's discovery worker
    // -- and OUTSIDE a take as well as during one, which it was not: a camera
    // plugged in or pulled out between takes was not even noticed, let alone
    // said, so someone who connected a camera and pressed record found out it
    // was missing from the take afterwards.
    if (--ticksUntilCameraRecheck <= 0)
    {
        ticksUntilCameraRecheck = kStatusRefreshHz * 2;
        application.getCameraController().refreshCamerasIfIdle();
        application.announceCameraChanges();
    }

    if (! isRecording)
        return;

    // §0.1: the disk has to agree that this is a take. Judged before the
    // watchdog's softer news, because "nothing is being recorded" outranks
    // everything else that could be said.
    const auto verdict = recordingProof.observe (application.snapshotProof());

    if (verdict == ProofVerdict::NothingWritten || verdict == ProofVerdict::Stalled
        || verdict == ProofVerdict::NoSoundArriving)
    {
        const auto when = Application::formatDuration (application.getElapsedRecordingSeconds()) + " in";
        takeAlertCard.addAlert (when, juce::String (RecordingProof::message (verdict)),
                                TakeAlertCard::Tone::NeedsYou);

        if (verdict == ProofVerdict::NothingWritten)
        {
            // Stop it NOW. The saved-take card that follows says the files
            // are empty; this card says why the take ended.
            takeStoppedByProof = true;
            takeAlertCard.setSevere (true, true);
            showTakeAlertCard();
            startRecordingNow();
            return;
        }

        takeAlertCard.setSevere (true, false);
        showTakeAlertCard();
    }

    const auto alerts = takeWatchdog.observe (application.snapshotTakeHealth());

    if (alerts.empty())
        return;

    const auto when = Application::formatDuration (application.getElapsedRecordingSeconds()) + " in";

    for (const auto& alert : alerts)
        takeAlertCard.addAlert (when, juce::String (alert.message), toneFor (alert));

    // Good news alone does not interrupt anyone; it joins the card if the
    // card is already up. Bad news brings the card up, unless another card
    // -- the save prompt, say -- is already asking something.
    bool anyBad = false;
    for (const auto& alert : alerts)
        anyBad = anyBad || ! alert.recovery;

    if (anyBad && ! takeAlertCard.isVisible()
        && ! saveLocationPrompt.isVisible() && ! savedTakePanel.isVisible()
        && ! recoveredTakesPanel.isVisible())
        showTakeAlertCard();
}

void MainComponent::showTakeAlertCard()
{
    growWindowToFit (takeAlertCard.getRequiredHeight() + 32);
    takeAlertCard.setBounds (getLocalBounds());
    takeAlertCard.setVisible (true);
    takeAlertCard.toFront (true);
    takeAlertCard.setAlarming (true);
    takeAlertCard.prepareToShow();
}

void MainComponent::announceTakeTransitions()
{
    // On the fast tick, whichever way the take started or stopped: the
    // button, the card, the proof, the drive. A start or stop nobody could
    // miss: the whole window flashes for three seconds and the headphones
    // chirp, up for a start and down for a stop.
    const bool recording = application.getRecordingEngine().getState() == RecordingState::Recording;

    if (recording != lastAnnouncedRecording)
    {
        lastAnnouncedRecording = recording;

        if (recording)
        {
            announcedTakeFolder = juce::File (application.getCurrentSessionFolder()).getFileName();
            takeBanner.show (TakeBanner::Kind::Started, "RECORDING",
                             announcedTakeFolder.isNotEmpty() ? "Take: " + announcedTakeFolder : juce::String());
            application.announceRecordingStarted();
        }
        else
        {
            takeBanner.show (TakeBanner::Kind::Stopped, "RECORDING STOPPED",
                             announcedTakeFolder.isNotEmpty() ? "Saved: " + announcedTakeFolder : juce::String());
            application.announceRecordingStopped();
        }
    }

    // The siren follows the card: on while it is up and alarming, off the
    // moment it is not. Re-asserted every tick, so a capture rebuilt during
    // the alarm (a device coming or going) picks it up again. A start or stop
    // chirp is allowed to finish first.
    const bool wanted = takeAlertCard.isVisible() && takeAlertCard.isAlarming();

    if (! wanted || ! application.isAlarmChirping())
        application.setFaultAlarm (wanted);
}

void MainComponent::showSavedTake()
{
    // A take the proof stopped leaves its red card up, alarming. "Saved." must
    // not open over it: the siren would carry on behind a card that does not
    // mention it, with the card that explains it out of sight. Both notices
    // stay pending in Application; the card's OK (or the next slow tick once
    // it is gone) brings this card up after it.
    if (takeAlertCard.isVisible())
        return;

    // §6.5: consumed first and unconditionally, so the alert cannot be stranded
    // by an early return further down.
    CardRemovalNotice removal;
    const bool driveWentAway = application.consumeCardRemovalNotice (removal);

    Application::SavedTake take;

    if (! application.consumeSavedTake (take))
        return;

    // The card is gone, so its folder cannot be listed -- what survived is the
    // mirror's copy, and that is the folder worth showing and worth opening.
    if (driveWentAway && removal.aCompleteCopySurvives)
    {
        take.folder = juce::String (removal.survivingFolder);
        take.mirrorFolder = {};
        // The mirror is on the internal drive, but a listing is still never
        // allowed to hold the window open indefinitely.
        bool listed = false;
        take.files = Application::listSessionFilesWithin (take.folder, 5000, listed);
        take.filesListed = listed;
    }

    savedTakePanel.setProblem (driveWentAway ? juce::String (removal.message) : juce::String());

    savedTakeFolder = take.folder;

    std::vector<SavedTakePanel::FileRow> rows;
    rows.reserve (take.files.size());

    for (const auto& file : take.files)
        rows.push_back ({ file.name, file.sizeBytes });

    savedTakePanel.setTake (take.folder, take.mirrorFolder, rows, take.verdict, take.filesListed,
                            take.mirrorNote);

    // Recorded on this computer: the card copy's line where the backup line
    // goes, and the reason a take for the card went straight there.
    savedTakeCopiesToCard = take.copiesToCard;
    savedTakePanel.setRouteNote (take.routeNote);
    if (savedTakeCopiesToCard)
        refreshSavedTakeCardCopy();

    growWindowToFit (savedTakePanel.getRequiredHeight() + 32);

    savedTakePanel.setBounds (getLocalBounds());
    savedTakePanel.setVisible (true);
    savedTakePanel.toFront (true);
    savedTakePanel.prepareToShow();
}

void MainComponent::refreshSavedTakeCardCopy()
{
    if (! savedTakeCopiesToCard)
        return;

    const auto view = application.getCardCopyView (savedTakeFolder);
    savedTakePanel.setCardCopy (view.line, view.canCopyAgain, view.copyAgainLabel);
}

void MainComponent::chooseDestinationFolder (std::function<void()> onChosen)
{
    // The chooser stats its starting folder on this thread, and the moment
    // someone is told to "choose another location" is exactly when the current
    // one may be a card that has stopped answering -- the window froze there
    // until Force Quit. A removable volume is never the starting point; the
    // home folder always answers.
    const juce::String current (application.getDestinationFolder());
    const auto start = current.isEmpty() || current.startsWith ("/Volumes/")
                         ? juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                         : juce::File (current);

    folderChooser = std::make_unique<juce::FileChooser> ("Choose where recordings are saved", start);

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                [this, onChosen] (const juce::FileChooser& chooser) {
                                    const auto result = chooser.getResult();

                                    // The mount can disappear after the native
                                    // chooser returns. Even isDirectory() is an
                                    // unbounded filesystem call on macOS, so pass
                                    // the inert path to Application and let its
                                    // detached recovery/preflight gates validate
                                    // it before Record can arm.
                                    if (result.getFullPathName().isNotEmpty())
                                        application.setDestinationFolder (result);

                                    if (onChosen)
                                        onChosen();
                                });
}

void MainComponent::promptRenameMic (int index)
{
    // §14.6: click the meter that lit up when you tapped the mic, type who it
    // is. The name follows the physical port across replug (§2.4).
    const auto target = application.getMicRenameTarget (index);
    if (! target.isValid())
        return;

    auto* window = new juce::AlertWindow ("Name this microphone",
                                          "The name goes on its strip and into its recording's filename.",
                                          juce::MessageBoxIconType::QuestionIcon, this);
    window->addTextEditor ("name", application.getMicDisplayName (index));
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, window, target] (int result)
        {
            if (result == 1)
            {
                application.setMicAssignedName (target, window->getTextEditorContents ("name"));

                // Outside a take the rename rebuilds the capture, which rebinds
                // the strips anyway. During one, §6.5 defers that rebuild to the
                // take's end -- and the strip kept the old name until then.
                // The name on screen is not part of the take; show it now.
                rebindMeters();
            }

            // Saved or cancelled, the dialog took the keyboard with it. Hand
            // it back, or the next Space -- the room's mute -- goes nowhere.
            grabKeyboardFocus();
        }),
        true); // delete the window when dismissed
}

void MainComponent::refreshShows (const juce::String& select)
{
    advancedPanel.setShows (application.listTemplates(), select);
}

void MainComponent::promptSaveShow()
{
    auto* window = new juce::AlertWindow ("Save this setup as a show",
                                          "Microphone names, trims and switches, headphones, cameras, "
                                          "where takes go and the recording format -- all brought back "
                                          "with one click. A show with the same name is replaced.",
                                          juce::MessageBoxIconType::QuestionIcon, this);
    window->addTextEditor ("name", advancedPanel.getSelectedShow());
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, window] (int result)
        {
            if (result == 1)
            {
                const auto name = window->getTextEditorContents ("name").trim();

                // Selected by the name it is listed under, which is the
                // tidied one: a name past 60 characters, or with a line break
                // pasted in, was saved but left the picker on nothing, with
                // Load and Delete greyed out.
                if (application.saveTemplate (name))
                    refreshShows (juce::String::fromUTF8 (ShowTemplate::cleanName (name.toStdString()).c_str()));

                refreshAdvanced();
            }

            grabKeyboardFocus();
        }),
        true);
}

void MainComponent::confirmDeleteShow (const juce::String& name)
{
    if (name.isEmpty())
        return;

    // Asked, because it cannot be undone and the button sits beside Load.
    auto* window = new juce::AlertWindow ("Delete \"" + name + "\"?",
                                          "The saved show is removed. Your current setup is not changed.",
                                          juce::MessageBoxIconType::WarningIcon, this);
    window->addButton ("Delete", 1);
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, name] (int result)
        {
            if (result == 1 && application.deleteTemplate (name))
                refreshShows();

            refreshAdvanced();
            grabKeyboardFocus();
        }),
        true);
}

void MainComponent::refreshStatus()
{
    // AVFoundation closes movie files on its own callback queue. Consume that
    // bounded mailbox from the ordinary UI tick; this never waits, and is what
    // releases metadata/combining/saved notices once every movie is real.
    application.pollCameraFinalization();

    // The opt-in update check: starts the launch check once it is due and no
    // take is running, and passes on a result that arrived during one.
    application.pollUpdateCheck();

    // Only painting depends on this. Everything below -- alarms, the siren,
    // the cards, the take's status -- runs whether or not the window is seen.
    updateWindowVisibility();
    updateWindowMinimumSize();

    announceTakeTransitions();

    // The capture-rebuilt callback is the source of truth after construction.
    // The first refresh performs the initial bind because Application has
    // already opened monitoring before this component exists. In particular,
    // do not rebuild the channel plan at 60 Hz just to rediscover an unchanged
    // count.
    if (lastMicCount < 0)
        rebindMeters();

    // Two controls that used to open at a hardcoded value and never restate
    // what was actually saved. The slider ignores this while it is being
    // dragged, so restating it on the status tick cannot fight the user.
    mainScreen.setMasterVolume (application.getMasterVolume());

    const int micCount = juce::jmax (0, lastMicCount);

    // Liveness can change mid-take without rebuilding the frozen channel set.
    // This is cheap state, unlike names/product strings: those require a
    // channel-plan rebuild and are bound only when capture itself changes.
    for (int i = 0; i < micCount; ++i)
        if (auto* meter = mainScreen.getChannelMeter (i))
            meter->setNoSignal (application.getChannelMetering (i) == nullptr
                                || ! application.isMicLive (i));

    // §14.6: light the meter of whoever was just heard alone.
    mainScreen.setHighlightedMic (application.getTappedChannel());

    if (guideVisible)
        guidePanel.setHighlightedMic (application.getTappedChannel());

    // The mute button reflects the bus, including a §5 runaway cut it must
    // be able to undo.
    if (auto* bus = application.getMonitorBus())
        mainScreen.setMuteState (bus->isMuted(), bus->isRunawayMuted());

    // The meters tick their own ballistics as they paint, so a repaint is the poll.
    mainScreen.repaintMeters();

    const bool isRecording = application.getRecordingEngine().getState() == RecordingState::Recording;
    mainScreen.setRecording (isRecording);

    mainScreen.setElapsedTimeText (isRecording
        ? "Recording for " + Application::formatDuration (application.getElapsedRecordingSeconds())
        : juce::String());

    // Remaining time consumes the filesystem worker's latest snapshot at the
    // status cadence. No volume or per-file stat happens on this thread.
    if (statusClock.isDue (1.0 / kStatusRefreshHz))
    {
        // How long it has really been: App Nap and a busy message thread
        // stretch the timer, and every detector fed from here times itself
        // by this. See ElapsedClock.
        const double sinceLastStatus = statusClock.tick();

        // The app's own voice. The figure is the §6.4 remaining-time estimate
        // either way -- the joke is in the unit, not in the number, so nothing
        // a user relies on is being played with.
        mainScreen.setRemainingTimeText ("Room for "
            + Application::formatDuration (application.getRemainingRecordingSeconds())
            + " of feelings");

        // Idle: where the next take will go. Recording: the folder this one is
        // actually in, which is the specific thing a user needs and the parent
        // folder only implies.
        // The home folder as "~": the label cuts the END off a long path,
        // which is the part that names the take folder.
        const auto shortPath = [] (const juce::String& path)
        {
            const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
            return home.isNotEmpty() && path.startsWith (home) ? "~" + path.substring (home.length())
                                                               : path;
        };

        mainScreen.setSaveLocationText (isRecording
            ? "Saving into " + shortPath (application.getCurrentSessionFolder())
            : application.wouldRecordOnComputer()
                ? "Saves to " + shortPath (application.getDestinationFolder())
                      + " (recorded on this computer first)"
                : "Saves to " + shortPath (application.getDestinationFolder()));

        // §6.2/§10.6: the files themselves, growing. These are the worker's
        // sampled file sizes rather than names inferred from channel count, so
        // what is on screen still reflects what is actually on disk.
        juce::String savingLine;

        if (isRecording)
        {
            const auto files = application.getCurrentSessionFiles();
            int64_t total = 0;

            for (const auto& file : files)
                total += file.sizeBytes;

            if (! files.empty())
                savingLine = "Writing " + juce::String ((int) files.size()) + " files, "
                           + juce::File::descriptionOfSizeInBytes (total) + " so far";
        }
        else
        {
            // Between takes the line is otherwise empty: a quit waiting for
            // the combined video or podcast copy is said here, and otherwise
            // a take still on its way to the card.
            savingLine = quitPendingNote.isNotEmpty() ? quitPendingNote
                                                      : application.getCardCopySummary();
        }

        // The saved-take card follows its copy while it is open.
        if (savedTakePanel.isVisible())
            refreshSavedTakeCardCopy();

        if (savingLine != lastSavingLine)
        {
            lastSavingLine = savingLine;
            mainScreen.setFilesBeingSavedText (savingLine);
        }

        // Takes waiting for their own card while another is in: the line
        // says so, and "Copy to this card" beside it copies every one of them
        // to the card in now -- not only the take whose saved-take card is up.
        mainScreen.setCopyToThisCardVisible (! isRecording && quitPendingNote.isEmpty()
                                             && application.getTakesWaitingForAnotherCard() > 0);

        watchTake (isRecording);

        // A take can also end without the record button: §6.5 stops one when
        // the card fills or is pulled. The notice belongs to the stop, not to
        // the press, so it is picked up here too.
        showSavedTake();

        // Recovery now runs away from the message thread. The constructor's
        // first attempt usually sees it in flight; this slow tick presents the
        // combined result once both roots have settled.
        showRecoveredTakes();

        // After the recovery card has had its turn: an interrupted take is the
        // more urgent thing to hear about, and two things at once is neither.
        openSetupGuideOnFirstLaunch();

        if (guideVisible || guideSuspended)
        {
            guideSawRecording = guideSawRecording || isRecording;
            guideTakeSaved = guideTakeSaved || (guideSawRecording && ! isRecording);
        }

        if (guideVisible)
            refreshSetupGuide();

        const auto reason = application.getRecordDisabledReason();
        mainScreen.setRecordButtonEnabled (reason.isEmpty(), reason);

        // §5.4: if the low-latency monitor path could not be obtained, say so.
        // §5.3: otherwise, if no headphone output could be chosen, say that.
        // A §5 runaway cut outranks both: the user is sitting in silence and
        // the line has to say why and what to press.
        auto monitorProblem = application.getMonitorProblem();

        if (auto* bus = application.getMonitorBus())
            if (bus->isRunawayMuted())
                monitorProblem = "Sound was cut to protect your ears from feedback. Press Unmute to bring it back.";

        if (monitorProblem.isEmpty())
            monitorProblem = juce::String (application.getOutputSelectionProblem());

        mainScreen.setMonitorProblemText (monitorProblem);

        // §10.5/§6.5/§6.6 guidance. The slow tick consumes background disk
        // snapshots and advances the level detectors.
        mainScreen.setAdviceText (application.pollStatusAdvice (sinceLastStatus));

        mainScreen.setCameraCount (application.getCameraController().getSelection().getEnabledCount());

        // The benchmark behind this finishes on its own thread, so a prompt
        // that was blocked when it opened has to notice when it stops being.
        if (saveLocationPrompt.isVisible())
            saveLocationPrompt.setBlockedReason (application.getRecordDisabledReason());

        if (advancedVisible)
            refreshAdvanced();

        // The main screen carries the pictures, and it is always showing --
        // the Cameras page opens beside it -- so the cameras have to actually
        // be open for it to have anything to show.
        //
        // This opens only what is switched on (a camera is on until it is
        // switched off on the Cameras page), and does not retry one that
        // failed: that retry is the Cameras door's, and switching a camera off
        // and on. A rig with no camera on opens nothing, prompts for nothing,
        // and shows no tiles. applySelection() skips what is already open, so
        // this is a no-op once they are.
        application.openEnabledCameras();
        refreshCameras();
    }
}

void MainComponent::rebindMeters()
{
    mainScreen.setMixMetering (application.getMixMetering());

    const int micCount = application.getIncludedMicCount();
    lastMicCount = micCount;

    // Size the strip set BEFORE binding anything into it.
    //
    // Rebuilding the capture destroys every Metering object the existing strips
    // point at -- CaptureCoordinator::startMonitoring clears and recreates the
    // lot on every call. Binding only the first micCount strips therefore left
    // any surplus strip from a larger set holding a pointer into freed memory,
    // and the strips repaint at 60 Hz, so it was read long before the next
    // timer tick could have tidied up.
    //
    // That window was survivable while the channel count only shrank on an
    // unplug. Adding a checkbox to deselect a microphone made it reachable on
    // demand, and it crashed.
    if (mainScreen.getMicCount() != micCount)
        mainScreen.setMicCount (micCount);

    for (int i = 0; i < micCount; ++i)
    {
        if (auto* meter = mainScreen.getChannelMeter (i))
        {
            auto* metering = application.getChannelMetering (i);
            meter->setMetering (metering);
            meter->setMicName (application.getMicDisplayName (i));

            // §6.5 "meter goes dashed", and §8.1's live numbers. This was never
            // called: noSignal defaults to true, so every meter stayed dashed
            // for the life of the app and both readouts were a hardcoded
            // "--.-" no matter what the microphone was doing.
            //
            // Dashed means the channel is not delivering audio -- either it has
            // no meter bound at all, or §6.5 has it writing silence because its
            // microphone was unplugged mid-take. A connected microphone in a
            // quiet room is not dashed: it reads its real level, which is the
            // difference between "nobody is talking" and "this is not working".
            meter->setNoSignal (metering == nullptr || ! application.isMicLive (i));

            // The faint second line under the name: the hardware's own product
            // string. The strip has always reserved and painted this row and
            // setDeviceName() had no callers, so it drew an empty line on every
            // channel for the life of the app.
            meter->setDeviceName (application.getMicProductName (i));
        }
    }
}

void MainComponent::refreshAdvanced()
{
    advancedPanel.setSampleRates (application.getAvailableSampleRates(),
                                  static_cast<uint32_t> (application.getSampleRate() + 0.5));
    advancedPanel.setSampleRateSelection (application.getSampleRateOverride());
    advancedPanel.setBitDepthChoice (application.getBitDepth());
    advancedPanel.setBufferSizeChoice (application.getCurrentBufferSize(),
                                       application.getBufferSizeOverride());
    advancedPanel.setMeasuredLatency (application.getMeasuredLatencyMs(), application.getSlowestMicLatencyMs());
    advancedPanel.setActiveBackendDescription (application.getActiveBackendDescription());
    advancedPanel.setDriftReport (application.getDriftReport());
    advancedPanel.setAggregateStatus (application.getAggregateStatus());
    advancedPanel.setAggregateName (application.getAggregateDeviceName());
    advancedPanel.setDestinationFolderText ("Destination folder: " + application.getDestinationFolder());
    advancedPanel.setMirrorEnabled (application.isMirrorEnabledByUser());
    advancedPanel.setCopyToCardEnabled (application.isRecordOnComputerThenCopy());
    advancedPanel.setUpdateState (application.getCheckForUpdates(),
                                  application.getUpdateStatusText(),
                                  application.isUpdateAvailable(),
                                  ! application.isRecording() && ! application.isUpdateCheckRunning());
    advancedPanel.setDeliveryTargets (Application::getDeliveryTargetNames(),
                                      application.getDeliveryTarget());
    // The measured figure leads the advice it is based on. getLoudnessReading()
    // was written, and never called from anywhere -- so the panel gave
    // instructions ("Turn up by 3.2 dB") with no way to see the number behind
    // them, while its own header promised the line that says what the mix
    // measures. It returns empty when there is not enough to judge, which is
    // the same condition under which the advice says so itself.
    {
        const auto reading = application.getLoudnessReading();
        const auto advice = application.getLoudnessAdvice();

        advancedPanel.setLoudnessAdvice (reading.isNotEmpty() && advice.isNotEmpty()
                                             ? reading + ". " + advice
                                             : (reading.isNotEmpty() ? reading : advice));
    }
    advancedPanel.setActivityLines (application.getRecentActivityLines());

    // They are on screen, so they have been shown. Without this the advice line
    // above the record button would go on announcing entries the user is
    // already looking at.
    application.markActivitySeen();

    juce::StringArray outputs;
    for (const auto& name : application.getOutputDeviceNames())
        outputs.add (juce::String (name));

    advancedPanel.setOutputDevices (outputs,
                                    juce::String (application.getSelectedOutputDeviceName()));

    std::vector<AdvancedPanel::HeadphoneChoice> headphones;
    for (const auto& h : application.getHeadphoneChoices())
        headphones.push_back ({ h.key, h.displayName, h.on });
    advancedPanel.setHeadphoneChoices (headphones);

    const int micCount = application.getIncludedMicCount();
    juce::StringArray micNames;
    for (int i = 0; i < micCount; ++i)
        micNames.add (application.getMicDisplayName (i));

    std::vector<AdvancedPanel::MicChoice> micSelections;
    for (const auto& m : application.getMicSelections())
    {
        // An interface is one row, because it is switched on and off as one
        // thing -- but it is several microphones, and the row has to say so.
        // A user with two people plugged into one interface saw a single line
        // here and read it as the app refusing to take their second mic.
        auto label = m.displayName;

        if (m.channelCount > 1)
            label += " (" + juce::String (m.channelCount) + " microphones)";

        AdvancedPanel::MicChoice choice;
        choice.label = label;
        choice.deviceKey = m.deviceKey; // what the app matches back on: identity, not the label
        choice.enabled = m.enabled;

        for (const auto& in : m.inputs)
            choice.inputs.push_back ({ in.index, in.label, in.enabled });

        micSelections.push_back (std::move (choice));
    }
    advancedPanel.setMicSelections (micSelections);

    // Adding or removing a microphone changes how tall the panel needs to be,
    // and only this component can resize it inside its viewport. Guarded on the
    // height actually changing, because refreshAdvanced runs on the UI tick and
    // relaying out the whole panel twice a second would be churn for nothing.
    const int requiredHeight = advancedPanel.getRequiredHeight();

    if (requiredHeight != lastAdvancedHeight)
    {
        lastAdvancedHeight = requiredHeight;
        resized();
    }

    std::vector<AdvancedPanel::VolumeChoice> volumes;
    for (const auto& v : application.getStorageVolumes())
        volumes.push_back ({ v.displayName, v.path, v.isCurrent });
    advancedPanel.setStorageVolumes (volumes);


    // Rebuilt only when the rows themselves change: doing it every tick would
    // reset a slider out from under the user mid-drag. Keyed on each row's
    // physical input and name, not the count -- a rename kept the old name on
    // its row, and two microphones swapping places left a slider labelled for
    // one driving the other's trim.
    const auto trimOf = [this] (int i) { return application.getChannelTrimDb (i); };
    juce::StringArray trimRows;

    for (int i = 0; i < micCount; ++i)
    {
        const auto target = application.getMicRenameTarget (i);
        trimRows.add (juce::String (target.deviceKey) + ":" + juce::String (target.deviceChannel)
                      + "|" + micNames[i]);
    }

    if (trimRows != lastAdvancedTrimRows)
    {
        advancedPanel.setTrimChannels (micNames, trimOf);
        lastAdvancedTrimRows = trimRows;
    }
    else
    {
        advancedPanel.setTrimValues (trimOf);
    }
}

void MainComponent::toggleCameras()
{
    cameraVisible = ! cameraVisible;

    if (cameraVisible)
    {
        // One drawer at a time, as for Settings and Help. The Cameras page
        // sits beside the main screen like them, so the pictures it is about
        // stay in view while their size is chosen.
        advancedVisible = false;
        helpVisible = false;
        suspendSetupGuide();

        // Opening the door is the user asking about the cameras, which is the
        // moment to look for new ones and to try again any that failed to
        // open -- and on macOS the moment to spend the privacy prompt, with
        // the reason on screen beside it.
        application.getCameraController().refreshCameras();
        application.openEnabledCameras (true);
    }
    else if (! anyDrawerOpen())
    {
        resumeSetupGuideIfSuspended();
    }

    applyPanelVisibility();
    refreshCameras();

    // Back to the top on entry, and wide enough for both halves, as Settings.
    if (cameraVisible)
    {
        cameraViewport.setViewPosition (0, 0);
        growWindowToFitWidth (kMainMinWidth + drawerWidth());
    }

    resized();
}

void MainComponent::refreshCameras()
{
    auto& controller = application.getCameraController();

    if (controller.applyPendingCameraList())
        application.announceCameraChanges();

    // A take with zero successfully opened camera writers is still a take.
    // Camera controls must stay frozen until the audio recording ends, or an
    // apparent late camera change would not be represented in that take.
    if (cameraVisible)
    {
        cameraPanel.setUnavailableReason (controller.getUnavailableReason());
        cameraPanel.setProblemText (controller.getProblem());
        cameraPanel.setRecording (application.getRecordingEngine().getState() == RecordingState::Recording);
        cameraPanel.setCombineVideoState (application.getCombineVideoAndAudio(),
                                          application.getCombineUnavailableReason());
        cameraPanel.setSpaceText (application.recordingSpaceLine(), application.nextTakeRoomNote());
        cameraPanel.setLookingForCameras (! controller.hasLookedForCameras());
    }

    std::vector<CameraPanel::CameraRow> cameras;
    std::map<std::string, CameraController::TakeCameraState> takeCameraStates;

    if (application.getRecordingEngine().getState() == RecordingState::Recording)
        for (const auto& camera : controller.getTakeCameraStates())
            takeCameraStates[camera.id] = camera;

    for (const auto& camera : controller.getSelection().getAvailableCameras())
    {
        const auto takeState = takeCameraStates.find (camera.id);
        cameras.push_back ({ camera.id,
                             juce::String (controller.getSelection().getDisplayName (camera.id)),
                             controller.getSelection().isEnabled (camera.id),
                             true,
                             false,
                             takeState != takeCameraStates.end() && takeState->second.recording,
                             takeState != takeCameraStates.end() && takeState->second.starting,
                             controller.getPlannedFileNameFor (camera.id),
                             controller.getViewerRevision (camera.id),
                             controller.getSignalStatusText (camera.id),
                             controller.getSelection().getQuality (camera.id),
                             controller.getActiveFormatText (camera.id) });
    }

    // Keep remembered rows actionable while the asynchronous first snapshot
    // runs. If a platform driver wedges, the user can still switch its camera
    // off and record sound; after the bounded grace period this becomes the
    // ordinary unavailable row while discovery continues in the background.
    for (const auto& camera : controller.getSelection().getUnavailableEnabledCameras())
    {
        const auto takeState = takeCameraStates.find (camera.id);
        cameras.push_back ({ camera.id,
                             juce::String (camera.displayName),
                             true,
                             false,
                             controller.isInitialDiscoveryPending(),
                             takeState != takeCameraStates.end() && takeState->second.recording,
                             takeState != takeCameraStates.end() && takeState->second.starting,
                             controller.getPlannedFileNameFor (camera.id),
                             controller.getViewerRevision (camera.id),
                             // Was hardcoded empty, which is what stopped the
                             // incomplete-file warning reaching an unplugged
                             // camera -- the one case that truncates a movie.
                             controller.getSignalStatusText (camera.id),
                             controller.getSelection().getQuality (camera.id),
                             {} });
    }

    // The page is fed only while it is open; it is read again on the way in.
    if (cameraVisible)
        cameraPanel.setCameras (cameras);

    // The main screen shows only what is switched on: a tile per camera that is
    // actually going into the take. The off ones are a settings question, and
    // settings live behind the door. Every drawer -- Cameras included -- sits
    // beside the main screen rather than over it, so the tiles always stay.
    {
        std::vector<MainScreen::CameraTile> tiles;

        for (const auto& camera : cameras)
            if (camera.enabled && camera.available)
                tiles.push_back ({ camera.id, camera.displayName, camera.viewerRevision,
                                   camera.signalStatusText,
                                   camera.recordingThisTake,
                                   camera.startingThisTake });

        const int before = mainScreen.getPreferredHeight();
        mainScreen.setCameraTiles (tiles);

        if (mainScreen.getPreferredHeight() != before)
        {
            // Switching a camera on is a request to SEE it. Without this the
            // window keeps the height an audio-only rig needed, and the picture
            // is squeezed into whatever was left -- 352px wide in the window
            // this opens at, which is not a preview anyone can frame a shot in.
            growWindowToFitMainScreen();
            resized();
        }
    }

    const int requiredHeight = cameraPanel.getRequiredHeight();

    if (requiredHeight != lastCameraHeight)
    {
        lastCameraHeight = requiredHeight;
        resized();
    }
}

void MainComponent::growWindowToFitMainScreen()
{
    growWindowToFit (mainScreen.getPreferredHeight());
}

// The part of the screen the window's CONTENT may occupy: the display the
// window is on now, not the primary one -- fitting against the laptop's screen
// threw a window on an external display back onto the laptop every time
// Settings or Help opened -- and below the native title bar, which on the Mac
// sits outside the component's bounds. Growing to the full height put the
// title bar, and with it the close button and the drag area, under the menu
// bar where it could not be reached.
juce::Rectangle<int> MainComponent::usableAreaForWindow (juce::Component& window)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect (window.getScreenBounds());
    if (display == nullptr)
        display = displays.getPrimaryDisplay();

    auto usable = display != nullptr ? display->userArea : juce::Rectangle<int> (0, 0, 1180, 900);

    if (auto* peer = window.getPeer())
        if (const auto frame = peer->getFrameSizeIfPresent())
            usable = frame->subtractedFrom (usable);

    return usable;
}

void MainComponent::growWindowToFit (int contentHeight)
{
    // The window, not this component: setSize() on a DocumentWindow's content
    // resizes the frame around it, which is the thing with the title bar and
    // the position on screen.
    auto* window = getTopLevelComponent();

    // Before this component is put inside a window, the top level IS this
    // component, and resizing it here would fight the owner that is about to
    // size it. Nothing to grow until there is a frame to grow.
    if (window == nullptr || window == this)
        return;

    const auto usable = usableAreaForWindow (*window);

    // What the window would have to be for the picture to come out at the size
    // the user picked. The frame is taller than its content by the title bar,
    // so the difference is measured rather than assumed -- a guess here is a
    // few pixels of clipping on one platform and not the other.
    const int chrome = juce::jmax (0, window->getHeight() - getHeight());
    const int wanted = contentHeight + 24 + chrome;

    // Only ever grows. A camera switched off leaves the window where it is:
    // shrinking it would throw away a size the user may have set by hand, and
    // "it resized itself smaller behind my back" is worse than a little slack.
    const int height = juce::jlimit (window->getHeight(),
                                     juce::jmax (window->getHeight(), usable.getHeight()),
                                     wanted);

    if (height == window->getHeight())
        return;

    window->setSize (window->getWidth(), height);

    // Keep it on the screen. Growing from a window already near the bottom
    // would otherwise push its lower edge -- and the record button with it --
    // past the edge of the display.
    const int top = juce::jlimit (usable.getY(),
                                  juce::jmax (usable.getY(), usable.getBottom() - height),
                                  window->getY());

    window->setTopLeftPosition (window->getX(), top);
}

void MainComponent::toggleAdvanced()
{
    advancedVisible = ! advancedVisible;

    if (advancedVisible)
    {
        // One drawer at a time: Help and the Cameras page close.
        helpVisible = false;
        cameraVisible = false;
        suspendSetupGuide();

        refreshShows();
        refreshAdvanced();
    }
    else if (! anyDrawerOpen())
    {
        resumeSetupGuideIfSuspended();
    }

    applyPanelVisibility();
    refreshCameras();

    // Back to the top on entry, so opening Settings never starts halfway down
    // wherever it was last left. And wide enough for both halves.
    if (advancedVisible)
    {
        advancedViewport.setViewPosition (0, 0);
        growWindowToFitWidth (kMainMinWidth + drawerWidth());
    }

    resized();
}

void MainComponent::toggleHelp()
{
    helpVisible = ! helpVisible;

    if (helpVisible)
    {
        advancedVisible = false;
        cameraVisible = false;
        suspendSetupGuide();
    }
    else if (! anyDrawerOpen())
    {
        resumeSetupGuideIfSuspended();
    }

    applyPanelVisibility();
    refreshCameras();

    if (helpVisible)
    {
        helpViewport.setViewPosition (0, 0);
        growWindowToFitWidth (kMainMinWidth + drawerWidth());

        // The text wraps to the width, so the width has to be known before
        // the height can be asked for.
        helpPanel.setSize (juce::jmax (1, drawerWidth() - helpViewport.getScrollBarThickness()),
                           juce::jmax (1, helpPanel.getHeight()));
    }

    resized();
}

SetupGuideRig MainComponent::readSetupGuideRig() const
{
    SetupGuideRig rig;
    rig.micCount = application.getIncludedMicCount();

    for (const auto& m : application.getMicSelections())
        rig.switchedOffMicCount += m.enabled ? 0 : 1;

    rig.headphoneJackCount = static_cast<int> (application.getHeadphoneChoices().size());
    // With no microphone there is no combined device yet to be monitoring
    // through, which is not the same as the user having picked something else.
    rig.mixGoesToEveryMic = application.isMonitoringThroughCombinedDevice() || rig.micCount == 0;

    const auto& selection = application.getCameraController().getSelection();
    rig.cameraCount = static_cast<int> (selection.getAvailableCameras().size());
    rig.camerasOn = selection.getEnabledCount();

    rig.recording = application.getRecordingEngine().getState() == RecordingState::Recording;
    rig.recordingSeconds = rig.recording ? application.getElapsedRecordingSeconds() : 0.0;
    rig.testTakeSaved = guideTakeSaved;
    rig.recordBlockedReason = application.getRecordDisabledReason().toStdString();
    return rig;
}

void MainComponent::refreshSetupGuide()
{
    const auto rig = readSetupGuideRig();

    std::vector<SetupGuidePanel::MicRow> mics;
    for (int i = 0; i < rig.micCount; ++i)
        mics.push_back ({ application.getMicDisplayName (i), application.getMicProductName (i) });
    guidePanel.setMicrophones (mics);

    std::vector<SetupGuidePanel::HeadphoneRow> headphones;
    for (const auto& h : application.getHeadphoneChoices())
        headphones.push_back ({ h.key, h.displayName, h.on });
    guidePanel.setHeadphones (headphones);

    guidePanel.setDestinationFolder (application.getDestinationFolder());
    guidePanel.setHighlightedMic (application.getTappedChannel());
    guidePanel.setPage (setupGuide.page (rig));

    // A microphone plugged in on the first page adds a row; only this
    // component can make the panel taller inside its viewport.
    const int requiredHeight = guidePanel.getRequiredHeight();

    if (requiredHeight != lastGuideHeight)
    {
        lastGuideHeight = requiredHeight;
        resized();
    }
}

void MainComponent::openSetupGuide()
{
    // One drawer at a time, as for Settings, Help and Cameras.
    advancedVisible = false;
    helpVisible = false;
    cameraVisible = false;

    guideVisible = true;
    guideSuspended = false;
    guideSawRecording = false;
    guideTakeSaved = false;
    setupGuide.restart();

    applyPanelVisibility();
    refreshCameras();
    refreshSetupGuide();

    guideViewport.setViewPosition (0, 0);
    growWindowToFitWidth (kMainMinWidth + drawerWidth());
    resized();
}

void MainComponent::closeSetupGuide()
{
    guideVisible = false;
    guideSuspended = false;

    // Skipped counts as done. Someone who skipped it has seen where it lives,
    // and opening it again on every launch would be nagging.
    application.markSetupGuideDone();

    applyPanelVisibility();
    refreshCameras();
    resized();
    grabKeyboardFocus();
}

void MainComponent::suspendSetupGuide()
{
    if (! guideVisible)
        return;

    guideVisible = false;
    guideSuspended = true;
}

void MainComponent::resumeSetupGuideIfSuspended()
{
    if (! guideSuspended)
        return;

    guideSuspended = false;
    guideVisible = true;
    refreshSetupGuide();
    growWindowToFitWidth (kMainMinWidth + drawerWidth());
}

void MainComponent::openSetupGuideOnFirstLaunch()
{
    if (guideAutoOpenConsidered || application.isSetupGuideDone())
        return;

    // The end-to-end scripts drive a fresh profile by clicking fixed points
    // on the main screen; the drawer opening beside it moves them. Those
    // scripts opt out here; the UI walker covers the guide itself.
    if (juce::SystemStats::getEnvironmentVariable ("MMA_SKIP_SETUP_GUIDE", {}).isNotEmpty())
    {
        guideAutoOpenConsidered = true;
        return;
    }

    // Not while the window is still being built (there is no frame yet to
    // widen for the drawer), and never over something more pressing: a card,
    // a take, or a recovery scan whose card may be about to appear.
    if (! isShowing()
        || application.isRecoveryScanPending()
        || application.getRecordingEngine().getState() == RecordingState::Recording
        || saveLocationPrompt.isVisible() || savedTakePanel.isVisible()
        || recoveredTakesPanel.isVisible() || takeAlertCard.isVisible()
        || anyDrawerOpen())
        return;

    // Once a run, so closing it and carrying on is not undone half a second
    // later if saving the answer failed.
    guideAutoOpenConsidered = true;
    openSetupGuide();
}

void MainComponent::exportDiagnostics()
{
    // §11: logs, recent session.json files and the device inventory. Never audio.
    const auto destination = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                 .getNonexistentChildFile ("SobStage-diagnostics", ".zip");
    application.exportDiagnostics (destination);
}

int MainComponent::drawerWidth() const
{
    // Two fifths of the window, within limits: narrower than 380 the Settings
    // rows squash their pickers, wider than 500 the main screen pays for room
    // the drawer does not use.
    return juce::jlimit (380, 500, getWidth() * 2 / 5);
}

void MainComponent::applyPanelVisibility()
{
    // The main screen is always showing. Every drawer -- Settings, Help, the
    // setup guide and the Cameras page -- opens beside it, one at a time.
    mainViewport.setVisible (true);
    cameraViewport.setVisible (cameraVisible);
    advancedViewport.setVisible (advancedVisible);
    helpViewport.setVisible (helpVisible);
    guideViewport.setVisible (guideVisible);

    mainScreen.setDoorsOpen (advancedVisible, helpVisible, cameraVisible);

    updateWindowMinimumSize();
}

void MainComponent::updateWindowVisibility()
{
    // Minimised (every platform, through the peer), or on macOS completely
    // covered, on another Space or on a sleeping display: the 60 Hz meters
    // were repainting for nobody the whole time, at real battery cost on a
    // laptop running a long show.
    bool visible = isShowing();

    if (visible)
        if (auto* peer = getPeer())
            visible = ! peer->isMinimised() && isNativeWindowVisible (peer->getNativeHandle());

    if (visible == windowVisible)
        return;

    windowVisible = visible;
    mainScreen.setMetersPaused (! visible);

    // Back in view: the whole window at once, so the meters, and a flashing
    // card or banner in its current phase, are there in the first frame.
    if (visible)
        repaint();
}

void MainComponent::updateWindowMinimumSize()
{
    auto* window = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent());
    if (window == nullptr)
        return;

    auto* constrainer = window->getConstrainer();
    if (constrainer == nullptr)
        return;

    if (windowBaseMinimumWidth < 0)
    {
        windowBaseMinimumWidth = constrainer->getMinimumWidth();
        windowBaseMinimumHeight = constrainer->getMinimumHeight();
    }

    const auto usable = usableAreaForWindow (*window);

    // The drawer never goes below 380px, so a window dragged down to its own
    // 560px minimum with Settings open left the main screen 179px wide: the
    // record button hung off its left edge and the header buttons sat on top
    // of each other. The limit is raised for as long as a drawer is open.
    int minimumWidth = windowBaseMinimumWidth;

    if (anyDrawerOpen())
    {
        const int chrome = juce::jmax (0, window->getWidth() - getWidth());

        // Never more than the display has room for, or a small screen could
        // not hold the window at all; there the drawer gets what it can.
        minimumWidth = juce::jmax (windowBaseMinimumWidth,
                                   juce::jmin (kMainMinWidth + 380 + 1, usable.getWidth()) + chrome);
    }

    // A full-window card lays itself out in the window's height less 32px.
    // Each grows the window as it opens, but the window could then be dragged
    // shorter, and the card's last rows -- its buttons, Start and Stop among
    // them -- were squeezed to nothing. While one is up, it sets the floor.
    int cardHeight = 0;

    for (auto* card : std::initializer_list<const ModalCard*> { &saveLocationPrompt, &savedTakePanel,
                                                                &recoveredTakesPanel, &takeAlertCard })
        if (card->isVisible())
            cardHeight = juce::jmax (cardHeight, card->getRequiredHeight() + 32);

    int minimumHeight = windowBaseMinimumHeight;

    if (cardHeight > 0)
    {
        const int chrome = juce::jmax (0, window->getHeight() - getHeight());
        minimumHeight = juce::jmax (windowBaseMinimumHeight,
                                    juce::jmin (cardHeight, usable.getHeight()) + chrome);
    }

    // Only the limits: the constrainer is consulted on the next drag. The
    // window itself was already grown, kept on the screen, by
    // growWindowToFitWidth()/growWindowToFit(), which a forced resize from
    // here would not do.
    if (constrainer->getMinimumWidth() != minimumWidth)
        constrainer->setMinimumWidth (minimumWidth);

    if (constrainer->getMinimumHeight() != minimumHeight)
        constrainer->setMinimumHeight (minimumHeight);
}

void MainComponent::growWindowToFitWidth (int contentWidth)
{
    auto* window = getTopLevelComponent();

    if (window == nullptr || window == this)
        return;

    const auto usable = usableAreaForWindow (*window);

    const int chrome = juce::jmax (0, window->getWidth() - getWidth());
    const int wanted = contentWidth + chrome;

    // Only ever grows, like the height. Shrinking would undo a width the user
    // set by hand.
    const int width = juce::jlimit (window->getWidth(),
                                    juce::jmax (window->getWidth(), usable.getWidth()),
                                    wanted);

    if (width == window->getWidth())
        return;

    window->setSize (width, window->getHeight());

    const int left = juce::jlimit (usable.getX(),
                                   juce::jmax (usable.getX(), usable.getRight() - width),
                                   window->getX());

    window->setTopLeftPosition (left, window->getY());
}

void MainComponent::setQuitPendingNote (const juce::String& note)
{
    if (note == quitPendingNote)
        return;

    quitPendingNote = note;

    // Now rather than at the next status tick, so the line appears with the
    // answer that caused it. During a take the line belongs to the files.
    if (! application.isRecording() && note != lastSavingLine)
    {
        lastSavingLine = note;
        mainScreen.setFilesBeingSavedText (note);
    }

    // Every card covers that line, and one is usually up when this is said:
    // "Stop and Quit" stops the take, and its saved-take card opens behind
    // the "Still saving" question; Cmd-Q pressed from the saved-take card
    // leaves it up. With the sentence on that line alone, the screen looked
    // unchanged until Done -- the pending quit that seemed ignored, which is
    // what the line is for. So each card says it too, at its foot. Hidden
    // cards as well, so one that opens during the wait already does.
    for (auto* card : std::initializer_list<ModalCard*> { &saveLocationPrompt, &savedTakePanel,
                                                          &recoveredTakesPanel, &takeAlertCard })
    {
        card->setFootnote (note);

        // The card grew by a line; the window grows with it, as it does when
        // a card opens, so the card's buttons are not squeezed to make room.
        if (card->isVisible())
            growWindowToFit (card->getRequiredHeight() + 32);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    // The drawer's edge: one hairline where the main screen ends and the
    // drawer begins, so the two read as a screen and a drawer rather than one
    // wide screen with a seam in it.
    if (anyDrawerOpen())
    {
        g.setColour (AppLookAndFeel::outline);
        g.fillRect (mainViewport.getRight(), 0, 1, getHeight());
    }
}

void MainComponent::resized()
{
    // Taken before the drawer comes off the side: the cards below cover the
    // whole window, drawer included.
    const auto full = getLocalBounds();
    auto bounds = full;

    // The drawer takes the right-hand side only while one is open; otherwise
    // the main screen has the whole window, as before.
    auto drawer = juce::Rectangle<int>();

    if (anyDrawerOpen())
    {
        drawer = bounds.removeFromRight (drawerWidth());
        drawer.removeFromLeft (1); // the hairline paint() draws
    }

    mainViewport.setBounds (bounds);
    advancedViewport.setBounds (drawer);
    cameraViewport.setBounds (drawer);
    helpViewport.setBounds (drawer);
    guideViewport.setBounds (drawer);

    // The modal cards cover whichever screen is underneath, so they follow the
    // window rather than the viewport they happen to be over.
    saveLocationPrompt.setBounds (full);
    savedTakePanel.setBounds (full);
    recoveredTakesPanel.setBounds (full);
    takeAlertCard.setBounds (full);
    takeBanner.setBounds (full);

    // Each screen is laid out at least as tall as its content needs, and at
    // least as tall as the window -- so a short window scrolls and a tall one
    // does not leave the content floating in a strip at the top. The width
    // excludes the scrollbar when one is (or is about to be) showing, or the
    // content would sit underneath it. Asking isVerticalScrollBarShown() alone
    // missed the first layout: the bar only appears after setSize() makes the
    // content taller, and then covered the right edge of every row.
    const auto fit = [] (juce::Viewport& viewport, juce::Component& content, int requiredHeight)
    {
        const bool needsBar = viewport.isVerticalScrollBarShown()
                           || requiredHeight > viewport.getHeight();
        const int width = viewport.getWidth() - (needsBar ? viewport.getScrollBarThickness() : 0);

        content.setSize (juce::jmax (1, width),
                         juce::jmax (viewport.getHeight(), requiredHeight));
    };

    // Before fit(), not after: the camera picture is sized against this, and
    // fit() then asks how tall the content wants to be. Telling it afterwards
    // would measure the picture against the previous window size for one frame,
    // which is visible as a jump every time the window is dragged.
    mainScreen.setVisibleHeight (mainViewport.getHeight());

    fit (mainViewport, mainScreen, mainScreen.getRequiredHeight());
    fit (advancedViewport, advancedPanel, advancedPanel.getRequiredHeight());

    // Width first, then height, as for Help below: the page's lines wrap, so
    // what it needs depends on what it is given across.
    cameraPanel.setSize (juce::jmax (1, cameraViewport.getWidth() - cameraViewport.getScrollBarThickness()),
                         juce::jmax (1, cameraPanel.getHeight()));
    fit (cameraViewport, cameraPanel, cameraPanel.getRequiredHeight());

    // Width first, then height: the help text wraps, so what it needs
    // depends on what it is given across.
    helpPanel.setSize (juce::jmax (1, helpViewport.getWidth() - helpViewport.getScrollBarThickness()),
                       juce::jmax (1, helpPanel.getHeight()));
    fit (helpViewport, helpPanel, helpPanel.getRequiredHeight());

    guidePanel.setSize (juce::jmax (1, guideViewport.getWidth() - guideViewport.getScrollBarThickness()),
                        juce::jmax (1, guidePanel.getHeight()));
    fit (guideViewport, guidePanel, guidePanel.getRequiredHeight());
}

} // namespace mma
