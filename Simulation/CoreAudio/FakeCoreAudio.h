#pragma once

// The virtual CoreAudio HAL behind the shim headers.
//
// A harness describes devices the way real hardware behaves -- a stereo
// microphone that hands over one interleaved buffer, an interface that
// advertises a continuous 44.1-96 kHz range, a card whose hog mode another
// process already owns -- then drives the backend's IOProc and inspects what
// the backend did with it. The backend source is compiled unmodified.

#include <CoreAudio/CoreAudio.h>

#include <string>
#include <utility>
#include <vector>

namespace fakeca {

/// How the HAL presents a device's channels to the IOProc. Both shapes occur on
/// real hardware and the backend has to handle both; this is the single most
/// important axis the simulation varies, because getting it wrong is silent.
enum class BufferShape
{
    oneChannelPerBuffer, ///< N buffers of 1 channel each.
    interleaved          ///< 1 buffer carrying N interleaved channels.
};

struct DeviceSpec
{
    std::string name;
    std::string uid;

    int inputChannels = 0;
    int outputChannels = 0;
    BufferShape shape = BufferShape::oneChannelPerBuffer;

    /// How CoreAudio says this device is connected. Most scenarios are USB;
    /// enumeration tests override it to model built-in, Continuity, wireless,
    /// aggregate, and other non-hardware inputs.
    UInt32 transportType = kAudioDeviceTransportTypeUSB;

    /// kAudioDevicePropertyDataSource on the output scope, or 0 when the
    /// device does not have the property (most USB devices). An Intel Mac's
    /// "Built-in Output" reports 'ispk' for its speakers and 'hdpn' while
    /// headphones are in the jack.
    UInt32 outputDataSource = 0;

    /// kAudioDevicePropertyJackIsConnected on the output scope: 1 or 0, or -1
    /// when the device does not have the property (most devices).
    int jackConnected = -1;

    /// Rates the device reports. A discrete rate is a range whose ends are
    /// equal; a continuous range has them different, which is what a device
    /// with a sample-rate converter advertises.
    std::vector<std::pair<double, double>> rateRanges { { 48000.0, 48000.0 } };

    double currentRate = 48000.0;

    /// false models a device whose rate is fixed, or one another process holds:
    /// the property write fails. The backend must still succeed when the device
    /// already runs at the requested rate.
    bool allowRateChange = true;

    /// Number of nominal-rate reads that keep returning the old value after a
    /// successful property write. CoreAudio applies this property
    /// asynchronously on real hardware. Zero is immediate; a deliberately huge
    /// value models a driver that acknowledges the write but never settles
    /// within the backend's bounded confirmation window.
    int rateChangeDelayReads = 0;

    /// Models a cable pulled while the backend waits for a rate change to
    /// settle: the rate write is accepted, then the device stops being alive
    /// and its nominal rate can no longer be read.
    bool unpluggedDuringRateChange = false;

    /// §2.3: the depths this device's stream will actually deliver, as
    /// CoreAudio would report them through the stream's available physical
    /// formats. A Blue Yeti (§14.1) is { 16 }; an interface is typically
    /// { 16, 24 }; empty models a device with no streams to ask.
    ///
    /// Default matches the ordinary case so every existing scenario keeps
    /// reporting what it always did.
    std::vector<int> bitDepths { 16, 24, 32 };

    /// false models a device that will not grant exclusive use -- Bluetooth
    /// output, or a card another process has hogged.
    bool allowHogMode = true;

    int bufferFrameSize = 256;
    bool allowBufferSizeChange = true;

    /// Input-scope kAudioDevicePropertyLatency and kAudioDevicePropertySafetyOffset,
    /// in frames: how long before the IOProc a captured sample was at the mic.
    /// Output scope reports neither, as before.
    int inputLatencyFrames = 0;
    int inputSafetyOffsetFrames = 0;

    /// kAudioStreamPropertyLatency of the device's one stream. This fake
    /// presents a single stream to both scopes, so a duplex device reports it
    /// on its output side too; zero leaves every existing figure alone.
    int streamLatencyFrames = 0;

    /// kAudioDevicePropertyDeviceIsAlive. A false value means the object still
    /// exists long enough to notify clients but is no longer usable.
    bool isAlive = true;

    /// Artificial HAL delay used to prove a driver stuck in AudioDeviceStart,
    /// on either an input or output, cannot hold launch indefinitely.
    int startDelayMilliseconds = 0;

