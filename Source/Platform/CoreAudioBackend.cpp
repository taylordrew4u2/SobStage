#include <atomic>
#include <chrono>
#include <condition_variable>
#include "CoreAudioBackend.h"
#include "SystemAggregateDevice.h"

#if JUCE_MAC

#include "MacCoreAudioQueries.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>            // AudioGetCurrentHostTime(): the callback watchdog's clock
#include <CoreFoundation/CoreFoundation.h>
#include <time.h>                          // clock_gettime_nsec_np(): what counts through sleep
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h> // getpid() for hog-mode ownership
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <cmath>
#include <thread>
#include <unordered_set>
#include <vector>

// The Mac's power state: whether its audio is on, which it is not in a dark
// wake (awake with the lid shut for Power Nap or a backup) or on the way to
// sleep. IOKit's power-management SPI, declared in IOPMLibPrivate.h, which is
// not in the SDK; IOKit.framework has exported it since macOS 10.7 and the
// SDK's IOKit.tbd lists it. Imported weakly, so a macOS without it still
// launches, and the callback watchdog then just cannot ask (see
// systemAudioIsOff()). The simulation's virtual HAL defines it instead.
#if defined (MMA_SIMULATE_MAC)
extern "C" uint32_t IOPMConnectionGetSystemCapabilities (void);
#else
extern "C" uint32_t IOPMConnectionGetSystemCapabilities (void) __attribute__ ((weak_import));
#endif

namespace mma {

struct CoreAudioStream
{
    AudioObjectID deviceId = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcId = nullptr;
    AudioCallback callback;
    bool isOutput = false;
    bool ownsHogMode = false;

    /// The device's input-side latency, read once inside the bounded open
    /// (a HAL property read can wedge like any other) and kept for the
    /// message thread. See readInputLatencyFrames().
    int inputLatencyFrames = 0;

    // Channel pointer scratch, sized once at open time. §11 forbids allocation
    // inside the callback, so the IOProc only ever fills these.
    static constexpr int kMaxChannels = 64;
    const float* inputPointers[kMaxChannels] = {};
    float* outputPointers[kMaxChannels] = {};

    // Deinterleave scratch. Many USB microphones present one buffer carrying
    // N interleaved channels rather than N single-channel buffers; without
    // somewhere to unpack them the callback can only handle the second shape.
    // Sized at open time because §11 forbids allocating in the IOProc.
    std::vector<float> deinterleaveScratch;
    int maxFramesPerCallback = 0;

    /// §0.1: frames that reached the IOProc and were never handed to the
    /// callback, because the scratch was sized for less. Dropping is the right
    /// answer; dropping without counting was not.
    std::atomic<uint64_t> framesDropped { 0 };

    /// The device this stream was opened for, as the app names it, and when the
    /// IOProc last ran.
    ///
    /// CoreAudio has no worker loop of its own to notice dying -- the HAL calls
    /// the IOProc, and a device that stops simply stops being called. So the
    /// only way this backend can tell is to remember when it was last called
    /// and let the message thread ask. Without it macOS was the one platform
    /// where a stream that stopped after opening produced nothing at all.
    ///
    /// Both times are on the host clock (hostClockSeconds()), which stops
    /// while the Mac sleeps, so a night with the lid closed is not a night of
    /// silence.
    std::string uid;
    double expectedSampleRate = 0.0;
    double startedSeconds = 0.0;
    std::atomic<double> lastCallbackSeconds { 0.0 };
    std::atomic<bool> reportedDead { false };

    // One atomic is both the admission gate and the in-flight callback count.
    // A separate `enabled` flag and counter has a TOCTOU window where teardown
    // can observe zero just before a callback increments it. Here the closed
    // bit and every lease share one modification order, so once close wins no
    // new callback can acquire a lease.
    std::atomic<uint64_t> callbackLeases { 0 };

    // CoreAudio can deliver all three property notifications from threads the
    // app does not own. Processor-overload notifications in particular are
    // normally sent from the device's IO thread, so the listener itself may do
    // no allocation, locking, property reads, or logging. It only leaves these
    // atomics for takeStreamFailures() on the message thread.
    std::atomic<bool> nominalRateCheckPending { false };
    std::atomic<bool> deviceAliveCheckPending { false };
    std::atomic<uint64_t> processorOverloads { 0 };

    // The monitor output only: the backend's IAudioBackend::isOutputOffRate()
    // flag, raised by the nominal-rate listener the moment the device says its
    // rate changed and lowered by takeStreamFailures() once it reads the
    // stream's own rate back. Shared rather than pointing into the backend,
    // because a stream whose driver will not let go of it outlives the
    // backend (retainInertStream); its callback gate is closed by then, so
    // nothing writes through it, but the pointer stays valid regardless.
    // Null on an input stream.
    std::shared_ptr<std::atomic<bool>> outputOffRate;

    // Message-thread reporting latches. A rate mismatch or dead device is said
    // once until it recovers; overloads are reported whenever the count rises.
    bool sampleRateMismatchReported = false;
    bool deviceUnavailableReported = false;
    uint64_t reportedProcessorOverloads = 0;

    // When the callback watchdog called this input dead (0: not dead, or the
    // resume already reported), on the same clock as lastCallbackSeconds. A
    // callback after it means the HAL resumed the IOProc, and the app has to
    // hear that or the mic's track stays silent for the rest of the take.
    double deadReportedAt = 0.0;
    bool listenerProblemReported = false;

    // AudioObjectRemovePropertyListener must use the exact registrations that
    // succeeded. Some third-party drivers expose a property but refuse its
    // listener, and removing a registration that never existed obscures the
    // real teardown result.
    bool nominalRateListenerInstalled = false;
    bool deviceAliveListenerInstalled = false;
    bool processorOverloadListenerInstalled = false;

    // The mirror of the above for playback. An interface that presents its
    // output as one interleaved buffer needs the callback's per-channel writes
    // packed back together before the IOProc returns; without this the monitor
    // mix is silent on exactly the hardware most people plug in.
    std::vector<float> interleaveScratch;

    struct PendingInterleave
    {
        float* destination;   // the device's own interleaved buffer
        int firstChannel;     // index into interleaveScratch slices
        int channels;
        int frames;
    };

    PendingInterleave pendingInterleave[kMaxChannels] = {};
    int numPendingInterleave = 0;
};

struct CoreAudioDeviceListListenerState
{
    DeviceChangeCallback callback;

    // The system listener has the same raw-clientData lifetime problem as an
    // IOProc. Once the high bit closes admission, the low bits drain callbacks
    // that were already inside the trampoline.
    std::atomic<uint64_t> callbackLeases { 0 };

