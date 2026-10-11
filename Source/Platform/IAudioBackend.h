#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <mutex>
#include <utility>

namespace mma {

struct AudioDeviceDescriptor
{
    std::string name;          // e.g. "Blue Yeti"
    std::string usbLocationId; // §2.4 identity
    std::string serialNumber;  // may be empty
    bool isBuiltIn = false;    // the machine's own mic, not something plugged in
    int maxInputChannels = 0;
    std::vector<uint32_t> supportedSampleRates;
    /// The rate the device is running at now, or 0 when the backend cannot say.
    /// Advertising a rate is not the same as being willing to switch to it.
    uint32_t currentSampleRate = 0;
    std::vector<int> supportedBitDepths;
    bool isMicrophone = false;
    bool hasPhysicalHeadphoneJack = false; // relevant for output-device candidates (§5.3)
    /// Output only: this is the output the OS itself currently plays through
    /// (§5.3 priority 4). On a Mac that is usually the headphones or amp the
    /// user plugged in, because macOS switches its default to them.
    bool isSystemDefault = false;
    /// Output only: Bluetooth, Bluetooth LE or AirPlay. Such a device can
    /// connect on its own (AirPods, a paired speaker powering on), so its
    /// arrival is not evidence that the user plugged in monitoring headphones.
    bool isWireless = false;
};

/// §5.4: the monitor path requires exclusive-mode audio. This describes what a
/// candidate device/mode combination can actually deliver.
struct ExclusiveModeCapability
{
    bool exclusiveModeAvailable = false;
    double measuredOrEstimatedLatencyMs = 0.0;
    std::string unavailableReason; // populated when exclusiveModeAvailable is false
};

/// Machine-readable cause for a backend event. Most historical backends only
/// supplied prose, so unknown remains the compatibility default. The CoreAudio
/// rate/alive listeners use explicit values because the application must react
/// to those state changes, not parse user-facing English.
enum class StreamFailureKind
{
    unknown,
    sampleRateChanged,
    deviceUnavailable,
    processorOverload,
    safetyMonitoringUnavailable,

    /// Not a failure: an input stream reported dead has started delivering
    /// again on the same stream. The owner takes back what the dead report
    /// did -- its channel goes live again -- and says so. With no device id it
    /// is the headphone output, back at the rate it was opened at after a
    /// sampleRateChanged report (see outputRateChangeAction()).
    resumed
};

/// Application policy for typed backend events. A live sample-rate change
/// invalidates the WAV format the take was opened with, so continuing would
/// knowingly write audio under a stale header. Disconnects retain the frozen
/// take layout and write silence (§6.5); overload/listener reports are visible
/// warnings rather than reasons to discard the rest of a take.
///
/// A rate change on the headphone output alone is not a microphone's, and is
/// decided by outputRateChangeAction() before this is asked.
constexpr bool streamFailureRequiresRecordingStop (StreamFailureKind kind) noexcept
{
    return kind == StreamFailureKind::sampleRateChanged;
}

constexpr bool streamFailureIsWarning (StreamFailureKind kind) noexcept
{
    return kind == StreamFailureKind::processorOverload
        || kind == StreamFailureKind::safetyMonitoringUnavailable;
}

/// §0.1: what the owner does about a microphone whose input stream has died
/// while its device is still listed (one that has left the list is an unplug,
/// handled as one).
enum class DeadInputStreamAction
{
    reopenNow,    // nothing is recording: reopen the streams straight away
    reopenAtStop, // a take is running: its channels are fixed, so Stop reopens
    leaveDead     // reopened a moment ago and died again: show it dead, retry no more
};

/// A device that dies again within this long of its streams being reopened is
/// left dead rather than reopened again, so one that keeps failing cannot hold
/// the rig in a restart loop.
constexpr double kDeadInputReopenBackoffMs = 30000.0;

constexpr DeadInputStreamAction deadInputStreamAction (bool recording, bool reopenedRecently) noexcept
{
    if (reopenedRecently)
        return DeadInputStreamAction::leaveDead;

    return recording ? DeadInputStreamAction::reopenAtStop : DeadInputStreamAction::reopenNow;
}

/// §0.1: whether an input stream that was called dead, because its callback
/// stopped arriving, is delivering again. CoreAudio's check is only a timeout
/// on a callback the HAL may resume: across system sleep, another app's brief
/// hog-mode grab, or an interface re-locking its clock the IOProc stops and
/// then runs again on the same device. Mid-take nothing reopens a stream, so
/// without this the mic's track stayed silent for the rest of the take while
/// its audio was arriving.
///
/// `deadReportedAt` is when the dead report was made (0: never, or already
/// taken back), `lastCallback` when the stream last delivered, on the same
/// clock. A device the OS itself says is unavailable is not revived by this.
constexpr bool inputStreamHasResumed (double deadReportedAt, double lastCallback,
                                      bool deviceUnavailable) noexcept
{
    return deadReportedAt > 0.0 && lastCallback > deadReportedAt && ! deviceUnavailable;
}

/// A stream that died or became unsafe after it had been opened. See
/// takeStreamFailures().
struct StreamFailure
{
    /// The device the stream was opened for. Empty for the monitor output.
    std::string deviceId;

