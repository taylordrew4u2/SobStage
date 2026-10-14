#include "MainScreen.h"
#include "AppLookAndFeel.h"
#include <array>

namespace mma {

namespace {
    // 16:9 wells, sized as a share of the width the screen actually has rather
    // than as fixed pixel counts.
    //
    // A pixel table was the wrong shape for this control. It made "as large as
    // it goes" a number, so a bigger window bought only more empty space around
    // the same picture -- which is the opposite of what dragging a window
    // larger is asking for. As a fraction, the picture grows with the window
    // and the arrows choose how much of it to spend on the shot.
    //
    // The bottom of the range is still small, because a four-camera rig that
    // wants everything on one row has not gone away; the top is the whole
    // width, which is what someone who has made the window big wanted from it.
    constexpr double kCameraWidthFractions[] = { 0.22, 0.32, 0.45, 0.62, 0.80, 1.00 };
    constexpr int kCameraScaleSteps = static_cast<int> (std::size (kCameraWidthFractions));

    constexpr int kCameraCaptionHeight = 16;
    constexpr int kCameraRowGap       = 12;
    constexpr int kCameraTileGap      = 8;

    // The arrows and their label, on their own line above the pictures.
    constexpr int kCameraControlHeight = 22;

    int tileHeightFor (int tileWidth) noexcept
    {
        // 16:9, which is what every webcam this is likely to meet delivers.
        return (tileWidth * 9) / 16;
    }
}

const juce::Colour MainScreen::kBackground { palette::background };

MainScreen::MainScreen()
{
    // The arrows. Small, beside the pictures they resize, and hidden entirely
    // when there are no pictures -- a size control for nothing is clutter.
    cameraSizeLabel.setText ("Camera size", juce::dontSendNotification);
    cameraSizeLabel.setFont (juce::Font (12.0f));
    cameraSizeLabel.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    cameraSizeLabel.setJustificationType (juce::Justification::centredRight);
    addChildComponent (cameraSizeLabel);

    cameraSmallerButton.onClick = [this] {
        setCameraScale (cameraScale - 1);
        if (onCameraScaleChanged) onCameraScaleChanged (cameraScale);
    };
    cameraLargerButton.onClick = [this] {
        setCameraScale (cameraScale + 1);
        if (onCameraScaleChanged) onCameraScaleChanged (cameraScale);
    };
    cameraSmallerButton.setTitle ("Make camera previews smaller");
    cameraLargerButton.setTitle ("Make camera previews larger");

    for (auto* b : { &cameraSmallerButton, &cameraLargerButton })
    {
        b->setColour (juce::TextButton::buttonColourId, AppLookAndFeel::surfaceHigh);
        addChildComponent (*b);
    }

    cameraSmallerButton.setEnabled (cameraScale > 0);
    cameraLargerButton.setEnabled (cameraScale < kCameraScaleSteps - 1);

    addAndMakeVisible (mixBar);

    // The one thing on this screen someone came here to press. Flat and
    // in-palette like everything else, but filled with the accent so it is
    // unmistakably the primary action -- minimal is not the same as invisible,
    // and before this it read as another dark slab.
    recordButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    recordButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    recordButton.onClick = [this] { if (onRecordButtonClicked) onRecordButtonClicked(); };
    addAndMakeVisible (recordButton);

    elapsedLabel.setJustificationType (juce::Justification::centred);
    remainingLabel.setJustificationType (juce::Justification::centred);
    saveLocationLabel.setJustificationType (juce::Justification::centred);

    // A quiet supporting tier under the record button, rather than three lines
    // competing with it and with each other at the same weight.
    for (auto* l : { &elapsedLabel, &remainingLabel, &saveLocationLabel, &filesSavingLabel })
    {
        l->setFont (juce::Font (12.0f));
        l->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    }
    addAndMakeVisible (elapsedLabel);
    addAndMakeVisible (remainingLabel);
    addAndMakeVisible (saveLocationLabel);

    // Green rather than the supporting grey the line above it uses: while a
    // take is running this is the one status line saying the thing the user
    // most wants to be true, which is that files are appearing.
    filesSavingLabel.setColour (juce::Label::textColourId, AppLookAndFeel::meterLow);
    addAndMakeVisible (filesSavingLabel);

    copyToThisCardButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::surfaceHigh);
    copyToThisCardButton.onClick = [this] { if (onCopyToThisCardClicked) onCopyToThisCardClicked(); };
    addChildComponent (copyToThisCardButton);

    noMicsLabel.setText ("Plug in a USB microphone or audio interface to get started.",
                         juce::dontSendNotification);
    noMicsLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (noMicsLabel);

    adviceLabel.setJustificationType (juce::Justification::centred);
    adviceLabel.setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    adviceLabel.setFont (juce::Font (12.0f));
    adviceLabel.setMinimumHorizontalScale (1.0f); // wrap, never squash or cut
    addChildComponent (adviceLabel);

    monitorProblemLabel.setJustificationType (juce::Justification::centred);
    // Was JUCE's stock orange at the default size, running the full width of the
    // window in one thin line. Larger now, and allowed to wrap so it reads as a
    // sentence rather than a ticker.
    //
    // Warning amber rather than the accent: the accent fills the record button,
    // so painting a problem in it said "press me" in the colour of the thing
    // that had just gone wrong.
    monitorProblemLabel.setColour (juce::Label::textColourId, AppLookAndFeel::warning);
    monitorProblemLabel.setFont (juce::Font (13.0f));
    addChildComponent (monitorProblemLabel);

    // Why the record button will not go. Warning amber and small, matching the
    // monitor-problem line it sits next to, rather than primary-text white,
    // which read as an ordinary status line.
    disabledReasonLabel.setJustificationType (juce::Justification::centred);
    disabledReasonLabel.setColour (juce::Label::textColourId, AppLookAndFeel::warning);
    disabledReasonLabel.setFont (juce::Font (12.0f));
    disabledReasonLabel.setMinimumHorizontalScale (1.0f); // wrap, never squash or cut
    addChildComponent (disabledReasonLabel);

