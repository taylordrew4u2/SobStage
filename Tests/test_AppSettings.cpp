#include "TestFramework.h"
#include "Core/AppSettings.h"

using namespace mma;

namespace {

AppSettings populated()
{
    AppSettings s;
    s.destinationFolder = "/Volumes/CARD/RECORDINGS";
    s.confirmedSaveLocation = "/Volumes/CARD/RECORDINGS";
    s.askWhereToSaveEveryTime = true;
    s.mirrorEnabled = false;
    s.aggregateName = "Kitchen Table";
    s.masterVolume = 42.0;
    s.rememberedOutputDeviceId = "usb-headphone-amp@3";
    s.cameraPreviewFullQuality = true;
    s.cameraTileScale = 3;

    PersistedPort port;
    port.key = "usb-1-2|SERIAL9";
    port.settings.assignedName = "Alice";
    port.settings.trimDb = -3.5f;
    port.settings.channelLayoutIsMono = false;
    port.settings.hasChannelLayoutDecision = true;
    s.ports.push_back (port);

    s.disabledMicKeys.push_back ("usb-9-9");
    s.cameras.push_back ({ "Logitech C920", true, "Wide shot", "1080p" });

    return s;
}

} // namespace

TEST_CASE (AppSettings_everythingSurvivesARoundTrip)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());

    REQUIRE (restored.destinationFolder == std::string ("/Volumes/CARD/RECORDINGS"));
    REQUIRE (restored.confirmedSaveLocation == std::string ("/Volumes/CARD/RECORDINGS"));
    REQUIRE (restored.askWhereToSaveEveryTime);
    REQUIRE_FALSE (restored.mirrorEnabled);
    REQUIRE (restored.aggregateName == std::string ("Kitchen Table"));
    REQUIRE (restored.masterVolume == 42.0);
    REQUIRE (restored.rememberedOutputDeviceId == std::string ("usb-headphone-amp@3"));
    REQUIRE (restored.cameraPreviewFullQuality);
    REQUIRE (restored.cameraTileScale == 3);
}

TEST_CASE (AppSettings_OutputChoiceDefaultsToAutomaticAndRoundTripsWhenChosen)
{
    REQUIRE (AppSettings::fromJsonString ("{}").rememberedOutputDeviceId.empty());

    AppSettings settings;
    settings.rememberedOutputDeviceId = "headphones-at-left-port";
    REQUIRE (AppSettings::fromJsonString (settings.toJsonString()).rememberedOutputDeviceId
             == std::string ("headphones-at-left-port"));
}

TEST_CASE (AppSettings_aMicrophoneKeepsItsNameAndTrimAcrossLaunches)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());
    const auto* port = restored.findPort ("usb-1-2|SERIAL9");

    // §2.4 already carried these across a replug and lost them on quit, because
    // nothing ever wrote the store to disk.
    REQUIRE (port != nullptr);
    REQUIRE (port->settings.assignedName == std::string ("Alice"));
    REQUIRE (port->settings.trimDb == -3.5f);
    REQUIRE_FALSE (port->settings.channelLayoutIsMono);
    REQUIRE (port->settings.hasChannelLayoutDecision);

    REQUIRE (restored.findPort ("usb-nothing-here") == nullptr);
}

TEST_CASE (AppSettings_AMonoMicrophoneKeepsThePhysicalSideWithItsVerdict)
{
    AppSettings settings;
    PersistedPort port;
    port.key = "usb-right-wired";
    port.settings.hasChannelLayoutDecision = true;
    port.settings.channelLayoutIsMono = true;
    port.settings.channelLayoutMonoSource = 1;
    settings.ports.push_back (port);

    const auto restored = AppSettings::fromJsonString (settings.toJsonString());
    const auto* remembered = restored.findPort (port.key);

    REQUIRE (remembered != nullptr);
    REQUIRE (remembered->settings.hasChannelLayoutDecision);
    REQUIRE (remembered->settings.channelLayoutIsMono);
    REQUIRE (remembered->settings.channelLayoutMonoSource == 1);

    // Settings written before the source field existed remain valid and use
    // the historical left-side default rather than an unchecked value.
    const auto legacy = AppSettings::fromJsonString (
        "{\"ports\":[{\"key\":\"old\",\"hasChannelLayoutDecision\":true,"
        "\"channelLayoutIsMono\":true}]}"
    );
    REQUIRE (legacy.findPort ("old")->settings.channelLayoutMonoSource == 0);
}

