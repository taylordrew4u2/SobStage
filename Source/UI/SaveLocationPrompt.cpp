#include "SaveLocationPrompt.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
    // The whole list is shown when it is short, which it usually is. Past this
    // the card would grow taller than a laptop screen, so the tail becomes a
    // count -- the point of the list is "one file each, plus the mix", and that
    // reads just as well from eight lines and a remainder.
    constexpr int kMaxListedFiles = 8;
}

SaveLocationPrompt::SaveLocationPrompt()
{
    setHeading ("Where does this recording go?",
                "Every take gets its own folder, and you can open it the moment you stop. "
                "This is asked once -- after that, record starts straight away.");

    styleBody (nameQuestion, AppLookAndFeel::bone);
    nameQuestion.setText ("What should this recording be called?", juce::dontSendNotification);
    addAndMakeVisible (nameQuestion);

    // §6.2: naming a take is never a gate. The placeholder says what happens to
    // someone who ignores the field entirely, which most people will.
    nameEditor.setTextToShowWhenEmpty ("Session", AppLookAndFeel::tertiary);
    nameEditor.setJustification (juce::Justification::centredLeft);
    nameEditor.onTextChange = [this] { refreshFolderName(); };
    // Return is the same as pressing the primary button, which is where a
    // finger already is after typing a name.
    nameEditor.onReturnKey = [this] { if (onStart && startButton.isEnabled()) onStart(); };
    addAndMakeVisible (nameEditor);

    styleBody (whereQuestion, AppLookAndFeel::bone);
    whereQuestion.setText ("You'll find it here:", juce::dontSendNotification);
    addAndMakeVisible (whereQuestion);

    stylePath (folderValue, AppLookAndFeel::bone);
    addAndMakeVisible (folderValue);

    stylePath (folderNameValue, AppLookAndFeel::accent);
    addAndMakeVisible (folderNameValue);

    styleBody (folderNameNote, AppLookAndFeel::tertiary);
    folderNameNote.setText ("The exact time and any _2 suffix are chosen when you press start.",
                            juce::dontSendNotification);
    addAndMakeVisible (folderNameNote);

    styleBody (contentsQuestion, AppLookAndFeel::bone);
    contentsQuestion.setText ("What'll be in it:", juce::dontSendNotification);
    addAndMakeVisible (contentsQuestion);

    stylePath (contentsValue, AppLookAndFeel::secondary);
    contentsValue.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (contentsValue);

    styleBody (blockedReason, AppLookAndFeel::warning);
    addChildComponent (blockedReason);

    // The thing about a take with a camera in it that most needs saying before
    // rather than after: the video has no sound in it, and that is deliberate.
    styleBody (videoNote, AppLookAndFeel::secondary);
    addChildComponent (videoNote);

    styleBody (mirrorValue, AppLookAndFeel::secondary);
    addChildComponent (mirrorValue);

    // Off by default: the answer is the same every time until the destination
    // moves, and asking again would be the confirmation dialog §10.4 forbids.
    askEveryTimeToggle.setToggleState (false, juce::dontSendNotification);
    addAndMakeVisible (askEveryTimeToggle);

    chooseButton.onClick = [this] { if (onChooseFolder) onChooseFolder(); };
    addAndMakeVisible (chooseButton);

    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible (cancelButton);

    // The primary action, filled with the accent like the record button it is
    // standing in front of -- pressing it is pressing record.
    startButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    startButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    startButton.onClick = [this] { if (onStart) onStart(); };
    addAndMakeVisible (startButton);
}

SaveLocationPrompt::~SaveLocationPrompt() = default;

void SaveLocationPrompt::setSessionName (const juce::String& name)
{
    nameEditor.setText (name, juce::dontSendNotification);
    refreshFolderName();
}

