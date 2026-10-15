#include "AdvancedPanel.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {

/// Room for a dozen lines of activity at 11pt. Fixed rather than measured
/// because getRequiredHeight() has to be able to state the panel's height
/// without laying it out, and this is the one block that would otherwise grow
/// under it.
constexpr int kActivityHeight = 12 * 14;

void configureRow (juce::Label& label, juce::Label& value, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
}
} // namespace

AdvancedPanel::AdvancedPanel()
{
    sampleRateLabel.setText ("Sample rate", juce::dontSendNotification);
    bitDepthLabel.setText ("Bit depth", juce::dontSendNotification);
    bufferSizeLabel.setText ("Buffer size", juce::dontSendNotification);
    configureRow (latencyLabel, latencyValue, "Measured latency");
    clockMasterLabel.setText ("Clock master", juce::dontSendNotification);
    driftLabel.setText ("Clock drift per mic (PPM)", juce::dontSendNotification);
    driftLabel.setJustificationType (juce::Justification::topLeft);
    outputDeviceLabel.setText ("Output device", juce::dontSendNotification);
    backendLabel.setText ("Seen by other apps", juce::dontSendNotification);
    aggregateNameLabel.setText ("Combined device name", juce::dontSendNotification);
    destinationFolderLabel.setText ("Destination folder", juce::dontSendNotification);

    outputDeviceCombo.setTitle ("Output device");
    outputDeviceCombo.setDescription ("Choose where the live monitor mix is heard.");
    sampleRateCombo.setTitle ("Sample rate");
    bitDepthCombo.setTitle ("Bit depth");
    bufferSizeCombo.setTitle ("Buffer size");
    storageCombo.setTitle ("Save recordings to");
    deliveryCombo.setTitle ("Delivery loudness target");
    aggregateNameEditor.setTitle ("Combined device name");

    for (auto* c : { &sampleRateLabel, &bitDepthLabel,
                     &bufferSizeLabel, &latencyLabel, &latencyValue,
                     &clockMasterLabel, &driftLabel, &outputDeviceLabel, &backendLabel,
                     &backendValue, &destinationFolderLabel })
        addAndMakeVisible (c);

    // Two tones rather than one. Every row rendered at the same weight, so the
    // question and its answer were indistinguishable down a column.
    for (auto* l : { &sampleRateLabel, &bitDepthLabel, &bufferSizeLabel, &latencyLabel,
                     &clockMasterLabel, &outputDeviceLabel, &backendLabel,
                     &aggregateNameLabel, &destinationFolderLabel, &storageLabel,
                     &micSelectionLabel })
        l->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);

    for (auto* v : { &latencyValue, &backendValue })
        v->setColour (juce::Label::textColourId, AppLookAndFeel::bone);

    activityLabel.setJustificationType (juce::Justification::topLeft);
    activityLabel.setText ("Nothing to report yet.", juce::dontSendNotification);
    addAndMakeVisible (activityLabel);

    // Supporting text, not body text: the drift report, the aggregate status,
    // the clock-master explanation and the activity list all sat at the same
    // size and weight as the settings they describe.
    for (auto* l : { &driftLabel, &aggregateStatusLabel, &clockMasterHelpLabel, &activityLabel })
    {
        l->setFont (juce::Font (11.0f));
        l->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    }

    // Not a picker. The clock master is this computer, always: every
    // microphone is corrected onto the machine's own clock, so there is
    // nothing to choose and a choice would have had no audible consequence.
    clockMasterValue.setText ("This computer", juce::dontSendNotification);
    clockMasterValue.setColour (juce::Label::textColourId, AppLookAndFeel::bone);
    addAndMakeVisible (clockMasterValue);

    headphonesLabel.setText ("Who hears the mix in their headphones", juce::dontSendNotification);
    headphonesLabel.setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    addChildComponent (headphonesLabel);

    addAndMakeVisible (outputDeviceCombo);
    outputDeviceCombo.onChange = [this] {
        if (onOutputDeviceChanged)
            onOutputDeviceChanged (outputDeviceCombo.getText());
    };

    // The three format controls. These were read-only lines, and the app once
    // told a user to change one of them here -- so the instruction on screen
    // named a control that did not exist. Like Audio MIDI Setup: pick it, the
    // app tries it, and if the hardware refuses the main screen says so.
    addAndMakeVisible (bitDepthCombo);
    bitDepthCombo.onChange = [this] {
        if (onBitDepthChanged && bitDepthCombo.getSelectedId() > 0)
            onBitDepthChanged (bitDepthCombo.getSelectedId());
    };

    addAndMakeVisible (bufferSizeCombo);
    bufferSizeCombo.onChange = [this] {
        if (! onBufferSizeChanged)
            return;

        // Item 1 is Automatic; every other id is the size in samples.
        const int id = bufferSizeCombo.getSelectedId();
        onBufferSizeChanged (id <= 1 ? 0 : id);
    };

    addAndMakeVisible (sampleRateCombo);
    sampleRateCombo.onChange = [this] {
        if (! onSampleRateChanged)
            return;

        // Item 1 is Automatic. The ids above it are the rate in Hz, so the
        // selection carries its own meaning and no parallel table can drift.
        const int id = sampleRateCombo.getSelectedId();
        onSampleRateChanged (id <= 1 ? 0u : static_cast<uint32_t> (id));
    };

    // A click on a picker leaves the keyboard where it was. Otherwise the last
    // picker clicked kept focus, and Up/Down -- pressed for anything at all --
    // quietly switched the headphone output, save drive, rate or bit depth.
    // Tab still reaches them.
    for (auto* c : { &outputDeviceCombo, &storageCombo, &sampleRateCombo,
                     &bitDepthCombo, &bufferSizeCombo, &deliveryCombo, &showsCombo })
        c->setMouseClickGrabsKeyboardFocus (false);

    // Choosing a show in the list only selects it. Loading changes the whole
    // rig, so it waits for the button rather than happening on a stray pick.
    showsCombo.setTitle ("Saved shows");
    showsCombo.setTextWhenNothingSelected ("Choose a saved show");
    showsCombo.setTextWhenNoChoicesAvailable ("No saved shows yet");
    showsCombo.onChange = [this] { updateShowButtons(); };
    addAndMakeVisible (showsCombo);

    loadShowButton.onClick = [this] {
        if (onLoadShowClicked && getSelectedShow().isNotEmpty())
            onLoadShowClicked (getSelectedShow());
    };
    deleteShowButton.onClick = [this] {
        if (onDeleteShowClicked && getSelectedShow().isNotEmpty())
            onDeleteShowClicked (getSelectedShow());
    };
    saveShowButton.onClick = [this] { if (onSaveShowClicked) onSaveShowClicked(); };

    for (auto* b : { &loadShowButton, &deleteShowButton, &saveShowButton })
        addAndMakeVisible (b);

    updateShowButtons();

    trimViewport.setViewedComponent (&trimContainer, false);
    addAndMakeVisible (trimViewport);

    copyToCardToggle.setToggleState (true, juce::dontSendNotification); // default on
    copyToCardToggle.onClick = [this]
    { if (onCopyToCardToggled) onCopyToCardToggled (copyToCardToggle.getToggleState()); };
    addAndMakeVisible (copyToCardToggle);

    copyToCardNote.setText ("Gentler on SD cards and USB sticks. Turn off to record straight to the card.",
                            juce::dontSendNotification);
    copyToCardNote.setFont (juce::Font (12.0f, juce::Font::italic));
    copyToCardNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    addAndMakeVisible (copyToCardNote);

    mirrorToggle.setToggleState (true, juce::dontSendNotification); // §6.3 default on
    mirrorToggle.onClick = [this] { if (onMirrorToggled) onMirrorToggled (mirrorToggle.getToggleState()); };
    addAndMakeVisible (mirrorToggle);

    mirrorNote.setText ("in case the first copy gets a little damp", juce::dontSendNotification);
    mirrorNote.setFont (juce::Font (12.0f, juce::Font::italic));
    mirrorNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    addAndMakeVisible (mirrorNote);

    deliveryLabel.setText ("Aim the loudness at", juce::dontSendNotification);
    addAndMakeVisible (deliveryLabel);

    deliveryCombo.onChange = [this]
    {
        if (! onDeliveryTargetChanged)
            return;

        // Item 1 is "Not delivering anywhere", which is the absence of a
        // choice rather than a choice -- so it reports as empty.
        const int index = deliveryCombo.getSelectedId() - 2;
        onDeliveryTargetChanged (index >= 0 && index < deliveryNames.size()
                                     ? deliveryNames[index] : juce::String());
    };
    addAndMakeVisible (deliveryCombo);

    deliveryNote.setText ("Every streaming service turns everything it plays to the same "
                          "loudness, so how loud you record decides what people hear -- "
                          "and the peak meters can't tell you. Your recording is never changed: "
                          "this measures the mix and says which way to move. After each take, "
                          "a ready-to-upload copy of the mix at this loudness is saved beside it.",
                          juce::dontSendNotification);
    deliveryNote.setJustificationType (juce::Justification::topLeft);
    deliveryNote.setMinimumHorizontalScale (1.0f);
    deliveryNote.setFont (juce::Font (12.0f));
    deliveryNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    addAndMakeVisible (deliveryNote);

    loudnessAdviceLabel.setJustificationType (juce::Justification::topLeft);
    loudnessAdviceLabel.setMinimumHorizontalScale (1.0f);
    loudnessAdviceLabel.setFont (juce::Font (13.0f));
    loudnessAdviceLabel.setColour (juce::Label::textColourId, AppLookAndFeel::accent);
    addAndMakeVisible (loudnessAdviceLabel);

    destinationFolderButton.onClick = [this] { if (onDestinationFolderClicked) onDestinationFolderClicked(); };
    addAndMakeVisible (destinationFolderButton);

    diagnosticsExportButton.onClick = [this] { if (onDiagnosticsExportClicked) onDiagnosticsExportClicked(); };
    addAndMakeVisible (diagnosticsExportButton);

    // Off by default: PRIVACY.md says the app sends nothing anywhere, and this
    // is the one request it can make, so it is the user's to switch on. The
    // note says what that costs them in the same breath as the switch.
    checkForUpdatesToggle.setToggleState (false, juce::dontSendNotification);
    checkForUpdatesToggle.onClick = [this]
    { if (onCheckForUpdatesToggled) onCheckForUpdatesToggled (checkForUpdatesToggle.getToggleState()); };
    addAndMakeVisible (checkForUpdatesToggle);

    checkForUpdatesNote.setText ("Contacts GitHub once a day to look for a new version. Sends nothing about you.",
                                 juce::dontSendNotification);
    checkForUpdatesNote.setFont (juce::Font (12.0f, juce::Font::italic));
    checkForUpdatesNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    addAndMakeVisible (checkForUpdatesNote);

    // Works with the switch off: pressing it is the user asking, today, once.
    checkNowButton.onClick = [this] { if (onCheckForUpdatesNowClicked) onCheckForUpdatesNowClicked(); };
    addAndMakeVisible (checkNowButton);

    updateStatusLabel.setFont (juce::Font (13.0f));
    updateStatusLabel.setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    addAndMakeVisible (updateStatusLabel);

    // Opens the release page in the browser. Nothing is downloaded or
    // installed behind the user's back; they take it from there.
    downloadUpdateButton.onClick = [this] { if (onDownloadUpdateClicked) onDownloadUpdateClicked(); };
    addChildComponent (downloadUpdateButton);

    closeButton.onClick = [this] { if (onCloseClicked) onCloseClicked(); };
    addAndMakeVisible (closeButton);

    helpButton.onClick = [this] { if (onHelpClicked) onHelpClicked(); };
    addAndMakeVisible (helpButton);

    setupGuideButton.onClick = [this] { if (onSetupGuideClicked) onSetupGuideClicked(); };
    addAndMakeVisible (setupGuideButton);

    // Four headings over what was a flat list. The reader can now find the
    // storage picker by scanning four words instead of reading fifteen rows.
    const std::pair<juce::Label*, const char*> sections[] = {
        { &showsSection,   "SHOWS" },
        { &storageSection, "WHERE RECORDINGS GO" },
        { &formatSection,  "RECORDING FORMAT" },
        { &deliverySection, "LOUDNESS FOR STREAMING" },
        { &micSection,     "MICROPHONES" },
        { &outputSection,  "MONITORING AND OUTPUT" },
        { &updatesSection, "UPDATES" },
        { &activitySection, "WHAT HAPPENED" },
    };

    for (auto& [label, text] : sections)
    {
        label->setText (text, juce::dontSendNotification);
        label->setFont (juce::Font (10.0f, juce::Font::bold));
        label->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
        addAndMakeVisible (label);
    }

    micSelectionLabel.setText ("Microphones to record", juce::dontSendNotification);
    addAndMakeVisible (micSelectionLabel);

    storageLabel.setText ("Save recordings to", juce::dontSendNotification);
    addAndMakeVisible (storageLabel);

    storageCombo.onChange = [this] {
        const int index = storageCombo.getSelectedItemIndex();

        if (index >= 0 && index < storagePaths.size() && onStorageVolumeChosen)
            onStorageVolumeChosen (storagePaths[index]);
    };
    addAndMakeVisible (storageCombo);

    // §10.3 says nothing behind this door may be a gate the novice must pass,
    // which makes an unexplained term worse than no term: someone who does not
    // know what a clock master is cannot tell whether they need to care.
    clockMasterHelpLabel.setText (
        "Every USB microphone runs on its own crystal, and no two tick at exactly "
        "the same rate. All of them are continuously nudged to match this "
        "computer's clock, so the tracks stay lined up over a long take. "
        "There is nothing to set here.",
        juce::dontSendNotification);
    clockMasterHelpLabel.setJustificationType (juce::Justification::topLeft);
    clockMasterHelpLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (clockMasterHelpLabel);

    // The name other apps see this rig under. Applied when typing finishes,
    // not per keystroke -- each change replaces a device other apps may be
    // recording from.
    addAndMakeVisible (aggregateNameLabel);
    aggregateNameEditor.setTextToShowWhenEmpty ("SobStage", juce::Colours::grey);
    aggregateNameEditor.onReturnKey = [this]
    {
        if (onAggregateNameChanged)
            onAggregateNameChanged (aggregateNameEditor.getText());

        // Return is "done". The box kept the keyboard, so the next Space --
        // the room's mute, live beside this drawer -- typed into the name
        // instead. The window passes the focus on to the screen that mutes.
        if (auto* window = getTopLevelComponent(); window != nullptr && window != this)
            window->grabKeyboardFocus();
    };
    aggregateNameEditor.onFocusLost = [this] { if (onAggregateNameChanged) onAggregateNameChanged (aggregateNameEditor.getText()); };
    addAndMakeVisible (aggregateNameEditor);
    aggregateStatusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (aggregateStatusLabel);
}