    // The bare number told a user nothing about what it controlled. Naming it
    // inside the value box costs no layout and needs no separate label; a
    // suffix would read "70 monitor", so the name goes in front.
    volumeSlider.textFromValueFunction = [] (double value)
    {
        return "Monitor " + juce::String (juce::roundToInt (value));
    };
    volumeSlider.valueFromTextFunction = [] (const juce::String& text)
    {
        return text.retainCharacters ("0123456789").getDoubleValue();
    };
    volumeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, true, 86, 20);
    volumeSlider.setTitle ("Monitor volume");
    volumeSlider.setDescription ("Adjust headphone volume; recorded levels stay unchanged.");
    volumeSlider.setRange (0.0, 100.0, 1.0);

    // A two-finger scroll over the main screen is someone scrolling it, not
    // turning everyone's headphones up. Drag, click and typing still work.
    volumeSlider.setScrollWheelEnabled (false);
    volumeSlider.setValue (70.0); // §5.1 default
    volumeSlider.onValueChange = [this] { if (onVolumeChanged) onVolumeChanged (volumeSlider.getValue()); };
    addAndMakeVisible (volumeSlider);

    muteButton.setClickingTogglesState (false); // state comes from the bus, not the click
    muteButton.onClick = [this] { if (onMuteToggled) onMuteToggled(); };
    addAndMakeVisible (muteButton);

    // §6.2: naming the take is optional and never a gate -- the placeholder
    // says what happens if it is left alone.
    sessionNameEditor.setTextToShowWhenEmpty ("Session name (optional)", juce::Colours::grey);
    sessionNameEditor.setTitle ("Session name, optional");
    sessionNameEditor.setJustification (juce::Justification::centredLeft);
    // A single-line box keeps the keyboard after Return, so "type the name,
    // press Return, press Space to mute" typed a space into the name and left
    // the room live. Return and Escape end the typing; so does a click away.
    sessionNameEditor.onReturnKey = [this] { finishSessionName(); };
    sessionNameEditor.onEscapeKey = [this] { finishSessionName(); };
    addAndMakeVisible (sessionNameEditor);

    // A click on the background goes to the watcher below rather than handing
    // the keyboard to whichever control JUCE picks as this screen's default.
    setMouseClickGrabsKeyboardFocus (false);
    addMouseListener (&clickAwayWatcher, true);

    advancedButton.onClick = [this] { if (onAdvancedClicked) onAdvancedClicked(); };
    addAndMakeVisible (advancedButton);

    camerasButton.onClick = [this] { if (onCamerasClicked) onCamerasClicked(); };
    addAndMakeVisible (camerasButton);

    helpButton.onClick = [this] { if (onHelpClicked) onHelpClicked(); };
    addAndMakeVisible (helpButton);

    // While a drawer is open its button stays lit, in a lifted grey rather
    // than the accent, which is reserved for the one button that records.
    for (auto* b : { &advancedButton, &helpButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, AppLookAndFeel::surfaceHigh.brighter (0.7f));
        b->setColour (juce::TextButton::textColourOnId, AppLookAndFeel::bone);
    }

    // The masthead. Letter-spaced by hand, because a wordmark is the one piece
    // of type on this screen that is a picture of a word rather than a word.
    brandLabel.setText ("S O B S T A G E", juce::dontSendNotification);
    brandLabel.setFont (juce::Font (16.0f, juce::Font::bold));
    brandLabel.setColour (juce::Label::textColourId, AppLookAndFeel::bone);
    brandLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (brandLabel);

    taglineLabel.setText ("let's give these tears a stage", juce::dontSendNotification);
    taglineLabel.setFont (juce::Font (13.0f));
    taglineLabel.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    taglineLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (taglineLabel);

    versionLabel.setFont (juce::Font (11.0f));
    versionLabel.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    versionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (versionLabel);

    // Small, wide-tracked, tertiary: a heading that labels a group without
    // competing with anything inside it.
    for (auto* l : { &cameraSectionLabel, &micSectionLabel })
    {
        l->setFont (juce::Font (11.0f, juce::Font::bold));
        l->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
        l->setJustificationType (juce::Justification::centredLeft);
        addChildComponent (*l);
    }

    cameraSectionLabel.setText ("C A M E R A", juce::dontSendNotification);
    micSectionLabel.setText ("M I C R O P H O N E S", juce::dontSendNotification);
    micSectionLabel.setVisible (true);

    fullPreviewToggle.onClick = [this]
    { if (onFullPreviewToggled) onFullPreviewToggled (fullPreviewToggle.getToggleState()); };
    addChildComponent (fullPreviewToggle);

    // The door to the camera panel, on the row it belongs to. The old
    // "Cameras" button at the foot of the screen stays for the rig that has
    // none switched on yet -- with no pictures there is no row for this to sit
    // on, and a user with no camera still has to be able to find the panel.
    addCameraButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::surfaceHigh);
    addCameraButton.onClick = [this] { if (onCamerasClicked) onCamerasClicked(); };
    addChildComponent (addCameraButton);
}

void MainScreen::setVersionText (const juce::String& text)
{
    versionLabel.setText (text, juce::dontSendNotification);
}

void MainScreen::paintBrandMark (juce::Graphics& g, juce::Rectangle<float> b) const
{
    // The same four shapes as Tools/make_icon.py, at the size a title bar has.
    const auto cx = b.getCentreX();
    const auto cy = b.getCentreY();
    const auto r = juce::jmin (b.getWidth(), b.getHeight()) * 0.46f;

    g.setColour (AppLookAndFeel::bone);
    g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, juce::jmax (1.2f, r * 0.14f));

    const float eyeR = r * 0.115f;
    const float eyeDx = r * 0.37f;
    const float eyeY = cy - r * 0.22f;
    for (float sx : { -1.0f, 1.0f })
        g.fillEllipse (cx + sx * eyeDx - eyeR, eyeY - eyeR, eyeR * 2.0f, eyeR * 2.0f);

    // The frown: an arc of a circle centred below the face, so what shows is
    // its top edge. Centred above, the identical arc reads as a smile.
    juce::Path mouth;
    const float mouthR = r * 0.45f;
    const float mouthCy = cy + r * 0.70f;
    mouth.addCentredArc (cx, mouthCy, mouthR, mouthR, 0.0f,
                         juce::MathConstants<float>::pi * 1.28f,
                         juce::MathConstants<float>::pi * 1.72f, true);
    g.strokePath (mouth, juce::PathStrokeType (juce::jmax (1.0f, r * 0.12f)));

    // The tear, and the only saturated thing in the mark.
    g.setColour (AppLookAndFeel::accent);
    const float dropR = r * 0.12f;
    const float tx = cx - eyeDx;
    const float ty = eyeY + r * 0.50f;
    g.fillEllipse (tx - dropR, ty - dropR, dropR * 2.0f, dropR * 2.0f);

    juce::Path drop;
    drop.addTriangle (tx, ty - dropR * 2.6f, tx - dropR, ty, tx + dropR, ty);
    g.fillPath (drop);
}