void SaveLocationPrompt::setPlan (const juce::String& parentFolder,
                                  const juce::String& folderName,
                                  const juce::String& mirrorFolder,
                                  const juce::StringArray& fileNames,
                                  int armedCameraCount,
                                  int readyCameraCount,
                                  const juce::String& mirrorNote,
                                  const juce::String& computerFolder)
{
    folderValue.setText (parentFolder, juce::dontSendNotification);
    folderNameValue.setText (folderName, juce::dontSendNotification);

    juce::StringArray listed;
    for (int i = 0; i < juce::jmin (fileNames.size(), kMaxListedFiles); ++i)
        listed.add (fileNames[i]);

    if (fileNames.size() > kMaxListedFiles)
        listed.add ("and " + juce::String (fileNames.size() - kMaxListedFiles) + " more");

    contentsValue.setText (listed.joinIntoString ("\n"), juce::dontSendNotification);
    contentsLines = juce::jmax (1, listed.size());

    const auto armed = juce::jmax (0, armedCameraCount);
    const auto ready = juce::jlimit (0, armed, readyCameraCount);
    videoNote.setVisible (armed > 0);
    contentsQuestion.setText (armed > ready ? "What this take is set up to make:"
                                            : "What'll be in it:",
                              juce::dontSendNotification);

    if (armed > 0 && ready == 0)
        videoNote.setText (juce::String (armed)
                               + (armed == 1 ? " camera is" : " cameras are")
                               + " switched on, but none has a live picture yet. Video records only "
                                 "if it is live when the take starts.",
                           juce::dontSendNotification);
    else if (ready < armed)
        videoNote.setText (juce::String (armed) + " cameras are switched on; "
                               + juce::String (ready) + (ready == 1 ? " is" : " are")
                               + " live and ready. Only live cameras record when the take starts.",
                           juce::dontSendNotification);
    else if (armed == 1)
        videoNote.setText ("1 camera is live and ready. Video and sound stay separate, and the "
                           "video has no sound of its own.",
                           juce::dontSendNotification);
    else if (armed > 1)
        videoNote.setText (juce::String (armed)
                               + " cameras are live and ready, one video file each. Video and sound "
                                 "stay separate files.",
                           juce::dontSendNotification);

    // §6.3: a second copy nobody knows about is a second copy nobody finds.
    // And a backup that will not be made for a reason the user can act on
    // (it would share the recording's disk) is said in the same place.
    mirrorValue.setVisible (mirrorFolder.isNotEmpty() || mirrorNote.isNotEmpty() || computerFolder.isNotEmpty());
    mirrorValueHeight = computerFolder.isNotEmpty() ? 64 : 32;

    // A take for a card is recorded here and copied when it stops: where it
    // lands first is where to look if the card copy goes wrong, so it is said
    // now, in place of the backup line -- the take itself is the copy here.
    if (computerFolder.isNotEmpty())
        mirrorValue.setText ("Recorded on this computer first, then copied to the card when you stop. "
                             "This computer keeps its copy in " + computerFolder + ".",
                             juce::dontSendNotification);
    else
        mirrorValue.setText (mirrorFolder.isNotEmpty() ? "A backup copy also goes to " + mirrorFolder
                                                       : mirrorNote,
                             juce::dontSendNotification);

    resized();
}

void SaveLocationPrompt::setBlockedReason (const juce::String& reason)
{
    const bool blocked = reason.isNotEmpty();

    startButton.setEnabled (! blocked);

    if (blockedReason.getText() == reason)
        return;

    blockedReason.setText (reason, juce::dontSendNotification);
    blockedReason.setVisible (blocked);
    resized();
}

void SaveLocationPrompt::refreshFolderName()
{
    if (folderNameFor)
        folderNameValue.setText (folderNameFor (nameEditor.getText()), juce::dontSendNotification);
}

void SaveLocationPrompt::prepareToShow()
{
    refreshFolderName();
    nameEditor.grabKeyboardFocus();
    nameEditor.moveCaretToEnd();
}

bool SaveLocationPrompt::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onCancel)
            onCancel();

        return true;
    }

    return false;
}

int SaveLocationPrompt::getContentHeight() const
{
    int height = kRowHeight + 4 + 30    // the name question and its field
               + 18                     // gap
               + kRowHeight + 2 + kRowHeight + 2 + kRowHeight  // where: question, folder, folder name
               + 16                     // the note about the timestamp
               + 16                     // gap
               + kRowHeight + 4 + contentsLines * 17;

    if (videoNote.isVisible())
        height += 8 + 32;

    if (mirrorValue.isVisible())
        height += 8 + mirrorValueHeight;

    if (blockedReason.isVisible())
        height += 8 + 32;

    return height + 18 + kRowHeight + 14 + kButtonHeight;
}

void SaveLocationPrompt::layOutContent (juce::Rectangle<int> area)
{
    nameQuestion.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (4);
    nameEditor.setBounds (area.removeFromTop (30));

    area.removeFromTop (18);
    whereQuestion.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (2);
    folderValue.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (2);
    folderNameValue.setBounds (area.removeFromTop (kRowHeight));
    folderNameNote.setBounds (area.removeFromTop (16));

    area.removeFromTop (16);
    contentsQuestion.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (4);
    contentsValue.setBounds (area.removeFromTop (contentsLines * 17));

    if (videoNote.isVisible())
    {
        area.removeFromTop (8);
        videoNote.setBounds (area.removeFromTop (32));
    }

    if (mirrorValue.isVisible())
    {
        area.removeFromTop (8);
        mirrorValue.setBounds (area.removeFromTop (mirrorValueHeight));
    }

    if (blockedReason.isVisible())
    {
        area.removeFromTop (8);
        blockedReason.setBounds (area.removeFromTop (32));
    }

    area.removeFromTop (18);
    askEveryTimeToggle.setBounds (area.removeFromTop (kRowHeight));

    area.removeFromTop (14);
    auto buttons = area.removeFromTop (kButtonHeight);

    // The way out sits on the left, away from the primary action, so a
    // fast second click on "start" can never land on "not yet".
    cancelButton.setBounds (buttons.removeFromLeft (90));
    buttons.removeFromLeft (8);
    startButton.setBounds (buttons.removeFromRight (juce::jmin (150, buttons.getWidth() / 2)));
    buttons.removeFromRight (8);
    chooseButton.setBounds (buttons);
}

} // namespace mma
