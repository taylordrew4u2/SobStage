#include "TestFramework.h"
#include "Core/HeadphoneRouting.h"
#include <set>

using namespace mma;

TEST_CASE (HeadphoneRouting_EveryJackGetsTheMixByDefault)
{
    const std::vector<CombinedDeviceOutputs> layout { { "yeti-a", 2 }, { "yeti-b", 2 } };
    const auto gains = headphoneChannelGains (layout, nullptr);

    REQUIRE (gains.size() == 4u);
    for (float g : gains)
        REQUIRE (g == 1.0f);
}

TEST_CASE (HeadphoneRouting_SwitchingOnePersonOffSilencesOnlyTheirJack)
{
    const std::vector<CombinedDeviceOutputs> layout { { "yeti-a", 2 }, { "yeti-b", 2 }, { "yeti-c", 2 } };
    const auto gains = headphoneChannelGains (layout, [] (const std::string& id) { return id != "yeti-b"; });

    REQUIRE (gains.size() == 6u);
    REQUIRE (gains[0] == 1.0f);
    REQUIRE (gains[1] == 1.0f);
    REQUIRE (gains[2] == 0.0f);
    REQUIRE (gains[3] == 0.0f);
    REQUIRE (gains[4] == 1.0f);
    REQUIRE (gains[5] == 1.0f);
}

TEST_CASE (HeadphoneRouting_AMicWithNoJackTakesNoChannels)
{
    // A lavalier with no output sits in the combined device but brings no
    // output channels; the next mic's jack must not shift onto its slot.
    const std::vector<CombinedDeviceOutputs> layout { { "lav", 0 }, { "yeti", 2 } };
    const auto gains = headphoneChannelGains (layout, [] (const std::string& id) { return id != "lav"; });

    REQUIRE (gains.size() == 2u);
    REQUIRE (gains[0] == 1.0f);
    REQUIRE (gains[1] == 1.0f);
    REQUIRE (combinedDeviceHasHeadphones (layout));
    REQUIRE_FALSE (combinedDeviceHasHeadphones ({ { "lav", 0 } }));
}

TEST_CASE (HeadphoneRouting_LayoutIsCompleteOnlyWhenEverySubDeviceIsActive)
{
    REQUIRE (combinedLayoutIsComplete ({ { "yeti-b", 2 }, { "yeti-a", 2 } }, { "yeti-a", "yeti-b" }));
    REQUIRE (combinedLayoutIsComplete ({ { "lav", 0 } }, { "lav" }));
    REQUIRE_FALSE (combinedLayoutIsComplete ({}, { "yeti-a" }));
    REQUIRE_FALSE (combinedLayoutIsComplete ({ { "yeti-a", 2 } }, { "yeti-a", "yeti-b" }));
}

TEST_CASE (HeadphoneRouting_ANewCombinedDeviceIsWaitedForUntilItsJacksAppear)
{
    // Empty for the first few reads after creation, as the HAL builds it.
    int reads = 0, pauses = 0;
    const auto read = [&reads]
    {
        return ++reads <= 3 ? std::vector<CombinedDeviceOutputs> {}
                            : std::vector<CombinedDeviceOutputs> { { "yeti-a", 2 }, { "yeti-b", 2 } };
    };

    REQUIRE (waitForCombinedLayout (read, { "yeti-a", "yeti-b" }, 50, [&pauses] { ++pauses; }));
    REQUIRE (reads == 4);
    REQUIRE (pauses == 3);
}

TEST_CASE (HeadphoneRouting_ASubDeviceThatNeverActivatesCostsOnlyTheBound)
{
    int reads = 0, pauses = 0;
    const auto read = [&reads]
    {
        ++reads;
        return std::vector<CombinedDeviceOutputs> { { "yeti-a", 2 } };
    };

    REQUIRE_FALSE (waitForCombinedLayout (read, { "yeti-a", "gone" }, 5, [&pauses] { ++pauses; }));
    REQUIRE (pauses == 5);
    REQUIRE (reads == 6);

    // Already there: no pause at all.
    pauses = 0;
    REQUIRE (waitForCombinedLayout (read, { "yeti-a" }, 5, [&pauses] { ++pauses; }));
    REQUIRE (pauses == 0);
}

namespace
{
    /// Everything HeadphoneJacks hands on, in order.
    struct LaidGains
    {
        std::string deviceId;
        std::vector<float> gains;
    };

    struct Recorder
    {
        std::vector<LaidGains> laid;

        HeadphoneJacks::LayGains sink()
        {
            return [this] (const std::string& id, const std::vector<float>& gains) { laid.push_back ({ id, gains }); };
        }

        const LaidGains& last() const
        {
            REQUIRE_FALSE (laid.empty());
            return laid.back();
        }
    };
}