AdvancedPanel::~AdvancedPanel() = default;

namespace {
/// A rate as a person says it: "44.1 kHz", not "44100".
juce::String rateText (uint32_t rate)
{
    const double khz = rate / 1000.0;

    return std::abs (khz - std::floor (khz)) < 0.01
         ? juce::String (static_cast<int> (khz)) + " kHz"
         : juce::String (khz, 1) + " kHz";
}

void fillCombo (juce::ComboBox& combo, const juce::StringArray& names, const juce::String& selected)
{
    combo.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        combo.addItem (names[i], i + 1);

    const auto index = names.indexOf (selected);

    // dontSendNotification throughout: repopulating the list must not look like
    // the user having chosen something.
    if (index >= 0)
        combo.setSelectedId (index + 1, juce::dontSendNotification);
}
} // namespace

void AdvancedPanel::setOutputDevices (const juce::StringArray& names, const juce::String& selected)
{
    juce::String signature;
    for (const auto& name : names)
        signature += name + "\n";
    signature += "@" + selected;

    // This is called on the Settings refresh tick. Rebuilding an unchanged
    // combo twice a second blanked its selection and could close the menu under
    // someone using it; a snapshot with the same options and choice is a no-op.
    if (signature == lastOutputSignature)
        return;

    lastOutputSignature = signature;
    fillCombo (outputDeviceCombo, names, selected);
}