    /// Instead of a fixed delay, AudioDeviceStart waits until releaseStart()
    /// names this device (or reset() runs; at most ten seconds), so a test
    /// decides when a slow start finishes instead of racing a timer against
    /// a busy runner's scheduling.
    bool startWaitsForRelease = false;

    /// Other calls made before and after Start can wedge in third-party HAL
    /// plug-ins too. These delays exercise the same deadline at UID lookup,
    /// IOProc creation, Stop, and Destroy.
    int uidReadDelayMilliseconds = 0;
    int createDelayMilliseconds = 0;
    int stopDelayMilliseconds = 0;
    int destroyDelayMilliseconds = 0;

    /// Deliver one input block before the delayed Start call returns. Some real
    /// HALs begin IO before unblocking AudioDeviceStart; the backend must still
    /// close callback admission when its caller times out.
    bool callbackBeforeStartReturns = false;

    /// false models a broken driver retaining property-listener clientData even
    /// after the application asks to remove it.
    bool allowPropertyListenerRemoval = true;

    /// For an aggregate device: the UIDs of its sub-devices. Those present are
    /// what kAudioAggregateDevicePropertyActiveSubDeviceList reports.
    std::vector<std::string> subDeviceUids;

    /// Reads of that list that still come back empty, as a just-created
    /// aggregate's does while the HAL builds it. Each size query is one read.
    int subDeviceActivationDelayReads = 0;
};

/// Clears every device, listener and IOProc. Call between scenarios.
///
/// The clocks are not reset: like the Mac's, they only ever move forward.
void reset();

/// Awake time passing at once, without the harness waiting for it: the host
/// clock (AudioGetCurrentHostTime) and the wall clock (CFAbsoluteTimeGetCurrent)
/// both move on by `seconds`. Nothing is delivered meanwhile -- an IOProc runs
/// only when pumped -- so this is also every device going quiet for that long.
void passTime (double seconds);

/// The Mac sleeping for `seconds`: the wall clock moves on and the host clock
/// does not, which is what sleep does to mach_absolute_time and the time of
/// day. No IOProc runs across it.
void sleepSystem (double seconds);

/// What the power manager says about the Mac's audio
/// (IOPMConnectionGetSystemCapabilities()): on, as when fully awake (the reset
/// state), or off, as in a dark wake -- awake with the lid shut, display and
/// audio off -- or on the way down into sleep. Only what is reported: no
/// IOProc is stopped or started by it.
void setSystemAudioOn (bool on);

/// How many times the backend has asked the power manager since reset().
int systemCapabilityQueryCount();

/// false makes AudioObjectAddPropertyListener fail, which models a Mac that
/// will not tell the app about devices coming and going.
void setPropertyListenersAllowed (bool allowed);

/// false models a HAL that accepts the system hot-plug listener but refuses to
/// remove it later, retaining its raw clientData.
void setSystemPropertyListenerRemovalAllowed (bool allowed);

/// Fires the current system device-list listeners without mutating a device.
/// Useful for deterministic callback/destructor races.
void fireDeviceListChange();

int systemPropertyListenerCount();

AudioObjectID addDevice (const DeviceSpec& spec);

/// kAudioHardwarePropertyDefaultOutputDevice: the output macOS plays through.
/// kAudioObjectUnknown (the reset state) models a Mac that reports none.
void setDefaultOutputDevice (AudioObjectID device);

/// Removes a device and fires the system device-list listener, which is what an
/// unplug looks like to the backend (§2: the OS tells us, we never poll).
void removeDevice (AudioObjectID device);

/// Changes a live device's nominal rate as another app or Audio MIDI Setup
/// would, then fires the per-device CoreAudio property listener.
bool setNominalRateExternally (AudioObjectID device, double sampleRate);

/// Changes kAudioDevicePropertyDeviceIsAlive and notifies its listeners.
bool setDeviceAlive (AudioObjectID device, bool alive);

/// Moves a live output to another data source, as an Intel Mac's "Built-in
/// Output" does when headphones go into or come out of its jack ('hdpn' /
/// 'ispk'). The device list does NOT change; only the output-scope
/// kAudioDevicePropertyDataSource listeners on that device are told.
bool setOutputDataSource (AudioObjectID device, UInt32 dataSource);

/// Changes kAudioDevicePropertyJackIsConnected (output scope) and notifies its
/// listeners. Also without any device-list change.
bool setJackConnected (AudioObjectID device, bool connected);

/// The HAL finishing an aggregate it was slow to build: its sub-devices that
/// are present become active at once, and the only word of it is on the
/// aggregate itself -- its active sub-device list (global scope) and its
/// output stream configuration. The device list does not change, so a client
/// listening only to that never hears it.
bool activateSubDevices (AudioObjectID aggregate);

/// Models `sudo killall coreaudiod`: every aggregate created through
/// AudioHardwareCreateAggregateDevice is gone, and the ONLY notification is
/// kAudioHardwarePropertyServiceRestarted on the system object -- the worst
/// case, where no device-list notification arrives at all. Listeners on
/// device objects are dropped (they belonged to the old service); devices keep
/// their AudioObjectIDs, so a client cannot tell from the IDs alone.
void restartService();

/// Listeners on the system object for kAudioHardwarePropertyServiceRestarted.
int serviceRestartListenerCount();

/// How many aggregates AudioHardwareCreateAggregateDevice has made since reset().
int aggregatesCreated();

/// Fires one kAudioDeviceProcessorOverload event (one missed IO deadline).
bool fireProcessorOverload (AudioObjectID device);

/// Number of per-object property listeners still registered. Used to prove
/// stream teardown leaves no callback pointing at freed client data.
int propertyListenerCount (AudioObjectID device);

/// True once the backend has called AudioDeviceStart on this device.
bool isRunning (AudioObjectID device);

/// Lets a start held by DeviceSpec::startWaitsForRelease finish.
void releaseStart (AudioObjectID device);

/// What the device's nominal rate property actually holds now.
double nominalRate (AudioObjectID device);

/// Whether this process currently owns hog mode on the device.
bool hogModeHeld (AudioObjectID device);

/// The pid written into kAudioDevicePropertyHogMode, or -1 for nobody.
///
/// hogModeHeld only answers "somebody holds it", which is not what §5.4
/// claims. A backend that wrote a bogus pid -- 0, or a stale one -- would pass
/// every existing hog-mode assertion while owning nothing it could later
/// release. The exclusivity claim is about THIS process, so the tests have to
/// be able to name it.
int hogOwnerPid (AudioObjectID device);

/// The buffer frame size the device settled on.
int bufferFrameSize (AudioObjectID device);

/// How many IOProcs have been CREATED on this device and not destroyed --
/// that is, how many times something opened it.
///
/// §5.2 and CoreAudio both want one. A duplex mixer presents its microphone
/// inputs and its monitor output as a single device, and asking macOS for a
/// second IOProc on a device this process has already hog-moded comes back as
/// "couldn't be opened for recording" against a microphone that is plugged in
/// and working -- so a four-input interface must be opened once, not four
/// times.
///
/// Exposed because the harness asserted that in words and checked nothing: the
/// line read `check (true, "one stream opens for the device, not one per
/// microphone")`, which passes just as happily when four are open.
///
/// Registrations rather than RUNNING procs, and the difference matters. This
/// fake hands back the proc's own function pointer as its AudioDeviceIOProcID,
/// so several registrations sharing one trampoline -- which is exactly what a
/// per-channel open would produce -- collide, and AudioDeviceStart marks only
/// the first of them running before returning. A count of running procs would
/// therefore read 1 whether one stream or four had been opened: the same
/// vacuous assertion in a less obvious costume.
int openIoProcCount (AudioObjectID device);

/// Delivers one input callback carrying `channels` (per-channel, equal length),
/// packed in whatever shape the device was configured with. Returns false if no
/// IOProc is running on the device.
bool pumpInput (AudioObjectID device, const std::vector<std::vector<float>>& channels);

/// Delivers one input callback with an explicit AudioBufferList layout. Each
/// entry is one interleaved buffer whose first value is its channel count and
/// whose second value is its sample storage. This exposes mixed layouts (for
/// example, an interleaved stereo buffer followed by a mono buffer) that real
/// HALs may provide but DeviceSpec::shape cannot describe.
struct InputBuffer
{
    int channels = 0;
    std::vector<float> samples;
};

bool pumpInputBuffers (AudioObjectID device, const std::vector<InputBuffer>& buffers);

/// Runs one output callback for `frames` and returns what the backend wrote,
/// de-interleaved back into per-channel vectors. This is the mirror of
/// pumpInput: it proves the backend's playback packing is right, not just that
/// it did not crash.
bool pumpOutput (AudioObjectID device, int frames, std::vector<std::vector<float>>& out);

} // namespace fakeca

/// IOKit's power-management SPI as the virtual HAL answers it: the backend
/// declares it itself, as the real one is in no SDK header. See
/// fakeca::setSystemAudioOn().
extern "C" uint32_t IOPMConnectionGetSystemCapabilities (void);