MainScreen::~MainScreen()
{
    removeMouseListener (&clickAwayWatcher);
}

void MainScreen::finishSessionName()
{
    if (! sessionNameEditor.hasKeyboardFocus (true))
        return;

    if (onSessionNameFinished)
        onSessionNameFinished();

    // No owner to hand it to (or it would not take it): at least let go, so
    // the box stops swallowing keys meant for the window.
    if (sessionNameEditor.hasKeyboardFocus (true))
        sessionNameEditor.giveAwayKeyboardFocus();
}

void MainScreen::ClickAwayWatcher::mouseDown (const juce::MouseEvent& e)
{
    auto& editor = owner.sessionNameEditor;

    if (e.eventComponent == &editor || editor.isParentOf (e.eventComponent))
        return;

    owner.finishSessionName();
}

void MainScreen::setMetersPaused (bool paused)
{
    metersPaused = paused;

    for (auto* meter : channelMeters)
        meter->setRepaintPaused (paused);

    mixBar.setRepaintPaused (paused);
}

void MainScreen::repaintMeters()
{
    if (metersPaused)
        return;

    for (auto* meter : channelMeters)
        meter->repaint();

    mixBar.repaint();
}

void MainScreen::setMicCount (int count)
{
    channelMeters.clear();
    for (int i = 0; i < count; ++i)
    {
        auto* meter = new ChannelMeterComponent();
        meter->onNameClicked = [this, i] { if (onMicNameClicked) onMicNameClicked (i); };
        // A hand over the strip: the name is clickable, and nothing on the
        // strip itself said so.
        meter->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        meter->setRepaintPaused (metersPaused);
        addAndMakeVisible (meter);
        channelMeters.add (meter);
    }
    setNoMicsMessage (count == 0);

    // The strips wrap onto more rows as they are added, so the height asked
    // for changes with the count even when the message does not.
    requiredHeightChanged();
}

int MainScreen::getCameraScaleStepCount() noexcept
{
    return kCameraScaleSteps;
}

int MainScreen::monitorProblemHeight() const noexcept
{
    constexpr int kLineHeight = 17;
    constexpr int kOneLine    = 20;

    const auto text = monitorProblemLabel.getText();

    if (text.isEmpty())
        return kOneLine;

    const int width = juce::jmax (1, getWidth() - 32);
    const int textWidth = juce::roundToInt (monitorProblemLabel.getFont()
                                                .getStringWidthFloat (text));

    // The wrap JUCE will apply when it draws, worked out ahead of it so the
    // band reserved is the band needed. Capped: a runaway message must not push
    // the record button off the screen, which is the one control that cannot be
    // allowed to go looking for.
    const int lines = juce::jlimit (1, 4, (textWidth + width - 1) / width);

    return juce::jmax (kOneLine, lines * kLineHeight + 3);
}

int MainScreen::wrappedLineHeight (const juce::Label& label) const noexcept
{
    constexpr int kOneLine  = 20;
    constexpr int kMaxLines = 3;

    const auto text = label.getText().trim();

    if (text.isEmpty())
        return kOneLine;

    const auto font = label.getFont();
    const auto border = label.getBorderSize();
    const float width = (float) juce::jmax (1, (getWidth() > 0 ? getWidth() : 1180) - 32
                                                   - border.getLeftAndRight());
    const float textWidth = font.getStringWidthFloat (text);

    // JUCE's own rule in GlyphArrangement: one line when it fits, otherwise
    // the fewest lines that beat the text's width plus an 80px allowance for
    // ragged breaks. Reserving fewer than it draws squashes the font; more
    // only leaves a gap.
    const int lines = textWidth < width
                          ? 1
                          : juce::jlimit (1, kMaxLines, (int) ((textWidth + 80.0f) / width) + 1);

    if (lines == 1)
        return kOneLine;

    const int lineHeight = (int) std::ceil (font.getHeight()) + 3;
    return lines * lineHeight + border.getTopAndBottom() + 2;
}

int MainScreen::nonCameraHeight() const noexcept
{
    // Every band resized() lays out beneath the pictures. Kept here rather than
    // inline in getRequiredHeight() because the camera sizing needs the same
    // number: the picture is given the height that is left over, and "left
    // over" is only meaningful against this.
    constexpr int kMargins     = 32;   // 16 top and bottom
    constexpr int kHeader      = 40 + 18;
    constexpr int kMicHeading  = 16 + 6;
    constexpr int kStripHeight = 40;
    constexpr int kStripGap    = 12;
    constexpr int kNoMicsBand  = 56;
    constexpr int kActionRow   = 16 + 52;
    // The row under the record button that says why it is off, or the gap left
    // when there is no reason to give. resized() spends one or the other, and a
    // budget that counted neither handed the shortfall to the footer: at
    // exactly this height the mute button came out 13px tall.
    // Measured, like the monitor problem below, because the reason can run to
    // more than one line.
    const int kReasonRow = disabledReasonLabel.isVisible() ? disabledReasonHeight() + 8 : 14;
    // files, monitor problem, advice. The monitor problem and the advice are
    // measured rather than assumed: they grow to fit their sentence, and a band
    // reserved at one line while three are drawn is content spilling past the
    // bottom of the window.
    const int kStatusLines = (copyToThisCardButton.isVisible() ? kCopyToThisCardRow : 18)
                           + monitorProblemHeight() + adviceHeight();
    constexpr int kFooter      = 40;

    // MIX is laid out with the microphones, so it counts towards the wrap.
    const int cells = juce::jmax (1, channelMeters.size() + 1);
    const int available = juce::jmax (1, (getWidth() > 0 ? getWidth() : 1180) - 32);
    const int perRow = juce::jlimit (1, cells, (available + kStripGap) / (190 + kStripGap));
    const int rows = (cells + perRow - 1) / perRow;

    // No microphones is a message rather than a row of strips, and a taller one.
    const int levels = channelMeters.isEmpty() ? kNoMicsBand
                                               : rows * kStripHeight + (rows - 1) * kStripGap;

    return kMargins + kHeader + kMicHeading + levels
         + kActionRow + kReasonRow + kStatusLines + kFooter;
}

