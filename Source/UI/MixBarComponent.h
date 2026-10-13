#pragma once
#include "../Core/ElapsedClock.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/Metering.h"

namespace mma {

/// §9.1: the shared mix uses a horizontal bar, not a ring -- visually
/// distinct so the bus is never confused with a channel.
class MixBarComponent : public juce::Component, private juce::Timer
{
public:
    MixBarComponent();
    ~MixBarComponent() override;

    void setMetering (Metering* meteringSource) { metering = meteringSource; }

    /// See ChannelMeterComponent::setRepaintPaused.
    void setRepaintPaused (bool paused) noexcept { repaintPaused = paused; }

    void paint (juce::Graphics& g) override;

    /// §9.1: tap to acknowledge and clear the clip latch, as on a strip.
    void mouseUp (const juce::MouseEvent& event) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void clearClip();
    mma::ElapsedClock meterClock { 1.0 / 60.0 };
    void timerCallback() override;
    void updateAccessibilityText();

    Metering* metering = nullptr;
    int lastAccessibleLevelDb = 1;
    bool repaintPaused = false;
    float currentLevelDb = Metering::kMinDb;
    float currentPeakDb = Metering::kMinDb;
    bool currentClip = false;
    int currentClipCount = 0;

    static const juce::Colour kPanel;
    static const juce::Colour kEmptyInterior;
    static const juce::Colour kFillLow;
    static const juce::Colour kFillMid;
    static const juce::Colour kFillHigh;
    static const juce::Colour kBone;
    static const juce::Colour kSecondaryText;
    static const juce::Colour kOutline;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixBarComponent)
};

} // namespace mma
