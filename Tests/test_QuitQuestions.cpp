#include "TestFramework.h"
#include "Core/QuitQuestions.h"

using namespace mma;

using Step = QuitQuestions::Step;
using Next = QuitQuestions::Next;
using Tick = QuitQuestions::Tick;
using WorkAnswer = QuitQuestions::WorkAnswer;

TEST_CASE (QuitQuestions_NothingToAskQuitsAtOnce)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (false) == Step::Proceed);
    REQUIRE (q.onTakeSettled (false, false) == Step::Quit);
    REQUIRE_FALSE (q.isAsking());
}

TEST_CASE (QuitQuestions_MidTakeAsksAndStopAndQuitCarriesOn)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.isAsking());

    REQUIRE (q.onStopTheTakeAnswered (true) == Next::CarryOn);

    // The take is still running until the quit stops it.
    REQUIRE (q.onQuitRequested (true) == Step::Proceed);
    REQUIRE (q.onTakeSettled (false, false) == Step::Quit);
}

TEST_CASE (QuitQuestions_KeepRecordingAsksAgainNextTime)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onStopTheTakeAnswered (false) == Next::Stay);
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
}

TEST_CASE (QuitQuestions_ARepeatWhileAQuestionIsOpenBringsItForward)
{
    // A Dock Quit while the app is hidden or behind another app showed its
    // question where nobody could see it, and a second Quit did nothing at
    // all. Each repeat now asks for the open question to be shown -- and
    // never stacks a second question or counts as an answer.
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onQuitRequested (true) == Step::BringQuestionForward);
    REQUIRE (q.onQuitRequested (false) == Step::BringQuestionForward);
    REQUIRE (q.onStopTheTakeAnswered (true) == Next::CarryOn);

    REQUIRE (q.onQuitRequested (true) == Step::Proceed);
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutProblem);
    REQUIRE (q.onQuitRequested (false) == Step::BringQuestionForward);
    REQUIRE (q.onTakeSettled (true, true) == Step::BringQuestionForward);

    REQUIRE (q.onProblemAnswered (true) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutWork);
    REQUIRE (q.onQuitRequested (false) == Step::BringQuestionForward);
    REQUIRE (q.onTakeSettled (false, true) == Step::BringQuestionForward);
}

TEST_CASE (QuitQuestions_QuitNowCancelsTheWork)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (false) == Step::Proceed);
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitNow) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (false, true) == Step::Quit);
}

TEST_CASE (QuitQuestions_QuitWhenDoneWaitsThenQuitsByItself)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.isWaiting());

    REQUIRE (q.onWaitTick (false, true) == Tick::KeepWaiting);
    REQUIRE (q.onWaitTick (false, false) == Tick::Quit);
    REQUIRE_FALSE (q.isWaiting());
}

TEST_CASE (QuitQuestions_ASecondQuitWhileWaitingAsksAgainInsteadOfCancelling)
{
    // The bug: after "Quit When Done", Cmd-Q again -- or the red button, or
    // the Shut Down macOS had cancelled for the first question -- quit at
    // once and threw away the combined video the user had chosen to keep.
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);

    REQUIRE (q.onQuitRequested (false) == Step::Proceed);
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);

    // Asked again, "Quit When Done" keeps waiting; the wait still ends by itself.
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onWaitTick (false, false) == Tick::Quit);
}

TEST_CASE (QuitQuestions_DontQuitWhenAskedAgainEndsTheWait)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);

    REQUIRE (q.onWorkAnswered (WorkAnswer::DontQuit) == Next::Stay);
    REQUIRE_FALSE (q.isWaiting());

    // The work finishing afterwards must not quit the app the user kept open.
    REQUIRE (q.onWaitTick (false, false) == Tick::StopWaiting);
}

TEST_CASE (QuitQuestions_WorkFinishingWhileAskedAgainWaitsForTheAnswer)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);

    // Done, but the question is still on screen: its answer decides.
    REQUIRE (q.onWaitTick (false, false) == Tick::KeepWaiting);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onWaitTick (false, false) == Tick::Quit);
}

TEST_CASE (QuitQuestions_ANewTakeWhileWaitingCallsTheQuitOff)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onStopTheTakeAnswered (true) == Next::CarryOn);
    REQUIRE (q.onQuitRequested (true) == Step::Proceed);
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);

    // Called off, as "Don't Quit" would: the app stops watching the copies
    // the wait was for, so nothing it kept is asked about at a later quit.
    REQUIRE (q.onWaitTick (true, true) == Tick::CalledOff);
    REQUIRE_FALSE (q.isWaiting());

    // The "Stop and Quit" was for the last take; this one is asked about.
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
}

