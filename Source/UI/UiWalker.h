#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"
#include "MainScreen.h"
#include "AdvancedPanel.h"
#include "CameraPanel.h"
#include "HelpPanel.h"
#include "SetupGuidePanel.h"
#include "SaveLocationPrompt.h"
#include "SavedTakePanel.h"
#include "RecoveredTakesPanel.h"
#include "TakeAlertCard.h"
#include "TakeBanner.h"
#include "ChannelMeterComponent.h"
#include "MeterFaceProbe.h"
#include "../App/Application.h"
#include "../Core/QuitQuestions.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <type_traits>

namespace mma {

/// Test builds only. Drives the real app through every screen the way a
/// person would -- Settings, Help, Cameras, every picker, tick box and slider,
/// a renamed microphone, a real take, the saved-take card, diagnostics export
/// -- by walking the live component tree rather than clicking fixed pixels, so
/// a moved button is still found and a new one is still pressed.
///
/// Inert unless MMA_UI_WALK_REPORT names a file. Every check is written there
/// as it happens ("PASS ..." / "FAIL ..."), and the last line is
/// "UI-WALK DONE failures=N". Tools/e2e_ui_walk.sh reads it.
///
///   MMA_UI_WALK_MODE=crash      record, write "RECORDING" and wait to be killed
///   MMA_UI_WALK_MODE=fault      record, create MMA_UI_WALK_FAULT_FILE (which
///                               Tools/alsa_readi_shim.cpp watches to kill a
///                               microphone), walk the mid-take alert card,
///                               remove the file and record a second take,
///                               then kill it again between takes for a third
///   MMA_UI_WALK_MODE=proof      record a take whose files never grow (the proof's
///                               evidence is starved), and walk the app's own stop
///   MMA_UI_WALK_EXPECT_RECOVERED=1   the recovery card must appear at launch
///   MMA_UI_WALK_SNAPSHOT_DIR=<dir>   save PNGs of the window at the banners
///                               and the alarm card, for a person to look at
class UiWalker : private juce::Timer
{
public:
    UiWalker (juce::Component& mainContent, Application& app)
        : root (mainContent), application (app)
    {
        const char* path = std::getenv ("MMA_UI_WALK_REPORT");
        if (path == nullptr || *path == '\0')
            return;

        report.open (path, std::ios::out | std::ios::trunc);
        const char* mode = std::getenv ("MMA_UI_WALK_MODE");
        crashMode = mode != nullptr && juce::String (mode) == "crash";
        faultMode = mode != nullptr && juce::String (mode) == "fault";
        proofMode = mode != nullptr && juce::String (mode) == "proof";
        if (const char* f = std::getenv ("MMA_UI_WALK_FAULT_FILE"))
            faultFile = f;
        if (const char* dir = std::getenv ("MMA_UI_WALK_SNAPSHOT_DIR"))
            snapshotDir = dir;
        const char* recovered = std::getenv ("MMA_UI_WALK_EXPECT_RECOVERED");
        expectRecovered = recovered != nullptr && juce::String (recovered) == "1";

        buildScript();
        startTimer (kTickMs);
    }

    ~UiWalker() override { stopTimer(); }

private:
    static constexpr int kTickMs = 100;

    struct Step
    {
        juce::String what;
        std::function<void()> action;           // run once
        std::function<bool()> until;            // polled until true, or the step fails
        int timeoutMs = 3000;
        bool required = true;                   // a failed optional step is reported, not counted
    };

    juce::Component& root;
    Application& application;
    std::ofstream report;
    std::deque<Step> steps;
    std::optional<Step> current;
    int stepElapsedMs = 0;
    int failures = 0;
    int passes = 0;
    bool crashMode = false;
    bool faultMode = false;
    bool proofMode = false;
    juce::String faultFile;
    bool expectRecovered = false;
    bool finished = false;
    std::set<juce::String> exercised;
    int diagnosticsZipsBefore = 0;
    juce::String takeFolder;
    juce::Rectangle<int> windowBoundsBefore;

    // --- reporting -------------------------------------------------------

    void line (const juce::String& text)
    {
        report << text.toStdString() << std::endl;
    }

    void pass (const juce::String& what) { ++passes; line ("PASS  " + what); }
    void fail (const juce::String& what)
    {
        ++failures;
        line ("FAIL  " + what);
        line ("      on screen: " + whatIsUp());
    }

    /// The screens and cards showing, and every enabled button's label, so a
    /// failure says what the walker was looking at.
    juce::String whatIsUp() const
    {
        juce::StringArray parts;
        if (isUp<AdvancedPanel>())       parts.add ("Settings");
        if (isUp<HelpPanel>())           parts.add ("Help");
        if (isUp<SetupGuidePanel>())     parts.add ("setup guide");
        if (isUp<CameraPanel>())         parts.add ("Cameras");
        if (isUp<SaveLocationPrompt>())  parts.add ("save-location card");
        if (isUp<SavedTakePanel>())      parts.add ("saved-take card");
        if (isUp<RecoveredTakesPanel>()) parts.add ("recovered-takes card");
        if (isUp<TakeAlertCard>())       parts.add ("mid-take alert card");
        if (find<juce::AlertWindow>() != nullptr) parts.add ("dialog");

        juce::StringArray buttons;
        for (auto* c : showing())
            if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->isEnabled() && b->getButtonText().isNotEmpty())
                buttons.addIfNotAlreadyThere ("\"" + b->getButtonText() + "\"");

        return (parts.isEmpty() ? juce::String ("main screen only") : parts.joinIntoString (", "))
             + "; buttons " + buttons.joinIntoString (" ");
    }

    // --- finding things in the live window --------------------------------

    static void collect (juce::Component& c, std::vector<juce::Component*>& out)
    {
        for (int i = 0; i < c.getNumChildComponents(); ++i)
        {
            auto* child = c.getChildComponent (i);
            if (child->isShowing())
            {
                out.push_back (child);
                collect (*child, out);
            }
        }
    }

    std::vector<juce::Component*> showing() const
    {
        std::vector<juce::Component*> out;
        collect (root, out);

        // Anything modal lives outside the window's content: an AlertWindow.
        auto* modal = juce::ModalComponentManager::getInstance();
        for (int i = 0; i < modal->getNumModalComponents(); ++i)
            if (auto* m = modal->getModalComponent (i); m != nullptr && m->isShowing())
            {
                out.push_back (m);
                collect (*m, out);
            }

        return out;
    }

    template <typename T>
    T* find() const
    {
        for (auto* c : showing())
            if (auto* t = dynamic_cast<T*> (c))
                return t;
        return nullptr;
    }

    template <typename Panel>
    bool isUp() const { return find<Panel>() != nullptr; }

    /// Whether Panel is what a person actually sees: the frontmost thing at the
    /// middle of the window. isUp() only asks whether a card is showing, and to
    /// JUCE a card buried under another one is still showing.
    template <typename Panel>
    bool onTop() const
    {
        auto* hit = root.getComponentAt (root.getLocalBounds().getCentre());
        return hit != nullptr
            && (dynamic_cast<Panel*> (hit) != nullptr || hit->findParentComponentOfClass<Panel>() != nullptr);
    }

    /// A line reading `text` inside a showing Panel, and the frontmost thing
    /// at its middle: what a person sees there, not merely what JUCE calls
    /// showing -- a line under a card is showing too.
    template <typename Panel>
    bool lineInFront (const juce::String& text) const
    {
        for (auto* c : showing())
            if (auto* label = dynamic_cast<juce::Label*> (c);
                label != nullptr && label->getText() == text && label->findParentComponentOfClass<Panel>() != nullptr)
                return root.getComponentAt (root.getLocalPoint (label, label->getLocalBounds().getCentre())) == label;

        return false;
    }

    /// A showing, enabled button with this text, inside a Panel if one is
    /// given (void: anywhere). Several screens have a "Done" or a "Close".
    template <typename Panel = void>
    juce::Button* button (const juce::String& text) const
    {
        for (auto* c : showing())
            if (auto* b = dynamic_cast<juce::Button*> (c))
                if (b->getButtonText() == text && b->isEnabled())
                {
                    if constexpr (std::is_void_v<Panel>)
                        return b;
                    else if (b->findParentComponentOfClass<Panel>() != nullptr)
                        return b;
                }
        return nullptr;
    }

    template <typename Panel = void>
    bool click (const juce::String& text)
    {
        if (auto* b = button<Panel> (text))
        {
            exercised.insert (b->getButtonText());
            b->triggerClick();
            return true;
        }
        return false;
    }

    /// The monitor mute button reads "Mute" or "Unmute" depending on state.
    juce::Button* muteButton() const
    {
        if (auto* b = button<MainScreen> ("Mute"))
            return b;
        return button<MainScreen> ("Unmute");
    }

    bool muted() const
    {
        auto* b = muteButton();
        return b != nullptr && b->getToggleState();
    }

    /// What the headphones actually do, not what the button shows.
    bool busMuted()
    {
        auto* bus = application.getMonitorBus();
        return bus != nullptr && bus->isMuted();
    }

    /// A §5 runaway cut on the bus the headphones are fed from right now.
    bool busCut()
    {
        auto* bus = application.getMonitorBus();
        return bus != nullptr && bus->isRunawayMuted();
    }

    bool recording() { return application.getRecordingEngine().getState() == RecordingState::Recording; }

    juce::Component* mainComponent() const
    {
        for (auto* c : showing())
            if (dynamic_cast<MainComponent*> (c) != nullptr)
                return c;
        return dynamic_cast<MainComponent*> (&root) != nullptr ? &root : nullptr;
    }