TEST_CASE (AppSettings_switchedOffMicrophonesAreKeyedByPortNotName)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());

    // Four identical microphones share a product string (§14.6). Keyed by name,
    // switching one off would switch off its siblings on the next launch.
    REQUIRE (restored.isMicDisabled ("usb-9-9"));
    REQUIRE_FALSE (restored.isMicDisabled ("usb-1-2|SERIAL9"));
}

TEST_CASE (AppSettings_camerasKeepTheirAnswers)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());
    const auto* camera = restored.findCamera ("Logitech C920");

    REQUIRE (camera != nullptr);
    REQUIRE (camera->enabled);
    REQUIRE (camera->assignedName == std::string ("Wide shot"));
    REQUIRE (camera->quality == std::string ("1080p"));
    REQUIRE (restored.findCamera ("no such camera") == nullptr);
}

TEST_CASE (AppSettings_aCameraWithoutAQualityIsBest)
{
    // Files written before cameras had a quality setting must open as Best,
    // which is what those cameras were already doing.
    const auto s = AppSettings::fromJsonString ("{\"cameras\":[{\"id\":\"cam\",\"enabled\":true}]}");
    REQUIRE (s.cameras.size() == 1u);
    REQUIRE (s.cameras[0].quality == std::string ("best"));
}

TEST_CASE (AppSettings_garbageLoadsAsDefaultsRatherThanThrowing)
{
    // §10.1: the app launches to a working state. A preferences file truncated
    // by a power cut is not a reason to refuse to start, so every one of these
    // has to come back as the defaults without throwing.
    for (const auto* text : { "", "{", "not json at all", "[1,2,3", "\xff\xfe garbage" })
    {
        const auto s = AppSettings::fromJsonString (text);
        REQUIRE (s.mirrorEnabled);                  // §6.3 default on
        REQUIRE (s.masterVolume == 70.0);           // §5.1 default
        REQUIRE_FALSE (s.askWhereToSaveEveryTime);
        REQUIRE (s.destinationFolder.empty());
        REQUIRE (s.ports.empty());
    }
}

TEST_CASE (AppSettings_aFileFromAnotherVersionKeepsWhatItCanRead)
{
    // One known key, one key this version has never heard of, and none of the
    // rest. The known value has to survive and everything else fall back --
    // dropping the file whole would silently reset a rig on every upgrade.
    const auto s = AppSettings::fromJsonString (
        "{\"aggregateName\":\"From The Future\",\"somethingNewerVersionsAdded\":{\"a\":1}}");

    REQUIRE (s.aggregateName == std::string ("From The Future"));
    REQUIRE (s.mirrorEnabled);
    REQUIRE (s.masterVolume == 70.0);
}

TEST_CASE (AppSettings_entriesWithNoKeyAreDroppedRatherThanKept)
{
    // A port row with no key can never be matched to a device, and a camera row
    // with no id can never be matched to a camera. Keeping them would grow the
    // file on every launch with rows that can never apply.
    const auto s = AppSettings::fromJsonString (
        "{\"ports\":[{\"assignedName\":\"orphan\"},{\"key\":\"usb-1\",\"trimDb\":2.0}],"
        " \"cameras\":[{\"enabled\":true},{\"id\":\"cam\",\"enabled\":true}]}");

    REQUIRE (s.ports.size() == 1u);
    REQUIRE (s.ports[0].key == std::string ("usb-1"));
    REQUIRE (s.cameras.size() == 1u);
    REQUIRE (s.cameras[0].id == std::string ("cam"));
}

TEST_CASE (AppSettings_CameraTileScaleDefaultsWhenAbsent)
{
    // Every settings file written before the camera size existed has no such
    // key. Loading one must land on the default the main screen ships with
    // rather than zero, which is the smallest tile and would silently shrink
    // the pictures of everyone who upgrades.
    //
    // That default is the top of the range on purpose: the steps are shares of
    // the window's width, so the largest means "fill it", and someone who has
    // switched a camera on wants to see the shot rather than a thumbnail they
    // have to hunt for arrows to enlarge.
    const auto restored = AppSettings::fromJsonString ("{}");

    REQUIRE (restored.cameraTileScale == 5);
}