    // Set from the HAL thread when coreaudiod restarts: the per-output route
    // listeners belong to the old service, even where a device kept its
    // AudioObjectID, so the next sync on the message thread installs them
    // afresh.
    std::atomic<bool> serviceRestarted { false };
};

struct CoreAudioPendingInputAttempts
{
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_set<std::string> deviceIds;
    bool teardownInProgress = false;
    bool cleanupUnsafe = false;
    // Orders detached open/cleanup work without ever making the message thread
    // wait behind a wedged HAL call.
    std::mutex halTransactions;
};

namespace {

constexpr uint64_t kCallbackGateClosed = uint64_t { 1 } << 63;
constexpr uint64_t kCallbackLeaseCountMask = kCallbackGateClosed - 1;

// HAL calls are isolated on owned workers, so the caller must remain bounded
// even when a broken driver never returns.  A short deadline keeps startup and
// reconfiguration responsive while the worker continues its own cleanup.
//
// The same worker that the open deadline bounds may first spend the whole
// rate-settle window inside setNominalSampleRate, and only then take hog mode,
// create the IOProc and Start. Each open deadline must therefore contain the
// settle window with room to spare, or an ordinary 44.1 <-> 48 kHz switch is
// reported as a device that "took too long" (the output deadline was once
// 150 ms against a 500 ms settle window).
#if defined (MMA_SIMULATE_MAC)
constexpr auto kHalTransactionTimeout = std::chrono::milliseconds (250);
// The output open does the most HAL work (rate switch, hog mode, IOProc,
// Start); a loaded macOS CI runner has needed more than 250 ms for a healthy
// one, so its simulated deadline keeps the same headroom ratio as on a Mac.
constexpr auto kOutputHalTransactionTimeout = std::chrono::milliseconds (400);
constexpr auto kRateSettleTimeout = std::chrono::milliseconds (50);
#else
constexpr auto kHalTransactionTimeout = std::chrono::milliseconds (1000);
constexpr auto kOutputHalTransactionTimeout = std::chrono::milliseconds (1000);
constexpr auto kRateSettleTimeout = std::chrono::milliseconds (500);
#endif

static_assert (kHalTransactionTimeout > kRateSettleTimeout + std::chrono::milliseconds (100),
               "an input open must be allowed to finish settling its sample rate and start");
static_assert (kOutputHalTransactionTimeout > kRateSettleTimeout + std::chrono::milliseconds (100),
               "an output open must be allowed to finish settling its sample rate and start");
static_assert (kOutputHalTransactionTimeout >= kHalTransactionTimeout,
               "the output open does at least as much HAL work as an input open");

/// The callback watchdog's clock: seconds on the Mac's host clock
/// (mach_absolute_time, which CoreAudio stamps its own IO with). It stops while
/// the Mac sleeps. steady_clock does not -- libc++ builds it on
/// CLOCK_MONOTONIC_RAW, which keeps counting through sleep -- and an hour with
/// the lid closed read as an hour of every IOProc being silent. See
/// CallbackWatchdogSleepGuard.
///
/// Called from the IOProc. §11 allows it: a clock read and a fixed conversion,
/// with no allocation, lock or logging.
double hostClockSeconds() noexcept
{
    return static_cast<double> (AudioConvertHostTimeToNanos (AudioGetCurrentHostTime())) * 1.0e-9;
}

/// A clock that keeps counting while the Mac sleeps, set against
/// hostClockSeconds() only to notice that it slept. CLOCK_MONOTONIC_RAW
/// (mach_continuous_time) counts through sleep and, unlike the time of day,
/// is never stepped. It used to be the wall clock, which timed or someone
/// changing the time can step by seconds at once; since a sleep mid-take is
/// written into the take's record, a step read as a sleep left a gap in
/// session.json the audio never had.
double sleepCountingSeconds() noexcept
{
   #if defined (MMA_SIMULATE_MAC)
    return static_cast<double> (SimulatedContinuousTimeNanos()) * 1.0e-9;
   #else
    return static_cast<double> (clock_gettime_nsec_np (CLOCK_MONOTONIC_RAW)) * 1.0e-9;
   #endif
}

/// True when the Mac says its audio is off right now: a dark wake, where the
/// app runs and polls with the lid shut while CoreAudio runs no IOProc, for
/// minutes at a time; or the way down into sleep. No device can be judged by
/// its silence then. False when its audio is on, or the Mac cannot be asked.
///
/// An IPC to the power manager, not a clock read: message thread only, and
/// only while the callback watchdog is held or about to call something dead.
bool systemAudioIsOff() noexcept
{
    // IOPMLibPrivate.h's kIOPMCapabilityAudio, "audio output is supported";
    // spelled here because that header is not in the SDK.
    constexpr uint32_t kCapabilityAudio = 0x4;

    // A weak import's address is null on a macOS that does not have it. (Not
    // const: the simulation's is an ordinary definition, and a compiler that
    // folds the address warns that the check can never fail there.)
    auto* getSystemCapabilities = &IOPMConnectionGetSystemCapabilities;

    if (getSystemCapabilities == nullptr)
        return false;

    return (getSystemCapabilities() & kCapabilityAudio) == 0;
}

bool tryAcquireCallbackLease (CoreAudioStream& stream) noexcept
{
    auto state = stream.callbackLeases.load (std::memory_order_acquire);

    while ((state & kCallbackGateClosed) == 0)
    {
        if ((state & kCallbackLeaseCountMask) == kCallbackLeaseCountMask)
            return false;

        if (stream.callbackLeases.compare_exchange_weak (
                state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }

    return false;
}

bool tryAcquireCallbackLease (CoreAudioDeviceListListenerState& state) noexcept
{
    auto leases = state.callbackLeases.load (std::memory_order_acquire);

    while ((leases & kCallbackGateClosed) == 0)
    {
        if ((leases & kCallbackLeaseCountMask) == kCallbackLeaseCountMask)
            return false;

        if (state.callbackLeases.compare_exchange_weak (
                leases, leases + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }

    return false;
}

void releaseCallbackLease (CoreAudioStream& stream) noexcept
{
    stream.callbackLeases.fetch_sub (1, std::memory_order_release);
}

void releaseCallbackLease (CoreAudioDeviceListListenerState& state) noexcept
{
    state.callbackLeases.fetch_sub (1, std::memory_order_release);
}

void closeCallbackGateAndDrain (CoreAudioStream& stream) noexcept
{
    stream.callbackLeases.fetch_or (kCallbackGateClosed, std::memory_order_acq_rel);

    // The IOProc contract forbids blocking work, so an admitted callback is a
    // few hundred microseconds at most. Waiting for its lease is what makes it
    // safe for a timed-out worker to outlive the CaptureCoordinator captured by
    // the callback: after this returns that callback can never run again.
    while ((stream.callbackLeases.load (std::memory_order_acquire)
            & kCallbackLeaseCountMask) != 0)
        std::this_thread::yield();
}

void closeCallbackGateAndDrain (CoreAudioDeviceListListenerState& state) noexcept
{
    state.callbackLeases.fetch_or (kCallbackGateClosed, std::memory_order_acq_rel);

    while ((state.callbackLeases.load (std::memory_order_acquire)
            & kCallbackLeaseCountMask) != 0)
        std::this_thread::yield();
}

// A HAL that refuses to remove a callback may retain its clientData forever.
// Keep those small, closed-gate objects reachable for the process lifetime so
// a late driver callback is harmless and leak checkers can distinguish this
// deliberate quarantine from an accidental lost allocation.
void retainInertStream (std::unique_ptr<CoreAudioStream> stream) noexcept
{
    try
    {
        static auto* mutex = new std::mutex();
        static auto* streams = new std::vector<std::unique_ptr<CoreAudioStream>>();
        const std::lock_guard<std::mutex> guard (*mutex);
        streams->push_back (std::move (stream));
    }
    catch (...)
    {
        // Losing ownership is intentional here: deleting the object is the one
        // unsafe action when the HAL may still call its raw clientData.
        (void) stream.release();
    }
}

void retainInertDeviceListListener (
    std::unique_ptr<CoreAudioDeviceListListenerState> state) noexcept
{
    try
    {
        static auto* mutex = new std::mutex();
        static auto* states =
            new std::vector<std::unique_ptr<CoreAudioDeviceListListenerState>>();
        const std::lock_guard<std::mutex> guard (*mutex);
        states->push_back (std::move (state));
    }
    catch (...)
    {
        (void) state.release();
    }
}

struct PendingInputToken
{
    std::shared_ptr<CoreAudioPendingInputAttempts> registry;
    std::string deviceId;

    ~PendingInputToken()
    {
        if (registry == nullptr)
            return;

        {
            const std::lock_guard<std::mutex> guard (registry->mutex);
            registry->deviceIds.erase (deviceId);
        }

        registry->changed.notify_all();
    }
};

std::shared_ptr<PendingInputToken> beginInputAttempt (
    const std::shared_ptr<CoreAudioPendingInputAttempts>& registry,
    const std::string& deviceId)
{
    std::unique_lock<std::mutex> lock (registry->mutex);

    // Refused at once: an unsafe driver never becomes safe by waiting, and a
    // second attempt on a device whose own attempt is still in flight would
    // only queue another call behind the same stuck HAL.
    if (registry->cleanupUnsafe || registry->deviceIds.count (deviceId) != 0)
        return {};

    // Another device's attempt, or a teardown, is still finishing. HAL work is
    // serialized, so wait for it -- bounded, so a wedged driver still cannot
    // freeze the caller -- rather than failing every later device in the same
    // rebuild because one earlier device was slow. Every path that releases a
    // token, ends a teardown or sets cleanupUnsafe notifies `changed`.
    (void) registry->changed.wait_for (lock, kHalTransactionTimeout, [&registry]
    {
        return registry->cleanupUnsafe
            || (! registry->teardownInProgress && registry->deviceIds.empty());
    });

    if (registry->teardownInProgress || registry->cleanupUnsafe
        || ! registry->deviceIds.empty())
        return {};

    if (! registry->deviceIds.insert (deviceId).second)
        return {};

    auto token = std::make_shared<PendingInputToken>();
    token->registry = registry;
    token->deviceId = deviceId;
    return token;
}

void holdAudioTeardownQuarantine (
    const std::shared_ptr<CoreAudioPendingInputAttempts>& registry)
{
    {
        const std::lock_guard<std::mutex> guard (registry->mutex);
        registry->cleanupUnsafe = true;
    }

    // Wakes any attempt waiting in beginInputAttempt so it refuses now rather
    // than at the end of its bounded wait.
    registry->changed.notify_all();
}

// The shared HAL reads (MacCoreAudioQueries.h), also used by the combined
// device.
using macaudio::readStringProperty;
using macaudio::countChannels;
using macaudio::findDeviceByUID;
using macaudio::readActiveSubDevices;

/// §2.3: the depths this device can actually deliver.
///
/// CoreAudio keeps this on the STREAM rather than the device, so it is two
/// hops: the device's streams, then one stream's available physical formats.
/// One stream is enough -- a device's inputs share a format set, and the
/// question being asked is "what can this microphone give", not "what is each
/// channel doing right now".
///
/// Returns EMPTY when the device cannot be asked. That is not "this device is
/// limited": chooseRecordingBitDepth turns empty into the caller's fallback,
/// where a wrong list would silently change the depth of a recording.
std::vector<int> querySupportedBitDepths (AudioObjectID device)
{
    AudioObjectPropertyAddress streamsAddress { kAudioDevicePropertyStreams,
                                                kAudioObjectPropertyScopeInput,
                                                kAudioObjectPropertyElementMain };
    UInt32 streamsSize = 0;
    if (AudioObjectGetPropertyDataSize (device, &streamsAddress, 0, nullptr, &streamsSize) != noErr
        || streamsSize < sizeof (AudioObjectID))
        return {};

    std::vector<AudioObjectID> streams (streamsSize / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (device, &streamsAddress, 0, nullptr, &streamsSize,
                                    streams.data()) != noErr || streams.empty())
        return {};

    AudioObjectPropertyAddress formatsAddress { kAudioStreamPropertyAvailablePhysicalFormats,
                                                kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
    UInt32 formatsSize = 0;
    if (AudioObjectGetPropertyDataSize (streams.front(), &formatsAddress, 0, nullptr,
                                        &formatsSize) != noErr
        || formatsSize < sizeof (AudioStreamRangedDescription))
        return {};

    std::vector<AudioStreamRangedDescription> formats (formatsSize
                                                       / sizeof (AudioStreamRangedDescription));
    if (AudioObjectGetPropertyData (streams.front(), &formatsAddress, 0, nullptr, &formatsSize,
                                    formats.data()) != noErr)
        return {};

    std::vector<int> depths;

    for (const auto& format : formats)
    {
        const auto bits = static_cast<int> (format.mFormat.mBitsPerChannel);

        // Only what this app can write. A float stream reports 32 bits here and
        // is not a 32-bit integer format, so it is left out rather than read as
        // a depth the writer would take literally.
        if (format.mFormat.mFormatID != kAudioFormatLinearPCM)
            continue;

        if (bits != 16 && bits != 24)
            continue;

        if (std::find (depths.begin(), depths.end(), bits) == depths.end())
            depths.push_back (bits);
    }

    return depths;
}

std::vector<uint32_t> querySupportedSampleRates (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyAvailableNominalSampleRates,
                                         kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<AudioValueRange> ranges (size / sizeof (AudioValueRange));
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, ranges.data()) != noErr)
        return {};

    // A device may report discrete rates (mMinimum == mMaximum) or a
    // continuous range. Reporting only mMaximum, as this once did, hides every
    // rate a range covers -- so a device advertising 44100-96000 would look
    // like it could not do 48000, and §2.2 would negotiate around it.
    static constexpr uint32_t kCommonRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

    std::vector<uint32_t> rates;

    for (const auto& r : ranges)
    {
        if (r.mMinimum == r.mMaximum)
        {
            rates.push_back (static_cast<uint32_t> (r.mMaximum));
            continue;
        }

        for (auto rate : kCommonRates)
            if (static_cast<double> (rate) >= r.mMinimum && static_cast<double> (rate) <= r.mMaximum)
                rates.push_back (rate);
    }

    std::sort (rates.begin(), rates.end());
    rates.erase (std::unique (rates.begin(), rates.end()), rates.end());
    return rates;
}

// AudioObjectPropertyListenerProc trampoline: forwards into the backend's
// DeviceChangeCallback. Registered on kAudioObjectSystemObject for
// kAudioHardwarePropertyDevices so hotplug is delivered by the OS, never
// polled on a timer (§2) -- and, with the same clientData, for
// kAudioHardwarePropertyServiceRestarted, on each output's data source /
// jack state and on the combined device's shape, because all of them want the
// same re-evaluation.
OSStatus deviceListChanged (AudioObjectID, UInt32 numAddresses,
                            const AudioObjectPropertyAddress* addresses, void* clientData)
{
    auto* state = static_cast<CoreAudioDeviceListListenerState*> (clientData);

    if (state == nullptr || ! tryAcquireCallbackLease (*state))
        return noErr;

    struct LeaseReleaser
    {
        CoreAudioDeviceListListenerState& state;
        ~LeaseReleaser() { releaseCallbackLease (state); }
    } lease { *state };

    if (addresses != nullptr)
        for (UInt32 i = 0; i < numAddresses; ++i)
            if (addresses[i].mSelector == kAudioHardwarePropertyServiceRestarted)
                state->serviceRestarted.store (true, std::memory_order_relaxed);

    if (state->callback)
        state->callback();

    return noErr;
}

/// coreaudiod restarted (`sudo killall coreaudiod`, a crash, some driver
/// installs). Every AudioObjectID may now be stale and the combined device
/// may be gone, and nothing guarantees a device-list notification follows.
AudioObjectPropertyAddress serviceRestartedAddress()
{
    return { kAudioHardwarePropertyServiceRestarted,
             kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain };
}

/// What an output is routed to. On an Intel Mac, headphones going into the
/// built-in jack move "Built-in Output" from 'ispk' to 'hdpn' without adding
/// or removing a device, so the device-list listener never hears it; some
/// Macs report the plug through JackIsConnected as well or instead.
constexpr AudioObjectPropertySelector kOutputRouteSelectors[] = { kAudioDevicePropertyDataSource,
                                                                  kAudioDevicePropertyJackIsConnected };

AudioObjectPropertyAddress watchedAddress (AudioObjectPropertySelector selector, AudioObjectPropertyScope scope)
{
    return { selector, scope, kAudioObjectPropertyElementMain };
}

/// What the app's own combined device is made of. Its output channels are
/// numbered over the sub-devices active now, so one leaving it (a cable
/// bumped) or joining it late (activated after the device was made) moves
/// every later microphone's headphone jack to other channel numbers -- and
/// neither is sure to be followed by a device-list notification: an unplug's
/// own can arrive before the HAL has rebuilt the combined device, and a late
/// activation has none at all. The app re-reads the layout, and lays the
/// per-jack headphone gains again, on these.
struct WatchedProperty
{
    AudioObjectPropertySelector selector;
    AudioObjectPropertyScope scope;
};

constexpr WatchedProperty kCombinedDeviceLayoutProperties[] = {
    { kAudioAggregateDevicePropertyActiveSubDeviceList, kAudioObjectPropertyScopeGlobal },
    { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput },
};

/// The app's own combined device. Only an aggregate has an active sub-device
/// list, so only an aggregate's UID is read.
bool isOurCombinedDevice (AudioObjectID device)
{
    auto address = watchedAddress (kAudioAggregateDevicePropertyActiveSubDeviceList,
                                   kAudioObjectPropertyScopeGlobal);
    return AudioObjectHasProperty (device, &address)
        && readStringProperty (device, kAudioDevicePropertyDeviceUID) == kOurAggregateUid;
}

/// Every AudioObjectID the HAL lists now, or empty when it cannot be read.
std::vector<AudioObjectID> readDeviceList()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
        return {};

    std::vector<AudioObjectID> devices (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr)
        return {};

    devices.resize (size / sizeof (AudioObjectID));
    return devices;
}

AudioObjectPropertyAddress nominalRateAddress()
{
    return { kAudioDevicePropertyNominalSampleRate,
             kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain };
}

AudioObjectPropertyAddress deviceAliveAddress()
{
    return { kAudioDevicePropertyDeviceIsAlive,
             kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain };
}

AudioObjectPropertyAddress processorOverloadAddress()
{
    return { kAudioDeviceProcessorOverload,
             kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain };
}

// Per-device CoreAudio notification trampoline. kAudioDeviceProcessorOverload
// is normally delivered synchronously from the device IO thread, so this code
// deliberately does nothing beyond relaxed atomic stores/increments. Reading
// the new rate/alive property and composing reports is deferred to the message
// thread in takeStreamFailures().
OSStatus streamPropertyChanged (AudioObjectID, UInt32 numAddresses,
                                const AudioObjectPropertyAddress* addresses,
                                void* clientData)
{
    auto* stream = static_cast<CoreAudioStream*> (clientData);

    if (stream == nullptr || addresses == nullptr
        || ! tryAcquireCallbackLease (*stream))
        return noErr;

    struct LeaseReleaser
    {
        CoreAudioStream& stream;
        ~LeaseReleaser() { releaseCallbackLease (stream); }
    } lease { *stream };

    bool overloadSeen = false;

    for (UInt32 i = 0; i < numAddresses; ++i)
    {
        switch (addresses[i].mSelector)
        {
            case kAudioDevicePropertyNominalSampleRate:
                // §5.3: the headphone output's callback is the clock every
                // microphone is pulled on, and from this moment it may be
                // calling back at another rate. Held off the clock now, not
                // at the app's next poll up to half a second later, by which
                // time it had pulled every ring at that rate. An atomic
                // store, which is all this listener may do.
                //
                // The hold goes up before the pending mark, both sequentially
                // consistent. The other way round, a poll could take the
                // pending mark, read the rate unchanged and lower the hold all
                // between the two stores; the hold then went up after it with
                // nothing pending to lower it again, and the headphones stayed
                // off the clock for good. This way a poll that takes the mark
                // reads the rate after the hold went up, and a poll that comes
                // between the stores leaves the mark for the next one.
                if (stream->outputOffRate != nullptr)
                    stream->outputOffRate->store (true);

                stream->nominalRateCheckPending.store (true);
                break;

            case kAudioDevicePropertyDeviceIsAlive:
                stream->deviceAliveCheckPending.store (true, std::memory_order_relaxed);
                break;

            case kAudioDeviceProcessorOverload:
                overloadSeen = true;
                break;

            default:
                break;
        }
    }

    // A HAL callback may contain the same address more than once. One callback
    // represents one missed deadline, so count it once rather than inflating
    // the user-visible glitch total.
    if (overloadSeen)
        stream->processorOverloads.fetch_add (1, std::memory_order_relaxed);

    return noErr;
}

void installStreamPropertyListeners (CoreAudioStream& stream)
{
    auto address = nominalRateAddress();
    stream.nominalRateListenerInstalled =
        AudioObjectAddPropertyListener (stream.deviceId, &address,
                                        streamPropertyChanged, &stream) == noErr;

    address = deviceAliveAddress();
    stream.deviceAliveListenerInstalled =
        AudioObjectAddPropertyListener (stream.deviceId, &address,
                                        streamPropertyChanged, &stream) == noErr;

    address = processorOverloadAddress();
    stream.processorOverloadListenerInstalled =
        AudioObjectAddPropertyListener (stream.deviceId, &address,
                                        streamPropertyChanged, &stream) == noErr;
}

bool removeStreamPropertyListeners (CoreAudioStream& stream)
{
    bool allRemoved = true;

    if (stream.nominalRateListenerInstalled)
    {
        auto address = nominalRateAddress();
        if (AudioObjectRemovePropertyListener (stream.deviceId, &address,
                                               streamPropertyChanged, &stream) == noErr)
            stream.nominalRateListenerInstalled = false;
        else
            allRemoved = false;
    }

    if (stream.deviceAliveListenerInstalled)
    {
        auto address = deviceAliveAddress();
        if (AudioObjectRemovePropertyListener (stream.deviceId, &address,
                                               streamPropertyChanged, &stream) == noErr)
            stream.deviceAliveListenerInstalled = false;
        else
            allRemoved = false;
    }

    if (stream.processorOverloadListenerInstalled)
    {
        auto address = processorOverloadAddress();
        if (AudioObjectRemovePropertyListener (stream.deviceId, &address,
                                               streamPropertyChanged, &stream) == noErr)
            stream.processorOverloadListenerInstalled = false;
        else
            allRemoved = false;
    }

    return allRemoved;
}

bool readDeviceIsAlive (AudioObjectID device)
{
    auto address = deviceAliveAddress();
    UInt32 alive = 0;
    UInt32 size = sizeof (alive);

    // Once an unplug has invalidated the AudioObjectID, the read itself fails.
    // That is indistinguishable from (and just as actionable as) alive == 0.
    return AudioObjectGetPropertyData (device, &address, 0, nullptr,
                                       &size, &alive) == noErr
        && alive != 0;
}

/// The device's transport is the reliable distinction between an audio box
/// connected to this Mac and an input supplied by the Mac, an iPhone, or
/// software. Names are deliberately not involved: they are localised, mutable,
/// and routinely reused by drivers.
UInt32 readTransportType (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyTransportType,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 transport = 0;
    UInt32 size = sizeof (transport);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &transport) != noErr)
        return 0;

    return transport;
}

/// SobStage records only directly attached external hardware. Keep this as a
/// positive allow-list so a new virtual, wireless, or Continuity transport does
/// not silently become a recording source. A phone connected by cable still
/// reports a Continuity transport, not USB, and is therefore excluded.
bool isDirectlyAttachedInputTransport (UInt32 transport)
{
    return transport == kAudioDeviceTransportTypeUSB
        || transport == kAudioDeviceTransportTypeFireWire
        || transport == kAudioDeviceTransportTypeThunderbolt;
}

/// Bluetooth, Bluetooth LE and AirPlay outputs connect by themselves (AirPods
/// reconnecting, a paired speaker powering on). They stay available as monitor
/// outputs, but their arrival must not be read as headphones being plugged in.
bool isWirelessTransport (UInt32 transport)
{
    return transport == kAudioDeviceTransportTypeBluetooth
        || transport == kAudioDeviceTransportTypeBluetoothLE
        || transport == kAudioDeviceTransportTypeAirPlay;
}

/// The output macOS itself plays through, or kAudioObjectUnknown. macOS moves
/// it to headphones or an amp when they are plugged in, which is §5.3's
/// priority 4 ("system default output").
AudioObjectID readDefaultOutputDevice()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDefaultOutputDevice,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof (device);

    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr,
                                    &size, &device) != noErr)
        return kAudioObjectUnknown;

    return device;
}

