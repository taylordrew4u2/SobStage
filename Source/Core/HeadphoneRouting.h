#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mma {

/// One sub-device of the combined device, in the order its output channels
/// appear: the device's id and how many output channels it brings.
struct CombinedDeviceOutputs
{
    std::string deviceId;
    int outputChannels = 0;
};

/// The gain for every output channel of the combined device: 1 for a channel
/// on a microphone whose wearer hears the mix, 0 for one whose headphones are
/// switched off. The combined device lays its outputs out sub-device after
/// sub-device, so a microphone's jack is the run of channels it brings.
///
/// Everyone who hears anything hears the same mix (§5.2) -- this only decides
/// whose headphones it reaches.
inline std::vector<float> headphoneChannelGains (const std::vector<CombinedDeviceOutputs>& layout,
                                                 const std::function<bool (const std::string&)>& hearsMix)
{
    std::vector<float> gains;

    for (const auto& device : layout)
    {
        const float gain = (hearsMix == nullptr || hearsMix (device.deviceId)) ? 1.0f : 0.0f;

        for (int ch = 0; ch < device.outputChannels; ++ch)
            gains.push_back (gain);
    }

    return gains;
}

/// The combined device's headphone jacks as the app knows them: its layout
/// as last read, and who hears the mix. Every layout read, and every lay()
/// (someone switched on or off), hands the gains on through the LayGains
/// given at construction, always as the combined device's own -- which stream
/// they reach is CaptureCoordinator::setHeadphoneJackGains()'s to decide, from
/// the stream that is actually open, never from the output chosen.
///
/// The layout is only ever stored through layoutRead(), which lays the gains
/// every time, so no read of it can leave them behind: a sub-device leaving
/// the combined device (a cable bumped mid-take) or joining it late (slower
/// than the publish wait) moves every later jack's channels, and nothing else
/// lays them again -- mid-take the reopen waits for Stop, and between takes an
/// unchanged rig is not reopened.
class HeadphoneJacks
{
public:
    /// Whether the wearer of the microphone with this sub-device id hears the mix.
    using HearsMix = std::function<bool (const std::string& subDeviceId)>;
    /// Hands the gains on: the device whose channels they describe, and the gains.
    using LayGains = std::function<void (const std::string& outputDeviceId, const std::vector<float>& gains)>;

    HeadphoneJacks (std::string combinedDeviceIdToUse, HearsMix hearsMixToUse, LayGains layToUse)
        : combinedDeviceId (std::move (combinedDeviceIdToUse)),
          hearsMix (std::move (hearsMixToUse)),
          layGains (std::move (layToUse))
    {
    }

    /// The combined device's layout, just read (empty when there is none).
    /// Lays the gains again whether or not it changed.
    void layoutRead (std::vector<CombinedDeviceOutputs> newLayout)
    {
        layout = std::move (newLayout);
        lay();
    }

    /// Lays the gains again from the layout as last read: after someone's
    /// headphones are switched on or off, or onto a new coordinator.
    void lay() const
    {
        if (layGains != nullptr)
            layGains (combinedDeviceId, headphoneChannelGains (layout, hearsMix));
    }

    const std::vector<CombinedDeviceOutputs>& getLayout() const noexcept { return layout; }

private:
    std::string combinedDeviceId;
    HearsMix hearsMix;
    LayGains layGains;
    std::vector<CombinedDeviceOutputs> layout;
};

/// True when at least one microphone in the combined device has a headphone
/// jack to send the mix to. A rig of microphones with no outputs has nothing
/// for the combined device to play through.
inline bool combinedDeviceHasHeadphones (const std::vector<CombinedDeviceOutputs>& layout)
{
    for (const auto& device : layout)
        if (device.outputChannels > 0)
            return true;

    return false;
}

/// True when every expected sub-device is in the combined device's active
/// layout. Order is the HAL's to choose, so only membership is compared.
inline bool combinedLayoutIsComplete (const std::vector<CombinedDeviceOutputs>& layout,
                                      const std::vector<std::string>& expectedIds)
{
    for (const auto& id : expectedIds)
        if (std::none_of (layout.begin(), layout.end(),
                          [&id] (const CombinedDeviceOutputs& d) { return d.deviceId == id; }))
            return false;

    return true;
}

/// A combined device just created is built by the OS asynchronously: for a
/// moment its active sub-device list can be empty, and a layout read then says
/// "no headphone jacks" about a rig full of them. Reads `readLayout` until it
/// lists every expected sub-device, calling `pause` between reads, at most
/// `maxPauses` times. Returns whether it got there; the caller goes on either
/// way, so a sub-device that never activates costs only the bound.
inline bool waitForCombinedLayout (const std::function<std::vector<CombinedDeviceOutputs>()>& readLayout,
                                   const std::vector<std::string>& expectedIds,
                                   int maxPauses,
                                   const std::function<void()>& pause)
{
    for (int pauses = 0;; ++pauses)
    {
        if (combinedLayoutIsComplete (readLayout(), expectedIds))
            return true;

        if (pauses >= maxPauses)
            return false;

        if (pause != nullptr)
            pause();
    }
}

} // namespace mma