TEST_CASE (AppSettings_FormatOverridesRoundTrip)
{
    // Sample rate, bit depth and buffer size were read-only lines in Settings.
    // Now that they are controls, a choice has to survive a relaunch -- a pin
    // that vanished on restart would read as the control doing nothing.
    AppSettings s;
    s.sampleRateOverride = 44100;
    s.bitDepthOverride = 16;
    s.bufferSizeOverride = 256;

    const auto back = AppSettings::fromJsonString (s.toJsonString());

    REQUIRE (back.sampleRateOverride == 44100u);
    REQUIRE (back.bitDepthOverride == 16);
    REQUIRE (back.bufferSizeOverride == 256);
}

TEST_CASE (AppSettings_FormatOverridesDefaultToAutomatic)
{
    const auto s = AppSettings::fromJsonString ("{}");

    REQUIRE (s.sampleRateOverride == 0u);
    REQUIRE (s.bitDepthOverride == 0);
    REQUIRE (s.bufferSizeOverride == 0);
}

TEST_CASE (AppSettings_PerInputSettingsRoundTrip)
{
    // Which sockets of an interface are switched off, and what each is called,
    // are port memory: they follow the box across a replug and a relaunch. A
    // choice that vanished on restart would read as the control doing nothing.
    AppSettings s;
    PersistedPort port;
    port.key = "usb-3|SN123";
    port.settings.disabledInputs = { 1, 3 };
    port.settings.inputNames[0] = "Alex";
    port.settings.inputNames[2] = "Sam";
    s.ports.push_back (port);

    const auto back = AppSettings::fromJsonString (s.toJsonString());

    REQUIRE (back.ports.size() == 1);
    REQUIRE (back.ports[0].settings.disabledInputs == std::vector<int> ({ 1, 3 }));
    REQUIRE (back.ports[0].settings.inputNames.size() == 2);
    REQUIRE (back.ports[0].settings.inputNames.at (0) == "Alex");
    REQUIRE (back.ports[0].settings.inputNames.at (2) == "Sam");
}

TEST_CASE (AppSettings_InterfaceTrimIsKeptPerInput)
{
    // Two people on one interface are set up by ear separately. A trim that
    // was saved against the box came back on every input after a relaunch.
    AppSettings s;
    PersistedPort port;
    port.key = "usb-3|SN123";
    port.settings.trimDb = -2.0f;
    port.settings.inputTrimDb[0] = 6.0f;
    port.settings.inputTrimDb[3] = -4.5f;
    s.ports.push_back (port);

    const auto back = AppSettings::fromJsonString (s.toJsonString());

    REQUIRE (back.ports.size() == 1);
    REQUIRE (back.ports[0].settings.trimDb == -2.0f);
    REQUIRE (back.ports[0].settings.inputTrimDb.size() == 2);
    REQUIRE (back.ports[0].settings.inputTrimDb.at (0) == 6.0f);
    REQUIRE (back.ports[0].settings.inputTrimDb.at (3) == -4.5f);
}

TEST_CASE (PersistedDeviceSettings_TrimOnOneInputLeavesTheOthersAlone)
{
    PersistedDeviceSettings settings;

    settings.setTrimDbForInput (1, true, 6.0f);

    REQUIRE (settings.trimDbForInput (1) == 6.0f);
    REQUIRE (settings.trimDbForInput (0) == 0.0f);
    REQUIRE (settings.trimDbForInput (2) == 0.0f);

    // A single microphone's trim is the box's, as it always was.
    PersistedDeviceSettings mic;
    mic.setTrimDbForInput (0, false, -3.0f);
    REQUIRE (mic.trimDb == -3.0f);
    REQUIRE (mic.trimDbForInput (0) == -3.0f);
}

TEST_CASE (PersistedDeviceSettings_AnInterfaceSavedBeforePerInputTrimKeepsItsTrim)
{
    // Settings from before trim was per input have only the box's value; every
    // input keeps it until that input is given its own.
    PersistedDeviceSettings legacy;
    legacy.trimDb = -6.0f;

    REQUIRE (legacy.trimDbForInput (0) == -6.0f);
    REQUIRE (legacy.trimDbForInput (1) == -6.0f);

    legacy.setTrimDbForInput (0, true, 0.0f);
    REQUIRE (legacy.trimDbForInput (0) == 0.0f);
    REQUIRE (legacy.trimDbForInput (1) == -6.0f);
}