void AdvancedPanel::setShows (const juce::StringArray& names, const juce::String& select)
{
    const auto keep = select.isNotEmpty() ? select : getSelectedShow();

    showNames = names;
    fillCombo (showsCombo, names, keep);
    updateShowButtons();
}

juce::String AdvancedPanel::getSelectedShow() const
{
    const int index = showsCombo.getSelectedId() - 1;
    return index >= 0 && index < showNames.size() ? showNames[index] : juce::String();
}

void AdvancedPanel::updateShowButtons()
{
    const bool chosen = getSelectedShow().isNotEmpty();
    loadShowButton.setEnabled (chosen);
    deleteShowButton.setEnabled (chosen);
}

void AdvancedPanel::setSampleRates (const std::vector<uint32_t>& rates, uint32_t current)
{
    // Rebuilt only when the offered set or the selection changes: this runs on
    // the panel's refresh tick, and repopulating under the user's cursor would
    // fight a menu they have open.
    juce::String signature;
    for (auto r : rates)
        signature += juce::String (static_cast<int> (r)) + ",";
    signature += "@" + juce::String (static_cast<int> (current));

    if (signature == lastSampleRateSignature)
        return;

    lastSampleRateSignature = signature;
    sampleRateCombo.clear (juce::dontSendNotification);

    // "Automatic" first and selected by default, because staying on whatever
    // the hardware is already doing is what works; the explicit rates are the
    // escape hatch, not the ordinary path.
    sampleRateCombo.addItem ("Automatic (" + rateText (current) + ")", 1);

    for (auto r : rates)
        sampleRateCombo.addItem (rateText (r), static_cast<int> (r));

    sampleRateCombo.setSelectedId (1, juce::dontSendNotification);
}

