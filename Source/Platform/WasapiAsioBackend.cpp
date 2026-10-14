#include "WasapiAsioBackend.h"
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <chrono>

#if JUCE_WINDOWS

#include "../Core/SampleFormat.h"

#include <windows.h>
// devpkey.h declares DEVPROPKEY values unless one translation unit includes
// initguid.h first. The full Windows app uses these keys directly, so provide
// their definitions here instead of relying on a coincidental SDK library.
#if defined (_WIN32) && ! defined (MMA_SIMULATE_WINDOWS)
#include <initguid.h>
#endif
#include <devpkey.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cfgmgr32.h>
#include <wrl/client.h>
#include <avrt.h>
#include <thread>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace mma {

/// §2 hotplug on Windows: the OS tells us, we never poll. This is the
/// counterpart to the CoreAudio property listener on macOS -- without it a mic
/// plugged in after launch is simply never noticed, because nothing else in the
/// app ever re-enumerates on its own.
///
/// Deliberately minimal: every notification funnels to the same callback, which
/// re-runs enumeration. Distinguishing "added" from "state changed" here would
/// duplicate logic DeviceManager already owns.
class DeviceNotificationClient final : public IMMNotificationClient
{
public:
    explicit DeviceNotificationClient (DeviceChangeCallback* target) : callback (target) {}

