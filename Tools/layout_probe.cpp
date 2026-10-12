// Lays out the real MainScreen with camera tiles and reports what the picture
// actually comes out as. No display needed: Component layout is arithmetic.
#include "UI/MainScreen.h"
#include "UI/CameraPanel.h"
#include "UI/ModalCard.h"
#include "UI/AdvancedPanel.h"
#include "UI/SaveLocationPrompt.h"
#include "UI/SetupGuidePanel.h"
#include "UI/RecoveredTakesPanel.h"
#include "UI/SavedTakePanel.h"
#include "UI/TakeAlertCard.h"
#include "UI/CameraPreviewCover.h"
#include "Core/QuitQuestions.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

class FocusProbeCard final : public mma::ModalCard
{
public:
    FocusProbeCard()
    {
        first.setWantsKeyboardFocus (true);
        second.setWantsKeyboardFocus (true);
        addAndMakeVisible (first);
        addAndMakeVisible (second);
    }

    juce::TextButton first { "First" };
    juce::TextButton second { "Second" };

private:
    int getContentHeight() const override { return 40; }

    void layOutContent (juce::Rectangle<int> area) override
    {
        first.setBounds (area.removeFromLeft (100));
        second.setBounds (area.removeFromLeft (100));
    }
};

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int failures = 0;

    std::printf ("-- modal keyboard focus containment --\n");

    juce::Component window;
    juce::TextButton controlBehindCard { "Record" };
    FocusProbeCard card;
    window.addAndMakeVisible (controlBehindCard);
    window.addAndMakeVisible (card);

    auto* focusRoot = card.first.findKeyboardFocusContainer();
    const auto focusContainerIsCard = focusRoot == &card;
    auto traverser = card.first.createKeyboardFocusTraverser();
    const auto focusable = focusRoot != nullptr
                         ? traverser->getAllComponents (focusRoot)
                         : std::vector<juce::Component*> {};
    const auto contains = [&focusable] (juce::Component* component)
    {
        return std::find (focusable.begin(), focusable.end(), component) != focusable.end();
    };
    const auto traversalStaysOnCard = contains (&card.first) && contains (&card.second)
                                   && ! contains (&controlBehindCard);

    std::printf ("focus container: %s\n", focusContainerIsCard ? "PASS" : "FAIL");
    std::printf ("focus traversal stays on card: %s\n\n",
                 traversalStaysOnCard ? "PASS" : "FAIL");
    failures += focusContainerIsCard ? 0 : 1;
    failures += traversalStaysOnCard ? 0 : 1;

    std::printf ("-- channel-meter keyboard button semantics --\n");

    mma::ChannelMeterComponent meter;
    int primaryActions = 0;
    meter.onNameClicked = [&primaryActions] { ++primaryActions; };
    const bool spaceConsumed = meter.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
    const bool spaceActivated = spaceConsumed && primaryActions == 1;
    const bool returnConsumed = meter.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    const bool returnActivated = returnConsumed && primaryActions == 2;
    const bool hintNamesBothKeys = meter.getDescription().containsIgnoreCase ("Return or Space");

    std::printf ("Space activates focused meter: %s\n", spaceActivated ? "PASS" : "FAIL");
    std::printf ("Return still activates focused meter: %s\n", returnActivated ? "PASS" : "FAIL");
    std::printf ("accessible hint names both keys: %s\n\n", hintNamesBothKeys ? "PASS" : "FAIL");
    failures += spaceActivated ? 0 : 1;
    failures += returnActivated ? 0 : 1;
    failures += hintNamesBothKeys ? 0 : 1;

    std::printf ("-- what VoiceOver reads for the meters and camera switches --\n");
    {
        // The mix meter was an unnamed group; every camera's switch read the
        // same "Record this camera" with nothing to say which camera it was.
        mma::MixBarComponent mix;
        const bool mixNamed = mix.getTitle() == "Mix meter"
                           && mix.getDescription().contains ("decibels");

        mma::CameraPanel panel;
        panel.setCameras ({ { "a", "Wide Shot", true, true, false, false, false, "A.mov", 1 },
                            { "b", "Close Up", false, true, false, false, false, "B.mov", 1 } });

        juce::StringArray switchTitles;
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
            if (auto* t = dynamic_cast<juce::ToggleButton*> (panel.getChildComponent (i));
                t != nullptr && t->getButtonText() == "Record this camera")
                switchTitles.add (t->getTitle());

        const bool switchesNamed = switchTitles.contains ("Record Wide Shot")
                                && switchTitles.contains ("Record Close Up");

        std::printf ("mix meter has a spoken name and level: %s\n", mixNamed ? "PASS" : "FAIL");
        std::printf ("each camera's record switch names its camera: %s\n\n", switchesNamed ? "PASS" : "FAIL");
        failures += mixNamed ? 0 : 1;
        failures += switchesNamed ? 0 : 1;
    }

    std::printf ("-- saved sample rate with no connected microphone --\n");

    mma::AdvancedPanel advancedPanel;
    advancedPanel.setSampleRates ({}, 44100);
    advancedPanel.setSampleRateSelection (44100);

    bool savedRateRemainsVisible = false;
    for (int i = 0; i < advancedPanel.getNumChildComponents(); ++i)
        if (auto* combo = dynamic_cast<juce::ComboBox*> (
                advancedPanel.getChildComponent (i));
            combo != nullptr && combo->getSelectedId() == 44100
                && combo->getText() == "44.1 kHz")
            savedRateRemainsVisible = true;

    std::printf ("saved 44.1 kHz stays visible: %s\n\n",
                 savedRateRemainsVisible ? "PASS" : "FAIL");
    failures += savedRateRemainsVisible ? 0 : 1;

    // §5.4: the headline headphone delay is the quickest microphone's own
    // path, so a slower interface's own delay has to be on screen beside it
    // -- and only when there is one.
    {
        const auto latencyText = [&advancedPanel]
        {
            for (int i = 0; i < advancedPanel.getNumChildComponents(); ++i)
                if (auto* label = dynamic_cast<juce::Label*> (advancedPanel.getChildComponent (i));
                    label != nullptr && label->getText().startsWith ("2.7 ms"))
                    return label->getText();

            return juce::String();
        };

        advancedPanel.setMeasuredLatency (2.7, 45.3);
        const auto slower = latencyText();
        advancedPanel.setMeasuredLatency (2.7, 2.7);
        const auto alike = latencyText();
        const bool slowestShown = slower == "2.7 ms (slowest mic 45.3 ms)" && alike == "2.7 ms";

        std::printf ("headphone delay names a slower mic's own beside it: %s ('%s', '%s')\n\n",
                     slowestShown ? "PASS" : "FAIL", slower.toRawUTF8(), alike.toRawUTF8());
        failures += slowestShown ? 0 : 1;
    }

    std::printf ("-- camera viewer revision invalidates UI caches --\n");

    int mainViewerCreates = 0;
    juce::Component* lastMainViewer = nullptr;
    mma::MainScreen cameraScreen;
    cameraScreen.makeViewer = [&mainViewerCreates, &lastMainViewer] (const std::string&)
    {
        ++mainViewerCreates;
        auto viewer = std::make_unique<juce::Component>();
        lastMainViewer = viewer.get();
        return viewer;
    };
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 1, {}, true } });
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 1, {}, true } });
    const bool mainCacheKeepsViewer = mainViewerCreates == 1;
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 2, {}, true } });
    const bool mainRevisionRebuildsViewer = mainViewerCreates == 2;

    const auto hasExactLabel = [&cameraScreen] (const juce::String& text)
    {
        for (int i = 0; i < cameraScreen.getNumChildComponents(); ++i)
            if (auto* label = dynamic_cast<juce::Label*> (cameraScreen.getChildComponent (i));
                label != nullptr && label->getText() == text)
                return true;

        return false;
    };

    cameraScreen.setRecording (true);
    const bool recCaptionIsOutsideNativePreview = hasExactLabel ("REC: HDMI");
    cameraScreen.setRecording (false);
    const bool stoppedCaptionDropsRec = hasExactLabel ("HDMI")
                                     && ! hasExactLabel ("REC: HDMI");

    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 3,
                                     "Waiting for HDMI signal from HDMI.", false } });
    cameraScreen.setRecording (true);
    const bool omittedCameraNeverClaimsRec = hasExactLabel ("NOT RECORDING: HDMI")
                                          && ! hasExactLabel ("REC: HDMI");

    // Words around the picture change in place. Reparenting the native preview
    // for a caption, a rename or a signal message can leave AVFoundation's
    // layer black, so only a new viewer revision may rebuild the host.
    const int createsBeforeWords = mainViewerCreates;
    auto* const viewerBeforeWords = lastMainViewer;
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 3, {}, false, true } });
    const bool startingCaptionInPlace = hasExactLabel ("STARTING: HDMI");
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 3, {}, true } });
    const bool recCaptionInPlace = hasExactLabel ("REC: HDMI");
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 3, "Signal dropped.", true } });
    const bool signalCaptionInPlace = hasExactLabel ("SIGNAL LOST: HDMI");
    cameraScreen.setCameraTiles ({ { "capture", "Wide shot", 3, {}, true } });
    const bool renameInPlace = hasExactLabel ("REC: Wide shot");
    const bool captionsKeepViewerHost = startingCaptionInPlace && recCaptionInPlace
        && signalCaptionInPlace && renameInPlace
        && mainViewerCreates == createsBeforeWords
        && viewerBeforeWords != nullptr
        && cameraScreen.getIndexOfChildComponent (viewerBeforeWords) >= 0;

    int placeholderCreates = 0;
    mma::MainScreen placeholderScreen;
    placeholderScreen.makeViewer = [&placeholderCreates] (const std::string&)
    {
        ++placeholderCreates;
        return std::unique_ptr<juce::Component>();
    };
    const auto placeholderHas = [&placeholderScreen] (const juce::String& text)
    {
        for (int i = 0; i < placeholderScreen.getNumChildComponents(); ++i)
            if (auto* label = dynamic_cast<juce::Label*> (placeholderScreen.getChildComponent (i));
                label != nullptr && label->getText() == text)
                return true;

        return false;
    };
    placeholderScreen.setCameraTiles ({ { "late", "Late", 1, "Waiting for video.", false } });
    placeholderScreen.setCameraTiles ({ { "late", "Late", 1, "No video signal.", false } });
    const bool statusTextUpdatesPlaceholderInPlace = placeholderCreates == 1
        && placeholderHas ("No video signal.") && ! placeholderHas ("Waiting for video.");

    int panelViewerCreates = 0;
    mma::CameraPanel cameraPanel;
    cameraPanel.makeViewer = [&panelViewerCreates] (const std::string&)
    {
        ++panelViewerCreates;
        return std::make_unique<juce::Component>();
    };
    cameraPanel.setCameras (
        { { "capture", "HDMI", true, true, false, false, false, "HDMI.mov", 1 } });
    cameraPanel.setCameras (
        { { "capture", "HDMI", true, true, false, false, false, "HDMI.mov", 1 } });
    const bool panelCacheKeepsViewer = panelViewerCreates == 1;
    cameraPanel.setCameras (
        { { "capture", "HDMI", true, true, false, false, false, "HDMI.mov", 2 } });
    const bool panelRevisionRebuildsViewer = panelViewerCreates == 2;

    auto cameraConfigurationHasState = [&cameraPanel] (bool expectedEnabled)
    {
        bool foundRecordToggle = false;
        bool foundNameEditor = false;
        bool allMatch = true;

        for (int i = 0; i < cameraPanel.getNumChildComponents(); ++i)
        {
            auto* child = cameraPanel.getChildComponent (i);
            if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child);
                toggle != nullptr && toggle->getButtonText() == "Record this camera")
            {
                foundRecordToggle = true;
                allMatch = allMatch && toggle->isEnabled() == expectedEnabled;
            }

            if (auto* editor = dynamic_cast<juce::TextEditor*> (child))
            {
                foundNameEditor = true;
                allMatch = allMatch && editor->isEnabled() == expectedEnabled;
            }
        }

        return foundRecordToggle && foundNameEditor && allMatch;
    };

    cameraPanel.setRecording (true);
    const bool takeFreezesCameraControls = cameraConfigurationHasState (false);
    cameraPanel.setRecording (false);
    const bool stopRestoresCameraControls = cameraConfigurationHasState (true);

    mma::CameraPanel takeUnavailablePanel;
    takeUnavailablePanel.setRecording (true);
    takeUnavailablePanel.setCameras (
        { { "late", "Late HDMI", true, false, false, false, false,
            "V01_Late-HDMI.mov", 1 } });
    juce::String inTakeUnavailableCopy;
    for (int i = 0; i < takeUnavailablePanel.getNumChildComponents(); ++i)
        if (auto* label = dynamic_cast<juce::Label*> (
                takeUnavailablePanel.getChildComponent (i)))
            inTakeUnavailableCopy += " " + label->getText();
    const bool lateCameraCopyNamesThisTake =
        inTakeUnavailableCopy.containsIgnoreCase ("no video recording")
        && inTakeUnavailableCopy.containsIgnoreCase ("Not recording video in this take")
        && ! inTakeUnavailableCopy.containsIgnoreCase ("Recording to");

    takeUnavailablePanel.setRecording (false);
    takeUnavailablePanel.setCameras (
        { { "late", "Late HDMI", true, false, false, false, false,
            "V01_Late-HDMI.mov", 1 } });
    juce::String afterTakeUnavailableCopy;
    for (int i = 0; i < takeUnavailablePanel.getNumChildComponents(); ++i)
        if (auto* label = dynamic_cast<juce::Label*> (
                takeUnavailablePanel.getChildComponent (i)))
            afterTakeUnavailableCopy += " " + label->getText();
    const bool afterTakeCopyReturnsToReconnect =
        afterTakeUnavailableCopy.containsIgnoreCase ("reconnect");

    mma::SaveLocationPrompt cameraReadinessPrompt;
    cameraReadinessPrompt.setPlan ("/Volumes/CARD", "Take", {},
                                   { "MIX.wav", "V01-Wide.mov", "V02-Close.mov" },
                                   2, 1);
    juce::String readinessCopy;
    for (int i = 0; i < cameraReadinessPrompt.getNumChildComponents(); ++i)
        if (auto* label = dynamic_cast<juce::Label*> (
                cameraReadinessPrompt.getChildComponent (i)))
            readinessCopy += " " + label->getText();
    const bool savePromptDistinguishesArmedFromReady =
        readinessCopy.containsIgnoreCase ("2 cameras are switched on")
        && readinessCopy.containsIgnoreCase ("1 is live and ready")
        && readinessCopy.containsIgnoreCase ("Only live cameras record");

    std::printf ("main cache preserves unchanged viewer: %s\n",
                 mainCacheKeepsViewer ? "PASS" : "FAIL");
    std::printf ("main revision rebuilds viewer: %s\n",
                 mainRevisionRebuildsViewer ? "PASS" : "FAIL");
    std::printf ("camera REC state uses caption below native preview: %s\n",
                 recCaptionIsOutsideNativePreview ? "PASS" : "FAIL");
    std::printf ("stopped camera caption clears REC state: %s\n",
                 stoppedCaptionDropsRec ? "PASS" : "FAIL");
    std::printf ("camera omitted from frozen take never claims REC: %s\n",
                 omittedCameraNeverClaimsRec ? "PASS" : "FAIL");
    std::printf ("caption/name/signal changes keep the viewer host: %s\n",
                 captionsKeepViewerHost ? "PASS" : "FAIL");
    std::printf ("status text updates the placeholder in place: %s\n",
                 statusTextUpdatesPlaceholderInPlace ? "PASS" : "FAIL");
    std::printf ("panel cache preserves unchanged viewer: %s\n",
                 panelCacheKeepsViewer ? "PASS" : "FAIL");
    std::printf ("panel revision rebuilds viewer: %s\n\n",
                 panelRevisionRebuildsViewer ? "PASS" : "FAIL");
    std::printf ("recording freezes camera roster controls: %s\n",
                 takeFreezesCameraControls ? "PASS" : "FAIL");
    std::printf ("stopping restores camera roster controls: %s\n\n",
                 stopRestoresCameraControls ? "PASS" : "FAIL");
    std::printf ("omitted camera row and heading stay truthful: %s\n",
                 lateCameraCopyNamesThisTake ? "PASS" : "FAIL");
    std::printf ("after-take unavailable copy returns to reconnect: %s\n\n",
                 afterTakeCopyReturnsToReconnect ? "PASS" : "FAIL");
    std::printf ("save prompt distinguishes armed cameras from ready cameras: %s\n\n",
                 savePromptDistinguishesArmedFromReady ? "PASS" : "FAIL");
    failures += mainCacheKeepsViewer ? 0 : 1;
    failures += mainRevisionRebuildsViewer ? 0 : 1;
    failures += recCaptionIsOutsideNativePreview ? 0 : 1;
    failures += stoppedCaptionDropsRec ? 0 : 1;
    failures += omittedCameraNeverClaimsRec ? 0 : 1;
    failures += captionsKeepViewerHost ? 0 : 1;
    failures += statusTextUpdatesPlaceholderInPlace ? 0 : 1;
    failures += panelCacheKeepsViewer ? 0 : 1;
    failures += panelRevisionRebuildsViewer ? 0 : 1;
    failures += takeFreezesCameraControls ? 0 : 1;
    failures += stopRestoresCameraControls ? 0 : 1;
    failures += lateCameraCopyNamesThisTake ? 0 : 1;
    failures += afterTakeCopyReturnsToReconnect ? 0 : 1;
    failures += savePromptDistinguishesArmedFromReady ? 0 : 1;

    struct Case { int w, h, mics; const char* label; };
    const Case cases[] = {
        // The size the window ACTUALLY opens at, which is what a user sees
        // before touching anything. The cases below it assume a window someone
        // has already dragged bigger, and measuring only those is how a picture
        // that is small on launch went out reported as large.
        { 1180,  560, 0, "REAL launch, no mics" },
        { 1180,  560, 3, "REAL launch, 3 mics" },
        { 1180,  900, 0, "dragged taller, no mics" },
        { 1180,  900, 3, "dragged taller, 3 mics" },
        { 2000, 1071, 0, "maximised wide, no mics" },
        { 1680, 1050, 3, "1680x1050, 3 mics" },
    };

    for (const auto& c : cases)
    {
        mma::MainScreen s;
        s.setMicCount (c.mics);
        s.setCameraScale (5);                       // the shipped default
        s.setCameraTiles ({ { "cam", "FaceTime" } });

        // What MainComponent does, in its order: tell the screen how much of it
        // the window can actually show, THEN size the content to the larger of
        // what it wants and the viewport.
        //
        // The order is the point. Sizing first and reporting after measures the
        // picture against a canvas the picture itself grew, which is the loop
        // that let a 1007px layout settle inside a 560px window.
        s.setVisibleHeight (c.h);
        s.setSize (c.w, juce::jmax (c.h, s.getRequiredHeight()));
        s.resized();

        // The tile is whatever child sits in the camera row -- the placeholder,
        // since makeViewer is unset here.
        juce::Rectangle<int> tile;
        for (int i = 0; i < s.getNumChildComponents(); ++i)
        {
            auto b = s.getChildComponent (i)->getBounds();
            if (b.getWidth() > tile.getWidth() && b.getHeight() > 60)
                tile = b;
        }

        std::printf ("%-26s window %4dx%-5d  required %5d  wants %5d  picture %4dx%-4d (%.0f%% of width)%s\n",
                     c.label, c.w, c.h, s.getRequiredHeight(), s.getPreferredHeight(),
                     tile.getWidth(), tile.getHeight(),
                     100.0 * tile.getWidth() / juce::jmax (1, c.w - 32),
                     s.getRequiredHeight() > c.h ? "  OVERFLOWS WINDOW" : "");
    }
    std::printf ("\n-- after the window grows to what the screen asked for --\n");

    for (const auto& c : cases)
    {
        mma::MainScreen s;
        s.setMicCount (c.mics);
        s.setCameraScale (5);
        s.setCameraTiles ({ { "cam", "FaceTime" } });

        // Pass one: what does it want in the window it has?
        s.setVisibleHeight (c.h);
        s.setSize (c.w, juce::jmax (c.h, s.getRequiredHeight()));
        s.resized();

        // Pass two: the owner grows the window to that, bounded by the display,
        // and the screen is laid out again in the window it now has.
        const int grown = juce::jmin (s.getPreferredHeight(), 1071);
        s.setVisibleHeight (grown);
        s.setSize (c.w, juce::jmax (grown, s.getRequiredHeight()));
        s.resized();

        juce::Rectangle<int> tile;
        for (int i = 0; i < s.getNumChildComponents(); ++i)
        {
            auto b = s.getChildComponent (i)->getBounds();
            if (b.getWidth() > tile.getWidth() && b.getHeight() > 60)
                tile = b;
        }

        std::printf ("%-26s window %4dx%-5d  required %5d  picture %4dx%-4d (%.0f%% of width)%s\n",
                     c.label, c.w, grown, s.getRequiredHeight(),
                     tile.getWidth(), tile.getHeight(),
                     100.0 * tile.getWidth() / juce::jmax (1, c.w - 32),
                     s.getRequiredHeight() > grown ? "  OVERFLOWS WINDOW" : "");
    }

    std::printf ("\n-- the monitor-problem line, which must fit its reason --\n");

    // The message that named a cause used to be clipped to the first few words
    // by a fixed one-line band, which left exactly the dead end it was written
    // to end. The band has to grow with the text, and the screen has to stay
    // inside the window while it does.
    struct Msg { const char* label; const char* text; };
    const Msg messages[] = {
        { "empty",  "" },
        { "short",  "Mic 1 couldn't be opened for recording." },
        { "reason", "PUPGSIS-T12S 1 couldn't be opened for recording. This interface is "
                    "running at 44.1 kHz and won't change to the 48 kHz this recording uses. "
                    "Set the recording to 44.1 kHz in Settings, or change the interface to "
                    "48 kHz in Audio MIDI Setup." },
    };

    for (const auto& m : messages)
    {
        mma::MainScreen s;
        s.setMicCount (2);
        s.setMonitorProblemText (m.text);

        s.setVisibleHeight (560);
        s.setSize (1180, juce::jmax (560, s.getRequiredHeight()));
        s.resized();

        const int grown = juce::jmin (s.getPreferredHeight(), 1071);
        s.setVisibleHeight (grown);
        s.setSize (1180, juce::jmax (grown, s.getRequiredHeight()));
        s.resized();

        std::printf ("%-8s band %3d px   required %4d  window %4d%s\n",
                     m.label, s.getMonitorProblemBandHeight(),
                     s.getRequiredHeight(), grown,
                     s.getRequiredHeight() > grown ? "  OVERFLOWS WINDOW" : "");
    }

    // --- Controls that must show what was SAVED, not a constant ------------
    //
    // Three controls opened at a hardcoded value with no setter at all: the
    // monitor volume slider at 70, the backup-copy box ticked, and the full-
    // preview box unticked. Each therefore described a state the app might not
    // be in. The backup one is the dangerous member of the set -- it told
    // someone who had switched the second copy off that it was on.
    {
        mma::MainScreen screen;
        mma::AdvancedPanel panel;

        screen.setMasterVolume (15.0);
        const bool volumeRoundTrips = std::abs (screen.getMasterVolume() - 15.0) < 1.0e-9;

        screen.setFullPreview (true);
        const bool previewRoundTrips = screen.isFullPreview();

        panel.setMirrorEnabled (false);
        const bool mirrorRoundTrips = ! panel.isMirrorEnabled();

        std::printf ("\nControls restate what was saved\n");
        std::printf ("  %s  the volume slider shows the saved volume, not 70\n",
                     volumeRoundTrips ? "PASS" : "FAIL");
        std::printf ("  %s  the full-preview box shows the saved quality\n",
                     previewRoundTrips ? "PASS" : "FAIL");
        std::printf ("  %s  the backup-copy box shows the saved setting, not always on\n",
                     mirrorRoundTrips ? "PASS" : "FAIL");

        failures += volumeRoundTrips ? 0 : 1;
        failures += previewRoundTrips ? 0 : 1;
        failures += mirrorRoundTrips ? 0 : 1;
    }

    // --- The footer at the height the screen asked for ----------------------
    //
    // The budget getRequiredHeight() sums has to be the one resized() spends.
    // It left out the row under the record button that says why it is off, and
    // the taller band a rig with no microphones gets -- so at exactly the
    // height it asked for, the footer took the shortfall and the mute button
    // came out 13px tall.
    const auto findChild = [] (juce::Component& parent, auto predicate) -> juce::Component*
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
            if (predicate (parent.getChildComponent (i)))
                return parent.getChildComponent (i);

        return nullptr;
    };

    const auto findButton = [&findChild] (juce::Component& parent, const juce::String& text)
    {
        return findChild (parent, [&text] (juce::Component* c)
        {
            auto* b = dynamic_cast<juce::Button*> (c);
            return b != nullptr && b->getButtonText() == text;
        });
    };

    const auto findLabel = [&findChild] (juce::Component& parent, const juce::String& text)
    {
        return dynamic_cast<juce::Label*> (findChild (parent, [&text] (juce::Component* c)
        {
            auto* l = dynamic_cast<juce::Label*> (c);
            return l != nullptr && l->getText() == text;
        }));
    };

    const auto findSlider = [&findChild] (juce::Component& parent)
    {
        return findChild (parent, [] (juce::Component* c)
        {
            return dynamic_cast<juce::Slider*> (c) != nullptr;
        });
    };

    {
        std::printf ("\nFooter keeps its height at the required height\n");

        struct FooterCase { const char* label; int mics; bool recordEnabled; bool camera; };
        const FooterCase footerCases[] = {
            { "3 mics, record enabled",  3, true,  false },
            { "3 mics, record disabled", 3, false, false },
            { "no mics",                 0, true,  false },
            { "no mics, record disabled", 0, false, false },
            { "16 mics, record disabled", 16, false, false },
            { "camera, record disabled", 3, false, true  },
        };

        for (const auto& fc : footerCases)
        {
            for (const int width : { 1180, 707, 560 })
            {
                mma::MainScreen s;
                s.setMicCount (fc.mics);
                s.setMuteState (false, false);
                s.setRecordButtonEnabled (fc.recordEnabled,
                                          fc.recordEnabled ? juce::String()
                                                           : juce::String ("Choose where recordings go first."));
                if (fc.camera)
                    s.setCameraTiles ({ { "cam", "FaceTime" } });

                // Laid out at exactly what it asked for: the tightest height the
                // owner will ever give it once the content outgrows the window.
                s.setSize (width, 420);
                s.setVisibleHeight (420);
                s.setSize (width, s.getRequiredHeight());
                s.resized();

                auto* mute = findButton (s, "Mute");
                auto* slider = findSlider (s);

                const int muteHeight = mute != nullptr ? mute->getHeight() : 0;
                const int sliderHeight = slider != nullptr ? slider->getHeight() : 0;
                const bool insideWindow = mute != nullptr
                                       && mute->getBottom() <= s.getHeight() - 16;
                const bool ok = muteHeight >= 28 && sliderHeight >= 28 && insideWindow;

                std::printf ("  %s  %-26s %4dpx wide: mute %2dpx, volume %2dpx tall\n",
                             ok ? "PASS" : "FAIL", fc.label, width, muteHeight, sliderHeight);
                failures += ok ? 0 : 1;
            }
        }
    }

    // --- Long advice and record-button reasons wrap rather than cut ---------
    //
    // Both lines were one fixed 20px row, so a sentence longer than the window
    // was squashed and then cut off. They grow to fit -- below the record
    // button, which must not move for a sentence that fits on one line.
    {
        std::printf ("\nAdvice and the record-button reason wrap to fit\n");

        const juce::String shortText = "Choose where recordings go first.";
        const juce::String longText = "Recording is off because the card that takes were going to has gone away. "
                                      "Plug it back in, or choose another place to save in Settings, and the "
                                      "button comes back on its own -- nothing recorded so far is lost.";

        for (const int width : { 1180, 707, 560 })
        {
            const auto recordTop = [&findButton] (mma::MainScreen& s)
            {
                auto* b = findButton (s, "Start recording");
                return b != nullptr ? b->getY() : -1;
            };

            const auto layOut = [width] (mma::MainScreen& s)
            {
                s.setSize (width, 420);
                s.setVisibleHeight (420);
                s.setSize (width, s.getRequiredHeight());
                s.resized();
            };

            mma::MainScreen plain;
            plain.setMicCount (3);
            plain.setRecordButtonEnabled (false, shortText);
            layOut (plain);
            const int plainTop = recordTop (plain);

            for (const bool longOne : { false, true })
            {
                mma::MainScreen s;
                s.setMicCount (3);
                const auto text = longOne ? longText : shortText;
                s.setRecordButtonEnabled (false, text);
                s.setAdviceText (text);
                layOut (s);

                bool ok = recordTop (s) == plainTop && plainTop >= 0;
                // Both labels carry the same text here; the reason is the one
                // directly under the button, the advice the lower one.
                std::vector<juce::Label*> found;

                for (int i = 0; i < s.getNumChildComponents(); ++i)
                {
                    auto* label = dynamic_cast<juce::Label*> (s.getChildComponent (i));
                    if (label == nullptr || label->getText() != text)
                        continue;

                    // Lines the label's height allows JUCE to draw, against the
                    // lines the sentence needs at this width.
                    const auto font = label->getFont();
                    const auto area = label->getBorderSize().subtractedFrom (label->getLocalBounds());
                    const int allowed = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
                    const float needed = font.getStringWidthFloat (text);
                    const bool fits = needed < (float) area.getWidth()
                                   || (float) allowed > (needed + 80.0f) / (float) area.getWidth();

                    ok = ok && fits && label->getBottom() <= s.getHeight() - 40 - 16;
                    found.push_back (label);
                }

                std::sort (found.begin(), found.end(),
                           [] (juce::Label* a, juce::Label* b) { return a->getY() < b->getY(); });
                const int reasonHeight = found.size() == 2 ? found[0]->getHeight() : 0;
                const int adviceHeight = found.size() == 2 ? found[1]->getHeight() : 0;

                auto* mute = findButton (s, "Mute");
                ok = ok && reasonHeight > 0 && adviceHeight > 0
                        // A sentence that fits on one line keeps the 20px row;
                        // the fit check above covers the ones that wrap.
                        && (longOne || (reasonHeight == 20 && adviceHeight == 20))
                        && mute != nullptr && mute->getHeight() >= 28;

                std::printf ("  %s  %4dpx %-5s reason %2dpx, advice %2dpx, record button at y=%d\n",
                             ok ? "PASS" : "FAIL", width, longOne ? "long" : "short",
                             reasonHeight, adviceHeight, recordTop (s));
                failures += ok ? 0 : 1;
            }
        }
    }

    // --- The remaining-time line in a narrow window -------------------------
    //
    // Beside an open drawer the screen is ~707px wide, and the window can be
    // dragged down to 560. The volume slider kept its full 220px and the
    // capacity figure -- the one number in the footer anyone acts on -- was
    // left ~100px and ellipsized to "Room for 1...".
    {
        std::printf ("\nRemaining time stays readable in a narrow window\n");

        const juce::String remaining = "Room for 12h 40m of feelings";
        const juce::String elapsed = "Recording for 1h 02m";

        for (const bool takeRunning : { false, true })
        {
            for (const auto& size : { juce::Point<int> (707, 560), juce::Point<int> (560, 420),
                                      juce::Point<int> (1180, 560) })
            {
                mma::MainScreen s;
                s.setMicCount (2);
                s.setRecording (takeRunning);
                s.setRemainingTimeText (remaining);
                s.setElapsedTimeText (takeRunning ? elapsed : juce::String());
                s.setSaveLocationText ("Saves to /Users/someone/Music/SobStage");

                s.setVisibleHeight (size.y);
                s.setSize (size.x, juce::jmax (size.y, s.getRequiredHeight()));
                s.resized();

                // Idle, the figure is alone in the row and has to fit whole.
                // During a take it shares with the elapsed time, and at 560px
                // the two cannot both be whole -- but neither may be starved
                // to a stub while the other is comfortable.
                const double share = takeRunning ? 0.7 : 1.0;

                const auto labelFits = [&s, &findLabel, share] (const juce::String& text, int& width, int& needed)
                {
                    auto* label = findLabel (s, text);
                    width = label != nullptr ? label->getWidth() : 0;
                    needed = label != nullptr
                                 ? juce::roundToInt (label->getFont().getStringWidthFloat (text))
                                       + label->getBorderSize().getLeftAndRight()
                                 : 1;
                    return width >= juce::roundToInt (share * needed);
                };

                int remainingWidth = 0, remainingNeeded = 0;
                bool ok = labelFits (remaining, remainingWidth, remainingNeeded);

                int elapsedWidth = 0, elapsedNeeded = 0;
                if (takeRunning)
                    ok = labelFits (elapsed, elapsedWidth, elapsedNeeded) && ok;

                auto* slider = findSlider (s);
                ok = ok && slider != nullptr && slider->getWidth() >= 80;

                std::printf ("  %s  %4dx%-4d %-9s remaining %3d/%3dpx  elapsed %3d/%3dpx  volume %3dpx\n",
                             ok ? "PASS" : "FAIL", size.x, size.y,
                             takeRunning ? "recording" : "idle",
                             remainingWidth, remainingNeeded, elapsedWidth, elapsedNeeded,
                             slider != nullptr ? slider->getWidth() : 0);
                failures += ok ? 0 : 1;
            }
        }
    }

    // --- A quit waiting for the after-take work says so, whole --------------
    //
    // "Quit When Done" used to leave the screen exactly as it was, so the
    // natural next move was to quit again -- which cancelled the combined
    // video. The line under the record button now says the app is waiting,
    // and it has to be readable at the narrowest window, longest wording.
    {
        std::printf ("\nA pending quit is readable under the record button\n");

        const juce::String pending (mma::QuitQuestions::pendingQuitLine (
            "the podcast copy and the combined video"));

        for (const int width : { 1180, 707, 560 })
        {
            mma::MainScreen s;
            s.setMicCount (2);
            s.setFilesBeingSavedText (pending);

            s.setVisibleHeight (560);
            s.setSize (width, juce::jmax (560, s.getRequiredHeight()));
            s.resized();

            auto* label = findLabel (s, pending);
            const int labelWidth = label != nullptr ? label->getWidth() : 0;
            const int needed = label != nullptr
                                   ? juce::roundToInt (label->getFont().getStringWidthFloat (pending))
                                         + label->getBorderSize().getLeftAndRight()
                                   : 1;
            const bool ok = label != nullptr && label->isVisible() && labelWidth >= needed;

            std::printf ("  %s  %4dpx: line %3dpx, sentence needs %3dpx\n",
                         ok ? "PASS" : "FAIL", width, labelWidth, needed);
            failures += ok ? 0 : 1;
        }
    }

    // --- ... and on the card that covers that line ---------------------------
    //
    // A card covers the line under the record button, and one is usually up
    // when the quit starts waiting: "Stop and Quit" stops the take, and its
    // saved-take card opens behind the "Still saving" question; Cmd-Q from
    // the saved-take card leaves it up. With the sentence only on the main
    // screen, the window looked exactly as before until Done. Every card says
    // it at its foot: grown to hold it, inside the card, below its buttons,
    // drawn whole, and frontmost where it is drawn. Clearing it puts the card
    // back as it was.
    {
        std::printf ("\nA pending quit is readable on the card that covers that line\n");

        const juce::String pending (mma::QuitQuestions::pendingQuitLine (
            "the podcast copy and the combined video"));

        const auto footnoteShows = [&findLabel, &pending] (mma::ModalCard& card, int width,
                                                           juce::String& detail)
        {
            card.setVisible (true); // as the owner shows it; a hidden one hit-tests as nothing
            card.setFootnote ({});
            card.setSize (width, 2000);
            const int heightWithout = card.getRequiredHeight();

            card.setFootnote (pending);
            const int heightWith = card.getRequiredHeight();

            // What MainComponent gives a card that has just grown: the window
            // grown to its required height plus the 32px it always keeps.
            card.setSize (width, heightWith + 32);

            const int cardWidth = juce::jmin (480, juce::jmax (280, width - 32));
            const auto cardBounds = juce::Rectangle<int> (cardWidth, heightWith)
                                        .withCentre (card.getLocalBounds().getCentre());

            auto* label = findLabel (card, pending);
            bool ok = label != nullptr && label->isVisible() && heightWith > heightWithout;

            if (label != nullptr)
            {
                const auto font = label->getFont();
                const auto area = label->getBorderSize().subtractedFrom (label->getLocalBounds());
                const int allowed = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
                const float needed = font.getStringWidthFloat (pending);
                const bool whole = needed < (float) area.getWidth()
                                || (float) allowed > (needed + 80.0f) / (float) area.getWidth();

                int lowestButton = 0;
                for (int i = 0; i < card.getNumChildComponents(); ++i)
                    if (auto* b = dynamic_cast<juce::Button*> (card.getChildComponent (i)); b != nullptr && b->isVisible())
                        lowestButton = juce::jmax (lowestButton, b->getBottom());

                const bool inside = cardBounds.contains (label->getBounds());
                const bool belowButtons = lowestButton > 0 && label->getY() >= lowestButton;
                const bool frontmost = card.getComponentAt (label->getBounds().getCentre()) == label;

                ok = ok && whole && inside && belowButtons && frontmost;
                detail << "line " << label->getHeight() << "px tall at y=" << label->getY()
                       << ", buttons end at y=" << lowestButton << ", card +"
                       << (heightWith - heightWithout) << "px"
                       << (whole ? "" : ", CUT OFF") << (inside ? "" : ", OUTSIDE THE CARD")
                       << (frontmost ? "" : ", COVERED");
            }
            else
            {
                detail << "no line on the card";
            }

            card.setFootnote ({});
            const bool clears = (label == nullptr || ! label->isVisible())
                             && card.getRequiredHeight() == heightWithout;

            if (! clears)
                detail << ", NOT CLEARED";

            return ok && clears;
        };

        for (const int width : { 1180, 707, 560, 420 })
        {
            mma::SavedTakePanel saved;
            saved.setTake ("/Volumes/CARD/RECORDINGS/2026-10-12_2100_Late-Show",
                           "/Users/me/RECORDINGS-MIRROR/2026-10-12_2100_Late-Show",
                           { { "MIX.wav", 412000000 }, { "Mic 1 - Host.wav", 206000000 },
                             { "Mic 2 - Guest.wav", 206000000 }, { "V01-Wide.mov", 2100000000 } },
                           mma::TakeAudioVerdict::Recorded);

            mma::TakeAlertCard alert;
            alert.addAlert ("12:04", "Mic 2 stopped sending audio.", mma::TakeAlertCard::Tone::NeedsYou);
            alert.setSevere (true, true);

            mma::RecoveredTakesPanel recovered;
            {
                mma::RecoveredTakeRow take;
                take.folderName = "2026-10-12_2100_Late-Show";
                take.fullPath = "/Volumes/CARD/RECORDINGS/" + take.folderName;
                take.fileCount = take.playableFileCount = 3;
                take.longestSeconds = 754.0;
                recovered.setTakes ({ take });
            }

            mma::SaveLocationPrompt prompt;
            prompt.setPlan ("/Volumes/CARD/RECORDINGS", "2026-10-12_2200_Encore", {},
                            { "MIX.wav", "Mic 1 - Host.wav", "Mic 2 - Guest.wav" }, 0, 0);

            const std::pair<const char*, mma::ModalCard*> cards[] = {
                { "saved-take card", &saved },
                { "mid-take alert card", &alert },
                { "recovered-takes card", &recovered },
                { "save-location card", &prompt },
            };

            for (const auto& [name, card] : cards)
            {
                juce::String detail;
                const bool ok = footnoteShows (*card, width, detail);

                std::printf ("  %s  %4dpx %-21s %s\n", ok ? "PASS" : "FAIL", width, name,
                             detail.toRawUTF8());
                failures += ok ? 0 : 1;
            }
        }
    }


    // macOS camera previews are native NSViews stacked above everything JUCE
    // paints in the window, so a card or the take banner cannot be drawn over
    // them. They are hidden instead, for exactly as long as something covers
    // them -- and a viewer rebuilt while a card is up has to stay hidden too.
    std::printf ("\n-- camera previews hide under cards and the take banner --\n");
    {
        const auto allViewers = [] (std::vector<juce::Component*>& viewers, bool visible)
        {
            bool ok = ! viewers.empty();
            for (auto* viewer : viewers)
                ok = ok && viewer->isVisible() == visible;
            return ok;
        };

        std::vector<juce::Component*> screenViewers;
        mma::MainScreen screen;
        screen.makeViewer = [&screenViewers] (const std::string&)
        {
            auto viewer = std::make_unique<juce::Component>();
            screenViewers.push_back (viewer.get());
            return viewer;
        };
        screen.setCameraTiles ({ { "a", "Cam A", 1, {}, false }, { "b", "Cam B", 1, {}, false } });
        const bool screenShownByDefault = allViewers (screenViewers, true);
        screen.setCameraPreviewsHidden (true);
        const bool screenHides = allViewers (screenViewers, false);
        screenViewers.clear();
        screen.setCameraTiles ({ { "a", "Cam A", 2, {}, false }, { "b", "Cam B", 2, {}, false } });
        const bool screenRebuildStaysHidden = allViewers (screenViewers, false);
        screen.setCameraPreviewsHidden (false);
        const bool screenShowsAgain = allViewers (screenViewers, true);

        std::vector<juce::Component*> panelViewers;
        mma::CameraPanel panel;
        panel.makeViewer = [&panelViewers] (const std::string&)
        {
            auto viewer = std::make_unique<juce::Component>();
            panelViewers.push_back (viewer.get());
            return viewer;
        };
        panel.setCameras ({ { "a", "Cam A", true, true, false, false, false, "A.mov", 1 } });
        const bool panelShownByDefault = allViewers (panelViewers, true);
        panel.setCameraPreviewsHidden (true);
        const bool panelHides = allViewers (panelViewers, false);
        panelViewers.clear();
        panel.setCameras ({ { "a", "Cam A", true, true, false, false, false, "A.mov", 2 } });
        const bool panelRebuildStaysHidden = allViewers (panelViewers, false);
        panel.setCameraPreviewsHidden (false);
        const bool panelShowsAgain = allViewers (panelViewers, true);

        // The window-level watcher: any watched overlay going visible covers
        // the previews at once, and they come back only when all are gone.
        juce::Component coverWindow;
        FocusProbeCard coverCard;
        juce::Component coverBanner;
        coverWindow.addChildComponent (coverCard);
        coverWindow.addChildComponent (coverBanner);
        std::vector<bool> reported;
        mma::CameraPreviewCover cover;
        cover.onCoverChanged = [&reported] (bool covered) { reported.push_back (covered); };
        cover.watch (coverCard);
        cover.watch (coverBanner);
        const bool startsUncovered = ! cover.isCovered() && reported.empty();
        coverCard.setVisible (true);
        const bool cardCovers = cover.isCovered() && reported == std::vector<bool> { true };
        coverBanner.setVisible (true);
        coverCard.setVisible (false);
        const bool bannerKeepsCovered = cover.isCovered() && reported.size() == 1;
        coverBanner.setVisible (false);
        const bool allGoneUncovers = ! cover.isCovered()
                                  && reported == std::vector<bool> { true, false };

        // What the native attachment relies on: hiding the host reaches a
        // movement watcher registered on the inner native component, the way
        // NSViewAttachment turns it into [view setHidden: ! isShowing()].
        // Headless there is no peer, so nothing is ever "showing"; what is
        // checked is that the host's own visibility change is delivered.
        struct VisibilityWatcher final : juce::ComponentMovementWatcher
        {
            using juce::ComponentMovementWatcher::ComponentMovementWatcher;
            void componentMovedOrResized (bool, bool) override {}
            void componentPeerChanged() override {}
            using juce::ComponentMovementWatcher::componentVisibilityChanged;
            void componentVisibilityChanged() override {}
            void componentVisibilityChanged (juce::Component& changed) override
            {
                if (&changed == watchedHost)
                    ++hostChanges;
                juce::ComponentMovementWatcher::componentVisibilityChanged (changed);
            }
            juce::Component* watchedHost = nullptr;
            int hostChanges = 0;
        };
        juce::Component nativeHostParent, nativeHost, nativeView;
        nativeHostParent.addAndMakeVisible (nativeHost);
        nativeHost.addAndMakeVisible (nativeView);
        VisibilityWatcher watcher (&nativeView);
        watcher.watchedHost = &nativeHost;
        nativeHost.setVisible (false);
        const bool hostHideReachesNativeView = watcher.hostChanges == 1 && nativeView.isVisible();

        const std::pair<const char*, bool> checks[] = {
            { "main-screen previews shown by default", screenShownByDefault },
            { "main-screen previews hide under a card", screenHides },
            { "main-screen rebuild under a card stays hidden", screenRebuildStaysHidden },
            { "main-screen previews come back after the card", screenShowsAgain },
            { "cameras-panel previews shown by default", panelShownByDefault },
            { "cameras-panel previews hide under a card", panelHides },
            { "cameras-panel rebuild under a card stays hidden", panelRebuildStaysHidden },
            { "cameras-panel previews come back after the card", panelShowsAgain },
            { "cover starts uncovered", startsUncovered },
            { "a card going up covers at once", cardCovers },
            { "banner keeps previews covered after the card", bannerKeepsCovered },
            { "previews uncovered once every overlay is gone", allGoneUncovers },
            { "hiding the host reaches the native view's watcher", hostHideReachesNativeView },
        };

        for (const auto& [name, ok] : checks)
        {
            std::printf ("%s: %s\n", name, ok ? "PASS" : "FAIL");
            failures += ok ? 0 : 1;
        }
    }

    // --- The setup guide at the drawer's narrowest and widest -----------------
    //
    // Every page, with a four-microphone rig, at the size the drawer gives it
    // and at exactly the height it asks for: every control inside the panel,
    // none on top of another, and Back and Next under the page's own content
    // rather than clipped off the bottom of it.
    {
        std::printf ("\n-- setup guide pages fit the drawer --\n");

        mma::SetupGuideRig rig;
        rig.micCount = 4;
        rig.headphoneJackCount = 4;
        rig.cameraCount = 1;
        rig.camerasOn = 1;
        rig.recordBlockedReason = "Checking the drive is fast enough to record to.";

        std::vector<mma::SetupGuidePanel::MicRow> mics;
        std::vector<mma::SetupGuidePanel::HeadphoneRow> phones;
        for (int i = 1; i <= 4; ++i)
        {
            mics.push_back ({ "Person " + juce::String (i), "Yeti Stereo Microphone" });
            phones.push_back ({ "key" + juce::String (i), "Person " + juce::String (i), i != 2 });
        }

        bool everyPageFits = true;

        for (const int width : { 366, 488 })
            for (const auto step : mma::SetupGuide::stepsFor (rig))
            {
                mma::SetupGuidePanel guide;
                guide.setMicrophones (mics);
                guide.setHeadphones (phones);
                guide.setDestinationFolder ("/Users/someone/Music/SobStage Recordings");
                guide.setPage (mma::SetupGuide::pageFor (step, rig));
                guide.setSize (width, 10);
                guide.setSize (width, guide.getRequiredHeight());

                std::vector<juce::Rectangle<int>> placed;
                bool fits = true;

                for (int i = 0; i < guide.getNumChildComponents(); ++i)
                {
                    auto* child = guide.getChildComponent (i);
                    if (! child->isVisible())
                        continue;

                    const auto b = child->getBounds();
                    fits = fits && ! b.isEmpty() && guide.getLocalBounds().contains (b);

                    for (const auto& other : placed)
                        fits = fits && ! other.intersects (b);

                    placed.push_back (b);
                }

                const auto* next = findButton (guide, step == mma::SetupGuideStep::TestTake ? "Done" : "Next");
                fits = fits && next != nullptr && next->isVisible()
                    && next->getBottom() <= guide.getHeight() - 12;

                // A page's own controls: one Name per microphone, one switch
                // per headphone socket, and the folder's Change... button.
                int visibleNames = 0, visibleToggles = 0;
                for (int i = 0; i < guide.getNumChildComponents(); ++i)
                    if (auto* b = dynamic_cast<juce::Button*> (guide.getChildComponent (i)); b != nullptr && b->isVisible())
                    {
                        visibleNames += b->getButtonText() == "Name" ? 1 : 0;
                        visibleToggles += dynamic_cast<juce::ToggleButton*> (b) != nullptr ? 1 : 0;
                    }

                fits = fits && visibleNames == (step == mma::SetupGuideStep::Names ? 4 : 0);
                fits = fits && visibleToggles == (step == mma::SetupGuideStep::Headphones ? 4 : 0);

                const auto* change = findButton (guide, "Change...");
                fits = fits && change != nullptr && change->isVisible() == (step == mma::SetupGuideStep::SaveFolder);

                if (! fits)
                    std::printf ("  FAIL  page %d at width %d\n", static_cast<int> (step), width);

                everyPageFits = everyPageFits && fits;
            }

        std::printf ("every setup guide page fits at both drawer widths: %s\n", everyPageFits ? "PASS" : "FAIL");
        failures += everyPageFits ? 0 : 1;
    }

    // --- The recovered-takes card: every take's account is readable ---------
    //
    // The line under each take grew a clause at a time -- files that could not
    // be repaired, empty ones, the camera movies, the backup copy offered in
    // the card's place -- and its row was sized by counting clauses, so the
    // last one was cut off. Every detail must be drawn whole, inside the card,
    // above the buttons, in a full-width window and a narrow one.
    {
        std::printf ("\nThe recovered-takes card shows every take's account whole\n");

        std::vector<mma::RecoveredTakeRow> takes;
        {
            mma::RecoveredTakeRow plain;
            plain.folderName = "2026-10-07_2100_Plain";
            plain.fullPath = "/Volumes/CARD/RECORDINGS/" + plain.folderName;
            plain.fileCount = plain.playableFileCount = 3;
            plain.longestSeconds = 754.0;
            takes.push_back (plain);

            mma::RecoveredTakeRow backup = plain;
            backup.folderName = "2026-10-07_2000_Locked-card_2";
            backup.fullPath = "/Users/me/RECORDINGS-MIRROR/" + backup.folderName;
            backup.emptyFileCount = 2;
            backup.movieCount = 2;
            backup.quickTimeMovieCount = 2; // a Mac's: AVFoundation's wording
            backup.moviesInCardCopy = true;
            backup.isBackupBecauseCardCopyUnrepairable = true;
            takes.push_back (backup);

            mma::RecoveredTakeRow worst = plain;
            worst.folderName = "2026-10-07_1900_Everything";
            worst.fileCount = 12;
            worst.playableFileCount = 1;
            worst.emptyFileCount = 3;
            worst.movieCount = 3; // a Windows camera's .wmv: the longer, neutral wording
            takes.push_back (worst);
        }

        for (const int width : { 1180, 707, 420 })
        {
            mma::RecoveredTakesPanel panel;
            panel.setTakes (takes);
            panel.setSize (width, 1400);

            bool ok = panel.getRequiredHeight() < 1400 - 32;
            int lastBottom = 0, rowsFound = 0;
            const juce::Rectangle<int> card ((width - juce::jmin (480, juce::jmax (280, width - 32))) / 2, 0,
                                             juce::jmin (480, juce::jmax (280, width - 32)), 1400);

            for (int i = 0; i < panel.getNumChildComponents(); ++i)
            {
                auto* label = dynamic_cast<juce::Label*> (panel.getChildComponent (i));
                if (label == nullptr)
                    continue;

                const bool isDetail = std::any_of (takes.begin(), takes.end(), [label] (const auto& t)
                {
                    return label->getText() == juce::String::fromUTF8 (mma::recoveredTakeDetail (t).c_str());
                });
                if (! isDetail)
                    continue;

                ++rowsFound;
                const auto font = label->getFont();
                const auto area = label->getBorderSize().subtractedFrom (label->getLocalBounds());
                const int allowed = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
                const float needed = font.getStringWidthFloat (label->getText());
                const bool fits = needed < (float) area.getWidth()
                               || (float) allowed > (needed + 80.0f) / (float) area.getWidth();

                ok = ok && fits && label->getX() >= card.getX() && label->getRight() <= card.getRight()
                        && label->getY() >= lastBottom;
                lastBottom = label->getBottom();

                if (! fits)
                    std::printf ("  cut off at %dpx: \"%s\" (%d lines allowed)\n", width,
                                 label->getText().toRawUTF8(), allowed);
            }

            auto* open = findButton (panel, "Open the newest");
            ok = ok && rowsFound == (int) takes.size() && open != nullptr && open->getY() >= lastBottom;

            std::printf ("  %s  %4dpx: %d takes listed, last detail ends at y=%d, buttons at y=%d\n",
                         ok ? "PASS" : "FAIL", width, rowsFound, lastBottom,
                         open != nullptr ? open->getY() : -1);
            failures += ok ? 0 : 1;
        }
    }

    return failures == 0 ? 0 : 1;
}
