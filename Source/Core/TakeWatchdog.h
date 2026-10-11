#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace mma {

/// One reading of everything that can go wrong in the middle of a take, in
/// the terms the user thinks in: which microphones are sending sound, which
/// cameras are still there, whether the disk is keeping up.
struct TakeHealth
{
    struct Mic    { std::string name; bool live = true; };

    /// `id` is the camera's device id and is what the watchdog matches on;
    /// `name` is only for the sentence. Two cameras of the same model show
    /// the same name, and matching on it read one twin's state as the
    /// other's: a twin that never started looked like a loss at the start of
    /// the take, and the latch then hid the real loss of the other. Empty
    /// falls back to the name. Last, so `{ name, present }` still builds.
    struct Camera { std::string name; bool present = true; std::string id {}; };

    std::vector<Mic> mics;
    std::vector<Camera> cameras;

    /// The camera controller's own complaint, empty when it has none.
    std::string cameraProblem;
    /// The monitor path's complaint (a device that would not open, a rate
    /// refused), empty when healthy.
    std::string monitorProblem;

    /// Audio frames this app dropped so far in the take. Any increase is
    /// sound that is gone.
    uint64_t framesDropped = 0;
    /// Samples the per-device rings discarded because nothing pulled them in
    /// time. Any increase is sound that is gone.
    uint64_t samplesOverrun = 0;

    /// True while the headphone output has stopped calling back and the take
    /// is running on the computer's own clock.
    bool outputClockLost = false;
    /// True while the headphone output has been set aside because it was
    /// switched to another sample rate (CaptureCoordinator::
    /// abandonOutputClock). The take runs on the computer's clock then too,
    /// but the monitor problem says why in its own words, so the "has
    /// stopped -- unplugged, or taken by another app" alert is not raised
    /// over it.
    bool outputRateChanged = false;
    /// True while the writer is falling behind (ring buffer past its warning
    /// mark) and true while it has given up the stems and kept only the mix.
    bool writerBehind = false;
    bool mixOnly = false;

    /// Room left on the destination, in seconds. Negative when unknown.
    double remainingSeconds = -1.0;

    /// The rate the take is actually running at. The seconds figure below was
    /// computed against a hardcoded 48000, so at 96 kHz it said twice the audio
    /// had been lost -- the same wrong-magnitude family as counting channel
    /// frames as wall-clock ones. Zero or less means "unknown", and the
    /// sentence falls back to 48 kHz rather than dividing by nothing.
    double sampleRate = 0.0;

    /// How far into the take this reading was taken. Used to space out
    /// repeats of the same complaint.
    double elapsedSeconds = 0.0;
};

struct TakeAlert
{
    enum class Kind
    {
        // No CameraBack. A camera cannot rejoin an interrupted movie, so the
        // loss is latched for the whole take -- which made the "is back"
        // branch unreachable for a genuine return, and reachable only by a
        // camera that had not gone anywhere. The message is gone rather than
        // left in place looking like a feature.
        MicLost, MicBack, CameraLost, CameraTrouble,
        AudioDropped, WriterBehind, MixOnly, MonitorTrouble,
        OutputLost, OutputBack,
        TenMinutesLeft, TwoMinutesLeft,
    };

    Kind kind;
    /// §10.6: what happened, then what it means, in plain language.
    std::string message;
    /// True for the good news: something that was lost has come back.
    bool recovery = false;
};

/// §0.1 and §6.5: nothing that goes wrong mid-take may go unsaid. The main
/// screen has always shown the state; this turns a CHANGE in it into an
/// alert, once per change, so a pop-up can name it the moment it happens
/// rather than leaving the user to notice a dashed meter.
///
/// Plain data in and alerts out, no clock and no UI, so every rule here is
/// held by a test.
class TakeWatchdog
{
public:
    /// The take has started; this is what healthy looks like. Nothing in the
    /// baseline is an alert, however bad -- a mic that was already dead when
    /// record was pressed was on screen before the press.
    void beginTake (const TakeHealth& baseline);
    void endTake();
    bool isWatching() const { return watching; }

    /// Compares against the last reading and reports what changed for the
    /// worse, and what came back. Empty when nothing changed.
    std::vector<TakeAlert> observe (const TakeHealth& now);

    static constexpr double kTenMinutes = 600.0;
    static constexpr double kTwoMinutes = 120.0;

    /// Dropped audio is said when it starts and then no more than once a
    /// minute, with the running total: a machine that is struggling drops a
    /// little on every tick, and a card that grows a line twice a second is
    /// one nobody can read or dismiss.
    static constexpr double kDroppedRepeatSeconds = 60.0;

private:
    bool watching = false;
    TakeHealth last;
    bool warnedTenMinutes = false;
    bool warnedTwoMinutes = false;
    bool droppedReported = false;
    double droppedReportedAt = 0.0;
    /// Keyed by device id (the name where there is none), never by name alone.
    std::set<std::string> lostCamerasThisTake;
};

} // namespace mma
