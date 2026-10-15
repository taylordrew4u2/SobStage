#include "CameraSelection.h"
#include "SessionFolderNaming.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace mma {

namespace {

/// "Name" or "Name #n" (n >= 2): the ids cameras called `name` had before
/// per-device ids.
bool isNameBasedIdFor (const std::string& key, const std::string& name)
{
    if (key == name)
        return true;

    const auto prefix = name + " #";
    if (key.size() <= prefix.size() || key.compare (0, prefix.size(), prefix) != 0)
        return false;

    const auto digits = key.substr (prefix.size());
    return digits.find_first_not_of ("0123456789") == std::string::npos
        && digits != "0" && digits != "1" && digits.front() != '0';
}

} // namespace

void CameraSelection::adoptLegacyChoice (const CameraDeviceInfo& camera)
{
    if (camera.legacyId.empty() || camera.legacyId == camera.id || choices.count (camera.id) > 0)
        return;

    // Old entries for this name that no connected camera is using as its id
    // right now. A camera without a per-device id still goes by its name-based
    // id, and its choices are its own.
    std::vector<std::string> candidates;
    for (const auto& entry : choices)
    {
        if (! isNameBasedIdFor (entry.first, camera.displayName))
            continue;

        const bool inUse = std::any_of (available.begin(), available.end(),
                                        [&entry] (const CameraDeviceInfo& c) { return c.id == entry.first; });
        if (! inUse)
            candidates.push_back (entry.first);
    }

    std::string adopted;

    if (candidates.size() == 1)
        adopted = candidates.front();
    else if (std::find (candidates.begin(), candidates.end(), camera.legacyId) != candidates.end())
        adopted = camera.legacyId; // several: OS order decides, as it always did

    if (adopted.empty())
        return;

    choices[camera.id] = choices[adopted];
    choices.erase (adopted);
    adoptedFrom[adopted] = camera.id;
    adoptedSinceAsked = true;
}

std::string CameraSelection::resolveId (const std::string& id) const
{
    if (choices.count (id) > 0)
        return id;

    const auto adopted = adoptedFrom.find (id);
    return adopted != adoptedFrom.end() ? adopted->second : id;
}

bool CameraSelection::takeAdoptedLegacyChoices() noexcept
{
    return std::exchange (adoptedSinceAsked, false);
}

void CameraSelection::setAvailableCameras (std::vector<CameraDeviceInfo> cameras)
{
    available = std::move (cameras);

    // Every camera keeps an entry whether it is on or off, so turning one off
    // and unplugging it does not turn it back on when it returns.
    for (const auto& camera : available)
    {
        adoptLegacyChoice (camera);

        const bool newlySeen = choices.count (camera.id) == 0;
        auto& choice = choices[camera.id];
        choice.lastDisplayName = camera.displayName;

        // A camera seen for the first time records. Every camera plugged in
        // is meant to be in the take, each in its own file, without a trip to
        // the Cameras panel first; that panel is where one is switched OFF,
        // and that choice is kept across an unplug and a relaunch. The cost is
        // that a first launch may raise the OS camera prompt before anyone
        // has pressed record, which the panel's own wording explains.
        if (newlySeen)
            choice.enabled = true;
    }
}

std::vector<CameraDeviceInfo> CameraSelection::getUnavailableEnabledCameras() const
{
    std::vector<CameraDeviceInfo> unavailable;

    for (const auto& [id, choice] : choices)
    {
        if (! choice.enabled)
            continue;

        bool isAvailable = false;
        for (const auto& camera : available)
            if (camera.id == id)
            {
                isAvailable = true;
                break;
            }

        if (! isAvailable)
            unavailable.push_back ({ id, getDisplayName (id) });
    }

    return unavailable;
}

std::vector<CameraDeviceInfo> CameraSelection::getKnownCameras() const
{
    std::vector<CameraDeviceInfo> known;
    known.reserve (choices.size());

    for (const auto& entry : choices)
        known.push_back ({ entry.first, getDisplayName (entry.first) });

    return known;
}

void CameraSelection::setEnabled (const std::string& id, bool enabled)
{
    choices[id].enabled = enabled;

}

bool CameraSelection::isEnabled (const std::string& id) const
{
    const auto it = choices.find (id);
    return it != choices.end() && it->second.enabled;
}

int CameraSelection::getEnabledCount() const
{
    int count = 0;

    for (const auto& camera : available)
        if (isEnabled (camera.id))
            ++count;

    return count;
}

void CameraSelection::setAssignedName (const std::string& id, const std::string& name)
{
    choices[id].assignedName = name;
}

std::string CameraSelection::getDisplayName (const std::string& id) const
{
    const auto choice = choices.find (id);

    if (choice != choices.end() && ! choice->second.assignedName.empty())
        return choice->second.assignedName;

    const auto camera = std::find_if (available.begin(), available.end(),
                                      [&id] (const CameraDeviceInfo& c) { return c.id == id; });

    if (camera != available.end())
        return camera->displayName;

    if (choice != choices.end() && ! choice->second.lastDisplayName.empty())
        return choice->second.lastDisplayName;

    // An old settings entry may predate lastDisplayName. Its id is still much
    // more useful than a blank label when explaining which device is missing.
    return id;
}

