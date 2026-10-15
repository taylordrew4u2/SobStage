#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include <cstdint>
#include <memory>
#include <utility>

namespace mma {

/// §10.3: the Advanced panel's exhaustive contents. One door, one click,
/// nothing behind it required for correct operation -- everything here is
/// informational/optional, never a gate the novice must pass through.
class AdvancedPanel : public juce::Component
{
public:
    AdvancedPanel();
    ~AdvancedPanel() override;

    void resized() override;

    /// The height this panel's content needs at its current item count.
    /// The panel is taller than most windows once a few microphones are
    /// listed, so a container scrolls it rather than clipping it -- and a
    /// container can only do that if it is told how tall to make it.
    int getRequiredHeight() const;
    void paint (juce::Graphics& g) override;

    /// Bit depth as a control. 16, 24 or 32; `current` is what the next take
    /// will use.
    void setBitDepthChoice (int current);
    std::function<void (int)> onBitDepthChanged;

    /// The rates this rig can actually record at, and the one in use.
    ///
    /// This was a read-only line, and the app told people to change it here --
    /// so the one instruction on screen named a control that did not exist.
    void setSampleRates (const std::vector<uint32_t>& rates, uint32_t current);

    /// Reflects the stored choice without firing onSampleRateChanged.
    void setSampleRateSelection (uint32_t chosen);

    /// "Automatic" or a rate in Hz; 0 means automatic.
    std::function<void (uint32_t)> onSampleRateChanged;
    /// Buffer size as a control. `current` is the size in use; `pinned` is the
    /// user's choice, 0 meaning automatic (§5.4's ladder).
    void setBufferSizeChoice (int current, int pinned);
    std::function<void (int)> onBufferSizeChanged;
    /// §5.4: the headphone delay -- the quickest microphone's own path --
    /// and, where one device is slower than the rest (a larger buffer, a
    /// longer reported delay), that microphone's own beside it. A figure
    /// that is only ever the quickest's could read 3 ms while one person
    /// heard themselves 45 ms late.
    void setMeasuredLatency (double ms, double slowestMs)
    {
        auto text = juce::String (ms, 1) + " ms";

        if (slowestMs - ms >= 0.05)
            text << " (slowest mic " << juce::String (slowestMs, 1) << " ms)";

        latencyValue.setText (text, juce::dontSendNotification);
    }
    void setActiveBackendDescription (const juce::String& text) { backendValue.setText (text, juce::dontSendNotification); }
    void setDriftReport (const juce::String& text) { driftLabel.setText (text, juce::dontSendNotification); }

    /// What other apps currently see, and the field that names it. The editor
    /// is only overwritten while unfocused, so refresh never eats typing.
    void setAggregateStatus (const juce::String& text) { aggregateStatusLabel.setText (text, juce::dontSendNotification); }
    void setAggregateName (const juce::String& name)
    {
        if (! aggregateNameEditor.hasKeyboardFocus (true))
            aggregateNameEditor.setText (name, juce::dontSendNotification);
    }
    void setDestinationFolderText (const juce::String& text) { destinationFolderLabel.setText (text, juce::dontSendNotification); }

    /// Fills a combo without firing onChange -- otherwise refreshing the list
    /// would read back as the user having picked something.
    void setOutputDevices (const juce::StringArray& names, const juce::String& selected);
    /// One row in the microphone list: what it reads as, and the device's
    /// identity key the app matches it back to.
    ///
    /// Never the visible text: it gains a microphone count on an interface, and
    /// identical mics share a name -- matching on it switched the wrong one.
    struct MicChoice
    {
        juce::String label, deviceKey;
        bool enabled = false;

        /// One per socket on an interface. Each is its own tick box, indented
        /// under the box it belongs to, so an eight-input interface with two
        /// people on it records two files rather than eight.
        struct Input { int index = 0; juce::String label; bool enabled = true; };
        std::vector<Input> inputs;
    };

    /// The microphones the OS reports and whether each is currently selected.
    void setMicSelections (const std::vector<MicChoice>& mics);

    /// One tick box per person whose microphone has a headphone jack on the
    /// combined device: whether they hear the mix. Empty hides the list.
    struct HeadphoneChoice { juce::String key, label; bool on = true; };
    void setHeadphoneChoices (const std::vector<HeadphoneChoice>& choices);
    std::function<void (const juce::String&, bool)> onHeadphonesToggled;

