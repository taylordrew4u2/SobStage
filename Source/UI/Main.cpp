#include <juce_gui_extra/juce_gui_extra.h>
#include <iostream>
#include "MainComponent.h"
#include "AppLookAndFeel.h"
#include "../App/Application.h"
#include "../App/CappedFileLogger.h"
#include "../Core/QuitGate.h"
#include "../Core/QuitQuestions.h"
#include "../Platform/AppActivation.h"
#if defined (MMA_STALL_METER)
 #include "MessageThreadStallMeter.h"
#endif
#if defined (MMA_UI_WALK)
 #include "UiWalker.h"
#endif

namespace mma {

/// §6.6: a laptop sleeping mid-take is a total failure, so the recording
/// lifetime holds a sleep/screensaver inhibitor for its whole duration. That
/// is Application::sleepInhibitor (Platform/SleepInhibitor): IOKit power
/// assertions on macOS, held from record to stop; a no-op elsewhere.
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, Application& app)
        : juce::DocumentWindow (name,
                                juce::Colour (0xff16110f), // §9.2 background
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (app), true);
        setResizable (true, false);
        // Small enough for a laptop half-screen, never small enough to crush
        // the meters into unreadability.
        // No practical ceiling: at 4096 the window could not be dragged or
        // zoomed to fill a 5120-point-wide ultrawide, or a display arrangement
        // wider than that. 16384 is past any single display in use.
        setResizeLimits (560, 420, 16384, 16384);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    /// The window itself does nothing with a key. When it gets the focus back
    /// with nothing inside remembered to hand it to -- after a dialog such as
    /// a strip's rename closes -- pass it on to the screen, or the next Space
    /// (the room's mute) stops at the window frame.
    void focusGained (FocusChangeType) override
    {
        if (auto* content = getContentComponent())
            content->grabKeyboardFocus();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

class SobStageApplication : public juce::JUCEApplication,
                            private juce::Timer
{
public:
    SobStageApplication() = default;

    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override
    {
        // From CMake's project() version, not typed again here. The literal
        // this replaces said 0.1.0 on every build from v0.1.0 to v0.9.2, so
        // the one place a user could read the version was wrong for nine
        // releases -- and "which build am I running" is the first question
        // asked when a change appears not to have arrived.
       #if defined (SOBSTAGE_VERSION_STRING)
        return SOBSTAGE_VERSION_STRING;
       #else
        return "dev";
       #endif
    }
    bool moreThanOneInstanceAllowed() override          { return false; }

    /// Only one SobStage runs, so a second launch is handed to this one. With
    /// the window minimised or behind others, ignoring it looked like the app
    /// had failed to start; bring the window back instead.
    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow == nullptr)
            return;

        mainWindow->setMinimised (false);
        mainWindow->setVisible (true);
        mainWindow->toFront (true);
    }

    void initialise (const juce::String&) override
    {
        // §11: the diagnostics bundle promises a log, so one has to exist
        // before the first thing that can go wrong. The path comes from
        // Application so the bundle and the logger cannot disagree about where
        // it is, and it is capped so a bundle a user emails stays small --
        // through the whole session, not only at launch (CappedFileLogger).
        constexpr int kMaxLogBytes = 256 * 1024;

        logger = std::make_unique<CappedFileLogger> (
            Application::getLogFile(),
            "SobStage " + getApplicationVersion(),
            kMaxLogBytes);

        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("Starting up.");

       #if defined (MMA_STALL_METER)
        stallMeter = std::make_unique<MessageThreadStallMeter>();
       #endif

        // A machine with no display cannot show a recording screen, and this is
        // the last moment at which that can be said rather than crashed on.
        // JUCE's centreWithSize goes through getParentOrMainMonitorBounds,
        // which dereferences getPrimaryDisplay() WITHOUT checking it: with an
        // empty display list that is a null read, so building the window
        // segfaulted on launch instead of explaining anything. Over SSH, or
        // with a broken display configuration, the app simply died -- exit 139
        // and not a word, in the log or anywhere else.
        //
        // Checked before Application is constructed, so a launch that cannot
        // succeed does not open the audio devices on its way to failing.
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            const auto message = juce::String ("SobStage needs a display, and this computer reports "
                                               "none. If you are connected over SSH or a remote "
                                               "shell, run SobStage on the computer itself.");
            juce::Logger::writeToLog (message);
            std::cerr << message << std::endl;

            setApplicationReturnValue (1);
            quit();
            return;
        }

        application = std::make_unique<Application>();
        // Installed before any window exists, so every control is painted in the
        // §9.2 palette from its first frame rather than flashing JUCE's default
        // blue and being recoloured afterwards.
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        application->initialise();
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *application);

       #if defined (MMA_UI_WALK)
        uiWalker = std::make_unique<UiWalker> (*mainWindow->getContentComponent(), *application);
       #endif
    }

    void shutdown() override
    {
        stopTimer();

       #if defined (MMA_UI_WALK)
        uiWalker.reset();
       #endif

       #if defined (MMA_STALL_METER)
        stallMeter.reset();
       #endif

        mainWindow.reset();

        // Cleared before the look-and-feel goes out of scope: JUCE asserts if a
        // component outlives the look-and-feel it points at.
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

        if (application != nullptr)
            application->shutdown();

        application.reset();

        // Cleared before the logger is destroyed: anything that logs during
        // teardown would otherwise write through a dangling pointer.
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override
    {
        // Mid-take, quitting is asked about first. Cmd+Q sits next to Cmd+W
        // and Tab, and the red button is where the eye goes to close a stray
        // window -- either one, pressed by accident during a show, ended the
        // recording on the spot. Keep Recording is the default and Escape.
        if (! quitsWithoutQuestions())
        {
            const auto step = questions.onQuitRequested (application != nullptr && application->isRecording());

            if (step == QuitQuestions::Step::AskToStopTheTake)
            {
                askBeforeStoppingTheTake();
                return;
            }

            if (step == QuitQuestions::Step::BringQuestionForward)
            {
                bringQuitQuestionForward();
                return;
            }
        }

        const bool ready = application == nullptr || application->prepareToQuit();

        switch (quitGate.onQuitRequested (ready, juce::Time::getMillisecondCounterHiRes()))
        {
            case QuitGate::Decision::QuitNow:
                stopTimer();
                quitOnceAfterTakeWorkIsSettled();
                return;

            case QuitGate::Decision::StartWaiting:
                startTimer (50);
                return;

            case QuitGate::Decision::Ignore:
                // Forcing now would tear the camera down mid-write and leave a
                // truncated movie. Its finalization is bounded, so the first
                // request's wait ends by itself.
                juce::Logger::writeToLog (
                    "Quit requested again while camera files were finishing; still waiting for them.");
                return;

            case QuitGate::Decision::ForceQuit:
                // The user's escape hatch, once the wait has outlived even the
                // camera's own bound. Audio was already drained by the first
                // request; no saved-video claim is made for a movie whose
                // didFinish callback never arrived.
                juce::Logger::writeToLog (
                    "Quit requested again after camera files outlived their time limit; forcing shutdown.");
                stopTimer();
                quit();
                return;
        }
    }

    void askBeforeStoppingTheTake()
    {
        bringQuitQuestionForward();

        auto* window = new juce::AlertWindow ("Stop recording and quit?",
                                              "A take is recording. Quitting stops it now and saves "
                                              "everything recorded so far.",
                                              juce::MessageBoxIconType::WarningIcon,
                                              mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr);
        window->addButton ("Keep Recording", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->addButton ("Stop and Quit", 1);
        openQuestion = window;

        window->enterModalState (true,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                openQuestion = nullptr;

                if (questions.onStopTheTakeAnswered (result == 1) == QuitQuestions::Next::CarryOn)
                    systemRequestedQuit();
                else
                    stayOpen();
            }),
            true);
    }

    void timerCallback() override
    {
        if (! quitGate.onPoll (application == nullptr || application->prepareToQuit()))
            return;

        stopTimer();
        quitOnceAfterTakeWorkIsSettled();
    }

    // Reached once the take is stopped and its files are finished, by a quit
    // with nothing to wait for and by the poll that waited for the cameras.
    //
    // The podcast copy and the combined video are made after a take stops,
    // and quitting cancels them. Quitting straight after a show -- the usual
    // moment -- threw them away without a word, and the user found a folder
    // without the file they were told would be there. So they are asked: wait
    // for it (the app quits by itself once it is done), or quit now.
    void quitOnceAfterTakeWorkIsSettled()
    {
        if (quitsWithoutQuestions())
        {
            afterTakeWaitTimer.stopTimer();
            quit();
            return;
        }

        // The work first: a copy to the card that fails between the two
        // looks is then a problem below, never neither.
        const auto work = application != nullptr ? application->unfinishedAfterTakeWork()
                                                  : juce::String();
        const auto problem = application != nullptr ? application->problemToTellBeforeQuitting()
                                                    : juce::String();

        switch (questions.onTakeSettled (problem.isNotEmpty(), work.isNotEmpty()))
        {
            case QuitQuestions::Step::Quit:
                afterTakeWaitTimer.stopTimer();
                quit();
                return;

            case QuitQuestions::Step::AskAboutProblem:
                askAboutTheTakeProblem (problem);
                return;

            case QuitQuestions::Step::AskAboutWork:
                askAboutAfterTakeWork (work);
                return;

            case QuitQuestions::Step::BringQuestionForward:
                bringQuitQuestionForward();
                return;

            case QuitQuestions::Step::Proceed:
            case QuitQuestions::Step::AskToStopTheTake:
                // Answers to a quit request, never to a settled take.
                return;
        }
    }

    // The UI walk asks to quit at its end and walks none of these questions;
    // only while it is walking. A test build is otherwise the shipping app,
    // so the e2e gates reach the questions.
    bool quitsWithoutQuestions() const
    {
       #if defined (MMA_UI_WALK)
        return uiWalker != nullptr && uiWalker->isWalking();
       #else
        return false;
       #endif
    }

    // A take stopped on the way out -- "Stop and Quit", or a quit that waited
    // for a camera -- can turn out to have a problem: the card stopped
    // answering, the files hold no sound, a movie could not be finished. The
    // saved-take card that says so opens on the next screen refresh, and a
    // quit in the same breath never got there: the user found out later, if
    // at all, after being told everything recorded so far was saved.
    void askAboutTheTakeProblem (const juce::String& problem)
    {
        bringQuitQuestionForward();

        // Said now: the copy failures in it are not asked about again.
        if (application != nullptr)
            application->problemToldBeforeQuitting();

        auto* window = new juce::AlertWindow ("There is a problem with the take",
                                              problem + "\n\nDon't Quit keeps SobStage open so you "
                                              "can check the take.",
                                              juce::MessageBoxIconType::WarningIcon,
                                              mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr);
        window->addButton ("Don't Quit", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->addButton ("Quit Anyway", 1);
        openQuestion = window;

        window->enterModalState (true,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                openQuestion = nullptr;
                carryOnAfterAnswer (questions.onProblemAnswered (result == 1));
            }),
            true);
    }

    void askAboutAfterTakeWork (const juce::String& work)
    {
        bringQuitQuestionForward();

        auto* window = new juce::AlertWindow ("Still saving " + work,
                                              "SobStage is still making " + work + " from the last "
                                              "take. Quitting now cancels it; the recording itself "
                                              "is already saved.",
                                              juce::MessageBoxIconType::QuestionIcon,
                                              mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr);
        window->addButton ("Quit When Done", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Quit Now", 2);
        window->addButton ("Don't Quit", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        openQuestion = window;

        window->enterModalState (true,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                openQuestion = nullptr;
                carryOnAfterAnswer (questions.onWorkAnswered (
                    result == 2 ? QuitQuestions::WorkAnswer::QuitNow
                  : result == 1 ? QuitQuestions::WorkAnswer::QuitWhenDone
                                : QuitQuestions::WorkAnswer::DontQuit));
            }),
            true);
    }

    void carryOnAfterAnswer (QuitQuestions::Next next)
    {
        switch (next)
        {
            case QuitQuestions::Next::CarryOn:
                quitOnceAfterTakeWorkIsSettled();
                return;

            case QuitQuestions::Next::Wait:
                // The app quits by itself once the work is done, and says so
                // meanwhile: a pending quit that showed nothing looked like
                // one that had been ignored, and quitting again cancelled it.
                showQuitPendingNote (application != nullptr ? application->unfinishedAfterTakeWork()
                                                            : juce::String());

                if (! afterTakeWaitTimer.isTimerRunning())
                    afterTakeWaitTimer.startTimer (250);
                return;

            case QuitQuestions::Next::Stay:
                stayOpen();
                return;
        }
    }

    void stayOpen()
    {
        stopWaitingToQuit();

        if (application != nullptr)
            application->quitCalledOff();
    }

    void stopWaitingToQuit()
    {
        afterTakeWaitTimer.stopTimer();
        showQuitPendingNote ({});
    }

    void showQuitPendingNote (const juce::String& work)
    {
        if (mainWindow == nullptr)
            return;

        if (auto* screen = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            screen->setQuitPendingNote (juce::String (QuitQuestions::pendingQuitLine (work.toStdString())));
    }

    // Quit from the Dock, Log Out and Restart reach SobStage while it is
    // hidden or behind another app. Its question then opened behind whatever
    // was in front -- or not at all, while hidden -- and macOS had already
    // been told to wait, so the quit looked ignored and Force Quit mid-take
    // was the next thing to try. Before a question opens, and whenever a quit
    // arrives while one is open: unhide, ask to come forward, bounce the Dock
    // icon until that happens, and raise the question.
    void bringQuitQuestionForward()
    {
        revealAppForQuestion();
        juce::Process::makeForegroundProcess();

        if (auto* question = openQuestion.getComponent())
            question->toFront (true);
    }

private:
    std::unique_ptr<CappedFileLogger> logger;
    std::unique_ptr<Application> application;
    // Declared before mainWindow, so it is destroyed after it. Members are
    // destroyed in reverse declaration order, and JUCE asserts if a component
    // outlives the look-and-feel it points at.
    AppLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    QuitGate quitGate;
    QuitQuestions questions;
    // The quit question on screen, if any, so a repeated quit can raise it.
    juce::Component::SafePointer<juce::AlertWindow> openQuestion;
    juce::TimedCallback afterTakeWaitTimer { [this]
    {
        // The work first, then what failed: the copy to the card that the
        // wait was for is out of the work by the time its failure is read.
        const auto work = application != nullptr ? application->unfinishedAfterTakeWork()
                                                 : juce::String();
        const auto copyFailed = application != nullptr ? application->cardCopyFailedWhileQuitting()
                                                       : juce::String();

        switch (questions.onWaitTick (application != nullptr && application->isRecording(),
                                      work.isNotEmpty(), copyFailed.isNotEmpty()))
        {
            case QuitQuestions::Tick::KeepWaiting:
                // What is left, as each piece finishes.
                if (work.isNotEmpty())
                    showQuitPendingNote (work);
                return;

            case QuitQuestions::Tick::StopWaiting:
                // A new take started while waiting -- the quit is off -- or
                // the wait had already ended.
                stopWaitingToQuit();
                return;

            case QuitQuestions::Tick::Quit:
                afterTakeWaitTimer.stopTimer();
                quit();
                return;

            case QuitQuestions::Tick::AskAboutProblem:
                // "Quit When Done" was a promise to quit once the copy to the
                // card was finished. It failed: the card in hand does not hold
                // the take, and quitting as if it did is what this stops.
                stopWaitingToQuit();
                askAboutTheTakeProblem (copyFailed);
                return;
        }
    } };

   #if defined (MMA_STALL_METER)
    std::unique_ptr<MessageThreadStallMeter> stallMeter;
   #endif

   #if defined (MMA_UI_WALK)
    std::unique_ptr<UiWalker> uiWalker;
   #endif
};

} // namespace mma

START_JUCE_APPLICATION (mma::SobStageApplication)
