#include "TestFramework.h"
#include "Core/CardRemovalNotice.h"

using namespace mma;

TEST_CASE (CardRemovalNotice_ARunningMirrorIsOfferedByPath)
{
    const auto notice = CardRemovalNotice::build ("/Users/sam/RECORDINGS-MIRROR/2026-08-31_0900_Rehearsal", true);

    // §6.5: "state that a complete copy survives locally and give its path."
    // The path is the difference between an accident and a disaster.
    REQUIRE (notice.aCompleteCopySurvives);
    REQUIRE (notice.message.find ("/Users/sam/RECORDINGS-MIRROR/2026-08-31_0900_Rehearsal") != std::string::npos);
    REQUIRE (notice.survivingFolder == std::string ("/Users/sam/RECORDINGS-MIRROR/2026-08-31_0900_Rehearsal"));

    // And it says the recording stopped, because it did.
    REQUIRE (notice.message.find ("stopped") != std::string::npos);
}

TEST_CASE (CardRemovalNotice_AMirrorThatHadAlreadyStoppedIsNotOffered)
{
    // §6.3 stops the mirror when internal space runs low, and never restarts it
    // within a take. What is left is a copy with a hole in it, which must never
    // be presented as complete -- the user would trust it.
    const auto notice = CardRemovalNotice::build ("/home/sam/RECORDINGS-MIRROR/take", false);

    REQUIRE_FALSE (notice.aCompleteCopySurvives);
    REQUIRE (notice.survivingFolder.empty());
    REQUIRE (notice.message.find ("/home/sam/RECORDINGS-MIRROR/take") == std::string::npos);
    REQUIRE (notice.message.find ("backup copy is safe") == std::string::npos);
}

TEST_CASE (CardRemovalNotice_NoMirrorAtAllSaysSoWithoutPromisingOne)
{
    const auto notice = CardRemovalNotice::build ("", true);

    REQUIRE_FALSE (notice.aCompleteCopySurvives);
    REQUIRE (notice.message.find ("backup copy is safe") == std::string::npos);

    // §10.6: still says what happened and what to do next.
    REQUIRE (notice.message.find ("stopped responding") != std::string::npos);
    REQUIRE (notice.message.find ("plugged in") != std::string::npos);
}

TEST_CASE (CardRemovalNotice_EveryMessageNamesTheStopAndAvoidsCodes)
{
    for (const auto& notice : { CardRemovalNotice::build ("/m/take", true),
                                CardRemovalNotice::build ("", false) })
    {
        // §10.6: no codes, no apologies, no vagueness.
        REQUIRE (notice.message.find ("rror 0x") == std::string::npos);
        REQUIRE (notice.message.find ("Sorry") == std::string::npos);
        REQUIRE (notice.message.find ("every file closed") != std::string::npos);
    }
}

// A take recorded on this computer for a card: its drive is this computer's.
// The card notice told the user to check the card was plugged in and to keep
// the backup copy switched on -- a card never written, a backup there is none
// of. Mutation: the card's notice used for it.
TEST_CASE (CardRemovalNotice_ATakeRecordedHereNamesThisComputersDrive)
{
    const auto notice = CardRemovalNotice::buildForTakeRecordedHere ("/Users/sam/RECORDINGS-MIRROR/2026-10-14_2000_Show");

    REQUIRE_FALSE (notice.aCompleteCopySurvives);
    REQUIRE (notice.survivingFolder.empty());
    REQUIRE (notice.message.find ("This computer's drive stopped accepting writes") == 0);
    REQUIRE (notice.message.find ("/Users/sam/RECORDINGS-MIRROR/2026-10-14_2000_Show") != std::string::npos);
    REQUIRE (notice.message.find ("backup") == std::string::npos);
    REQUIRE (notice.message.find ("plugged in") == std::string::npos);
    REQUIRE (notice.message.find ("card") == std::string::npos);
}