/// One UInt32 output-scope property, or 0 when the device has none.
UInt32 readOutputUInt32 (AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain };
    UInt32 value = 0;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, &value) != noErr)
        return 0;

    return value;
}

/// §5.3 priority 3 on a Mac: only the Mac's own headphone jack. Apple Silicon
/// exposes it as a separate built-in device ("External Headphones") that exists
/// only while something is plugged in; Intel routes one built-in device to it.
/// USB, FireWire and Thunderbolt outputs are deliberately not marked: a capture
/// card's playback endpoint would otherwise outrank the Mac's own output.
bool isBuiltInHeadphoneOutput (const std::string& uid, bool isBuiltIn, UInt32 dataSource)
{
    constexpr UInt32 kHeadphonesDataSource = (UInt32 ('h') << 24) | (UInt32 ('d') << 16)
                                           | (UInt32 ('p') << 8)  |  UInt32 ('n');

    return isBuiltIn
        && (uid == "BuiltInHeadphoneOutputDevice" || dataSource == kHeadphonesDataSource);
}

/// True only when the HAL positively lists this stream's device as gone: its
/// AudioObjectID has left the device list, or now names a different UID. An
/// unreadable list is not proof of anything, so it answers false.
bool streamDeviceIsGone (const CoreAudioStream& stream)
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
        return false;

    std::vector<AudioObjectID> devices (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr)
        return false;

    devices.resize (size / sizeof (AudioObjectID));
    if (std::find (devices.begin(), devices.end(), stream.deviceId) == devices.end())
        return true;

    const auto uid = readStringProperty (stream.deviceId, kAudioDevicePropertyDeviceUID);
    return ! stream.uid.empty() && ! uid.empty() && uid != stream.uid;
}

double getNominalSampleRate (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    Float64 rate = 0.0;
    UInt32 size = sizeof (rate);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &rate) != noErr)
        return 0.0;

    return static_cast<double> (rate);
}

/// A sample rate as a person says it: "48 kHz", not "48000.000000".
std::string formatRate (double rate)
{
    const double khz = rate / 1000.0;
    char text[32] = {};

    if (std::abs (khz - std::round (khz)) < 0.01)
        std::snprintf (text, sizeof (text), "%d kHz", static_cast<int> (std::round (khz)));
    else
        std::snprintf (text, sizeof (text), "%.1f kHz", khz);

    return text;
}

bool setNominalSampleRate (AudioObjectID device, double sampleRate)
{
    // Ask first. A device another process already holds -- or one whose rate is
    // fixed in hardware -- refuses the write even when it is already running at
    // exactly the rate we want, and treating that as a failure turns a working
    // microphone into one that will not open.
    if (std::abs (getNominalSampleRate (device) - sampleRate) < 1.0)
        return true;

    AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    Float64 rate = sampleRate;

    if (AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (rate), &rate) != noErr)
        return false;

    // The HAL applies this property asynchronously on real USB devices. An
    // immediate read-back therefore turns a rate change that is still in flight
    // into a false refusal. This runs only while opening a stream, before its
    // IOProc exists, so a short sleep here can never block the real-time thread.
    //
    // Still keep the wait bounded: a driver that acknowledges the write but
    // never applies it must not hang launch. Confirm the final value rather than
    // assuming success, because landing on a neighbouring rate would make the
    // device a permanent drift source.
    constexpr auto pollInterval = std::chrono::milliseconds (10);
    const auto deadline = std::chrono::steady_clock::now() + kRateSettleTimeout;

    for (;;)
    {
        if (std::abs (getNominalSampleRate (device) - sampleRate) < 1.0)
            return true;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;

        std::this_thread::sleep_until (std::min (deadline, now + pollInterval));
    }
}

int getBufferFrameSize (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 value = 0;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &value) != noErr)
        return 0;

    return static_cast<int> (value);
}

UInt32 readInputUInt32 (AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeInput,
                                         kAudioObjectPropertyElementMain };
    UInt32 value = 0;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, &value) != noErr)
        return 0;

    return value;
}