TEST_CASE (HeadphoneRouting_GainsFollowAJackThatMovesWhenAMicLeavesTheCombinedDevice)
{
    // A, B and C each have a jack; B has switched theirs off. Mid-take A's
    // cable is bumped and the combined device drops it, so B's jack moves to
    // channels 0-1 and C's to 2-3. Gains worked out for the old layout would
    // now silence C and play the mix to B: reading the layout lays them again,
    // with nothing else asked of the caller.
    Recorder recorder;
    HeadphoneJacks jacks ("combined", [] (const std::string& id) { return id != "yeti-b"; }, recorder.sink());

    jacks.layoutRead ({ { "yeti-a", 2 }, { "yeti-b", 2 }, { "yeti-c", 2 } });
    REQUIRE (recorder.laid.size() == 1u);
    REQUIRE (recorder.last().deviceId == "combined");
    REQUIRE (recorder.last().gains == std::vector<float> ({ 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f }));

    jacks.layoutRead ({ { "yeti-b", 2 }, { "yeti-c", 2 } });
    REQUIRE (recorder.laid.size() == 2u);
    REQUIRE (recorder.last().deviceId == "combined");
    REQUIRE (recorder.last().gains == std::vector<float> ({ 0.0f, 0.0f, 1.0f, 1.0f }));
}

TEST_CASE (HeadphoneRouting_GainsFollowAJackThatJoinsTheCombinedDeviceLate)
{
    // A is switched off; B's microphone is slow and joins after the device
    // was made, in front of C. C's jack moves from channels 2-3 to 4-5, and B
    // must not inherit the gains A's neighbour had. The rig is unchanged, so
    // nothing reopens: the late read is the only thing that lays them.
    Recorder recorder;
    HeadphoneJacks jacks ("combined", [] (const std::string& id) { return id != "yeti-a"; }, recorder.sink());

    jacks.layoutRead ({ { "yeti-a", 2 }, { "yeti-c", 2 } });
    REQUIRE (recorder.last().gains == std::vector<float> ({ 0.0f, 0.0f, 1.0f, 1.0f }));

    jacks.layoutRead ({ { "yeti-a", 2 }, { "yeti-b", 2 }, { "yeti-c", 2 } });
    REQUIRE (recorder.laid.size() == 2u);
    REQUIRE (recorder.last().gains == std::vector<float> ({ 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f }));
    REQUIRE (jacks.getLayout().size() == 3u);
}

TEST_CASE (HeadphoneRouting_EveryReadAndEverySwitchLaysTheGainsForTheCombinedDeviceOnly)
{
    // Who hears the mix changes outside the layout; switching someone off
    // lays the gains from the layout as last read. However the output choice
    // moves meanwhile (mid-take it can, while the stream stays put until
    // Stop), the gains are always handed on as the combined device's own:
    // which stream they reach is the coordinator's to say.
    std::set<std::string> off;
    Recorder recorder;
    HeadphoneJacks jacks ("combined", [&off] (const std::string& id) { return off.count (id) == 0; },
                          recorder.sink());

    jacks.layoutRead ({ { "yeti-a", 2 }, { "yeti-b", 2 } });
    REQUIRE (recorder.last().gains == std::vector<float> ({ 1.0f, 1.0f, 1.0f, 1.0f }));

    // An unchanged layout read again still lays them: cheap, and nothing
    // has to know whether it changed.
    jacks.layoutRead ({ { "yeti-a", 2 }, { "yeti-b", 2 } });
    REQUIRE (recorder.laid.size() == 2u);

    off.insert ("yeti-b");
    jacks.lay();
    REQUIRE (recorder.laid.size() == 3u);
    REQUIRE (recorder.last().gains == std::vector<float> ({ 1.0f, 1.0f, 0.0f, 0.0f }));

    // The combined device gone (or not published): the mix everywhere.
    jacks.layoutRead ({});
    REQUIRE (recorder.last().gains.empty());
    REQUIRE (jacks.getLayout().empty());

    for (const auto& laid : recorder.laid)
        REQUIRE (laid.deviceId == "combined");
}

TEST_CASE (HeadphoneRouting_TheCombinedDeviceIsRemadeOnlyWhenOurStreamIsNotOnIt)
{
    // Nothing of ours plays through it (the stream is on another output, or
    // there is none): re-made at once, mid-take or not. Our stream on it
    // between takes: closed first, then reopened onto the new one. The
    // take's mix on it: not until Stop.
    REQUIRE (combinedDeviceRemakeStep (false, false) == CombinedDeviceRemake::Now);
    REQUIRE (combinedDeviceRemakeStep (false, true) == CombinedDeviceRemake::Now);
    REQUIRE (combinedDeviceRemakeStep (true, false) == CombinedDeviceRemake::CloseOurStreamFirst);
    REQUIRE (combinedDeviceRemakeStep (true, true) == CombinedDeviceRemake::AfterTheTake);
}
