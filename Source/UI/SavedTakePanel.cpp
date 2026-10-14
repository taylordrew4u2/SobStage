#include "SavedTakePanel.h"
#include "../Core/TakeCompleteness.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
    constexpr int kRowPitch = 20;
    // Past this the list scrolls rather than the card growing off the screen.
    constexpr int kMaxVisibleRows = 9;
}

SavedTakePanel::SavedTakePanel()
{
    setHeading ("Saved.", {});

    stylePath (folderValue, AppLookAndFeel::bone);
    addAndMakeVisible (folderValue);

    styleBody (totalLabel, AppLookAndFeel::secondary);
    addAndMakeVisible (totalLabel);

    listViewport.setViewedComponent (&listContainer, false);
    listViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (listViewport);

    styleBody (mirrorValue, AppLookAndFeel::secondary);
    addChildComponent (mirrorValue);

    // §10.6: what happened, then what to do, in one sentence and no code.
    styleBody (emptyWarning, AppLookAndFeel::warning);
    emptyWarning.setText ("These files are empty. Check that the microphones aren't muted "
                          "at their own switches, then record again.", juce::dontSendNotification);
    addChildComponent (emptyWarning);

    // §6.5 "alert loudly". Danger red rather than the warning amber the
    // empty-files line uses: this is not something to look at later, it is the
    // take having been ended by the hardware.
    styleBody (problemLabel, AppLookAndFeel::danger);
    addChildComponent (problemLabel);

    styleBody (routeNote, AppLookAndFeel::warning);
    addChildComponent (routeNote);

    copyAgainButton.onClick = [this] { if (onCopyAgain) onCopyAgain(); };
    addChildComponent (copyAgainButton);

    // The offer §6.2 asks for, and the reason this panel exists rather than a
    // status line: a path someone has to retype is a path they will not visit.
    openButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    openButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    openButton.onClick = [this] { if (onOpenFolder) onOpenFolder(); };
    addAndMakeVisible (openButton);

    doneButton.onClick = [this] { if (onDone) onDone(); };
    addAndMakeVisible (doneButton);
}

SavedTakePanel::~SavedTakePanel() = default;

void SavedTakePanel::setTake (const juce::String& folder,
                              const juce::String& mirrorFolder,
                              const std::vector<FileRow>& files,
                              TakeAudioVerdict verdict,
                              bool filesListed,
                              const juce::String& mirrorNote)
{
    folderValue.setText (folder, juce::dontSendNotification);

    rows.clear();
    listContainer.removeAllChildren();

    int64_t total = 0;
    std::vector<TakeFile> forJudging;
    forJudging.reserve (files.size());

    for (const auto& file : files)
    {
        total += file.sizeBytes;
        forJudging.push_back ({ file.name.toStdString(), file.sizeBytes });

        Row row;
        row.name = std::make_unique<juce::Label>();
        stylePath (*row.name, AppLookAndFeel::bone);
        row.name->setText (file.name, juce::dontSendNotification);

        row.size = std::make_unique<juce::Label>();
        stylePath (*row.size, AppLookAndFeel::secondary);
        row.size->setText (juce::File::descriptionOfSizeInBytes (file.sizeBytes),
                           juce::dontSendNotification);
        row.size->setJustificationType (juce::Justification::centredRight);

        listContainer.addAndMakeVisible (*row.name);
        listContainer.addAndMakeVisible (*row.size);
        rows.push_back (std::move (row));
    }

    // "0 files, 0 bytes" over a folder that simply did not answer reads as a
    // take that wrote nothing, which nobody can know.
    totalLabel.setText (filesListed
                            ? juce::String (files.size()) + " files, "
                                  + juce::File::descriptionOfSizeInBytes (total)
                            : juce::String ("The drive didn't answer, so its files couldn't be listed."),
                        juce::dontSendNotification);

    // Handed in, not worked out here. The status line and this panel have to
    // reach the same verdict -- they once disagreed, this warning the files
    // were empty while the line beside it said "Saved to ..." -- and only the
    // engine can tell a real take from a full-length silent one, because from
    // here the two are the same list of megabyte files.
    everythingWasEmpty = verdict == TakeAudioVerdict::NothingWritten
                      || verdict == TakeAudioVerdict::OnlySilence
                      || verdict == TakeAudioVerdict::DroppedByApp;

    emptyWarning.setText (verdict == TakeAudioVerdict::DroppedByApp
                          ? "The sound reached SobStage but not these files, so your "
                            "microphones were working -- this is a fault in the app, not "
                            "your setup. Please report it."
                          : verdict == TakeAudioVerdict::OnlySilence
                              ? "These files are silent. The microphones were connected, but no "
                                "sound reached them -- check they aren't muted at their own "
                                "switches, and that the right input is selected."
                              : "These files are empty. Check that the microphones aren't muted "
                                "at their own switches, then record again.",
                          juce::dontSendNotification);

    emptyWarning.setVisible (everythingWasEmpty);

    mirrorValue.setVisible (mirrorFolder.isNotEmpty() || mirrorNote.isNotEmpty());
    mirrorValue.setText (mirrorFolder.isNotEmpty() ? "A second copy is in " + mirrorFolder : mirrorNote,
                         juce::dontSendNotification);
    mirrorValueHeight = kRowHeight;
    copyAgainButton.setVisible (false);
    routeNote.setVisible (false);

    listHeight = juce::jmin ((int) rows.size(), kMaxVisibleRows) * kRowPitch;

    resized();
}

