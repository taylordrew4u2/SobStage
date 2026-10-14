#pragma once

// Guarded on __linux__, not a JUCE macro. The compiler defines this one itself,
// so unlike JUCE_MAC / JUCE_WINDOWS it cannot silently evaluate to 0 and leave
// an empty object file -- which is exactly how both shipping platforms once
// ended up unable to link. It also keeps this backend out of the JUCE
// dependency, so the headless test build can drive a real OS audio API.
//
// MMA_NO_ALSA is set by CMake when the ALSA development headers are absent, so
// a Linux box without them still builds everything else.
#if defined(__linux__) && ! defined(MMA_NO_ALSA)

#include "IAudioBackend.h"
#include <memory>
#include <vector>

namespace mma {

/// One open ALSA stream: its PCM handle, worker thread and conversion scratch.
/// Defined in the .cpp so <alsa/asoundlib.h> stays out of this header.
struct AlsaStream;

/// Linux implementation of IAudioBackend, on ALSA directly rather than through
/// JUCE, for the same reason as the other two: §5.4 needs control over whether
/// the device is opened exclusively, and §2 needs device-change notification
/// that is not a timer.
///
/// §5.4 on ALSA: a `hw:` device is exclusive by construction -- the kernel
/// hands it to one client. `default`, `plughw:` and `dmix` are the shared,
/// resampled paths and are reported as non-exclusive, so the monitor path
/// refuses them rather than silently delivering a mixed, high-latency stream.
class AlsaBackend : public IAudioBackend
{
public:
    AlsaBackend();
    ~AlsaBackend() override;

    std::string getBackendName() const override { return "ALSA"; }

    std::vector<AudioDeviceDescriptor> enumerateInputDevices() override;
    std::vector<AudioDeviceDescriptor> enumerateOutputDevices() override;
    void setDeviceChangeCallback (DeviceChangeCallback callback) override;

    ExclusiveModeCapability checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                          double sampleRate, int bufferSizeSamples) override;

    bool openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                    int bufferSizeSamples, AudioCallback callback) override;

    bool openInputStream (const std::string& inputDeviceId, double sampleRate,
                          int bufferSizeSamples, AudioCallback callback) override;

    void closeAllStreams() override;

    std::vector<StreamFailure> takeStreamFailures() override { return streamFailures.take(); }

    /// §5.4 asks for the cause to be named when a stream will not open. This
    /// backend never overrode it, so every ALSA refusal reached the user as the
    /// generic "couldn't open your headphones" with nothing to act on.
    std::string getLastOpenError() const override { return lastOpenError; }
    OpenRefusal getLastOpenRefusal() const override { return lastOpenRefusal; }

    std::string getHotplugProblem() const override { return hotplugProblem; }

    uint64_t getFramesDroppedByBackend() const override;
    uint64_t getOutputGlitchCount() const override;
    int getGrantedOutputBufferFrames() const override;

private:
    std::string lastOpenError;
    OpenRefusal lastOpenRefusal = OpenRefusal::unknown;
    std::string hotplugProblem;

    /// §0.1: where the worker threads leave a stream that stopped on its own.
    StreamFailureSink streamFailures;

    DeviceChangeCallback deviceChangeCallback;
    std::vector<std::unique_ptr<AlsaStream>> openStreams;

    /// inotify watch on /dev/snd, so hotplug arrives from the kernel rather
    /// than a timer (§2). The watcher owns an explicit eventfd wake-up so its
    /// thread has a bounded teardown even when no device event ever arrives.
    std::unique_ptr<struct AlsaHotplugWatcher> hotplug;

    std::vector<AudioDeviceDescriptor> enumerate (bool wantInput, bool probeCapabilities) const;
    bool openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                     bool isInput, AudioCallback callback);
};

} // namespace mma

#endif // __linux__ && ! MMA_NO_ALSA