    /// The main screen, found whether or not anything is showing.
    MainScreen* mainScreenEvenIfHidden() const
    {
        std::function<MainScreen* (juce::Component&)> search = [&search] (juce::Component& c) -> MainScreen*
        {
            if (auto* ms = dynamic_cast<MainScreen*> (&c))
                return ms;
            for (auto* child : c.getChildren())
                if (auto* ms = search (*child))
                    return ms;
            return nullptr;
        };
        return search (root);
    }

    /// The app's one window, found from the root even while it is hidden.
    juce::ResizableWindow* mainWindow() const
    {
        return dynamic_cast<juce::ResizableWindow*> (root.getTopLevelComponent());
    }

    bool pressKey (int keyCode)
    {
        if (auto* mc = mainComponent())
            return mc->keyPressed (juce::KeyPress (keyCode));
        return false;
    }

    /// A real left click, delivered through the window the way the OS would
    /// deliver one. triggerClick() and calling mouseUp() skip everything JUCE
    /// does on the way in -- in particular which component the click hands
    /// the keyboard to.
    bool clickWithMouse (juce::Component& target)
    {
        auto* peer = target.getPeer();
        if (peer == nullptr)
            return false;

        const auto at = peer->globalToLocal (target.localPointToGlobal (target.getLocalBounds().getCentre()).toFloat());
        const auto now = juce::Time::currentTimeMillis();
        const auto source = juce::MouseInputSource::InputSourceType::mouse;
        peer->handleMouseEvent (source, at, juce::ModifierKeys::leftButtonModifier,
                                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, now);
        peer->handleMouseEvent (source, at, juce::ModifierKeys(),
                                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, now + 50);
        return true;
    }

    /// A key delivered through the window, so it goes to whatever has the
    /// keyboard focus -- unlike pressKey(), which hands it to MainComponent
    /// whoever has the focus.
    bool pressKeyThroughWindow (int keyCode)
    {
        if (auto* mc = mainComponent())
            if (auto* peer = mc->getPeer())
                return peer->handleKeyPress (juce::KeyPress (keyCode));
        return false;
    }

    /// Every modal card, and the take banner, is the size of the whole window
    /// -- not of whatever the Settings or Help drawer left beside it, or the
    /// drawer's controls stay live beside a card that is meant to own the
    /// screen. Asked of the cards whether or not they are showing: the layout
    /// is the same either way, and a hidden card is the next one to appear.
    bool overlaysCoverWindow() const
    {
        auto* mc = mainComponent();
        if (mc == nullptr)
            return false;

        int overlays = 0;
        for (auto* c : mc->getChildren())
            if (dynamic_cast<SaveLocationPrompt*> (c) != nullptr || dynamic_cast<SavedTakePanel*> (c) != nullptr
                || dynamic_cast<RecoveredTakesPanel*> (c) != nullptr || dynamic_cast<TakeAlertCard*> (c) != nullptr
                || dynamic_cast<TakeBanner*> (c) != nullptr)
            {
                if (c->getBounds() != mc->getLocalBounds())
                    return false;
                ++overlays;
            }

        return overlays == 5;
    }

    static juce::File desktop() { return juce::File::getSpecialLocation (juce::File::userDesktopDirectory); }

    /// The Settings trim slider with this accessible title, if it is there.
    juce::Slider* trimSlider (const juce::String& title) const
    {
        auto* panel = find<AdvancedPanel>();
        if (panel == nullptr)
            return nullptr;

        std::vector<juce::Component*> inside;
        collect (*panel, inside);
        for (auto* c : inside)
            if (auto* s = dynamic_cast<juce::Slider*> (c); s != nullptr && s->getTitle() == title)
                return s;
        return nullptr;
    }

    static int countDiagnosticsZips()
    {
        return desktop().getNumberOfChildFiles (juce::File::findFiles, "SobStage-diagnostics*.zip");
    }

    static bool newestDiagnosticsZipIsReal()
    {
        auto zips = desktop().findChildFiles (juce::File::findFiles, false, "SobStage-diagnostics*.zip");
        if (zips.isEmpty())
            return false;

        std::sort (zips.begin(), zips.end(),
                   [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() < b.getLastModificationTime(); });
        juce::ZipFile zip (zips.getLast());
        return zip.getNumEntries() > 0;
    }

    /// Dismisses whatever card or drawer is up, so the next step starts from
    /// the plain main screen.
    void backToMain()
    {
        for (const auto& text : { "Done", "Not yet", "Keep recording", "Cancel" })
            if (auto* b = button (text))
                b->triggerClick();

        pressKey (juce::KeyPress::escapeKey);
    }

    // --- the script -------------------------------------------------------

    void add (juce::String what, std::function<void()> action, std::function<bool()> until,
              int timeoutMs = 3000, bool required = true)
    {
        steps.push_back ({ std::move (what), std::move (action), std::move (until), timeoutMs, required });
    }

    void check (juce::String what, std::function<bool()> condition, int timeoutMs = 3000)
    {
        add (std::move (what), [] {}, std::move (condition), timeoutMs);
    }

    void settle (int ms)
    {
        auto remaining = std::make_shared<int> (ms);
        add ({}, [] {}, [remaining] { *remaining -= kTickMs; return *remaining <= 0; }, ms + 1000);
    }

    /// Queued at the front, so a step can expand into the controls it finds.
    void insertNext (std::vector<Step> more)
    {
        for (auto it = more.rbegin(); it != more.rend(); ++it)
            steps.push_front (std::move (*it));
    }

    /// Every tick box, picker and slider inside Panel, each moved and put
    /// back, with the app given a moment to act on each change.
    template <typename Panel>
    std::vector<Step> exerciseControlsIn (const juce::String& where)
    {
        std::vector<Step> more;
        auto* panel = find<Panel>();
        if (panel == nullptr)
            return more;

        std::vector<juce::Component*> inside;
        collect (*panel, inside);

        const auto settleStep = [] { return Step { {}, [] {}, [n = std::make_shared<int> (4)] { return --*n <= 0; }, 2000, false }; };

        for (auto* c : inside)
        {
            juce::Component::SafePointer<juce::Component> safe (c);

            // A toggle whose state is set by the app rather than by the click
            // (the monitor mute follows the headphone bus) has its own checks.
            if (auto* t = dynamic_cast<juce::ToggleButton*> (c); t != nullptr && t->isEnabled() && t->getClickingTogglesState())
            {
                const auto name = t->getButtonText();
                const bool before = t->getToggleState();
                more.push_back ({ where + ": tick box \"" + name + "\" turns " + (before ? "off" : "on"),
                                  [safe] { if (auto* b = dynamic_cast<juce::Button*> (safe.getComponent())) b->triggerClick(); },
                                  [safe, before] { auto* b = dynamic_cast<juce::Button*> (safe.getComponent()); return b == nullptr || b->getToggleState() != before; } });
                more.push_back (settleStep());
                more.push_back ({ where + ": tick box \"" + name + "\" goes back",
                                  [safe, before] { if (auto* b = dynamic_cast<juce::Button*> (safe.getComponent())) if (b->getToggleState() != before) b->triggerClick(); },
                                  [safe, before] { auto* b = dynamic_cast<juce::Button*> (safe.getComponent()); return b == nullptr || b->getToggleState() == before; } });
                more.push_back (settleStep());
                exercised.insert ("toggle:" + name);
            }
            else if (auto* combo = dynamic_cast<juce::ComboBox*> (c); combo != nullptr && combo->isEnabled() && combo->getNumItems() > 0)
            {
                const int original = combo->getSelectedItemIndex();
                const auto label = combo->getName().isNotEmpty() ? combo->getName() : combo->getTooltip();

                for (int i = 0; i < combo->getNumItems(); ++i)
                {
                    if (! combo->isItemEnabled (combo->getItemId (i)))
                        continue;

                    const auto item = combo->getItemText (i);
                    more.push_back ({ where + ": picker " + label + " set to \"" + item + "\"",
                                      [safe, i] { if (auto* cb = dynamic_cast<juce::ComboBox*> (safe.getComponent())) cb->setSelectedItemIndex (i, juce::sendNotificationSync); },
                                      [] { return true; } });
                    more.push_back (settleStep());
                }

                more.push_back ({ where + ": picker " + label + " restored",
                                  [safe, original] { if (auto* cb = dynamic_cast<juce::ComboBox*> (safe.getComponent())) cb->setSelectedItemIndex (original, juce::sendNotificationSync); },
                                  [] { return true; } });
                more.push_back (settleStep());
                exercised.insert ("picker:" + label);
            }
            else if (auto* slider = dynamic_cast<juce::Slider*> (c); slider != nullptr && slider->isEnabled())
            {
                const double original = slider->getValue();
                for (double v : { slider->getMinimum(), slider->getMaximum(), original })
                {
                    more.push_back ({ where + ": slider to " + juce::String (v, 1),
                                      [safe, v] { if (auto* s = dynamic_cast<juce::Slider*> (safe.getComponent())) s->setValue (v, juce::sendNotificationSync); },
                                      [safe, v] { auto* s = dynamic_cast<juce::Slider*> (safe.getComponent()); return s == nullptr || std::abs (s->getValue() - v) < 1e-6; } });
                    more.push_back (settleStep());
                }
                exercised.insert ("slider:" + where);
            }
        }

        return more;
    }

