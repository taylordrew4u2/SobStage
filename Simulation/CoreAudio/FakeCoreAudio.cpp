#include "FakeCoreAudio.h"

#include <CoreAudio/CoreAudio.h>

#include <unistd.h> // pid_t, and getpid() where the platform has it

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Every CoreFoundation object the shim hands out. Reference-counted the way
/// CF is: created at one, CFRelease drops one, a container retains what it
/// holds. A constant (CFSTR) string is immortal.
struct FakeCFObject
{
    virtual ~FakeCFObject() = default;
    int retainCount = 1;
    bool immortal = false;
};

struct FakeString : FakeCFObject
{
    explicit FakeString (std::string v) : value (std::move (v)) {}
    std::string value;
};

void releaseObject (FakeCFObject* object)
{
    if (object != nullptr && ! object->immortal && --object->retainCount == 0)
        delete object;
}

/// Every opaque CF handle in the shim is a FakeCFObject*, cast through the
/// base so the pointer value is the same whichever handle type carries it.
template <typename Ref>
Ref toRef (FakeCFObject* object) { return reinterpret_cast<Ref> (object); }

const FakeCFObject* fromRef (const void* ref) { return static_cast<const FakeCFObject*> (ref); }

FakeCFObject* retainObject (const void* ref)
{
    auto* object = const_cast<FakeCFObject*> (fromRef (ref));
    if (object != nullptr)
        ++object->retainCount;
    return object;
}

struct FakeArray : FakeCFObject
{
    ~FakeArray() override
    {
        for (auto* value : values)
            releaseObject (value);
    }

    std::vector<FakeCFObject*> values;
};

struct FakeDictionary : FakeCFObject
{
    ~FakeDictionary() override
    {
        for (auto& entry : entries)
            releaseObject (entry.second);
    }

    const FakeCFObject* find (const std::string& key) const
    {
        for (const auto& entry : entries)
            if (entry.first == key)
                return entry.second;
        return nullptr;
    }

    std::vector<std::pair<std::string, FakeCFObject*>> entries;
};

struct FakeNumber : FakeCFObject
{
    int value = 0;
};

std::string stringValue (const FakeCFObject* object)
{
    const auto* s = dynamic_cast<const FakeString*> (object);
    return s == nullptr ? std::string() : s->value;
}

struct IoProcRegistration
{
    AudioDeviceIOProc proc = nullptr;
    void* clientData = nullptr;
    bool running = false;
};

struct Device
{
    fakeca::DeviceSpec spec;

    /// The pid holding hog mode, or -1 for unowned -- the same encoding
    /// CoreAudio uses, so the backend's own comparison against getpid() is
    /// what decides whether the device is ours.
    int hogOwner = -1;

    std::vector<IoProcRegistration> procs;

    bool rateChangePending = false;
    double pendingRate = 0.0;
    int rateReadsRemaining = 0;
    bool rateUnreadable = false;

    int subDeviceReadsUntilActive = 0;

    /// Made through AudioHardwareCreateAggregateDevice: gone after a
    /// coreaudiod restart.
    bool createdAggregate = false;
};

struct Listener
{
    AudioObjectID object;
    AudioObjectPropertyAddress address;
    AudioObjectPropertyListenerProc proc;
    void* clientData;
};

struct State
{
    std::map<AudioObjectID, Device> devices;
    std::vector<AudioObjectID> order;
    std::vector<Listener> listeners;
    AudioObjectID nextId = 100;
    AudioObjectID defaultOutput = kAudioObjectUnknown;
    bool allowPropertyListeners = true;
    bool allowSystemPropertyListenerRemoval = true;
    int aggregatesCreated = 0;
};

State& state()
{
    static State s;
    return s;
}

// The Mac's two clocks. The host clock is real time plus the awake time
// passTime() skipped; the wall clock is that plus every simulated sleep.
// Atomics, and outside State, because the IOProc reads the host clock on
// whichever thread pumps it.
std::atomic<int64_t> skippedAwakeNanos { 0 };
std::atomic<int64_t> sleptNanos { 0 };
std::atomic<int64_t> wallStepNanos { 0 };

// What the power manager says the Mac can do right now (IOPMLibPrivate.h's
// IOPMCapabilityBits), and how often it has been asked.
constexpr uint32_t kFullWakeCapabilities = 0x1 | 0x2 | 0x4 | 0x8 | 0x10; // CPU, video, audio, network, disk
constexpr uint32_t kDarkWakeCapabilities = 0x1 | 0x8 | 0x10;             // CPU, network, disk
std::atomic<uint32_t> systemCapabilities { kFullWakeCapabilities };
std::atomic<int> systemCapabilityQueries { 0 };

int64_t realNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t toNanos (double seconds)
{
    return seconds > 0.0 ? static_cast<int64_t> (std::llround (seconds * 1.0e9)) : 0;
}

/// A device's stream object. Derived rather than registered: CoreAudio hands
/// out opaque AudioObjectIDs and the backend only ever uses one to ask the
/// question below, so a reversible offset models it exactly and keeps the
/// device map the single place a device exists.
constexpr AudioObjectID kStreamIdBase = 900000;

AudioObjectID streamIdFor (AudioObjectID device) { return kStreamIdBase + device; }
bool isStreamId (AudioObjectID object) { return object >= kStreamIdBase; }
AudioObjectID deviceForStream (AudioObjectID stream) { return stream - kStreamIdBase; }

/// Starts held by DeviceSpec::startWaitsForRelease, and the devices let go.
struct StartGate
{
    std::mutex mutex;
    std::condition_variable changed;
    std::set<AudioObjectID> released;
    int generation = 0;
};

StartGate& startGate()
{
    static StartGate gate;
    return gate;
}

Device* find (AudioObjectID id)
{
    auto it = state().devices.find (id);
    return it == state().devices.end() ? nullptr : &it->second;
}

