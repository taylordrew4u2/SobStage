#pragma once
#include <memory>
#include <string>
#include <vector>
#include "BusPowerDetector.h"
#include "ControllerContentionDetector.h"
#include "DeadChannelDetector.h"
#include "PolarPatternDetector.h"

namespace mma {

enum class SetupIssue
{
    BusPowerExhausted,    // §14.2

    /// §14.2's other half: one device failing again and again while nothing
    /// else does. That is its cable or port, not the hub's power, and saying
    /// "needs more power" about it sent people shopping for the wrong thing.
    DeviceKeepsDropping,

    ControllerContention, // §14.3
    NonCardioidPattern,   // §14.4
    SilentChannel,        // §8.1 / §10.5 -- usually the hardware mute switch

    /// §10.1: nothing has arrived from ANY microphone since monitoring began.
    /// DeadChannelDetector cannot report this and should not: a channel counts
    /// as dead only while another is alive, which is what separates a broken
    /// mic from a quiet room. When they are all silent there is no contrast to
    /// find, so the app said nothing at all -- about the one case §10.1 names
    /// as "the first real obstacle".
    EverythingSilent,
};

struct SetupAdvice
{
    SetupIssue issue;

    /// §10.6: what happened, then what to do, in one sentence. No codes, no
    /// apologies. The channel is named rather than numbered where one applies.
    std::string message;

    /// Channel this is about, or -1 when it concerns the whole rig.
    int channelIndex = -1;
};

/// §10.5 physical setup guidance. Novices fail on hardware, not software, so
/// the detectors for those failures are only worth having if something runs
/// them and turns them into a sentence. This is that something.
class SetupAdvisor
{
public:
    /// §14.2: `deviceId` (shown as `name`) failed to open or dropped off the
    /// bus, with `devicesAttached` devices plugged in. Counted once per
    /// failure episode: see noteDeviceHealthy().
    void noteDeviceDropout (double nowSeconds, const std::string& deviceId, const std::string& name,
                            int devicesAttached);

    /// The device opened or came back, so its next failure counts again.
    void noteDeviceHealthy (const std::string& deviceId);

    /// The device left the device list, so its next failure, once it is
    /// back, counts again (BusPowerDetector::noteLeft).
    void noteDeviceLeft (const std::string& deviceId);

    /// The computer has just woken from sleep (BusPowerDetector::
    /// noteSystemWoke): the drops of its waking are not a power problem.
    void noteSystemWoke (double nowSeconds);

    /// §14.3: called when the device list changes, with whatever controller
    /// topology the OS exposed. An empty list clears the warning.
    void updateControllerTopology (const std::vector<ControllerContentionDetector::DeviceControllerInfo>& devices);

    /// §8.1: per-block channel peaks, used to find channels that have gone
    /// silent while others are live.
    void updateChannelLevels (const std::vector<float>& peaksDb, double blockSeconds);

    /// §14.4: correlation between two channels plus a third channel's level,
    /// which together indicate a microphone set to something other than cardioid.
    void updatePolarPattern (float correlationAB, float thirdChannelPeakDb, double blockSeconds);

    /// Names used in messages, so advice says "Kitchen" rather than "channel 2".
    void setChannelNames (std::vector<std::string> names);

    /// Everything currently worth telling the user, most serious first.
    std::vector<SetupAdvice> getActiveAdvice (double nowSeconds) const;

    void reset();

private:
    BusPowerDetector busPower;
    PolarPatternDetector polarPattern;

    // Built lazily and rebuilt when the channel count changes, because the
    // detector is fixed-width and mics come and go (§2, hot-plug).
    std::unique_ptr<DeadChannelDetector> deadChannels;

    /// Seconds every channel has been at or under the metering floor together.
    double allChannelsSilentSeconds = 0.0;

    /// Cleared the instant anything arrives, and never set again for this rig.
    /// A take that started working and later went quiet is a quiet room, not a
    /// refused permission, and must not be told otherwise.
    bool everHeardAudio = false;

    std::vector<std::string> channelNames;
    std::string contentionReason;
    int numChannels = 0;

    std::string nameFor (int channelIndex) const;
};

} // namespace mma
