#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace mma {

/// A card floating in the middle of a dimmed window.
///
/// The app stops and talks to the user about their files at exactly two
/// moments: before the first take of a run, and after every take. §6.2 calls a
/// novice losing track of their recording a total product failure, so those two
/// moments are the ones least worth getting wrong -- and a juce::AlertWindow,
/// while fewer lines, would have shown up in stock chrome with stock spacing and
/// none of the §9.2 palette, looking like a different application had
/// interrupted this one.
class ModalCard : public juce::Component
{
public:
    ModalCard();
    ~ModalCard() override;

    void paint (juce::Graphics& g) override;

    /// Places the card and hands the subclass the space inside it. Final
    /// because the card's own geometry is this class's business; a subclass
    /// arranges its contents in layOutContent().
    void resized() final;

    /// Swallow anything aimed at the window underneath. A modal card that let
    /// clicks through would let someone press record behind the question that
    /// is asking them where the recording goes.
    void mouseDown (const juce::MouseEvent&) override {}
    void mouseUp (const juce::MouseEvent&) override {}
    void mouseDrag (const juce::MouseEvent&) override {}
    void mouseDoubleClick (const juce::MouseEvent&) override {}

    /// The height the whole card wants, so a container can tell whether it
    /// fits before showing it.
    int getRequiredHeight() const;

    /// A line at the foot of the card about the app rather than the card:
    /// while SobStage waits to quit until the combined video or the podcast
    /// copy is finished, it says so here. The main screen says it too, on
    /// the line under the record button -- but a card covers that line, and
    /// the saved-take card is exactly what is up after a take. Empty removes
    /// it. Counted in getRequiredHeight(), so the owner grows the window for
    /// it the way it does for the card.
    void setFootnote (const juce::String& text);

protected:
    /// Height the subclass's own content needs: everything below the heading
    /// rows and above the card's bottom padding.
    virtual int getContentHeight() const = 0;

    /// Lay the subclass's children out in `area` -- the card with its padding
    /// and heading rows already taken off.
    virtual void layOutContent (juce::Rectangle<int> area) = 0;

    void setHeading (const juce::String& heading, const juce::String& subheading);
    void setHeadingColour (juce::Colour colour);

    /// Note for anything added here later: a child that starts hidden goes in
    /// with addChildComponent, never addAndMakeVisible followed by
    /// setVisible(false) -- addAndMakeVisible makes it visible whatever was
    /// asked for a line earlier, and a card that reserves a row for an empty
    /// warning label has a hole in the middle of it.

    /// A supporting line: small, quiet, and wrapping rather than clipping.
    static void styleBody (juce::Label& label, const juce::Colour& colour);
    /// A path, a filename, anything the user may want to read character by
    /// character. Monospaced so a path does not turn into a smear.
    static void stylePath (juce::Label& label, const juce::Colour& colour);

    static constexpr int kCardWidth   = 480;
    static constexpr int kCardPadding = 22;
    static constexpr int kRowHeight   = 22;
    static constexpr int kButtonHeight = 38;
    /// Between the card's content (its buttons, last) and setFootnote()'s line.
    static constexpr int kFootnoteGap = 14;

    juce::Rectangle<int> getCardBounds() const;
    /// The card's width alone, which does not depend on its height -- so a
    /// subclass can measure wrapped text for getContentHeight() with it, which
    /// getCardBounds() (sized from that very height) cannot be used for.
    int getCardWidth() const;

private:
    juce::Label headingLabel, subheadingLabel, footnoteLabel;
    int headingBlockHeight() const;
    int footnoteBlockHeight() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModalCard)
};

} // namespace mma