int MainScreen::cameraTileWidthFor (int step, int availableWidth,
                                    int availableHeight) const noexcept
{
    const auto fraction = kCameraWidthFractions[juce::jlimit (0, kCameraScaleSteps - 1, step)];

    int width = juce::jmax (72, static_cast<int> (availableWidth * fraction));

    // Then the height it has to fit in. Without this, widening the window makes
    // the picture taller than the window has room for and the record button
    // goes below the fold -- so "make it bigger" would cost the user the one
    // control they came to the screen to press.
    //
    // A non-positive height means the screen has not been laid out yet, and
    // there is nothing to clamp against; the width alone stands until it has.
    if (availableHeight > 0)
    {
        const int rows = juce::jmax (1, cameraRowsNeeded (width, availableWidth));
        const int perRowHeight = (availableHeight - (rows - 1) * kCameraTileGap) / rows
                               - kCameraCaptionHeight;

        if (perRowHeight > 0)
            width = juce::jmin (width, (perRowHeight * 16) / 9);
    }

    return juce::jmax (72, juce::jmin (width, availableWidth));
}

int MainScreen::cameraRowsNeeded (int tileWidth, int availableWidth) const noexcept
{
    if (cameraViews.empty())
        return 0;

    const int perRow = juce::jmax (1, (availableWidth + kCameraTileGap)
                                          / juce::jmax (1, tileWidth + kCameraTileGap));

    return (static_cast<int> (cameraViews.size()) + perRow - 1) / perRow;
}

void MainScreen::setCameraScale (int step)
{
    // Clamped rather than asserted: this arrives from a settings file, which a
    // future version may have written a larger number into.
    const int clamped = juce::jlimit (0, kCameraScaleSteps - 1, step);

    if (clamped == cameraScale)
        return;

    cameraScale = clamped;

    // The ends of the range are the one thing the arrows must say for
    // themselves -- a button that keeps accepting clicks and changes nothing
    // reads as broken.
    cameraSmallerButton.setEnabled (cameraScale > 0);
    cameraLargerButton.setEnabled (cameraScale < kCameraScaleSteps - 1);

    resized();
}

void MainScreen::setCameraTiles (const std::vector<CameraTile>& tiles)
{
    std::vector<std::string> ids;
    std::vector<std::string> displayNames;
    std::vector<uint64_t> viewerRevisions;
    juce::StringArray signalStatusTexts;
    std::vector<char> recordingStates;
    std::vector<char> startingStates;
    ids.reserve (tiles.size());
    displayNames.reserve (tiles.size());
    viewerRevisions.reserve (tiles.size());
    recordingStates.reserve (tiles.size());
    startingStates.reserve (tiles.size());
    for (const auto& tile : tiles)
    {
        ids.push_back (tile.id);
        displayNames.push_back (tile.displayName.toStdString());
        viewerRevisions.push_back (tile.viewerRevision);
        signalStatusTexts.add (tile.signalStatusText);
        recordingStates.push_back (tile.recordingThisTake ? 1 : 0);
        startingStates.push_back (tile.startingThisTake ? 1 : 0);
    }

    // Only the set of cameras and their viewer revisions decide whether the
    // native previews are rebuilt. A revision is bumped whenever a camera's
    // picture appears, goes black or is lost, which is exactly when the host
    // must change between preview and placeholder. Everything else -- a
    // rename, a signal message, a take's REC/STARTING state -- is words
    // around the picture. Reparenting the NSView that holds AVFoundation's
    // preview layer for any of those can leave the layer black, so they are
    // said in place and the view hierarchy is left alone.
    const bool sameViewers = ids == lastTileIds && viewerRevisions == lastTileViewerRevisions
                          && cameraViews.size() == tiles.size();

    if (sameViewers)
    {
        // Called from the UI tick, so the unchanged case must cost nothing.
        if (displayNames == lastTileDisplayNames
            && signalStatusTexts == lastTileSignalStatusTexts
            && recordingStates == lastTileRecordingStates
            && startingStates == lastTileStartingStates)
            return;

        for (size_t i = 0; i < cameraViews.size(); ++i)
        {
            auto& view = cameraViews[i];
            const auto& tile = tiles[i];
            view.displayName = tile.displayName;
            view.signalStatusText = tile.signalStatusText;
            view.recordingThisTake = tile.recordingThisTake;
            view.startingThisTake = tile.startingThisTake;
            refreshCameraCaption (view);

            if (view.placeholder != nullptr)
                view.placeholder->setText (placeholderTextFor (view.signalStatusText),
                                           juce::dontSendNotification);
        }

        lastTileDisplayNames = std::move (displayNames);
        lastTileSignalStatusTexts = std::move (signalStatusTexts);
        lastTileRecordingStates = std::move (recordingStates);
        lastTileStartingStates = std::move (startingStates);
        return;
    }

    lastTileIds = std::move (ids);
    lastTileDisplayNames = std::move (displayNames);
    lastTileViewerRevisions = std::move (viewerRevisions);
    lastTileSignalStatusTexts = std::move (signalStatusTexts);
    lastTileRecordingStates = std::move (recordingStates);
    lastTileStartingStates = std::move (startingStates);
    cameraViews.clear();

    for (const auto& tile : tiles)
    {
        CameraView view;
        view.id = tile.id;
        view.displayName = tile.displayName;
        view.recordingThisTake = tile.recordingThisTake;
        view.startingThisTake = tile.startingThisTake;
        view.signalStatusText = tile.signalStatusText;

        if (makeViewer)
            view.viewer = makeViewer (tile.id);

        if (view.viewer != nullptr)
        {
            // Born hidden when a card is up (setCameraPreviewsHidden): a
            // rebuild must not put the live picture back over the card.
            addChildComponent (*view.viewer);
            view.viewer->setVisible (! previewsHidden);
        }
        else
        {
            // A camera that would not open gets a well saying so, never an
            // empty rectangle -- and on Linux, where JUCE has no CameraDevice
            // at all, this is the honest thing to show rather than a black box.
            view.placeholder = std::make_unique<juce::Label>();
            view.placeholder->setText (placeholderTextFor (tile.signalStatusText),
                                       juce::dontSendNotification);
            view.placeholder->setJustificationType (juce::Justification::centred);
            view.placeholder->setFont (juce::Font (12.0f));
            view.placeholder->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
            addAndMakeVisible (*view.placeholder);
        }

        // Whose picture this is. Four identical webcams are the same problem
        // §14.6 solves for microphones, and the answer is the same: put the
        // name on the thing.
        view.caption = std::make_unique<juce::Label>();
        view.caption->setJustificationType (juce::Justification::centred);
        view.caption->setFont (juce::Font (12.0f));
        refreshCameraCaption (view);
        addAndMakeVisible (*view.caption);

        cameraViews.push_back (std::move (view));
    }

    resized();
}

