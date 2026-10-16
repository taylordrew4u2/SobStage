#pragma once
#include "ModalCard.h"
#include <functional>

namespace mma {

/// The questions asked before a take starts: what is this called, and where is
/// it going to be.
///
/// §10.4 says the record button starts immediately with no confirmation, and
/// this does not change that: once the answer is given it is remembered for the
/// destination it was given about, and every later press goes straight to
/// recording. What it removes is the case §6.2 warns about -- someone finishes a
/// take and has no idea where the files are, because nothing ever told them.
/// The answer is cheapest to give before there is anything to lose.
class SaveLocationPrompt : public ModalCard
{
public:
    SaveLocationPrompt();
    ~SaveLocationPrompt() override;

    /// Everything the card states about where this take will land. Called each
    /// time the card is shown, since the destination and the mic set move.
    void setPlan (const juce::String& parentFolder,
                  const juce::String& folderName,
                  const juce::String& mirrorFolder,
                  const juce::StringArray& fileNames,
                  int armedCameraCount,
                  int readyCameraCount,
                  const juce::String& mirrorNote = {},
                  const juce::String& computerFolder = {},
                  const juce::String& roomNote = {});

    void setSessionName (const juce::String& name);
    juce::String getSessionName() const { return nameEditor.getText(); }

    /// §6.4: why recording cannot start yet -- an unbenchmarked destination, no
    /// microphones. Empty enables the start button; anything else disables it
    /// and says so, rather than leaving a button that does nothing when pressed.
    void setBlockedReason (const juce::String& reason);

    void setAskEveryTime (bool ask) { askEveryTimeToggle.setToggleState (ask, juce::dontSendNotification); }
    bool getAskEveryTime() const { return askEveryTimeToggle.getToggleState(); }

    /// Resolves a typed session name to the folder name it would produce, so
    /// the name on screen is the name the user will go looking for rather than
    /// a stale one from before they typed. Supplied by the owner, because only
    /// the app knows about collision suffixes.
    std::function<juce::String (const juce::String&)> folderNameFor;

    std::function<void()> onStart;
    std::function<void()> onChooseFolder;
    std::function<void()> onCancel;

    /// Puts the caret in the name field. Called when the card is shown, so the
    /// first question is also the first thing the keyboard is pointed at.
    void prepareToShow();

    bool keyPressed (const juce::KeyPress& key) override;

protected:
    int getContentHeight() const override;
    void layOutContent (juce::Rectangle<int> area) override;

private:
    void refreshFolderName();

    juce::Label nameQuestion;
    juce::TextEditor nameEditor;

    juce::Label whereQuestion;
    juce::Label folderValue;      // the parent folder, in full
    juce::Label folderNameValue;  // the folder this take will make inside it
    juce::Label folderNameNote;

    juce::Label contentsQuestion;
    juce::Label contentsValue;
    int contentsLines = 1;

    juce::Label blockedReason;
    juce::Label videoNote;
    juce::Label mirrorValue;
    /// Height of the backup line, measured from its words at `contentWidth`:
    /// the "recorded on this computer first" line carries a path and, when
    /// this computer runs out before the card would, how long it can record
    /// for -- six lines or more on the narrowest card.
    int mirrorValueHeight (int contentWidth) const { return wrappedHeight (mirrorValue, contentWidth); }
    /// The same for why Record is refused, which can carry a card's reason
    /// and what to delete on this computer.
    int blockedReasonHeight (int contentWidth) const { return wrappedHeight (blockedReason, contentWidth); }
    /// As tall as `label`'s words need at `width`, and never under two lines.
    static int wrappedHeight (const juce::Label& label, int width);
    /// The width layOutContent() will be given, for getContentHeight().
    int contentWidth() const;

    juce::ToggleButton askEveryTimeToggle { "Ask me this before every recording" };
    juce::TextButton chooseButton { "Choose a different folder..." };
    juce::TextButton cancelButton  { "Not yet" };
    juce::TextButton startButton   { "Start recording" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SaveLocationPrompt)
};

} // namespace mma