    void buildScript()
    {
        line ("UI-WALK START");

        check ("the main screen comes up", [this] { return find<MainScreen>() != nullptr; }, 30000);

        if (expectRecovered)
        {
            check ("the interrupted take from last time is offered back at launch",
                   [this] { return isUp<RecoveredTakesPanel>(); }, 30000);
            add ("the recovery card's Done closes it",
                 [this] { click<RecoveredTakesPanel> ("Done"); },
                 [this] { return ! isUp<RecoveredTakesPanel>(); });
        }

        check ("the microphones are found and shown", [this]
        {
            auto* ms = find<MainScreen>();
            return ms != nullptr && ms->getMicCount() > 0;
        }, 30000);

        // The first-run setup guide. Every launch but the one after the crash
        // is on a fresh profile, so it opens by itself; walked here, before
        // anything else, so the rest of the walk starts from the plain screen.
        if (expectRecovered)
        {
            settle (1500);
            check ("the setup guide closed on the last launch stays closed",
                   [this] { return ! isUp<SetupGuidePanel>() && application.isSetupGuideDone(); });
        }
        else
        {
            check ("a first launch opens the setup guide beside the main screen",
                   [this] { return isUp<SetupGuidePanel>() && isUp<MainScreen>(); }, 15000);
            check ("its first page counts the microphones", [this]
            {
                auto* guide = find<SetupGuidePanel>();
                return guide != nullptr && guide->getPage().step == SetupGuideStep::Microphones
                    && juce::String (guide->getPage().status).startsWith ("Found");
            }, 5000);
            check ("and the record button is not behind it", [this]
            {
                auto* record = button<MainScreen> ("Start recording");
                if (record == nullptr)
                    for (auto* c : showing())
                        if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->getButtonText() == "Start recording")
                            record = b;

                auto* mc = mainComponent();
                if (record == nullptr || mc == nullptr)
                    return false;

                const auto centre = mc->getLocalPoint (record, record->getLocalBounds().getCentre());
                return mc->getComponentAt (centre) == record;
            });
            add ("Next goes to naming the microphones", [this] { click<SetupGuidePanel> ("Next"); }, [this]
            {
                auto* guide = find<SetupGuidePanel>();
                return guide != nullptr && guide->getPage().step == SetupGuideStep::Names;
            });
            check ("with a Name button for every microphone", [this]
            {
                auto* ms = find<MainScreen>();
                int names = 0;
                for (auto* c : showing())
                    if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->getButtonText() == "Name"
                                                                   && b->findParentComponentOfClass<SetupGuidePanel>() != nullptr)
                        ++names;
                return ms != nullptr && names == ms->getMicCount();
            });
            add ("Back goes back", [this] { click<SetupGuidePanel> ("Back"); }, [this]
            {
                auto* guide = find<SetupGuidePanel>();
                return guide != nullptr && guide->getPage().step == SetupGuideStep::Microphones;
            });
            add ("Next walks every page to the last", [] {}, [this]
            {
                auto* guide = find<SetupGuidePanel>();
                if (guide == nullptr)
                    return false;
                if (guide->getPage().isLast)
                    return guide->getPage().step == SetupGuideStep::TestTake;
                click<SetupGuidePanel> ("Next");
                return false;
            }, 10000);
            add ("Settings puts the guide aside", [this] { click<MainScreen> ("Settings"); },
                 [this] { return isUp<AdvancedPanel>() && ! isUp<SetupGuidePanel>(); });
            add ("and closing Settings brings it back on the same page",
                 [this] { pressKey (juce::KeyPress::escapeKey); }, [this]
            {
                auto* guide = find<SetupGuidePanel>();
                return ! isUp<AdvancedPanel>() && guide != nullptr && guide->getPage().step == SetupGuideStep::TestTake;
            });
            add ("Done closes the guide", [this] { click<SetupGuidePanel> ("Done"); },
                 [this] { return ! isUp<SetupGuidePanel>(); });
            check ("and it is remembered as done", [this] { return application.isSetupGuideDone(); });
            add ("Settings can run it again", [this] { click<MainScreen> ("Settings"); },
                 [this] { return isUp<AdvancedPanel>(); });
            add ("  (Run setup guide again)", [this] { click<AdvancedPanel> ("Run setup guide again"); }, [this]
            {
                auto* guide = find<SetupGuidePanel>();
                return ! isUp<AdvancedPanel>() && guide != nullptr && guide->getPage().step == SetupGuideStep::Microphones;
            });
            add ("Escape closes the guide", [this] { pressKey (juce::KeyPress::escapeKey); },
                 [this] { return ! isUp<SetupGuidePanel>(); });
        }

        // The crying face, on the real screen with real audio behind it. The
        // virtual microphones play their tones at 0.4 of full scale (-8 dBFS),
        // over the -18 dBFS line, and nothing clips them: every strip must
        // show one tear, and the tears on screen must be the ones its face
        // says it is showing (Tools/sim_channel_meter.cpp covers every other
        // state).
        check ("the two tone microphones each cry one tear at -8 dBFS",
               [this] { return stripsNamed ("mma_mic", ChannelMeterComponent::Face::OneTear) == 2; }, 10000);
        check ("a silent input frowns, with no tear", [this]
        {
            // The fixture's output device also enumerates as an input, and
            // carries nothing: -60 dBFS, a frown. Only checked when present.
            const auto all = strips();
            const int others = (int) all.size() - stripsNamed ("mma_mic");
            return others == 0 || stripsNamed ("mma_out", ChannelMeterComponent::Face::Frown) == others;
        }, 10000);
        add ({}, [this] { line (describeStrips()); }, [] { return true; });
        check ("and the tears on screen match every strip's face", [this] { return facesMatchPixels(); });
        add ({}, [this] { snapshot ("meters-live"); }, [] { return true; });

        // A leftover card from an earlier run must not swallow every click.
        add ("nothing is covering the main screen", [this] { backToMain(); },
             [this] { return ! isUp<RecoveredTakesPanel>() && ! isUp<SavedTakePanel>(); }, 5000, false);

        if (faultMode)
        {
            add ("recording starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            settle (2000);
            add ("a microphone dies mid-take", [this]
            {
                takeFolder = application.getCurrentSessionFolder();
                juce::File (faultFile).replaceWithText ("dead");
            }, [this] { return juce::File (faultFile).existsAsFile(); });
            check ("the mid-take alert card says so", [this] { return isUp<TakeAlertCard>(); }, 30000);
            check ("the dead microphone's face closes its eyes, and its tear stops",
                   [this] { return countStrips (ChannelMeterComponent::Face::Asleep) >= 1
                                && countStrips (ChannelMeterComponent::Face::OneTear) >= 1
                                && facesMatchPixels(); }, 15000);
            check ("the card is flashing its alarm", [this]
            {
                auto* card = find<TakeAlertCard>();
                return card != nullptr && card->isAlarming() && card->getBannerText().contains ("WRONG");
            });
            check ("the siren is on in the headphones", [this] { return application.isFaultAlarmOn(); });
            add ({}, [this] { snapshot ("alarm-card-lit"); }, [] { return true; });
            add ({}, [] {}, [n = std::make_shared<int> (3)] { return --*n <= 0; });
            add ({}, [this] { snapshot ("alarm-card-dark"); }, [] { return true; });
            add ("and it keeps sounding", [this] { sirenSamplesAt = application.getAlarmSamplesRendered(); },
                 [this] { return application.getAlarmSamplesRendered() > sirenSamplesAt + 4800; }, 5000);
            settle (1500);
            add ("it is still sounding a second and a half later", [] {},
                 [this] { return application.isFaultAlarmOn() && application.getAlarmSamplesRendered() > sirenSamplesAt + 48000; });
            // The record button stays enabled for the whole take, and the
            // keyboard can be left on it behind the card: the Mac hands focus
            // back to the last focused control on Cmd+Tab back, or a Tab
            // reaches it. Return there used to click it and stop the take --
            // the one thing the card says no key does.
            add ("Return on the record button behind the card does not stop the take", [this]
            {
                if (auto* b = button<MainScreen> ("Recording. Tap to stop."))
                    b->grabKeyboardFocus();
                pressKeyThroughWindow (juce::KeyPress::returnKey);
            }, [] { return true; });
            settle (1000);
            check ("the take is still running and the card still up",
                   [this] { return recording() && isUp<TakeAlertCard>(); });
            add ("Keep recording closes the card and the take carries on", [this] { click<TakeAlertCard> ("Keep recording"); },
                 [this] { return ! isUp<TakeAlertCard>() && recording(); });
            add ("and the siren stops", [this] { sirenSamplesAt = application.getAlarmSamplesRendered(); },
                 [this] { return ! application.isFaultAlarmOn(); });
            check ("with the card gone, the dead microphone is still asleep and the live one still cries",
                   [this] { return stripsNamed ("mma_mic", ChannelMeterComponent::Face::Asleep) == 1
                                && stripsNamed ("mma_mic", ChannelMeterComponent::Face::OneTear) == 1
                                && facesMatchPixels(); }, 5000);
            add ({}, [this] { line (describeStrips()); snapshot ("meters-mic-dead"); }, [] { return true; });
            settle (500);
            add ("with nothing more rendered after", [] {},
                 [this] { return ! application.isFaultAlarmOn() && application.getAlarmSamplesRendered() <= sirenSamplesAt + 2400; });
            // Plugged back in before Stop (MMA_SHIM_FAIL_WHILE_FILE: a reopen
            // succeeds once the file has gone). The dead stream has already
            // given up, so this take's track stays silent; only a reopen can
            // bring the microphone back for the next one.
            add ("the dead microphone is plugged back into the same port",
                 [this] { juce::File (faultFile).deleteFile(); },
                 [this] { return ! juce::File (faultFile).existsAsFile(); });
            settle (2000);
            add ("the take still stops normally", [this]
            {
                backToMain();
                click<MainScreen> ("Recording. Tap to stop.");
            }, [this] { return ! recording(); }, 20000);
            check ("the saved-take card comes up", [this] { return isUp<SavedTakePanel>(); }, 30000);
            add ("the take's files are on disk", [] {}, [this]
            {
                const juce::File folder (takeFolder);
                return folder.getChildFile ("MIX.wav").existsAsFile() && folder.getChildFile ("session.json").existsAsFile();
            }, 10000);
            add ("the take's record says which microphone stopped, and when", [] {}, [this]
            {
                return juce::File (takeFolder).getChildFile ("session.json").loadFileAsString()
                           .contains ("Microphone stopped sending audio");
            }, 10000);
            add ("the saved-take card's Done closes it", [this] { click<SavedTakePanel> ("Done"); },
                 [this] { return ! isUp<SavedTakePanel>(); });

            // §0.1: nothing used to reopen a stream that died. The device was
            // still listed, so no replug ever came, and the next take recorded
            // silence for that microphone -- with its strip lit as live and no
            // card or dropout to say so. Stop owes the reopen.
            // Clicked with the mouse, the way a person starts one. A click
            // hands the keyboard to what it lands on; left on the record
            // button, a stray Return would stop the take with no question.
            add ("a second take starts, clicked with the mouse", [this]
            {
                backToMain();
                if (auto* b = button<MainScreen> ("Start recording"))
                    clickWithMouse (*b);
            }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            check ("and the click has not left the keyboard on the button that stops it", [this]
            {
                auto* b = button<MainScreen> ("Recording. Tap to stop.");
                return b != nullptr && ! b->hasKeyboardFocus (false);
            });
            add ("nor does switching away and back put it there", [this]
            {
                if (auto* mc = mainComponent())
                    if (auto* peer = mc->getPeer())
                    {
                        peer->handleFocusLoss();
                        peer->handleFocusGain();
                    }
            }, [this]
            {
                auto* b = button<MainScreen> ("Recording. Tap to stop.");
                return b != nullptr && ! b->hasKeyboardFocus (false) && recording();
            });
            add ({}, [this]
            {
                takeFolder = application.getCurrentSessionFolder();
                line ("SECOND-TAKE " + takeFolder);
            }, [] { return true; });
            check ("in the second take the microphone that died is heard again, beside the other",
                   [this] { return stripsNamed ("mma_mic", ChannelMeterComponent::Face::OneTear) == 2
                                && facesMatchPixels(); }, 15000);
            add ({}, [this] { line (describeStrips()); }, [] { return true; });
            settle (4000);
            add ("the second take stops", [this]
            {
                backToMain();
                click<MainScreen> ("Recording. Tap to stop.");
            }, [this] { return ! recording(); }, 20000);
            check ("its saved-take card comes up", [this] { return isUp<SavedTakePanel>(); }, 30000);
            add ("and its record has no microphone stopping in it", [] {}, [this]
            {
                const auto json = juce::File (takeFolder).getChildFile ("session.json");
                return json.existsAsFile() && ! json.loadFileAsString().contains ("Microphone stopped sending audio");
            }, 10000);
            add ("the saved-take card's Done closes it", [this] { click<SavedTakePanel> ("Done"); },
                 [this] { return ! isUp<SavedTakePanel>(); });

            // And one that dies between takes and stays dead. Its failure was
            // dropped outright while nothing was recording: the strip stayed
            // lit as live, and the next take wrote its track as silence with
            // no card and no dropout. It is reopened once, dies again, and
            // must then be shown dead and reported by the take.
            add ("the same microphone dies again, between takes",
                 [this] { juce::File (faultFile).replaceWithText ("dead"); },
                 [this] { return juce::File (faultFile).existsAsFile(); });
            check ("its strip goes to sleep, with nothing recording",
                   [this] { return stripsNamed ("mma_mic2", ChannelMeterComponent::Face::Asleep) == 1
                                && stripsNamed ("mma_mic1", ChannelMeterComponent::Face::OneTear) == 1
                                && facesMatchPixels(); }, 15000);
            add ({}, [this] { line (describeStrips()); }, [] { return true; });
            add ("a third take starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            add ({}, [this] { takeFolder = application.getCurrentSessionFolder(); }, [] { return true; });
            check ("the dead microphone is still asleep in it",
                   [this] { return stripsNamed ("mma_mic2", ChannelMeterComponent::Face::Asleep) == 1; }, 5000);
            settle (2000);
            add ("the third take stops", [this]
            {
                backToMain();
                click<MainScreen> ("Recording. Tap to stop.");
            }, [this] { return ! recording(); }, 20000);
            check ("its saved-take card comes up", [this] { return isUp<SavedTakePanel>(); }, 30000);
            add ("and its record says the microphone was not sending audio", [] {}, [this]
            {
                return juce::File (takeFolder).getChildFile ("session.json").loadFileAsString()
                           .contains ("stopped sending audio");
            }, 10000);
            add ("the saved-take card's Done closes it", [this] { backToMain(); },
                 [this] { return ! isUp<SavedTakePanel>() && ! isUp<TakeAlertCard>(); }, 10000);
            return;
        }

        if (proofMode)
        {
            // §0.1's own stop: a take whose files never grow is ended by the
            // app three seconds in. The red card saying so has to stay the
            // card on screen. The saved-take card used to open over it, and the
            // siren carried on behind a card that read "Saved.".
            add ("a take whose files never grow starts", [this]
            {
                application.setProofStarvedForTesting (true);
                startRecording();
            }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            add ("the app stops it by itself", [] {}, [this] { return ! recording(); }, 15000);
            add ({}, [this] { application.setProofStarvedForTesting (false); }, [] { return true; });
            check ("the red card says the recording failed", [this]
            {
                auto* card = find<TakeAlertCard>();
                return card != nullptr && card->isAlarming() && card->getBannerText() == "RECORDING FAILED";
            });
            check ("and it is the card on top, with no saved-take card over it",
                   [this] { return onTop<TakeAlertCard>() && ! isUp<SavedTakePanel>(); });
            check ("the siren sounds while that card is the one on screen",
                   [this] { return application.isFaultAlarmOn() && onTop<TakeAlertCard>(); }, 5000);
            settle (1500);
            check ("a second and a half later the red card is still the one on screen",
                   [this] { return onTop<TakeAlertCard>() && ! isUp<SavedTakePanel>(); });

            // The stop's banner flashes over everything for three seconds; the
            // picture is of what a person is left looking at once it has gone.
            add ({}, [] {}, [this] { return ! isUp<TakeBanner>(); }, 5000, false);
            add ({}, [this] { snapshot ("proof-stop-card"); }, [] { return true; });
            // Keep recording is hidden on this card, so the keyboard has to
            // land on OK -- not stay behind the card on the record button,
            // where Return would start a new take under it.
            check ("the keyboard is on the red card's OK",
                   [this] { auto* ok = button<TakeAlertCard> ("OK"); return ok != nullptr && ok->hasKeyboardFocus (false); });

            // The card lays itself out in the window's height. It grew the
            // window as it opened, but the window could then be dragged
            // shorter and the card's buttons were squeezed to nothing.
            add ("with the red card up, the window cannot be dragged shorter than the card", [this]
            {
                if (auto* window = mainWindow())
                {
                    windowBoundsBefore = window->getBounds();
                    window->setBoundsConstrained (windowBoundsBefore.withHeight (420));
                }
            }, [this]
            {
                auto* card = find<TakeAlertCard>();
                auto* ok = button<TakeAlertCard> ("OK");
                return card != nullptr && ok != nullptr && ok->getHeight() > 0
                    && card->getHeight() >= card->getRequiredHeight() + 32;
            });
            add ("  (window size put back)", [this]
            {
                if (auto* window = mainWindow())
                    window->setBounds (windowBoundsBefore);
            }, [this] { auto* window = mainWindow(); return window != nullptr && window->getBounds() == windowBoundsBefore; });

            // Nobody can see the window: the meters stop repainting, but the
            // siren is driven by the card, not by painting, and keeps going.
            add ("with the window out of sight, the meters stop repainting and the siren carries on", [this]
            {
                if (auto* window = mainWindow())
                    window->setVisible (false);
            }, [this]
            {
                auto* ms = mainScreenEvenIfHidden();
                return ms != nullptr && ms->areMetersPaused() && application.isFaultAlarmOn();
            });
            add ("back in sight, the meters repaint and the red card is still up, alarming", [this]
            {
                if (auto* window = mainWindow())
                {
                    window->setVisible (true);
                    window->toFront (true);
                }

                // Hiding the window let go of the keyboard; the card's own
                // focus was checked above, and Return below needs it back.
                if (auto* ok = button<TakeAlertCard> ("OK"))
                    ok->grabKeyboardFocus();
            }, [this]
            {
                auto* ms = mainScreenEvenIfHidden();
                auto* card = find<TakeAlertCard>();
                return ms != nullptr && ! ms->areMetersPaused() && card != nullptr && card->isAlarming()
                    && onTop<TakeAlertCard>() && application.isFaultAlarmOn();
            });
            // Xvfb has no window manager to hand a re-mapped window the
            // keyboard back, so it is asked for until the window has it.
            add ("  (keyboard back on the red card's OK)", [] {}, [this]
            {
                auto* ok = button<TakeAlertCard> ("OK");
                if (ok == nullptr)
                    return false;
                if (! ok->hasKeyboardFocus (false))
                {
                    if (auto* window = mainWindow())
                        window->toFront (true);
                    ok->grabKeyboardFocus();
                }
                return ok->hasKeyboardFocus (false);
            }, 5000, false);
            add ("Return answers OK: the red card closes and the siren stops", [this] { pressKeyThroughWindow (juce::KeyPress::returnKey); },
                 [this] { return ! isUp<TakeAlertCard>() && ! application.isFaultAlarmOn(); });
            check ("and no new take started behind it", [this] { return ! recording(); });
            check ("then the saved-take card comes up, on top",
                   [this] { return isUp<SavedTakePanel>() && onTop<SavedTakePanel>(); }, 5000);
            add ("the saved-take card's Done goes back to a quiet main screen",
                 [this] { click<SavedTakePanel> ("Done"); },
                 [this] { return ! isUp<SavedTakePanel>() && ! isUp<TakeAlertCard>() && ! application.isFaultAlarmOn(); });
            check ("with no card up, the window can be made short again", [this]
            {
                auto* window = mainWindow();
                auto* constrainer = window != nullptr ? window->getConstrainer() : nullptr;
                return constrainer != nullptr && constrainer->getMinimumHeight() == 420;
            });
            return;
        }

        if (crashMode)
        {
            add ("recording starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            add ("the take has a folder", [] {}, [this] { return application.getCurrentSessionFolder().isNotEmpty(); }, 10000);
            add ("the take is running", [this] { line ("RECORDING " + application.getCurrentSessionFolder()); },
                 [] { return false; }, 600000);
            return;
        }

        // Main screen controls.
        add ("the monitor mute button mutes", [this] { if (auto* b = muteButton()) b->triggerClick(); },
             [this] { return muted() && busMuted(); });
        add ("and unmutes", [this] { if (auto* b = muteButton()) b->triggerClick(); },
             [this] { return ! muted() && ! busMuted(); });
        add ("the space bar mutes the monitor", [this] { pressKey (juce::KeyPress::spaceKey); },
             [this] { return muted() && busMuted(); });
        add ("and space again unmutes it", [this] { pressKey (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted(); });

        // A buffer or rate change builds a new monitor bus (§2.2, §5.4). The
        // room was muted for a reason; the rebuild must not quietly unmute it.
        add ("muted again, before the audio engine rebuilds", [this] { pressKey (juce::KeyPress::spaceKey); },
             [this] { return muted() && busMuted(); });
        add ("a buffer-size change rebuilds the audio engine", [this]
        {
            generationBefore = application.getCaptureGeneration();
            bufferOverrideBefore = application.getBufferSizeOverride();
            application.setBufferSizeOverride (bufferOverrideBefore == 512 ? 1024 : 512);
        }, [this] { return application.getCaptureGeneration() != generationBefore; }, 20000);
        add ("the headphones are still muted after the rebuild", [] {},
             [this] { return busMuted() && muted(); }, 5000);
        add ("space still unmutes them", [this] { pressKey (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted(); });
        add ("the buffer size goes back", [this]
        {
            generationBefore = application.getCaptureGeneration();
            application.setBufferSizeOverride (bufferOverrideBefore);
        }, [this] { return application.getCaptureGeneration() != generationBefore
                        && application.getBufferSizeOverride() == bufferOverrideBefore; }, 20000);
        add ("and stays unmuted through that rebuild", [] {}, [this] { return ! muted() && ! busMuted(); }, 5000);

        // §5: a runaway cut stays until someone presses Unmute. The cut lives
        // on the bus, and a rebuild nobody asked for -- a buffer-ladder step,
        // a hot-plug that moves the rate, the restart a take deferred -- builds
        // a fresh one. The feedback detector latches the same cut the limiter
        // does, and one grown band trips it without a howl in the fixture.
        add ("feedback cuts the headphones", [this]
        {
            if (auto* bus = application.getMonitorBus())
                bus->processFeedbackCandidate (-20.0, -20.0, MonitorBus::kFeedbackWindowSeconds);
        }, [this] { return busCut() && button<MainScreen> ("Unmute (sound was cut)") != nullptr; });
        add ("the audio engine rebuilds while they are cut", [this]
        {
            generationBefore = application.getCaptureGeneration();
            application.setBufferSizeOverride (bufferOverrideBefore == 512 ? 1024 : 512);
        }, [this] { return application.getCaptureGeneration() != generationBefore; }, 20000);
        check ("the headphones are still cut after the rebuild", [this]
        {
            return busCut() && busMuted() && button<MainScreen> ("Unmute (sound was cut)") != nullptr;
        });
        add ("the cut's Unmute brings the sound back", [this] { click<MainScreen> ("Unmute (sound was cut)"); },
             [this] { return ! busCut() && ! busMuted() && ! muted(); });
        add ("the buffer size goes back after the cut", [this]
        {
            generationBefore = application.getCaptureGeneration();
            application.setBufferSizeOverride (bufferOverrideBefore);
        }, [this] { return application.getCaptureGeneration() != generationBefore
                        && application.getBufferSizeOverride() == bufferOverrideBefore; }, 20000);
        add ("and the sound stays on through that rebuild", [] {},
             [this] { return ! busCut() && ! busMuted() && ! muted(); }, 5000);
        add ("the main screen's controls", [this] { insertNext (exerciseControlsIn<MainScreen> ("main screen")); }, [] { return true; });

        add ("the session name can be typed", [this]
        {
            if (auto* editor = find<juce::TextEditor>())
            {
                editor->setText ("UI walk", true);
                editor->grabKeyboardFocus();
            }
        }, [this] { auto* ms = find<MainScreen>(); return ms != nullptr && ms->getSessionName() == "UI walk"; });

        // Type the name, press Return, press Space to mute: the box kept the
        // keyboard after Return, so the space went into the take's name and
        // the room stayed live.
        add ("Return in the name box hands the keyboard back to the window", [this]
        {
            pressKeyThroughWindow (juce::KeyPress::returnKey);
        }, [this]
        {
            auto* editor = find<juce::TextEditor>();
            return editor != nullptr && ! editor->hasKeyboardFocus (true);
        });
        add ("so the space bar then mutes the room, not types into the name",
             [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this]
             {
                 auto* ms = find<MainScreen>();
                 return muted() && busMuted() && ms != nullptr && ms->getSessionName() == "UI walk";
             });
        add ("  (and unmutes it)", [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted(); });
        // The same after a click on something that does not take the keyboard
        // itself: JUCE left the focus in the box.
        add ("a click on a heading also ends typing in the name box", [this]
        {
            if (auto* editor = find<juce::TextEditor>())
                editor->grabKeyboardFocus();

            for (auto* c : showing())
                if (auto* l = dynamic_cast<juce::Label*> (c); l != nullptr && l->getText() == "M I C R O P H O N E S")
                {
                    clickWithMouse (*l);
                    break;
                }
        }, [this]
        {
            auto* editor = find<juce::TextEditor>();
            return editor != nullptr && ! editor->hasKeyboardFocus (true);
        });
        add ("  and the space bar mutes", [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this]
             {
                 auto* ms = find<MainScreen>();
                 return muted() && busMuted() && ms != nullptr && ms->getSessionName() == "UI walk";
             });
        add ("  (and unmutes it)", [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted(); });

        // Rename microphone 1, and check the name reaches its file later.
        add ("clicking a microphone's name asks for a new one", [this]
        {
            if (auto* ms = find<MainScreen>())
                if (auto* meter = ms->getChannelMeter (0); meter != nullptr && meter->onNameClicked)
                    meter->onNameClicked();
        }, [this] { return find<juce::AlertWindow>() != nullptr; });
        add ("typing a name and pressing Save renames it", [this]
        {
            if (auto* alert = find<juce::AlertWindow>())
            {
                if (auto* editor = alert->getTextEditor ("name"))
                    editor->setText ("Walker Vox");
                click<juce::AlertWindow> ("Save");
            }
        }, [this] { return find<juce::AlertWindow>() == nullptr && application.getMicDisplayName (0).contains ("Walker"); });

        // A strip is clicked with the mouse far more often than it is tabbed
        // to. The click must not keep the keyboard afterwards: Space is the
        // room's mute, and a strip holding the focus took it to clear a clip
        // or reopen the rename dialog instead.
        add ("a strip that has clipped", [this]
        {
            if (auto* mc = mainComponent())
                mc->grabKeyboardFocus();
            if (auto* metering = application.getChannelMetering (1))
                metering->pushBlockStats (1.0f, 512);
        }, [this]
        {
            auto* ms = find<MainScreen>();
            auto* meter = ms != nullptr ? ms->getChannelMeter (1) : nullptr;
            return meter != nullptr && meter->getDescription().contains ("Clipping");
        });
        add ("clicking it with the mouse clears the clip", [this]
        {
            if (auto* ms = find<MainScreen>())
                if (auto* meter = ms->getChannelMeter (1))
                    clickWithMouse (*meter);
        }, [this]
        {
            auto* metering = application.getChannelMetering (1);
            return metering != nullptr && ! metering->isClipped() && find<juce::AlertWindow>() == nullptr;
        });
        add ("then the space bar still mutes the monitor",
             [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this] { return muted() && busMuted() && find<juce::AlertWindow>() == nullptr; });
        add ("  (and unmutes it)", [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted() && find<juce::AlertWindow>() == nullptr; });
        add ({}, [this]
        {
            click<juce::AlertWindow> ("Cancel");
            if (muted())
                if (auto* b = muteButton())
                    b->triggerClick();
        }, [] { return true; });
        add ("clicking a strip with the mouse opens its rename dialog", [this]
        {
            if (auto* mc = mainComponent())
                mc->grabKeyboardFocus();
            if (auto* ms = find<MainScreen>())
                if (auto* meter = ms->getChannelMeter (0))
                    clickWithMouse (*meter);
        }, [this] { return find<juce::AlertWindow>() != nullptr; });
        add ("Cancel closes it", [this] { click<juce::AlertWindow> ("Cancel"); },
             [this] { return find<juce::AlertWindow>() == nullptr; });
        add ("after clicking a strip, the space bar still mutes the monitor",
             [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this] { return muted() && busMuted() && find<juce::AlertWindow>() == nullptr; });
        add ("  (and unmutes it)", [this] { pressKeyThroughWindow (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted() && find<juce::AlertWindow>() == nullptr; });
        add ({}, [this]
        {
            // So a failure above does not leave a dialog or a mute in the way
            // of everything after it.
            click<juce::AlertWindow> ("Cancel");
            if (muted())
                if (auto* b = muteButton())
                    b->triggerClick();
        }, [] { return true; });

        // Launching SobStage again while it is minimised must bring the one
        // window back: only one instance is allowed, so the second launch is
        // handed to this one, and doing nothing with it looked like the app
        // had failed to start. Xvfb has no window manager, so a minimise is
        // never observable there (JUCE reads it back from _NET_WM_STATE); the
        // window is also hidden outright, which is, and asserted on.
        add ("a second launch brings a minimised window back", [this]
        {
            if (auto* window = mainWindow())
            {
                window->setMinimised (true);
                line ("      (minimise observable here: " + juce::String (window->isMinimised() ? "yes" : "no") + ")");
                window->setVisible (false);
            }

            if (auto* app = juce::JUCEApplication::getInstance())
                app->anotherInstanceStarted ({});
        }, [this]
        {
            auto* window = mainWindow();
            return window != nullptr && window->isVisible() && window->isShowing() && ! window->isMinimised();
        });
        add ("  (window put back)", [this]
        {
            if (auto* window = mainWindow())
                window->setVisible (true);
        }, [this] { auto* window = mainWindow(); return window != nullptr && window->isShowing(); }, 3000, false);

        // Settings drawer.
        add ("Settings opens", [this] { click<MainScreen> ("Settings"); }, [this] { return isUp<AdvancedPanel>(); });
        add ("with Settings open, a resized window's cards still cover all of it", [this]
        {
            if (auto* mc = mainComponent())
                if (auto* window = mc->getTopLevelComponent())
                {
                    windowBoundsBefore = window->getBounds();
                    window->setSize (window->getWidth() + 40, window->getHeight() + 20);
                }
        }, [this] { return overlaysCoverWindow(); });
        add ("  (window size put back)", [this]
        {
            if (auto* mc = mainComponent())
                if (auto* window = mc->getTopLevelComponent())
                    window->setBounds (windowBoundsBefore);
        }, [this] { return overlaysCoverWindow(); });
        // The window's own minimum is 560px and the drawer never goes below
        // 380, so dragging the window narrow with Settings open squeezed the
        // main screen to ~180px: record button off its edge, header buttons
        // on top of each other. The limit rises while a drawer is open.
        add ("with Settings open, the window cannot be dragged narrower than the main screen needs", [this]
        {
            if (auto* window = mainWindow())
            {
                windowBoundsBefore = window->getBounds();
                window->setBoundsConstrained (windowBoundsBefore.withWidth (560));
            }
        }, [this]
        {
            auto* ms = find<MainScreen>();
            return ms != nullptr && ms->getWidth() >= 600;
        });
        add ("  (window size put back)", [this]
        {
            if (auto* window = mainWindow())
                window->setBounds (windowBoundsBefore);
        }, [this] { auto* window = mainWindow(); return window != nullptr && window->getBounds() == windowBoundsBefore; });
        // §4: the trim rows were rebuilt only when the NUMBER of microphones
        // changed, so a rename left the old name on its row -- and a swap left
        // a slider labelled for one microphone driving another.
        add ("a microphone renamed with Settings open gets its own trim row", [this]
        {
            application.setMicAssignedName (0, "Walker Trim");
        }, [this]
        {
            const auto stored = application.getMicDisplayName (0);
            return stored.contains ("Trim") && trimSlider (stored + " monitor trim") != nullptr;
        }, 5000);
        add ("and a trim set elsewhere shows on that row", [this] { application.setChannelTrimDb (0, 3.5f); }, [this]
        {
            auto* s = trimSlider (application.getMicDisplayName (0) + " monitor trim");
            return s != nullptr && std::abs (s->getValue() - 3.5) < 0.01;
        }, 5000);
        add ("  (name and trim put back)", [this]
        {
            application.setChannelTrimDb (0, 0.0f);
            application.setMicAssignedName (0, "Walker Vox");
        }, [this] { return application.getMicDisplayName (0).contains ("Vox")
                        && trimSlider (application.getMicDisplayName (0) + " monitor trim") != nullptr; }, 5000);
        add ("every control in Settings", [this] { insertNext (exerciseControlsIn<AdvancedPanel> ("Settings")); }, [] { return true; });
        add ("Export diagnostics in Settings writes a zip", [this]
        {
            diagnosticsZipsBefore = countDiagnosticsZips();
            click<AdvancedPanel> ("Export diagnostics");
        }, [this] { return countDiagnosticsZips() > diagnosticsZipsBefore && newestDiagnosticsZipIsReal(); }, 20000);
        add ("Help opens from Settings", [this] { click<AdvancedPanel> ("Help"); }, [this] { return isUp<HelpPanel>(); });
        add ("Help's Open Settings goes back to Settings", [this] { click<HelpPanel> ("Open Settings"); },
             [this] { return isUp<AdvancedPanel>(); });
        add ("Settings' Close closes it", [this] { click<AdvancedPanel> ("Close"); },
             [this] { return ! isUp<AdvancedPanel>(); });
        check ("and the window can be made narrow again once it is closed", [this]
        {
            auto* window = mainWindow();
            auto* constrainer = window != nullptr ? window->getConstrainer() : nullptr;
            return constrainer != nullptr && constrainer->getMinimumWidth() == 560;
        });
        add ("Escape closes Settings too", [this] { click<MainScreen> ("Settings"); },
             [this] { return isUp<AdvancedPanel>(); });
        add ("  (Escape)", [this] { pressKey (juce::KeyPress::escapeKey); }, [this] { return ! isUp<AdvancedPanel>(); });

        // Help drawer.
        add ("Help opens from the main screen", [this] { click<MainScreen> ("Help"); }, [this] { return isUp<HelpPanel>(); });
        add ("Export diagnostics in Help writes another zip", [this]
        {
            diagnosticsZipsBefore = countDiagnosticsZips();
            click<HelpPanel> ("Export diagnostics");
        }, [this] { return countDiagnosticsZips() > diagnosticsZipsBefore; }, 20000);
        add ("Help's Close closes it", [this] { click<HelpPanel> ("Close"); }, [this] { return ! isUp<HelpPanel>(); });
        add ("Help can run the setup guide again", [this] { click<MainScreen> ("Help"); }, [this] { return isUp<HelpPanel>(); });
        add ("  (Run setup guide again)", [this] { click<HelpPanel> ("Run setup guide again"); },
             [this] { return isUp<SetupGuidePanel>() && ! isUp<HelpPanel>(); });
        add ("and its Skip closes it", [this] { click<SetupGuidePanel> ("Skip"); },
             [this] { return ! isUp<SetupGuidePanel>(); });

        // Cameras.
        add ("Cameras opens", [this] { click<MainScreen> ("Cameras"); }, [this] { return isUp<CameraPanel>(); });
        add ("every control on the Cameras screen", [this] { insertNext (exerciseControlsIn<CameraPanel> ("Cameras")); }, [] { return true; });
        add ("Cameras' Done goes back", [this] { click<CameraPanel> ("< Done"); }, [this] { return ! isUp<CameraPanel>(); });

        // A take.
        add ("recording starts", [this]
        {
            alarmSamplesBefore = application.getAlarmSamplesRendered();
            startRecording();
        }, [this] { return recordingOrAnswerPrompt(); }, 90000);
        add ("the whole window says RECORDING", [] {}, [this] { return bannerSays (TakeBanner::Kind::Started); });
        add ({}, [this] { snapshot ("banner-recording"); }, [] { return true; });
        add ("and the headphones chirp", [] {}, [this] { return application.getAlarmSamplesRendered() > alarmSamplesBefore; }, 5000);
        add ("the button says it is recording", [] {}, [this]
        {
            return button<MainScreen> ("Recording. Tap to stop.") != nullptr;
        });
        settle (1500);
        add ("Settings opens during a take", [this] { click<MainScreen> ("Settings"); }, [this] { return isUp<AdvancedPanel>(); });
        add ("changing the buffer size during a take", [this]
        {
            // §5.4: a mid-take buffer change restarts capture. The take must
            // survive it.
            if (auto* panel = find<AdvancedPanel>())
            {
                std::vector<juce::Component*> inside;
                collect (*panel, inside);
                for (auto* c : inside)
                    if (auto* cb = dynamic_cast<juce::ComboBox*> (c); cb != nullptr && cb->getText().containsIgnoreCase ("sample")
                                                                       && cb->isEnabled() && cb->getNumItems() > 1)
                    {
                        bufferOverrideBefore = application.getBufferSizeOverride();
                        cb->setSelectedItemIndex ((cb->getSelectedItemIndex() + 1) % cb->getNumItems(), juce::sendNotificationSync);
                        break;
                    }
            }
        }, [this]
        {
            // The picker must have been found, and the choice must have landed.
            return recording() && bufferOverrideBefore != -12345
                && application.getBufferSizeOverride() != bufferOverrideBefore;
        });
        settle (2000);
        add ("the take is still running after it", [] {}, [this] { return recording(); });

        // Settings does not stop anyone picking a new bit depth or rate mid-take,
        // and both are for the NEXT take: this one's files are already open at
        // the old format. session.json is rewritten at stop, and it has to
        // describe those files, not whatever the setting says by then.
        add ("a new bit depth and rate can be picked mid-take, for the next take", [this]
        {
            bitDepthBefore = application.getBitDepth();
            rateOverrideBefore = application.getSampleRateOverride();
            midTakeBitDepth = bitDepthBefore == 16 ? 24 : 16;
            midTakeRate = juce::roundToInt (application.getSampleRate()) == 44100 ? 48000u : 44100u;
            application.setBitDepthOverride (midTakeBitDepth);
            application.setSampleRateOverride (midTakeRate);
        }, [this]
        {
            return recording() && application.getBitDepth() == midTakeBitDepth
                && juce::roundToInt (application.getSampleRate()) == static_cast<int> (midTakeRate);
        });
        settle (1000);
        add ("the take is still running after that too", [] {}, [this] { return recording(); });
        add ("Settings closes during a take", [this] { pressKey (juce::KeyPress::escapeKey); },
             [this] { return ! isUp<AdvancedPanel>(); });
        settle (2000);

        // Renaming during a take: this take's files keep the name they started
        // with (§6.5 fixes the channel list), but the strip must show the new
        // name straight away. It used to wait for the take to end.
        for (const char* name : { "Walker Mid", "Walker Vox" })
        {
            const juce::String newName (name);
            add ("renaming a microphone during the take asks for the name", [this]
            {
                if (auto* ms = find<MainScreen>())
                    if (auto* meter = ms->getChannelMeter (0); meter != nullptr && meter->onNameClicked)
                        meter->onNameClicked();
            }, [this] { return find<juce::AlertWindow>() != nullptr; });
            add ("and its strip shows \"" + newName + "\" at once, mid-take", [this, newName]
            {
                if (auto* alert = find<juce::AlertWindow>())
                {
                    if (auto* editor = alert->getTextEditor ("name"))
                        editor->setText (newName);
                    click<juce::AlertWindow> ("Save");
                }
            }, [this, newName]
            {
                auto* ms = find<MainScreen>();
                auto* meter = ms != nullptr ? ms->getChannelMeter (0) : nullptr;
                // Names are stored filename-safe ("Walker Mid" -> "Walker-Mid"),
                // so the strip is matched against the name the app now holds.
                const auto stored = application.getMicDisplayName (0);
                return recording() && find<juce::AlertWindow>() == nullptr
                    && stored.contains (newName.fromLastOccurrenceOf (" ", false, false))
                    && meter != nullptr && meter->getTitle() == stored + " meter";
            }, 2000);
        }
        // §6.3: unticking the local backup mid-take. It used to change only the
        // setting: the copy went on being written, the low-space stop no
        // longer watched it, and session.json said the backup was off while
        // the take's own record showed it running to the end.
        add ("unticking the local backup mid-take stops this take's copy", [this]
        {
            takeFolder = application.getCurrentSessionFolder();
            application.setMirrorEnabled (false);
        }, [this] { return recording() && ! application.isMirrorEnabledByUser(); });
        settle (1000);
        add ("recording stops", [this]
        {
            takeFolder = application.getCurrentSessionFolder();
            alarmSamplesBefore = application.getAlarmSamplesRendered();
            click<MainScreen> ("Recording. Tap to stop.");
        }, [this] { return ! recording(); }, 20000);
        add ("the whole window says RECORDING STOPPED", [] {}, [this] { return bannerSays (TakeBanner::Kind::Stopped); });
        add ({}, [this] { snapshot ("banner-stopped"); }, [] { return true; });
        add ("and the headphones chirp again", [] {}, [this] { return application.getAlarmSamplesRendered() > alarmSamplesBefore; }, 5000);
        add ("the banner goes away on its own", [] {}, [this] { return banner() == nullptr; }, TakeBanner::kHoldMs + 2000);
        check ("the saved-take card says the take is saved", [this] { return isUp<SavedTakePanel>(); }, 30000);
        add ("the take's files are on disk, with the renamed microphone", [] {}, [this] { return takeFilesLookRight(); }, 10000);
        add ("its record says the backup copy stopped when it was turned off", [] {}, [this]
        {
            const auto json = juce::JSON::parse (juce::File (takeFolder).getChildFile ("session.json"));
            return json.isObject() && ! static_cast<bool> (json["mirrorActive"])
                && juce::JSON::toString (json).contains ("Local backup copy turned off");
        }, 10000);
        add ("  (local backup ticked again for the next take)", [this] { application.setMirrorEnabled (true); },
             [this] { return application.isMirrorEnabledByUser(); });
        add ("session.json gives the rate and bit depth MIX.wav was written at, not the ones picked mid-take",
             [] {}, [this]
        {
            const auto f = takeFormats();
            return f.has_value() && f->jsonBits == f->wavBits && std::abs (f->jsonRate - f->wavRate) < 0.5;
        }, 10000);
        add ({}, [this]
        {
            if (const auto f = takeFormats())
                line ("      session.json " + juce::String (f->jsonRate, 0) + " Hz " + juce::String (f->jsonBits)
                      + "-bit; MIX.wav " + juce::String (f->wavRate, 0) + " Hz " + juce::String (f->wavBits) + "-bit");
        }, [] { return true; });
        add ("the bit depth and rate go back to what they were", [this]
        {
            application.setBitDepthOverride (bitDepthBefore);
            application.setSampleRateOverride (rateOverrideBefore);
        }, [this]
        {
            return application.getBitDepth() == bitDepthBefore
                && application.getSampleRateOverride() == rateOverrideBefore;
        }, 20000);
        add ("the saved-take card's Done closes it", [this] { click<SavedTakePanel> ("Done"); },
             [this] { return ! isUp<SavedTakePanel>(); });

        // A second, shorter take that changes the buffer size and nothing
        // else. §5.4 fixes the size for the length of a take, so a size picked
        // in Settings mid-take is owed to the stop, and Stop has to pay it.
        // The take above cannot show that: its mid-take rename owes a reopen
        // of its own, and paying that one hid a buffer change that was never
        // owed.
        check ("before the second take, the streams run at the buffer size Settings shows",
               [this] { return application.getOpenBufferSize() == application.getCurrentBufferSize(); }, 10000);
        add ("a second take starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
        settle (2000);
        add ("the buffer size is changed during the second take, and nothing reopens yet", [this]
        {
            generationBefore = application.getCaptureGeneration();
            bufferOverrideBefore = application.getBufferSizeOverride();
            application.setBufferSizeOverride (application.getOpenBufferSize() == 512 ? 1024 : 512);
        }, [this]
        {
            return recording() && application.getBufferSizeOverride() != bufferOverrideBefore
                && application.getCaptureGeneration() == generationBefore;
        });
        settle (3000);

        add ("the second take stops", [this]
        {
            backToMain();
            click<MainScreen> ("Recording. Tap to stop.");
        }, [this] { return ! recording(); }, 20000);
        add ("after Stop, the streams reopen at the buffer size picked mid-take", [] {}, [this]
        {
            return application.getCaptureGeneration() != generationBefore
                && application.getOpenBufferSize() == application.getCurrentBufferSize();
        }, 20000);
        add ({}, [] {}, [this] { return banner() == nullptr; }, TakeBanner::kHoldMs + 2000);
        check ("the second take's saved-take card comes up", [this] { return isUp<SavedTakePanel>(); }, 30000);

        // "Quit When Done" says the app is waiting on the line under the
        // record button -- which this card covers, and this card is what is
        // up when the quit starts waiting: "Stop and Quit" opens it behind the
        // "Still saving" question, and Cmd-Q from it leaves it up. The card
        // has to say it too, where it can be read, and stop when the wait
        // ends. Set the way Main.cpp sets it; the walk never quits mid-way.
        add ("a quit waiting for the combined video says so on the saved-take card, in front", [this]
        {
            if (auto* mc = dynamic_cast<MainComponent*> (mainComponent()))
                mc->setQuitPendingNote (juce::String (QuitQuestions::pendingQuitLine ("the combined video")));
        }, [this]
        {
            return onTop<SavedTakePanel>()
                && lineInFront<SavedTakePanel> (juce::String (QuitQuestions::pendingQuitLine ("the combined video")));
        });
        add ({}, [this] { snapshot ("saved-take-card-quit-pending"); }, [] { return true; });
        add ("and the card's line goes when the wait ends", [this]
        {
            if (auto* mc = dynamic_cast<MainComponent*> (mainComponent()))
                mc->setQuitPendingNote ({});
        }, [this]
        {
            return isUp<SavedTakePanel>()
                && ! lineInFront<SavedTakePanel> (juce::String (QuitQuestions::pendingQuitLine ("the combined video")));
        });

        add ("and its Done closes it", [this] { click<SavedTakePanel> ("Done"); },
             [this] { return ! isUp<SavedTakePanel>(); });

        // AlarmSpeaker.h: the computer's default output and the monitor output
        // are never open together. The fixture's only monitor candidate is its
        // shared "default", which is refused for monitoring, so the takes'
        // chirps went to the default output instead and left it open.
        // Choosing the headphone output again -- what a user does in Settings
        // -- must let go of it BEFORE the coordinator opens the output. On a
        // real card the default output (dmix, PipeWire; WASAPI shared mode)
        // holds the very device the exclusive open needs, the open is refused
        // as "in use", monitoring falls back to input-only, and nothing ever
        // closes the speaker again. Here the monitor still cannot open, so the
        // speaker is given straight back; the handover count is what shows it
        // was let go first.
        add ("the takes' chirps left the computer's own output open", [] {},
             [this] { return application.isAlarmSpeakerOpen()
                          && ! application.getSelectedOutputDeviceName().empty(); }, 5000);
        add ("choosing the headphone output again lets go of it before opening the output", [this]
        {
            speakerHandoversBefore = application.getSpeakerHandoversForMonitorOpen();
            application.setOutputDeviceByName (juce::String (application.getSelectedOutputDeviceName()));
        }, [this] { return application.getSpeakerHandoversForMonitorOpen() > speakerHandoversBefore; }, 2000);
        add ("and with no monitor stream, the default output is given straight back", [] {},
             [this] { return application.isAlarmSpeakerOpen(); }, 2000);
        add ("and the next chirp still finds an output at once", [this]
        {
            alarmSamplesBefore = application.getAlarmSamplesRendered();
            application.announceRecordingStopped();
        }, [this] { return application.isAlarmAudible()
                        && application.getAlarmSamplesRendered() > alarmSamplesBefore; }, 5000);

        // Anything with a label nobody above pressed: press it, then clear up.
        add ("every other button on the main screen", [this] { insertNext (pressEverythingElse()); }, [] { return true; });

        // A choice made in Settings is on disk.
        add ("a Settings choice is saved to disk", [] {}, [] {
            return Application::getSettingsFile().existsAsFile() && Application::getSettingsFile().getSize() > 0;
        }, 10000);
    }

    void startRecording()
    {
        backToMain();
        click<MainScreen> ("Start recording");
    }

    /// Polled while waiting for a take to start: a fresh profile asks where
    /// recordings go first, and that card can take a moment to appear.
    bool recordingOrAnswerPrompt()
    {
        if (recording())
            return true;

        if (isUp<SaveLocationPrompt>())
        {
            click<SaveLocationPrompt> ("Start recording");
            return false;
        }

        // At launch the button stays disabled while each save location is
        // checked, which can take half a minute. Keep pressing once it is ready.
        if (++ticksSinceRecordPress >= 20)
        {
            ticksSinceRecordPress = 0;
            click<MainScreen> ("Start recording");
        }

        return false;
    }

    int ticksSinceRecordPress = 0;
    uint64_t alarmSamplesBefore = 0;
    juce::String snapshotDir;

    /// A PNG of the whole window as it is right now, when a directory was
    /// given. Rendered through the same paint calls the screen gets.
    void snapshot (const juce::String& name)
    {
        if (snapshotDir.isEmpty())
            return;

        auto* top = root.getTopLevelComponent();
        if (top == nullptr)
            return;

        const auto image = top->createComponentSnapshot (top->getLocalBounds());
        const juce::File file = juce::File (snapshotDir).getChildFile (name + ".png");
        file.getParentDirectory().createDirectory();
        juce::FileOutputStream out (file);
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            juce::PNGImageFormat().writeImageToStream (image, out);
        }
    }
    uint64_t sirenSamplesAt = 0;

    /// Every channel strip on the main screen.
    std::vector<ChannelMeterComponent*> strips() const
    {
        std::vector<ChannelMeterComponent*> all;
        if (auto* ms = find<MainScreen>())
            for (int i = 0; i < ms->getMicCount(); ++i)
                if (auto* m = ms->getChannelMeter (i); m != nullptr && m->isShowing())
                    all.push_back (m);
        return all;
    }

    /// One line per strip -- name, face, what a screen reader hears, tear
    /// pixels -- so a failed face check says what the screen showed.
    juce::String describeStrips() const
    {
        static const char* names[] = { "asleep", "frown", "one tear", "sob" };
        juce::String out;
        for (auto* m : strips())
        {
            const auto p = MeterFaceProbe::of (*m);
            out << "  strip \"" << m->getTitle() << "\": " << names[(int) m->getFace()]
                << ", tears " << p.tearLeft << "/" << p.tearRight << ", \"" << m->getDescription() << "\"\n";
        }
        return out.trimEnd();
    }

    int countStrips (ChannelMeterComponent::Face face) const
    {
        int n = 0;
        for (auto* m : strips())
            n += m->getFace() == face ? 1 : 0;
        return n;
    }

    /// Strips whose microphone name starts with prefix -- showing face, when
    /// one is given.
    int stripsNamed (const juce::String& prefix,
                     std::optional<ChannelMeterComponent::Face> face = std::nullopt) const
    {
        int n = 0;
        for (auto* m : strips())
            if (m->getTitle().startsWith (prefix) && (! face || m->getFace() == *face))
                ++n;
        return n;
    }

    /// Reads each strip's badge back from its own pixels: no tear for asleep
    /// and frown, the left one for one tear, both for a sob.
    bool facesMatchPixels() const
    {
        using Face = ChannelMeterComponent::Face;
        const auto all = strips();
        if (all.empty())
            return false;

        for (auto* m : all)
        {
            const auto p = MeterFaceProbe::of (*m);
            const auto face = m->getFace();
            const bool ok = (face == Face::Asleep || face == Face::Frown)
                                ? p.tears() < MeterFaceProbe::kTearMinPixels
                          : face == Face::OneTear ? (p.tearOnLeft() && ! p.tearOnRight())
                                                  : (p.tearOnLeft() && p.tearOnRight());
            if (! ok)
                return false;
        }
        return true;
    }

    /// The take banner, if it is up right now.
    TakeBanner* banner() const { return find<TakeBanner>(); }

    bool bannerSays (TakeBanner::Kind kind) const
    {
        auto* b = banner();
        return b != nullptr && b->getKind() == kind;
    }
    int bufferOverrideBefore = -12345;
    int generationBefore = 0;
    int speakerHandoversBefore = 0;
    int bitDepthBefore = 24;
    uint32_t rateOverrideBefore = 0;
    int midTakeBitDepth = 0;
    uint32_t midTakeRate = 0;

    /// What session.json says the take was recorded at, beside what MIX.wav's
    /// own header says. Returns nothing until the rewrite at stop is on disk:
    /// the copy written at the start has no stop time and would pass for the
    /// wrong reason.
    struct TakeFormats
    {
        double jsonRate = 0.0, wavRate = 0.0;
        int jsonBits = 0, wavBits = 0;
    };

    std::optional<TakeFormats> takeFormats() const
    {
        if (takeFolder.isEmpty())
            return std::nullopt;

        const juce::File folder (takeFolder);
        const auto json = juce::JSON::parse (folder.getChildFile ("session.json"));
        if (! json.isObject() || json["stopTimestamp"].toString().isEmpty())
            return std::nullopt;

        auto stream = folder.getChildFile ("MIX.wav").createInputStream();
        if (stream == nullptr)
            return std::nullopt;

        std::unique_ptr<juce::AudioFormatReader> mix (juce::WavAudioFormat().createReaderFor (stream.release(), true));
        if (mix == nullptr)
            return std::nullopt;

        TakeFormats f;
        f.jsonRate = static_cast<double> (json["sampleRate"]);
        f.jsonBits = static_cast<int> (json["bitDepth"]);
        f.wavRate = mix->sampleRate;
        f.wavBits = static_cast<int> (mix->bitsPerSample);
        return f;
    }

    bool takeFilesLookRight()
    {
        if (takeFolder.isEmpty())
            return false;

        const juce::File folder (takeFolder);
        const auto wavs = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
        bool hasMix = false, hasRenamed = false;

        for (const auto& w : wavs)
        {
            hasMix = hasMix || w.getFileName() == "MIX.wav";
            hasRenamed = hasRenamed || w.getFileName().contains ("Walker");
        }

        return hasMix && hasRenamed && wavs.size() >= 2
            && folder.getChildFile ("session.json").existsAsFile();
    }

    std::vector<Step> pressEverythingElse()
    {
        // Buttons that hand off to the operating system -- a folder picker, a
        // file manager window -- have nothing in this process to verify.
        static const std::set<juce::String> leaveAlone {
            "Change...", "Choose a different folder...", "Open the folder",
            "Start recording", "Recording. Tap to stop."
        };

        std::vector<Step> more;
        for (auto* c : showing())
            if (auto* b = dynamic_cast<juce::Button*> (c))
            {
                const auto text = b->getButtonText();
                if (text.isEmpty() || leaveAlone.count (text) != 0 || exercised.count (text) != 0
                    || dynamic_cast<juce::ToggleButton*> (b) != nullptr || ! b->isEnabled())
                    continue;

                exercised.insert (text);
                juce::Component::SafePointer<juce::Component> safe (b);
                more.push_back ({ "\"" + text + "\" does not break anything",
                                  [safe] { if (auto* btn = dynamic_cast<juce::Button*> (safe.getComponent())) btn->triggerClick(); },
                                  [this] { return find<MainScreen>() != nullptr || isUp<CameraPanel>(); } });
                more.push_back ({ {}, [this] { backToMain(); }, [n = std::make_shared<int> (5)] { return --*n <= 0; }, 2000, false });
            }

        return more;
    }

    // --- the loop ---------------------------------------------------------

    void timerCallback() override
    {
        if (finished)
            return;

        if (! current.has_value())
        {
            if (steps.empty())
            {
                finish();
                return;
            }

            // Taken off the queue before it runs, so steps it adds with
            // insertNext() come straight after it rather than ahead of it.
            current = std::move (steps.front());
            steps.pop_front();
            stepElapsedMs = 0;
            current->action();
        }

        stepElapsedMs += kTickMs;

        if (current->until())
        {
            if (current->what.isNotEmpty())
                pass (current->what);
            current.reset();
        }
        else if (stepElapsedMs >= current->timeoutMs)
        {
            if (current->what.isNotEmpty())
            {
                if (current->required)
                    fail (current->what);
                else
                    line ("SKIP  " + current->what);
            }
            current.reset();
        }
    }

    void finish()
    {
        finished = true;
        stopTimer();
        line ("controls exercised: " + juce::String (static_cast<int> (exercised.size())));
        line ("UI-WALK DONE passes=" + juce::String (passes) + " failures=" + juce::String (failures));

        // Quit the way the window's close button does, so shutdown is walked too.
        juce::MessageManager::callAsync ([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UiWalker)
};

} // namespace mma
