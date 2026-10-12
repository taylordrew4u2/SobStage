#pragma once

#include <string>
#include "TakeCompleteness.h"

namespace mma {

/// The questions a request to quit can raise, and what each answer means.
///
/// Three things are asked about, in this order: a take that is still
/// recording ("Stop recording and quit?"), a problem with the take that was
/// just stopped that nobody has been shown yet, and the combined video or
/// podcast copy still being made from it ("Still saving ...").
///
/// Mac users repeat a quit that seems to have done nothing: Cmd-Q again, the
/// red close button, Quit from the Dock, and a Shut Down or Log Out that
/// macOS already gave up on because the app asked a question. So:
///  - a request while a question is open shows that question again, and
///    never stacks a second one or answers the first;
///  - "Quit When Done" is a promise to wait. A request while waiting asks
///    again. Only "Quit Now" cancels the work: a repeat used to quit at once
///    and throw away the combined video the user had just chosen to keep.
///
/// QuitGate decides whether the take's camera files are finished; this
/// decides what the user is asked once they are. Pure bookkeeping, so every
/// path is testable without a message loop or a dialog.
class QuitQuestions
{
public:
    enum class Step
    {
        Proceed,              ///< nothing to ask yet: stop the take if need be, wait for its files
        Quit,                 ///< nothing left to ask: quit now
        AskToStopTheTake,     ///< "Stop recording and quit?"
        AskAboutProblem,      ///< the take just stopped has a problem the user has not seen
        AskAboutWork,         ///< "Still saving ...": the combined video or podcast copy
        BringQuestionForward, ///< a question is already open: show it again
    };

    /// What follows an answer.
    enum class Next
    {
        CarryOn, ///< the quit goes on: ask the next question, or quit
        Wait,    ///< quit by itself once the work is done
        Stay,    ///< the quit is off
    };

    enum class WorkAnswer { QuitWhenDone, QuitNow, DontQuit };

    enum class Tick
    {
        KeepWaiting, ///< the work is still running, or a question is open
        Quit,        ///< the work is done: quit now
        StopWaiting, ///< the wait is over without quitting (a take started)
    };

    /// A request to quit. `recording`: a take is running.
    /// Proceed, AskToStopTheTake or BringQuestionForward.
    Step onQuitRequested (bool recording) noexcept;

    /// The request has reached the point where the take is stopped and its
    /// files are finished. `unseenProblem`: something went wrong with that
    /// take that has not been shown. `workRunning`: the combined video or the
    /// podcast copy is still being made. Quit, AskAboutProblem, AskAboutWork
    /// or BringQuestionForward.
    Step onTakeSettled (bool unseenProblem, bool workRunning) noexcept;

    /// "Stop and Quit" (true) or "Keep Recording" (false).
    Next onStopTheTakeAnswered (bool stopAndQuit) noexcept;

    /// "Quit Anyway" (true) or "Don't Quit" (false).
    Next onProblemAnswered (bool quitAnyway) noexcept;

    Next onWorkAnswered (WorkAnswer answer) noexcept;

    /// While waiting, polled. `recording`: a take is running.
    /// `workRunning`: the combined video or podcast copy is still being made.
    Tick onWaitTick (bool recording, bool workRunning) noexcept;

    bool isAsking() const noexcept   { return asking != Question::None; }
    bool isWaiting() const noexcept  { return waiting; }

    /// The line shown on the main screen while the app waits to quit.
    /// `work` is unfinishedAfterTakeWork(): "the combined video", ...
    static std::string pendingQuitLine (const std::string& work);

private:
    enum class Question { None, StopTheTake, TakeProblem, AfterTakeWork };

    void reset() noexcept;

    Question asking = Question::None;
    bool stopConfirmed = false;       ///< "Stop and Quit"
    bool problemAcknowledged = false; ///< "Quit Anyway"
    bool quitNowConfirmed = false;    ///< "Quit Now"
    bool waiting = false;             ///< "Quit When Done"
};

/// What happened to a take that was just stopped, as far as quitting needs
/// to know.
struct StoppedTakeOutcome
{
    /// The saved-take card has opened for this take (or there is no take).
    /// Nothing is said at quit about a take whose result was already shown.
    bool alreadyShown = true;

    /// The card-removal account, when one is waiting to be shown.
    std::string cardNotice;

    /// The card stopped answering while the take was being finished.
    bool cardStoppedAnswering = false;

    TakeAudioVerdict verdict = TakeAudioVerdict::NotJudged;

    /// The take folder's name, for "the files in <folder>".
    std::string folderName;

    /// A camera that could not finish its movie, in its own words.
    std::string cameraProblem;
};

/// What went wrong with a take that was just stopped and has not been shown,
/// in plain language; empty when nothing did or it was already shown.
///
/// Stopping a take on the way out -- "Stop and Quit", or a quit while a
/// camera was finishing -- used to quit in the same breath. A card that
/// stopped answering during the stop, files with no sound in them, or a
/// movie that could not be finished were raised for the saved-take card, and
/// the app closed before that card could open.
std::string describeUnseenTakeProblem (const StoppedTakeOutcome& outcome);

} // namespace mma
