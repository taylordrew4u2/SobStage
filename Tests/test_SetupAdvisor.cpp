#include "TestFramework.h"
#include "Core/SetupAdvisor.h"

using namespace mma;

namespace {

bool hasIssue (const std::vector<SetupAdvice>& advice, SetupIssue issue)
{
    for (const auto& a : advice)
        if (a.issue == issue)
            return true;
    return false;
}

const SetupAdvice* find (const std::vector<SetupAdvice>& advice, SetupIssue issue)
{
    for (const auto& a : advice)
        if (a.issue == issue)
            return &a;
    return nullptr;
}

} // namespace

TEST_CASE (SetupAdvisor_QuietWhenNothingIsWrong)
{
    SetupAdvisor advisor;
    REQUIRE (advisor.getActiveAdvice (0.0).empty());
}

TEST_CASE (SetupAdvisor_ReportsBusPowerExhaustion)
{
    SetupAdvisor advisor;
    // §14.2: two or more drops inside five minutes with three or more mics.
    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    advisor.noteDeviceDropout (20.0, "dev-hall", "Hall", 4);

    REQUIRE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::BusPowerExhausted));
}

TEST_CASE (SetupAdvisor_BusPowerMessageNamesThePoweredHub)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    advisor.noteDeviceDropout (20.0, "dev-hall", "Hall", 4);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::BusPowerExhausted);
    REQUIRE (a != nullptr);
    // §14.2 insists on "its own power adapter" wording: a hub that only plugs
    // into the computer adds ports, not power, and the two look identical.
    REQUIRE (a->message.find ("own power adapter") != std::string::npos);
}

TEST_CASE (SetupAdvisor_SingleDropoutIsNotEnough)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (20.0), SetupIssue::BusPowerExhausted));
}

TEST_CASE (SetupAdvisor_ReportsControllerContention)
{
    SetupAdvisor advisor;

    std::vector<ControllerContentionDetector::DeviceControllerInfo> devices;
    devices.push_back ({ "reader", "usb-controller-0", true, false });
    devices.push_back ({ "yeti", "usb-controller-0", false, true });

    advisor.updateControllerTopology (devices);
    REQUIRE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));
}

TEST_CASE (SetupAdvisor_NoContentionOnSeparateControllers)
{
    SetupAdvisor advisor;

    std::vector<ControllerContentionDetector::DeviceControllerInfo> devices;
    devices.push_back ({ "reader", "usb-controller-0", true, false });
    devices.push_back ({ "yeti", "usb-controller-1", false, true });

    advisor.updateControllerTopology (devices);
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));
}

TEST_CASE (SetupAdvisor_ContentionClearsWhenTopologyChanges)
{
    SetupAdvisor advisor;

    std::vector<ControllerContentionDetector::DeviceControllerInfo> shared;
    shared.push_back ({ "reader", "c0", true, false });
    shared.push_back ({ "yeti", "c0", false, true });
    advisor.updateControllerTopology (shared);
    REQUIRE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));

    // Reader moved to the built-in slot: the warning must go away rather than
    // stick around telling the user to fix something they already fixed.
    std::vector<ControllerContentionDetector::DeviceControllerInfo> separate;
    separate.push_back ({ "yeti", "c0", false, true });
    advisor.updateControllerTopology (separate);
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));
}

TEST_CASE (SetupAdvisor_SilentChannelNamesTheMuteButton)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    // Kitchen silent while Couch is live, held long enough to qualify (§8.1).
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    REQUIRE (a->channelIndex == 0);
    // §10.5: the hardware mute switch is the single most common failure, so it
    // is named rather than hinted at.
    REQUIRE (a->message.find ("mute button") != std::string::npos);
}

TEST_CASE (SetupAdvisor_SilentChannelUsesTheAssignedName)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    // §6.2/§14.6: a novice cannot act on "channel 1".
    REQUIRE (a->message.find ("Kitchen") != std::string::npos);
}

TEST_CASE (SetupAdvisor_FallsBackToMicNumberWithoutAName)
{
    SetupAdvisor advisor;

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    REQUIRE (a->message.find ("Mic 1") != std::string::npos);
}

TEST_CASE (SetupAdvisor_SilenceEverywhereIsNotADeadChannel)
{
    SetupAdvisor advisor;

    // Nobody is talking. That is not a fault, and warning about it would train
    // the user to ignore the warning.
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -80.0f }, 1.0);

    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::SilentChannel));
}

TEST_CASE (SetupAdvisor_PolarPatternMessageAvoidsTheWordBleed)
{
    SetupAdvisor advisor;

    // §14.4: correlation above 0.6 sustained for 10 s with a third channel quiet.
    for (int i = 0; i < 12; ++i)
        advisor.updatePolarPattern (0.9f, -60.0f, 1.0);

    const auto advice = advisor.getActiveAdvice (0.0);
    const auto* a = find (advice, SetupIssue::NonCardioidPattern);
    REQUIRE (a != nullptr);
    REQUIRE (a->message.find ("bleed") == std::string::npos);
    REQUIRE (a->message.find ("single-heart") != std::string::npos);
}