void AdvancedPanel::setBitDepthChoice (int current)
{
    const juce::String signature (current);

    if (signature == lastBitDepthSignature)
        return;

    lastBitDepthSignature = signature;
    bitDepthCombo.clear (juce::dontSendNotification);

    for (int bits : { 16, 24, 32 })
        bitDepthCombo.addItem (juce::String (bits) + "-bit", bits);

    bitDepthCombo.setSelectedId (current, juce::dontSendNotification);
}

void AdvancedPanel::setBufferSizeChoice (int current, int pinned)
{
    const juce::String signature = juce::String (current) + "@" + juce::String (pinned);

    if (signature == lastBufferSignature)
        return;

    lastBufferSignature = signature;
    bufferSizeCombo.clear (juce::dontSendNotification);

    // Automatic first: §5.4's ladder starts small and steps up only when the
    // machine cannot keep up, which is what most people want without knowing
    // they want it. The fixed sizes are for someone who does know.
    bufferSizeCombo.addItem ("Automatic (" + juce::String (current) + " samples)", 1);

    for (int size : { 64, 128, 256, 512, 1024 })
        bufferSizeCombo.addItem (juce::String (size) + " samples", size);

    bufferSizeCombo.setSelectedId (pinned > 0 ? pinned : 1, juce::dontSendNotification);
}

void AdvancedPanel::setSampleRateSelection (uint32_t chosen)
{
    if (sampleRateCombo.getNumItems() == 0)
        return;

    const int id = chosen == 0 ? 1 : static_cast<int> (chosen);

    // A saved explicit rate remains meaningful while every eligible external
    // microphone is unplugged. In that state the live capability list is
    // empty, so selecting the saved ID used to clear the ComboBox and leave a
    // blank setting. Keep the stored choice visible (and selectable) until the
    // rig returns and the normal capability refresh rebuilds the menu.
    if (chosen != 0 && sampleRateCombo.indexOfItemId (id) < 0)
        sampleRateCombo.addItem (rateText (chosen), id);

    if (sampleRateCombo.getSelectedId() != id)
        sampleRateCombo.setSelectedId (id, juce::dontSendNotification);
}