TEST_CASE (AppSettings_AnUnreadableFileSaysSoRatherThanQuietlyResetting)
{
    // Defaults instead of a failure is right -- a preferences file is never
    // worth failing to launch over. Defaults instead of a failure AND without a
    // word is not: every remembered microphone name, trim, disabled mic and the
    // destination folder have just been replaced, and the user is about to meet
    // an app that looks like it forgot them.
    const auto settings = AppSettings::fromJsonString ("\x01\x02 not json at all");

    REQUIRE (settings.wasUnreadable);
    REQUIRE (settings.destinationFolder.empty());
    REQUIRE (settings.ports.empty());
}

TEST_CASE (AppSettings_ATruncatedFileKeepsWhatSurvivedAndIsNotCalledUnreadable)
{
    // A power cut mid-write leaves a file that stops part way. Whatever keys
    // did land are still the user's settings and are still worth keeping, so
    // this is not the "everything is gone" case and must not claim to be --
    // crying wolf about settings that are actually intact is its own noise.
    const auto settings = AppSettings::fromJsonString ("{\"masterVolume\": 0.5, \"destinationFol");

    REQUIRE_FALSE (settings.wasUnreadable);
    REQUIRE (settings.masterVolume == 0.5);
}

TEST_CASE (AppSettings_ATruncatedFileNeverKeepsAValueThatWasCutOff)
{
    // Keeping what survived is right; keeping the half of a value that got
    // written is not. A folder cut to "/Users/me/RECO" would be recorded into
    // and saved back as though the user had chosen it.
    const auto settings = AppSettings::fromJsonString (
        "{\"masterVolume\": 0.5, \"destinationFolder\": \"/Users/me/RECO");

    REQUIRE_FALSE (settings.wasUnreadable);
    REQUIRE (settings.masterVolume == 0.5);
    REQUIRE (settings.destinationFolder.empty());
}

TEST_CASE (AppSettings_ATruncatedFileNeverNamesAMicrophoneWithHalfItsName)
{
    // Cut inside "Lead Vocal": the complete port before it stays, the one that
    // was being written when the file stopped is not rebuilt as "Lea".
    const auto settings = AppSettings::fromJsonString (
        "{\"masterVolume\": 0.5, \"ports\": ["
        "{\"key\": \"usb-1\", \"assignedName\": \"Drums\"}, "
        "{\"key\": \"usb-2\", \"assignedName\": \"Lea");

    REQUIRE_FALSE (settings.wasUnreadable);
    REQUIRE (settings.masterVolume == 0.5);
    REQUIRE (settings.findPort ("usb-1") != nullptr);
    REQUIRE (settings.findPort ("usb-1")->settings.assignedName == std::string ("Drums"));

    for (const auto& port : settings.ports)
        REQUIRE (port.settings.assignedName != std::string ("Lea"));
}

TEST_CASE (JsonValue_ATruncatedDocumentDropsTheMemberThatWasCut)
{
    // The parser's half of the above: members that finished are kept, the one
    // the input stopped inside is not, and the caller is told it was cut.
    bool truncated = false;
    const auto v = JsonValue::parse ("{\"a\": 1, \"b\": \"hal", &truncated);

    REQUIRE (truncated);
    REQUIRE (v.find ("a") != nullptr);
    REQUIRE (v.find ("b") == nullptr);

    // Cut inside a literal is the same case, not an unreadable file.
    const auto literal = JsonValue::parse ("{\"a\": 1, \"b\": tr", &truncated);
    REQUIRE (truncated);
    REQUIRE (literal.find ("a") != nullptr);
    REQUIRE (literal.find ("b") == nullptr);

    truncated = true;
    JsonValue::parse ("{\"a\": 1}", &truncated);
    REQUIRE_FALSE (truncated);
}

TEST_CASE (AppSettings_AnEmptyFileIsAFirstLaunchNotACorruptOne)
{
    // Nothing to have forgotten, so nothing to report.
    REQUIRE_FALSE (AppSettings::fromJsonString ("").wasUnreadable);
    REQUIRE_FALSE (AppSettings::fromJsonString ("   \n\t ").wasUnreadable);
}