TEST_CASE (SetupAdvisor_PowerIsReportedBeforeOtherIssues)
{
    SetupAdvisor advisor;

    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    advisor.noteDeviceDropout (20.0, "dev-hall", "Hall", 4);
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    REQUIRE (advice.size() >= 2);
    // Power causes the dropouts everything else gets blamed for, so it leads.
    REQUIRE (advice.front().issue == SetupIssue::BusPowerExhausted);
}

TEST_CASE (SetupAdvisor_ResetClearsAdvice)
{
    SetupAdvisor advisor;

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);
    REQUIRE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::SilentChannel));

    advisor.reset();
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::SilentChannel));
}

// §10.1 calls permissions "the first real obstacle", and says a denied
// microphone "looks exactly like broken hardware to a novice". That is not a
// metaphor: macOS answers a refused microphone with SILENCE, not an error, so
// the device still lists, the stream still opens, and every channel reads dead.
//
// Told per channel, that came out as "Kitchen isn't sending sound. Check the
// mute button on the mic." once per person in the room -- the app pointing
// confidently at hardware that is working perfectly. Several mute switches do
// not get pressed at the same instant; one cause upstream of all of them does.
TEST_CASE (SetupAdvisor_EveryChannelSilentIsNotEveryoneHittingMute)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    // -60 is what real metering emits for digital silence: it FLOORS there.
    // Feeding -80 would be testing a value the app cannot produce, which is
    // how this detector's own tests once passed while it could not fire.
    for (int i = 0; i < 45; ++i)
        advisor.updateChannelLevels ({ -60.0f, -60.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (50.0);

    REQUIRE (find (advice, SetupIssue::EverythingSilent) != nullptr);

    // The misleading advice is GONE, not merely accompanied. Left in beside the
    // real cause it would still be read, and still send someone hunting over a
    // mute button that was never pressed.
    REQUIRE (find (advice, SetupIssue::SilentChannel) == nullptr);
}

TEST_CASE (SetupAdvisor_EverythingSilentNamesPermissionAndTheSharedInterface)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    for (int i = 0; i < 45; ++i)
        advisor.updateChannelLevels ({ -60.0f, -60.0f }, 1.0);

    // The vector is held in a named local, deliberately. getActiveAdvice
    // returns BY VALUE, so find()ing into the temporary hands back a pointer
    // into storage that dies at the end of the statement -- which is a
    // use-after-free, and is exactly how this test first arrived: a segfault
    // rather than a failure. Every other case in this file holds it too.
    const auto advice = advisor.getActiveAdvice (50.0);
    const auto* a = find (advice, SetupIssue::EverythingSilent);
    REQUIRE (a != nullptr);

    // §10.6: what is wrong, then what to do. The two causes that actually
    // produce this, in the order they are worth checking.
    REQUIRE (a->message.find ("microphone") != std::string::npos);
    REQUIRE (a->message.find ("Privacy") != std::string::npos);
    REQUIRE (a->message.find ("interface") != std::string::npos);

    // Whole-rig, so it is not attached to one person's strip.
    REQUIRE (a->channelIndex == -1);
}

// The narrow rule must not eat the common one. One mute switch among several
// really is a mute switch, and that advice is the single most useful sentence
// this advisor produces (§10.5).
TEST_CASE (SetupAdvisor_OneSilentChannelAmongSeveralStillNamesTheMuteButton)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch", "Porch" });

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);

    REQUIRE (find (advice, SetupIssue::EverythingSilent) == nullptr);

    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    REQUIRE (a->message.find ("mute") != std::string::npos);
}