    // IUnknown. Reference counted because the enumerator holds a reference for
    // as long as the registration lives.
    ULONG STDMETHODCALLTYPE AddRef() override { return refCount.fetch_add (1) + 1; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = refCount.fetch_sub (1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;

        if (riid == __uuidof (IUnknown) || riid == __uuidof (IMMNotificationClient))
        {
            *object = static_cast<IMMNotificationClient*> (this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR) override { return fire(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR) override { return fire(); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR, DWORD) override { return fire(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow, ERole, LPCWSTR) override { return fire(); }

    // Property changes fire constantly (volume, mute, jack presence) and none
    // of them alter the device list, so re-enumerating on them would be the
    // polling §2 rules out, just triggered by a different clock.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT fire()
    {
        // These arrive on an OS-owned thread. The callback only marks the
        // device list dirty for the message thread to pick up; it must not do
        // enumeration work here.
        if (callback != nullptr && *callback)
            (*callback)();

        return S_OK;
    }

    DeviceChangeCallback* callback;
    std::atomic<ULONG> refCount { 1 };
};

struct WasapiStream
{
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioCaptureClient> capture;

    HANDLE readyEvent = nullptr;
    UINT32 bufferFrames = 0;
    int channels = 0;
    bool isInput = false;

    // The format the device actually accepted. Exclusive mode does no
    // conversion, so whatever this ends up being is what the worker has to
    // read and write byte for byte.
    int bytesPerSample = 4;
    bool sampleIsFloat = true;

    AudioCallback callback;
    std::thread worker;
    std::atomic<bool> running { false };

    /// Which device this stream is for, and where to report it dying. The
    /// worker thread is the only thing that knows the stream has stopped, and
    /// until now it was also the only thing that would ever know.
    std::string deviceId;
    StreamFailureSink* failures = nullptr;

    /// §0.1: frames that arrived from the device and were never handed to the
    /// callback. The two ways that happens in here -- a packet wider than the
    /// scratch this stream allocated, and a packet the device refused to hand
    /// over -- both used to discard audio without counting it, which is the one
    /// failure §0.1 does not allow to be invisible.
    std::atomic<uint64_t> framesDropped { 0 };

    // Pre-allocated deinterleave scratch and channel pointers. §11 forbids
    // allocation on the audio thread, so these are sized once at open time.
    std::vector<float> scratch;
    std::vector<float*> channelPointers;

    ~WasapiStream()
    {
        if (readyEvent != nullptr)
            CloseHandle (readyEvent);
    }
};

namespace {

// How long the caller waits for Windows to finish opening a device before
// giving up on it. CoreAudioBackend bounds its HAL calls the same way and for
// the same reason; this is the Windows half of that. Generous enough that a
// merely slow interface still opens, short enough that a wedged one cannot
// hold the window closed.
#if defined (MMA_SIMULATE_WINDOWS)
constexpr auto kOpenDeadline = std::chrono::milliseconds (750);
#else
constexpr auto kOpenDeadline = std::chrono::seconds (5);
#endif


std::string wideToUtf8 (const std::wstring& wide)
{
    if (wide.empty())
        return {};
    int sizeNeeded = WideCharToMultiByte (CP_UTF8, 0, wide.data(), static_cast<int> (wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result (static_cast<size_t> (sizeNeeded), 0);
    WideCharToMultiByte (CP_UTF8, 0, wide.data(), static_cast<int> (wide.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

/// Resolves the endpoint id §2.4 stores back to a live IMMDevice. Fails when
/// the device is gone, which is the normal case after an unplug.
bool resolveDevice (const std::string& deviceId, ComPtr<IMMDevice>& out)
{
    ComPtr<IMMDeviceEnumerator> enumerator;

    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (enumerator.GetAddressOf()))))
        return false;

    const int wideLength = MultiByteToWideChar (CP_UTF8, 0, deviceId.data(),
                                                static_cast<int> (deviceId.size()), nullptr, 0);
    std::wstring wide (static_cast<size_t> (wideLength), 0);
    MultiByteToWideChar (CP_UTF8, 0, deviceId.data(), static_cast<int> (deviceId.size()),
                         wide.data(), wideLength);

    return SUCCEEDED (enumerator->GetDevice (wide.c_str(), out.GetAddressOf()));
}

/// Follows a WASAPI endpoint through its device topology to the physical audio
/// filter and reads that filter's Plug and Play instance id. Windows' endpoint
/// ids themselves are opaque and cannot tell a built-in microphone from a USB
/// interface; the physical id can (for example USB\\VID_... versus HDAUDIO\\...
/// or BTHHFENUM\\...). This is the same OS relationship Device Manager uses.
bool isDirectlyAttachedExternalInput (IMMDevice* endpoint,
                                      IMMDeviceEnumerator* enumerator)
{
    if (endpoint == nullptr || enumerator == nullptr)
        return false;

    ComPtr<IDeviceTopology> topology;
    if (FAILED (endpoint->Activate (__uuidof (IDeviceTopology), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**> (topology.GetAddressOf()))))
        return false;

    ComPtr<IConnector> connector;
    if (FAILED (topology->GetConnector (0, connector.GetAddressOf())))
        return false;

    LPWSTR connectedDeviceId = nullptr;
    const auto connectionStatus = connector->GetDeviceIdConnectedTo (&connectedDeviceId);
    if (FAILED (connectionStatus) || connectedDeviceId == nullptr)
    {
        CoTaskMemFree (connectedDeviceId);
        return false;
    }

    ComPtr<IMMDevice> physicalNode;
    const auto resolveStatus = enumerator->GetDevice (connectedDeviceId,
                                                       physicalNode.GetAddressOf());
    CoTaskMemFree (connectedDeviceId);

    if (FAILED (resolveStatus) || physicalNode == nullptr)
        return false;

    ComPtr<IPropertyStore> properties;
    if (FAILED (physicalNode->OpenPropertyStore (STGM_READ, properties.GetAddressOf())))
        return false;

    PROPVARIANT instance;
    PropVariantInit (&instance);
    const auto propertyStatus = properties->GetValue (PKEY_Device_InstanceId, &instance);
    const std::wstring instanceId = SUCCEEDED (propertyStatus)
                                  && instance.vt == VT_LPWSTR
                                  && instance.pwszVal != nullptr
                                  ? std::wstring (instance.pwszVal) : std::wstring();
    PropVariantClear (&instance);

    if (instanceId.empty())
        return false;

    std::wstring upper = instanceId;
    std::transform (upper.begin(), upper.end(), upper.begin(),
                    [] (wchar_t c) { return static_cast<wchar_t> (std::towupper (c)); });

    // The prefix is the PnP enumerator namespace, not a product-name guess. It
    // excludes software and wireless endpoints before following any parents.
    // PCI/HDAUDIO are considered only so a genuinely removable Thunderbolt
    // branch can prove itself below; built-in instances fail that proof.
    const bool eligibleTransport = upper.rfind (L"USB\\", 0) == 0
                                || upper.rfind (L"1394\\", 0) == 0
                                || upper.rfind (L"PCI\\", 0) == 0
                                || upper.rfind (L"HDAUDIO\\", 0) == 0;
    if (! eligibleTransport)
        return false;

    DEVINST node = 0;
    if (CM_Locate_DevNodeW (&node, const_cast<DEVINSTID_W> (instanceId.c_str()),
                            CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return false;

    // Windows marks the top-most removable node, not necessarily the audio
    // function below it. Walk a bounded ancestor chain and require both the
    // removable capability and a removal policy that expects actual removal.
    // Missing/malformed properties and an unexpectedly deep/cyclic tree all
    // fail closed. This runs only during enumeration, never on an audio thread.
    constexpr int maxAncestorDepth = 32;
    for (int depth = 0; depth < maxAncestorDepth; ++depth)
    {
        ULONG capabilities = 0;
        ULONG capabilityBytes = sizeof (capabilities);
        DEVPROPTYPE capabilityType = DEVPROP_TYPE_EMPTY;
        const bool hasCapabilities =
            CM_Get_DevNode_PropertyW (node, &DEVPKEY_Device_Capabilities,
                                     &capabilityType,
                                     reinterpret_cast<PBYTE> (&capabilities),
                                     &capabilityBytes, 0) == CR_SUCCESS
            && capabilityBytes == sizeof (capabilities)
            && (capabilityType == DEVPROP_TYPE_INT32
                || capabilityType == DEVPROP_TYPE_UINT32);

        ULONG removalPolicy = 0;
        ULONG removalPolicyBytes = sizeof (removalPolicy);
        DEVPROPTYPE removalPolicyType = DEVPROP_TYPE_EMPTY;
        const bool hasRemovalPolicy =
            CM_Get_DevNode_PropertyW (node, &DEVPKEY_Device_RemovalPolicy,
                                     &removalPolicyType,
                                     reinterpret_cast<PBYTE> (&removalPolicy),
                                     &removalPolicyBytes, 0) == CR_SUCCESS
            && removalPolicyBytes == sizeof (removalPolicy)
            && (removalPolicyType == DEVPROP_TYPE_INT32
                || removalPolicyType == DEVPROP_TYPE_UINT32);

        if (hasCapabilities && hasRemovalPolicy
            && (capabilities & CM_DEVCAP_REMOVABLE) != 0
            && (removalPolicy == CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL
                || removalPolicy == CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL))
            return true;

        DEVINST parent = 0;
        if (CM_Get_Parent (&parent, node, 0) != CR_SUCCESS || parent == node)
            return false;

        node = parent;
    }

    return false;
}

/// 32-bit float, the format the engine works in throughout. Exclusive mode
/// requires an exact match rather than letting a mixer convert.
WAVEFORMATEXTENSIBLE makeFloat32Format (double sampleRate, int channels)
{
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD> (channels);
    format.Format.nSamplesPerSec = static_cast<DWORD> (sampleRate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD> (channels * 4);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                         : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

/// Fixed-point PCM in the same layout. Most USB microphones and many consumer
/// interfaces expose only 16- or 24-bit PCM in exclusive mode and refuse float
/// outright, so this is not an exotic fallback -- it is the common case.
WAVEFORMATEXTENSIBLE makePcmFormat (double sampleRate, int channels, int containerBits, int validBits)
{
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD> (channels);
    format.Format.nSamplesPerSec = static_cast<DWORD> (sampleRate);
    format.Format.wBitsPerSample = static_cast<WORD> (containerBits);
    format.Format.nBlockAlign = static_cast<WORD> (channels * (containerBits / 8));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = static_cast<WORD> (validBits);
    format.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                         : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return format;
}

// Use the same layouts for capability discovery and stream opening.
bool findExclusiveFormat (IAudioClient* client, double rate, int channels,
                          WAVEFORMATEXTENSIBLE& format)
{
    const int layouts[][2] = { { 32, 0 }, { 32, 32 }, { 32, 24 }, { 24, 24 }, { 16, 16 } };
    for (const auto& layout : layouts)
    {
        const auto candidate = layout[1] == 0 ? makeFloat32Format (rate, channels)
                                             : makePcmFormat (rate, channels, layout[0], layout[1]);
        if (client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                      &candidate.Format, nullptr) == S_OK)
        {
            format = candidate;
            return true;
        }
    }
    return false;
}

/// The channel counts an exclusive open tries, in order, and the one layout
/// search both the open and the capability probe run over them.
///
/// Outputs try stereo, then mono, then the device's own mix width. Inputs must
/// open every advertised socket, even when the driver also accepts mono -- a
/// mono fallback would silently omit the other tracks -- so an input with a
/// readable mix format tries that width alone.
///
/// Shared because the probe once asked about stereo alone while the open went
/// on to try the rest: mono-only and four-channel outputs were declared
/// incapable, and the user blamed a Windows setting, for hardware the open
/// would have taken on its next candidate.
bool findExclusiveFormatForAnyChannelCount (IAudioClient* client, double rate, bool isInput,
                                            WAVEFORMATEXTENSIBLE& format)
{
    int channelCandidates[3] = { isInput ? 1 : 2, isInput ? 2 : 1, 0 };
    int numChannelCandidates = 2;

    WAVEFORMATEX* mixFormat = nullptr;
    if (SUCCEEDED (client->GetMixFormat (&mixFormat)) && mixFormat != nullptr)
    {
        const int mixChannels = mixFormat->nChannels;
        if (mixChannels > 0 && isInput)
        {
            channelCandidates[0] = mixChannels;
            numChannelCandidates = 1;
        }
        else if (mixChannels > 0 && mixChannels != channelCandidates[0] && mixChannels != channelCandidates[1])
            channelCandidates[numChannelCandidates++] = mixChannels;
        CoTaskMemFree (mixFormat);
    }

    for (int c = 0; c < numChannelCandidates; ++c)
        if (findExclusiveFormat (client, rate, channelCandidates[c], format))
            return true;

    return false;
}

/// §2.3: the depths this endpoint will actually accept exclusively.
///
/// The same IsFormatSupported question findExclusiveFormat above already asks,
/// narrowed to "what widths", because the depth a stem is written at follows
/// the hardware: "do not upconvert -- it adds file size and no information".
///
/// Asked in exclusive mode because that is the only mode this app records in
/// (§5.4); a shared-mode answer would describe the Windows mixer rather than
/// the device. 24-bit is accepted in either container a device may offer it
/// in -- packed 24, or 24 valid bits inside a 32-bit container -- since both
/// mean the same thing to a listener.
///
/// EMPTY when nothing could be asked, which chooseRecordingBitDepth turns into
/// the caller's fallback rather than a guess.
std::vector<int> querySupportedBitDepths (IAudioClient* client, double rate, int channels)
{
    std::vector<int> depths;

    if (client == nullptr || channels <= 0 || rate <= 0.0)
        return depths;

    const auto accepts = [client, rate, channels] (int containerBits, int validBits)
    {
        const auto candidate = makePcmFormat (rate, channels, containerBits, validBits);
        return client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                          &candidate.Format, nullptr) == S_OK;
    };

    if (accepts (16, 16))
        depths.push_back (16);

    if (accepts (24, 24) || accepts (32, 24))
        depths.push_back (24);

    return depths;
}

/// Called when the capture side has failed often enough to be called dead.
void reportCaptureDeath (WasapiStream* stream)
{
    if (stream->failures != nullptr)
        stream->failures->note (stream->deviceId,
                                "stopped sending audio. Unplug it and plug it back in.");

    stream->running.store (false, std::memory_order_release);

    if (stream->client)
        stream->client->Stop();
}

/// Called when the render side has failed often enough to be called dead. Stops
/// the stream rather than leaving it spinning on a device the message below is
/// asking the user to reconnect.
void reportRenderDeath (WasapiStream* stream)
{
    if (stream->failures != nullptr)
        stream->failures->note (stream->deviceId,
                                "stopped accepting audio, so you can't hear anything through it. "
                                "Try selecting it again.");

    stream->running.store (false, std::memory_order_release);

    if (stream->client)
        stream->client->Stop();
}

/// Joins the calling thread to the multithreaded apartment for its lifetime.
/// The backend's constructor cannot do this for the threads it starts: it runs
/// on JUCE's message thread, which OleInitialize has already made a
/// single-threaded apartment, so its own CoInitializeEx(MULTITHREADED) fails
/// with RPC_E_CHANGED_MODE and no thread in the process is in the MTA. A COM
/// call from a worker in no apartment then fails with CO_E_NOTINITIALIZED --
/// every microphone "no longer connected". Each worker joins for itself.
struct ScopedMtaMembership
{
    ScopedMtaMembership() : joined (SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) {}
    ~ScopedMtaMembership() { if (joined) CoUninitialize(); }
    ScopedMtaMembership (const ScopedMtaMembership&) = delete;
    ScopedMtaMembership& operator= (const ScopedMtaMembership&) = delete;
    const bool joined;
};

/// The audio worker. Waits on the client's event and services one buffer per
/// wake. §11: no allocation, locking, logging or file I/O in here.
void runStreamThread (WasapiStream* stream)
{
    const ScopedMtaMembership com;

    // Exclusive-mode buffers are small; without Pro Audio scheduling the OS
    // will not wake this thread reliably enough to hold the §5.4 budget.
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);

    // Consecutive two-second waits that timed out. A device that has stopped
    // waking its event is not going to start again, and the loop used to spin
    // on that forever: no audio, no error, no end -- monitoring simply went
    // quiet, or a microphone's track was written as silence for the rest of the
    // take. Three in a row is six seconds, long enough that a machine briefly
    // under load is not accused of failing.
    constexpr int kTimeoutsBeforeGivingUp = 3;
    int consecutiveTimeouts = 0;
    int consecutiveRenderFailures = 0;
    int consecutiveCaptureFailures = 0;

    /// A run this long means the device is refusing everything, not glitching.
    constexpr int kCaptureFailuresBeforeGivingUp = 200;

    // Where the device's clock says the next capture packet should start.
    // Unknown until the first packet after Start, whose DISCONTINUITY flag is
    // routine (there is nothing before it) and so is not read as loss.
    bool haveCapturePosition = false;
    UINT64 expectedCapturePosition = 0;

    // Loss already counted by a failure run and not yet matched against the
    // position jump that follows it, so the same gap is not counted twice.
    uint64_t lossChargedAhead = 0;

    while (stream->running.load (std::memory_order_acquire))
    {
        if (WaitForSingleObject (stream->readyEvent, 2000) != WAIT_OBJECT_0)
        {
            if (++consecutiveTimeouts < kTimeoutsBeforeGivingUp)
                continue;

            if (stream->failures != nullptr)
                stream->failures->note (stream->deviceId,
                                        stream->isInput
                                            ? "stopped sending audio. Unplug it and plug it back in."
                                            : "stopped accepting audio, so you can't hear anything "
                                              "through it. Try selecting it again.");

            // Stopped rather than left spinning: a dead stream that keeps its
            // thread alive holds the device open against the reconnection the
            // message above is asking the user to make. Stopping the client is
            // the half that actually releases it; a second Stop() from
            // closeAllStreams later is harmless.
            stream->running.store (false, std::memory_order_release);

            if (stream->client)
                stream->client->Stop();

            break;
        }

        consecutiveTimeouts = 0;

        const int channels = stream->channels;

        for (int ch = 0; ch < channels; ++ch)
            stream->channelPointers[static_cast<size_t> (ch)] =
                stream->scratch.data() + static_cast<size_t> (ch) * stream->bufferFrames;

        if (stream->isInput)
        {
            UINT32 packetFrames = 0;

            // Taken rather than tested inline, because a FAILING one is the
            // one that matters. Once the device behind the stream goes away,
            // Windows answers AUDCLNT_E_DEVICE_INVALIDATED here, not in
            // GetBuffer -- and a driver that goes on signalling the ready
            // event then walks past both of the other exits: the wait never
            // times out, and the GetBuffer failure count below is never
            // reached because this loop is never entered. That left the
            // worker spinning for the rest of the session on a microphone
            // that had been unplugged: no audio, no error, no end, exactly
            // the shape of bug the wait above and the render side below each
            // carry a comment about having fixed.
            for (;;)
            {
                if (FAILED (stream->capture->GetNextPacketSize (&packetFrames)))
                {
                    if (consecutiveCaptureFailures == 0)
                    {
                        stream->framesDropped.fetch_add (stream->bufferFrames,
                                                         std::memory_order_relaxed);
                        lossChargedAhead += stream->bufferFrames;
                    }

                    if (++consecutiveCaptureFailures >= kCaptureFailuresBeforeGivingUp)
                    {
                        reportCaptureDeath (stream);
                        return;
                    }

                    break;
                }

                if (packetFrames == 0)
                    break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;

                if (FAILED (stream->capture->GetBuffer (&data, &frames, &flags, &devicePosition, nullptr)))
                {
                    // Counted once per run of failures, not once per attempt.
                    // GetNextPacketSize keeps reporting the same undelivered
                    // packet, so charging a buffer's worth on every retry made
                    // the reported loss 200x the real one -- and an inflated
                    // number is its own kind of wrong answer.
                    if (consecutiveCaptureFailures == 0)
                    {
                        stream->framesDropped.fetch_add (stream->bufferFrames,
                                                         std::memory_order_relaxed);
                        lossChargedAhead += stream->bufferFrames;
                    }

                    // A device that refuses every packet is not dropping audio,
                    // it is gone. Counting alone reported that as drift and
                    // never as a dead stream, so the mic went on being written
                    // as silence with only a rising number to show for it.
                    if (++consecutiveCaptureFailures >= kCaptureFailuresBeforeGivingUp)
                    {
                        reportCaptureDeath (stream);
                        return;
                    }

                    break;
                }

                consecutiveCaptureFailures = 0;

                // A driver overrun is not an error return. The packet after it
                // simply starts further along the device's clock, flagged
                // DATA_DISCONTINUITY. Reading neither, the loss went unreported
                // and skewed drift. The position says how much went; the flag
                // alone, from a driver whose position does not move, is
                // charged a buffer's worth. Only a flagged packet is read at
                // all: a driver whose positions drift from its frame counts
                // must not report loss on every packet.
                //
                // Told to the stream as well as counted: no inputs, that many
                // frames -- the produced-and-lost signal AlsaBackend sends
                // after an xrun and CaptureCoordinator forwards, so drift
                // counts the device's clock rather than reading a gap as the
                // clock running slow. A failure run above only counted.
                {
                    uint64_t lost = 0;

                    if (haveCapturePosition && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0)
                        lost = devicePosition > expectedCapturePosition
                                 ? devicePosition - expectedCapturePosition
                                 : static_cast<uint64_t> (stream->bufferFrames);

                    if (lost > lossChargedAhead)
                        stream->framesDropped.fetch_add (lost - lossChargedAhead, std::memory_order_relaxed);

                    if (lost > 0)
                        stream->callback (nullptr, 0, nullptr, 0,
                                          static_cast<int> (std::min<uint64_t> (lost, uint64_t { 1 } << 30)));

                    lossChargedAhead = 0;
                    haveCapturePosition = true;
                    expectedCapturePosition = devicePosition + frames;
                }

                if (frames > stream->bufferFrames)
                {
                    // The scratch was sized at open time and cannot grow on
                    // this thread (§11), so the tail of an over-size packet has
                    // nowhere to go. Counted rather than silently trimmed.
                    stream->framesDropped.fetch_add (frames - stream->bufferFrames,
                                                     std::memory_order_relaxed);
                    frames = stream->bufferFrames;
                }

                // AUDCLNT_BUFFERFLAGS_SILENT means the buffer contents are
                // undefined and must be treated as silence rather than read.
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    std::memset (stream->scratch.data(), 0, stream->scratch.size() * sizeof (float));
                }
                else
                {
                    const int bytes = stream->bytesPerSample;
                    const bool isFloat = stream->sampleIsFloat;

                    for (UINT32 f = 0; f < frames; ++f)
                        for (int ch = 0; ch < channels; ++ch)
                            stream->channelPointers[static_cast<size_t> (ch)][f] =
                                SampleFormat::read (data, static_cast<size_t> (f) * channels + ch, bytes, isFloat);
                }

                stream->callback (stream->channelPointers.data(), channels,
                                  nullptr, 0, static_cast<int> (frames));

                stream->capture->ReleaseBuffer (frames);
            }
        }
        else
        {
            // The render side's own way of dying. Each of these used to
            // `continue` unconditionally, so an output that had stopped
            // accepting audio spun here for the rest of the session: no sound,
            // no error, no end -- the user simply could not hear anything and
            // nothing anywhere said why. One failure is a glitch; a run of them
            // is a dead output, and the same three-strikes rule the wait above
            // uses applies.
            constexpr int kRenderFailuresBeforeGivingUp = 200;

            UINT32 padding = 0;
            if (FAILED (stream->client->GetCurrentPadding (&padding)))
            {
                if (++consecutiveRenderFailures < kRenderFailuresBeforeGivingUp)
                    continue;

                reportRenderDeath (stream);
                break;
            }

            const UINT32 frames = stream->bufferFrames - padding;
            if (frames == 0)
            {
                // Nothing to fill is normal -- the device simply has not
                // drained yet -- so this is not counted against the stream.
                continue;
            }

            BYTE* data = nullptr;
            if (FAILED (stream->render->GetBuffer (frames, &data)) || data == nullptr)
            {
                if (++consecutiveRenderFailures < kRenderFailuresBeforeGivingUp)
                    continue;

                reportRenderDeath (stream);
                break;
            }

            consecutiveRenderFailures = 0;

            std::memset (stream->scratch.data(), 0, stream->scratch.size() * sizeof (float));

            stream->callback (nullptr, 0,
                              stream->channelPointers.data(), channels,
                              static_cast<int> (frames));

            const int bytes = stream->bytesPerSample;
            const bool isFloat = stream->sampleIsFloat;

            for (UINT32 f = 0; f < frames; ++f)
                for (int ch = 0; ch < channels; ++ch)
                    SampleFormat::write (data, static_cast<size_t> (f) * channels + ch, bytes, isFloat,
                                 stream->channelPointers[static_cast<size_t> (ch)][f]);

            stream->render->ReleaseBuffer (frames, 0);
        }
    }

    if (task != nullptr)
        AvRevertMmThreadCharacteristics (task);
}

} // namespace

WasapiAsioBackend::WasapiAsioBackend()
{
    ownsComInitialisation = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
}

WasapiAsioBackend::~WasapiAsioBackend()
{
    unregisterNotificationClient();
    closeAllStreams();
    if (ownsComInitialisation)
        CoUninitialize();
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateWasapiDevices (bool wantInput) const
{
    std::vector<AudioDeviceDescriptor> result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), &enumerator)))
        return result;

    ComPtr<IMMDeviceCollection> collection;
    const EDataFlow flow = wantInput ? eCapture : eRender;
    if (FAILED (enumerator->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &collection)))
        return result;

    UINT count = 0;
    collection->GetCount (&count);

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (collection->Item (i, &device)))
            continue;

        if (wantInput && ! isDirectlyAttachedExternalInput (device.Get(), enumerator.Get()))
            continue;

        LPWSTR idWide = nullptr;
        device->GetId (&idWide);
        std::string id = idWide ? wideToUtf8 (idWide) : std::string();
        if (idWide) CoTaskMemFree (idWide);

        ComPtr<IPropertyStore> props;
        std::string friendlyName;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &props)))
        {
            PROPVARIANT nameProp;
            PropVariantInit (&nameProp);
            if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &nameProp)) && nameProp.pwszVal)
                friendlyName = wideToUtf8 (nameProp.pwszVal);
            PropVariantClear (&nameProp);
        }

        AudioDeviceDescriptor d;
        d.name = friendlyName;
        d.usbLocationId = id; // WASAPI endpoint IDs are stable per-port identifiers already
        d.isMicrophone = wantInput;
        d.hasPhysicalHeadphoneJack = ! wantInput; // refined by jack-presence property when available
        ComPtr<IAudioClient> client;
        if (SUCCEEDED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**> (client.GetAddressOf()))))
        {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED (client->GetMixFormat (&mix)) && mix != nullptr)
            {
                const int channels = mix->nChannels;
                const auto mixRate = mix->nSamplesPerSec;
                d.maxInputChannels = wantInput ? channels : 0;
                // WASAPI exposes the shared engine rate, not the hardware clock.
                d.currentSampleRate = mix->nSamplesPerSec;
                CoTaskMemFree (mix);
                std::vector<uint32_t> rates { 8000, 11025, 16000, 22050, 32000,
                                             44100, 48000, 88200, 96000, 176400, 192000 };
                if (d.currentSampleRate != 0)
                    rates.push_back (d.currentSampleRate);
                std::sort (rates.begin(), rates.end());
                rates.erase (std::unique (rates.begin(), rates.end()), rates.end());
                for (const auto rate : rates)
                {
                    WAVEFORMATEXTENSIBLE format {};
                    if (channels > 0 && findExclusiveFormat (client.Get(), rate, channels, format))
                        d.supportedSampleRates.push_back (rate);
                }
                // §2.3: asked at a rate the device actually accepts, so the
                // answer describes this device rather than a rate it refused.
                const double probeRate = ! d.supportedSampleRates.empty()
                                       ? static_cast<double> (d.supportedSampleRates.front())
                                       : static_cast<double> (mixRate);
                d.supportedBitDepths = querySupportedBitDepths (client.Get(), probeRate, channels);

                // A shared mixer can run at a rate the exclusive stream
                // cannot use. Do not let that rate override the probed list.
                if (std::find (d.supportedSampleRates.begin(), d.supportedSampleRates.end(),
                               d.currentSampleRate) == d.supportedSampleRates.end())
                    d.currentSampleRate = 0;
            }
        }
        result.push_back (d);
    }

    return result;
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateInputDevices()
{
    return enumerateWasapiDevices (true);
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateOutputDevices()
{
    return enumerateWasapiDevices (false);
}

void WasapiAsioBackend::setDeviceChangeCallback (DeviceChangeCallback callback)
{
    deviceChangeCallback = std::move (callback);

    unregisterNotificationClient();

    if (! deviceChangeCallback)
        return;

    hotplugProblem.clear();

    // Both failures below left the app deaf to the rig for the rest of the
    // session without a word -- the same silence the ALSA backend had: a
    // microphone plugged in is never noticed, and one pulled out MID-TAKE is
    // never reported, so a take that lost a channel looks like a clean one.
    static constexpr const char* kNoWatch =
        "Windows won't tell this app when microphones are plugged in or unplugged, so the list "
        "only updates when the app starts. Restart it after changing your rig.";

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS (&enumerator))))
    {
        hotplugProblem = kNoWatch;
        return;
    }

    auto* client = new DeviceNotificationClient (&deviceChangeCallback);

    if (SUCCEEDED (enumerator->RegisterEndpointNotificationCallback (client)))
    {
        notificationClient = client;   // the enumerator holds its own reference
    }
    else
    {
        client->Release();
        hotplugProblem = kNoWatch;
    }
}

