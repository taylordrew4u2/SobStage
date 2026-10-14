#pragma once
#include "Json.h"
#include "PortIdentity.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mma {

/// One camera's remembered answer, keyed by the id the OS gives it.
struct PersistedCamera
{
    std::string id;
    bool enabled = false;
    std::string assignedName;
    /// cameraQualityKey(): "best", "1080p" or "720p".
    std::string quality = "best";
};

/// One physical port's remembered settings (§2.4), keyed by PortIdentity::key().
struct PersistedPort
{
    std::string key;
    PersistedDeviceSettings settings;
};

/// Everything the app should still know about a rig the second time it opens.
///
/// §2.4 already carries a microphone's name and trim across a replug, and
/// PortIdentityStore says in as many words that writing them to disk is the App
/// layer's job -- which the App layer had never done. So every one of them was
/// carried faithfully across an unplug and lost completely on quit, along with
/// the destination folder, the mirror setting and which microphones were
/// switched off. §10.1 wants a novice to reach a working state without being
/// asked anything; asking them the same questions again every launch is the
/// same failure spread over more days.
///
/// Pure data and JSON, with no file I/O: where this lives on disk is the App
/// layer's business, and keeping it out of here is what lets the round trip be
/// tested on a machine with no audio hardware.
struct AppSettings
{
    // §10.1/§6.2: where recordings go, and the answer to the question asked
    // before the first take -- which is about a specific folder, so both are
    // stored and compared rather than reduced to one flag.
    std::string destinationFolder;
    std::string confirmedSaveLocation;
    bool askWhereToSaveEveryTime = false;

    bool mirrorEnabled = true;                          // §6.3, default on

    /// A take meant for an SD card or USB stick is recorded on this
    /// computer's own drive and copied to the card after Stop (see
    /// TakeRoute). On by default; off records straight to the card, exactly
    /// as before this existed.
    bool recordOnComputerThenCopy = true;

    /// The card last seen at each save location (cardIdentityOf), by its
    /// path. A take armed with the card out is still for that card: without
    /// it, the take went to whatever card was next mounted there -- a camera
    /// card also called "NO NAME", a USB stick given the reader's drive
    /// letter -- and was reported copied "to the card".
    std::map<std::string, std::string> cardAtSaveLocation;
    std::string aggregateName = "SobStage"; // §7, what other apps see
    double masterVolume = 70.0;                         // §5.1 default
    /// Stable id of the monitor output the user explicitly chose. §5.3 makes
    /// that choice priority one on the next launch.
    std::string rememberedOutputDeviceId;

    /// The live mix goes to the combined device -- every microphone's own
    /// headphone jack at once, so everyone hears everyone. On by default;
    /// picking another output in Settings turns it off and remembers that.
    bool monitorThroughCombinedDevice = true;

    /// §2.4 keys of the microphones whose headphone jack gets no mix. Keyed by
    /// port, like disabledMicKeys, so one of four identical mics can be quiet.
    std::vector<std::string> headphonesOffKeys;
    bool cameraPreviewFullQuality = false;

    /// How large the camera tiles on the main screen are drawn, as a step into
    /// MainScreen's size table. Remembered because it is a decision about the
    /// room -- one camera across a table wants a bigger picture than four in a
    /// row -- and re-making it every launch is the §10.1 failure of asking the
    /// same question twice.
    ///
    /// The default is the top of the range -- the picture takes the full width
    /// the window has. Someone who has switched a camera on wants to see the
    /// shot, and the first thing they saw was a thumbnail they had to go
    /// hunting for arrows to enlarge. The layout gives it only the height left
    /// after everything else, so filling the width cannot cost the record
    /// button its place on screen.
    int cameraTileScale = 5;

    /// Whether a take also writes one video-with-sound file per camera.
    ///
    /// Off by default. It costs disk and minutes of CPU after every take, and
    /// the picture and sound are already both saved and already aligned by the
    /// shared session origin -- so this is for the person who wants a file they
    /// can send without opening an editor, and nobody else pays for it.
    bool combineVideoAndAudio = false;