TEST_CASE (QuitQuestions_AQuitDuringANewTakeBeforeTheWaitNoticesItStillAsks)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onStopTheTakeAnswered (true) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);

    // Record pressed and Cmd-Q at once, before the wait's next poll.
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE_FALSE (q.isWaiting());
}

TEST_CASE (QuitQuestions_AProblemWithTheStoppedTakeIsAskedBeforeQuitting)
{
    // The bug: "Stop and Quit" on a take whose card stopped answering, or
    // whose files came out empty, quit in the same breath -- the notice that
    // said so was raised for a screen that never got to open.
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onStopTheTakeAnswered (true) == Next::CarryOn);
    REQUIRE (q.onQuitRequested (true) == Step::Proceed);

    REQUIRE (q.onTakeSettled (true, false) == Step::AskAboutProblem);
    REQUIRE (q.onProblemAnswered (true) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (true, false) == Step::Quit);
}

TEST_CASE (QuitQuestions_TheProblemComesBeforeTheWorkAndIsNotAskedTwice)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutProblem);
    REQUIRE (q.onProblemAnswered (true) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);

    // Asked again while waiting: about the work, not the problem again.
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutWork);
}

TEST_CASE (QuitQuestions_DontQuitOverAProblemStaysOpenAndAsksAfresh)
{
    QuitQuestions q;
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onStopTheTakeAnswered (true) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutProblem);

    REQUIRE (q.onProblemAnswered (false) == Next::Stay);
    REQUIRE_FALSE (q.isAsking());
    REQUIRE_FALSE (q.isWaiting());

    // Nothing carried over: a later take is asked about from the start, and
    // a problem still unseen is asked about again.
    REQUIRE (q.onQuitRequested (true) == Step::AskToStopTheTake);
    REQUIRE (q.onStopTheTakeAnswered (false) == Next::Stay);
    REQUIRE (q.onTakeSettled (true, false) == Step::AskAboutProblem);
}

TEST_CASE (QuitQuestions_PendingQuitLineNamesTheWork)
{
    REQUIRE (QuitQuestions::pendingQuitLine ("the combined video")
             == "SobStage will quit once it has finished the combined video.");
    REQUIRE (QuitQuestions::pendingQuitLine ("").empty());
}

TEST_CASE (QuitQuestions_ATakeAlreadyShownHasNothingToSay)
{
    StoppedTakeOutcome outcome;
    outcome.alreadyShown = true;
    outcome.cardStoppedAnswering = true;
    outcome.verdict = TakeAudioVerdict::NothingWritten;
    outcome.cameraProblem = "Desk Cam could not finish its video file.";
    REQUIRE (describeUnseenTakeProblem (outcome).empty());
}

TEST_CASE (QuitQuestions_AGoodTakeHasNothingToSay)
{
    StoppedTakeOutcome outcome;
    outcome.alreadyShown = false;
    outcome.folderName = "2026-10-11_2030_show";

    outcome.verdict = TakeAudioVerdict::Recorded;
    REQUIRE (describeUnseenTakeProblem (outcome).empty());

    outcome.verdict = TakeAudioVerdict::NotJudged;
    REQUIRE (describeUnseenTakeProblem (outcome).empty());
}

TEST_CASE (QuitQuestions_EachProblemWithTheTakeIsNamed)
{
    StoppedTakeOutcome outcome;
    outcome.alreadyShown = false;
    outcome.folderName = "2026-10-11_2030_show";

    outcome.verdict = TakeAudioVerdict::NothingWritten;
    REQUIRE (describeUnseenTakeProblem (outcome)
             == "The files in 2026-10-11_2030_show are empty -- no audio reached the drive.");

    outcome.verdict = TakeAudioVerdict::OnlySilence;
    REQUIRE (describeUnseenTakeProblem (outcome).find ("are silent") != std::string::npos);

    outcome.verdict = TakeAudioVerdict::DroppedByApp;
    REQUIRE (describeUnseenTakeProblem (outcome).find ("fault in SobStage") != std::string::npos);

    outcome.verdict = TakeAudioVerdict::NotJudged;
    outcome.cardStoppedAnswering = true;
    REQUIRE (describeUnseenTakeProblem (outcome).find ("may be missing from 2026-10-11_2030_show")
             != std::string::npos);

    // The card's own notice, which says where the backup is, wins over the
    // generic sentence; a camera's account is added after it.
    outcome.cardNotice = "The card stopped answering. The backup copy is at /Users/me/Backup.";
    outcome.cameraProblem = "Desk Cam could not finish its video file.";
    REQUIRE (describeUnseenTakeProblem (outcome)
             == "The card stopped answering. The backup copy is at /Users/me/Backup. "
                "Desk Cam could not finish its video file.");
}