    /// Volumes to offer as save destinations: display name, path, and whether
    /// it is the one currently in use.
    struct VolumeChoice { juce::String label, path; bool current = false; };
    void setStorageVolumes (const std::vector<VolumeChoice>& volumes);


    /// §4: one trim slider per microphone, rebuilt when the mic set changes.
    /// currentTrimDb supplies each row's starting value.
    void setTrimChannels (const juce::StringArray& micNames,
                          const std::function<float (int)>& currentTrimDb);
    /// Brings each row's slider to the stored trim without notifying, so a trim
    /// changed elsewhere shows here. A slider the user is dragging is left alone.
    void setTrimValues (const std::function<float (int)>& currentTrimDb);

    std::function<void (int, float)> onTrimChanged; // channel index, dB
    std::function<void (const juce::String&)> onAggregateNameChanged;
    std::function<void()> onDiagnosticsExportClicked;

    /// The opt-in update check: the switch, Check now, and the line between
    /// them that says what the last check found. `statusText` empty means
    /// nothing to say; `updateAvailable` shows Download; `canCheckNow` is false
    /// during a take or while a check is already out.
    void setUpdateState (bool checkEnabled, const juce::String& statusText,
                         bool updateAvailable, bool canCheckNow);
    std::function<void (bool)> onCheckForUpdatesToggled;
    std::function<void()> onCheckForUpdatesNowClicked;
    std::function<void()> onDownloadUpdateClicked;

    /// §10.3 says one door. A door has to open both ways: showing this panel
    /// hides the main screen, and the button that opened it lives there, so
    /// without this the panel is a dead end with no way back.
    std::function<void()> onCloseClicked;

    /// Beside Done: the Help screen explains the controls on this one, so
    /// the way to it is here as well as on the main screen.
    std::function<void()> onHelpClicked;

    /// Beside Help: the first-run walkthrough, for whoever skipped it or is
    /// setting up a different rig.
    std::function<void()> onSetupGuideClicked;
    std::function<void (bool)> onMirrorToggled;

    /// "Record on this computer, then copy to the card". On by default.
    std::function<void (bool)> onCopyToCardToggled;

    /// Which platform the take is aimed at, by name; empty means none.
    std::function<void (juce::String)> onDeliveryTargetChanged;

    /// Fills the picker, selects one, and sets the line under it that says what
    /// the mix currently measures and what to do about it.
    void setDeliveryTargets (const juce::StringArray& names, const juce::String& chosen);
    void setLoudnessAdvice (const juce::String& text);

    /// §0.1 / §10.6: everything the app has done and everything that has gone
    /// wrong this session, newest first.
    ///
    /// The advice line above the record button holds one sentence at a time,
    /// so anything that happened while something more serious was on screen was
    /// never shown at all. This is where those go, and where a user answers
    /// "what was that amber thing I looked away from" without having to have
    /// been watching.
    void setActivityLines (const juce::StringArray& lines);

    /// The backup-copy box was fixed ON at construction with no setter, so it
    /// read "ticked" to someone who had turned it off -- telling them a second
    /// copy was being written when none was. §6.3 exists to prevent exactly
    /// that belief.
    void setMirrorEnabled (bool enabled)
    {
        mirrorToggle.setToggleState (enabled, juce::dontSendNotification);
    }
    bool isMirrorEnabled() const { return mirrorToggle.getToggleState(); }

    void setCopyToCardEnabled (bool enabled)
    {
        copyToCardToggle.setToggleState (enabled, juce::dontSendNotification);
    }
    bool isCopyToCardEnabled() const { return copyToCardToggle.getToggleState(); }
    std::function<void()> onDestinationFolderClicked;

    /// Saved shows: the rig's whole setup under a name. Fills the picker
    /// without firing anything, keeping the current pick unless `select`
    /// names another.
    void setShows (const juce::StringArray& names, const juce::String& select = {});
    juce::String getSelectedShow() const;
    std::function<void (const juce::String&)> onLoadShowClicked;
    std::function<void()> onSaveShowClicked;
    std::function<void (const juce::String&)> onDeleteShowClicked;