// The input-side mirror of the output figure below: how long before the
// IOProc a captured sample was actually at the microphone, in frames -- the
// device's input latency, its input safety offset and its first input
// stream's latency. A device with no inputs, or a read that fails, counts as
// nothing.
int readInputLatencyFrames (AudioObjectID device)
{
    uint64_t frames = static_cast<uint64_t> (readInputUInt32 (device, kAudioDevicePropertyLatency))
                    + readInputUInt32 (device, kAudioDevicePropertySafetyOffset);

    AudioObjectPropertyAddress address { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) == noErr
        && size >= sizeof (AudioObjectID))
    {
        std::vector<AudioObjectID> streams (size / sizeof (AudioObjectID));
        if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, streams.data()) == noErr
            && ! streams.empty())
        {
            AudioObjectPropertyAddress latency { kAudioStreamPropertyLatency,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
            UInt32 value = 0;
            UInt32 valueSize = sizeof (value);
            if (AudioObjectGetPropertyData (streams.front(), &latency, 0, nullptr, &valueSize, &value) == noErr)
                frames += value;
        }
    }

    // Same ceiling as the output side: a confused driver's figure must not
    // delay every other microphone by minutes.
    return static_cast<int> (std::min<uint64_t> (frames, 192000));
}

// What the device adds after our buffers, in frames: its own latency, its
// safety offset and its first output stream's latency. Any read that fails
// counts as nothing -- an under-estimate, never an invented figure.
int readOutputPresentationLatencyFrames (AudioObjectID device)
{
    // 64-bit: a nonsense figure near UInt32's limit must not wrap around to a
    // small one and slip under the cap below.
    uint64_t frames = static_cast<uint64_t> (readOutputUInt32 (device, kAudioDevicePropertyLatency))
                    + readOutputUInt32 (device, kAudioDevicePropertySafetyOffset);

    AudioObjectPropertyAddress address { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) == noErr
        && size >= sizeof (AudioObjectID))
    {
        std::vector<AudioObjectID> streams (size / sizeof (AudioObjectID));
        if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, streams.data()) == noErr
            && ! streams.empty())
        {
            AudioObjectPropertyAddress latency { kAudioStreamPropertyLatency,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
            UInt32 value = 0;
            UInt32 valueSize = sizeof (value);
            if (AudioObjectGetPropertyData (streams.front(), &latency, 0, nullptr, &valueSize, &value) == noErr)
                frames += value;
        }
    }

    // A nonsense figure from a confused driver must not print a latency of
    // minutes; a second is already far past anything a person could monitor on.
    return static_cast<int> (std::min<uint64_t> (frames, 192000));
}

bool setBufferFrameSize (AudioObjectID device, int frames)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 value = static_cast<UInt32> (frames);
    return AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value) == noErr;
}

bool setHogMode (AudioObjectID device, pid_t owner)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyHogMode,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    pid_t value = owner;
    return AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value) == noErr;
}

bool takeHogMode (AudioObjectID device)
{
    return setHogMode (device, getpid());
}

bool releaseHogMode (AudioObjectID device)
{
    return setHogMode (device, -1);
}

/// The real-time IOProc. §11: no allocation, locking, logging or file I/O here.
/// It only unpacks the buffer lists into pre-sized pointer arrays and forwards.
OSStatus ioProcTrampoline (AudioObjectID /*device*/,
                           const AudioTimeStamp* /*now*/,
                           const AudioBufferList* inputData,
                           const AudioTimeStamp* /*inputTime*/,
                           AudioBufferList* outputData,
                           const AudioTimeStamp* /*outputTime*/,
                           void* clientData)
{
    auto* stream = static_cast<CoreAudioStream*> (clientData);

    if (stream == nullptr || ! tryAcquireCallbackLease (*stream))
        return noErr;

    struct LeaseReleaser
    {
        CoreAudioStream& stream;
        ~LeaseReleaser() { releaseCallbackLease (stream); }
    } lease { *stream };

    if (! stream->callback)
        return noErr;

    // A timestamp, taken before any work, so the message thread can tell a
    // stream that is running from one the HAL has stopped calling. §11 allows
    // this: the host clock allocates nothing and takes no lock.
    stream->lastCallbackSeconds.store (hostClockSeconds(), std::memory_order_relaxed);

    // A device that was called dead and has started running again is alive, and
    // the latch has to let go or the next genuine death is never reported.
    if (stream->reportedDead.load (std::memory_order_relaxed))
        stream->reportedDead.store (false, std::memory_order_relaxed);

    int numInputChannels = 0;
    int numSamples = 0;

    if (inputData != nullptr)
    {
        for (UInt32 i = 0; i < inputData->mNumberBuffers && numInputChannels < CoreAudioStream::kMaxChannels; ++i)
        {
            const auto& buffer = inputData->mBuffers[i];

            if (buffer.mData == nullptr || buffer.mNumberChannels == 0)
                continue;

            const int channelsHere = static_cast<int> (buffer.mNumberChannels);
            const int framesHere = static_cast<int> (buffer.mDataByteSize
                                                     / (sizeof (float) * buffer.mNumberChannels));

            if (channelsHere == 1)
            {
                // One channel per buffer: hand the device's own memory straight
                // through, no copy.
                stream->inputPointers[numInputChannels++] = static_cast<const float*> (buffer.mData);
                numSamples = framesHere;
                continue;
            }

            // Interleaved. Unpacking is required, and skipping it -- as this
            // once did -- means a stereo USB microphone records pure silence
            // with no error anywhere.
            if (framesHere > stream->maxFramesPerCallback
                || numInputChannels + channelsHere > CoreAudioStream::kMaxChannels)
            {
                // Dropping still beats overrunning the scratch, which was sized
                // at open time and cannot grow on this thread (§11). What was
                // missing is the count: §0.1 makes unreported loss the one
                // unacceptable failure, and this discarded a whole device's
                // block with nothing anywhere recording that it had.
                stream->framesDropped.fetch_add (static_cast<uint64_t> (framesHere),
                                                 std::memory_order_relaxed);

                // Keep the physical channel positions occupied even though
                // this buffer cannot be unpacked. Otherwise a later buffer in
                // the same AudioBufferList slides into these slots and can be
                // mistaken for a selected input channel.
                const int slotsToReserve = std::min (channelsHere,
                                                     CoreAudioStream::kMaxChannels - numInputChannels);
                for (int ch = 0; ch < slotsToReserve; ++ch)
                    stream->inputPointers[numInputChannels++] = nullptr;

                continue;
            }

            const auto* source = static_cast<const float*> (buffer.mData);

            for (int ch = 0; ch < channelsHere; ++ch)
            {
                float* dest = stream->deinterleaveScratch.data()
                            + static_cast<size_t> (numInputChannels) * stream->maxFramesPerCallback;

                for (int f = 0; f < framesHere; ++f)
                    dest[f] = source[f * channelsHere + ch];

                stream->inputPointers[numInputChannels++] = dest;
            }

            numSamples = framesHere;
        }
    }

    int numOutputChannels = 0;

    if (outputData != nullptr)
    {
        for (UInt32 i = 0; i < outputData->mNumberBuffers && numOutputChannels < CoreAudioStream::kMaxChannels; ++i)
        {
            auto& buffer = outputData->mBuffers[i];

            if (buffer.mData == nullptr || buffer.mNumberChannels == 0)
                continue;

            const int channelsHere = static_cast<int> (buffer.mNumberChannels);
            const int framesHere = static_cast<int> (buffer.mDataByteSize
                                                     / (sizeof (float) * buffer.mNumberChannels));

            if (channelsHere == 1)
            {
                stream->outputPointers[numOutputChannels++] = static_cast<float*> (buffer.mData);
                numSamples = framesHere;
                continue;
            }

            // Both the scratch and the channel table must hold the whole
            // buffer: a partial take would repack with the wrong stride.
            if (framesHere > stream->maxFramesPerCallback
                || numOutputChannels + channelsHere > CoreAudioStream::kMaxChannels)
                continue;

            auto& pending = stream->pendingInterleave[stream->numPendingInterleave++];
            pending.destination = static_cast<float*> (buffer.mData);
            pending.firstChannel = numOutputChannels;
            pending.channels = 0;
            pending.frames = framesHere;

            for (int ch = 0; ch < channelsHere; ++ch)
            {
                stream->outputPointers[numOutputChannels++] = stream->interleaveScratch.data()
                    + static_cast<size_t> (pending.firstChannel + ch) * stream->maxFramesPerCallback;
                ++pending.channels;
            }

            numSamples = framesHere;
        }
    }

    if (numSamples > 0)
        stream->callback (stream->inputPointers, numInputChannels,
                          stream->outputPointers, numOutputChannels,
                          numSamples);

    // Pack whatever the callback wrote into the scratch back into the device's
    // interleaved buffers. Done after the callback so it sees a flat layout.
    for (int i = 0; i < stream->numPendingInterleave; ++i)
    {
        const auto& pending = stream->pendingInterleave[i];

        for (int ch = 0; ch < pending.channels; ++ch)
        {
            const float* source = stream->interleaveScratch.data()
                                + static_cast<size_t> (pending.firstChannel + ch) * stream->maxFramesPerCallback;

            for (int f = 0; f < pending.frames; ++f)
                pending.destination[f * pending.channels + ch] = source[f];
        }
    }

    stream->numPendingInterleave = 0;

    return noErr;
}

struct CoreAudioOpenResult
{
    bool succeeded = false;
    std::string error;
    bool streamSafeToDelete = true;
    bool cleanupSucceeded = true;
    bool bufferSizeWasRefused = false;
    OpenRefusal refusal = OpenRefusal::unknown;
};

// The refusal's kind, for §14.2's power check, on a result built the usual way.
CoreAudioOpenResult refusedBecause (OpenRefusal why, CoreAudioOpenResult result)
{
    result.refusal = why;
    return result;
}

struct CoreAudioCleanupResult
{
    bool streamSafeToDelete = true;
    bool cleanupSucceeded = true;
};

CoreAudioCleanupResult destroyStreamAfterFailedOpen (CoreAudioStream& stream)
{
    closeCallbackGateAndDrain (stream);
    const bool listenersRemoved = removeStreamPropertyListeners (stream);
    bool ioProcDestroyed = true;

    if (stream.ioProcId != nullptr)
    {
        ioProcDestroyed = AudioDeviceDestroyIOProcID (stream.deviceId, stream.ioProcId) == noErr;
        if (ioProcDestroyed)
            stream.ioProcId = nullptr;
    }

    bool hogReleased = true;
    if (stream.ownsHogMode)
    {
        hogReleased = releaseHogMode (stream.deviceId);
        if (hogReleased)
            stream.ownsHogMode = false;
    }

    // An unplugged device fails Destroy and listener removal with
    // kAudioHardwareBadObjectError because the HAL has already discarded the
    // object and every registration on it. That is not a refusal, so it must
    // not quarantine the survivors; the stream is still retained, inert.
    const bool safe = listenersRemoved && ioProcDestroyed;
    const bool succeeded = safe && hogReleased;
    return { safe, succeeded || streamDeviceIsGone (stream) };
}

