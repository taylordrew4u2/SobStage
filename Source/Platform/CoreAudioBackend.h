#pragma once
#include <atomic>
#include "IAudioBackend.h"
#include "PlatformMacros.h"

#if JUCE_MAC

#include "CallbackWatchdogSleepGuard.h"
#include <memory>
#include <string>

namespace mma {

/// One open CoreAudio device stream: its AudioObjectID, IOProc registration and
/// the callback to forward into. Defined in the .cpp so this header stays free
/// of <CoreAudio/CoreAudio.h>.
struct CoreAudioStream;
struct CoreAudioPendingInputAttempts;
struct CoreAudioDeviceListListenerState;

/// macOS implementation of IAudioBackend using CoreAudio directly (not JUCE's
/// generic AudioIODeviceType) so we get exclusive/hog-mode control and raw
/// AudioObjectID-level device change notifications per §5.4/§11. Every USB
/// mic on macOS is a HAL AudioObjectID. Input enumeration admits only directly
/// attached USB, FireWire, and Thunderbolt hardware; built-in, phone/
/// Continuity, wireless, aggregate, and virtual inputs are not recording
/// sources. Hotplug arrives via
/// kAudioHardwarePropertyDevices property listeners, never a timer (§2); the
/// same callback also fires when coreaudiod restarts and when an output's data
/// source or jack state changes (headphones in an Intel Mac's jack).
/// Every open stream also watches its nominal rate, alive state, and processor
/// overload property; those callbacks only touch atomics because CoreAudio may
/// deliver overload notifications on the device IO thread.
class CoreAudioBackend : public IAudioBackend
{
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    std::string getBackendName() const override { return "CoreAudio"; }

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

    std::string getLastOpenError() const override { return lastOpenError; }

    std::string getHotplugProblem() const override { return hotplugProblem; }

    std::vector<StreamFailure> takeStreamFailures() override;
    uint64_t getFramesDroppedByBackend() const override;
    int getGrantedOutputBufferFrames() const override;
    int getOutputPresentationLatencyFrames() const override;
    int getInputLatencyFrames (const std::string& deviceId) const override;
    uint64_t getOutputGlitchCount() const override;
    bool isOutputOffRate() const noexcept override { return outputOffRate->load(); }

private:
    // Open streams, each owning its IOProc registration. Held as pointers so
    // the address handed to CoreAudio as clientData stays stable.
    std::vector<std::unique_ptr<CoreAudioStream>> openStreams;

    // CoreAudio receives a raw pointer for the system device-list listener.
    // Its separately-owned state has its own callback lease gate, so destroying
    // the backend can make an already-dispatched notification inert and drain
    // a callback already in progress without exposing `this` after teardown.
    std::unique_ptr<CoreAudioDeviceListListenerState> deviceListListenerState;
    bool deviceListListenerInstalled = false;

    // coreaudiod restarting (kAudioHardwarePropertyServiceRestarted), an
    // output's route changing (kAudioDevicePropertyDataSource /
    // kAudioDevicePropertyJackIsConnected: headphones into or out of an Intel
    // Mac's jack, which changes no device list) and the app's combined device
    // changing shape (its active sub-device list and output stream
    // configuration: a microphone dropping out of it or joining it late) are
    // delivered through the same listener state, trampoline and callback as
    // the device list, so they share its lease gate and its process-lifetime
    // retirement. Each registration is remembered so it is removed before
    // that state is retired.
    bool serviceRestartListenerInstalled = false;

    struct OutputRouteListener
    {
        uint32_t device = 0;   // AudioObjectID
        uint32_t selector = 0; // AudioObjectPropertySelector
        uint32_t scope = 0;    // AudioObjectPropertyScope
    };

    std::vector<OutputRouteListener> outputRouteListeners;

    std::string lastOpenError;

    std::string hotplugProblem;

    /// isOutputOffRate(): raised by the open output's nominal-rate listener
    /// on the HAL's word, lowered by takeStreamFailures() when it reads the
    /// stream's rate back, and on close. Shared with that output's stream
    /// (CoreAudioStream::outputOffRate), which can outlive this backend.
    std::shared_ptr<std::atomic<bool>> outputOffRate = std::make_shared<std::atomic<bool>> (false);

    /// A device that would not take the requested buffer size. Not a failure --
    /// the stream opens and records -- but the extra latency was invisible.
    ///
    /// Atomic because it is written while opening a stream and read by
    /// takeStreamFailures(); both are message-thread today, and this makes the
    /// flag correct without depending on that staying true.
    std::atomic<bool> bufferSizeWasRefused { false };

    /// Keeps the callback watchdog in takeStreamFailures() from calling the
    /// rig dead because the Mac slept. Message thread, as that is.
    CallbackWatchdogSleepGuard watchdogSleepGuard;

    // A timed-out HAL call is deliberately left on its worker until CoreAudio
    // returns and that worker can tear the IOProc down. The registry outlives
    // this backend when necessary and stops device-list churn from launching a
    // second stuck audio transaction in the meantime.
    std::shared_ptr<CoreAudioPendingInputAttempts> pendingInputAttempts;

    static std::vector<AudioDeviceDescriptor> enumerateDevices (bool wantInput);
    void installDeviceListListener();
    void removeDeviceListListener();

    /// Message thread only. Brings the output-route listeners in line with the
    /// outputs now listed: one per route property each output actually has,
    /// and the combined device's layout properties while it is listed; none
    /// left on a device that has gone.
    void syncOutputRouteListeners();
    bool openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                     AudioCallback callback, bool isOutput);

   #if defined (MMA_SIMULATE_MAC)
public:
    /// Simulation-only synchronization point for delayed HAL-call tests.
    bool waitForPendingInputAttemptsForTesting (int timeoutMilliseconds);

    /// Exercises the no-thread fallback used when the OS refuses to create the
    /// detached teardown owner. The shipping build has no injection point.
    void failNextCleanupWorkerStartForTesting() noexcept
    {
        failNextCleanupWorkerStart = true;
    }

private:
    bool failNextCleanupWorkerStart = false;
   #endif
};

} // namespace mma

#endif // JUCE_MAC
