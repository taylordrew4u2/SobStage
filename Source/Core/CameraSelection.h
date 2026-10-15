#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mma {

/// One camera the OS is offering us, whatever it is plugged into. §2 treats
/// every microphone the same whether it is USB, built in or on a capture card,
/// and cameras get the same deal: the app takes what the OS lists and does not
/// second-guess where it came from.
struct CameraDeviceInfo
{
    std::string id;          // stable for as long as the camera is connected
    std::string displayName; // "Logitech C920"

    /// Only for a camera whose `id` is the platform's own per-device identifier
    /// (macOS's uniqueID): the id it had before those were used -- its name,
    /// or "Name #2" and on by OS order among cameras with the same name. Used
    /// once, to carry over choices remembered under that old id. Empty when
    /// `id` already is the name-based id.
    std::string legacyId;
};

/// The live view and the recorded file answer two different questions.
///
/// The file requests the platform's high-quality capture mode; the OS/driver
/// ultimately chooses its format, and nothing on screen may lower that request.
/// The view only has to be good enough to aim by, and drawing a 4K frame sixty
/// times a second to check someone is in shot is exactly the CPU §6.6 warns
/// about spending before it starts dropping audio. So this picks how big the
/// picture is *drawn*, never how big it is captured or written.
enum class PreviewQuality
{
    Low,  // small, cheap, always on
    Full  // as large as the panel allows, for a quick check of focus and framing
};

/// How much picture one camera is asked to record. Best takes whatever the
/// camera can deliver smoothly, up to 4K; the other two cap it, for a card
/// that is short of space or speed, or a machine that is short of CPU.
enum class CameraQuality
{
    Best,   // up to 4K at ~30 fps
    HD1080, // at most 1920x1080
    HD720   // at most 1280x720
};

struct CameraFrameLimit
{
    int maxWidth = 0;
    int maxHeight = 0;
};

/// "best", "1080p", "720p": the settings-file spelling. Anything unknown reads
/// as Best, so a newer file opened by an older build loses nothing worse.
const char* cameraQualityKey (CameraQuality quality) noexcept;
CameraQuality cameraQualityFromKey (const std::string& key) noexcept;

struct PreviewSettings
{
    int maxViewHeight = 0; // the tallest the live picture is drawn
};

/// One camera's part in a take: §6.2 naming, applied to pictures.
struct CameraPlan
{
    std::string deviceId;
    std::string displayName;
    /// "V01_Kitchen-Cam" -- no extension, since the container is the platform's
    /// choice. The "V" keeps video out of the middle of the audio stems when a
    /// file browser sorts the folder by name, and the number keeps two cameras
    /// with the same product string apart.
    std::string fileName;
};

/// Which cameras are in a take, what they are called, and what they will write.
///
/// Kept apart from the platform camera API for the same reason DeviceManager is
/// kept apart from the audio backends: this is all the behaviour worth being
/// sure about, and none of it needs a camera to be plugged into the machine
/// running the tests.
class CameraSelection
{
public:
    /// A rough figure for how much disk a high-quality camera stream can eat, used
    /// only to keep the §6.4 remaining-time estimate honest once video is in
    /// the take. Deliberately pessimistic: telling someone they have less room
    /// than they do costs them nothing, and the reverse costs them the end of
    /// their recording. Sized for a 4K camera on a Mac (the 4K preset is used
    /// whenever a camera can run it at ~30 fps), not for 1080p.
    static constexpr int64_t kEstimatedVideoBytesPerSecond = 8 * 1000 * 1000; // ~64 Mbit/s

    /// Replaces the list of connected cameras, keeping every choice already
    /// made about a camera that is still there. A camera that comes back after
    /// being unplugged comes back switched on if that is how it was left.
    ///
    /// A camera seen for the first time under a per-device id, with choices
    /// still remembered under the old name-based ids for its name, takes them
    /// over: the only one there is, or -- when there are several, which is
    /// exactly the case name-based ids could not tell apart -- the one its OS
    /// order gives it (`legacyId`), as before. From then on its choices follow
    /// the device, not its place in the list.
    void setAvailableCameras (std::vector<CameraDeviceInfo> cameras);
    const std::vector<CameraDeviceInfo>& getAvailableCameras() const { return available; }