CoreAudioCleanupResult stopAndDestroyAbandonedStream (CoreAudioStream& stream)
{
    closeCallbackGateAndDrain (stream);
    bool listenersRemoved = true;
    bool ioProcDestroyed = true;
    bool stopped = true;

    if (stream.ioProcId != nullptr)
    {
        stopped = AudioDeviceStop (stream.deviceId, stream.ioProcId) == noErr;
        listenersRemoved = removeStreamPropertyListeners (stream);
        ioProcDestroyed = AudioDeviceDestroyIOProcID (stream.deviceId, stream.ioProcId) == noErr;
        if (ioProcDestroyed)
            stream.ioProcId = nullptr;
    }
    else
    {
        listenersRemoved = removeStreamPropertyListeners (stream);
    }

    bool hogReleased = true;
    if (stream.ownsHogMode)
    {
        hogReleased = releaseHogMode (stream.deviceId);
        if (hogReleased)
            stream.ownsHogMode = false;
    }

    // If CoreAudio refused to remove either callback registration, its
    // clientData may still be called later. Retaining the tiny stream object is
    // safer than freeing that pointer; the closed lease gate makes it inert.
    // An unplug makes Stop, listener removal and Destroy all fail with
    // kAudioHardwareBadObjectError: the object and its registrations are
    // already gone. Only a refusal from a device that is still present means
    // CoreAudio may call us again, and only that may quarantine the backend.
    const bool safe = listenersRemoved && ioProcDestroyed;
    const bool succeeded = stopped && safe && hogReleased;
    return { safe, succeeded || streamDeviceIsGone (stream) };
}

// The HAL transaction intentionally includes rollback. Real devices have been
// observed blocking not only in CreateIOProc/Start but also in DestroyIOProc
// after Start fails. Running the whole transaction on one owned worker means a
// timeout never hands closeAllStreams a half-created IOProc.
CoreAudioOpenResult createAndStartStream (CoreAudioStream& stream)
{
    if (AudioDeviceCreateIOProcID (stream.deviceId, ioProcTrampoline, &stream,
                                   &stream.ioProcId) != noErr
        || stream.ioProcId == nullptr)
    {
        CoreAudioCleanupResult cleanup;
        if (stream.ioProcId != nullptr || stream.ownsHogMode)
            cleanup = destroyStreamAfterFailedOpen (stream);

        return refusedBecause (OpenRefusal::inUse, {
                 false,
                 "macOS wouldn't let this app attach to this interface. Another app is usually "
                 "holding it -- close anything else recording or streaming from it, then try again.",
                 cleanup.streamSafeToDelete, cleanup.cleanupSucceeded });
    }

    installStreamPropertyListeners (stream);

    const double rateImmediatelyBeforeStart = getNominalSampleRate (stream.deviceId);
    if (! readDeviceIsAlive (stream.deviceId)
        || rateImmediatelyBeforeStart <= 0.0
        || std::abs (rateImmediatelyBeforeStart - stream.expectedSampleRate) >= 1.0)
    {
        if (rateImmediatelyBeforeStart > 0.0
            && std::abs (rateImmediatelyBeforeStart - stream.expectedSampleRate) >= 1.0)
        {
            const auto error = "This interface changed to " + formatRate (rateImmediatelyBeforeStart)
                             + " while SobStage was opening it at "
                             + formatRate (stream.expectedSampleRate)
                             + ". Set both to the same rate, then try again.";
            const auto cleanup = destroyStreamAfterFailedOpen (stream);
            return refusedBecause (OpenRefusal::settingsRefused,
                                   { false, error, cleanup.streamSafeToDelete, cleanup.cleanupSucceeded });
        }

        const auto cleanup = destroyStreamAfterFailedOpen (stream);
        return refusedBecause (OpenRefusal::gone, {
                 false,
                 "This interface disconnected while SobStage was opening it. Plug it back in, "
                 "then try again.", cleanup.streamSafeToDelete, cleanup.cleanupSucceeded });
    }

    if (AudioDeviceStart (stream.deviceId, stream.ioProcId) != noErr)
    {
        const auto cleanup = destroyStreamAfterFailedOpen (stream);
        return refusedBecause (OpenRefusal::permission, {
                 false,
                 "macOS wouldn't start this interface. Check that SobStage is allowed to use the "
                 "microphone in System Settings > Privacy & Security > Microphone, and that no "
                 "other app is recording from it.", cleanup.streamSafeToDelete,
                 cleanup.cleanupSucceeded });
    }

    stream.startedSeconds = hostClockSeconds();
    return { true, {}, true, true };
}

/// Whether hog mode on this output would take a microphone away from other
/// apps. Hog mode is per DEVICE, not per direction: on a microphone's own
/// headphone jack (a Yeti's) it locks the microphone's input too, and Zoom or
/// OBS recording from that microphone -- or from the combined device it is a
/// part of -- loses it. So it is skipped on the combined device itself, on any
/// output that also has input streams, and on any device the combined device
/// lists as a sub-device. Their inputs are microphones already running, so
/// there is nothing a second client could do to the timing that hog mode
/// would prevent.
bool outputIsSharedWithRecording (AudioObjectID device, const std::string& deviceUid)
{
    if (deviceUid == kOurAggregateUid)
        return true;

    if (countChannels (device, true) > 0)
        return true;

    if (const auto combined = findDeviceByUID (kOurAggregateUid); combined != kAudioObjectUnknown)
        for (const auto sub : readActiveSubDevices (combined))
            if (sub == device)
                return true;

    return false;
}

// Every device call involved in opening belongs in the bounded HAL
// transaction, including UID resolution, transport/rate checks, buffer setup,
// hog mode, IOProc creation and Start. A bad driver can block any of these --
// moving only AudioDeviceStart off the launch thread merely moves the freeze to
// the property call immediately before it.
CoreAudioOpenResult prepareAndStartStream (CoreAudioStream& stream,
                                           const std::string& deviceUid,
                                           double sampleRate,
                                           int bufferSizeSamples,
                                           bool isOutput)
{
    const auto device = findDeviceByUID (deviceUid);

    if (device == kAudioObjectUnknown)
    {
        return refusedBecause (OpenRefusal::gone, {
                 false,
                 isOutput
                    ? "That sound output isn't there any more. Choose another one."
                    : "This microphone is no longer connected. Unplug it and plug it back in, "
                      "then try again." });
    }

    stream.deviceId = device;

    if (! isOutput && ! isDirectlyAttachedInputTransport (readTransportType (device)))
    {
        return refusedBecause (OpenRefusal::notExternal, {
                 false,
                 "SobStage only records from a directly connected external USB, FireWire, "
                 "or Thunderbolt microphone or audio interface." });
    }

    if (! readDeviceIsAlive (device))
    {
        return refusedBecause (OpenRefusal::gone, {
                 false,
                 isOutput
                    ? "That sound output disconnected while SobStage was opening it. Choose another one."
                    : "This interface disconnected while SobStage was opening it. Plug it back in, "
                      "then try again." });
    }

    if (! setNominalSampleRate (device, sampleRate))
    {
        const double actual = getNominalSampleRate (device);

        // Unplugged during the settle wait: the rate read now fails and comes
        // back as 0, and the advice below became "Set the recording to 0 kHz".
        if (actual <= 0.0 || ! readDeviceIsAlive (device))
        {
            return refusedBecause (OpenRefusal::gone, {
                     false,
                     isOutput
                        ? "That sound output disconnected while SobStage was opening it. Choose another one."
                        : "This interface disconnected while SobStage was opening it. Plug it back in, "
                          "then try again." });
        }

        return refusedBecause (OpenRefusal::settingsRefused, {
                 false,
                 "This interface is running at " + formatRate (actual)
                    + " and won't change to the " + formatRate (sampleRate)
                    + " this recording uses. Set the recording to " + formatRate (actual)
                    + " in Settings, or change the interface to " + formatRate (sampleRate)
                    + " in Audio MIDI Setup." });
    }

    const bool bufferSizeRefused = ! setBufferFrameSize (device, bufferSizeSamples);

    // Read here, inside the bounded open, rather than on the message thread
    // later. A duplex output carries microphones too, so it is read for both.
    stream.inputLatencyFrames = readInputLatencyFrames (device);

    stream.uid = deviceUid;
    stream.expectedSampleRate = sampleRate;
    stream.isOutput = isOutput;

    const int granted = std::max (getBufferFrameSize (device), bufferSizeSamples);
    stream.maxFramesPerCallback = std::max (granted * 2, 4096);

    const size_t scratchSamples = static_cast<size_t> (CoreAudioStream::kMaxChannels)
                                * static_cast<size_t> (stream.maxFramesPerCallback);
    stream.deinterleaveScratch.assign (scratchSamples, 0.0f);
    stream.interleaveScratch.assign (scratchSamples, 0.0f);

    // Not on a device other apps record from: hog mode would take it away from
    // them -- the one thing the combined device exists to give them. See
    // outputIsSharedWithRecording().
    if (isOutput && ! outputIsSharedWithRecording (device, deviceUid))
    {
        if (! takeHogMode (device))
        {
            return { false,
                     "This sound output won't give this app exclusive use, which live monitoring needs. "
                     "Pick a different output in Advanced -- a USB or Thunderbolt interface usually works, "
                     "Bluetooth headphones usually don't.",
                     true, true, bufferSizeRefused };
        }

        stream.ownsHogMode = true;
    }

    auto result = createAndStartStream (stream);
    result.bufferSizeWasRefused = bufferSizeRefused;
    return result;
}

} // namespace

CoreAudioBackend::CoreAudioBackend()
    : pendingInputAttempts (std::make_shared<CoreAudioPendingInputAttempts>())
{
}

CoreAudioBackend::~CoreAudioBackend()
{
    closeAllStreams();
    removeDeviceListListener();
}

std::vector<AudioDeviceDescriptor> CoreAudioBackend::enumerateDevices (bool wantInput)
{
    std::vector<AudioDeviceDescriptor> result;

    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
        return result;

    std::vector<AudioObjectID> deviceIds (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, deviceIds.data()) != noErr)
        return result;

    // Read once per enumeration, outputs only: an input is never a monitor
    // candidate, even on a duplex device that is also the default output.
    const AudioObjectID defaultOutput = wantInput ? kAudioObjectUnknown : readDefaultOutputDevice();

    for (auto deviceId : deviceIds)
    {
        // DeviceIsAlive can fall to zero before the AudioObject disappears
        // from kAudioHardwarePropertyDevices. Treating that dying object as
        // present delays the existing mid-take unplug/silence path until a
        // later device-list event; filtering it here lets the per-device alive
        // listener drive the same reconciliation immediately.
        if (! readDeviceIsAlive (deviceId))
            continue;

        const int channels = countChannels (deviceId, wantInput);
        if (channels <= 0)
            continue;

        AudioDeviceDescriptor d;
        d.name = readStringProperty (deviceId, kAudioObjectPropertyName);
        // §2.4: USB location ID would come from IORegistry (kUSBDevicePropertyLocationID)
        // by walking the device's IOKit registry entry via
        // kAudioDevicePropertyDeviceUID's underlying transport; using the
        // stable CoreAudio UID string here as the practical stand-in since it
        // already encodes enough to distinguish ports across reconnects.
        d.usbLocationId = readStringProperty (deviceId, kAudioDevicePropertyDeviceUID);

        // Never feed our published aggregate back into its own source list.
        if (d.usbLocationId == kOurAggregateUid)
            continue;

        const auto transport = readTransportType (deviceId);

        // §2 is an external-hardware recorder. The Mac's microphone, iPhone
        // Continuity inputs (wired or wireless), Bluetooth/AirPlay sources,
        // and aggregate/virtual devices never belong in its microphone list.
        // Outputs are intentionally unaffected: built-in speakers and
        // Bluetooth headphones can still be monitor destinations.
        if (wantInput && ! isDirectlyAttachedInputTransport (transport))
            continue;

        d.isBuiltIn = (transport == kAudioDeviceTransportTypeBuiltIn);
        d.maxInputChannels = wantInput ? channels : 0;
        d.supportedSampleRates = querySupportedSampleRates (deviceId);
        d.supportedBitDepths = querySupportedBitDepths (deviceId);
        d.currentSampleRate = static_cast<uint32_t> (getNominalSampleRate (deviceId) + 0.5);
        d.isMicrophone = wantInput;

        if (! wantInput)
        {
            d.hasPhysicalHeadphoneJack = isBuiltInHeadphoneOutput (
                d.usbLocationId, d.isBuiltIn, readOutputUInt32 (deviceId, kAudioDevicePropertyDataSource));
            d.isSystemDefault = defaultOutput != kAudioObjectUnknown && deviceId == defaultOutput;
            d.isWireless = isWirelessTransport (transport);
        }

        result.push_back (d);
    }

    return result;
}