    /// §10.6: what happened, in the user's words. The owner puts the device's
    /// name in front of it -- the backend only knows its id.
    std::string reason;

    /// A complete subject to use instead of the device's name, for the reports
    /// that are not about one device ("The sound card", "This interface").
    /// Empty means the owner names the device, which is the usual case.
    std::string subject {};

    StreamFailureKind kind = StreamFailureKind::unknown;
};

/// The headphone output's own sample rate changed under the app -- another
/// app, Audio MIDI Setup or a music player switched the device -- rather than
/// a microphone's. The backend names no device for the monitor output.
inline bool streamFailureIsOutputRateChange (const StreamFailure& failure) noexcept
{
    return failure.kind == StreamFailureKind::sampleRateChanged && failure.deviceId.empty();
}

/// §5.3: what the owner does when the headphone output changes sample rate.
///
/// The output's callback is the clock the whole rig is pulled on (§3.2). At
/// the wrong rate it pulls every microphone's ring too fast or too slow, so
/// leaving it running loses audio from every track -- and the ring losses
/// then read as the computer being too slow, and raise the buffer size.
enum class OutputRateChangeAction
{
    /// The output carries take audio itself (the combined device, whose
    /// rate is the microphones', or a mixer that is both): the take's format
    /// would become untrue, so it stops, as for a microphone's rate change.
    stopTake,
    /// Mid-take, an output that carries none of it: the output stops being
    /// the clock and the software clock takes over, so the take carries on
    /// intact with the headphones silent. Stop reopens the output.
    carryOnWithoutIt,
    /// Nothing recording: reopen the rig at its own rate now, which sets the
    /// output back.
    reopenNow,
    /// Changed again straight after such a reopen -- something keeps
    /// switching it. Rather than fight it in a loop, the output stops being
    /// the clock (headphones silent, recording unaffected) until it is back
    /// at the rig's rate.
    setAside
};

constexpr OutputRateChangeAction outputRateChangeAction (bool recording,
                                                         bool outputCarriesTakeAudio,
                                                         bool reopenedRecently) noexcept
{
    if (recording)
        return outputCarriesTakeAudio ? OutputRateChangeAction::stopTake
                                      : OutputRateChangeAction::carryOnWithoutIt;

    return reopenedRecently ? OutputRateChangeAction::setAside : OutputRateChangeAction::reopenNow;
}

/// Where a backend's worker threads leave a failure for the message thread to
/// pick up. Locked rather than lock-free: a stream dying is a once-per-session
/// event, and the alternative -- a fixed slot that the second failure
/// overwrites -- loses exactly the information this exists to keep.
///
/// note() is called from a worker thread that has already stopped feeding the
/// audio callback, so it is not on the real-time path and may lock.
class StreamFailureSink
{
public:
    void note (std::string deviceId, std::string reason, std::string subject = {})
    {
        const std::lock_guard<std::mutex> guard (lock);

        // A device that fails repeatedly says it once. The owner turns this
        // into a sentence for the user, not a counter.
        for (const auto& existing : failures)
            if (existing.deviceId == deviceId)
                return;

        failures.push_back ({ std::move (deviceId), std::move (reason), std::move (subject) });
    }

    std::vector<StreamFailure> take()
    {
        const std::lock_guard<std::mutex> guard (lock);
        return std::exchange (failures, {});
    }

    void clear()
    {
        const std::lock_guard<std::mutex> guard (lock);
        failures.clear();
    }

private:
    std::mutex lock;
    std::vector<StreamFailure> failures;
};

using AudioCallback = std::function<void (const float* const* inputChannels, int numInputChannels,
                                          float* const* outputChannels, int numOutputChannels,
                                          int numSamples)>;
using DeviceChangeCallback = std::function<void()>;

/// Platform audio backend interface (§11: CoreAudio on macOS, WASAPI
/// exclusive + ASIO on Windows). Implementations enumerate input devices,
/// open exclusive-mode streams for the monitor path, and deliver a
/// synchronous audio callback per §11 ("no allocation, locking, logging,
/// file I/O, or drawing" inside it).
class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    virtual std::string getBackendName() const = 0;

    /// Enumerate all USB audio input devices, called at launch and again on
    /// every OS device-change notification (§2, never on a timer).
    virtual std::vector<AudioDeviceDescriptor> enumerateInputDevices() = 0;

    virtual std::vector<AudioDeviceDescriptor> enumerateOutputDevices() = 0;

    /// Registers a callback the backend invokes on its own OS device-change
    /// notification mechanism (IONotification/CoreAudio listener on macOS,
    /// WM_DEVICECHANGE/MMNotificationClient on Windows).
    virtual void setDeviceChangeCallback (DeviceChangeCallback callback) = 0;