    std::function<void (const juce::String&, bool)> onMicEnabledChanged;
    /// (device name, physical input, enabled)
    std::function<void (const juce::String&, int, bool)> onInputEnabledChanged;
    std::function<void (const juce::String&)> onStorageVolumeChosen;
    std::function<void (const juce::String&)> onOutputDeviceChanged;

private:
    juce::Label sampleRateLabel;
    juce::Label bitDepthLabel, bufferSizeLabel;
    juce::ComboBox bitDepthCombo, bufferSizeCombo;
    juce::String lastBitDepthSignature, lastBufferSignature;
    juce::Label latencyLabel, latencyValue;
    juce::Label clockMasterLabel, clockMasterValue;
    juce::Label driftLabel; // per-device drift in PPM, populated externally as a multi-line label
    juce::Viewport trimViewport; // per-microphone trim sliders, one row per device
    juce::Component trimContainer;
    std::vector<std::unique_ptr<juce::Label>> trimNameLabels;
    std::vector<std::unique_ptr<juce::Slider>> trimSliders;
    void layOutTrimRows();
    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceCombo;
    juce::Label headphonesLabel;
    std::vector<std::unique_ptr<juce::ToggleButton>> headphoneToggles;
    juce::StringArray lastHeadphoneNames;
    juce::String lastOutputSignature;
    juce::ComboBox sampleRateCombo;
    juce::String lastSampleRateSignature;
    juce::Label backendLabel, backendValue;
    juce::Label aggregateNameLabel;
    juce::TextEditor aggregateNameEditor;
    juce::Label aggregateStatusLabel;
    // Takes for an SD card or USB stick are recorded on this computer's own
    // drive and copied to the card after Stop. The note says why to leave it
    // on, in the user's terms, and what turning it off does.
    juce::ToggleButton copyToCardToggle { "Record on this computer, then copy to the card" };
    juce::Label copyToCardNote;

    juce::ToggleButton mirrorToggle { "Keep a local backup copy" };

    // The reason, under the switch, in the app's voice. §6.3's backup exists
    // for the case where the card does not survive the day, and saying that
    // plainly is what makes someone leave it on.
    juce::Label mirrorNote;
    // "Also save video with the sound in one file" is on the Cameras page: it
    // is a question about the cameras' files, asked where the cameras are.
    juce::Label destinationFolderLabel;
    juce::TextButton destinationFolderButton { "Change..." };
    juce::TextButton diagnosticsExportButton { "Export diagnostics" };

    // Off unless switched on, and the note under it says in one line what
    // switching it on means -- the only thing the app ever sends anywhere.
    juce::ToggleButton checkForUpdatesToggle { "Check for updates" };
    juce::Label checkForUpdatesNote;
    juce::TextButton checkNowButton { "Check now" };
    juce::Label updateStatusLabel;
    juce::TextButton downloadUpdateButton { "Download" };
    juce::TextButton closeButton { "Close" };
    juce::TextButton helpButton { "Help" };
    juce::TextButton setupGuideButton { "Run setup guide again" };

    // Where the take is going, and how loud that place wants it. In its own
    // section because it is a decision about delivery rather than about the
    // rig -- the answer changes per project, not per machine.
    juce::Label deliverySection;
    juce::Label deliveryLabel;
    juce::ComboBox deliveryCombo;
    juce::Label deliveryNote;
    juce::Label loudnessAdviceLabel;
    juce::StringArray deliveryNames;

    // First in the panel: picking tonight's show is the one choice that sets
    // every other row below it.
    juce::Label showsSection;
    juce::ComboBox showsCombo;
    juce::TextButton loadShowButton { "Load" };
    juce::TextButton deleteShowButton { "Delete" };
    juce::TextButton saveShowButton { "Save current as..." };
    juce::StringArray showNames;
    void updateShowButtons();

    juce::Label micSelectionLabel;
    juce::Label storageLabel;
    juce::ComboBox storageCombo;
    juce::StringArray storagePaths;
    juce::StringArray lastStorageLabels;
    juce::Label clockMasterHelpLabel;

    // Section headings and the hairline rules under them. The panel was one
    // flat list of fifteen rows, so finding the storage picker meant reading
    // every unrelated row on the way past it. resized() records where the
    // rules go; paint() draws them, since a rule is a line rather than a
    // component.
    juce::Label storageSection, formatSection, micSection, outputSection, updatesSection,
                activitySection;
    juce::Label activityLabel; // multi-line, newest first, populated externally
    std::vector<int> ruleYs;
    std::vector<std::unique_ptr<juce::ToggleButton>> micToggles;
    /// Parallel to micToggles: true for a socket row, which is indented under
    /// its box and enabled only while the box itself is.
    std::vector<bool> micToggleIsInput;
    juce::StringArray lastMicNames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
};

} // namespace mma
