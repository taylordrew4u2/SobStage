#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mma {

/// §14.2: bus power exhaustion doesn't produce a clean error; it shows up as
/// enumeration failures and device drops. Heuristic: two or more DIFFERENT
/// devices failing or dropping within 5 minutes, while 3+ devices are
/// attached, means "get a powered hub."
///
/// Different devices, because one device failing twice is that device: a bad
/// cable or port, which a powered hub does not fix. It used to count events,
/// so one mic that would not open counted again every time another was
/// plugged in, and a mic dying mid-take counted once for the death and again
/// for the reopen at Stop -- two events, "needs more power", from one cable.
class BusPowerDetector
{
public:
    static constexpr int kDistinctDevicesThreshold = 2;
    static constexpr double kWindowSeconds = 5.0 * 60.0;
    static constexpr int kMinMicsForConcern = 3;

    /// Record an enumeration failure or unexpected drop of `deviceId` at
    /// `nowSeconds` (monotonic, app-relative clock), with the number of
    /// devices attached. Ignored while that device is still failing from an
    /// earlier event: one failure episode counts once, until noteHealthy().
    /// Returns whether it was counted.
    bool recordEvent (double nowSeconds, const std::string& deviceId, const std::string& name,
                      int devicesAttached);

    /// The device opened or came back: a later failure is a new episode.
    void noteHealthy (const std::string& deviceId);

    /// The device left the device list. Whatever it was failing with is over:
    /// plugged back in -- by hand, or by USB itself after a brownout -- and
    /// failing again, that is a new failure. Only opening ended an episode,
    /// so a mic that failed, was replugged and failed again counted once for
    /// the life of the app, and neither the power advice nor "keeps losing
    /// its connection" could ever be given for it.
    void noteLeft (const std::string& deviceId);

    /// The computer has just woken from sleep, noticed at `nowSeconds`. Every
    /// device it had open drops as it sleeps and comes back as it wakes -- on
    /// Windows each stream simply dies, unexplained -- which says nothing about
    /// power: three mics on one laptop were "all lost their connection ... may
    /// need more power" after an ordinary lid close. Drops within
    /// kWakeGraceSeconds either side of it are not counted: those recorded
    /// before it (the device list can notice them before the wake is) are
    /// forgotten, those after it ignored.
    void noteSystemWoke (double nowSeconds);

    static constexpr double kWakeGraceSeconds = 60.0;

    /// True if the sustained-power-exhaustion condition currently holds.
    bool isBusPowerExhausted (double nowSeconds) const;

    /// The names of the devices with an event in the window, each once, in
    /// the order they first failed.
    std::vector<std::string> namesInWindow (double nowSeconds) const;

    /// The name of the one device that has failed twice or more in the
    /// window when no other device has failed in it; empty otherwise.
    std::string repeatingDevice (double nowSeconds) const;

    /// Forgets every recorded event. SetupAdvisor::reset() cleared its other
    /// detectors and not this one, so events from a rig that has since been
    /// unplugged could still produce "use a powered hub" for five minutes
    /// against hardware that does not need one.
    void reset()
    {
        events.clear();
        failingNow.clear();
        wokeAt.reset();
    }

private:
    struct Event
    {
        double timestamp;
        std::string deviceId;
        std::string name;
        int devicesAttached;
    };

    std::deque<Event> events;
    std::set<std::string> failingNow;
    std::optional<double> wokeAt;

    bool inWindow (const Event& e, double nowSeconds) const noexcept
    {
        return (nowSeconds - e.timestamp) <= kWindowSeconds;
    }

    void prune (double nowSeconds);
};

/// §14.2's "devices attached" for a failure or a drop: what was plugged in
/// just before it. `listedNow` is the included devices in the device list as
/// it is now -- which, for a drop seen through the device list, has already
/// lost every device that dropped with it: a hub browning out took its three
/// mics off the list first and each was counted against what was left, 0,
/// so the powered-hub advice could not be reached in the very case it is for.
/// `takeDeviceIds` is the device of every channel of the take running (each
/// was attached when it started; repeats are one device), empty between takes.
int devicesAttachedAtDrop (int listedNow, const std::vector<std::string>& takeDeviceIds);

} // namespace mma
