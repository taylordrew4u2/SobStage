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

    REQUIRE (q.onWaitTick (true, true) == Tick::StopWaiting);
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