void WasapiAsioBackend::unregisterNotificationClient()
{
    if (notificationClient == nullptr)
        return;

    auto* client = static_cast<DeviceNotificationClient*> (notificationClient);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS (&enumerator))))
        enumerator->UnregisterEndpointNotificationCallback (client);

    // Must outlive the registration: the OS can be mid-notification on another
    // thread when this runs, and releasing our reference is the only safe way
    // to hand ownership back to the reference count.
    client->Release();
    notificationClient = nullptr;
}

ExclusiveModeCapability WasapiAsioBackend::checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                         double sampleRate, int bufferSizeSamples)
{
    ExclusiveModeCapability cap;

    // WASAPI: only AUDCLNT_SHAREMODE_EXCLUSIVE qualifies. IAudioClient::
    // IsFormatSupported against AUDCLNT_SHAREMODE_EXCLUSIVE would be the
    // real check here; shared mode is never accepted as a substitute (§5.4).
    ComPtr<IMMDevice> device;

    if (! resolveDevice (outputDeviceId, device) || device == nullptr)
    {
        cap.unavailableReason = "That sound output isn't connected any more.";
        return cap;
    }

    ComPtr<IAudioClient> client;

    if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**> (client.GetAddressOf()))))
    {
        cap.unavailableReason = "Windows wouldn't give this app direct access to your headphones.";
        return cap;
    }

    // Every layout the OPEN would try, not just the first one.
    //
    // This asked IsFormatSupported for float32 alone, while buildExclusiveStream
    // goes on to try 32/32, 32/24, 24/24 and 16/16 as well -- and the comment
    // above findExclusiveFormat says, in as many words, "use the same layouts
    // for capability discovery and stream opening". It was the one caller that
    // did not.
    //
    // What that costs is a real and common device: plenty of USB interfaces and
    // DACs accept exclusive output only in their native INTEGER format and
    // refuse float32 outright. Those were declared incapable and the user was
    // told to "turn on exclusive mode for this device" in Windows sound
    // settings -- advice about a setting that was already correct, for hardware
    // the app could have opened on the very next layout it never asked about.
    // Monitoring simply went off, blamed on Windows.
    // And every channel count it would try, for the same reason: the
    // probe asked about stereo alone.
    WAVEFORMATEXTENSIBLE format {};

    if (! findExclusiveFormatForAnyChannelCount (client.Get(), sampleRate, false, format))
    {
        // §5.4: shared mode is never the fallback. Name the cause instead.
        cap.unavailableReason = "Your headphones won't accept direct low-latency audio. In Windows sound settings, "
                                "turn on exclusive mode for this device, or use a different output.";
        return cap;
    }

    cap.exclusiveModeAvailable = true;
    cap.measuredOrEstimatedLatencyMs = (bufferSizeSamples / sampleRate) * 1000.0 * 2.0;
    return cap;
}