void AdvancedPanel::setMicSelections (const std::vector<MicChoice>& mics)
{
    // Rebuilt only when the set of rows changes. The panel repaints at 2 Hz,
    // and recreating the toggles every tick would fight the user's click.
    // Keyed on identity as well as label: identical mics share a label, so
    // two of them swapping places after a replug changed nothing visible and
    // left each box switching the other one.
    juce::StringArray incoming;
    for (const auto& m : mics)
    {
        incoming.add (m.deviceKey + "\n" + m.label);
        for (const auto& in : m.inputs)
            incoming.add ("  " + in.label);
    }

    if (incoming != lastMicNames)
    {
        micToggles.clear();
        micToggleIsInput.clear();
        lastMicNames = incoming;

        // Deferred rather than called straight through. Ticking a box rebuilds
        // the audio streams, and a rebuild can reach back into this panel;
        // destroying a button from inside its own click handler is a crash
        // JUCE gives no warning about. Reading the state here and doing the
        // work on the next message keeps the button alive for the whole of
        // its own callback.
        //
        // The SafePointer is built outside the inner lambda and captured by
        // copy rather than constructed in its init-capture: inside a nested
        // lambda MSVC resolves `this` to the enclosing closure object, so the
        // init-capture form compiled on Clang and GCC and failed on Windows.
        for (const auto& m : mics)
        {
            auto toggle = std::make_unique<juce::ToggleButton> (m.label);
            const auto name = m.deviceKey; // the identity the app looks up by

            toggle->onClick = [this, name, raw = toggle.get()] {
                const bool state = raw->getToggleState();
                juce::Component::SafePointer<AdvancedPanel> safe (this);
                juce::MessageManager::callAsync ([safe, name, state] {
                    if (safe != nullptr && safe->onMicEnabledChanged)
                        safe->onMicEnabledChanged (name, state);
                });
            };
            addAndMakeVisible (*toggle);
            micToggles.push_back (std::move (toggle));
            micToggleIsInput.push_back (false);

            for (const auto& in : m.inputs)
            {
                auto inputToggle = std::make_unique<juce::ToggleButton> (in.label);
                const int input = in.index;

                inputToggle->onClick = [this, name, input, raw = inputToggle.get()] {
                    const bool state = raw->getToggleState();
                    juce::Component::SafePointer<AdvancedPanel> safe (this);
                    juce::MessageManager::callAsync ([safe, name, input, state] {
                        if (safe != nullptr && safe->onInputEnabledChanged)
                            safe->onInputEnabledChanged (name, input, state);
                    });
                };
                addAndMakeVisible (*inputToggle);
                micToggles.push_back (std::move (inputToggle));
                micToggleIsInput.push_back (true);
            }
        }

        resized();
    }

    // State is refreshed every tick regardless, so a change made elsewhere --
    // the 8-mic cap, a device leaving -- shows up here. A socket row is greyed
    // while its box is switched off: the box's tick governs the lot.
    size_t row = 0;
    for (const auto& m : mics)
    {
        if (row >= micToggles.size()) break;
        micToggles[row++]->setToggleState (m.enabled, juce::dontSendNotification);

        for (const auto& in : m.inputs)
        {
            if (row >= micToggles.size()) break;
            micToggles[row]->setToggleState (in.enabled, juce::dontSendNotification);
            micToggles[row]->setEnabled (m.enabled);
            ++row;
        }
    }
}

void AdvancedPanel::setHeadphoneChoices (const std::vector<HeadphoneChoice>& choices)
{
    juce::StringArray incoming;
    for (const auto& c : choices)
        incoming.add (c.key + "\n" + c.label);

    // Rebuilt only when the set of people changes, like the microphone list:
    // recreating a tick box on every refresh would fight the user's click.
    if (incoming != lastHeadphoneNames)
    {
        headphoneToggles.clear();
        lastHeadphoneNames = incoming;

        for (const auto& c : choices)
        {
            auto toggle = std::make_unique<juce::ToggleButton> (c.label);
            toggle->setTitle (c.label + " headphones");
            toggle->setDescription ("Whether this person hears everyone in the headphones plugged into their microphone.");
            const auto name = c.key;

            toggle->onClick = [this, name, raw = toggle.get()] {
                const bool state = raw->getToggleState();
                juce::Component::SafePointer<AdvancedPanel> safe (this);
                juce::MessageManager::callAsync ([safe, name, state] {
                    if (safe != nullptr && safe->onHeadphonesToggled)
                        safe->onHeadphonesToggled (name, state);
                });
            };
            addAndMakeVisible (*toggle);
            headphoneToggles.push_back (std::move (toggle));
        }

        headphonesLabel.setVisible (! headphoneToggles.empty());
        resized();
    }

    for (size_t i = 0; i < choices.size() && i < headphoneToggles.size(); ++i)
        headphoneToggles[i]->setToggleState (choices[i].on, juce::dontSendNotification);
}

