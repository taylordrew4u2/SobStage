#include "SetupAdvisor.h"

#include <algorithm>

namespace mma {

void SetupAdvisor::noteDeviceDropout (double nowSeconds, const std::string& deviceId,
                                      const std::string& name, int devicesAttached)
{
    busPower.recordEvent (nowSeconds, deviceId, name, devicesAttached);
}

void SetupAdvisor::noteDeviceHealthy (const std::string& deviceId)
{
    busPower.noteHealthy (deviceId);
}

namespace {

/// "Kitchen and Hall", "Kitchen, Hall and Door".
std::string joinNames (const std::vector<std::string>& names)
{
    std::string joined;

    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i > 0)
            joined += (i + 1 == names.size()) ? " and " : ", ";

        joined += names[i].empty() ? std::string ("A microphone") : names[i];
    }

    return joined;
}

} // namespace

void SetupAdvisor::updateControllerTopology (const std::vector<ControllerContentionDetector::DeviceControllerInfo>& devices)
{
    contentionReason.clear();

    std::string reason;
    if (ControllerContentionDetector::detectContention (devices, reason))
        contentionReason = reason;
}

void SetupAdvisor::updateChannelLevels (const std::vector<float>& peaksDb, double blockSeconds)
{
    const int incoming = static_cast<int> (peaksDb.size());

    // A hot-plug changes the channel count, and the detector is fixed-width.
    // Rebuilding drops the accumulated window, which is correct: the timings
    // measured against a different set of mics no longer describe this one.
    if (deadChannels == nullptr || incoming != numChannels)
    {
        numChannels = incoming;
        deadChannels = std::make_unique<DeadChannelDetector> (incoming);
    }

    if (incoming > 0)
        deadChannels->processBlock (peaksDb, blockSeconds);

    // Every channel at or under the metering floor, together, since the take
    // began. DeadChannelDetector deliberately cannot see this -- it requires
    // another channel to be ALIVE before calling one dead, which is what keeps
    // a quiet room from being reported as a rig full of broken microphones.
    // Correct, and it leaves exactly one case unreported: all of them at once.
    if (incoming > 0)
    {
        const bool anythingArrived =
            std::any_of (peaksDb.begin(), peaksDb.end(),
                         [] (float db) { return db > DeadChannelDetector::kDeadThresholdDb; });

        // The distinction this rests on, stated rather than buried: a connected
        // microphone in a silent room is not at the metering floor. It has a
        // noise floor -- preamp hiss, the room itself -- and reads somewhere
        // above -60. EXACTLY the floor means literal zero samples, which is
        // what a refused permission and a powered-off interface both deliver.
        //
        // That is an assumption about real hardware, and it is the one thing
        // here no fixture can confirm. If it is ever wrong for some device,
        // the symptom is this advice appearing for a rig that works -- which
        // is why it takes forty seconds of NOTHING, and why a single arriving
        // sample retires it for the rest of the session.
        if (anythingArrived)
        {
            everHeardAudio = true;
            allChannelsSilentSeconds = 0.0;
        }
        else if (! everHeardAudio)
        {
            // Only counted while NOTHING has ever arrived. A rig that worked
            // and then went quiet is a pause between takes; this is about a
            // rig that has never once produced a sample.
            allChannelsSilentSeconds += blockSeconds;
        }
    }
}

void SetupAdvisor::updatePolarPattern (float correlationAB, float thirdChannelPeakDb, double blockSeconds)
{
    polarPattern.processBlock (correlationAB, thirdChannelPeakDb, blockSeconds);
}

void SetupAdvisor::setChannelNames (std::vector<std::string> names)
{
    channelNames = std::move (names);
}

std::string SetupAdvisor::nameFor (int channelIndex) const
{
    if (channelIndex >= 0 && channelIndex < static_cast<int> (channelNames.size())
        && ! channelNames[static_cast<size_t> (channelIndex)].empty())
        return channelNames[static_cast<size_t> (channelIndex)];

    return "Mic " + std::to_string (channelIndex + 1);
}