std::unique_ptr<WasapiStream> WasapiAsioBackend::buildExclusiveStream (const std::string& deviceId,
                                                                      double sampleRate,
                                                                      int bufferSizeSamples,
                                                                      bool isInput,
                                                                      AudioCallback callback,
                                                                      std::string& openError,
                                                                      OpenRefusal& refusal)
{
    refusal = OpenRefusal::unknown;

    if (! callback)
        return nullptr;

    openError.clear();

    ComPtr<IMMDevice> device;
    if (! resolveDevice (deviceId, device) || device == nullptr)
    {
        // §5.4 asks for the cause to be named. Only the format-refused case
        // below ever filled this in, so every other refusal in here reached the
        // user as a generic "couldn't open" with nothing to act on.
        openError = isInput
            ? "This microphone is no longer connected. Unplug it and plug it back in, then try "
              "again."
            : "This sound output isn't there any more. Pick a different one in Advanced.";
        refusal = OpenRefusal::gone;
        return nullptr;
    }

    // Re-resolve the physical device evidence when an input is actually
    // opened. Enumeration is deliberately fail-closed, but it is not an
    // authorization token: endpoint state can change between the list refresh
    // and this call, and this public method must also be safe when called by a
    // future path that did not originate in enumerateInputDevices().
    if (isInput)
    {
        ComPtr<IMMDeviceEnumerator> eligibilityEnumerator;
        const bool hasEnumerator = SUCCEEDED (CoCreateInstance (
            __uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof (IMMDeviceEnumerator),
            reinterpret_cast<void**> (eligibilityEnumerator.GetAddressOf())));

        if (! hasEnumerator
            || ! isDirectlyAttachedExternalInput (device.Get(), eligibilityEnumerator.Get()))
        {
            openError = "SobStage only records from directly connected external audio hardware.";
            refusal = OpenRefusal::notExternal;
            return nullptr;
        }
    }

    auto stream = std::make_unique<WasapiStream>();
    stream->callback = std::move (callback);
    stream->isInput = isInput;

    // The output stream reports itself with an empty id, which is what the
    // owner uses to tell "your headphones stopped" from "this microphone
    // stopped" without having to know the device.
    stream->deviceId = isInput ? deviceId : std::string();
    stream->failures = &streamFailures;

    if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**> (stream->client.GetAddressOf()))))
    {
        openError = isInput
            ? "Windows wouldn't let this app attach to this microphone. Check Settings > Privacy > "
              "Microphone, then try again."
            : "Windows wouldn't let this app attach to this output. Close anything else using it, "
              "then try again.";
        refusal = OpenRefusal::permission;
        return nullptr;
    }

    // §5.4: exclusive mode or nothing -- falling back to shared would deliver
    // 40-100 ms and the product fails at that latency. But exclusive mode also
    // performs no conversion, so the format has to be one the hardware speaks
    // natively. Offering only float32 (as this once did) means most USB
    // microphones, which are 16- or 24-bit PCM devices, simply refuse to open.
    // Try the engine's own format first, then descend through the fixed-point
    // layouts, and try the device's native channel count before giving up.
    WAVEFORMATEXTENSIBLE format {};
    const bool formatFound =
        findExclusiveFormatForAnyChannelCount (stream->client.Get(), sampleRate, isInput, format);

    if (! formatFound)
    {
        openError = "This device won't accept a low-latency connection at " + std::to_string ((int) sampleRate)
                      + " Hz. Try a different sample rate, or a different device, in Advanced.";
        refusal = OpenRefusal::settingsRefused;
        return nullptr;
    }

    stream->bytesPerSample = format.Format.wBitsPerSample / 8;
    stream->sampleIsFloat = (format.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    // Exclusive mode wants the buffer expressed as a duration in 100 ns units.
    REFERENCE_TIME duration =
        static_cast<REFERENCE_TIME> ((10000.0 * 1000.0 / sampleRate) * bufferSizeSamples + 0.5);

    // Below the device's minimum period Initialize fails with
    // AUDCLNT_E_INVALID_DEVICE_PERIOD, which used to be reported as another app
    // holding the device. Ask for the minimum instead; the granted size is
    // read back from GetBufferSize, so the latency quoted stays true.
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    if (SUCCEEDED (stream->client->GetDevicePeriod (&defaultPeriod, &minimumPeriod))
        && minimumPeriod > duration)
        duration = minimumPeriod;

    HRESULT hr = stream->client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             duration, duration,
                                             reinterpret_cast<const WAVEFORMATEX*> (&format),
                                             nullptr);

    // The device can reject the period and name the one it wants; retry once at
    // that size rather than giving up on exclusive mode.
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
    {
        UINT32 alignedFrames = 0;
        if (SUCCEEDED (stream->client->GetBufferSize (&alignedFrames)) && alignedFrames > 0)
        {
            const REFERENCE_TIME aligned =
                static_cast<REFERENCE_TIME> ((10000.0 * 1000.0 / sampleRate) * alignedFrames + 0.5);

            stream->client.Reset();

            if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**> (stream->client.GetAddressOf()))))
            {
                openError = isInput
                    ? "This microphone stopped responding while it was being set up. Check its cable, "
                      "then choose it again in Advanced."
                    : "These headphones stopped responding while they were being set up. Check the "
                      "cable, then choose them again in Advanced.";
                return nullptr;
            }

            hr = stream->client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             aligned, aligned,
                                             reinterpret_cast<const WAVEFORMATEX*> (&format),
                                             nullptr);
        }
    }

    if (hr == AUDCLNT_E_INVALID_DEVICE_PERIOD)
    {
        openError = std::string (isInput ? "This microphone" : "These headphones")
                  + " can't run with a buffer of " + std::to_string (bufferSizeSamples)
                  + " samples. Pick a larger buffer size in Advanced.";
        refusal = OpenRefusal::settingsRefused;
        return nullptr;
    }

    if (FAILED (hr) && hr != AUDCLNT_E_DEVICE_IN_USE && hr != AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
    {
        char code[16];
        std::snprintf (code, sizeof (code), "0x%08X", static_cast<unsigned> (hr));
        openError = std::string (isInput ? "This microphone" : "These headphones")
                  + " refused the connection (Windows error " + code
                  + "). Unplug it, plug it back in, and choose it again in Advanced.";
        return nullptr;
    }

    if (FAILED (hr))
    {
        // The commonest cause by far, and the one with a fix the user can carry
        // out: exclusive mode is refused because something else already holds
        // the device, or because it is switched off for this endpoint.
        openError = isInput
            ? "This microphone wouldn't give this app exclusive use, which recording needs. Close "
              "anything else recording or streaming from it, and check \"Allow applications to take "
              "exclusive control\" is ticked for it in Sound settings."
            : "These headphones wouldn't give this app exclusive use, which live monitoring needs. "
              "Close anything else playing sound, or pick a different output in Advanced.";
        refusal = OpenRefusal::inUse;
        return nullptr;
    }

    stream->readyEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    if (stream->readyEvent == nullptr || FAILED (stream->client->SetEventHandle (stream->readyEvent)))
    {
        openError = "Windows refused to set up the audio connection for this device. "
                        "Unplug it and plug it back in, or restart the app.";
        return nullptr;
    }

    if (FAILED (stream->client->GetBufferSize (&stream->bufferFrames)))
    {
        openError = "Windows refused to set up the audio connection for this device. "
                        "Unplug it and plug it back in, or restart the app.";
        return nullptr;
    }

    if (isInput)
    {
        if (FAILED (stream->client->GetService (__uuidof (IAudioCaptureClient),
                                                reinterpret_cast<void**> (stream->capture.GetAddressOf()))))
        {
            openError = "Windows refused to hand over this microphone's audio. Unplug it and "
                            "plug it back in, or restart the app.";
            return nullptr;
        }
    }
    else
    {
        if (FAILED (stream->client->GetService (__uuidof (IAudioRenderClient),
                                                reinterpret_cast<void**> (stream->render.GetAddressOf()))))
        {
            openError = "Windows refused to hand over this output's audio. Pick a different "
                            "output in Advanced.";
            return nullptr;
        }
    }

    stream->channels = format.Format.nChannels;

    // Deinterleave scratch, allocated here so the audio thread never does (§11).
    stream->scratch.assign (static_cast<size_t> (stream->channels) * stream->bufferFrames, 0.0f);
    stream->channelPointers.resize (stream->channels);

    if (FAILED (stream->client->Start()))
    {
        openError = isInput
            ? "This microphone accepted the connection but wouldn't start. Unplug it and plug it "
              "back in, then try again."
            : "This output accepted the connection but wouldn't start. Pick a different one in "
              "Advanced.";
        return nullptr;
    }

    return stream;
}