    /// Which platform's loudness the take is being aimed at, by name, or empty
    /// for none. Off by default: someone recording a rehearsal is not
    /// delivering to anything, and a number telling them they are 8 dB under
    /// Spotify is noise.
    std::string deliveryTarget;

    /// A rate the user pinned in Settings, or 0 for automatic.
    ///
    /// Automatic is the right answer almost always -- §2.2 stays on whatever
    /// the hardware is already running, which is what cannot fail. This exists
    /// because the app once told someone to change a rate here and there was
    /// nothing here to change.
    uint32_t sampleRateOverride = 0;

    /// Bit depth the user pinned in Settings, or 0 for the default (24-bit).
    int bitDepthOverride = 0;

    /// Buffer size the user pinned in Settings, or 0 for automatic -- §5.4's
    /// ladder, which starts small and steps up only on overruns.
    int bufferSizeOverride = 0;

    /// Whether the app may ask GitHub once a day if a newer version is out.
    /// Off by default and only ever switched on by the user: PRIVACY.md
    /// promises the app sends nothing anywhere, and this is the one exception,
    /// so it is theirs to make.
    bool checkForUpdates = false;

    /// When the automatic check last ran, in wall-clock seconds since the
    /// epoch; 0 for never. Kept so relaunching all afternoon is one request a
    /// day rather than one per launch.
    double lastUpdateCheckSeconds = 0.0;

    /// Whether the first-run setup guide has been finished or skipped. False
    /// only on a true first launch -- no settings file at all -- so the guide
    /// opens once for someone new and never again on its own. A file written
    /// before the guide existed reads as true: whoever wrote it has already
    /// set the rig up, and a walkthrough arriving after an update would be the
    /// app asking them something it already knows. Settings and Help both
    /// reopen it.
    bool setupGuideDone = false;

    std::vector<PersistedPort> ports;
    /// §2.4 keys of the microphones the user has switched off. Keyed by port
    /// rather than by display name: four identical mics share a product string,
    /// and switching one off must not switch off its three siblings on the next
    /// launch.
    std::vector<std::string> disabledMicKeys;
    std::vector<PersistedCamera> cameras;

    /// Members a newer version wrote that this one does not know, kept so
    /// they are written back exactly as they were read. Without them, opening
    /// an older copy of the app once -- the one still in the DMG, or one kept
    /// after a downgrade -- rewrote settings.json on the first change and
    /// silently threw away every setting the newer version had added, so the
    /// newer copy came back having forgotten them. Top-level members, and
    /// those inside each port (by port key) and camera (by id) entry.
    using UnknownMembers = std::vector<std::pair<std::string, JsonValue>>;
    UnknownMembers unknownFields;
    std::map<std::string, UnknownMembers> unknownPortFields;
    std::map<std::string, UnknownMembers> unknownCameraFields;

    /// Carries another AppSettings' unknown members onto this one: what the
    /// App layer does when it rebuilds the settings from the live rig.
    void keepUnknownFieldsFrom (const AppSettings& other)
    {
        unknownFields = other.unknownFields;
        unknownPortFields = other.unknownPortFields;
        unknownCameraFields = other.unknownCameraFields;
    }

    JsonValue toJson() const;
    static AppSettings fromJson (const JsonValue& v);

    std::string toJsonString() const { return toJson().dump (2); }

    /// Never throws and never reports failure: a settings file that is corrupt,
    /// truncated by a power cut, or written by a newer version comes back as
    /// defaults. §10.1 says the app launches to a working state, and refusing
    /// to start over a preferences file would be the opposite.
    static AppSettings fromJsonString (const std::string& text);

    /// True when fromJsonString() could not read the file and returned defaults
    /// instead. Not an error -- a preferences file is never worth failing to
    /// launch over -- but not a secret either: every remembered microphone
    /// name, trim, disabled mic and the destination folder have just been
    /// silently replaced by defaults, and the user is about to meet an app that
    /// appears to have forgotten them.
    bool wasUnreadable = false;

    /// Look-ups the App layer needs when applying these to a live rig.
    const PersistedPort* findPort (const std::string& key) const;
    const PersistedCamera* findCamera (const std::string& id) const;
    bool isMicDisabled (const std::string& key) const;
    bool areHeadphonesOff (const std::string& key) const;
};

} // namespace mma
