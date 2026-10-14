#pragma once

// The virtual audio endpoint layer behind the WASAPI shim.
//
// A harness describes endpoints the way real hardware behaves -- a microphone
// that accepts only 24-bit PCM in exclusive mode, an interface that rejects the
// requested period and names its own, a card another process already holds --
// then drives the backend's own worker thread and inspects the audio that comes
// out. The backend source is compiled unmodified.

#include <string>
#include <vector>

namespace fakewasapi {

/// One node in the physical device's PnP ancestry, leaf first. Windows normally
/// marks the top-most removable node rather than every child below it.
struct DeviceNodeSpec
{
    std::string instanceId;
    bool removable = false;
    bool removalExpected = false;
    bool propertiesReadable = true;
};

/// A format the endpoint will accept in exclusive mode. Exclusive mode performs
/// no conversion, so this list is the whole of what the device can do.
struct Format
{
    int channels = 2;
    int containerBits = 32;   ///< 16, 24 or 32
    bool isFloat = false;
    double sampleRate = 48000.0;

    static Format floatFormat (int channels, double rate) { return { channels, 32, true, rate }; }
    static Format pcm (int channels, int bits, double rate) { return { channels, bits, false, rate }; }
};

struct EndpointSpec
{
    std::string id;            ///< the endpoint id string the backend stores (§2.4)
    std::string friendlyName;
    bool isCapture = true;

    /// Plug and Play instance id of the physical audio filter behind this
    /// endpoint. Eligibility additionally requires positive removable evidence
    /// on this node or an ancestor.
    std::string physicalInstanceId = "USB\\SIMULATED_AUDIO";

    /// What the CONNECTOR reports as the device it is connected to, and what
    /// the ENDPOINT's own property store reports as its instance id. On
    /// Windows these are three different strings -- endpoint id, topology
    /// device id, physical instance id -- and the walk from one to the next is
    /// the whole of the §2.4 identity path.
    ///
    /// They used to be the same string here, which made two links of that walk
    /// identity functions: a backend that read PKEY_Device_InstanceId straight
    /// off the endpoint, skipping GetConnector and GetDeviceIdConnectedTo
    /// entirely, was indistinguishable from one that followed the topology. On
    /// real Windows that shortcut yields SWD\\MMDEVAPI\\..., which is not an
    /// eligible transport, so every microphone would be filtered out as
    /// not-external. Left blank, addEndpoint derives realistic distinct values.
    std::string connectedDeviceId;
    std::string endpointInstanceId;

    /// Leaf-to-root PnP ancestry. When empty, addEndpoint supplies one removable
    /// node matching physicalInstanceId so older audio-format scenarios keep
    /// modelling an ordinary plug-in USB interface.
    std::vector<DeviceNodeSpec> deviceNodeChain;

    /// Failure injection for the physical-identity path. Every failure must
    /// hide an input rather than falling back to endpoint names or ids.
    /// A driver that blocks the caller inside IAudioClient::Initialize. This is
    /// the Windows form of the hazard CoreAudioBackend bounds every HAL call
    /// against; nothing here could express it before.
    int initializeDelayMilliseconds = 0;

    /// The endpoint's Windows device state: DEVICE_STATE_ACTIVE,
    /// DEVICE_STATE_DISABLED, DEVICE_STATE_NOTPRESENT or
    /// DEVICE_STATE_UNPLUGGED.
    ///
    /// A disabled or unplugged endpoint is still in the registry and still
    /// enumerable — it just must not be offered as a working microphone. The
    /// fake reported ACTIVE for everything and discarded the state mask that
    /// EnumAudioEndpoints is given, so a backend asking for every state got the
    /// same answer as one asking for active devices only, and unplugged
    /// hardware appearing in the picker was untestable.
    ///
    /// Declared as unsigned long rather than DWORD so this header stays free of
    /// the Windows headers, as the rest of it is.
    unsigned long deviceState = 1; // DEVICE_STATE_ACTIVE

    bool topologyAvailable = true;
    bool connectorAvailable = true;
    bool connectedDeviceIdAvailable = true;
    bool propertyStoreAvailable = true;
    bool instanceIdPropertyAvailable = true;
    bool devNodeLookupAvailable = true;

    /// Everything this endpoint accepts exclusively. Empty means it accepts
    /// nothing, which is what a device already held by another process looks
    /// like to IsFormatSupported.
    std::vector<Format> exclusiveFormats;

    /// What GetMixFormat reports. The backend uses its channel count as a last
    /// negotiation candidate.
    Format mixFormat = Format::floatFormat (2, 48000.0);

    /// false makes Activate fail, which models a device Windows will not hand
    /// over at all.
    bool allowActivate = true;