    /// The id now holding the choices once remembered under `id`: `id` itself,
    /// or, for an old name-based id carried over to a per-device one, that
    /// device's id. For a show template written before the change.
    std::string resolveId (const std::string& id) const;

    /// True once after setAvailableCameras() carried old choices over, so the
    /// caller can save them under their new ids.
    bool takeAdoptedLegacyChoices() noexcept;

    /// Remembered, enabled cameras which the OS is not currently listing.
    /// These remain visible in the Cameras panel so an unplugged capture card
    /// cannot silently disappear from the rig the user thinks is armed.
    std::vector<CameraDeviceInfo> getUnavailableEnabledCameras() const;

    /// Every camera with remembered state, connected or not. Settings use
    /// this instead of copying an old persisted row back verbatim, so turning
    /// off a currently disconnected capture card is saved correctly.
    std::vector<CameraDeviceInfo> getKnownCameras() const;

    /// Cameras are opt-in. Enumeration alone must not open a camera, illuminate
    /// its privacy light, raise a permission prompt, or add gigabytes to a take.
    void setEnabled (const std::string& id, bool enabled);
    bool isEnabled (const std::string& id) const;
    /// Enabled cameras in the latest completed OS snapshot. Remembered missing
    /// cameras stay armed and visible through getUnavailableEnabledCameras(),
    /// but are not counted as files the next take can actually create.
    int getEnabledCount() const;

    /// The name that goes on the picture's file, like §14.6 for microphones.
    void setAssignedName (const std::string& id, const std::string& name);
    std::string getDisplayName (const std::string& id) const;

    /// Enabled cameras the OS is currently listing, numbered and named. This
    /// is the honest pre-take file promise shown in the save-location card.
    std::vector<CameraPlan> buildPlans() const;

    /// The complete armed roster at the instant a take starts. Available
    /// cameras come first in OS order; remembered missing cameras follow. The
    /// controller freezes this broader list for the watchdog, but it must not
    /// be presented as a promise that every listed file will exist.
    std::vector<CameraPlan> buildIntendedPlans() const;

    /// What a take with this many cameras adds to the bytes-per-second the
    /// §6.4 remaining-time figure is worked out from, camera by camera at the
    /// quality each one is set to.
    int64_t getEstimatedBytesPerSecond() const;

    /// Per camera, remembered across an unplug like every other choice.
    void setQuality (const std::string& id, CameraQuality quality);
    CameraQuality getQuality (const std::string& id) const;

    /// The largest frame a camera at this quality is asked for.
    static CameraFrameLimit frameLimitFor (CameraQuality quality) noexcept;
    /// The pessimistic disk budget for one camera at this quality.
    static int64_t estimatedBytesPerSecondFor (CameraQuality quality) noexcept;

    static PreviewSettings previewSettingsFor (PreviewQuality quality);

    /// "Recording now takes up to about 33 GB an hour, sound included." --
    /// the Cameras page's one line about space, in whole gigabytes. Given the
    /// same bytes-per-second the route and card checks use, so the page can
    /// never promise a figure those checks disagree with.
    static std::string spaceAnHourText (double bytesPerSecond);

private:
    struct Choice
    {
        bool enabled = false;
        std::string assignedName;
        std::string lastDisplayName;
        CameraQuality quality = CameraQuality::Best;
    };

    std::vector<CameraDeviceInfo> available;
    std::map<std::string, Choice> choices; // by id, so unplugging forgets nothing
    std::map<std::string, std::string> adoptedFrom; // old name-based id -> per-device id
    bool adoptedSinceAsked = false;

    void adoptLegacyChoice (const CameraDeviceInfo& camera);
};

} // namespace mma