/// An aggregate's sub-devices that are present, in the order it lists them.
std::vector<AudioObjectID> activeSubDevices (const Device& aggregate)
{
    std::vector<AudioObjectID> active;

    for (const auto& uid : aggregate.spec.subDeviceUids)
        for (const auto id : state().order)
            if (state().devices.at (id).spec.uid == uid)
                active.push_back (id);

    return active;
}

/// Copies `bytes` from `source` into the caller's buffer, honouring CoreAudio's
/// ioSize convention: the caller states its capacity, the HAL writes what it
/// actually produced. Under-reporting here is how a real HAL truncates.
OSStatus deliver (const void* source, UInt32 bytes, UInt32* ioSize, void* outData)
{
    if (ioSize == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (outData == nullptr)
    {
        *ioSize = bytes;
        return noErr;
    }

    const UInt32 toCopy = std::min (bytes, *ioSize);
    std::memcpy (outData, source, toCopy);
    *ioSize = toCopy;
    return noErr;
}

/// Builds the buffer list for one callback in the device's configured shape and
/// keeps the backing storage alive for the duration of the call.
struct BufferListStorage
{
    std::vector<char> listBytes;
    std::vector<std::vector<float>> blocks;

    AudioBufferList* build (int channels, int frames, fakeca::BufferShape shape)
    {
        const int bufferCount = (shape == fakeca::BufferShape::interleaved) ? 1 : channels;
        const int channelsPerBuffer = (shape == fakeca::BufferShape::interleaved) ? channels : 1;

        listBytes.assign (sizeof (AudioBufferList)
                          + sizeof (AudioBuffer) * static_cast<size_t> (std::max (0, bufferCount - 1)), 0);
        blocks.assign (static_cast<size_t> (bufferCount),
                       std::vector<float> (static_cast<size_t> (channelsPerBuffer) * frames, 0.0f));

        auto* list = reinterpret_cast<AudioBufferList*> (listBytes.data());
        list->mNumberBuffers = static_cast<UInt32> (bufferCount);

        for (int i = 0; i < bufferCount; ++i)
        {
            list->mBuffers[i].mNumberChannels = static_cast<UInt32> (channelsPerBuffer);
            list->mBuffers[i].mDataByteSize =
                static_cast<UInt32> (blocks[static_cast<size_t> (i)].size() * sizeof (float));
            list->mBuffers[i].mData = blocks[static_cast<size_t> (i)].data();
        }

        return list;
    }
};

void fireDeviceListListeners()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };

    // Copied first: a listener is allowed to add or remove listeners.
    const auto snapshot = state().listeners;

    for (const auto& l : snapshot)
        if (l.object == kAudioObjectSystemObject
            && l.address.mSelector == kAudioHardwarePropertyDevices)
            l.proc (kAudioObjectSystemObject, 1, &address, l.clientData);
}

void firePropertyListeners (AudioObjectID object, AudioObjectPropertySelector selector,
                            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    AudioObjectPropertyAddress address { selector,
                                         scope,
                                         kAudioObjectPropertyElementMain };

    // A listener may remove itself while it runs, so traverse a snapshot just
    // like the system device-list notification path above.
    const auto snapshot = state().listeners;

    // Scope and element are matched here because AudioObjectRemovePropertyListener
    // below matches them. While delivery ignored them the two halves disagreed,
    // and a registration added under one scope and removed under another would
    // silently leak -- the removal a no-op, the listener still firing, and
    // nothing able to tell. A real HAL matches on both sides; so does this now.
    for (const auto& l : snapshot)
        if (l.object == object
            && l.address.mSelector == selector
            && l.address.mScope == address.mScope
            && l.address.mElement == address.mElement)
            l.proc (object, 1, &address, l.clientData);
}

} // namespace

// --- CoreFoundation ---------------------------------------------------------

const CFArrayCallBacks kCFTypeArrayCallBacks { 0 };
const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks { 0 };
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks { 0 };

Boolean CFStringGetCString (CFStringRef value, char* buffer, long bufferSize, CFStringEncoding)
{
    if (value == nullptr || buffer == nullptr || bufferSize <= 0)
        return 0;

    const auto* s = dynamic_cast<const FakeString*> (fromRef (value));
    if (s == nullptr)
        return 0;

    const auto length = std::min (s->value.size(), static_cast<size_t> (bufferSize - 1));
    std::memcpy (buffer, s->value.data(), length);
    buffer[length] = '\0';
    return 1;
}

CFStringRef CFStringCreateWithCString (CFAllocatorRef, const char* text, CFStringEncoding)
{
    if (text == nullptr)
        return nullptr;

    return toRef<CFStringRef> (new FakeString (text));
}

CFStringRef mmaFakeConstantString (const char* text)
{
    // Interned and never released, like a real CFSTR constant. Owned here so
    // a leak checker sees them freed at exit rather than lost.
    static std::map<std::string, std::unique_ptr<FakeString>> constants;

    auto& slot = constants[text];
    if (slot == nullptr)
    {
        slot = std::make_unique<FakeString> (text);
        slot->immortal = true;
    }

    return toRef<CFStringRef> (slot.get());
}

CFMutableArrayRef CFArrayCreateMutable (CFAllocatorRef, CFIndex, const CFArrayCallBacks*)
{
    return toRef<CFMutableArrayRef> (new FakeArray());
}

void CFArrayAppendValue (CFMutableArrayRef array, const void* value)
{
    auto* a = dynamic_cast<FakeArray*> (const_cast<FakeCFObject*> (fromRef (array)));
    if (a != nullptr && value != nullptr)
        a->values.push_back (retainObject (value));
}

CFMutableDictionaryRef CFDictionaryCreateMutable (CFAllocatorRef, CFIndex,
                                                  const CFDictionaryKeyCallBacks*,
                                                  const CFDictionaryValueCallBacks*)
{
    return toRef<CFMutableDictionaryRef> (new FakeDictionary());
}

