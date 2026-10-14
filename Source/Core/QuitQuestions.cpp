#include "QuitQuestions.h"

namespace mma {

QuitQuestions::Step QuitQuestions::onQuitRequested (bool recording) noexcept
{
    // Asked again while the question is up -- from the Dock, the red button,
    // or a Log Out macOS already gave up on. The question may be hidden or
    // behind another app; show it rather than answering it or adding another.
    if (asking != Question::None)
        return Step::BringQuestionForward;

    // A take started while the app was waiting to quit. The wait, and the
    // "Stop and Quit" that led to it, belonged to the last take; this one is
    // asked about from the start.
    if (recording && waiting)
        reset();

    if (recording && ! stopConfirmed)
    {
        asking = Question::StopTheTake;
        return Step::AskToStopTheTake;
    }

    return Step::Proceed;
}

QuitQuestions::Step QuitQuestions::onTakeSettled (bool unseenProblem, bool workRunning) noexcept
{
    if (asking != Question::None)
        return Step::BringQuestionForward;

    // First, before the after-take work: a take with a problem is the thing
    // the user most needs to hear about, and the work question would hide it.
    if (unseenProblem && ! problemAcknowledged)
    {
        asking = Question::TakeProblem;
        return Step::AskAboutProblem;
    }

    // Only "Quit Now" cancels the work. Waiting ("Quit When Done") is not
    // permission: a repeat here asks again.
    if (! workRunning || quitNowConfirmed)
        return Step::Quit;

    asking = Question::AfterTakeWork;
    return Step::AskAboutWork;
}

QuitQuestions::Next QuitQuestions::onStopTheTakeAnswered (bool stopAndQuit) noexcept
{
    asking = Question::None;

    if (! stopAndQuit)
    {
        reset();
        return Next::Stay;
    }

    stopConfirmed = true;
    return Next::CarryOn;
}

QuitQuestions::Next QuitQuestions::onProblemAnswered (bool quitAnyway) noexcept
{
    asking = Question::None;

    if (! quitAnyway)
    {
        reset();
        return Next::Stay;
    }

    problemAcknowledged = true;
    return Next::CarryOn;
}

QuitQuestions::Next QuitQuestions::onWorkAnswered (WorkAnswer answer) noexcept
{
    asking = Question::None;

    switch (answer)
    {
        case WorkAnswer::QuitNow:
            quitNowConfirmed = true;
            waiting = false;
            return Next::CarryOn;

        case WorkAnswer::QuitWhenDone:
            waiting = true;
            return Next::Wait;

        case WorkAnswer::DontQuit:
            break;
    }

    // Staying open, including after a "Quit When Done" asked about again:
    // the app must not then quit by itself.
    reset();
    return Next::Stay;
}

QuitQuestions::Tick QuitQuestions::onWaitTick (bool recording, bool workRunning, bool problemArose) noexcept
{
    if (! waiting)
        return Tick::StopWaiting;

    // A new take started while waiting: the quit is off. Quitting by itself
    // mid-show would be far worse than not quitting. Called off as "Don't
    // Quit" calls it off, not merely stopped: what the wait was watching for
    // -- a copy to the card that failed -- must not be asked about at some
    // later quit, long after it was copied.
    if (recording)
    {
        reset();
        return Tick::CalledOff;
    }

    // The question on screen decides, even if the work has just finished:
    // its answer may be "Don't Quit".
    if (asking != Question::None || workRunning)
        return Tick::KeepWaiting;

    waiting = false;

    // The wait is over, and not as promised: a copy it was for failed. The
    // answer to this question decides, as it does before any wait.
    if (problemArose)
    {
        problemAcknowledged = false;
        asking = Question::TakeProblem;
        return Tick::AskAboutProblem;
    }

    return Tick::Quit;
}

void QuitQuestions::reset() noexcept
{
    stopConfirmed = false;
    problemAcknowledged = false;
    quitNowConfirmed = false;
    waiting = false;
}

std::string QuitQuestions::pendingQuitLine (const std::string& work)
{
    if (work.empty())
        return {};

    return "SobStage will quit once it has finished " + work + ".";
}

std::string describeStillSaving (const StillSaving& work)
{
    std::string text;

    if (! work.copyingToCard)
    {
        text = "SobStage is still making " + work.otherWork + " from the last take. Quitting now cancels it; "
               "the recording itself is already saved.";
    }
    else
    {
        const int n = work.takesCopying;
        const bool one = n == 1;
        const auto takes = one ? std::string ("1 take") : n > 1 ? std::to_string (n) + " takes" : std::string ("takes");

        text = "SobStage is still copying " + takes + " to the card. " + (one ? "It isn't" : "They aren't")
             + " on the card yet: quitting now leaves " + (one ? "it" : "them") + " only on this computer, in "
             + work.computerFolder + ", to be copied the next time you open SobStage with the card in.";

        if (! work.otherWork.empty())
            text += " It is also still making " + work.otherWork + ", which quitting now cancels.";
    }

    if (work.takesNotOnCard > 0)
    {
        const bool one = work.takesNotOnCard == 1;
        const auto other = work.copyingToCard ? std::string ("other ") : std::string();

        text += " " + (one ? "1 " + other + "take isn't" : std::to_string (work.takesNotOnCard) + " " + other + "takes aren't")
              + " on the card yet. " + (one ? "It's" : "They're") + " safe on this computer in "
              + work.computerFolder + ".";
    }

    return text;
}

std::string describeUnseenTakeProblem (const StoppedTakeOutcome& outcome)
{
    if (outcome.alreadyShown)
        return {};

    std::string text;

    const auto add = [&text] (const std::string& sentence)
    {
        if (sentence.empty())
            return;

        if (! text.empty())
            text += ' ';

        text += sentence;
    };

    const auto folder = outcome.folderName.empty() ? std::string ("the take's folder")
                                                   : outcome.folderName;

    // The card's own account when there is one: it says where the backup is.
    if (! outcome.cardNotice.empty())
        add (outcome.cardNotice);
    else if (outcome.cardStoppedAnswering)
        add ("The card stopped answering while the take was being finished, so the end of the "
             "take may be missing from " + folder + ".");

    // The same words the status line uses for these, so the two agree.
    switch (outcome.verdict)
    {
        case TakeAudioVerdict::DroppedByApp:
            add ("The sound did not reach the files in " + folder
                 + " -- your microphones were working. This is a fault in SobStage.");
            break;

        case TakeAudioVerdict::OnlySilence:
            add ("The files in " + folder
                 + " are silent -- the microphones were connected but sent no sound.");
            break;

        case TakeAudioVerdict::NothingWritten:
            add ("The files in " + folder + " are empty -- no audio reached the drive.");
            break;

        case TakeAudioVerdict::NotJudged:
        case TakeAudioVerdict::Recorded:
            break;
    }

    add (outcome.cameraProblem);
    return text;
}

} // namespace mma