std::vector<CameraPlan> CameraSelection::buildPlans() const
{
    std::vector<CameraPlan> plans;
    std::vector<std::string> enabledIds;

    // Connected cameras keep the OS order the operator sees. A remembered
    // missing camera is not a file this plan can honestly promise.
    for (const auto& camera : available)
        if (isEnabled (camera.id))
            enabledIds.push_back (camera.id);

    int index = 0;

    for (const auto& id : enabledIds)
    {
        ++index;

        CameraPlan plan;
        plan.deviceId = id;
        plan.displayName = getDisplayName (id);

        // §6.2's rules, unchanged: sanitized, numbered, sorting in the order
        // the cameras are listed rather than by whatever the OS calls them.
        // Padded by hand rather than with snprintf into a fixed buffer, which
        // cannot be sized so that the compiler stops warning about a truncation
        // the loop bound already rules out.
        const auto number = std::to_string (index);
        plan.fileName = "V" + (number.size() < 2 ? "0" + number : number)
                      + "_" + SessionFolderNaming::sanitizeName (plan.displayName);

        plans.push_back (std::move (plan));
    }

    return plans;
}

std::vector<CameraPlan> CameraSelection::buildIntendedPlans() const
{
    auto plans = buildPlans();
    std::vector<std::string> includedIds;
    includedIds.reserve (plans.size());

    for (const auto& plan : plans)
        includedIds.push_back (plan.deviceId);

    for (const auto& [id, choice] : choices)
    {
        if (! choice.enabled
            || std::find (includedIds.begin(), includedIds.end(), id) != includedIds.end())
            continue;

        CameraPlan plan;
        plan.deviceId = id;
        plan.displayName = getDisplayName (id);

        const auto number = std::to_string (plans.size() + 1);
        plan.fileName = "V" + (number.size() < 2 ? "0" + number : number)
                      + "_" + SessionFolderNaming::sanitizeName (plan.displayName);

        plans.push_back (std::move (plan));
    }

    return plans;
}

int64_t CameraSelection::getEstimatedBytesPerSecond() const
{
    int64_t total = 0;

    for (const auto& camera : available)
        if (isEnabled (camera.id))
            total += estimatedBytesPerSecondFor (getQuality (camera.id));

    return total;
}

void CameraSelection::setQuality (const std::string& id, CameraQuality quality)
{
    choices[id].quality = quality;
}

CameraQuality CameraSelection::getQuality (const std::string& id) const
{
    const auto it = choices.find (id);
    return it != choices.end() ? it->second.quality : CameraQuality::Best;
}

CameraFrameLimit CameraSelection::frameLimitFor (CameraQuality quality) noexcept
{
    switch (quality)
    {
        case CameraQuality::HD1080: return { 1920, 1080 };
        case CameraQuality::HD720:  return { 1280, 720 };
        case CameraQuality::Best:   break;
    }

    return { 4096, 2160 };
}

int64_t CameraSelection::estimatedBytesPerSecondFor (CameraQuality quality) noexcept
{
    switch (quality)
    {
        case CameraQuality::HD1080: return 4 * 1000 * 1000; // ~32 Mbit/s
        case CameraQuality::HD720:  return 2 * 1000 * 1000; // ~16 Mbit/s
        case CameraQuality::Best:   break;
    }

    return kEstimatedVideoBytesPerSecond;
}

std::string CameraSelection::spaceAnHourText (double bytesPerSecond)
{
    // Decimal gigabytes, the unit a card or drive is sold in, so the figure
    // can be held up against what is printed on the card.
    const double gigabytesAnHour = std::max (0.0, bytesPerSecond) * 3600.0 / 1.0e9;
    const auto rounded = static_cast<long long> (std::llround (gigabytesAnHour));

    const std::string amount = gigabytesAnHour < 1.0 ? std::string ("less than 1 GB")
                                                     : "about " + std::to_string (rounded) + " GB";

    return "Recording now takes up to " + amount + " an hour, sound included.";
}

const char* cameraQualityKey (CameraQuality quality) noexcept
{
    switch (quality)
    {
        case CameraQuality::HD1080: return "1080p";
        case CameraQuality::HD720:  return "720p";
        case CameraQuality::Best:   break;
    }

    return "best";
}

CameraQuality cameraQualityFromKey (const std::string& key) noexcept
{
    if (key == "1080p") return CameraQuality::HD1080;
    if (key == "720p")  return CameraQuality::HD720;
    return CameraQuality::Best;
}

PreviewSettings CameraSelection::previewSettingsFor (PreviewQuality quality)
{
    // Low is 180 lines: enough to see who is in shot and whether the lens cap
    // is on, and a fraction of the pixels of the picture being recorded. Full
    // is 540, which is a real check of focus and framing without ever being
    // what determines what is written.
    return quality == PreviewQuality::Full ? PreviewSettings { 540 }
                                           : PreviewSettings { 180 };
}

} // namespace mma
