#include "CameraPanel.h"
#include "AppLookAndFeel.h"
#include <cmath>

namespace mma {

namespace {
    // The same measures as Settings, so the two drawers read as one design.
    constexpr int kMargin       = 12;
    constexpr int kCloseRow     = 36;
    constexpr int kAfterClose   = 14;
    constexpr int kHeadingRow   = 28;
    constexpr int kAfterHeading = 4;
    constexpr int kSection      = 14 + 3 + 1 + 9; // heading, gap, rule, gap
    constexpr int kRowHeight    = 32;
    constexpr int kToggleRow    = 28;
    constexpr int kValueWidth   = 300;
    constexpr int kGapAfter     = 16;  // after a block, before the next heading or camera
    constexpr int kLineGap      = 4;
    constexpr int kNoteIndent   = 20;  // a note under a switch, as in Settings

    juce::Label& styled (juce::Label& label, float size, juce::Colour colour,
                         bool bold = false)
    {
        label.setFont (juce::Font (size, bold ? juce::Font::bold : juce::Font::plain));
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (juce::Justification::topLeft);

        // Wrap, never squash: a label narrower than its sentence otherwise
        // shrinks the type sideways until it is unreadable.
        label.setMinimumHorizontalScale (1.0f);
        return label;
    }

    std::unique_ptr<juce::Label> makeLabel (float size, juce::Colour colour)
    {
        auto label = std::make_unique<juce::Label>();
        styled (*label, size, colour);
        return label;
    }
}

CameraPanel::CameraPanel()
{
    closeButton.onClick = [this] { if (onCloseClicked) onCloseClicked(); };
    addAndMakeVisible (closeButton);

    styled (heading, 20.0f, AppLookAndFeel::bone, true);
    heading.setText ("Cameras", juce::dontSendNotification);
    addAndMakeVisible (heading);

    styled (problemLabel, 13.0f, AppLookAndFeel::warning);
    addChildComponent (problemLabel);

    // The whole shape of the feature in plain words, as §10.2 asks: no
    // codecs, no containers, no bitrates.
    styled (explanation, 13.0f, AppLookAndFeel::secondary);
    explanation.setText ("Every camera that's plugged in records with the sound, each to its own "
                         "video file. Their pictures show on the main screen. Switch off any "
                         "camera you don't want.",
                         juce::dontSendNotification);
    addAndMakeVisible (explanation);

    styled (unavailableLabel, 13.0f, AppLookAndFeel::secondary);
    addChildComponent (unavailableLabel);

    const std::pair<juce::Label*, const char*> sections[] = {
        { &camerasSection,   "YOUR CAMERAS" },
        { &afterTakeSection, "AFTER EACH TAKE" },
        { &spaceSection,     "SPACE" },
    };

    for (auto& [label, text] : sections)
    {
        styled (*label, 10.0f, AppLookAndFeel::tertiary, true);
        label->setText (text, juce::dontSendNotification);
        addAndMakeVisible (*label);
    }

    // Said, rather than an empty space under the heading, which reads as
    // broken.
    styled (noCamerasLabel, 13.0f, AppLookAndFeel::secondary);
    noCamerasLabel.setText ("No camera is plugged in. Plug one in and it shows up here, "
                            "ready to record.", juce::dontSendNotification);
    addAndMakeVisible (noCamerasLabel);

    // Off by default: it costs disk and minutes of CPU after every take, and
    // the picture and the sound are already both saved and already aligned by
    // the session origin. Here rather than in Settings because it is a
    // question about the cameras' files, asked where the cameras are.
    combineVideoToggle.setToggleState (false, juce::dontSendNotification);
    combineVideoToggle.onClick = [this]
    { if (onCombineVideoToggled) onCombineVideoToggled (combineVideoToggle.getToggleState()); };
    addAndMakeVisible (combineVideoToggle);

    // Named under the switch rather than in an alert, because the answer is
    // "install a thing", and an alert after the take is the wrong moment to
    // learn that.
    styled (combineVideoNote, 12.0f, AppLookAndFeel::secondary);
    addChildComponent (combineVideoNote);

    styled (spaceLabel, 13.0f, AppLookAndFeel::secondary);
    addAndMakeVisible (spaceLabel);

    styled (roomNoteLabel, 12.0f, AppLookAndFeel::secondary);
    addChildComponent (roomNoteLabel);
}

CameraPanel::~CameraPanel() = default;

// --- what each row says ------------------------------------------------------

juce::String CameraPanel::fileLine (const CameraRow& camera, bool recordingNow)
{
    // A camera that was recording this take and is no longer available has
    // been unplugged mid-take, and its movie is certainly truncated -- no
    // signal-status text required, and none was ever supplied for those
    // rows, so this warning could not reach the one case that actually cuts
    // a file short.
    return recordingNow && camera.recordingThisTake
               && (camera.signalStatusText.isNotEmpty() || ! camera.available)
               ? "Video signal lost. This file may be incomplete."
         : recordingNow && camera.recordingThisTake ? "Recording to " + camera.fileName
         : recordingNow && camera.startingThisTake ? "Starting video. Waiting for the camera to confirm."
         : recordingNow ? "Not recording video in this take"
         : camera.discoveryPending ? "Checking the system for this camera..."
         : ! camera.available ? "Unavailable. Not in the next recording."
         : camera.enabled && camera.signalStatusText.isNotEmpty()
               ? "Waiting for a live picture. Not ready to record."
         : camera.enabled ? "Writes " + camera.fileName
                          : "Not in the recording";
}

juce::String CameraPanel::hintLine (const CameraRow& camera, bool recordingNow)
{
    // What to do about a camera that is not live. The picture is on the main
    // screen, so a live camera needs no words here at all.
    if (camera.discoveryPending)
        return "Still checking for this camera. Sound can record now; video waits for the next take.";

    if (! camera.available)
        return recordingNow ? "Out for this take. SobStage will check and reopen it for the next take."
                            : "Not available to the system. Reconnect it; SobStage will rescan automatically.";

    if (! camera.enabled)
        return "Switched off. Turn it on to see it on the main screen.";

    return camera.signalStatusText;
}

juce::String CameraPanel::formatLine (const CameraRow& camera, bool recordingNow)
{
    // Only what the camera itself reports. Where it does not (Windows), no
    // line at all: a guess would be a promise the file might not keep.
    if (! camera.enabled || ! camera.available || camera.activeFormatText.isEmpty())
        return {};

    return (recordingNow && camera.recordingThisTake ? "Recording at " : "Running at ")
         + camera.activeFormatText;
}

// --- state -------------------------------------------------------------------

void CameraPanel::setRecording (bool isRecording)
{
    if (recording == isRecording)
        return;

    recording = isRecording;

    // §9.3: never colour alone. The heading says it as well as showing it.
    updateRecordingHeading();
    heading.setColour (juce::Label::textColourId,
                       recording ? AppLookAndFeel::danger : AppLookAndFeel::bone);

    // The camera roster, names and picture sizes are fixed when the take
    // begins: a control that seems to change one halfway through its movie
    // would be a promise the take cannot keep.
    applyRowsInPlace();
}

void CameraPanel::updateRecordingHeading()
{
    int recordingCount = 0, startingCount = 0;

    for (const auto& camera : lastCameras)
    {
        recordingCount += camera.recordingThisTake ? 1 : 0;
        startingCount += camera.startingThisTake ? 1 : 0;
    }

    heading.setText (! recording ? juce::String ("Cameras")
                     : recordingCount > 0 ? "Cameras: " + juce::String (recordingCount) + " recording"
                     : startingCount > 0 ? juce::String ("Cameras: starting video")
                                         : juce::String ("Cameras: no video recording"),
                     juce::dontSendNotification);
}

void CameraPanel::setUnavailableReason (const juce::String& reason)
{
    if (unavailableLabel.getText() == reason)
        return;

    unavailableLabel.setText (reason, juce::dontSendNotification);

    // The controls are not disabled, they are gone: a row of dead switches
    // invites someone to keep clicking them looking for the one that works.
    applyRowsInPlace();
    resized();
}

void CameraPanel::setProblemText (const juce::String& text)
{
    if (problemLabel.getText() == text)
        return;

    problemLabel.setText (text, juce::dontSendNotification);
    applyRowsInPlace();
    resized();
}

void CameraPanel::setCombineVideoState (bool on, const juce::String& unavailableReason)
{
    combineVideoToggle.setToggleState (on, juce::dontSendNotification);

    // §10.6: what is missing and what to do about it, under the control it
    // affects. The switch stays usable either way -- switching it on just
    // before installing ffmpeg is a reasonable thing to do, and a disabled
    // control with an explanation nobody can act on is worse than an enabled
    // one that says what it needs.
    if (combineVideoNote.getText() == unavailableReason)
        return;

    combineVideoNote.setText (unavailableReason, juce::dontSendNotification);
    applyRowsInPlace();
    resized();
}

void CameraPanel::setSpaceText (const juce::String& perHour, const juce::String& roomNote)
{
    if (spaceLabel.getText() == perHour && roomNoteLabel.getText() == roomNote)
        return;

    spaceLabel.setText (perHour, juce::dontSendNotification);
    roomNoteLabel.setText (roomNote, juce::dontSendNotification);
    applyRowsInPlace();
    resized();
}

void CameraPanel::setCameras (const std::vector<CameraRow>& cameras)
{
    bool sameSet = cameras.size() == rows.size();

    for (size_t i = 0; sameSet && i < cameras.size(); ++i)
        sameSet = cameras[i].id == rows[i].id;

    lastCameras = cameras;
    updateRecordingHeading();

    // Rebuilt only when cameras come or go. Everything else -- a switch, a
    // name, a signal message, a take starting -- is said in place, so a name
    // box being typed in is never pulled out from under the typing.
    if (sameSet)
        applyRowsInPlace();
    else
        rebuildRows();
}

void CameraPanel::rebuildRows()
{
    rows.clear();

    // Each change is handed on after the click or keystroke that made it has
    // finished. The owner re-reads the cameras in reply, and a camera leaving
    // the list rebuilds these rows -- destroying a control from inside its
    // own callback is a crash JUCE gives no warning about.
    const auto later = [this] (std::function<void (CameraPanel&)> call)
    {
        juce::Component::SafePointer<CameraPanel> safe (this);
        juce::MessageManager::callAsync ([safe, call = std::move (call)]
        {
            if (safe != nullptr)
                call (*safe);
        });
    };

    for (const auto& camera : lastCameras)
    {
        Row row;
        row.id = camera.id;

        // Every row's box says "Record this camera", which is all VoiceOver
        // read out -- the same words for each camera, with nothing to say
        // which one would be switched. The name is in the spoken title.
        row.enabledToggle = std::make_unique<juce::ToggleButton> ("Record this camera");
        row.enabledToggle->onClick = [later, id = camera.id, button = row.enabledToggle.get()]
        {
            const bool on = button->getToggleState();
            later ([id, on] (CameraPanel& panel)
            {
                if (panel.onCameraEnabledChanged)
                    panel.onCameraEnabledChanged (id, on);
            });
        };
        addChildComponent (*row.enabledToggle);

        // §14.6 for cameras: the name goes on the file, so "which one is the
        // wide shot" is answerable from a file browser a week later.
        row.nameEditor = std::make_unique<juce::TextEditor>();
        row.nameEditor->setText (camera.displayName, juce::dontSendNotification);
        row.nameEditor->setTextToShowWhenEmpty ("Name this camera", AppLookAndFeel::tertiary);
        row.nameEditor->setJustification (juce::Justification::centredLeft);
        row.nameEditor->setDescription ("This name is used in the camera recording's filename.");
        row.nameEditor->onFocusLost = [later, id = camera.id, editor = row.nameEditor.get()]
        {
            later ([id, name = editor->getText()] (CameraPanel& panel)
            {
                if (panel.onCameraRenamed)
                    panel.onCameraRenamed (id, name);
            });
        };
        row.nameEditor->onReturnKey = [this, later, id = camera.id, editor = row.nameEditor.get()]
        {
            later ([id, name = editor->getText()] (CameraPanel& panel)
            {
                if (panel.onCameraRenamed)
                    panel.onCameraRenamed (id, name);
            });

            // Return is "done". The main screen is live beside this page, and
            // the next Space is the room's mute, not a space in the name.
            if (auto* window = getTopLevelComponent(); window != nullptr && window != this)
                window->grabKeyboardFocus();
        };
        addChildComponent (*row.nameEditor);

        // The file this camera is going to write, said here because renaming
        // is the moment someone wants to see what the name does.
        row.fileName = makeLabel (12.0f, AppLookAndFeel::tertiary);
        row.fileName->setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        addChildComponent (*row.fileName);

        row.hint = makeLabel (12.0f, AppLookAndFeel::tertiary);
        addChildComponent (*row.hint);

        // How much picture to ask this camera for. Best is whatever it can do
        // smoothly up to 4K; the caps are for a slow card, a full one, or a
        // busy machine. Fixed for the take, like everything else about it.
        row.qualityLabel = makeLabel (13.0f, AppLookAndFeel::secondary);
        row.qualityLabel->setText ("Picture size", juce::dontSendNotification);
        row.qualityLabel->setJustificationType (juce::Justification::centredLeft);
        addChildComponent (*row.qualityLabel);

        row.qualityCombo = std::make_unique<juce::ComboBox>();
        row.qualityCombo->addItem ("Best (up to 4K)", 1 + static_cast<int> (CameraQuality::Best));
        row.qualityCombo->addItem ("1080p", 1 + static_cast<int> (CameraQuality::HD1080));
        row.qualityCombo->addItem ("720p (smaller files)", 1 + static_cast<int> (CameraQuality::HD720));

        // A click leaves the keyboard where it was: Up and Down resize the
        // pictures on the main screen, and must not quietly change a size here.
        row.qualityCombo->setWantsKeyboardFocus (false);
        row.qualityCombo->setMouseClickGrabsKeyboardFocus (false);
        row.qualityCombo->onChange = [later, id = camera.id, combo = row.qualityCombo.get()]
        {
            const int selected = combo->getSelectedId();
            if (selected <= 0)
                return;

            later ([id, quality = static_cast<CameraQuality> (selected - 1)] (CameraPanel& panel)
            {
                if (panel.onCameraQualityChanged)
                    panel.onCameraQualityChanged (id, quality);
            });
        };
        addChildComponent (*row.qualityCombo);

        row.formatLabel = makeLabel (12.0f, AppLookAndFeel::tertiary);
        addChildComponent (*row.formatLabel);

        rows.push_back (std::move (row));
    }

    applyRowsInPlace();
    resized();
}

void CameraPanel::applyRow (Row& row, const CameraRow& camera)
{
    const bool showRow = ! camerasUnavailable();

    row.enabledToggle->setToggleState (camera.enabled, juce::dontSendNotification);
    row.enabledToggle->setTitle ("Record " + camera.displayName);
    row.enabledToggle->setEnabled (! recording);
    row.enabledToggle->setVisible (showRow);

    // Never over someone's typing: the name they are part-way through is not
    // yet the one on file, and restating that would undo it.
    if (! row.nameEditor->hasKeyboardFocus (true) && row.nameEditor->getText() != camera.displayName)
        row.nameEditor->setText (camera.displayName, juce::dontSendNotification);

    row.nameEditor->setTitle ("Name " + camera.displayName);
    row.nameEditor->setEnabled (! recording);
    row.nameEditor->setVisible (showRow);

    row.fileName->setText (fileLine (camera, recording), juce::dontSendNotification);
    row.fileName->setVisible (showRow);

    const auto hint = hintLine (camera, recording);
    row.hint->setText (hint, juce::dontSendNotification);
    row.hint->setVisible (showRow && hint.isNotEmpty());

    row.qualityCombo->setSelectedId (1 + static_cast<int> (camera.quality), juce::dontSendNotification);
    row.qualityCombo->setTitle ("Picture size for " + camera.displayName);
    row.qualityCombo->setEnabled (! recording);
    row.qualityCombo->setVisible (showRow);
    row.qualityLabel->setVisible (showRow);

    const auto format = formatLine (camera, recording);
    row.formatLabel->setText (format, juce::dontSendNotification);
    row.formatLabel->setVisible (showRow && format.isNotEmpty());
}

void CameraPanel::applyRowsInPlace()
{
    // What each line says and whether it shows; resized() is asked for only
    // when that moves something, since this runs on the UI tick.
    juce::StringArray before;
    const auto snapshot = [this] (juce::StringArray& out)
    {
        out.clear();
        for (auto* child : getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child))
                out.add ((label->isVisible() ? "1" : "0") + label->getText());
            else
                out.add (child->isVisible() ? "1" : "0");
    };