std::vector<AudioDeviceDescriptor> CoreAudioBackend::enumerateInputDevices() { return enumerateDevices (true); }
std::vector<AudioDeviceDescriptor> CoreAudioBackend::enumerateOutputDevices()
{
    // Every rescan reads the outputs, so this is where the route listeners
    // follow outputs arriving and leaving. The re-read below then sees the
    // data source the listener will watch from here on.
    syncOutputRouteListeners();
    return enumerateDevices (false);
}

void CoreAudioBackend::setDeviceChangeCallback (DeviceChangeCallback callback)
{
    hotplugProblem.clear();
    removeDeviceListListener();

    if (! callback)
        return;

    deviceListListenerState = std::make_unique<CoreAudioDeviceListListenerState>();
    deviceListListenerState->callback = std::move (callback);
    installDeviceListListener();
    syncOutputRouteListeners();
}

void CoreAudioBackend::installDeviceListListener()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };

    if (deviceListListenerState == nullptr)
        return;

    // The result is read. Discarded, a listener macOS refused to install left
    // the app deaf to the rig for the rest of the session without a word: a
    // microphone plugged in is never noticed, and one pulled out MID-TAKE is
    // never reported, so a take that lost a channel looks like a clean one.
    if (AudioObjectAddPropertyListener (kAudioObjectSystemObject, &address, deviceListChanged,
                                        deviceListListenerState.get()) != noErr)
    {
        hotplugProblem =
            "This Mac won't tell the app when microphones are plugged in or unplugged, so the list "
            "only updates when the app starts. Restart it after changing your rig.";
        deviceListListenerState.reset();
        return;
    }

    deviceListListenerInstalled = true;

    // Not essential, so a refusal is not reported: a restart that removed
    // the combined device usually changes the device list too. When it does
    // not, this is the only word the app gets.
    auto restarted = serviceRestartedAddress();
    serviceRestartListenerInstalled =
        AudioObjectAddPropertyListener (kAudioObjectSystemObject, &restarted, deviceListChanged,
                                        deviceListListenerState.get()) == noErr;
}

void CoreAudioBackend::syncOutputRouteListeners()
{
    if (deviceListListenerState == nullptr || ! deviceListListenerInstalled)
        return;

    const auto devices = readDeviceList();

    // An unreadable list proves nothing about which devices left.
    if (devices.empty())
        return;

    // After a coreaudiod restart nothing registered with the old service is
    // trusted, listed or not.
    const bool startAfresh = deviceListListenerState->serviceRestarted.exchange (
        false, std::memory_order_relaxed);

    const auto listed = [&devices, startAfresh] (AudioObjectID device)
    {
        return ! startAfresh && std::find (devices.begin(), devices.end(), device) != devices.end();
    };

    // A device that has left the list (or every one, after coreaudiod
    // restarted) takes its registration with it. The remove usually fails
    // because the object is gone; that is harmless, since the clientData is
    // the listener state, which is never freed while the HAL may hold it.
    outputRouteListeners.erase (
        std::remove_if (outputRouteListeners.begin(), outputRouteListeners.end(),
                        [&] (const OutputRouteListener& registration)
                        {
                            if (listed (registration.device))
                                return false;

                            auto address = watchedAddress (registration.selector, registration.scope);
                            (void) AudioObjectRemovePropertyListener (registration.device, &address,
                                                                      deviceListChanged,
                                                                      deviceListListenerState.get());
                            return true;
                        }),
        outputRouteListeners.end());

    const auto watch = [this] (AudioObjectID device, AudioObjectPropertySelector selector,
                               AudioObjectPropertyScope scope)
    {
        const bool alreadyWatched = std::any_of (
            outputRouteListeners.begin(), outputRouteListeners.end(),
            [device, selector, scope] (const OutputRouteListener& registration)
            {
                return registration.device == device && registration.selector == selector
                    && registration.scope == scope;
            });

        if (alreadyWatched)
            return;

        auto address = watchedAddress (selector, scope);
        if (! AudioObjectHasProperty (device, &address))
            return;

        if (AudioObjectAddPropertyListener (device, &address, deviceListChanged,
                                            deviceListListenerState.get()) == noErr)
            outputRouteListeners.push_back ({ device, selector, scope });
    };

    for (auto device : devices)
    {
        // Most outputs have neither route property; only one that does can
        // change route without changing the device list.
        if (countChannels (device, false) > 0)
            for (auto selector : kOutputRouteSelectors)
                watch (device, selector, kAudioObjectPropertyScopeOutput);

        // Whether or not it has outputs yet: one made before its microphones
        // activate reports none until they do.
        if (isOurCombinedDevice (device))
            for (const auto& property : kCombinedDeviceLayoutProperties)
                watch (device, property.selector, property.scope);
    }
}

void CoreAudioBackend::removeDeviceListListener()
{
    if (deviceListListenerState == nullptr)
        return;

    closeCallbackGateAndDrain (*deviceListListenerState);

    // Output-route registrations share this clientData. A device already gone
    // refuses the remove, which is expected; the retirement below covers it.
    for (const auto& registration : outputRouteListeners)
    {
        auto routeAddress = watchedAddress (registration.selector, registration.scope);
        (void) AudioObjectRemovePropertyListener (registration.device, &routeAddress, deviceListChanged,
                                                  deviceListListenerState.get());
    }

    outputRouteListeners.clear();

    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };

    bool removed = true;
    if (deviceListListenerInstalled)
        removed = AudioObjectRemovePropertyListener (
            kAudioObjectSystemObject, &address, deviceListChanged,
            deviceListListenerState.get()) == noErr;

    deviceListListenerInstalled = false;

    if (serviceRestartListenerInstalled)
    {
        auto restarted = serviceRestartedAddress();
        removed = AudioObjectRemovePropertyListener (
                      kAudioObjectSystemObject, &restarted, deviceListChanged,
                      deviceListListenerState.get()) == noErr
               && removed;
    }

    serviceRestartListenerInstalled = false;

    // Even a successful remove may overlap a notification the HAL dispatched
    // just before it returned. Process-lifetime retirement makes that raw
    // clientData safe in both the success and refusal cases; its gate is closed
    // so it can never call the backend's former owner again.
    retainInertDeviceListListener (std::move (deviceListListenerState));

    if (! removed)
        hotplugProblem =
            "macOS didn't fully release the microphone hot-plug listener. SobStage made it inert, "
            "but restart the app before changing the audio rig again.";
}

ExclusiveModeCapability CoreAudioBackend::checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                        double sampleRate, int bufferSizeSamples)
{
    auto token = beginInputAttempt (pendingInputAttempts, outputDeviceId);
    if (token == nullptr)
    {
        ExclusiveModeCapability cap;
        cap.unavailableReason = "macOS is still finishing an earlier audio-rig operation. Wait a "
                                "moment, then try this output again.";
        return cap;
    }

    struct CapabilityAttempt
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool finished = false;
        ExclusiveModeCapability result;
        std::shared_ptr<PendingInputToken> token;
    };

    auto attempt = std::make_shared<CapabilityAttempt>();
    attempt->token = std::move (token);

    std::thread worker;
    try
    {
        worker = std::thread ([attempt, outputDeviceId, sampleRate, bufferSizeSamples]
        {
            ExclusiveModeCapability cap;

            {
                const std::lock_guard<std::mutex> halGuard (
                    attempt->token->registry->halTransactions);

                // CoreAudio's hog mode plus a direct IOProc is the
                // exclusive-equivalent path. UID resolution and the hog-owner
                // read are both driver calls, so they share the open deadline.
                const auto device = findDeviceByUID (outputDeviceId);
                if (device == kAudioObjectUnknown)
                {
                    cap.unavailableReason = "That sound output isn't connected any more.";
                }
                else
                {
                    AudioObjectPropertyAddress address {
                        kAudioDevicePropertyHogMode,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain };
                    pid_t owner = -1;
                    UInt32 size = sizeof (owner);

                    if (AudioObjectGetPropertyData (device, &address, 0, nullptr,
                                                    &size, &owner) == noErr
                        && owner != -1 && owner != getpid())
                    {
                        cap.unavailableReason =
                            "Another app has taken exclusive control of your headphones. Quit it, "
                            "then reopen this app.";
                    }
                    else
                    {
                        // The rate, which this never asked about. Hog mode was
                        // the whole test, so a fixed-rate 44.1 kHz interface --
                        // ordinary hardware, and most USB mics -- was reported
                        // as ready for exclusive monitoring and then refused
                        // the open with a rate-mismatch message. §5.4's
                        // preflight exists precisely so the user hears that
                        // while there is still time to act on it, not at the
                        // top of a take.
                        //
                        // Asked the way openStream's failure is reached: it
                        // writes the nominal rate, so what matters is whether
                        // the device is already there or lists the rate as one
                        // it supports.
                        const auto supported = querySupportedSampleRates (device);
                        const bool alreadyRunningThere =
                            std::abs (getNominalSampleRate (device) - sampleRate) < 1.0;
                        const bool advertisesIt =
                            std::find (supported.begin(), supported.end(),
                                       static_cast<uint32_t> (sampleRate)) != supported.end();

                        // An empty list is a property read that told us nothing,
                        // not a device that supports nothing, so it is not a
                        // reason to refuse. A preflight may only say no when it
                        // is sure; the open remains the authority.
                        if (! supported.empty() && ! alreadyRunningThere && ! advertisesIt)
                        {
                            const double actual = getNominalSampleRate (device);
                            cap.unavailableReason =
                                "This interface is running at " + formatRate (actual)
                                    + " and won't change to the " + formatRate (sampleRate)
                                    + " this recording uses. Set the recording to "
                                    + formatRate (actual) + " in Settings, or change the interface to "
                                    + formatRate (sampleRate) + " in Audio MIDI Setup.";
                        }
                        else
                        {
                            cap.exclusiveModeAvailable = true;
                            cap.measuredOrEstimatedLatencyMs =
                                ((2.0 * bufferSizeSamples + readOutputPresentationLatencyFrames (device))
                                 / sampleRate) * 1000.0;
                        }
                    }
                }
            }

            attempt->token.reset();
            {
                const std::lock_guard<std::mutex> guard (attempt->mutex);
                attempt->result = std::move (cap);
                attempt->finished = true;
            }
            attempt->changed.notify_one();
        });
    }
    catch (...)
    {
        ExclusiveModeCapability cap;
        cap.unavailableReason = "macOS couldn't create the worker needed to check this sound output "
                                "safely. Restart SobStage, then try again.";
        return cap;
    }

    std::unique_lock<std::mutex> lock (attempt->mutex);
    if (! attempt->changed.wait_for (lock, kHalTransactionTimeout,
                                     [&attempt] { return attempt->finished; }))
    {
        lock.unlock();
        worker.detach();
        ExclusiveModeCapability cap;
        cap.unavailableReason = "macOS took too long to check this sound output, so SobStage stopped "
                                "waiting. Choose another output or reconnect it.";
        return cap;
    }

    auto cap = std::move (attempt->result);
    lock.unlock();
    worker.join();
    return cap;
}