TEST_CASE (AppSettings_AReadableFileIsNotFlagged)
{
    AppSettings written;
    written.destinationFolder = "/Volumes/CARD";
    written.masterVolume = 0.5;

    const auto read = AppSettings::fromJsonString (written.toJsonString());

    REQUIRE_FALSE (read.wasUnreadable);
    REQUIRE (read.destinationFolder == std::string ("/Volumes/CARD"));
}

TEST_CASE (AppSettings_ATruncationThatSavedNothingIsUnreadable)
{
    // The other half of the truncation case above. This file stops before its
    // first value, so nothing of the user's survived -- and the lenient parser
    // still hands back one dangling key, which used to read as a setting
    // recovered. Everything was reset and nothing said so.
    const auto settings = AppSettings::fromJsonString ("{\"destinationFolde");

    REQUIRE (settings.wasUnreadable);
    REQUIRE (settings.destinationFolder.empty());
}

TEST_CASE (AppSettings_TheSecondSmallestCameraTileSurvivesARoundTrip)
{
    // cameraTileScale parsed with a fallback of 1 while the struct default was
    // 5, and Application compensated by rewriting any stored 1 back to 5 --
    // which made the second-smallest tile the one setting a user could not
    // keep. Pick it, relaunch, and the tiles came back at their largest.
    AppSettings s;
    s.cameraTileScale = 1;

    const auto parsed = AppSettings::fromJson (s.toJson());
    REQUIRE (parsed.cameraTileScale == 1);

    // An absent key still lands on the shipped default rather than the
    // smallest tile.
    REQUIRE (AppSettings::fromJson ("{}").cameraTileScale == AppSettings{}.cameraTileScale);
}

TEST_CASE (AppSettings_MonitoringGoesThroughTheCombinedDeviceUnlessToldOtherwise)
{
    // Absent from an older settings file: the new default applies, so an
    // upgrade lands on everyone hearing everyone.
    const auto fresh = AppSettings::fromJsonString ("{\"rememberedOutputDeviceId\":\"usb-pnp\"}");
    REQUIRE (fresh.monitorThroughCombinedDevice);
    REQUIRE (fresh.headphonesOffKeys.empty());

    AppSettings settings;
    settings.monitorThroughCombinedDevice = false;
    settings.headphonesOffKeys = { "yeti-2", "yeti-4" };

    const auto restored = AppSettings::fromJsonString (settings.toJsonString());
    REQUIRE_FALSE (restored.monitorThroughCombinedDevice);
    REQUIRE (restored.areHeadphonesOff ("yeti-2"));
    REQUIRE (restored.areHeadphonesOff ("yeti-4"));
    REQUIRE_FALSE (restored.areHeadphonesOff ("yeti-1"));
}

TEST_CASE (AppSettings_SetupGuideOpensOnlyOnATrueFirstLaunch)
{
    // No file at all: someone new. The guide opens by itself.
    REQUIRE_FALSE (AppSettings{}.setupGuideDone);
    REQUIRE_FALSE (AppSettings::fromJsonString ("").setupGuideDone);

    // Finished or skipped, and remembered.
    AppSettings done;
    done.setupGuideDone = true;
    REQUIRE (AppSettings::fromJsonString (done.toJsonString()).setupGuideDone);

    // Quit before finishing it: it opens again next time.
    REQUIRE_FALSE (AppSettings::fromJsonString (AppSettings{}.toJsonString()).setupGuideDone);

    // A file written before the guide existed belongs to someone who set the
    // rig up without it. An update must not greet them with a walkthrough.
    REQUIRE (AppSettings::fromJsonString ("{\"aggregateName\":\"Kitchen\"}").setupGuideDone);

    // Nor must a file that could not be read: they are already being told
    // their settings were lost, and are not new.
    const auto unreadable = AppSettings::fromJsonString ("not json at all");
    REQUIRE (unreadable.wasUnreadable);
    REQUIRE (unreadable.setupGuideDone);
}