void SavedTakePanel::setProblem (const juce::String& text)
{
    problemLabel.setText (text, juce::dontSendNotification);
    problemLabel.setVisible (text.isNotEmpty());

    // §9.3: the heading carries it too, not just the colour. "Saved." over a
    // take the drive cut short would be the app agreeing with a user who thinks
    // nothing went wrong.
    setHeading (text.isNotEmpty() ? "The recording was stopped." : "Saved.", {});

    resized();
}

void SavedTakePanel::setCardCopy (const juce::String& line, bool canCopyAgain)
{
    const bool lineChanged = mirrorValue.getText() != line || mirrorValue.isVisible() != line.isNotEmpty();
    const bool buttonChanged = copyAgainButton.isVisible() != canCopyAgain;

    if (! lineChanged && ! buttonChanged)
        return;

    mirrorValue.setText (line, juce::dontSendNotification);
    mirrorValue.setVisible (line.isNotEmpty());
    mirrorValueHeight = 3 * kRowHeight;
    copyAgainButton.setVisible (canCopyAgain);
    resized();
}

void SavedTakePanel::setRouteNote (const juce::String& note)
{
    routeNote.setText (note, juce::dontSendNotification);
    routeNote.setVisible (note.isNotEmpty());
    resized();
}

bool SavedTakePanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
    {
        if (onDone)
            onDone();

        return true;
    }

    return false;
}

int SavedTakePanel::getContentHeight() const
{
    int height = kRowHeight + 2 + kRowHeight   // the folder, then the file count
               + 12 + listHeight;

    if (problemLabel.isVisible())
        height += 52 + 12;

    if (routeNote.isVisible())
        height += 52 + 12;

    if (mirrorValue.isVisible())
        height += 10 + mirrorValueHeight;

    if (emptyWarning.isVisible())
        height += 10 + 34;

    return height + 18 + kButtonHeight;
}

void SavedTakePanel::layOutContent (juce::Rectangle<int> area)
{
    // Above the folder and the files: what happened comes before what survived.
    if (problemLabel.isVisible())
    {
        problemLabel.setBounds (area.removeFromTop (52));
        area.removeFromTop (12);
    }

    if (routeNote.isVisible())
    {
        routeNote.setBounds (area.removeFromTop (52));
        area.removeFromTop (12);
    }

    folderValue.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (2);
    totalLabel.setBounds (area.removeFromTop (kRowHeight));

    area.removeFromTop (12);
    listViewport.setBounds (area.removeFromTop (listHeight));

    const int contentWidth = listViewport.getWidth()
                           - (listViewport.isVerticalScrollBarShown() ? listViewport.getScrollBarThickness() : 0);
    listContainer.setSize (juce::jmax (1, contentWidth), (int) rows.size() * kRowPitch);

    int y = 0;
    for (auto& row : rows)
    {
        juce::Rectangle<int> line (0, y, listContainer.getWidth(), kRowPitch);
        row.size->setBounds (line.removeFromRight (juce::jmin (96, line.getWidth() / 3)));
        row.name->setBounds (line);
        y += kRowPitch;
    }

    if (mirrorValue.isVisible())
    {
        area.removeFromTop (10);
        mirrorValue.setBounds (area.removeFromTop (mirrorValueHeight));
    }

    if (emptyWarning.isVisible())
    {
        area.removeFromTop (10);
        emptyWarning.setBounds (area.removeFromTop (34));
    }

    area.removeFromTop (18);
    auto buttons = area.removeFromTop (kButtonHeight);
    doneButton.setBounds (buttons.removeFromLeft (90));
    openButton.setBounds (buttons.removeFromRight (juce::jmin (170, buttons.getWidth())));

    if (copyAgainButton.isVisible())
    {
        buttons.removeFromRight (8);
        copyAgainButton.setBounds (buttons.removeFromRight (juce::jmin (120, juce::jmax (0, buttons.getWidth() - 8))));
    }
}

} // namespace mma
