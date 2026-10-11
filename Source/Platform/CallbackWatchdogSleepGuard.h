#pragma once
#include <algorithm>

namespace mma {

/// §0.1: keeps the CoreAudio callback watchdog from calling every device dead
/// because the Mac went to sleep.
///
/// CoreAudio says nothing when a device stops calling its IOProc, so the
/// backend calls a stream dead once no callback has arrived for a few seconds.
/// Sleep stops every IOProc. Timed on a clock that counts sleep, an hour with
/// the lid closed read as an hour of silence: on waking, every microphone was
/// "stopped sending audio. Unplug it and plug it back in.", the headphones
/// "stopped accepting audio", and the whole rig was torn down and reopened
/// while its USB devices were still waking up.
///
/// The stamps are therefore taken on a clock that stops while the Mac sleeps
/// (the "awake" clock), and that alone takes the sleep itself out. It does not
/// take out the awake time around it: the seconds between the HAL stopping IO
/// and the Mac actually sleeping, the seconds a USB interface needs to start
/// again after waking, and a dark wake -- the Mac waking with the lid shut for
/// maintenance, app running, audio still off -- which can last a minute. So
/// this also notices the sleep, by comparing the awake clock with one that
/// keeps counting, and then holds the watchdog until audio is running again on
/// any open device (proof the Mac is fully awake and its audio back on). From
/// that moment every device gets the watchdog's usual grace to follow. If
/// nothing runs again at all, the watchdog resumes after a minute regardless,
/// so a rig that never comes back is still reported.
///
/// Message thread only; no clock of its own, so it can be tested.
class CallbackWatchdogSleepGuard
{
public:
    /// The clock that counts sleep has to gain this much on the awake clock
    /// between two polls for it to be a sleep. Awake, the two run together.
    static constexpr double kSleepDetectionSeconds = 1.0;

    /// After a sleep, how long (awake) to wait for audio to run again before
    /// judging the streams anyway. Longer than a typical dark wake.
    static constexpr double kMaxWaitForAudioSeconds = 60.0;

    /// Once per watchdog poll, before any stream is judged.
    ///
    /// `awakeNow`: now, on the clock the IOProc stamps use, which stops while
    /// the Mac sleeps. `sleepCountingNow`: now, on a clock that keeps counting
    /// through sleep. `latestCallback`: the most recent IOProc stamp across
    /// every open stream, on the awake clock (0: none yet).
    void poll (double awakeNow, double sleepCountingNow, double latestCallback) noexcept
    {
        if (polled
            && (sleepCountingNow - lastSleepCounting) - (awakeNow - lastAwake) > kSleepDetectionSeconds)
        {
            wokeAt = awakeNow;
            waitingForAudio = true;
        }

        polled = true;
        lastAwake = awakeNow;
        lastSleepCounting = sleepCountingNow;

        if (waitingForAudio
            && (latestCallback > wokeAt || awakeNow - wokeAt >= kMaxWaitForAudioSeconds))
        {
            waitingForAudio = false;
            rearmedAt = awakeNow;
        }
    }

    /// True between noticing a sleep and audio running again (or the wait
    /// running out): no stream is to be judged.
    bool isWaitingForAudio() const noexcept { return waitingForAudio; }

    /// When a stream's silence is counted from, on the awake clock: its last
    /// callback, or its start before the first one, but never earlier than
    /// the moment audio was back after the last sleep. 0 means it is not to be
    /// judged on this poll.
    double silenceBegan (double lastCallback, double startedAt) const noexcept
    {
        const double began = lastCallback > 0.0 ? lastCallback : startedAt;

        if (waitingForAudio || began <= 0.0)
            return 0.0;

        return std::max (began, rearmedAt);
    }

private:
    bool polled = false;
    double lastAwake = 0.0;
    double lastSleepCounting = 0.0;

    bool waitingForAudio = false;
    double wokeAt = 0.0;
    double rearmedAt = 0.0;
};

} // namespace mma
