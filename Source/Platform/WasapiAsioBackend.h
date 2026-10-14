#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "IAudioBackend.h"
#include "PlatformMacros.h"

#if JUCE_WINDOWS

#include <memory>

namespace mma {

/// One open WASAPI exclusive-mode stream: its IAudioClient, service interface,
/// event handle and worker thread. Defined in the .cpp so <audioclient.h> stays
/// out of this header.
struct WasapiStream;

/// Windows implementation of IAudioBackend using WASAPI in EXCLUSIVE mode.
/// ASIO support remains a separate, explicitly unimplemented virtual-device
/// backend; merely finding an unrelated ASIO driver on the machine must never
/// bypass the capability checks for the WASAPI streams this class opens.
/// §5.4: shared-mode WASAPI is explicitly disqualified
/// for the monitor path regardless of which app-facing backend (§7) is
/// active -- it is never selected here even as a last resort, because a
/// silent 40-100ms mix is worse than telling the user why low-latency
/// monitoring isn't available.
class WasapiAsioBackend : public IAudioBackend
{
public:
    WasapiAsioBackend();
    ~WasapiAsioBackend() override;

    std::string getBackendName() const override { return "WASAPI (exclusive)"; }

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
    OpenRefusal getLastOpenRefusal() const override { return lastOpenRefusal; }

    std::string getHotplugProblem() const override { return hotplugProblem; }

    std::vector<StreamFailure> takeStreamFailures() override { return streamFailures.take(); }

    uint64_t getFramesDroppedByBackend() const override;
    int getGrantedOutputBufferFrames() const override;

private:
    std::string lastOpenError;
    OpenRefusal lastOpenRefusal = OpenRefusal::unknown;
    std::string hotplugProblem;

    /// §0.1: where the worker threads leave a stream that stopped on its own.
    StreamFailureSink streamFailures;

    /// CoInitializeEx can report RPC_E_CHANGED_MODE when JUCE already owns the
    /// message thread's apartment. Only a successful call earns a matching
    /// CoUninitialize; otherwise tearing down this backend would pop JUCE's
    /// COM initialisation instead of our own.
    bool ownsComInitialisation = false;
    DeviceChangeCallback deviceChangeCallback;

    // Opaque IMMNotificationClient registration handle; the concrete COM
    // object is defined in the .cpp to avoid pulling <mmdeviceapi.h> into
    // every translation unit that includes this header.
    void* notificationClient = nullptr;

    std::vector<std::unique_ptr<WasapiStream>> openStreams;

    /// Outstanding open workers. Shared rather than owned, because a worker
    /// the caller stopped waiting for can outlive this backend: it must have
    /// somewhere to report finishing that is still alive when it does.
    struct PendingOpens
    {
        std::mutex mutex;
        std::condition_variable changed;
        int running = 0;
    };

    std::shared_ptr<PendingOpens> pendingOpens = std::make_shared<PendingOpens>();

public:
    /// Waits for every abandoned open worker to finish, up to a bound. Only
    /// harnesses need this: a worker still inside the driver when the next
    /// test tears the device down is a race, and the test has no other way to
    /// know it has left.
    bool waitForPendingOpensForTesting (int timeoutMilliseconds);

private:

    /// §2: unregisters and releases the hotplug notification client. Safe to
    /// call when none is registered.
    void unregisterNotificationClient();

    std::vector<AudioDeviceDescriptor> enumerateWasapiDevices (bool wantInput) const;

    /// Opens a WASAPI stream in AUDCLNT_SHAREMODE_EXCLUSIVE. Never opens
    /// shared mode for the monitor path -- see class doc.
    /// Does the whole COM open -- Activate, format negotiation, Initialize,
    /// service acquisition -- and returns the finished stream, or nullptr with
    /// the reason in `openError` and its kind in `refusal`. Deliberately
    /// touches no member: it runs on a worker that openWasapiExclusiveStream()
    /// may stop waiting for.
    std::unique_ptr<WasapiStream> buildExclusiveStream (const std::string& deviceId,
                                                        double sampleRate, int bufferSizeSamples,
                                                        bool isInput, AudioCallback callback,
                                                        std::string& openError,
                                                        OpenRefusal& refusal);

    bool openWasapiExclusiveStream (const std::string& deviceId, double sampleRate,
                                    int bufferSizeSamples, bool isInput, AudioCallback callback);
};

} // namespace mma

#endif // JUCE_WINDOWS
