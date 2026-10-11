#include "TestFramework.h"
#include "Core/HeadphoneRouting.h"

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

TEST_CASE (HeadphoneRouting_GainsFollowAJackThatMovesWhenAMicLeavesTheCombinedDevice)
{
    // A, B and C each have a jack; B has switched theirs off. Mid-take A's
    // cable is bumped and the combined device drops it, so B's jack moves to
    // channels 0-1 and C's to 2-3. Gains worked out for the old layout would
    // now silence C and play the mix to B -- they have to be worked out again
    // from the layout as it is now.
    const auto bIsOff = [] (const std::string& id) { return id != "yeti-b"; };

    const auto before = headphoneChannelGainsForOutput ("combined", "combined",
                                                        { { "yeti-a", 2 }, { "yeti-b", 2 }, { "yeti-c", 2 } },
                                                        bIsOff);
    REQUIRE (before == std::vector<float> ({ 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f }));

    const auto after = headphoneChannelGainsForOutput ("combined", "combined",
                                                       { { "yeti-b", 2 }, { "yeti-c", 2 } }, bIsOff);
    REQUIRE (after == std::vector<float> ({ 0.0f, 0.0f, 1.0f, 1.0f }));
}

TEST_CASE (HeadphoneRouting_GainsFollowAJackThatJoinsTheCombinedDeviceLate)
{
    // A is switched off; B's microphone is slow and joins after the device
    // was made, in front of C. C's jack moves from channels 2-3 to 4-5, and B
    // must not inherit the gains A's neighbour had.
    const auto aIsOff = [] (const std::string& id) { return id != "yeti-a"; };

    const auto early = headphoneChannelGainsForOutput ("combined", "combined",
                                                       { { "yeti-a", 2 }, { "yeti-c", 2 } }, aIsOff);
    REQUIRE (early == std::vector<float> ({ 0.0f, 0.0f, 1.0f, 1.0f }));

    const auto whole = headphoneChannelGainsForOutput ("combined", "combined",
                                                       { { "yeti-a", 2 }, { "yeti-b", 2 }, { "yeti-c", 2 } },
                                                       aIsOff);
    REQUIRE (whole == std::vector<float> ({ 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f }));
}

TEST_CASE (HeadphoneRouting_OnlyTheCombinedDeviceStillOpenGetsPerJackGains)
{
    // The gains describe the stream that is open, not the output chosen:
    // mid-take the choice can move while the reopen waits for Stop.
    const std::vector<CombinedDeviceOutputs> layout { { "yeti-a", 2 }, { "yeti-b", 2 } };
    const auto bIsOff = [] (const std::string& id) { return id != "yeti-b"; };

    // Still playing through the combined device: B stays silent.
    REQUIRE (headphoneChannelGainsForOutput ("combined", "combined", layout, bIsOff)
             == std::vector<float> ({ 1.0f, 1.0f, 0.0f, 0.0f }));

    // A six-channel interface still open while the combined device waits to
    // take over: its channels 2-3 are not B's jack, and get the mix.
    REQUIRE (headphoneChannelGainsForOutput ("scarlett", "combined", layout, bIsOff).empty());

    // No output open at all.
    REQUIRE (headphoneChannelGainsForOutput ("", "combined", layout, bIsOff).empty());
    REQUIRE (headphoneChannelGainsForOutput ("", "", layout, bIsOff).empty());
}