bool WasapiAsioBackend::openWasapiExclusiveStream (const std::string& deviceId, double sampleRate,
                                                  int bufferSizeSamples, bool isInput,
                                                  AudioCallback callback)
{
    lastOpenError.clear();
    lastOpenRefusal = OpenRefusal::unknown;

    // Every COM call below -- Activate, IsFormatSupported, Initialize -- runs
    // on an owned worker behind a deadline, for the reason CoreAudioBackend
    // gives for doing the same to the HAL: a class-compliant USB driver can
    // block the caller inside one of them for minutes, and the window is only
    // created after audio initialisation, so an unbounded call makes SobStage
    // appear never to launch. Windows had none of this and a driver stalling
    // three seconds inside Initialize blocked the caller for three seconds --
    // measured, not assumed, once the simulation could express a slow driver.
    //
    // Simpler than the macOS equivalent because COM is reference counted: a
    // worker that finishes after the deadline drops the last reference to
    // whatever it built and Windows releases it. There is no clientData for a
    // driver to retain and nothing to quarantine.
    struct Attempt
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool finished = false;
        bool abandoned = false;
        std::string error;
        OpenRefusal refusal = OpenRefusal::unknown;
        std::unique_ptr<WasapiStream> stream;
    };

    auto attempt = std::make_shared<Attempt>();
    auto pending = pendingOpens;

    {
        const std::lock_guard<std::mutex> guard (pending->mutex);
        ++pending->running;
    }

    std::thread worker;
    try
    {
        worker = std::thread ([this, attempt, pending, deviceId, sampleRate, bufferSizeSamples,
                               isInput, callback = std::move (callback)] () mutable
        {
            const ScopedMtaMembership com;
            std::string error;
            OpenRefusal refusal = OpenRefusal::unknown;
            auto built = buildExclusiveStream (deviceId, sampleRate, bufferSizeSamples, isInput,
                                               std::move (callback), error, refusal);

            // Announced before the result is published and on every exit, so a
            // caller that stopped waiting still learns when the driver let go.
            struct Leaving
            {
                std::shared_ptr<PendingOpens> pending;
                ~Leaving()
                {
                    {
                        const std::lock_guard<std::mutex> guard (pending->mutex);
                        --pending->running;
                    }
                    pending->changed.notify_all();
                }
            } leaving { pending };

            const std::lock_guard<std::mutex> guard (attempt->mutex);

            // The caller gave up. Let the stream go here rather than handing
            // back something nobody is waiting for; its COM pointers release
            // with it.
            if (attempt->abandoned)
                return;

            attempt->stream = std::move (built);
            attempt->error = std::move (error);
            attempt->refusal = refusal;
            attempt->finished = true;
            attempt->changed.notify_one();
        });
    }
    catch (...)
    {
        {
            const std::lock_guard<std::mutex> guard (pending->mutex);
            --pending->running;
        }
        pending->changed.notify_all();

        lastOpenError = "Windows couldn't create the worker needed to open this audio device "
                        "safely. Restart SobStage, then try again.";
        lastOpenRefusal = OpenRefusal::appBusy;
        return false;
    }

    std::unique_lock<std::mutex> lock (attempt->mutex);

    if (! attempt->changed.wait_for (lock, kOpenDeadline,
                                     [&attempt] { return attempt->finished; }))
    {
        attempt->abandoned = true;
        lock.unlock();
        worker.detach();

        lastOpenError = isInput
            ? "Windows took too long to connect this microphone, so SobStage stopped waiting. "
              "Unplug it and plug it back in, and close any other app using audio before trying "
              "again."
            : "Windows took too long to connect this sound output, so SobStage stopped waiting. "
              "Choose another output or reconnect it.";
        lastOpenRefusal = OpenRefusal::timedOut;
        return false;
    }

    auto stream = std::move (attempt->stream);
    lastOpenError = std::move (attempt->error);
    lastOpenRefusal = attempt->refusal;
    lock.unlock();
    worker.join();

    if (stream == nullptr)
        return false;

    stream->running = true;
    auto* raw = stream.get();
    stream->worker = std::thread ([raw] { runStreamThread (raw); });

    openStreams.push_back (std::move (stream));
    return true;
}