    snapshot (before);

    for (size_t i = 0; i < rows.size() && i < lastCameras.size(); ++i)
        applyRow (rows[i], lastCameras[i]);

    const bool available = ! camerasUnavailable();
    unavailableLabel.setVisible (! available);
    explanation.setVisible (available);
    problemLabel.setVisible (available && problemLabel.getText().isNotEmpty());
    camerasSection.setVisible (available);
    noCamerasLabel.setVisible (available && rows.empty());
    afterTakeSection.setVisible (available);
    combineVideoToggle.setVisible (available);
    combineVideoNote.setVisible (available && combineVideoNote.getText().isNotEmpty());
    roomNoteLabel.setVisible (roomNoteLabel.getText().isNotEmpty());

    juce::StringArray after;
    snapshot (after);

    if (after != before)
        resized();
}

// --- layout ------------------------------------------------------------------

int CameraPanel::contentWidth() const
{
    // Before the owner has given it a width, the narrowest drawer's.
    return juce::jmax (1, (getWidth() > 0 ? getWidth() : 380) - 2 * kMargin);
}

int CameraPanel::textHeight (const juce::Label& label, int width)
{
    if (label.getText().isEmpty())
        return 0;

    // Laid out exactly as the label draws it -- GlyphArrangement's own line
    // breaking at the label's inner width -- given room for every line it
    // wants. A label shorter than its lines shrinks the font instead.
    const auto font = label.getFont();
    const auto border = label.getBorderSize();
    const float available = (float) juce::jmax (1, width - border.getLeftAndRight());

    juce::GlyphArrangement glyphs;
    glyphs.addFittedText (font, label.getText(), 0.0f, 0.0f, available, 100000.0f,
                          juce::Justification::topLeft, 1000, label.getMinimumHorizontalScale());

    const float textHeight = glyphs.getNumGlyphs() > 0 ? glyphs.getBoundingBox (0, -1, true).getHeight()
                                                       : font.getHeight();

    return (int) std::ceil (textHeight) + border.getTopAndBottom() + 2;
}

