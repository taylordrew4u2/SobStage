#include "AppSettings.h"
#include <algorithm>
#include <cmath>

namespace mma {

namespace {

void appendUnknown (JsonValue& object, const AppSettings::UnknownMembers& unknown)
{
    // Never over a member this version wrote itself: what it knows, it owns.
    for (const auto& [key, value] : unknown)
        if (object.find (key) == nullptr)
            object[key] = value;
}

AppSettings::UnknownMembers unknownMembersOf (const JsonValue& object, const JsonValue& known)
{
    AppSettings::UnknownMembers unknown;

    for (const auto& [key, value] : object.getMembers())
        if (known.find (key) == nullptr)
            unknown.emplace_back (key, value);

    return unknown;
}

/// What this version writes, at the top level and inside one port and one
/// camera entry. Taken from toJson() itself, so a member added there is known
/// here without a second list to keep in step.
struct KnownShapes
{
    JsonValue top, port, camera;

    KnownShapes()
    {
        AppSettings sample;
        sample.ports.push_back ({ "k", {} });
        PersistedCamera sampleCamera;
        sampleCamera.id = "c";
        sample.cameras.push_back (sampleCamera);
        top = sample.toJson();

        if (const auto* p = top.find ("ports"); p != nullptr && ! p->asArray().empty())
            port = p->asArray().front();

        if (const auto* c = top.find ("cameras"); c != nullptr && ! c->asArray().empty())
            camera = c->asArray().front();
    }
};

const KnownShapes& knownShapes()
{
    static const KnownShapes shapes;
    return shapes;
}

} // namespace

JsonValue AppSettings::toJson() const
{
    JsonValue root = JsonValue::makeObject();
    root["destinationFolder"] = JsonValue (destinationFolder);
    root["confirmedSaveLocation"] = JsonValue (confirmedSaveLocation);
    root["askWhereToSaveEveryTime"] = JsonValue (askWhereToSaveEveryTime);
    root["mirrorEnabled"] = JsonValue (mirrorEnabled);
    root["recordOnComputerThenCopy"] = JsonValue (recordOnComputerThenCopy);

    {
        JsonValue cards = JsonValue::makeObject();
        for (const auto& [location, identity] : cardAtSaveLocation)
            cards[location] = JsonValue (identity);
        root["cardAtSaveLocation"] = cards;
    }
    root["aggregateName"] = JsonValue (aggregateName);
    root["masterVolume"] = JsonValue (masterVolume);
    root["rememberedOutputDeviceId"] = JsonValue (rememberedOutputDeviceId);
    root["monitorThroughCombinedDevice"] = JsonValue (monitorThroughCombinedDevice);
    root["cameraPreviewFullQuality"] = JsonValue (cameraPreviewFullQuality);
    root["cameraTileScale"] = JsonValue (static_cast<double> (cameraTileScale));
    root["combineVideoAndAudio"] = JsonValue (combineVideoAndAudio);
    root["deliveryTarget"] = JsonValue (deliveryTarget);
    root["sampleRateOverride"] = JsonValue (static_cast<double> (sampleRateOverride));
    root["bitDepthOverride"] = JsonValue (static_cast<double> (bitDepthOverride));
    root["bufferSizeOverride"] = JsonValue (static_cast<double> (bufferSizeOverride));
    root["checkForUpdates"] = JsonValue (checkForUpdates);
    // Whole seconds: JsonValue writes a fractional number to six significant
    // figures, which would round a 2026 timestamp by minutes.
    root["lastUpdateCheckSeconds"] = JsonValue (std::floor (lastUpdateCheckSeconds));
    root["setupGuideDone"] = JsonValue (setupGuideDone);

    JsonValue portArr = JsonValue::makeArray();
    for (const auto& p : ports)
    {
        JsonValue pv = JsonValue::makeObject();
        pv["key"] = JsonValue (p.key);
        pv["assignedName"] = JsonValue (p.settings.assignedName);
        pv["trimDb"] = JsonValue (static_cast<double> (p.settings.trimDb));
        pv["channelLayoutIsMono"] = JsonValue (p.settings.channelLayoutIsMono);
        pv["hasChannelLayoutDecision"] = JsonValue (p.settings.hasChannelLayoutDecision);
        pv["channelLayoutMonoSource"] = JsonValue (
            static_cast<double> (p.settings.channelLayoutMonoSource));

        JsonValue disabledInputs = JsonValue::makeArray();
        for (int input : p.settings.disabledInputs)
            disabledInputs.push_back (JsonValue (static_cast<double> (input)));
        pv["disabledInputs"] = disabledInputs;

        JsonValue inputNames = JsonValue::makeArray();
        for (const auto& [input, name] : p.settings.inputNames)
        {
            JsonValue entry = JsonValue::makeObject();
            entry["input"] = JsonValue (static_cast<double> (input));
            entry["name"] = JsonValue (name);
            inputNames.push_back (entry);
        }
        pv["inputNames"] = inputNames;

        JsonValue inputTrims = JsonValue::makeArray();
        for (const auto& [input, db] : p.settings.inputTrimDb)
        {
            JsonValue entry = JsonValue::makeObject();
            entry["input"] = JsonValue (static_cast<double> (input));
            entry["trimDb"] = JsonValue (static_cast<double> (db));
            inputTrims.push_back (entry);
        }
        pv["inputTrims"] = inputTrims;

        if (const auto it = unknownPortFields.find (p.key); it != unknownPortFields.end())
            appendUnknown (pv, it->second);

        portArr.push_back (pv);
    }
    root["ports"] = portArr;

    JsonValue disabledArr = JsonValue::makeArray();
    for (const auto& key : disabledMicKeys)
        disabledArr.push_back (JsonValue (key));
    root["disabledMicrophones"] = disabledArr;

    JsonValue headphonesOffArr = JsonValue::makeArray();
    for (const auto& key : headphonesOffKeys)
        headphonesOffArr.push_back (JsonValue (key));
    root["headphonesOff"] = headphonesOffArr;

    JsonValue cameraArr = JsonValue::makeArray();
    for (const auto& c : cameras)
    {
        JsonValue cv = JsonValue::makeObject();
        cv["id"] = JsonValue (c.id);
        cv["enabled"] = JsonValue (c.enabled);
        cv["assignedName"] = JsonValue (c.assignedName);
        cv["quality"] = JsonValue (c.quality);

        if (const auto it = unknownCameraFields.find (c.id); it != unknownCameraFields.end())
            appendUnknown (cv, it->second);

        cameraArr.push_back (cv);
    }
    root["cameras"] = cameraArr;

    appendUnknown (root, unknownFields);

    return root;
}

AppSettings AppSettings::fromJson (const JsonValue& v)
{
    AppSettings s;

    // Every field falls back to the default already in the struct, so a file
    // written by an older version -- or one missing a key entirely -- loads the
    // rest rather than being thrown away whole.
    if (auto* p = v.find ("destinationFolder")) s.destinationFolder = p->asString();
    if (auto* p = v.find ("confirmedSaveLocation")) s.confirmedSaveLocation = p->asString();
    if (auto* p = v.find ("askWhereToSaveEveryTime")) s.askWhereToSaveEveryTime = p->asBool (false);
    if (auto* p = v.find ("mirrorEnabled")) s.mirrorEnabled = p->asBool (true);
    if (auto* p = v.find ("recordOnComputerThenCopy")) s.recordOnComputerThenCopy = p->asBool (true);

    if (auto* p = v.find ("cardAtSaveLocation"); p != nullptr && p->getType() == JsonValue::Type::Object)
        for (const auto& [location, identity] : p->getMembers())
            if (! location.empty() && identity.getType() == JsonValue::Type::String && ! identity.asString().empty())
                s.cardAtSaveLocation[location] = identity.asString();
    if (auto* p = v.find ("aggregateName")) s.aggregateName = p->asString (s.aggregateName);
    if (auto* p = v.find ("masterVolume")) s.masterVolume = p->asDouble (s.masterVolume);
    if (auto* p = v.find ("rememberedOutputDeviceId")) s.rememberedOutputDeviceId = p->asString();
    if (auto* p = v.find ("monitorThroughCombinedDevice")) s.monitorThroughCombinedDevice = p->asBool (true);
    if (auto* p = v.find ("cameraPreviewFullQuality")) s.cameraPreviewFullQuality = p->asBool (false);
    // The fallback is the struct default, not 1. It only applies when the key
    // is present but unparsable; an absent key already leaves the default
    // standing. A fallback of 1 meant a corrupt value silently became the
    // smallest tile rather than the one the app ships with.
    if (auto* p = v.find ("cameraTileScale"))
        s.cameraTileScale = static_cast<int> (p->asDouble (static_cast<double> (AppSettings{}.cameraTileScale)));
    if (auto* p = v.find ("combineVideoAndAudio")) s.combineVideoAndAudio = p->asBool (false);
    if (auto* p = v.find ("deliveryTarget")) s.deliveryTarget = p->asString ("");
    if (auto* p = v.find ("sampleRateOverride"))
        s.sampleRateOverride = static_cast<uint32_t> (p->asDouble (0.0));
    if (auto* p = v.find ("bitDepthOverride"))
        s.bitDepthOverride = static_cast<int> (p->asDouble (0.0));
    if (auto* p = v.find ("bufferSizeOverride"))
        s.bufferSizeOverride = static_cast<int> (p->asDouble (0.0));
    if (auto* p = v.find ("checkForUpdates")) s.checkForUpdates = p->asBool (false);
    if (auto* p = v.find ("lastUpdateCheckSeconds")) s.lastUpdateCheckSeconds = p->asDouble (0.0);
    // Absent means the file predates the guide, which means the rig was set up
    // without it: done, not new. Only a launch with no file at all is first.
    s.setupGuideDone = true;
    if (auto* p = v.find ("setupGuideDone")) s.setupGuideDone = p->asBool (true);

    if (auto* p = v.find ("ports"))
        for (const auto& pv : p->asArray())
        {
            PersistedPort port;
            if (auto* n = pv.find ("key")) port.key = n->asString();

            // A port entry with no key cannot be matched to a device, so it is
            // dropped rather than kept as a row that can never apply.
            if (port.key.empty())
                continue;

            if (auto* n = pv.find ("assignedName")) port.settings.assignedName = n->asString();
            if (auto* n = pv.find ("trimDb")) port.settings.trimDb = static_cast<float> (n->asDouble());
            if (auto* n = pv.find ("channelLayoutIsMono")) port.settings.channelLayoutIsMono = n->asBool (true);
            if (auto* n = pv.find ("hasChannelLayoutDecision")) port.settings.hasChannelLayoutDecision = n->asBool (false);
            if (auto* n = pv.find ("channelLayoutMonoSource"))
            {
                const int source = static_cast<int> (n->asDouble (0.0));
                port.settings.channelLayoutMonoSource = source == 1 ? 1 : 0;
            }

            if (auto* n = pv.find ("disabledInputs"))
                for (const auto& iv : n->asArray())
                    if (const int input = static_cast<int> (iv.asDouble (-1.0)); input >= 0)
                        port.settings.disabledInputs.push_back (input);

            if (auto* n = pv.find ("inputNames"))
                for (const auto& ev : n->asArray())
                {
                    const auto* inputV = ev.find ("input");
                    const auto* nameV = ev.find ("name");

                    if (inputV == nullptr || nameV == nullptr)
                        continue;

                    if (const int input = static_cast<int> (inputV->asDouble (-1.0)); input >= 0)
                        if (const auto name = nameV->asString(); ! name.empty())
                            port.settings.inputNames[input] = name;
                }

            if (auto* n = pv.find ("inputTrims"))
                for (const auto& ev : n->asArray())
                {
                    const auto* inputV = ev.find ("input");
                    const auto* trimV = ev.find ("trimDb");

                    if (inputV == nullptr || trimV == nullptr)
                        continue;

                    if (const int input = static_cast<int> (inputV->asDouble (-1.0)); input >= 0)
                        port.settings.inputTrimDb[input] = static_cast<float> (trimV->asDouble());
                }

            if (auto unknown = unknownMembersOf (pv, knownShapes().port); ! unknown.empty())
                s.unknownPortFields[port.key] = std::move (unknown);

            s.ports.push_back (port);
        }

    if (auto* p = v.find ("disabledMicrophones"))
        for (const auto& dv : p->asArray())
            if (const auto key = dv.asString(); ! key.empty())
                s.disabledMicKeys.push_back (key);

    if (auto* p = v.find ("headphonesOff"))
        for (const auto& dv : p->asArray())
            if (const auto key = dv.asString(); ! key.empty())
                s.headphonesOffKeys.push_back (key);

    if (auto* p = v.find ("cameras"))
        for (const auto& cv : p->asArray())
        {
            PersistedCamera camera;
            if (auto* n = cv.find ("id")) camera.id = n->asString();

            if (camera.id.empty())
                continue;

            if (auto* n = cv.find ("enabled")) camera.enabled = n->asBool (false);
            if (auto* n = cv.find ("assignedName")) camera.assignedName = n->asString();
            if (auto* n = cv.find ("quality")) camera.quality = n->asString();

            if (auto unknown = unknownMembersOf (cv, knownShapes().camera); ! unknown.empty())
                s.unknownCameraFields[camera.id] = std::move (unknown);

            s.cameras.push_back (camera);
        }

    s.unknownFields = unknownMembersOf (v, knownShapes().top);

    return s;
}

AppSettings AppSettings::fromJsonString (const std::string& text)
{
    // JsonValue::parse throws on anything it cannot read. A preferences file is
    // never worth failing to launch over, so anything unreadable is defaults.
    // An empty file is a first launch, not a corrupt one: there is nothing to
    // have forgotten, so it is defaults without a word.
    if (text.find_first_not_of (" \t\r\n") == std::string::npos)
        return {};

    try
    {
        const auto parsed = JsonValue::parse (text);

        // The parser is lenient: it returns null, or an object it could make no
        // members out of, rather than throwing -- so the catch below almost
        // never fires, and this is where a corrupt file actually presents.
        //
        // Judged by whether ANY setting came back, not by which ones. A file
        // written by a different version legitimately carries a different set
        // of keys and must keep whatever it can -- so the test is that
        // something was read, not that a particular thing was.
        // Counted by members that carry a VALUE. A file truncated before its
        // first value finished -- {"destinationFolde -- used to parse to one
        // dangling key, which the member count read as a setting recovered, so
        // everything the user had was reset and the warning never fired. The
        // parser now drops that member; a literal null still counts as nothing.
        const bool readSomething = parsed.getType() == JsonValue::Type::Object
                                && parsed.getValuedMemberCount() > 0;

        if (! readSomething)
        {
            AppSettings defaults;
            defaults.wasUnreadable = true;
            // A file was there, so this is not a first launch. The user is
            // already being told their settings were lost; a walkthrough on
            // top of that would be a second interruption about the same thing.
            defaults.setupGuideDone = true;
            return defaults;
        }

        return fromJson (parsed);
    }
    catch (...)
    {
        AppSettings defaults;
        defaults.wasUnreadable = true;
        defaults.setupGuideDone = true;
        return defaults;
    }
}

const PersistedPort* AppSettings::findPort (const std::string& key) const
{
    const auto it = std::find_if (ports.begin(), ports.end(),
                                  [&key] (const PersistedPort& p) { return p.key == key; });
    return it != ports.end() ? &*it : nullptr;
}

const PersistedCamera* AppSettings::findCamera (const std::string& id) const
{
    const auto it = std::find_if (cameras.begin(), cameras.end(),
                                  [&id] (const PersistedCamera& c) { return c.id == id; });
    return it != cameras.end() ? &*it : nullptr;
}

bool AppSettings::isMicDisabled (const std::string& key) const
{
    return std::find (disabledMicKeys.begin(), disabledMicKeys.end(), key) != disabledMicKeys.end();
}

bool AppSettings::areHeadphonesOff (const std::string& key) const
{
    return std::find (headphonesOffKeys.begin(), headphonesOffKeys.end(), key) != headphonesOffKeys.end();
}

} // namespace mma