void AdvancedPanel::setStorageVolumes (const std::vector<VolumeChoice>& volumes)
{
    juce::StringArray labels;
    for (const auto& v : volumes)
        labels.add (v.label);

    // Rebuilt only when the set of volumes changes -- a card being plugged in
    // or pulled out. Repopulating on every tick would close the menu under
    // someone in the middle of choosing from it.
    if (labels != lastStorageLabels)
    {
        lastStorageLabels = labels;
        storagePaths.clear();
        storageCombo.clear (juce::dontSendNotification);

        int id = 1;
        for (const auto& v : volumes)
        {
            storageCombo.addItem (v.label, id++);
            storagePaths.add (v.path);
        }
    }

    for (size_t i = 0; i < volumes.size(); ++i)
        if (volumes[i].current)
            storageCombo.setSelectedItemIndex (static_cast<int> (i), juce::dontSendNotification);
}

void AdvancedPanel::setTrimChannels (const juce::StringArray& micNames,
                                     const std::function<float (int)>& currentTrimDb)
{
    trimNameLabels.clear();
    trimSliders.clear();

    for (int i = 0; i < micNames.size(); ++i)
    {
        auto label = std::make_unique<juce::Label>();
        label->setText (micNames[i], juce::dontSendNotification);
        trimContainer.addAndMakeVisible (*label);
        trimNameLabels.push_back (std::move (label));

        auto slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                      juce::Slider::TextBoxRight);
        // §4: -20..+20 dB in 0.5 dB steps, defaulting to 0.
        slider->setRange (-20.0, 20.0, 0.5);

        // Inside a scrolling panel: a trackpad scroll passing over a row was
        // changing that person's level instead of moving the panel.
        slider->setScrollWheelEnabled (false);
        slider->setTextValueSuffix (" dB");
        slider->setTitle (micNames[i] + " monitor trim");
        slider->setDescription ("Adjust this microphone in the headphones only; the recorded track stays unchanged.");
        slider->setValue (currentTrimDb != nullptr ? currentTrimDb (i) : 0.0,
                          juce::dontSendNotification);

        auto* raw = slider.get();
        slider->onValueChange = [this, raw, i] {
            if (onTrimChanged)
                onTrimChanged (i, static_cast<float> (raw->getValue()));
        };

        trimContainer.addAndMakeVisible (*slider);
        trimSliders.push_back (std::move (slider));
    }

    layOutTrimRows();
}

void AdvancedPanel::setTrimValues (const std::function<float (int)>& currentTrimDb)
{
    if (currentTrimDb == nullptr)
        return;

    for (size_t i = 0; i < trimSliders.size(); ++i)
    {
        auto& slider = *trimSliders[i];

        if (slider.isMouseButtonDown())
            continue;

        const double value = currentTrimDb (static_cast<int> (i));

        if (slider.getValue() != value)
            slider.setValue (value, juce::dontSendNotification);
    }
}

void AdvancedPanel::layOutTrimRows()
{
    constexpr int rowHeight = 26;
    const int width = juce::jmax (200, trimViewport.getWidth() - trimViewport.getScrollBarThickness());

    trimContainer.setSize (width, rowHeight * static_cast<int> (trimSliders.size()));

    for (size_t i = 0; i < trimSliders.size(); ++i)
    {
        juce::Rectangle<int> row (0, rowHeight * static_cast<int> (i), width, rowHeight);
        trimNameLabels[i]->setBounds (row.removeFromLeft (width / 3));
        trimSliders[i]->setBounds (row);
    }
}

void AdvancedPanel::paint (juce::Graphics& g)
{
    g.fillAll (AppLookAndFeel::surface);

    // One hairline under each heading, in the outline tone. Anything heavier
    // turns a grouping cue into four more lines competing with the settings.
    g.setColour (AppLookAndFeel::outline);
    const auto bounds = getLocalBounds().reduced (12, 0);

    for (int y : ruleYs)
        g.fillRect (bounds.getX(), y, bounds.getWidth(), 1);
}

int AdvancedPanel::getRequiredHeight() const
{
    // Mirrors resized() below. Kept as an explicit sum rather than measured
    // from a trial layout, because resized() consumes the bounds it is given
    // and cannot report what it would have wanted from a taller one.
    constexpr int kMargins       = 12 * 2;
    constexpr int kCloseButton   = 36 + 14;
    constexpr int kSection       = 14 + 3 + 1 + 9;  // heading, gap, rule, gap
    constexpr int kRow           = 32 + 4;
    constexpr int kMicListLabel  = 22;
    constexpr int kMicToggle     = 28 + 2;
    constexpr int kClockHelp     = 84 + 8;
    constexpr int kDrift         = 60 + 4;
    constexpr int kTrimViewport  = 100 + 16;
    constexpr int kAggregate     = 20 + 16;
    constexpr int kCopyToCard    = 28 + 32;  // switch, and its note on up to two lines
    constexpr int kMirror        = 28 + 16;
    constexpr int kDiagnostics   = 36;
    constexpr int kUpdates       = 28 + 20 + 32 + 16; // switch, note, Check now row, gap
    constexpr int kActivity      = 16 + kActivityHeight;

    // save-to volume, destination folder, sample rate, bit depth, buffer size,
    // latency, delivery target, clock master, output device, backend,
    // aggregate name.
    constexpr int kRowCount = 11;

    // The gaps resized() leaves between sections that are not a heading's own:
    // after the format rows, after the delivery block, and after the microphone
    // checkbox list.
    constexpr int kSectionGaps = 12 + 12 + 10;

    // The backup copy's note.
    constexpr int kMirrorNote  = 20;

    // "Where it's going": the explanation and the line of advice under it.
    constexpr int kDelivery    = 100 + 4 + 36;

    // The show picker with its buttons, the Save row, and the gap after them.
    constexpr int kShows       = 32 + 4 + 32 + 16;

    const int headphoneRows = headphoneToggles.empty()
        ? 0 : kMicListLabel + static_cast<int> (headphoneToggles.size()) * kMicToggle + 6;

    return kMargins + kCloseButton + (kSection * 7) + kShows + (kRow * kRowCount) + headphoneRows
         + kMicListLabel + static_cast<int> (micToggles.size()) * kMicToggle
         + kSectionGaps + kClockHelp + kDrift + kTrimViewport + kAggregate
         + kCopyToCard + kMirror + kMirrorNote + kDelivery + kDiagnostics + kUpdates + kActivity;
}

