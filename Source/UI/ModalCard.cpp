#include "ModalCard.h"
#include "AppLookAndFeel.h"
#include <cmath>

namespace mma {

ModalCard::ModalCard()
{
    // The card is the only thing on screen that matters while it is up, so it
    // takes the keys too -- Return starts, Escape backs out, and neither should
    // reach the main screen's spacebar mute behind it.
    setWantsKeyboardFocus (true);
    // Make this the root of keyboard traversal. JUCE's default traverser then
    // wraps Tab and Shift-Tab among this card's focusable descendants instead
    // of walking into the dimmed controls behind it. Mouse input is already
    // swallowed by the full-window component below, and MainComponent already
    // ignores its global shortcuts while a card is visible, so no scattered
    // enable/disable state is needed in the owner.
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setInterceptsMouseClicks (true, true);

    headingLabel.setFont (juce::Font (20.0f, juce::Font::bold));
    headingLabel.setColour (juce::Label::textColourId, AppLookAndFeel::bone);
    addAndMakeVisible (headingLabel);

    styleBody (subheadingLabel, AppLookAndFeel::secondary);
    addAndMakeVisible (subheadingLabel);

    // setFootnote(). In the colour the main screen gives the same sentence,
    // so the two read as one message. Hidden until there is something to say.
    styleBody (footnoteLabel, AppLookAndFeel::meterLow);
    addChildComponent (footnoteLabel);
}

ModalCard::~ModalCard() = default;

void ModalCard::styleBody (juce::Label& label, const juce::Colour& colour)
{
    label.setFont (juce::Font (13.0f));
    label.setColour (juce::Label::textColourId, colour);
    label.setMinimumHorizontalScale (1.0f); // wrap, never shrink the text to fit
}

void ModalCard::stylePath (juce::Label& label, const juce::Colour& colour)
{
    label.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    label.setColour (juce::Label::textColourId, colour);
    label.setMinimumHorizontalScale (0.75f); // a long path shrinks rather than truncating
}

void ModalCard::setHeading (const juce::String& heading, const juce::String& subheading)
{
    headingLabel.setText (heading, juce::dontSendNotification);
    subheadingLabel.setText (subheading, juce::dontSendNotification);
    subheadingLabel.setVisible (subheading.isNotEmpty());
}

int ModalCard::headingBlockHeight() const
{
    // Two lines of room for the subheading: these are sentences, not labels,
    // and one of them ("Every take gets its own folder...") does not fit in one.
    return 30 + (subheadingLabel.isVisible() ? 8 + 34 : 0);
}

int ModalCard::getRequiredHeight() const
{
    return kCardPadding * 2 + headingBlockHeight() + 16 + getContentHeight() + footnoteBlockHeight();
}

void ModalCard::setFootnote (const juce::String& text)
{
    if (text == footnoteLabel.getText() && text.isNotEmpty() == footnoteLabel.isVisible())
        return;

    footnoteLabel.setText (text, juce::dontSendNotification);
    footnoteLabel.setVisible (text.isNotEmpty());

    // The card itself changes height, not just the line: lay it out again and
    // repaint its body, which paint() draws from getCardBounds().
    resized();
    repaint();
}

int ModalCard::footnoteBlockHeight() const
{
    if (! footnoteLabel.isVisible())
        return 0;

    // Measured at the width the card will have; before it has been given one,
    // its full width, as RecoveredTakesPanel does.
    const int cardWidth = getWidth() > 0 ? getCardWidth() : kCardWidth;
    const auto font = footnoteLabel.getFont();
    const auto border = footnoteLabel.getBorderSize();
    const float available = (float) juce::jmax (1, cardWidth - 2 * kCardPadding - border.getLeftAndRight());
    const float textWidth = font.getStringWidthFloat (footnoteLabel.getText());

    // JUCE's own rule in GlyphArrangement: one line when it fits, otherwise
    // the fewest lines that beat the text's width plus an 80px allowance for
    // ragged breaks. The longest wording ("... the podcast copy and the
    // combined video.") fits a full-width card on one line, and needs two
    // on the narrowest.
    const int lines = textWidth < available
                          ? 1
                          : juce::jlimit (1, 3, (int) ((textWidth + 80.0f) / available) + 1);

    const int lineHeight = (int) std::ceil (font.getHeight()) + 3;
    const int labelHeight = juce::jmax (kRowHeight, lines * lineHeight + border.getTopAndBottom() + 2);

    return kFootnoteGap + labelHeight;
}

int ModalCard::getCardWidth() const
{
    return juce::jmin (kCardWidth, juce::jmax (280, getWidth() - 32));
}

juce::Rectangle<int> ModalCard::getCardBounds() const
{
    const int width = getCardWidth();
    const int height = juce::jmin (getRequiredHeight(), juce::jmax (160, getHeight() - 32));

    return juce::Rectangle<int> (width, height).withCentre (getLocalBounds().getCentre());
}

void ModalCard::setHeadingColour (juce::Colour colour)
{
    headingLabel.setColour (juce::Label::textColourId, colour);
}

void ModalCard::resized()
{
    auto card = getCardBounds().reduced (kCardPadding);

    headingLabel.setBounds (card.removeFromTop (30));

    if (subheadingLabel.isVisible())
    {
        card.removeFromTop (8);
        subheadingLabel.setBounds (card.removeFromTop (34));
    }

    // The foot of the card before its content: the content lays itself out
    // from the top, and in a window too short for the whole card the
    // footnote still shows rather than being pushed past the bottom edge.
    if (footnoteLabel.isVisible())
    {
        footnoteLabel.setBounds (card.removeFromBottom (footnoteBlockHeight() - kFootnoteGap));
        card.removeFromBottom (kFootnoteGap);
    }

    card.removeFromTop (16);
    layOutContent (card);
}

void ModalCard::paint (juce::Graphics& g)
{
    // Dim rather than hide: the meters stay visible behind the card, so it
    // still reads as this application asking, not as a system dialog arriving.
    g.fillAll (AppLookAndFeel::background.withAlpha (0.86f));

    const auto card = getCardBounds().toFloat();

    g.setColour (AppLookAndFeel::surface);
    g.fillRoundedRectangle (card, 12.0f);

    g.setColour (AppLookAndFeel::outline);
    g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, 1.0f);
}

} // namespace mma
