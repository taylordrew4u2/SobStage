#include "BusPowerDetector.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace mma {

bool BusPowerDetector::recordEvent (double nowSeconds, const std::string& deviceId,
                                    const std::string& name, int devicesAttached)
{
    // Waking from sleep: every device drops and comes back, which is not power.
    if (wokeAt.has_value() && std::abs (nowSeconds - *wokeAt) <= kWakeGraceSeconds)
        return false;

    // Still failing from before: a rebuild that reopens the rig, another mic
    // plugged in, or Stop reopening a mic that died mid-take all ask again,
    // and the answer is the same failure, not a second one.
    if (! failingNow.insert (deviceId).second)
        return false;

    events.push_back ({ nowSeconds, deviceId, name, devicesAttached });
    prune (nowSeconds);
    return true;
}

void BusPowerDetector::noteHealthy (const std::string& deviceId)
{
    failingNow.erase (deviceId);
}

void BusPowerDetector::noteLeft (const std::string& deviceId)
{
    failingNow.erase (deviceId);
}

void BusPowerDetector::noteSystemWoke (double nowSeconds)
{
    wokeAt = nowSeconds;

    // Drops of the wake noticed before the wake itself was: forgotten, and
    // their devices free to count a real failure later.
    for (auto it = events.begin(); it != events.end();)
    {
        if (it->timestamp >= nowSeconds - kWakeGraceSeconds)
        {
            failingNow.erase (it->deviceId);
            it = events.erase (it);
        }
        else
        {
            ++it;
        }
    }
}

int devicesAttachedAtDrop (int listedNow, const std::vector<std::string>& takeDeviceIds)
{
    const std::set<std::string> takeDevices (takeDeviceIds.begin(), takeDeviceIds.end());
    return std::max (listedNow, static_cast<int> (takeDevices.size()));
}

void BusPowerDetector::prune (double nowSeconds)
{
    while (! events.empty() && ! inWindow (events.front(), nowSeconds))
        events.pop_front();
}

bool BusPowerDetector::isBusPowerExhausted (double nowSeconds) const
{
    std::set<std::string> devices;
    bool hadEnoughMics = false;

    for (const auto& e : events)
    {
        if (! inWindow (e, nowSeconds))
            continue;

        devices.insert (e.deviceId);

        if (e.devicesAttached >= kMinMicsForConcern)
            hadEnoughMics = true;
    }

    return static_cast<int> (devices.size()) >= kDistinctDevicesThreshold && hadEnoughMics;
}

std::vector<std::string> BusPowerDetector::namesInWindow (double nowSeconds) const
{
    std::vector<std::string> names;
    std::set<std::string> seen;

    for (const auto& e : events)
        if (inWindow (e, nowSeconds) && seen.insert (e.deviceId).second)
            names.push_back (e.name);

    return names;
}

std::string BusPowerDetector::repeatingDevice (double nowSeconds) const
{
    std::map<std::string, int> counts;
    std::string name;

    for (const auto& e : events)
    {
        if (! inWindow (e, nowSeconds))
            continue;

        ++counts[e.deviceId];
        name = e.name;
    }

    if (counts.size() != 1 || counts.begin()->second < 2)
        return {};

    return name;
}

} // namespace mma
