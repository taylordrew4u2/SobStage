#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Core/CameraSelection.h"

namespace mma {

/// The Cameras page: a drawer down the right-hand side, beside the live main
/// screen, laid out and opened like Settings.
///
/// It holds only what the camera backends really apply: whether each camera
/// records, its name, and its picture size (the Mac picks the matching
/// preset; Windows the largest mode within it), plus what the camera says it
/// is running at where the Mac reports that. The pictures themselves stay on
/// the main screen, which is always showing beside this page, so framing a
/// shot and choosing its size happen with the picture in view.
class CameraPanel : public juce::Component
{
public:
    CameraPanel();
    ~CameraPanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /// The height this page needs at its current width. Several lines wrap,
    /// so the owner sets the width first, as for Help. Summed by hand to
    /// mirror resized(), like AdvancedPanel, from the same measurements.
    int getRequiredHeight() const;

    struct CameraRow
    {
        std::string id;
        juce::String displayName;
        bool enabled = false;
        bool available = true;
        bool discoveryPending = false;
        bool recordingThisTake = false;
        bool startingThisTake = false;
        /// The file this camera will write, extension included. Empty when the
        /// camera is switched off and so will not write one.
        juce::String fileName;
        /// The main screen's tiles rebuild their live view on this. The page
        /// shows no picture, so it does not.
        uint64_t viewerRevision = 0;
        /// Empty only when a current-generation image has proved this camera
        /// live. Otherwise this is the actionable waiting/open failure, shown
        /// on the row's hint line.
        juce::String signalStatusText;
        CameraQuality quality = CameraQuality::Best;
        /// What the camera says it is delivering ("3840 x 2160, 30 fps").
        /// Empty until it reports, and on platforms that don't.
        juce::String activeFormatText;
    };

    /// The camera list and the state of each one. The rows are rebuilt only
    /// when the set of cameras changes; everything else is said in place, so
    /// this is safe to call from the UI tick and never takes a name box away
    /// from someone typing in it.
    void setCameras (const std::vector<CameraRow>& cameras);

    /// Empty when cameras work here. Otherwise the one sentence saying why they
    /// do not, in place of the controls.
    void setUnavailableReason (const juce::String& reason);
    /// §10.6: anything currently wrong with a camera. Empty hides the line.
    void setProblemText (const juce::String& text);
    /// Reflected in the page's own wording, so what is on screen says whether
    /// the cameras are running, and freezes what a take fixes.
    void setRecording (bool isRecording);

    /// "Also save video with the sound in one file", and the sentence under it
    /// that says what the machine is missing (empty when nothing is).
    void setCombineVideoState (bool on, const juce::String& unavailableReason);

    /// How much an hour of recording takes now, and how long this computer
    /// can record for when that is the limit (empty hides it).
    void setSpaceText (const juce::String& perHour, const juce::String& roomNote);

    std::function<void (const std::string&, bool)> onCameraEnabledChanged;
    std::function<void (const std::string&, const juce::String&)> onCameraRenamed;
    std::function<void (const std::string&, CameraQuality)> onCameraQualityChanged;
    std::function<void (bool)> onCombineVideoToggled;
    std::function<void()> onCloseClicked;

private:
    struct Row
    {
        std::string id;
        std::unique_ptr<juce::ToggleButton> enabledToggle;
        std::unique_ptr<juce::TextEditor> nameEditor;
        std::unique_ptr<juce::Label> fileName;
        std::unique_ptr<juce::Label> hint;
        std::unique_ptr<juce::Label> qualityLabel;
        std::unique_ptr<juce::ComboBox> qualityCombo;
        std::unique_ptr<juce::Label> formatLabel;
    };

    juce::Label heading, explanation, problemLabel, unavailableLabel;
    // Worded and placed like the one in Settings: the same kind of door,
    // closing the same way.
    juce::TextButton closeButton { "Close" };

    // Section headings and the hairline rules under them, as in Settings.
    juce::Label camerasSection, afterTakeSection, spaceSection;
    juce::Label noCamerasLabel;
    juce::ToggleButton combineVideoToggle { "Also save video with the sound in one file" };
    juce::Label combineVideoNote;
    juce::Label spaceLabel, roomNoteLabel;
    std::vector<int> ruleYs;

    std::vector<Row> rows;
    std::vector<CameraRow> lastCameras;
    bool recording = false;

    bool camerasUnavailable() const { return unavailableLabel.getText().isNotEmpty(); }
    void rebuildRows();
    void applyRow (Row& row, const CameraRow& camera);
    void applyRowsInPlace();
    void updateRecordingHeading();

    /// The width a line of text gets at the page's current width.
    int contentWidth() const;
    /// Height of a wrapped label at `width`, measured the way the label draws
    /// it; 0 for an empty one.
    static int textHeight (const juce::Label& label, int width);
    /// The row's height for each of its lines, shared by getRequiredHeight()
    /// and resized() so the two cannot disagree.
    int rowHeight (const Row& row, int width) const;
    int headerToggleWidth (int width) const;

    static juce::String fileLine (const CameraRow& camera, bool recordingNow);
    static juce::String hintLine (const CameraRow& camera, bool recordingNow);
    static juce::String formatLine (const CameraRow& camera, bool recordingNow);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraPanel)
};

} // namespace mma