void CFDictionarySetValue (CFMutableDictionaryRef dictionary, const void* key, const void* value)
{
    auto* d = dynamic_cast<FakeDictionary*> (const_cast<FakeCFObject*> (fromRef (dictionary)));
    if (d == nullptr || key == nullptr || value == nullptr)
        return;

    const auto name = stringValue (fromRef (key));
    auto* retained = retainObject (value);

    for (auto& entry : d->entries)
        if (entry.first == name)
        {
            releaseObject (entry.second);
            entry.second = retained;
            return;
        }

    d->entries.emplace_back (name, retained);
}

CFNumberRef CFNumberCreate (CFAllocatorRef, CFNumberType type, const void* valuePtr)
{
    if (type != kCFNumberIntType || valuePtr == nullptr)
        return nullptr;

    auto* n = new FakeNumber();
    std::memcpy (&n->value, valuePtr, sizeof (int));
    return toRef<CFNumberRef> (n);
}

CFAbsoluteTime CFAbsoluteTimeGetCurrent()
{
    // Seconds since 2001 for a Mac today. Being nowhere near the host clock's
    // values also makes a mix-up between the two clocks fail loudly.
    constexpr double kSecondsSince2001 = 8.0e8;

    return kSecondsSince2001
         + static_cast<double> (realNanos() + skippedAwakeNanos.load (std::memory_order_relaxed)
                                + sleptNanos.load (std::memory_order_relaxed)
                                + wallStepNanos.load (std::memory_order_relaxed)) * 1.0e-9;
}

uint64_t clock_gettime_nsec_np (clockid_t)
{
    return static_cast<uint64_t> (realNanos() + skippedAwakeNanos.load (std::memory_order_relaxed)
                                  + sleptNanos.load (std::memory_order_relaxed));
}

UInt64 AudioGetCurrentHostTime()
{
    return static_cast<UInt64> (realNanos() + skippedAwakeNanos.load (std::memory_order_relaxed));
}

UInt64 AudioConvertHostTimeToNanos (UInt64 hostTime)
{
    return hostTime;
}

extern "C" uint32_t IOPMConnectionGetSystemCapabilities (void)
{
    systemCapabilityQueries.fetch_add (1, std::memory_order_relaxed);
    return systemCapabilities.load (std::memory_order_relaxed);
}

void CFRelease (CFTypeRef value)
{
    releaseObject (const_cast<FakeCFObject*> (fromRef (value)));
}

// --- Properties -------------------------------------------------------------