void AdvancedPanel::resized()
{
    auto area = getLocalBounds().reduced (12);
    ruleYs.clear();

    // Top-left and first in the layout, where a back control is looked for,
    // and placed before anything else claims the space so it cannot be pushed
    // off the bottom by a long device list.
    {
        auto top = area.removeFromTop (36);
        closeButton.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (8);
        helpButton.setBounds (top.removeFromLeft (64));
        top.removeFromLeft (8);
        // Whatever the row has left, up to what the words need: at the
        // narrowest drawer that is still the whole label.
        setupGuideButton.setBounds (top.removeFromLeft (juce::jmin (170, top.getWidth())));
    }
    area.removeFromTop (14);

    // A heading, then the hairline paint() draws under it. The gap below the
    // rule is wider than the one above the next heading, so a section reads as
    // one block rather than as rows that happen to be adjacent.
    auto section = [&] (juce::Label& heading) {
        heading.setBounds (area.removeFromTop (14));
        area.removeFromTop (3);
        ruleYs.push_back (area.getY());
        area.removeFromTop (9);
    };

    // The value column is capped rather than taking the whole right half. At
    // half the panel a combo holding "root - 29.8 GB free" stretched to 340px
    // of mostly empty well, and short labels sat a long way from their values.
    constexpr int kValueWidth = 300;

    // 32px rows: a combo or button that can be hit without aiming, and the
    // same height as the rest of the app's controls.
    auto row = [&] (juce::Label& label, juce::Component& value) {
        auto r = area.removeFromTop (32);
        value.setBounds (r.removeFromRight (juce::jmin (kValueWidth, r.getWidth() * 3 / 5)));
        r.removeFromRight (12);
        label.setBounds (r);
        area.removeFromTop (4);
    };

    // Shows first: loading one sets most of what is below, so it is the place
    // to start rather than something found after scrolling past it all.
    section (showsSection);
    {
        auto r = area.removeFromTop (32);
        deleteShowButton.setBounds (r.removeFromRight (72));
        r.removeFromRight (6);
        loadShowButton.setBounds (r.removeFromRight (72));
        r.removeFromRight (6);
        showsCombo.setBounds (r);
        area.removeFromTop (4);
        saveShowButton.setBounds (area.removeFromTop (32).removeFromLeft (180));
    }
    area.removeFromTop (16);

    // Where recordings go comes first, above the read-only format rows. It is
    // the choice a user comes in here to make -- picking an SD card before a
    // take -- and at the bottom of the panel it was below the fold, found only
    // by scrolling past four values nobody can change.
    section (storageSection);
    row (storageLabel, storageCombo);
    row (destinationFolderLabel, destinationFolderButton);

    // Where a take is written while it records is the first storage decision
    // after where it ends up.
    copyToCardToggle.setBounds (area.removeFromTop (28));
    copyToCardNote.setBounds (area.removeFromTop (32).reduced (20, 0));

    // The backup copy is a storage decision, so it belongs with the other two
    // rather than orphaned at the bottom between the aggregate device and the
    // diagnostics button.
    mirrorToggle.setBounds (area.removeFromTop (28));
    mirrorNote.setBounds (area.removeFromTop (20).reduced (20, 0));

    area.removeFromTop (16);

    section (formatSection);
    row (sampleRateLabel, sampleRateCombo);
    row (bitDepthLabel, bitDepthCombo);
    row (bufferSizeLabel, bufferSizeCombo);
    row (latencyLabel, latencyValue);
    area.removeFromTop (12);

    section (deliverySection);
    row (deliveryLabel, deliveryCombo);
    deliveryNote.setBounds (area.removeFromTop (100));
    area.removeFromTop (4);
    loudnessAdviceLabel.setBounds (area.removeFromTop (36));
    area.removeFromTop (12);

    section (micSection);
    micSelectionLabel.setBounds (area.removeFromTop (22));
    for (size_t i = 0; i < micToggles.size(); ++i)
    {
        // A socket row sits indented under its box, so the list reads as a
        // box with people on it rather than a flat run of identical ticks.
        const bool isInput = i < micToggleIsInput.size() && micToggleIsInput[i];
        auto rowArea = area.removeFromTop (28).reduced (8, 0);
        if (isInput) rowArea.removeFromLeft (28);
        micToggles[i]->setBounds (rowArea);
        area.removeFromTop (2);
    }
    area.removeFromTop (10);

    row (clockMasterLabel, clockMasterValue);
    clockMasterHelpLabel.setBounds (area.removeFromTop (84));
    area.removeFromTop (8);

    driftLabel.setBounds (area.removeFromTop (60));
    area.removeFromTop (4);

    trimViewport.setBounds (area.removeFromTop (100));
    layOutTrimRows();
    area.removeFromTop (16);

    section (outputSection);
    row (outputDeviceLabel, outputDeviceCombo);

    if (! headphoneToggles.empty())
    {
        headphonesLabel.setBounds (area.removeFromTop (22));
        for (auto& toggle : headphoneToggles)
        {
            toggle->setBounds (area.removeFromTop (28).reduced (8, 0));
            area.removeFromTop (2);
        }
        area.removeFromTop (6);
    }

    row (backendLabel, backendValue);
    row (aggregateNameLabel, aggregateNameEditor);
    aggregateStatusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (16);

    diagnosticsExportButton.setBounds (area.removeFromTop (36).removeFromLeft (180));
    area.removeFromTop (16);

    section (updatesSection);
    checkForUpdatesToggle.setBounds (area.removeFromTop (28));
    checkForUpdatesNote.setBounds (area.removeFromTop (20).reduced (20, 0));
    {
        // Check now, then what it found, then Download when there is
        // something to download -- one line, read left to right.
        auto r = area.removeFromTop (32);
        checkNowButton.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (12);

        if (downloadUpdateButton.isVisible())
        {
            downloadUpdateButton.setBounds (r.removeFromRight (110));
            r.removeFromRight (12);
        }

        updateStatusLabel.setBounds (r);
    }
    area.removeFromTop (16);

    // Last, and deliberately: it is the thing you come looking for rather than
    // the thing you set, and it grows downwards without pushing a control off
    // the bottom of the panel.
    section (activitySection);
    activityLabel.setBounds (area.removeFromTop (kActivityHeight));
}