bool WasapiAsioBackend::waitForPendingOpensForTesting (int timeoutMilliseconds)
{
    const auto pending = pendingOpens;
    std::unique_lock<std::mutex> lock (pending->mutex);
    return pending->changed.wait_for (lock, std::chrono::milliseconds (timeoutMilliseconds),
                                      [&pending] { return pending->running == 0; });
}

int WasapiAsioBackend::getGrantedOutputBufferFrames() const
{
    // GetBufferSize already told us at open time what the endpoint granted;
    // exclusive mode routinely aligns a requested size up to the device's own
    // period, so this is often not what was asked for. It was stored and never
    // read, while §5.4's latency figure went on describing the request.
    for (const auto& stream : openStreams)
        if (stream != nullptr && ! stream->isInput && stream->bufferFrames > 0)
            return static_cast<int> (stream->bufferFrames);

    return 0;
}

uint64_t WasapiAsioBackend::getFramesDroppedByBackend() const
{
    uint64_t total = 0;

    for (const auto& stream : openStreams)
        if (stream != nullptr)
            total += stream->framesDropped.load (std::memory_order_relaxed);

    return total;
}

bool WasapiAsioBackend::openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                                   int bufferSizeSamples, AudioCallback callback)
{
    return openWasapiExclusiveStream (outputDeviceId, sampleRate, bufferSizeSamples, false, std::move (callback));
}

bool WasapiAsioBackend::openInputStream (const std::string& inputDeviceId, double sampleRate,
                                         int bufferSizeSamples, AudioCallback callback)
{
    return openWasapiExclusiveStream (inputDeviceId, sampleRate, bufferSizeSamples, true, std::move (callback));
}

void WasapiAsioBackend::closeAllStreams()
{
    for (auto& stream : openStreams)
    {
        stream->running.store (false, std::memory_order_release);

        // Wake the worker immediately rather than letting it sit out its
        // timeout, so stopping is not perceptibly slow.
        if (stream->readyEvent != nullptr)
            SetEvent (stream->readyEvent);

        if (stream->worker.joinable())
            stream->worker.join();

        if (stream->client != nullptr)
            stream->client->Stop();
    }

    openStreams.clear();
}

} // namespace mma

#endif // JUCE_WINDOWS