int CameraPanel::headerToggleWidth (int width) const
{
    // What "Record this camera" needs beside its tick box (AppLookAndFeel
    // draws it at 14pt, 34px in), and never more than half the row: the name
    // box beside it is the thing being typed in.
    const auto text = juce::String ("Record this camera");
    const int wanted = (int) std::ceil (juce::Font (14.0f).getStringWidthFloat (text)) + 40;
    return juce::jmin (wanted, width / 2);
}

int CameraPanel::rowHeight (const Row& row, int width) const
{
    // The name and its switch, the file line, the hint when there is one,
    // the picture size, and what the camera says it runs at when it says.
    int height = kRowHeight + kLineGap + textHeight (*row.fileName, width);

    if (row.hint->isVisible())
        height += textHeight (*row.hint, width);

    height += kLineGap + kRowHeight;

    if (row.formatLabel->isVisible())
        height += textHeight (*row.formatLabel, width);

    return height;
}

int CameraPanel::getRequiredHeight() const
{
    // Mirrors resized() below, block by block.
    const int width = contentWidth();
    int height = kMargin + kCloseRow + kAfterClose + kHeadingRow + kAfterHeading;

    if (camerasUnavailable())
    {
        height += textHeight (unavailableLabel, width) + kGapAfter;
    }
    else
    {
        if (problemLabel.isVisible())
            height += textHeight (problemLabel, width) + kLineGap;

        height += textHeight (explanation, width) + kGapAfter;

        height += kSection;

        if (rows.empty())
            height += textHeight (noCamerasLabel, width) + kGapAfter;

        for (const auto& row : rows)
            height += rowHeight (row, width) + kGapAfter;

        height += kSection + kToggleRow;

        if (combineVideoNote.isVisible())
            height += textHeight (combineVideoNote, width - 2 * kNoteIndent);

        height += kGapAfter;
    }

    height += kSection + textHeight (spaceLabel, width);

    if (roomNoteLabel.isVisible())
        height += kLineGap + textHeight (roomNoteLabel, width);

    return height + kMargin;
}