void AdvancedPanel::setUpdateState (bool checkEnabled, const juce::String& statusText,
                                    bool updateAvailable, bool canCheckNow)
{
    checkForUpdatesToggle.setToggleState (checkEnabled, juce::dontSendNotification);
    checkNowButton.setEnabled (canCheckNow);

    // A newer version is the one thing here worth the accent colour.
    updateStatusLabel.setColour (juce::Label::textColourId,
                                 updateAvailable ? AppLookAndFeel::accent : AppLookAndFeel::secondary);
    updateStatusLabel.setText (statusText, juce::dontSendNotification);

    // Laid out again only when Download comes or goes. This runs on the
    // refresh tick, and a relayout twice a second for nothing is waste.
    if (downloadUpdateButton.isVisible() != updateAvailable)
    {
        downloadUpdateButton.setVisible (updateAvailable);
        resized();
    }
}

void AdvancedPanel::setDeliveryTargets (const juce::StringArray& names, const juce::String& chosen)
{
    if (names != deliveryNames)
    {
        deliveryNames = names;
        deliveryCombo.clear (juce::dontSendNotification);

        // First, and the default: most takes are not being delivered anywhere,
        // and telling someone recording a rehearsal that they are 8 dB under
        // Spotify is noise.
        deliveryCombo.addItem ("Not delivering anywhere", 1);

        for (int i = 0; i < names.size(); ++i)
            deliveryCombo.addItem (names[i], i + 2);
    }

    const int index = deliveryNames.indexOf (chosen);
    deliveryCombo.setSelectedId (index >= 0 ? index + 2 : 1, juce::dontSendNotification);
}

void AdvancedPanel::setLoudnessAdvice (const juce::String& text)
{
    loudnessAdviceLabel.setText (text, juce::dontSendNotification);
}

void AdvancedPanel::setActivityLines (const juce::StringArray& lines)
{
    // Said plainly when there is nothing, rather than left blank: an empty
    // space under a heading reads as broken, and "nothing has gone wrong" is
    // itself worth knowing.
    // One row per entry. The label is a fixed height, so a long entry that
    // wrapped onto four lines pushed the oldest rows off the bottom, cut off
    // mid-line. Shorten the display only; the take's activity.log keeps it all.
    constexpr int kMaxRowCharacters = 140;
    juce::StringArray rows;

    for (const auto& line : lines)
        rows.add (line.length() <= kMaxRowCharacters
                      ? line
                      : line.substring (0, kMaxRowCharacters).trimEnd()
                            + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")));

    activityLabel.setText (rows.isEmpty() ? juce::String ("Nothing to report yet.")
                                          : rows.joinIntoString ("\n"),
                           juce::dontSendNotification);
}

} // namespace mma