OSStatus AudioObjectGetPropertyDataSize (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         UInt32, const void*, UInt32* outSize)
{
    if (address == nullptr || outSize == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (object == kAudioObjectSystemObject && address->mSelector == kAudioHardwarePropertyDevices)
    {
        *outSize = static_cast<UInt32> (state().order.size() * sizeof (AudioObjectID));
        return noErr;
    }

    if (isStreamId (object) && address->mSelector == kAudioStreamPropertyAvailablePhysicalFormats)
    {
        auto* owner = find (deviceForStream (object));
        if (owner == nullptr)
            return kAudioHardwareBadObjectError;

        *outSize = static_cast<UInt32> (owner->spec.bitDepths.size()
                                        * sizeof (AudioStreamRangedDescription));
        return noErr;
    }

    auto* device = find (object);
    if (device == nullptr)
        return kAudioHardwareBadObjectError;

    if (address->mSelector == kAudioDevicePropertyStreams)
    {
        *outSize = static_cast<UInt32> (device->spec.bitDepths.empty()
                                            ? 0 : sizeof (AudioObjectID));
        return noErr;
    }

    if (address->mSelector == kAudioDevicePropertyStreamConfiguration)
    {
        const int channels = (address->mScope == kAudioObjectPropertyScopeInput)
                           ? device->spec.inputChannels : device->spec.outputChannels;

        if (channels <= 0)
        {
            // A real HAL reports a valid, empty buffer list rather than zero
            // bytes; the backend treats either as "no channels on this scope".
            *outSize = sizeof (AudioBufferList) - sizeof (AudioBuffer);
            return noErr;
        }

        const int bufferCount = (device->spec.shape == fakeca::BufferShape::interleaved) ? 1 : channels;
        *outSize = static_cast<UInt32> (sizeof (AudioBufferList)
                                        + sizeof (AudioBuffer) * static_cast<size_t> (bufferCount - 1));
        return noErr;
    }

    if (address->mSelector == kAudioDevicePropertyAvailableNominalSampleRates)
    {
        *outSize = static_cast<UInt32> (device->spec.rateRanges.size() * sizeof (AudioValueRange));
        return noErr;
    }

    if (address->mSelector == kAudioAggregateDevicePropertyActiveSubDeviceList)
    {
        if (device->spec.subDeviceUids.empty())
            return kAudioHardwareUnknownPropertyError;

        if (device->subDeviceReadsUntilActive > 0)
        {
            --device->subDeviceReadsUntilActive;
            *outSize = 0;
            return noErr;
        }

        *outSize = static_cast<UInt32> (activeSubDevices (*device).size() * sizeof (AudioObjectID));
        return noErr;
    }

    return kAudioHardwareUnknownPropertyError;
}

OSStatus AudioObjectGetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32, const void*, UInt32* ioSize, void* outData)
{
    if (address == nullptr || ioSize == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (isStreamId (object) && address->mSelector == kAudioStreamPropertyAvailablePhysicalFormats)
    {
        auto* owner = find (deviceForStream (object));
        if (owner == nullptr)
            return kAudioHardwareBadObjectError;

        std::vector<AudioStreamRangedDescription> formats;

        for (const int depth : owner->spec.bitDepths)
        {
            AudioStreamRangedDescription described {};
            described.mFormat.mSampleRate = owner->spec.currentRate;
            described.mFormat.mFormatID = kAudioFormatLinearPCM;
            described.mFormat.mBitsPerChannel = static_cast<UInt32> (depth);
            described.mFormat.mChannelsPerFrame =
                static_cast<UInt32> (std::max (1, owner->spec.inputChannels));
            described.mSampleRateRange = { owner->spec.currentRate, owner->spec.currentRate };
            formats.push_back (described);
        }

        return deliver (formats.data(),
                        static_cast<UInt32> (formats.size() * sizeof (AudioStreamRangedDescription)),
                        ioSize, outData);
    }

    if (isStreamId (object) && address->mSelector == kAudioStreamPropertyLatency)
    {
        auto* owner = find (deviceForStream (object));
        if (owner == nullptr)
            return kAudioHardwareBadObjectError;

        const UInt32 frames = static_cast<UInt32> (std::max (0, owner->spec.streamLatencyFrames));
        return deliver (&frames, sizeof (frames), ioSize, outData);
    }

    if (object == kAudioObjectSystemObject
        && address->mSelector == kAudioHardwarePropertyDefaultOutputDevice)
    {
        const AudioObjectID output = state().defaultOutput;
        return deliver (&output, sizeof (output), ioSize, outData);
    }

    if (object == kAudioObjectSystemObject && address->mSelector == kAudioHardwarePropertyDevices)
        return deliver (state().order.data(),
                        static_cast<UInt32> (state().order.size() * sizeof (AudioObjectID)),
                        ioSize, outData);

    auto* device = find (object);
    if (device == nullptr)
        return kAudioHardwareBadObjectError;

    switch (address->mSelector)
    {
        case kAudioDevicePropertyTransportType:
        {
            const UInt32 transport = device->spec.transportType;
            return deliver (&transport, sizeof (transport), ioSize, outData);
        }

        case kAudioDevicePropertyDataSource:
        {
            // Only the output scope has one, and only on devices that report
            // it; everything else answers the way a real HAL does.
            if (address->mScope != kAudioObjectPropertyScopeOutput
                || device->spec.outputDataSource == 0)
                return kAudioHardwareUnknownPropertyError;

            const UInt32 source = device->spec.outputDataSource;
            return deliver (&source, sizeof (source), ioSize, outData);
        }

        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        {
            if (address->mScope != kAudioObjectPropertyScopeInput || device->spec.inputChannels <= 0)
                return kAudioHardwareUnknownPropertyError;

            const UInt32 frames = static_cast<UInt32> (std::max (0,
                address->mSelector == kAudioDevicePropertyLatency ? device->spec.inputLatencyFrames
                                                                  : device->spec.inputSafetyOffsetFrames));
            return deliver (&frames, sizeof (frames), ioSize, outData);
        }

        case kAudioDevicePropertyJackIsConnected:
        {
            if (address->mScope != kAudioObjectPropertyScopeOutput
                || device->spec.jackConnected < 0)
                return kAudioHardwareUnknownPropertyError;

            const UInt32 connected = device->spec.jackConnected > 0 ? 1u : 0u;
            return deliver (&connected, sizeof (connected), ioSize, outData);
        }

        case kAudioObjectPropertyName:
        case kAudioDevicePropertyDeviceUID:
        {
            if (address->mSelector == kAudioDevicePropertyDeviceUID
                && device->spec.uidReadDelayMilliseconds > 0)
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (
                    device->spec.uidReadDelayMilliseconds));
            }

            auto* handle = new FakeString (address->mSelector == kAudioObjectPropertyName
                                               ? device->spec.name : device->spec.uid);
            auto ref = toRef<CFStringRef> (handle);
            const auto status = deliver (&ref, sizeof (ref), ioSize, outData);

            if (outData == nullptr || status != noErr)
                delete handle; // nothing took ownership

            return status;
        }

        case kAudioDevicePropertyStreamConfiguration:
        {
            const int channels = (address->mScope == kAudioObjectPropertyScopeInput)
                               ? device->spec.inputChannels : device->spec.outputChannels;

            BufferListStorage storage;

            if (channels <= 0)
            {
                AudioBufferList empty {};
                empty.mNumberBuffers = 0;
                return deliver (&empty, sizeof (AudioBufferList) - sizeof (AudioBuffer), ioSize, outData);
            }

            // Frame count is irrelevant to a configuration query; only the
            // channel-per-buffer split is being reported.
            auto* list = storage.build (channels, 1, device->spec.shape);
            const auto bytes = static_cast<UInt32> (storage.listBytes.size());
            return deliver (list, bytes, ioSize, outData);
        }

        case kAudioDevicePropertyStreams:
        {
            // One stream per device is enough to answer §2.3: the backend asks
            // a stream for its formats, and a device whose inputs all share a
            // format set needs no more than one to be asked. A device with no
            // depths to report has no stream at all, which is how CoreAudio
            // presents a device that cannot be queried.
            if (device->spec.bitDepths.empty())
                return deliver (nullptr, 0, ioSize, outData);

            const AudioObjectID stream = streamIdFor (object);
            return deliver (&stream, static_cast<UInt32> (sizeof (stream)), ioSize, outData);
        }

        case kAudioDevicePropertyAvailableNominalSampleRates:
        {
            std::vector<AudioValueRange> ranges;
            for (const auto& r : device->spec.rateRanges)
                ranges.push_back ({ r.first, r.second });

            return deliver (ranges.data(),
                            static_cast<UInt32> (ranges.size() * sizeof (AudioValueRange)),
                            ioSize, outData);
        }

        case kAudioDevicePropertyNominalSampleRate:
        {
            if (device->rateUnreadable)
                return kAudioHardwareBadObjectError;

            if (device->rateChangePending)
            {
                if (device->rateReadsRemaining > 0)
                    --device->rateReadsRemaining;
                else
                {
                    device->spec.currentRate = device->pendingRate;
                    device->rateChangePending = false;
                }
            }

            const Float64 rate = device->spec.currentRate;
            return deliver (&rate, sizeof (rate), ioSize, outData);
        }

        case kAudioDevicePropertyDeviceIsAlive:
        {
            const UInt32 alive = device->spec.isAlive ? 1u : 0u;
            return deliver (&alive, sizeof (alive), ioSize, outData);
        }

        case kAudioDevicePropertyBufferFrameSize:
        {
            const UInt32 frames = static_cast<UInt32> (device->spec.bufferFrameSize);
            return deliver (&frames, sizeof (frames), ioSize, outData);
        }

        case kAudioDevicePropertyHogMode:
        {
            // -1 means unowned. A device the harness marked unavailable reports
            // a foreign pid, which is what another app holding it looks like.
            // A device the harness marked unavailable reports a foreign pid,
            // which is what another app holding it looks like.
            constexpr int kSomeOtherProcess = 9999;
            const int owner = device->hogOwner != -1
                            ? device->hogOwner
                            : (device->spec.allowHogMode ? -1 : kSomeOtherProcess);
            return deliver (&owner, sizeof (owner), ioSize, outData);
        }

        case kAudioAggregateDevicePropertyActiveSubDeviceList:
        {
            if (device->spec.subDeviceUids.empty())
                return kAudioHardwareUnknownPropertyError;

            const auto active = activeSubDevices (*device);
            return deliver (active.data(), static_cast<UInt32> (active.size() * sizeof (AudioObjectID)),
                            ioSize, outData);
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus AudioObjectSetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32, const void*, UInt32 inSize, const void* inData)
{
    auto* device = find (object);
    if (device == nullptr || address == nullptr || inData == nullptr)
        return kAudioHardwareBadObjectError;

    switch (address->mSelector)
    {
        case kAudioDevicePropertyNominalSampleRate:
        {
            if (inSize < sizeof (Float64))
                return kAudioHardwareUnspecifiedError;

            Float64 requested;
            std::memcpy (&requested, inData, sizeof (requested));

            if (! device->spec.allowRateChange)
                return kAudioHardwareUnspecifiedError;

            const bool supported = std::any_of (device->spec.rateRanges.begin(),
                                                device->spec.rateRanges.end(),
                                                [requested] (const std::pair<double, double>& r)
                                                { return requested >= r.first && requested <= r.second; });

            if (! supported)
                return kAudioHardwareUnspecifiedError;

            if (device->spec.unpluggedDuringRateChange)
            {
                device->spec.isAlive = false;
                device->rateUnreadable = true;
                return noErr;
            }

            if (device->spec.rateChangeDelayReads > 0)
            {
                device->rateChangePending = true;
                device->pendingRate = requested;
                device->rateReadsRemaining = device->spec.rateChangeDelayReads;
            }
            else
            {
                device->spec.currentRate = requested;
                device->rateChangePending = false;
            }

            return noErr;
        }

        case kAudioDevicePropertyBufferFrameSize:
        {
            if (inSize < sizeof (UInt32))
                return kAudioHardwareUnspecifiedError;

            UInt32 frames;
            std::memcpy (&frames, inData, sizeof (frames));

            if (! device->spec.allowBufferSizeChange)
                return kAudioHardwareUnspecifiedError;

            device->spec.bufferFrameSize = static_cast<int> (frames);
            return noErr;
        }

        case kAudioDevicePropertyHogMode:
        {
            int owner;
            std::memcpy (&owner, inData, sizeof (owner));

            if (owner == -1)
            {
                device->hogOwner = -1;
                return noErr;
            }

            if (! device->spec.allowHogMode)
                return kAudioHardwareUnspecifiedError;

            device->hogOwner = owner;
            return noErr;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

Boolean AudioObjectHasProperty (AudioObjectID object, const AudioObjectPropertyAddress* address)
{
    if (address == nullptr)
        return 0;

    if (object == kAudioObjectSystemObject)
        return (address->mSelector == kAudioHardwarePropertyDevices
                || address->mSelector == kAudioHardwarePropertyDefaultOutputDevice
                || address->mSelector == kAudioHardwarePropertyServiceRestarted) ? 1 : 0;

    auto* device = find (object);
    if (device == nullptr)
        return 0;

    // The two route properties exist only where the device reports them, and
    // only on the output scope -- the case the backend has to tell apart.
    switch (address->mSelector)
    {
        case kAudioDevicePropertyDataSource:
            return (address->mScope == kAudioObjectPropertyScopeOutput
                    && device->spec.outputDataSource != 0) ? 1 : 0;

        case kAudioDevicePropertyJackIsConnected:
            return (address->mScope == kAudioObjectPropertyScopeOutput
                    && device->spec.jackConnected >= 0) ? 1 : 0;

        // Every aggregate has the list, filled in yet or not. Asking whether
        // it exists is not a read of it, so it does not use up one of the
        // reads a slow-to-build aggregate answers empty.
        case kAudioAggregateDevicePropertyActiveSubDeviceList:
            return device->spec.subDeviceUids.empty() ? 0 : 1;

        default:
        {
            UInt32 size = 0;
            return AudioObjectGetPropertyDataSize (object, address, 0, nullptr, &size) == noErr ? 1 : 0;
        }
    }
}

OSStatus AudioObjectAddPropertyListener (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         AudioObjectPropertyListenerProc listener,
                                         void* clientData)
{
    if (address == nullptr || listener == nullptr)
        return kAudioHardwareUnspecifiedError;

    // macOS can refuse it, and the backend has to notice: with no listener,
    // nothing about the rig is ever heard again.
    if (! state().allowPropertyListeners)
        return kAudioHardwareUnspecifiedError;

    state().listeners.push_back ({ object, *address, listener, clientData });
    return noErr;
}

OSStatus AudioObjectRemovePropertyListener (AudioObjectID object,
                                            const AudioObjectPropertyAddress* address,
                                            AudioObjectPropertyListenerProc listener,
                                            void* clientData)
{
    if (address == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (object == kAudioObjectSystemObject
        && ! state().allowSystemPropertyListenerRemoval)
        return kAudioHardwareUnspecifiedError;

    auto* device = find (object);
    if (device != nullptr && ! device->spec.allowPropertyListenerRemoval)
        return kAudioHardwareUnspecifiedError;

    // An unplugged device's object is gone, and the real HAL reports that
    // rather than pretending the removal happened.
    if (device == nullptr && object != kAudioObjectSystemObject)
        return kAudioHardwareBadObjectError;

    auto& listeners = state().listeners;
    listeners.erase (std::remove_if (listeners.begin(), listeners.end(),
                                     [&] (const Listener& l)
                                     {
                                         return l.object == object
                                             && l.address.mSelector == address->mSelector
                                             && l.address.mScope == address->mScope
                                             && l.address.mElement == address->mElement
                                             && l.proc == listener
                                             && l.clientData == clientData;
                                     }),
                     listeners.end());
    return noErr;
}

// --- IOProcs ----------------------------------------------------------------

OSStatus AudioDeviceCreateIOProcID (AudioObjectID device, AudioDeviceIOProc proc,
                                    void* clientData, AudioDeviceIOProcID* outProcId)
{
    auto* d = find (device);
    if (d == nullptr || proc == nullptr || outProcId == nullptr)
        return kAudioHardwareBadObjectError;

    if (d->spec.createDelayMilliseconds > 0)
        std::this_thread::sleep_for (
            std::chrono::milliseconds (d->spec.createDelayMilliseconds));

    d->procs.push_back ({ proc, clientData, false });
    *outProcId = proc;
    return noErr;
}

OSStatus AudioDeviceDestroyIOProcID (AudioObjectID device, AudioDeviceIOProcID procId)
{
    auto* d = find (device);
    if (d == nullptr)
        return kAudioHardwareBadObjectError;

    if (d->spec.destroyDelayMilliseconds > 0)
        std::this_thread::sleep_for (
            std::chrono::milliseconds (d->spec.destroyDelayMilliseconds));

    auto& procs = d->procs;
    procs.erase (std::remove_if (procs.begin(), procs.end(),
                                 [procId] (const IoProcRegistration& r) { return r.proc == procId; }),
                 procs.end());
    return noErr;
}

OSStatus AudioDeviceStart (AudioObjectID device, AudioDeviceIOProcID procId)
{
    auto* d = find (device);
    if (d == nullptr)
        return kAudioHardwareBadObjectError;

    if (d->spec.startDelayMilliseconds > 0)
        std::this_thread::sleep_for (
            std::chrono::milliseconds (d->spec.startDelayMilliseconds));

    if (d->spec.startWaitsForRelease)
    {
        auto& gate = startGate();
        std::unique_lock<std::mutex> lock (gate.mutex);
        const auto generation = gate.generation;
        gate.changed.wait_for (lock, std::chrono::seconds (10), [&]
        {
            return gate.released.count (device) > 0 || gate.generation != generation;
        });
    }

    for (auto& r : d->procs)
        if (r.proc == procId)
        {
            r.running = true;

            if (d->spec.callbackBeforeStartReturns && d->spec.inputChannels > 0)
            {
                float sample = 0.25f;
                AudioBufferList input {};
                input.mNumberBuffers = 1;
                input.mBuffers[0].mNumberChannels = 1;
                input.mBuffers[0].mDataByteSize = sizeof (sample);
                input.mBuffers[0].mData = &sample;
                AudioTimeStamp now {};
                r.proc (device, &now, &input, &now, nullptr, &now, r.clientData);
            }

            return noErr;
        }

    return kAudioHardwareBadObjectError;
}

OSStatus AudioDeviceStop (AudioObjectID device, AudioDeviceIOProcID procId)
{
    auto* d = find (device);
    if (d == nullptr)
        return kAudioHardwareBadObjectError;

    if (d->spec.stopDelayMilliseconds > 0)
        std::this_thread::sleep_for (
            std::chrono::milliseconds (d->spec.stopDelayMilliseconds));

    for (auto& r : d->procs)
        if (r.proc == procId)
            r.running = false;

    return noErr;
}

// --- Aggregate devices ------------------------------------------------------

OSStatus AudioHardwareCreateAggregateDevice (CFDictionaryRef description, AudioObjectID* outDevice)
{
    const auto* d = dynamic_cast<const FakeDictionary*> (fromRef (description));
    if (d == nullptr || outDevice == nullptr)
        return kAudioHardwareUnspecifiedError;

    fakeca::DeviceSpec spec;
    spec.uid = stringValue (d->find (kAudioAggregateDeviceUIDKey));
    spec.name = stringValue (d->find (kAudioAggregateDeviceNameKey));
    spec.transportType = kAudioDeviceTransportTypeAggregate;

    if (spec.uid.empty())
        return kAudioHardwareUnspecifiedError;

    // A device already holding the UID refuses the create -- what a leftover
    // from a crashed run, or one a restarted coreaudiod kept, looks like.
    for (const auto& entry : state().devices)
        if (entry.second.spec.uid == spec.uid)
            return kAudioHardwareUnspecifiedError;

    if (const auto* subs = dynamic_cast<const FakeArray*> (d->find (kAudioAggregateDeviceSubDeviceListKey)))
        for (const auto* sub : subs->values)
            if (const auto* subDict = dynamic_cast<const FakeDictionary*> (sub))
                spec.subDeviceUids.push_back (stringValue (subDict->find (kAudioSubDeviceUIDKey)));

    // Channels are the present sub-devices' channels, in order.
    for (const auto& uid : spec.subDeviceUids)
        for (const auto& entry : state().devices)
            if (entry.second.spec.uid == uid)
            {
                spec.inputChannels += entry.second.spec.inputChannels;
                spec.outputChannels += entry.second.spec.outputChannels;
            }

    const AudioObjectID id = fakeca::addDevice (spec);
    state().devices.at (id).createdAggregate = true;
    ++state().aggregatesCreated;
    *outDevice = id;
    return noErr;
}

OSStatus AudioHardwareDestroyAggregateDevice (AudioObjectID device)
{
    // Any aggregate: one restored by a restarted coreaudiod, or left by an
    // earlier run, was not made through this process's create.
    auto* d = find (device);
    if (d == nullptr || d->spec.transportType != kAudioDeviceTransportTypeAggregate)
        return kAudioHardwareBadObjectError;

    fakeca::removeDevice (device);
    return noErr;
}

// --- Harness control --------------------------------------------------------

namespace fakeca {

void reset()
{
    {
        auto& gate = startGate();
        const std::lock_guard<std::mutex> lock (gate.mutex);
        gate.released.clear();
        ++gate.generation;
    }
    startGate().changed.notify_all();

    state().devices.clear();
    state().order.clear();
    state().listeners.clear();
    state().nextId = 100;
    state().defaultOutput = kAudioObjectUnknown;
    state().allowPropertyListeners = true;
    state().allowSystemPropertyListenerRemoval = true;
    state().aggregatesCreated = 0;
    systemCapabilities.store (kFullWakeCapabilities, std::memory_order_relaxed);
    systemCapabilityQueries.store (0, std::memory_order_relaxed);
}

void releaseStart (AudioObjectID device)
{
    {
        auto& gate = startGate();
        const std::lock_guard<std::mutex> lock (gate.mutex);
        gate.released.insert (device);
    }
    startGate().changed.notify_all();
}

void stepWallClock (double seconds)
{
    wallStepNanos.fetch_add (toNanos (seconds), std::memory_order_relaxed);
}

void passTime (double seconds)
{
    skippedAwakeNanos.fetch_add (toNanos (seconds), std::memory_order_relaxed);
}

void sleepSystem (double seconds)
{
    sleptNanos.fetch_add (toNanos (seconds), std::memory_order_relaxed);
}

void setSystemAudioOn (bool on)
{
    systemCapabilities.store (on ? kFullWakeCapabilities : kDarkWakeCapabilities,
                              std::memory_order_relaxed);
}

int systemCapabilityQueryCount()
{
    return systemCapabilityQueries.load (std::memory_order_relaxed);
}

void setPropertyListenersAllowed (bool allowed)
{
    state().allowPropertyListeners = allowed;
}

void setSystemPropertyListenerRemovalAllowed (bool allowed)
{
    state().allowSystemPropertyListenerRemoval = allowed;
}

void fireDeviceListChange()
{
    fireDeviceListListeners();
}

int systemPropertyListenerCount()
{
    return static_cast<int> (std::count_if (
        state().listeners.begin(), state().listeners.end(),
        [] (const Listener& listener)
        {
            return listener.object == kAudioObjectSystemObject
                && listener.address.mSelector == kAudioHardwarePropertyDevices;
        }));
}

AudioObjectID addDevice (const DeviceSpec& spec)
{
    const AudioObjectID id = state().nextId++;
    state().devices[id] = Device { spec, -1, {} };
    state().devices[id].subDeviceReadsUntilActive = spec.subDeviceActivationDelayReads;
    state().order.push_back (id);
    fireDeviceListListeners();
    return id;
}

void setDefaultOutputDevice (AudioObjectID device)
{
    state().defaultOutput = device;
}

void removeDevice (AudioObjectID device)
{
    state().devices.erase (device);
    auto& order = state().order;
    order.erase (std::remove (order.begin(), order.end(), device), order.end());
    fireDeviceListListeners();
}

bool setNominalRateExternally (AudioObjectID device, double sampleRate)
{
    auto* d = find (device);
    if (d == nullptr)
        return false;

    d->spec.currentRate = sampleRate;
    d->rateChangePending = false;
    firePropertyListeners (device, kAudioDevicePropertyNominalSampleRate);
    return true;
}

bool setDeviceAlive (AudioObjectID device, bool alive)
{
    auto* d = find (device);
    if (d == nullptr)
        return false;

    d->spec.isAlive = alive;
    firePropertyListeners (device, kAudioDevicePropertyDeviceIsAlive);
    return true;
}

bool setOutputDataSource (AudioObjectID device, UInt32 dataSource)
{
    auto* d = find (device);
    if (d == nullptr)
        return false;

    d->spec.outputDataSource = dataSource;
    firePropertyListeners (device, kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput);
    return true;
}

bool setJackConnected (AudioObjectID device, bool connected)
{
    auto* d = find (device);
    if (d == nullptr)
        return false;

    d->spec.jackConnected = connected ? 1 : 0;
    firePropertyListeners (device, kAudioDevicePropertyJackIsConnected, kAudioObjectPropertyScopeOutput);
    return true;
}

bool activateSubDevices (AudioObjectID aggregate)
{
    auto* d = find (aggregate);
    if (d == nullptr || d->spec.subDeviceUids.empty())
        return false;

    d->subDeviceReadsUntilActive = 0;
    firePropertyListeners (aggregate, kAudioAggregateDevicePropertyActiveSubDeviceList);
    firePropertyListeners (aggregate, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    return true;
}

void restartService()
{
    std::vector<AudioObjectID> aggregates;
    for (const auto& entry : state().devices)
        if (entry.second.createdAggregate)
            aggregates.push_back (entry.first);

    for (const auto id : aggregates)
    {
        state().devices.erase (id);
        auto& order = state().order;
        order.erase (std::remove (order.begin(), order.end(), id), order.end());
    }

    // Listeners registered on devices with the old service are not carried
    // over, even for a device that kept its AudioObjectID. System-object
    // listeners are what tell the client the restart happened.
    auto& listeners = state().listeners;
    listeners.erase (std::remove_if (listeners.begin(), listeners.end(),
                                     [] (const Listener& l) { return l.object != kAudioObjectSystemObject; }),
                     listeners.end());

    firePropertyListeners (kAudioObjectSystemObject, kAudioHardwarePropertyServiceRestarted);
}

int serviceRestartListenerCount()
{
    return static_cast<int> (std::count_if (
        state().listeners.begin(), state().listeners.end(),
        [] (const Listener& listener)
        {
            return listener.object == kAudioObjectSystemObject
                && listener.address.mSelector == kAudioHardwarePropertyServiceRestarted;
        }));
}

int aggregatesCreated()
{
    return state().aggregatesCreated;
}

bool fireProcessorOverload (AudioObjectID device)
{
    if (find (device) == nullptr)
        return false;

    firePropertyListeners (device, kAudioDeviceProcessorOverload);
    return true;
}

int propertyListenerCount (AudioObjectID device)
{
    return static_cast<int> (std::count_if (state().listeners.begin(), state().listeners.end(),
                                            [device] (const Listener& listener)
                                            { return listener.object == device; }));
}

bool isRunning (AudioObjectID device)
{
    auto* d = find (device);
    if (d == nullptr)
        return false;

    return std::any_of (d->procs.begin(), d->procs.end(),
                        [] (const IoProcRegistration& r) { return r.running; });
}

int openIoProcCount (AudioObjectID device)
{
    auto* d = find (device);
    if (d == nullptr)
        return 0;

    return static_cast<int> (d->procs.size());
}

double nominalRate (AudioObjectID device)
{
    auto* d = find (device);
    return d == nullptr ? 0.0 : d->spec.currentRate;
}

bool hogModeHeld (AudioObjectID device)
{
    auto* d = find (device);
    return d != nullptr && d->hogOwner != -1;
}

int hogOwnerPid (AudioObjectID device)
{
    auto* d = find (device);
    return d == nullptr ? -1 : d->hogOwner;
}

int bufferFrameSize (AudioObjectID device)
{
    auto* d = find (device);
    return d == nullptr ? 0 : d->spec.bufferFrameSize;
}

bool pumpInput (AudioObjectID device, const std::vector<std::vector<float>>& channels)
{
    auto* d = find (device);
    if (d == nullptr || channels.empty())
        return false;

    const int numChannels = static_cast<int> (channels.size());
    const int frames = static_cast<int> (channels.front().size());

    BufferListStorage storage;
    auto* list = storage.build (numChannels, frames, d->spec.shape);

    // Pack the caller's per-channel signal in the device's own shape. This is
    // the whole point: the backend must unpack whichever one it is handed.
    if (d->spec.shape == BufferShape::interleaved)
    {
        auto& block = storage.blocks[0];
        for (int f = 0; f < frames; ++f)
            for (int ch = 0; ch < numChannels; ++ch)
                block[static_cast<size_t> (f) * numChannels + ch] = channels[static_cast<size_t> (ch)][static_cast<size_t> (f)];
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            std::copy (channels[static_cast<size_t> (ch)].begin(),
                       channels[static_cast<size_t> (ch)].end(),
                       storage.blocks[static_cast<size_t> (ch)].begin());
    }

    AudioTimeStamp now {};
    bool delivered = false;

    for (auto& r : d->procs)
        if (r.running)
        {
            r.proc (device, &now, list, &now, nullptr, &now, r.clientData);
            delivered = true;
        }

    return delivered;
}

bool pumpInputBuffers (AudioObjectID device, const std::vector<InputBuffer>& buffers)
{
    auto* d = find (device);
    if (d == nullptr || buffers.empty())
        return false;

    BufferListStorage storage;
    storage.listBytes.assign (sizeof (AudioBufferList)
                              + sizeof (AudioBuffer) * (buffers.size() - 1), 0);
    storage.blocks.reserve (buffers.size());

    auto* list = reinterpret_cast<AudioBufferList*> (storage.listBytes.data());
    list->mNumberBuffers = static_cast<UInt32> (buffers.size());

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        if (buffers[i].channels <= 0
            || buffers[i].samples.size() % static_cast<size_t> (buffers[i].channels) != 0)
            return false;

        storage.blocks.push_back (buffers[i].samples);
        list->mBuffers[i].mNumberChannels = static_cast<UInt32> (buffers[i].channels);
        list->mBuffers[i].mDataByteSize =
            static_cast<UInt32> (storage.blocks[i].size() * sizeof (float));
        list->mBuffers[i].mData = storage.blocks[i].data();
    }

    AudioTimeStamp now {};
    bool delivered = false;

    for (auto& r : d->procs)
        if (r.running)
        {
            r.proc (device, &now, list, &now, nullptr, &now, r.clientData);
            delivered = true;
        }

    return delivered;
}

bool pumpOutput (AudioObjectID device, int frames, std::vector<std::vector<float>>& out)
{
    auto* d = find (device);
    if (d == nullptr || d->spec.outputChannels <= 0 || frames <= 0)
        return false;

    const int numChannels = d->spec.outputChannels;

    BufferListStorage storage;
    auto* list = storage.build (numChannels, frames, d->spec.shape);

    AudioTimeStamp now {};
    bool delivered = false;

    for (auto& r : d->procs)
        if (r.running)
        {
            r.proc (device, &now, nullptr, &now, list, &now, r.clientData);
            delivered = true;
        }

    if (! delivered)
        return false;

    // Unpack whatever the backend wrote back into per-channel form, so the
    // harness checks the audio rather than the pointer arithmetic.
    out.assign (static_cast<size_t> (numChannels), std::vector<float> (static_cast<size_t> (frames), 0.0f));

    if (d->spec.shape == BufferShape::interleaved)
    {
        const auto& block = storage.blocks[0];
        for (int f = 0; f < frames; ++f)
            for (int ch = 0; ch < numChannels; ++ch)
                out[static_cast<size_t> (ch)][static_cast<size_t> (f)] =
                    block[static_cast<size_t> (f) * numChannels + ch];
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            out[static_cast<size_t> (ch)] = storage.blocks[static_cast<size_t> (ch)];
    }

    return true;
}

} // namespace fakeca