TEST_CASE (AppSettings_SettingsANewerVersionWroteSurviveARoundTrip)
{
    // A newer SobStage wrote a setting this version has never heard of, at the
    // top level and inside a microphone's and a camera's entry. The older copy
    // still in the DMG is opened once and saves. The newer copy must find its
    // settings where it left them.
    const std::string newer =
        "{\"masterVolume\": 55, \"futureLoudnessMode\": \"broadcast\","
        " \"futureList\": [1, 2],"
        " \"ports\": [{\"key\": \"usb-1\", \"assignedName\": \"Drums\", \"futurePhantom\": true}],"
        " \"cameras\": [{\"id\": \"FaceTime HD\", \"enabled\": true, \"futureZoom\": 2}]}";

    const auto read = AppSettings::fromJsonString (newer);
    REQUIRE_FALSE (read.wasUnreadable);
    REQUIRE (read.masterVolume == 55.0);

    const auto saved = JsonValue::parse (read.toJsonString());
    REQUIRE (saved.find ("futureLoudnessMode") != nullptr);
    REQUIRE (saved.find ("futureLoudnessMode")->asString() == std::string ("broadcast"));
    REQUIRE (saved.find ("futureList") != nullptr);
    REQUIRE (saved.find ("futureList")->asArray().size() == 2u);
    REQUIRE (saved.find ("ports")->asArray().size() == 1u);
    REQUIRE (saved.find ("ports")->asArray().front().find ("futurePhantom") != nullptr);
    REQUIRE (saved.find ("cameras")->asArray().size() == 1u);
    REQUIRE (saved.find ("cameras")->asArray().front().find ("futureZoom") != nullptr);
}

TEST_CASE (AppSettings_TheAppLayersRebuiltSettingsKeepWhatANewerVersionWrote)
{
    // Application::saveSettings() does not edit the settings it read; it
    // builds a fresh AppSettings from the live rig. That rebuild is where the
    // newer version's settings were actually lost.
    const auto read = AppSettings::fromJsonString (
        "{\"masterVolume\": 55, \"futureLoudnessMode\": \"broadcast\","
        " \"ports\": [{\"key\": \"usb-1\", \"assignedName\": \"Drums\", \"futurePhantom\": true}],"
        " \"cameras\": [{\"id\": \"FaceTime HD\", \"enabled\": true, \"futureZoom\": 2}]}");

    AppSettings rebuilt;
    rebuilt.keepUnknownFieldsFrom (read);
    rebuilt.masterVolume = 60.0;
    rebuilt.ports.push_back ({ "usb-1", read.findPort ("usb-1")->settings });
    rebuilt.cameras.push_back (*read.findCamera ("FaceTime HD"));

    const auto saved = JsonValue::parse (rebuilt.toJsonString());
    REQUIRE (saved.find ("masterVolume")->asDouble() == 60.0);
    REQUIRE (saved.find ("futureLoudnessMode") != nullptr);
    REQUIRE (saved.find ("ports")->asArray().front().find ("futurePhantom")->asBool());
    REQUIRE (saved.find ("cameras")->asArray().front().find ("futureZoom")->asDouble() == 2.0);

    // A value this version owns is never overruled by a stale copy of it.
    REQUIRE (JsonValue::parse (rebuilt.toJsonString()).getMembers().size()
             == JsonValue::parse (AppSettings{}.toJsonString()).getMembers().size() + 1);
}

TEST_CASE (AppSettings_AKnownSettingIsNeverMistakenForAnUnknownOne)
{
    // Every member this version writes is its own: none comes back as an
    // "unknown" one to be written twice.
    const auto read = AppSettings::fromJsonString (populated().toJsonString());

    REQUIRE (read.unknownFields.empty());
    REQUIRE (read.unknownPortFields.empty());
    REQUIRE (read.unknownCameraFields.empty());
}

// Recording on this computer and copying to the card is on unless someone
// turns it off, and a file from before it existed reads as on.
TEST_CASE (AppSettings_RecordOnComputerThenCopyDefaultsOnAndRoundTrips)
{
    REQUIRE (AppSettings().recordOnComputerThenCopy);

    AppSettings off;
    off.recordOnComputerThenCopy = false;
    REQUIRE_FALSE (AppSettings::fromJsonString (off.toJsonString()).recordOnComputerThenCopy);

    AppSettings on;
    REQUIRE (AppSettings::fromJsonString (on.toJsonString()).recordOnComputerThenCopy);

    REQUIRE (AppSettings::fromJsonString ("{ \"mirrorEnabled\": false }").recordOnComputerThenCopy);

    // Written as a member of its own, not folded into something an older
    // version would drop: an older copy keeps it through its unknown members.
    REQUIRE (off.toJsonString().find ("\"recordOnComputerThenCopy\": false") != std::string::npos);
}