bool CoreAudioBackend::openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                                   AudioCallback callback, bool isOutput)
{
    // Cleared here so a message from a previous failed open cannot be read back
    // as the reason this one failed -- or, worse, be shown beside a success.
    lastOpenError.clear();
    lastOpenRefusal = OpenRefusal::unknown;

    if (! callback)
    {
        lastOpenError = "SobStage couldn't start this audio path because its audio callback was missing.";
        lastOpenRefusal = OpenRefusal::appBusy;
        return false;
    }

    // Behind a wedged open of another mic. Not that mic's power: one wedged
    // device would otherwise make every mic queued after it count as a second
    // device dropping out, and §14.2 would blame the hub.
    auto transactionToken = beginInputAttempt (pendingInputAttempts, deviceId);
    if (transactionToken == nullptr)
    {
        lastOpenError = "macOS is still finishing an earlier audio-rig operation. Wait a moment, "
                        "then unplug and reconnect the interface before trying again.";
        lastOpenRefusal = OpenRefusal::appBusy;
        return false;
    }

    auto stream = std::make_unique<CoreAudioStream>();
    stream->callback = std::move (callback);
    stream->uid = deviceId;
    stream->expectedSampleRate = sampleRate;
    stream->isOutput = isOutput;

    // A new output starts as the clock. Its listener is installed only after
    // its rate has been set, so a hold raised from here on is about it.
    if (isOutput)
    {
        outputOffRate->store (false);
        stream->outputOffRate = outputOffRate;
    }

    struct Attempt
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool finished = false;
        bool abandoned = false;
        CoreAudioOpenResult result;
        std::unique_ptr<CoreAudioStream> stream;
        std::shared_ptr<PendingInputToken> token;
    };

    auto attempt = std::make_shared<Attempt>();
    attempt->stream = std::move (stream);
    attempt->token = std::move (transactionToken);

    std::thread worker;
    try
    {
        worker = std::thread ([attempt, deviceId, sampleRate, bufferSizeSamples, isOutput]
        {
            CoreAudioOpenResult result;
            try
            {
                const std::lock_guard<std::mutex> halGuard (
                    attempt->token->registry->halTransactions);
                result = prepareAndStartStream (*attempt->stream, deviceId, sampleRate,
                                                bufferSizeSamples, isOutput);
            }
            catch (...)
            {
                // Allocation can fail while preparing scratch storage. It is
                // normally before CreateIOProc, but clean defensively in case a
                // future HAL step that can throw is added after registration.
                const std::lock_guard<std::mutex> halGuard (
                    attempt->token->registry->halTransactions);
                const auto cleanup = stopAndDestroyAbandonedStream (*attempt->stream);
                result = { false,
                           "SobStage couldn't prepare this interface for audio. Close other apps "
                           "using it, then try again.",
                           cleanup.streamSafeToDelete, cleanup.cleanupSucceeded };
            }

            {
                const std::lock_guard<std::mutex> guard (attempt->mutex);
                if (! attempt->abandoned)
                {
                    attempt->result = std::move (result);
                    attempt->finished = true;
                    attempt->changed.notify_one();
                    return;
                }
            }

            // The caller timed out and has already closed/drained the callback
            // gate. If the HAL eventually started, stop it here. If transaction
            // rollback itself was what blocked, createAndStartStream did not
            // return until that rollback was complete. In either case this
            // worker remains the sole owner for the full unsafe lifetime.
            CoreAudioCleanupResult cleanup { result.streamSafeToDelete,
                                             result.cleanupSucceeded };
            if (result.succeeded)
            {
                // closeAllStreams uses the same transaction mutex. Keep late
                // rollback serialized with teardown of streams that were
                // already live when this attempt timed out.
                const std::lock_guard<std::mutex> halGuard (
                    attempt->token->registry->halTransactions);
                cleanup = stopAndDestroyAbandonedStream (*attempt->stream);
            }

            if (! cleanup.cleanupSucceeded)
                holdAudioTeardownQuarantine (attempt->token->registry);

            if (! cleanup.streamSafeToDelete)
                retainInertStream (std::move (attempt->stream));

            attempt->token.reset();
        });
    }
    catch (...)
    {
        lastOpenError = "macOS couldn't create the worker needed to open this audio interface "
                        "safely. Restart SobStage, then try again.";
        lastOpenRefusal = OpenRefusal::appBusy;
        return false;
    }

    std::unique_lock<std::mutex> lock (attempt->mutex);

    // isOutput, not attempt->stream->isOutput. The stream belongs to the worker
    // from the moment the thread starts, and prepareAndStartStream writes that
    // same field under the HAL transaction lock -- a different mutex from this
    // one, so reading it back here was a data race on a live object, which is
    // what ThreadSanitizer stops the build for. The value is already in hand.
    if (! attempt->changed.wait_for (lock,
                                     isOutput ? kOutputHalTransactionTimeout
                                              : kHalTransactionTimeout,
                                     [&attempt] { return attempt->finished; }))
    {
        attempt->abandoned = true;
        auto* timedOutStream = attempt->stream.get();
        lock.unlock();

        // This is the lifetime boundary: whether the worker is blocked in a
        // property call, CreateIOProc or Start, every later callback observes
        // the closed gate and cannot reach the caller's former owner.
        closeCallbackGateAndDrain (*timedOutStream);
        worker.detach();

        lastOpenError = "macOS took too long to connect this interface, so SobStage stopped "
                        "waiting. Unplug it and plug it back in, and close any other app using "
                        "audio before trying again.";
        lastOpenRefusal = OpenRefusal::timedOut;
        return false;
    }

    auto result = std::move (attempt->result);
    lock.unlock();
    worker.join();

    stream = std::move (attempt->stream);

    if (result.bufferSizeWasRefused)
        bufferSizeWasRefused.store (true);

    if (! result.succeeded)
    {
        lastOpenError = std::move (result.error);
        lastOpenRefusal = result.refusal;

        if (! result.cleanupSucceeded)
            holdAudioTeardownQuarantine (pendingInputAttempts);

        if (! result.streamSafeToDelete)
            retainInertStream (std::move (stream));

        attempt->token.reset();
        return false;
    }

    attempt->token.reset();
    openStreams.push_back (std::move (stream));
    return true;
}

std::vector<StreamFailure> CoreAudioBackend::takeStreamFailures()
{
    std::vector<StreamFailure> failures;
    bool devicePropertiesChanged = false;

    // Reported once, and through the same channel as a dead stream, because it
    // is the same kind of news: something about the rig is not what the app
    // asked for and the user cannot see it anywhere else.
    if (bufferSizeWasRefused.exchange (false))
    {
        failures.push_back ({ std::string(),
                              "is running at its own buffer size rather than the one asked for, so "
                              "there is a little more delay than usual.",
                              "Your sound hardware" });
    }

    // How long a started stream may go without its IOProc running before it is
    // called dead. Generous: the HAL can legitimately pause briefly around a
    // device or format change, and a false report here would send someone
    // unplugging hardware that is fine.
    constexpr double kSilentSecondsBeforeGivingUp = 5.0;

    // On the host clock, as every callback stamp is: it stops while the Mac
    // sleeps.
    const double now = hostClockSeconds();

    // Sleep stops every IOProc at once, and the awake seconds around it -- IO
    // stopping before the Mac sleeps, USB devices starting again after it
    // wakes, a dark wake with the lid shut -- are not a device failing. The
    // guard notices the sleep and holds the watchdog until audio runs again,
    // for as long as the Mac says its audio is off.
    bool anyStreamOpen = false;

    {
        double latestCallback = 0.0;

        for (const auto& stream : openStreams)
            if (stream != nullptr && stream->ioProcId != nullptr)
            {
                anyStreamOpen = true;
                latestCallback = std::max (latestCallback,
                                           stream->lastCallbackSeconds.load (std::memory_order_relaxed));
            }

        watchdogSleepGuard.poll (now, sleepCountingSeconds(), latestCallback, systemAudioIsOff);
    }

    // Asked at most once a poll, and only of a stream about to be called dead.
    bool systemAudioAsked = false;

    for (const auto& stream : openStreams)
    {
        if (stream == nullptr || stream->ioProcId == nullptr)
            continue;

        // A listener installation failure is not allowed to masquerade as a
        // fully watched stream. Keep the stream usable for unusual third-party
        // drivers, but tell the user once that these safety signals are absent.
        if (! stream->listenerProblemReported
            && (! stream->nominalRateListenerInstalled
                || ! stream->deviceAliveListenerInstalled
                || ! stream->processorOverloadListenerInstalled))
        {
            stream->listenerProblemReported = true;
            failures.push_back ({ stream->isOutput ? std::string() : stream->uid,
                                  "can't report every sample-rate, disconnect, or audio-overload "
                                  "change from this device. Keep Audio MIDI Setup closed while "
                                  "recording, and restart SobStage after changing the rig.",
                                  stream->isOutput ? "Your sound output" : std::string(),
                                  StreamFailureKind::safetyMonitoringUnavailable });
        }

        if (stream->deviceAliveCheckPending.exchange (false, std::memory_order_relaxed))
        {
            devicePropertiesChanged = true;
            const bool alive = readDeviceIsAlive (stream->deviceId);

            if (! alive)
            {
                if (! stream->deviceUnavailableReported)
                {
                    stream->deviceUnavailableReported = true;

                    // The app silences this mic's channel on the report, and
                    // mid-take only a `resumed` report gives it back. A USB hub
                    // re-settling can drop DeviceIsAlive and raise it again on
                    // the same AudioObjectID; with only the watchdog arming the
                    // resume, that mic recorded silence for the rest of the
                    // take while its audio arrived. Audio after this moment,
                    // once the device is alive again, is the resume.
                    if (! stream->isOutput)
                        stream->deadReportedAt = now;

                    failures.push_back ({ stream->isOutput ? std::string() : stream->uid,
                                          stream->isOutput
                                              ? "is no longer available, so you can't hear through "
                                                "it. Choose another output."
                                              : "is no longer available and has stopped sending "
                                                "audio. Unplug it and plug it back in.",
                                          stream->isOutput ? "Your sound output" : std::string(),
                                          StreamFailureKind::deviceUnavailable });
                }
            }
            else
            {
                // A driver can revive the same AudioObjectID. Let either a
                // later alive=false event or the callback watchdog report a
                // subsequent failure rather than keeping the old latch forever.
                stream->deviceUnavailableReported = false;
                stream->reportedDead.store (false, std::memory_order_relaxed);
            }
        }

        if (stream->nominalRateCheckPending.exchange (false))
        {
            const double actualRate = getNominalSampleRate (stream->deviceId);
            const bool mismatched = actualRate > 0.0
                                 && std::abs (actualRate - stream->expectedSampleRate) >= 1.0;

            // Handed to re-enumeration unless this is the monitor output
            // found at its own rate with no change of its reported: nothing
            // the rig is built from has moved then. That re-enumeration ran
            // a moment after the hold below was lowered, while the software
            // clock was still handing the rig back to the output -- the
            // output looked lost, and the app reopened every stream for it.
            if (! stream->isOutput || mismatched || stream->sampleRateMismatchReported)
                devicePropertiesChanged = true;

            if (stream->outputOffRate != nullptr)
            {
                if (mismatched)
                {
                    stream->outputOffRate->store (true);
                }
                else
                {
                    // Back at the stream's rate (or it never left): the
                    // output may clock the rig again. Unless the device has
                    // said something since the read above -- that is the
                    // next poll's to read, and the hold stands until then.
                    stream->outputOffRate->store (false);

                    if (stream->nominalRateCheckPending.load())
                        stream->outputOffRate->store (true);
                }
            }

            if (mismatched && ! stream->sampleRateMismatchReported)
            {
                stream->sampleRateMismatchReported = true;

                // The output's advice depends on what the app does about it
                // -- carry the take on without it, or put it back -- so the
                // app adds it (outputRateChangeAction()).
                failures.push_back ({ stream->isOutput ? std::string() : stream->uid,
                                      "changed to " + formatRate (actualRate)
                                      + " while SobStage is using "
                                      + formatRate (stream->expectedSampleRate) + "."
                                      + (stream->isOutput
                                             ? std::string()
                                             : " Set the interface back to "
                                               + formatRate (stream->expectedSampleRate)
                                               + ", then start a new take."),
                                      stream->isOutput ? "Your sound output" : std::string(),
                                      StreamFailureKind::sampleRateChanged });
            }
            else if (! mismatched)
            {
                // The headphone output back at the rate it was opened for.
                // The app may have stopped using it as the clock over the
                // change, and this is what tells it it can again.
                if (stream->sampleRateMismatchReported && stream->isOutput)
                    failures.push_back ({ std::string(),
                                          "is back at " + formatRate (stream->expectedSampleRate) + ".",
                                          "Your sound output",
                                          StreamFailureKind::resumed });

                // A later mismatch is new information once the device has
                // returned to the rate this stream was opened for.
                stream->sampleRateMismatchReported = false;
            }
        }

        const auto overloads = stream->processorOverloads.load (std::memory_order_relaxed);

        if (! stream->isOutput && overloads > stream->reportedProcessorOverloads)
        {
            stream->reportedProcessorOverloads = overloads;
            failures.push_back ({ stream->uid,
                                  "missed an audio processing deadline, so part of the recording "
                                  "may not have reached the app. Close other apps using audio and "
                                  "try a larger buffer size.",
                                  {}, StreamFailureKind::processorOverload });
        }

        // A stream the watchdog called dead whose IOProc has run since. The
        // HAL resumes a paused IOProc on the same device -- after system
        // sleep, another app's brief hog-mode grab, a clock re-lock -- and the
        // callback already let go of reportedDead; the app, which silenced the
        // channel, was never told. Said once per death.
        if (! stream->isOutput
            && inputStreamHasResumed (stream->deadReportedAt,
                                      stream->lastCallbackSeconds.load (std::memory_order_relaxed),
                                      stream->deviceUnavailableReported))
        {
            stream->deadReportedAt = 0.0;
            failures.push_back ({ stream->uid, "is sending audio again.", {},
                                  StreamFailureKind::resumed });
        }

        // DeviceIsAlive already supplied the immediate, typed report. The
        // five-second callback watchdog is a fallback for devices/drivers that
        // stop silently; emitting it as well would describe one unplug twice.
        if (stream->deviceUnavailableReported)
            continue;

        // Before the first callback, measure from the successful Start call.
        // Afterward, measure from the most recent callback as before. After a
        // sleep, from no earlier than audio running again; while waiting for
        // that, not at all (0).
        const double silenceBegan = watchdogSleepGuard.silenceBegan (
            stream->lastCallbackSeconds.load (std::memory_order_relaxed), stream->startedSeconds);

        if (silenceBegan <= 0.0 || now - silenceBegan < kSilentSecondsBeforeGivingUp)
            continue;

        if (stream->reportedDead.load (std::memory_order_relaxed))
            continue;

        // The Mac turning its audio off stops every IOProc, and it does that
        // on its way to sleep, which can take longer than these few seconds,
        // before any sleep has been seen. Nothing is dead then: held, as for
        // a sleep, until audio runs again.
        if (! systemAudioAsked)
        {
            systemAudioAsked = true;

            if (systemAudioIsOff())
                watchdogSleepGuard.holdWhileSystemAudioIsOff (now);
        }

        if (watchdogSleepGuard.isWaitingForAudio())
            continue;

        // Once per stream. The condition stays true for as long as the device
        // is dead, and repeating it every poll would bury everything else.
        if (stream->reportedDead.exchange (true, std::memory_order_relaxed))
            continue;

        if (! stream->isOutput)
            stream->deadReportedAt = now;

        failures.push_back ({ stream->isOutput ? std::string() : stream->uid,
                              stream->isOutput
                                  ? "stopped accepting audio, so you can't hear anything through "
                                    "it. Try selecting it again."
                                  : "stopped sending audio. Unplug it and plug it back in." });
    }

    // Holding the watchdog is not silence about the sleep: once per sleep with
    // the rig open, the app hears that every stream stopped together. None of
    // them failed, but a take running across it has a gap there -- the audio
    // from before the sleep runs straight into the audio from after it -- and
    // only the app can put that in the take's record.
    if (watchdogSleepGuard.takeSleepNotice() && anyStreamOpen)
        failures.push_back ({ std::string(),
                              "went to sleep, so every microphone and the headphones stopped until "
                              "it woke.",
                              "The Mac", StreamFailureKind::systemSlept });

    // Re-enumeration is useful for both a rate change and a device becoming
    // alive/dead, but invoking the app callback from the CoreAudio listener
    // itself would allocate from a HAL-owned thread. This method is message-
    // thread-only, so it is the safe bridge back into the normal hotplug path.
    if (devicePropertiesChanged && deviceListListenerState != nullptr)
        deviceListChanged (kAudioObjectSystemObject, 0, nullptr,
                           deviceListListenerState.get());

    return failures;
}

