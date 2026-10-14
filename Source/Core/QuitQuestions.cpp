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
    // mid-show would be far worse than not quitting.
    if (recording)
    {
        reset();
        return Tick::StopWaiting;
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