juce::String MainScreen::placeholderTextFor (const juce::String& signalStatusText)
{
    return signalStatusText.isNotEmpty() ? signalStatusText
                                         : juce::String ("No picture from this camera.");
}

void MainScreen::refreshCameraCaption (CameraView& view)
{
    if (view.caption == nullptr)
        return;

    const bool recordingThisCamera = recording && view.recordingThisTake;
    const auto& name = view.displayName;

    view.caption->setText (! recording ? name
                           : recordingThisCamera && view.signalStatusText.isNotEmpty()
                               ? "SIGNAL LOST: " + name
                           : recordingThisCamera ? "REC: " + name
                           : view.startingThisTake ? "STARTING: " + name
                                                   : "NOT RECORDING: " + name,
                           juce::dontSendNotification);
    view.caption->setColour (juce::Label::textColourId,
                             recordingThisCamera && view.signalStatusText.isEmpty()
                                 ? AppLookAndFeel::danger
                                 : recording ? AppLookAndFeel::warning
                                             : AppLookAndFeel::secondary);
}

void MainScreen::releaseCameraViews()
{
    if (cameraViews.empty())
        return;

    cameraViews.clear();

    // Cleared too, so the next setCameraTiles() rebuilds rather than deciding
    // nothing has changed and leaving the row empty.
    lastTileIds.clear();
    lastTileDisplayNames.clear();
    lastTileViewerRevisions.clear();
    lastTileSignalStatusTexts.clear();
    lastTileRecordingStates.clear();
    lastTileStartingStates.clear();
    resized();
}

void MainScreen::setVisibleHeight (int height)
{
    if (height == visibleHeight)
        return;

    visibleHeight = juce::jmax (0, height);
    resized();
}

int MainScreen::layoutHeight() const noexcept
{
    return visibleHeight > 0 ? visibleHeight : getHeight();
}

int MainScreen::cameraRowHeight() const
{
    // Zero when nothing is switched on, which is what keeps an audio-only rig
    // laid out exactly as it was before the pictures arrived.
    if (cameraViews.empty())
        return 0;

    // Measured against the width the screen actually has, because that decides
    // how many tiles fit across and therefore how many rows they wrap onto. Its
    // own width is the honest answer; before the first resize() there is none,
    // so fall back to the window width the owner opens at.
    const int available = juce::jmax (1, (getWidth() > 0 ? getWidth() : 1180) - 32);

    // What the camera row has to spend: the window, minus everything laid out
    // under it. This is what makes a taller window a taller picture, and what
    // stops a taller picture from pushing the record button off the bottom.
    const int spare = layoutHeight() > 0
                          ? layoutHeight() - nonCameraHeight() - kCameraControlHeight - 4 - kCameraRowGap
                          : 0;

    return cameraRowHeightForSpare (spare);
}

int MainScreen::cameraRowHeightForSpare (int spare) const
{
    if (cameraViews.empty())
        return 0;

    const int available = juce::jmax (1, (getWidth() > 0 ? getWidth() : 1180) - 32);

    const int tileWidth = cameraTileWidthFor (cameraScale, available, spare);
    const int rows = cameraRowsNeeded (tileWidth, available);
    const int cellHeight = tileHeightFor (tileWidth) + kCameraCaptionHeight;

    return kCameraControlHeight + 4
           + rows * cellHeight + (rows - 1) * kCameraTileGap
           + kCameraRowGap;
}

int MainScreen::getRequiredHeight() const
{
    // What the content wants inside the window it has. Never more than the
    // window can show, because the camera row is measured against the visible
    // height -- which is what keeps the record button on screen.
    return nonCameraHeight() + cameraRowHeight();
}

int MainScreen::getPreferredHeight() const
{
    // What the content would want if the window were not in the way.
    //
    // getRequiredHeight() cannot answer this, and deliberately so: it clamps the
    // picture to the height that is visible, so it can never ask for more than
    // it already has. That makes it useless for deciding how big to OPEN the
    // window, which is a question asked before the right window exists.
    //
    // A spare of 0 means "unconstrained" to cameraTileWidthFor, so this is the
    // fraction of the width the user chose, at 16:9, plus everything under it.
    return nonCameraHeight() + cameraRowHeightForSpare (0);
}

int MainScreen::getMicCount() const
{
    return channelMeters.size();
}

