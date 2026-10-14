#include "TestFramework.h"
#include "Core/BusPowerDetector.h"
#include "Platform/IAudioBackend.h"

using namespace mma;

// §14.2's detector, on its own. Every case names the false alarm it is there
// to keep away: "your microphones need more power" for a rig whose cause was
// something else.

// Mutation: the distinct-device rule removed (counting events again).
TEST_CASE (BusPowerDetector_SameDeviceTwiceIsNotPower)
{
    BusPowerDetector d;
    d.recordEvent (10.0, "a", "Kitchen", 4);
    d.noteHealthy ("a");
    d.recordEvent (20.0, "a", "Kitchen", 4);

    REQUIRE_FALSE (d.isBusPowerExhausted (30.0));
}

// Mutation: the episode rule removed. A mic that never opened was counted again
// each time another mic was plugged in, and a mic that died mid-take counted
// again when Stop failed to reopen it.
TEST_CASE (BusPowerDetector_StillFailingDeviceCountsOnce)
{
    BusPowerDetector d;
    REQUIRE (d.recordEvent (10.0, "a", "Kitchen", 4));
    REQUIRE_FALSE (d.recordEvent (20.0, "a", "Kitchen", 4));
    REQUIRE_FALSE (d.recordEvent (25.0, "a", "Kitchen", 4));

    // Counted once, so it is not even "keeps dropping".
    REQUIRE (d.repeatingDevice (30.0).empty());
    REQUIRE (d.namesInWindow (30.0).size() == 1);
}

// Mutation: noteHealthy ignored. A mic that came back and failed again is a
// second failure, and one that keeps doing it must still be reported.
TEST_CASE (BusPowerDetector_HealthyThenFailingAgainCountsAgain)
{
    BusPowerDetector d;
    REQUIRE (d.recordEvent (10.0, "a", "Kitchen", 4));
    d.noteHealthy ("a");
    REQUIRE (d.recordEvent (20.0, "a", "Kitchen", 4));

    REQUIRE (d.repeatingDevice (30.0) == "Kitchen");
}

TEST_CASE (BusPowerDetector_TwoDifferentDevicesIsPower)
{
    BusPowerDetector d;
    d.recordEvent (10.0, "a", "Kitchen", 4);
    d.recordEvent (20.0, "b", "Hall", 4);

    REQUIRE (d.isBusPowerExhausted (30.0));

    const auto names = d.namesInWindow (30.0);
    REQUIRE (names.size() == 2);
    REQUIRE (names[0] == "Kitchen");
    REQUIRE (names[1] == "Hall");

    // Two devices is power, not one repeating device.
    REQUIRE (d.repeatingDevice (30.0).empty());
}

// Mutation: the 3+ attached boundary moved.
TEST_CASE (BusPowerDetector_FewerThanThreeAttachedIsNot)
{
    BusPowerDetector two;
    two.recordEvent (10.0, "a", "Kitchen", 2);
    two.recordEvent (20.0, "b", "Hall", 2);
    REQUIRE_FALSE (two.isBusPowerExhausted (30.0));

    BusPowerDetector three;
    three.recordEvent (10.0, "a", "Kitchen", 2);
    three.recordEvent (20.0, "b", "Hall", 3);
    REQUIRE (three.isBusPowerExhausted (30.0));
}

// Mutation: the window off by one, either way.
TEST_CASE (BusPowerDetector_WindowBoundary)
{
    BusPowerDetector d;
    d.recordEvent (0.0, "a", "Kitchen", 4);
    d.recordEvent (10.0, "b", "Hall", 4);

    // Exactly five minutes after the first event it still counts.
    REQUIRE (d.isBusPowerExhausted (BusPowerDetector::kWindowSeconds));
    // A moment later it has left the window.
    REQUIRE_FALSE (d.isBusPowerExhausted (BusPowerDetector::kWindowSeconds + 0.001));
    REQUIRE (d.namesInWindow (BusPowerDetector::kWindowSeconds + 0.001).size() == 1);
}

TEST_CASE (BusPowerDetector_RepeatingDeviceIsReported)
{
    BusPowerDetector d;
    d.recordEvent (10.0, "a", "Kitchen", 4);
    REQUIRE (d.repeatingDevice (15.0).empty());   // once is not "keeps"

    d.noteHealthy ("a");
    d.recordEvent (20.0, "a", "Kitchen", 4);
    REQUIRE (d.repeatingDevice (30.0) == "Kitchen");

    // Another device failing too makes it a power question instead.
    d.recordEvent (25.0, "b", "Hall", 4);
    REQUIRE (d.repeatingDevice (30.0).empty());
    REQUIRE (d.isBusPowerExhausted (30.0));

    d.reset();
    REQUIRE_FALSE (d.isBusPowerExhausted (30.0));
    REQUIRE (d.repeatingDevice (30.0).empty());

    // reset() also ends every episode: the first failure after it counts.
    REQUIRE (d.recordEvent (40.0, "a", "Kitchen", 4));
}

// Mutation: any kind's answer flipped. Unknown keeps counting so a real
// brownout a backend cannot classify is not hidden.
TEST_CASE (BusPowerDetector_CountsAsPossiblePowerLoss_EachKind)
{
    REQUIRE (countsAsPossiblePowerLoss (OpenRefusal::unknown));
    REQUIRE (countsAsPossiblePowerLoss (OpenRefusal::gone));
    REQUIRE (countsAsPossiblePowerLoss (OpenRefusal::timedOut));
    REQUIRE_FALSE (countsAsPossiblePowerLoss (OpenRefusal::inUse));
    REQUIRE_FALSE (countsAsPossiblePowerLoss (OpenRefusal::settingsRefused));
    REQUIRE_FALSE (countsAsPossiblePowerLoss (OpenRefusal::permission));
    REQUIRE_FALSE (countsAsPossiblePowerLoss (OpenRefusal::appBusy));
    REQUIRE_FALSE (countsAsPossiblePowerLoss (OpenRefusal::notExternal));
}