std::vector<SetupAdvice> SetupAdvisor::getActiveAdvice (double nowSeconds) const
{
    std::vector<SetupAdvice> advice;

    // Power first: it causes the dropouts and disappearing devices that
    // everything else here would otherwise be blamed for (§14.2).
    //
    // In the past tense and naming the mics, so a line read five minutes after
    // it was written is still true -- the old present-tense "need more power"
    // read as a fact about now, about mics that may have been fine since.
    if (busPower.isBusPowerExhausted (nowSeconds))
    {
        const auto names = busPower.namesInWindow (nowSeconds);

        advice.push_back ({ SetupIssue::BusPowerExhausted,
                            joinNames (names) + (names.size() > 2 ? " all" : " both")
                                + " lost their connection. They may need more power: use a USB hub "
                                  "with its own power adapter.",
                            -1 });
    }
    else if (const auto repeating = busPower.repeatingDevice (nowSeconds); ! repeating.empty())
    {
        advice.push_back ({ SetupIssue::DeviceKeepsDropping,
                            repeating + " keeps losing its connection. Try another cable or USB port.",
                            -1 });
    }

    if (! contentionReason.empty())
        advice.push_back ({ SetupIssue::ControllerContention,
                            "Your card reader and microphones share one USB connection. "
                            "Use the built-in card slot if you have one.",
                            -1 });

    // §14.4: never say "bleed" -- the word means nothing to this user. Name the
    // knob and the setting instead.
    if (polarPattern.isTriggered())
        advice.push_back ({ SetupIssue::NonCardioidPattern,
                            "A microphone is picking up the whole room. "
                            "Turn its pattern knob to the single-heart setting.",
                            -1 });

    // §10.5: a silent channel is most often the hardware mute switch, which is
    // the single most common failure, so name it rather than being vague.
    //
    // Unless they are ALL silent. §10.1 calls permissions "the first real
    // obstacle" and says a denied microphone "looks exactly like broken
    // hardware to a novice" -- and that is precisely the shape it arrives in,
    // because macOS answers a refused microphone with SILENCE rather than an
    // error. The device still lists, the stream still opens, and every channel
    // reads dead at once.
    //
    // Told per channel, that came out as "Person 1 isn't sending sound. Check
    // the mute button on the mic." repeated for everyone in the room: the app
    // pointing confidently at hardware that is working perfectly, which is
    // worse than saying nothing. Several mute switches do not get pressed
    // simultaneously; one cause upstream of all of them does.
    // Twice the per-channel window, and only when not a single sample has ever
    // arrived. A quiet room eventually speaks; a refused microphone never does.
    constexpr double kNothingAtAllSeconds = DeadChannelDetector::kSustainSeconds * 2.0;

    if (numChannels > 0 && ! everHeardAudio
        && allChannelsSilentSeconds >= kNothingAtAllSeconds)
    {
        advice.push_back ({ SetupIssue::EverythingSilent,
                            "No sound is arriving from any microphone. On a Mac, check that "
                            "SobStage is allowed to use the microphone in System Settings under "
                            "Privacy & Security; otherwise check that the interface they share "
                            "is switched on and connected.",
                            -1 });

        return advice;
    }

    for (int ch = 0; ch < numChannels; ++ch)
        if (deadChannels != nullptr && deadChannels->isChannelDead (ch))
            advice.push_back ({ SetupIssue::SilentChannel,
                                nameFor (ch) + " isn't sending sound. Check the mute button on the mic.",
                                ch });

    return advice;
}

void SetupAdvisor::reset()
{
    allChannelsSilentSeconds = 0.0;
    everHeardAudio = false;
    deadChannels.reset();
    polarPattern.reset();
    busPower.reset();
    contentionReason.clear();
    numChannels = 0;
}

} // namespace mma