ChannelMeterComponent* MainScreen::getChannelMeter (int index)
{
    return channelMeters[index];
}

void MainScreen::setRecording (bool isRecording)
{
    const bool changed = recording != isRecording;
    recording = isRecording;
    // §10.4/§10.6: buttons say what happens. "Start recording" -> "Recording."
    recordButton.setButtonText (recording ? "Recording. Tap to stop." : "Start recording");

    // Cyan to start, red while running: the colour carries the state at a
    // glance, and §9.3 keeps the text saying it too rather than colour alone.
    recordButton.setColour (juce::TextButton::buttonColourId,
                            recording ? AppLookAndFeel::danger : AppLookAndFeel::accent);
    recordButton.setColour (juce::TextButton::textColourOffId,
                            recording ? AppLookAndFeel::bone : AppLookAndFeel::background);

    // The status row is split in two while recording and single when not, so
    // the change of state is a change of layout. Without this the elapsed-time
    // label kept the empty bounds resized() gave it in the idle layout, and
    // "Recording for 4m 12s" was set on a label nobody could see.
    if (changed)
    {
        // Camera previews on macOS are native NSViews and always cover JUCE
        // painting which overlaps them. Put the per-camera recording proof in
        // the non-overlapping caption band beneath each preview instead.
        for (auto& view : cameraViews)
            refreshCameraCaption (view);

        resized();
    }
}

void MainScreen::setCameraPreviewsHidden (bool hidden)
{
    if (hidden == previewsHidden)
        return;

    previewsHidden = hidden;

    // Visibility only. Destroying or reparenting the native preview is what
    // can leave AVFoundation's layer black; hiding it is [NSView setHidden:].
    for (auto& view : cameraViews)
        if (view.viewer != nullptr)
            view.viewer->setVisible (! hidden);
}

void MainScreen::setHighlightedMic (int index)
{
    for (int i = 0; i < channelMeters.size(); ++i)
        channelMeters[i]->setHighlighted (i == index);
}

void MainScreen::setMuteState (bool muted, bool runawayMuted)
{
    muteButton.setToggleState (muted, juce::dontSendNotification);

    // §10.6: the button says what pressing it will do. After a runaway cut it
    // is the recovery control, and it must say so.
    muteButton.setButtonText (runawayMuted ? "Unmute (sound was cut)"
                                           : (muted ? "Unmute" : "Mute"));
}

void MainScreen::setCameraCount (int count)
{
    // §9.3: the count is on the button rather than only in a colour, so a
    // camera being in the take is readable at a glance from the main screen.
    camerasButton.setButtonText (count > 0 ? "Cameras (" + juce::String (count) + ")" : "Cameras");
}

void MainScreen::setAdviceText (const juce::String& text)
{
    const int before = adviceHeight();

    adviceLabel.setText (text, juce::dontSendNotification);
    adviceLabel.setVisible (text.isNotEmpty());

    // Compared first: this arrives on every status tick, and only a sentence
    // that needs a different number of lines changes the height.
    if (adviceHeight() != before)
        requiredHeightChanged();
}

void MainScreen::setMonitorProblemText (const juce::String& text)
{
    const int before = monitorProblemHeight();

    monitorProblemLabel.setText (text, juce::dontSendNotification);
    monitorProblemLabel.setVisible (text.isNotEmpty());

    // The band is sized to the reason, so a longer one needs a taller screen.
    // Compared first because this arrives on every status tick.
    if (monitorProblemHeight() != before)
        requiredHeightChanged();
}

void MainScreen::setCopyToThisCardVisible (bool visible)
{
    // Arrives on every status tick: only a change lays the screen out again.
    if (copyToThisCardButton.isVisible() == visible)
        return;

    copyToThisCardButton.setVisible (visible);
    requiredHeightChanged();
}

void MainScreen::setNoMicsMessage (bool show)
{
    if (noMicsLabel.isVisible() == show)
        return;

    noMicsLabel.setVisible (show);
    requiredHeightChanged();
}

void MainScreen::requiredHeightChanged()
{
    resized();

    // Laying out again inside the same height only moves the shortfall to the
    // footer. The owner sizes this screen from getRequiredHeight(), so it is
    // the owner that has to be asked to measure again.
    if (onRequiredHeightChanged)
        onRequiredHeightChanged();
}

void MainScreen::setRecordButtonEnabled (bool enabled, const juce::String& disabledReason)
{
    recordButton.setEnabled (enabled);

    const int before = disabledReasonHeight();
    disabledReasonLabel.setText (disabledReason, juce::dontSendNotification);

    // resized() only reserves the reason row when it is visible, so the layout
    // has to be redone when that changes. Guarded because this is called from
    // a timer, and re-laying out the screen every tick is not free.
    if (disabledReasonLabel.isVisible() == enabled)
    {
        disabledReasonLabel.setVisible (! enabled);
        requiredHeightChanged();
    }
    else if (! enabled && disabledReasonHeight() != before)
    {
        // A different reason that wraps onto a different number of lines.
        requiredHeightChanged();
    }
}

void MainScreen::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    if (! brandMarkBounds.isEmpty())
        paintBrandMark (g, brandMarkBounds.toFloat());

    // A hairline under the masthead, so the title reads as a masthead rather
    // than as the first row of the content beneath it.
    if (! brandMarkBounds.isEmpty())
    {
        g.setColour (AppLookAndFeel::outline);
        const int y = brandMarkBounds.getBottom() + 10;
        g.fillRect (16, y, getWidth() - 32, 1);
    }
}

void MainScreen::setDoorsOpen (bool settingsOpen, bool helpOpen)
{
    advancedButton.setToggleState (settingsOpen, juce::dontSendNotification);
    helpButton.setToggleState (helpOpen, juce::dontSendNotification);
}