// One microphone is still "every microphone" when it is the only one, and a
// lone mic that has never produced a sample has the same causes: the app was
// refused access, or the thing it is plugged into is off. DeadChannelDetector
// cannot speak here either -- with one channel there is never another to be
// alive -- so without this rule a single-mic rig got nothing at all.
TEST_CASE (SetupAdvisor_ALoneMicrophoneThatNeverArrivesIsReported)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen" });

    for (int i = 0; i < 45; ++i)
        advisor.updateChannelLevels ({ -60.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (50.0);
    REQUIRE (find (advice, SetupIssue::EverythingSilent) != nullptr);
}

// The false positive that matters: a room where nobody happens to be talking.
// A rig that has produced audio ONCE is working, and a later quiet stretch --
// however long -- must never be reported as a permission problem.
TEST_CASE (SetupAdvisor_AQuietRoomAfterRealAudioIsNeverCalledSilent)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    // Somebody speaks, briefly.
    for (int i = 0; i < 2; ++i)
        advisor.updateChannelLevels ({ -20.0f, -20.0f }, 1.0);

    // Then a very long silence -- far past the window.
    for (int i = 0; i < 90; ++i)
        advisor.updateChannelLevels ({ -60.0f, -60.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (100.0);
    REQUIRE (find (advice, SetupIssue::EverythingSilent) == nullptr);
}

// The assumption the whole rule rests on, written down as a test: a CONNECTED
// microphone in a silent room still has a noise floor. Preamp hiss and the room
// itself put it above the metering floor, and only literal zero samples -- a
// refused permission, an interface with no power -- sit exactly on it.
//
// An existing case in this file says warning about a quiet room "would train
// the user to ignore the warning", and it is right. This is how that stays
// true: quiet is not silent.
TEST_CASE (SetupAdvisor_AConnectedMicInASilentRoomIsNotReported)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    // -55 dBFS: nobody speaking, but the microphones are plainly there.
    for (int i = 0; i < 120; ++i)
        advisor.updateChannelLevels ({ -55.0f, -55.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (130.0);
    REQUIRE (find (advice, SetupIssue::EverythingSilent) == nullptr);
}

// And it must not fire early, or a rig still being plugged in mid-setup would
// be accused of a permission problem before anyone had spoken.
TEST_CASE (SetupAdvisor_SilenceIsGivenTimeBeforeItIsCalledAProblem)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    for (int i = 0; i < 20; ++i)
        advisor.updateChannelLevels ({ -60.0f, -60.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (25.0);
    REQUIRE (find (advice, SetupIssue::EverythingSilent) == nullptr);
}

TEST_CASE (SetupAdvisor_ResetForgetsBusPowerEventsToo)
{
    // reset() cleared the polar, dead-channel and contention state and left
    // busPower alone, so events from a rig that had since been unplugged could
    // still produce "use a powered hub" for five minutes against hardware that
    // does not need one.
    SetupAdvisor a;

    a.noteDeviceDropout (1.0, "dev-kitchen", "Kitchen", 4);
    a.noteDeviceDropout (2.0, "dev-hall", "Hall", 4);

    bool sawPowerAdvice = false;
    for (const auto& advice : a.getActiveAdvice (3.0))
        if (advice.issue == SetupIssue::BusPowerExhausted)
            sawPowerAdvice = true;
    REQUIRE (sawPowerAdvice);

    a.reset();

    for (const auto& advice : a.getActiveAdvice (4.0))
        REQUIRE (advice.issue != SetupIssue::BusPowerExhausted);
}

// §14.2 in the past tense, naming the mics: a line read five minutes after it
// was written is still true, and someone can tell which mics to move. The
// powered-hub wording is kept exactly: "a hub with its own power adapter" is
// the distinction §14.2 insists on.
TEST_CASE (SetupAdvisor_PowerAdviceNamesBothMics)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    advisor.noteDeviceDropout (20.0, "dev-hall", "Hall", 4);

    const auto two = advisor.getActiveAdvice (30.0);
    const auto* a = find (two, SetupIssue::BusPowerExhausted);
    REQUIRE (a != nullptr);
    REQUIRE (a->message == "Kitchen and Hall both lost their connection. They may need more power: "
                           "use a USB hub with its own power adapter.");
    REQUIRE (a->message.find ("USB hub with its own power adapter") != std::string::npos);

    advisor.noteDeviceDropout (25.0, "dev-door", "Door", 4);
    const auto three = advisor.getActiveAdvice (30.0);
    const auto* b = find (three, SetupIssue::BusPowerExhausted);
    REQUIRE (b != nullptr);
    REQUIRE (b->message.rfind ("Kitchen, Hall and Door all lost their connection.", 0) == 0);
    REQUIRE (b->message.find ("USB hub with its own power adapter") != std::string::npos);

    // Not both at once: power is the cause, so the cable advice is not added.
    REQUIRE (find (three, SetupIssue::DeviceKeepsDropping) == nullptr);
}

// One mic dropping again and again is its cable or port. More power would not
// help, and "need more power" sent people to buy a hub they did not need.
TEST_CASE (SetupAdvisor_KeepsDroppingNamesTheMicAndCable)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    advisor.noteDeviceHealthy ("dev-kitchen");
    advisor.noteDeviceDropout (20.0, "dev-kitchen", "Kitchen", 4);

    const auto advice = advisor.getActiveAdvice (30.0);
    REQUIRE_FALSE (hasIssue (advice, SetupIssue::BusPowerExhausted));

    const auto* a = find (advice, SetupIssue::DeviceKeepsDropping);
    REQUIRE (a != nullptr);
    REQUIRE (a->message == "Kitchen keeps losing its connection. Try another cable or USB port.");
    REQUIRE (a->message.find ("power") == std::string::npos);

    // Ranked right after power: ahead of guidance about patterns and mutes.
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);
    const auto withSilence = advisor.getActiveAdvice (30.0);
    REQUIRE (withSilence.size() >= 2);
    REQUIRE (withSilence.front().issue == SetupIssue::DeviceKeepsDropping);
}

// A failure that is still the same failure is not news. A rebuild that asks
// the dead mic again, or Stop reopening a mic that died mid-take, must not
// turn one bad cable into the second of §14.2's two events.
TEST_CASE (SetupAdvisor_StillFailingMicIsNotCountedTwice)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, "dev-kitchen", "Kitchen", 4);
    advisor.noteDeviceDropout (20.0, "dev-kitchen", "Kitchen", 4);

    const auto advice = advisor.getActiveAdvice (30.0);
    REQUIRE_FALSE (hasIssue (advice, SetupIssue::BusPowerExhausted));
    REQUIRE_FALSE (hasIssue (advice, SetupIssue::DeviceKeepsDropping));
}