    /// Checks whether exclusive mode is available for the given output device
    /// at the given sample rate/buffer size, per §5.4.
    virtual ExclusiveModeCapability checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                  double sampleRate, int bufferSizeSamples) = 0;

    /// Opens the monitor output stream in exclusive mode. Returns false (and
    /// should log/report why) if exclusive mode can't be obtained -- per
    /// §5.4 "never ship a 40ms mix silently".
    virtual bool openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                            int bufferSizeSamples, AudioCallback callback) = 0;

    /// A user-facing explanation of why the most recent
    /// openExclusiveOutputStream call returned false, or "" if the backend has
    /// nothing more specific to add. Kept separate from the return value so
    /// existing backends need not implement it, and so the message can name a
    /// next step rather than leaving the user at a dead end.
    virtual std::string getLastOpenError() const { return {}; }

    /// Opens one input device's capture stream.
    virtual bool openInputStream (const std::string& inputDeviceId, double sampleRate,
                                  int bufferSizeSamples, AudioCallback callback) = 0;

    /// A stream that opened successfully and has since stopped delivering
    /// audio, or received a backend safety event such as a live sample-rate
    /// change or processor overload, with the reason, taken and cleared.
    ///
    /// Every backend has a worker loop that gives up on an unrecoverable device
    /// error and exits, and none of them told anyone. The stream simply stopped:
    /// monitoring went quiet, or a microphone's track went on being written as
    /// silence for the rest of a four-hour take, and the only evidence was in
    /// the file afterwards. §0.1 does not allow that, so a backend that stops
    /// says so, and the owner turns it into a sentence.
    ///
    /// Taken rather than read, so each failure is reported once. Empty
    /// deviceId means the monitor output rather than an input. `kind`
    /// distinguishes events that require application action; user-facing prose
    /// is never an API.
    /// Called from the message thread only, like every other method on this
    /// interface except the audio callback itself. Implementations walk their
    /// own stream list, which the open/close methods mutate, so calling this
    /// from another thread would race them.
    virtual std::vector<StreamFailure> takeStreamFailures() { return {}; }

    /// §0.1: frames that reached the backend from a device and were never
    /// handed to the audio callback, cumulative since the streams opened.
    ///
    /// WritePipeline already counts what it drops on the way to disk, and that
    /// count is reported. Everything lost BEFORE the callback was invisible to
    /// it -- a packet the device refused to hand over, or one wider than the
    /// scratch the stream allocated -- so audio could be lost between the
    /// microphone and the meter with nothing anywhere saying so.
    ///
    /// Message thread only, for the same reason as takeStreamFailures().
    virtual uint64_t getFramesDroppedByBackend() const { return 0; }

    /// Recovered breaks in the monitor output: heard as a click, not lost from
    /// the recording. Counted as events, not frames -- what matters is that
    /// they are happening at all and rising.
    ///
    /// Kept apart from getFramesDroppedByBackend() because the two need
    /// opposite sentences: one says audio is being lost from the take, the
    /// other says the machine is struggling to keep the headphones fed. Putting
    /// monitor glitches on the recording counter wrote a permanent claim in a
    /// take's own record that recorded audio had been lost, when none had.
    ///
    /// Message thread only.
    virtual uint64_t getOutputGlitchCount() const { return 0; }

    /// Frames per callback the monitor output actually GRANTED, or 0 when the
    /// backend cannot say.
    ///
    /// §5.4's latency figure was worked out from the size that was ASKED for,
    /// which is what the app wanted rather than what the device agreed to. A
    /// device is free to hand back a different buffer -- CoreAudio already
    /// notices and tells the user there is "a little more delay than usual" --
    /// and the number beside that sentence was still the figure for the buffer
    /// it refused.
    ///
    /// Zero means "unknown", and the caller keeps its estimate rather than
    /// reporting a latency of nothing.
    virtual int getGrantedOutputBufferFrames() const { return 0; }

    /// What the open monitor output adds beyond its buffers, in frames: the
    /// device's own latency, its safety offset and its stream's latency.
    /// Bluetooth headphones report well over 100 ms here; leaving it out
    /// printed about 3 ms for them. Zero when unknown.
    virtual int getOutputPresentationLatencyFrames() const { return 0; }

    /// How long before the app's callback this device's input audio was
    /// actually captured, in frames: the device's input latency, its input
    /// safety offset and its input stream's latency. Two microphones on
    /// different interfaces are handed over with different delays, so the
    /// coordinator delays the earlier one by the difference to line their
    /// tracks up. For an input this backend has open; zero when unknown, or
    /// on a platform that does not report it.
    virtual int getInputLatencyFrames (const std::string& /*deviceId*/) const { return 0; }

    /// Empty unless this backend cannot watch for microphones being plugged in
    /// or pulled out. Then it says so in the user's words: nothing about the
    /// rig will be noticed until the app is restarted, and §6.5's mid-take
    /// unplug reporting cannot fire at all -- which, unsaid, looks exactly like
    /// a take where nothing went wrong.
    ///
    /// Message thread only, for the same reason as takeStreamFailures().
    virtual std::string getHotplugProblem() const { return {}; }

    virtual void closeAllStreams() = 0;
};

} // namespace mma