void MainScreen::resized()
{
    auto area = getLocalBounds().reduced (16);

    // The masthead: the mark, the name, the tagline, and the one door out.
    // Settings moved up here from the foot of the screen because it is a
    // destination rather than a control -- it belongs with the title, not in
    // the row where the levels are being set.
    {
        auto header = area.removeFromTop (40);

        brandMarkBounds = header.removeFromLeft (28).withSizeKeepingCentre (26, 26);
        header.removeFromLeft (10);

        // Full-height targets. Reduced to 30px they were the smallest buttons
        // on the screen, and they are the two most people go looking for.
        advancedButton.setBounds (header.removeFromRight (112).reduced (0, 2));
        header.removeFromRight (8);
        helpButton.setBounds (header.removeFromRight (80).reduced (0, 2));
        header.removeFromRight (8);
        versionLabel.setBounds (header.removeFromRight (56));
        header.removeFromRight (8);

        brandLabel.setBounds (header.removeFromLeft (juce::jmin (128, header.getWidth())));
        header.removeFromLeft (10);
        taglineLabel.setBounds (header);

        area.removeFromTop (18);
    }

    // The pictures sit above the levels: the shot is what you look at, the
    // meters are what you glance at, and putting them in that order keeps the
    // record button where it has always been at the bottom.
    const bool haveCameras = ! cameraViews.empty();

    cameraSizeLabel.setVisible (haveCameras);
    cameraSmallerButton.setVisible (haveCameras);
    cameraLargerButton.setVisible (haveCameras);
    cameraSectionLabel.setVisible (haveCameras);
    fullPreviewToggle.setVisible (haveCameras);
    addCameraButton.setVisible (haveCameras);

    if (haveCameras)
    {
        auto cameraArea = area.removeFromTop (cameraRowHeight() - kCameraRowGap);

        // One row carrying the heading and everything that acts on the
        // pictures: what they are, how big, how good the preview is, and how to
        // add another. Grouped here rather than scattered down the screen,
        // because they are one job.
        auto controls = cameraArea.removeFromTop (kCameraControlHeight);
        cameraSectionLabel.setBounds (controls.removeFromLeft (110));

        addCameraButton.setBounds (controls.removeFromRight (108).reduced (0, 1));
        controls.removeFromRight (10);
        fullPreviewToggle.setBounds (controls.removeFromRight (108));
        controls.removeFromRight (10);
        cameraLargerButton.setBounds (controls.removeFromRight (28).reduced (1));
        controls.removeFromRight (4);
        cameraSmallerButton.setBounds (controls.removeFromRight (28).reduced (1));
        controls.removeFromRight (6);
        cameraSizeLabel.setBounds (controls.removeFromRight (80));

        cameraArea.removeFromTop (4);

        const int available = cameraArea.getWidth();

        // The same two bounds cameraRowHeight() sized this row from, so what is
        // drawn and what was reserved cannot disagree.
        const int spare = layoutHeight() - nonCameraHeight() - kCameraControlHeight - 4 - kCameraRowGap;
        const int tileWidth = cameraTileWidthFor (cameraScale, available, spare);
        const int tileHeight = tileHeightFor (tileWidth);
        const int cellHeight = tileHeight + kCameraCaptionHeight;

        // Wrap rather than shrink. Asking for a bigger picture on a four-camera
        // rig has to give you one; squeezing them all onto one row instead
        // would make the arrows do nothing precisely when they are wanted.
        const int perRow = juce::jmax (1, (available + kCameraTileGap) / (tileWidth + kCameraTileGap));

        int index = 0;
        for (auto& view : cameraViews)
        {
            if (index % perRow == 0)
            {
                if (index > 0)
                    cameraArea.removeFromTop (kCameraTileGap);

                const int inThisRow = juce::jmin (perRow, static_cast<int> (cameraViews.size()) - index);
                const int rowWidth = inThisRow * tileWidth + (inThisRow - 1) * kCameraTileGap;

                auto row = cameraArea.removeFromTop (cellHeight);

                // Centred as a block, like the channel strips: two cameras sit
                // in the middle rather than hugging the left edge.
                if (rowWidth < available)
                    row.removeFromLeft ((available - rowWidth) / 2);

                // Stashed for the tiles in this row to consume.
                cameraRowScratch = row;
            }
            else
            {
                cameraRowScratch.removeFromLeft (kCameraTileGap);
            }

            auto tile = cameraRowScratch.removeFromLeft (tileWidth);
            auto caption = tile.removeFromBottom (kCameraCaptionHeight);

            if (view.caption != nullptr)
                view.caption->setBounds (caption);

            if (view.viewer != nullptr)
                view.viewer->setBounds (tile);
            else if (view.placeholder != nullptr)
                view.placeholder->setBounds (tile);

            ++index;
        }

        area.removeFromTop (kCameraRowGap);
    }

    // The levels, as a row of horizontal strips rather than a rank of tall
    // channels. The pictures are what take the height now, and a level being
    // watched out of the corner of an eye is better served by a wide track
    // than by a tall one: the same information, in the shape the space is.
    //
    // MIX rides in the same row as one more strip, because it is one more
    // level -- it was a full-width bar of its own, which said it was a
    // different kind of thing than the channels it is the sum of.
    micSectionLabel.setBounds (area.removeFromTop (16));
    area.removeFromTop (6);

    if (channelMeters.isEmpty())
    {
        noMicsLabel.setBounds (area.removeFromTop (56));
        mixBar.setBounds ({});
    }
    else
    {
        constexpr int kStripHeight = 40;
        constexpr int kStripGap    = 12;
        constexpr int kMinStripWidth = 190;

        // MIX is laid out with them, so the wrap has to count it.
        const int cells = channelMeters.size() + 1;
        const int available = area.getWidth();

        const int perRow = juce::jlimit (1, cells,
                                         (available + kStripGap) / (kMinStripWidth + kStripGap));
        const int rows = (cells + perRow - 1) / perRow;

        auto stripArea = area.removeFromTop (rows * kStripHeight + (rows - 1) * kStripGap);

        juce::Rectangle<int> row;
        for (int i = 0; i < cells; ++i)
        {
            if (i % perRow == 0)
            {
                if (i > 0)
                    stripArea.removeFromTop (kStripGap);

                row = stripArea.removeFromTop (kStripHeight);
            }
            else
            {
                row.removeFromLeft (kStripGap);
            }

            // One width for every cell on the screen, taken from a full row.
            // Sizing the last row's cells to what is left instead stretched a
            // lone MIX across the entire window, which said it was a different
            // kind of thing than the channels it is the sum of.
            const int cellWidth = (available - (perRow - 1) * kStripGap) / perRow;

            auto cell = row.removeFromLeft (juce::jmin (cellWidth, row.getWidth()));

            if (i < channelMeters.size())
            {
                channelMeters[i]->setBounds (cell);
            }
            else
            {
                mixBar.setBounds (cell);
            }
        }
    }

    area.removeFromTop (16);

    // Side by side: naming the take and starting it are one action, and
    // stacking them put 8px of nothing between a field and the button that
    // consumes it while the eye travelled the full width twice.
    {
        auto actionRow = area.removeFromTop (52);
        const int buttonWidth = juce::jlimit (200, 380, actionRow.getWidth() / 2);

        recordButton.setBounds (actionRow.removeFromRight (buttonWidth));
        actionRow.removeFromRight (14);
        sessionNameEditor.setBounds (actionRow.withSizeKeepingCentre (actionRow.getWidth(), 40));
    }

    // Only take the row when there is something in it. Reserved unconditionally
    // it left a 28px hole under the record button whenever recording was
    // possible -- which is almost always -- and pushed the status lines away
    // from the button they belong to.
    if (disabledReasonLabel.isVisible())
    {
        disabledReasonLabel.setBounds (area.removeFromTop (disabledReasonHeight()));
        area.removeFromTop (8);
    }
    else
    {
        disabledReasonLabel.setBounds ({});
        area.removeFromTop (14);
    }

    // Elapsed, remaining and the destination all live in the footer now, so
    // nothing is placed here -- the record button is followed straight by the
    // lines that qualify it.
    // "Copy to this card" beside the line that says which takes it is for,
    // in a row tall enough for a button, only while it is shown.
    if (copyToThisCardButton.isVisible())
    {
        auto row = area.removeFromTop (kCopyToThisCardRow);
        copyToThisCardButton.setBounds (row.removeFromRight (juce::jmin (160, row.getWidth() / 2)));
        row.removeFromRight (8);
        filesSavingLabel.setBounds (row);
    }
    else
    {
        copyToThisCardButton.setBounds ({});
        filesSavingLabel.setBounds (area.removeFromTop (18));
    }

    // Sized to the message, not to one line. This label carries the reason a
    // microphone would not open -- which rate the interface is running at, that
    // macOS has not granted microphone access, that another app holds the
    // device -- and a fixed 20px row clipped all of it to the first few words,
    // leaving the same dead end the reason was written to end.
    monitorProblemLabel.setBounds (area.removeFromTop (monitorProblemHeight()));
    adviceLabel.setBounds (area.removeFromTop (adviceHeight()));

    // The footer: what the disk knows on the left, what the ears want on the
    // right. Both are ambient -- neither is something anyone comes to this
    // screen to do -- so they share one quiet row at the bottom rather than
    // taking a band of the middle each.
    auto bottomRow = area.removeFromBottom (40);

    muteButton.setBounds (bottomRow.removeFromRight (92));
    bottomRow.removeFromRight (8);

    // The door to the cameras only earns a place down here when there is no
    // camera row carrying "+ Add camera" -- otherwise the same door appears
    // twice on one screen.
    if (! haveCameras)
    {
        camerasButton.setBounds (bottomRow.removeFromRight (124));
        bottomRow.removeFromRight (10);
    }
    else
    {
        camerasButton.setBounds ({});
    }

    // What the text wants: the capacity figure, and the elapsed time beside it
    // during a take. Fixed rather than measured, so the row does not shuffle
    // every time the figure ticks over.
    constexpr int kElapsedWidth   = 150;
    constexpr int kRemainingWidth = 190;
    const int textWanted = recording ? kElapsedWidth + kRemainingWidth : 200;

    // The slider gives way before the text does. At its full 220px beside an
    // open drawer, or at the narrowest window, it left the remaining time about
    // 100px and "Room for 1..." -- the one figure in this row anyone acts on,
    // traded for a longer track on a control that works at a third the length.
    const int sliderWidth = juce::jlimit (80, 220, bottomRow.getWidth() - 16 - textWanted);
    volumeSlider.setBounds (bottomRow.removeFromRight (sliderWidth));
    bottomRow.removeFromRight (16);

    remainingLabel.setJustificationType (juce::Justification::centredLeft);
    saveLocationLabel.setJustificationType (juce::Justification::centredLeft);
    elapsedLabel.setJustificationType (juce::Justification::centredLeft);

    // Elapsed only appears while a take is running, and takes the space from
    // the destination rather than from the capacity figure: during a take, how
    // long it has been going matters more than where it is going, which has
    // not changed since it started.
    // One line each, centred in the row. Given the row's full height a label
    // wraps onto two lines the moment the window narrows -- beside an open
    // Settings drawer, for one -- and the footer turns into a stack.
    auto textRow = bottomRow.withSizeKeepingCentre (bottomRow.getWidth(), 20);

    if (recording)
    {
        // Short of room, the two are shrunk in proportion rather than elapsed
        // taking its full width first and leaving the remaining time nothing.
        const int textWidth = textRow.getWidth();
        const int elapsedWidth = textWidth >= textWanted
                                     ? kElapsedWidth
                                     : textWidth * kElapsedWidth / textWanted;

        elapsedLabel.setBounds (textRow.removeFromLeft (elapsedWidth));
        remainingLabel.setBounds (textRow.removeFromLeft (juce::jmin (kRemainingWidth, textRow.getWidth())));
        saveLocationLabel.setBounds (textRow);
    }
    else
    {
        // The figure first, the destination with what is left. Halving the row
        // between them cut the figure short at exactly the widths where the
        // destination -- a long path -- was going to be ellipsized anyway.
        elapsedLabel.setBounds ({});
        remainingLabel.setBounds (textRow.removeFromLeft (juce::jmin (textWanted, textRow.getWidth())));
        saveLocationLabel.setBounds (textRow);
    }
}

} // namespace mma
