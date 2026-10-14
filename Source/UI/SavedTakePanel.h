#pragma once
#include "ModalCard.h"
#include "../Core/TakeCompleteness.h"
#include <functional>
#include <memory>
#include <vector>

namespace mma {

/// §6.2: "on stop, show the location and offer to open the containing folder."
///
/// Not a line of status text -- the files themselves, named, with their sizes,
/// as they landed on disk. A user who can see MIX.wav and one file per
/// microphone, each with a plausible size next to it, knows the take worked and
/// knows what to go and get. A user who is told "Saved" knows neither.
class SavedTakePanel : public ModalCard
{
public:
    SavedTakePanel();
    ~SavedTakePanel() override;

    struct FileRow
    {
        juce::String name;
        int64_t sizeBytes = 0;
    };

    /// `verdict` comes from the engine. The panel deliberately does not work
    /// it out: it can see file sizes and nothing else, and a take that ran for
    /// its full length carrying silence looks identical to a good one from
    /// here. Only the writer knows whether any signal actually went past.
    void setTake (const juce::String& folder,
                  const juce::String& mirrorFolder,
                  const std::vector<FileRow>& files,
                  TakeAudioVerdict verdict,
                  bool filesListed = true,
                  const juce::String& mirrorNote = {});

    /// §6.5: the take was stopped by the drive going away rather than by the
    /// user. Shown loudly above the file list, and the heading stops claiming
    /// the take simply saved. Empty restores the ordinary "Saved." panel.
    void setProblem (const juce::String& text);

    /// A take recorded on this computer and being copied to the card: the
    /// copy's one line in place of the backup line ("Copying to the card:
    /// 40%.", "Copied to the card: ..."), and "Copy again" when a copy that
    /// failed can be tried again. Empty `line` hides it. Cheap to call on a
    /// timer: nothing moves unless the line or the button changed.
    void setCardCopy (const juce::String& line, bool canCopyAgain);

    /// Why the take went where it did, when the user should hear it ("This
    /// computer is nearly full, so this take is going straight to the
    /// card."). Empty hides it.
    void setRouteNote (const juce::String& note);

    std::function<void()> onCopyAgain;

    /// True when the panel is showing nothing but empty files, which is what a
    /// take that failed to write looks like. The owner uses this to say so
    /// rather than calling it saved.
    bool takeIsEmpty() const { return everythingWasEmpty; }

    std::function<void()> onOpenFolder;
    std::function<void()> onDone;

    bool keyPressed (const juce::KeyPress& key) override;
    void prepareToShow() { doneButton.grabKeyboardFocus(); }

protected:
    int getContentHeight() const override;
    void layOutContent (juce::Rectangle<int> area) override;

private:
    // Rows are components rather than one multi-line label so the size can be
    // right-aligned in its own column. Left to a single label, proportional
    // digits put every size at a different x and the column stopped scanning.
    struct Row
    {
        std::unique_ptr<juce::Label> name;
        std::unique_ptr<juce::Label> size;
    };

    juce::Label folderValue;
    juce::Label totalLabel;
    juce::Viewport listViewport;
    juce::Component listContainer;
    std::vector<Row> rows;
    juce::Label mirrorValue;
    juce::Label emptyWarning;
    juce::Label problemLabel;
    juce::Label routeNote;

    /// The line under the files is the card copy's sentence, not a backup's
    /// path: it is as tall as its words need (wrappedHeight).
    bool mirrorIsCardCopy = false;

    /// The height `label` needs to draw all of its text, wrapped at the width
    /// the card gives it, and never less than `atLeast`. The card copy's
    /// longest sentences end with the folder the take is safe in, and a fixed
    /// three lines cut exactly that off.
    int wrappedHeight (const juce::Label& label, int atLeast) const;

    juce::TextButton openButton { "Open the folder" };
    juce::TextButton copyAgainButton { "Copy again" };
    juce::TextButton doneButton { "Done" };

    bool everythingWasEmpty = false;
    int listHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SavedTakePanel)
};

} // namespace mma