    /// Non-zero: Activate for IAudioClient fails with this HRESULT -- for
    /// instance AUDCLNT_E_DEVICE_INVALIDATED, a microphone that dropped off
    /// the bus between being listed and being opened, or E_ACCESSDENIED.
    long activateError = 0;

    /// Non-zero: every IsFormatSupported answers this HRESULT -- for instance
    /// AUDCLNT_E_DEVICE_INVALIDATED, a microphone that dropped off the bus
    /// after Activate, while its formats were being asked about.
    long formatQuestionError = 0;

    /// With formatQuestionError: GetState reports the endpoint gone too once
    /// a format has been asked about, as Windows does after a removal.
    bool goneAfterFormatQuestion = false;

    /// When set, the first Initialize returns AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
    /// and reports this size, which the backend must retry at.
    int alignedFrames = 0;

    /// With alignedFrames: the re-Activate the backend must do before its
    /// retry fails, as a device yanked mid-open would.
    bool failActivateAfterAlignment = false;

    /// The smallest period the device runs at, as GetDevicePeriod reports it.
    /// Initialize rejects anything shorter with AUDCLNT_E_INVALID_DEVICE_PERIOD.
    /// 0 reports no minimum.
    int minimumPeriodFrames = 0;

    int bufferFrames = 256;
};

void reset();

/// Puts the calling thread in a single-threaded apartment, which is what
/// JUCE's OleInitialize does to the Windows message thread. COM is modelled
/// per thread: CoInitializeEx(MULTITHREADED) on an STA thread returns
/// RPC_E_CHANGED_MODE, and CoCreateInstance from a thread in no apartment
/// fails with CO_E_NOTINITIALIZED unless some thread holds the MTA open.
void enterSingleThreadedApartment();
void leaveApartment();

/// false makes RegisterEndpointNotificationCallback fail, which models a
/// Windows session that will not tell the app about devices coming and going.
void setNotificationRegistrationAllowed (bool allowed);

void addEndpoint (const EndpointSpec& spec);

/// Removes an endpoint and fires the registered IMMNotificationClient, which is
/// what an unplug looks like to the backend.
void removeEndpoint (const std::string& id);

/// Makes every IAudioCaptureClient call on this endpoint return
/// AUDCLNT_E_DEVICE_INVALIDATED, which is what Windows returns once the device
/// behind an open stream goes away, while leaving the endpoint in the list.
void invalidateEndpoint (const std::string& id);

/// Signals the endpoint's ready event without delivering a packet. An
/// invalidated endpoint whose event keeps firing is the case that neither the
/// stall timeout nor the GetBuffer failure count can see.
void pulseReadyEvent (const std::string& id);

/// Fires OnDeviceAdded without changing the device list, for testing that the
/// backend is driven by the OS rather than by a timer.
void notifyDeviceAdded (const std::string& id);

/// True once the backend has called IAudioClient::Start on this endpoint.
bool isRunning (const std::string& id);

/// The format the backend actually negotiated. Container bits of 0 means no
/// stream was opened.
Format negotiatedFormat (const std::string& id);

/// Whether the endpoint was opened in exclusive mode. §5.4 makes anything else
/// a defect, so this is asserted rather than assumed.
bool openedExclusive (const std::string& id);

/// Delivers one capture packet carrying `channels` (per-channel, equal length),
/// encoded in the endpoint's negotiated format, and blocks until the backend's
/// worker thread has consumed it. Returns false on timeout.
bool pushCapture (const std::string& id, const std::vector<std::vector<float>>& channels);

/// Delivers a packet the way Windows does after the driver's ring overran:
/// the device position jumps `lostFrames` past the previous packet's end and
/// AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY is set. The lost frames never reach
/// the backend; noticing them is its job. `lostFrames` of 0 is the flag alone,
/// which is what the first packet after Start often carries.
bool pushCaptureAfterOverrun (const std::string& id, int lostFrames,
                              const std::vector<std::vector<float>>& channels);

/// Delivers a packet the device marks AUDCLNT_BUFFERFLAGS_SILENT, whose
/// contents are undefined and must be treated as silence rather than read.
bool pushSilentCapture (const std::string& id, int frames);

/// Runs one render period and returns what the backend wrote, decoded from the
/// negotiated format back into per-channel floats. Returns false on timeout.
bool pullRender (const std::string& id, std::vector<std::vector<float>>& out);

/// Makes RegOpenKeyExA report an ASIO driver as present or absent, which is
/// what decides the backend's ASIO-versus-WASAPI preference.
void setAsioDriverInstalled (bool installed);

} // namespace fakewasapi