// "Quit When Done" while the copy to the card ran, and the copy failed: the
// wait ended because the copy was no longer running, and the app quit as
// though it had finished. The user left with a card that did not hold the
// take, having been told the app would quit once the copy was done.
TEST_CASE (QuitQuestions_ACopyThatFailsWhileWaitingIsAskedAboutNotQuitOver)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);

    // Still copying: whatever has failed is asked about once it is all done.
    REQUIRE (q.onWaitTick (false, true, true) == Tick::KeepWaiting);

    REQUIRE (q.onWaitTick (false, false, true) == Tick::AskAboutProblem);
    REQUIRE (q.isAsking());
    REQUIRE_FALSE (q.isWaiting());
    REQUIRE (q.onQuitRequested (false) == Step::BringQuestionForward);

    // "Don't Quit": open, and nothing quits by itself.
    REQUIRE (q.onProblemAnswered (false) == Next::Stay);
    REQUIRE (q.onWaitTick (false, false, true) == Tick::StopWaiting);
}

TEST_CASE (QuitQuestions_QuitAnywayAfterAFailedCopyQuits)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onWaitTick (false, false, true) == Tick::AskAboutProblem);
    REQUIRE (q.onProblemAnswered (true) == Next::CarryOn);

    // Told: the take's problem (the failed copy) is not asked again.
    REQUIRE (q.onTakeSettled (true, false) == Step::Quit);
}

TEST_CASE (QuitQuestions_AFailedCopyIsAskedEvenAfterAnEarlierProblemWasAnswered)
{
    // "Quit Anyway" was the answer to the take's silent files. The copy that
    // failed afterwards is a different problem, and is asked about too.
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutProblem);
    REQUIRE (q.onProblemAnswered (true) == Next::CarryOn);
    REQUIRE (q.onTakeSettled (true, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onWaitTick (false, false, true) == Tick::AskAboutProblem);
}

TEST_CASE (QuitQuestions_ACopyThatFinishesWhileWaitingStillQuitsByItself)
{
    QuitQuestions q;
    REQUIRE (q.onTakeSettled (false, true) == Step::AskAboutWork);
    REQUIRE (q.onWorkAnswered (WorkAnswer::QuitWhenDone) == Next::Wait);
    REQUIRE (q.onWaitTick (false, false, false) == Tick::Quit);
}

// The bug: the "Still saving" question, asked about the copy to the card,
// still ended "the recording itself is already saved" -- beside Quit Now,
// which left the take only on this computer and the card without it. Said
// to a producer about to walk off with the card, that read as the take being
// safe on it. Mutation: the copy to the card worded like the combined video.
TEST_CASE (QuitQuestions_StillSavingTheCopyToTheCardSaysTheTakeIsNotOnTheCardYet)
{
    StillSaving copy;
    copy.copyingToCard = true;
    copy.takesCopying = 1;
    copy.computerFolder = "/Users/me/RECORDINGS-MIRROR";

    const auto text = describeStillSaving (copy);
    REQUIRE (text.find ("already saved") == std::string::npos);
    REQUIRE (text.find ("from the last take") == std::string::npos);
    REQUIRE (text.find ("isn't on the card yet") != std::string::npos);
    REQUIRE (text.find ("only on this computer, in /Users/me/RECORDINGS-MIRROR") != std::string::npos);
    REQUIRE (text.find ("the next time you open SobStage with the card in") != std::string::npos);

    // Two takes, one the launch picked up: not "the last take".
    copy.takesCopying = 2;
    REQUIRE (describeStillSaving (copy).find ("still copying 2 takes to the card. They aren't on the card yet")
             != std::string::npos);

    // With the combined video too: that one is cancelled, the take stays here.
    copy.takesCopying = 1;
    copy.otherWork = "the combined video";
    const auto both = describeStillSaving (copy);
    REQUIRE (both.find ("isn't on the card yet") != std::string::npos);
    REQUIRE (both.find ("also still making the combined video, which quitting now cancels") != std::string::npos);
    REQUIRE (both.find ("already saved") == std::string::npos);
}

TEST_CASE (QuitQuestions_StillSavingTheCombinedVideoKeepsItsWords)
{
    StillSaving video;
    video.otherWork = "the combined video";
    REQUIRE (describeStillSaving (video)
             == "SobStage is still making the combined video from the last take. Quitting now cancels it; "
                "the recording itself is already saved.");

    // A take an earlier copy left only on this computer is said too, with
    // where it is -- never that it is on the card.
    video.takesNotOnCard = 1;
    video.computerFolder = "/Users/me/RECORDINGS-MIRROR";
    const auto text = describeStillSaving (video);
    REQUIRE (text.find ("1 take isn't on the card yet. It's safe on this computer in /Users/me/RECORDINGS-MIRROR.")
             != std::string::npos);
}