void CameraPanel::resized()
{
    auto area = getLocalBounds().reduced (kMargin);
    const int width = area.getWidth();
    ruleYs.clear();

    // Top-left and first, where Settings keeps its own, and placed before
    // anything else claims the space so a long list cannot push it away.
    closeButton.setBounds (area.removeFromTop (kCloseRow).removeFromLeft (110));
    area.removeFromTop (kAfterClose);

    heading.setBounds (area.removeFromTop (kHeadingRow));
    area.removeFromTop (kAfterHeading);

    const auto place = [&area, width] (juce::Label& label, int gapAfter = 0, int indent = 0)
    {
        auto line = area.removeFromTop (textHeight (label, width - 2 * indent));
        label.setBounds (line.reduced (indent, 0));
        area.removeFromTop (gapAfter);
    };

    // A heading, then the hairline paint() draws under it, as in Settings.
    const auto section = [this, &area] (juce::Label& label)
    {
        label.setBounds (area.removeFromTop (14));
        area.removeFromTop (3);
        ruleYs.push_back (area.getY());
        area.removeFromTop (1 + 9);
    };

    if (camerasUnavailable())
    {
        place (unavailableLabel, kGapAfter);
    }
    else
    {
        if (problemLabel.isVisible())
            place (problemLabel, kLineGap);

        place (explanation, kGapAfter);

        section (camerasSection);

        if (rows.empty())
            place (noCamerasLabel, kGapAfter);

        for (auto& row : rows)
        {
            auto header = area.removeFromTop (kRowHeight);
            row.enabledToggle->setBounds (header.removeFromRight (headerToggleWidth (width)));
            header.removeFromRight (12);
            row.nameEditor->setBounds (header);
            area.removeFromTop (kLineGap);

            place (*row.fileName);

            if (row.hint->isVisible())
                place (*row.hint);

            area.removeFromTop (kLineGap);

            // A Settings row: the question on the left, its answer in a
            // capped column on the right.
            auto sizeRow = area.removeFromTop (kRowHeight);
            row.qualityCombo->setBounds (sizeRow.removeFromRight (juce::jmin (kValueWidth, sizeRow.getWidth() * 3 / 5)));
            sizeRow.removeFromRight (12);
            row.qualityLabel->setBounds (sizeRow);

            if (row.formatLabel->isVisible())
                place (*row.formatLabel);

            area.removeFromTop (kGapAfter);
        }

        section (afterTakeSection);
        combineVideoToggle.setBounds (area.removeFromTop (kToggleRow));

        if (combineVideoNote.isVisible())
            place (combineVideoNote, 0, kNoteIndent);

        area.removeFromTop (kGapAfter);
    }

    section (spaceSection);
    place (spaceLabel);

    if (roomNoteLabel.isVisible())
    {
        area.removeFromTop (kLineGap);
        place (roomNoteLabel);
    }
}

void CameraPanel::paint (juce::Graphics& g)
{
    g.fillAll (AppLookAndFeel::surface);

    // One hairline under each heading, in the outline tone, as in Settings.
    g.setColour (AppLookAndFeel::outline);
    const auto bounds = getLocalBounds().reduced (kMargin, 0);

    for (int y : ruleYs)
        g.fillRect (bounds.getX(), y, bounds.getWidth(), 1);
}

} // namespace mma