int CoreAudioBackend::getGrantedOutputBufferFrames() const
{
    // Asked of the device rather than remembered from the request. This backend
    // already notices when a device refuses the size it was given -- it tells
    // the user there is "a little more delay than usual" -- and the latency
    // figure printed beside that sentence was still the one for the buffer the
    // device had just refused.
    for (const auto& stream : openStreams)
        if (stream != nullptr && stream->isOutput && stream->deviceId != kAudioObjectUnknown)
            if (const int granted = getBufferFrameSize (stream->deviceId); granted > 0)
                return granted;

    return 0;
}

int CoreAudioBackend::getInputLatencyFrames (const std::string& deviceId) const
{
    for (const auto& stream : openStreams)
        if (stream != nullptr && stream->uid == deviceId)
            return stream->inputLatencyFrames;

    return 0;
}

int CoreAudioBackend::getOutputPresentationLatencyFrames() const
{
    for (const auto& stream : openStreams)
        if (stream != nullptr && stream->isOutput && stream->deviceId != kAudioObjectUnknown)
            return readOutputPresentationLatencyFrames (stream->deviceId);

    return 0;
}

uint64_t CoreAudioBackend::getFramesDroppedByBackend() const
{
    uint64_t total = 0;

    for (const auto& stream : openStreams)
        if (stream != nullptr)
            total += stream->framesDropped.load (std::memory_order_relaxed);

    return total;
}

uint64_t CoreAudioBackend::getOutputGlitchCount() const
{
    uint64_t total = 0;

    for (const auto& stream : openStreams)
        if (stream != nullptr && stream->isOutput)
            total += stream->processorOverloads.load (std::memory_order_relaxed);

    return total;
}

bool CoreAudioBackend::openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                                  int bufferSizeSamples, AudioCallback callback)
{
    {
        const std::lock_guard<std::mutex> guard (pendingInputAttempts->mutex);
        if (pendingInputAttempts->teardownInProgress
            || pendingInputAttempts->cleanupUnsafe)
        {
            lastOpenError = "macOS is still releasing the previous audio rig. Wait a moment, then "
                            "unplug and reconnect the interface before trying again.";
            lastOpenRefusal = OpenRefusal::appBusy;
            return false;
        }
    }

    // UID/property queries, hog mode, IOProc creation and Start are all part of
    // openStream's bounded worker. Keeping even one of them here would leave a
    // broken output driver able to freeze launch before the deadline exists.
    return openStream (outputDeviceId, sampleRate, bufferSizeSamples,
                       std::move (callback), true);
}

bool CoreAudioBackend::openInputStream (const std::string& inputDeviceId, double sampleRate,
                                        int bufferSizeSamples, AudioCallback callback)
{
    // Cleared here, not in openStream: the output path clears it before taking
    // hog mode and sets its own reason on failure, and reporting that message
    // against a microphone would name the wrong device entirely.
    lastOpenError.clear();
    lastOpenRefusal = OpenRefusal::unknown;

    return openStream (inputDeviceId, sampleRate, bufferSizeSamples, std::move (callback), false);
}

void CoreAudioBackend::closeAllStreams()
{
    // No output is open to be held off the clock. Every gate below closes
    // before anything else, so no listener raises it again.
    outputOffRate->store (false);

    if (openStreams.empty())
        return;

    // Close callback admission on the caller before ownership moves. The HAL
    // can now stall in Stop, listener removal or Destroy for as long as it
    // likes without either freezing quit/reconfiguration or reaching the
    // CaptureCoordinator after this function returns.
    for (auto& stream : openStreams)
        closeCallbackGateAndDrain (*stream);

    struct CleanupAttempt
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool finished = false;
        std::vector<std::unique_ptr<CoreAudioStream>> streams;
        std::shared_ptr<CoreAudioPendingInputAttempts> registry;
    };

    std::shared_ptr<CleanupAttempt> attempt;
    try
    {
        attempt = std::make_shared<CleanupAttempt>();
        attempt->registry = pendingInputAttempts;
        attempt->streams = std::move (openStreams);
    }
    catch (...)
    {
        // No HAL call has been made and every gate is already closed. Preserve
        // all clientData forever rather than allowing vector destruction to
        // free an IOProc the driver still owns.
        for (auto& stream : openStreams)
            retainInertStream (std::move (stream));
        openStreams.clear();
        holdAudioTeardownQuarantine (pendingInputAttempts);
        return;
    }

    {
        const std::lock_guard<std::mutex> guard (attempt->registry->mutex);
        attempt->registry->teardownInProgress = true;
    }

    std::thread worker;
    bool workerStarted = false;

   #if defined (MMA_SIMULATE_MAC)
    const bool injectWorkerFailure = std::exchange (failNextCleanupWorkerStart, false);
   #else
    constexpr bool injectWorkerFailure = false;
   #endif

    if (! injectWorkerFailure)
    {
        try
        {
            worker = std::thread ([attempt]
            {
                bool cleanupSucceeded = true;

                {
                    const std::lock_guard<std::mutex> halGuard (
                        attempt->registry->halTransactions);

                    for (auto& stream : attempt->streams)
                    {
                        const auto cleanup = stopAndDestroyAbandonedStream (*stream);
                        cleanupSucceeded = cleanupSucceeded && cleanup.cleanupSucceeded;

                        if (! cleanup.streamSafeToDelete)
                            retainInertStream (std::move (stream));
                    }
                }

                {
                    const std::lock_guard<std::mutex> guard (
                        attempt->registry->mutex);
                    attempt->registry->teardownInProgress = false;
                    if (! cleanupSucceeded)
                        attempt->registry->cleanupUnsafe = true;
                }
                attempt->registry->changed.notify_all();

                {
                    const std::lock_guard<std::mutex> guard (attempt->mutex);
                    attempt->finished = true;
                }
                attempt->changed.notify_one();
            });
            workerStarted = true;
        }
        catch (...)
        {
            workerStarted = false;
        }
    }

    if (! workerStarted)
    {
        // Thread construction can fail under process pressure. The callable's
        // destructor must not own the only references to live clientData, so
        // the streams live in `attempt` until they are explicitly quarantined.
        for (auto& stream : attempt->streams)
            retainInertStream (std::move (stream));
        attempt->streams.clear();

        {
            const std::lock_guard<std::mutex> guard (attempt->registry->mutex);
            attempt->registry->teardownInProgress = false;
            attempt->registry->cleanupUnsafe = true;
        }
        attempt->registry->changed.notify_all();
        return;
    }

    std::unique_lock<std::mutex> lock (attempt->mutex);
    if (! attempt->changed.wait_for (lock, kHalTransactionTimeout,
                                     [&attempt] { return attempt->finished; }))
    {
        lock.unlock();
        worker.detach();
        return;
    }

    lock.unlock();
    worker.join();
}

#if defined (MMA_SIMULATE_MAC)
bool CoreAudioBackend::waitForPendingInputAttemptsForTesting (int timeoutMilliseconds)
{
    std::unique_lock<std::mutex> lock (pendingInputAttempts->mutex);
    return pendingInputAttempts->changed.wait_for (
        lock, std::chrono::milliseconds (timeoutMilliseconds),
        [this]
        {
            return pendingInputAttempts->deviceIds.empty()
                && ! pendingInputAttempts->teardownInProgress;
        });
}
#endif

} // namespace mma

#endif // JUCE_MAC
