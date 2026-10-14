#pragma once

// A stand-in for Apple's <CoreAudio/CoreAudio.h>, covering exactly the surface
// CoreAudioBackend.cpp uses and nothing more.
//
// The point is not to reimplement CoreAudio. It is to let the real, unmodified
// backend source compile and run on a machine that has no CoreAudio, driven by
// a virtual HAL (FakeCoreAudio.h) that can be configured to behave like the
// hardware people actually own -- interleaved stereo microphones, devices that
// advertise a continuous sample-rate range, devices that refuse hog mode.
//
// Every declaration here matches Apple's in type and semantics. Where a
// constant's numeric value is observable (the four-character property
// selectors) the real value is used, so a typo in the backend fails here the
// same way it would on macOS.

#include <cstdint>
#include <cstddef>

/// CoreAudio spells its selectors as four-character codes. Written out rather
/// than as multi-character literals, which are implementation-defined and warn.
constexpr uint32_t mmaFourCC (char a, char b, char c, char d)
{
    return (static_cast<uint32_t> (static_cast<unsigned char> (a)) << 24)
         | (static_cast<uint32_t> (static_cast<unsigned char> (b)) << 16)
         | (static_cast<uint32_t> (static_cast<unsigned char> (c)) << 8)
         |  static_cast<uint32_t> (static_cast<unsigned char> (d));
}

using OSStatus = int32_t;
using UInt32   = uint32_t;
using UInt64   = uint64_t;
using SInt32   = int32_t;
using Float64  = double;
using Boolean  = unsigned char;

constexpr OSStatus noErr = 0;
constexpr OSStatus kAudioHardwareUnspecifiedError = mmaFourCC ('w', 'h', 'a', 't');
constexpr OSStatus kAudioHardwareBadObjectError   = mmaFourCC ('!', 'o', 'b', 'j');
constexpr OSStatus kAudioHardwareBadDeviceError   = mmaFourCC ('!', 'd', 'e', 'v');
constexpr OSStatus kAudioDevicePermissionsError   = mmaFourCC ('!', 'h', 'o', 'g');
constexpr OSStatus kAudioHardwareUnknownPropertyError = mmaFourCC ('w', 'h', 'o', '?');

// --- CoreFoundation ---------------------------------------------------------
// Only the calls the backend and the combined device (MacSystemAggregateDevice)
// make are provided. Every object here is an owning, reference-counted handle
// the caller releases, exactly as on macOS, so a missing CFRelease shows up as
// a leak in the simulation too.
using CFTypeRef = const void*;
using CFIndex = long;

struct __CFAllocator;
using CFAllocatorRef = const __CFAllocator*;
constexpr CFAllocatorRef kCFAllocatorDefault = nullptr;

struct __CFString;
using CFStringRef = const __CFString*;
using CFStringEncoding = UInt32;
constexpr CFStringEncoding kCFStringEncodingUTF8 = 0x08000100;

struct __CFArray;
using CFArrayRef = const __CFArray*;
using CFMutableArrayRef = __CFArray*;

struct __CFDictionary;
using CFDictionaryRef = const __CFDictionary*;
using CFMutableDictionaryRef = __CFDictionary*;

struct __CFNumber;
using CFNumberRef = const __CFNumber*;
using CFNumberType = CFIndex;
constexpr CFNumberType kCFNumberIntType = 9;

struct CFArrayCallBacks { CFIndex version; };
struct CFDictionaryKeyCallBacks { CFIndex version; };
struct CFDictionaryValueCallBacks { CFIndex version; };
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;

Boolean CFStringGetCString (CFStringRef value, char* buffer, long bufferSize, CFStringEncoding encoding);
CFStringRef CFStringCreateWithCString (CFAllocatorRef allocator, const char* text, CFStringEncoding encoding);

/// CFSTR("...") is a compile-time constant on macOS that is never released.
/// Here it is an immortal string interned per literal.
CFStringRef mmaFakeConstantString (const char* text);
#define CFSTR(text) mmaFakeConstantString (text)

CFMutableArrayRef CFArrayCreateMutable (CFAllocatorRef allocator, CFIndex capacity, const CFArrayCallBacks* callBacks);
void CFArrayAppendValue (CFMutableArrayRef array, const void* value);

CFMutableDictionaryRef CFDictionaryCreateMutable (CFAllocatorRef allocator, CFIndex capacity,
                                                  const CFDictionaryKeyCallBacks* keyCallBacks,
                                                  const CFDictionaryValueCallBacks* valueCallBacks);
void CFDictionarySetValue (CFMutableDictionaryRef dictionary, const void* key, const void* value);

CFNumberRef CFNumberCreate (CFAllocatorRef allocator, CFNumberType type, const void* valuePtr);

void CFRelease (CFTypeRef value);

/// The wall clock, in seconds since 2001. It keeps counting while the Mac
/// sleeps; fakeca::sleepSystem() moves it on without moving the host clock.
using CFTimeInterval = double;
using CFAbsoluteTime = CFTimeInterval;
CFAbsoluteTime CFAbsoluteTimeGetCurrent();

// --- Host time (<CoreAudio/HostTime.h>) ---------------------------------------
// mach_absolute_time, which CoreAudio stamps its IO with. It stops while the
// Mac sleeps. Here a host tick is a nanosecond (on Apple Silicon it is not,
// which is why callers convert rather than assume).
UInt64 AudioGetCurrentHostTime();
UInt64 AudioConvertHostTimeToNanos (UInt64 hostTime);

// Not CoreAudio: the simulated clock_gettime_nsec_np (CLOCK_MONOTONIC_RAW)
// (mach_continuous_time), which counts through sleep and is never stepped
// with the time of day. Under its own name, so the real <time.h> on a Mac and
// MSVC's, which has no such call, are left alone.
UInt64 SimulatedContinuousTimeNanos();

// --- Objects and properties -------------------------------------------------
using AudioObjectID = UInt32;
using AudioObjectPropertySelector = UInt32;
using AudioObjectPropertyScope = UInt32;
using AudioObjectPropertyElement = UInt32;

constexpr AudioObjectID kAudioObjectUnknown = 0;
constexpr AudioObjectID kAudioObjectSystemObject = 1;

constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeGlobal = mmaFourCC ('g', 'l', 'o', 'b');
constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeInput  = mmaFourCC ('i', 'n', 'p', 't');
constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeOutput = mmaFourCC ('o', 'u', 't', 'p');
constexpr AudioObjectPropertyElement kAudioObjectPropertyElementMain = 0;

constexpr AudioObjectPropertySelector kAudioObjectPropertyName = mmaFourCC ('l', 'n', 'a', 'm');
constexpr AudioObjectPropertySelector kAudioHardwarePropertyDevices = mmaFourCC ('d', 'e', 'v', '#');
constexpr AudioObjectPropertySelector kAudioDevicePropertyDeviceUID = mmaFourCC ('u', 'i', 'd', ' ');

// §5.3 priorities 3 and 4 on a Mac: the output macOS currently plays through,
// and which physical destination (speaker or headphone jack) a built-in output
// is routed to. Apple's own selector values.
constexpr AudioObjectPropertySelector kAudioHardwarePropertyDefaultOutputDevice = mmaFourCC ('d', 'O', 'u', 't');
constexpr AudioObjectPropertySelector kAudioDevicePropertyDataSource = mmaFourCC ('s', 's', 'r', 'c');

// Whether something is plugged into a device's jack (output scope). Some Macs
// report it alongside, or instead of, a data-source change.
constexpr AudioObjectPropertySelector kAudioDevicePropertyJackIsConnected = mmaFourCC ('j', 'a', 'c', 'k');

// Sent on the system object when coreaudiod has restarted: every AudioObjectID
// the process held may now be stale, and aggregates may be gone.
constexpr AudioObjectPropertySelector kAudioHardwarePropertyServiceRestarted = mmaFourCC ('s', 'r', 's', 't');

// Transport types used by the backend's external-input allow-list. Values
// match Apple's four-character codes so the shipping source is exercised
// unchanged by the simulation.
constexpr AudioObjectPropertySelector kAudioDevicePropertyTransportType = mmaFourCC ('t', 'r', 'a', 'n');
constexpr UInt32 kAudioDeviceTransportTypeBuiltIn                   = mmaFourCC ('b', 'l', 't', 'n');
constexpr UInt32 kAudioDeviceTransportTypeAggregate                 = mmaFourCC ('g', 'r', 'u', 'p');
constexpr UInt32 kAudioDeviceTransportTypeVirtual                   = mmaFourCC ('v', 'i', 'r', 't');
constexpr UInt32 kAudioDeviceTransportTypePCI                       = mmaFourCC ('p', 'c', 'i', ' ');
constexpr UInt32 kAudioDeviceTransportTypeUSB                       = mmaFourCC ('u', 's', 'b', ' ');
constexpr UInt32 kAudioDeviceTransportTypeFireWire                  = mmaFourCC ('1', '3', '9', '4');
constexpr UInt32 kAudioDeviceTransportTypeBluetooth                 = mmaFourCC ('b', 'l', 'u', 'e');
constexpr UInt32 kAudioDeviceTransportTypeBluetoothLE               = mmaFourCC ('b', 'l', 'e', 'a');
constexpr UInt32 kAudioDeviceTransportTypeAirPlay                   = mmaFourCC ('a', 'i', 'r', 'p');
constexpr UInt32 kAudioDeviceTransportTypeAVB                       = mmaFourCC ('e', 'a', 'v', 'b');
constexpr UInt32 kAudioDeviceTransportTypeThunderbolt               = mmaFourCC ('t', 'h', 'u', 'n');
constexpr UInt32 kAudioDeviceTransportTypeContinuityCaptureWired    = mmaFourCC ('c', 'c', 'w', 'd');
constexpr UInt32 kAudioDeviceTransportTypeContinuityCaptureWireless = mmaFourCC ('c', 'c', 'w', 'l');
constexpr UInt32 kAudioDeviceTransportTypeContinuityCapture         = mmaFourCC ('c', 'c', 'a', 'p');
constexpr AudioObjectPropertySelector kAudioDevicePropertyStreamConfiguration = mmaFourCC ('s', 'l', 'a', 'y');
constexpr AudioObjectPropertySelector kAudioDevicePropertyDeviceIsAlive = mmaFourCC ('l', 'i', 'v', 'n');
constexpr AudioObjectPropertySelector kAudioDevicePropertyNominalSampleRate = mmaFourCC ('n', 's', 'r', 't');
constexpr AudioObjectPropertySelector kAudioDevicePropertyAvailableNominalSampleRates = mmaFourCC ('n', 's', 'r', '#');
constexpr AudioObjectPropertySelector kAudioDeviceProcessorOverload = mmaFourCC ('o', 'v', 'e', 'r');
constexpr AudioObjectPropertySelector kAudioDevicePropertyBufferFrameSize = mmaFourCC ('f', 's', 'i', 'z');
constexpr AudioObjectPropertySelector kAudioDevicePropertyHogMode = mmaFourCC ('o', 'i', 'n', 'k');
constexpr AudioObjectPropertySelector kAudioDevicePropertyLatency = mmaFourCC ('l', 't', 'n', 'c');
constexpr AudioObjectPropertySelector kAudioDevicePropertySafetyOffset = mmaFourCC ('s', 'a', 'f', 't');
constexpr AudioObjectPropertySelector kAudioStreamPropertyLatency = mmaFourCC ('l', 't', 'n', 'c');

// An aggregate device's sub-devices that are present and contributing
// channels, as AudioObjectIDs in channel order.
constexpr AudioObjectPropertySelector kAudioAggregateDevicePropertyActiveSubDeviceList = mmaFourCC ('a', 'g', 'r', 'p');

// The description dictionary keys AudioHardwareCreateAggregateDevice reads.
// Apple defines these as C string literals for use with CFSTR().
#define kAudioAggregateDeviceUIDKey "uid"
#define kAudioAggregateDeviceNameKey "name"
#define kAudioAggregateDeviceSubDeviceListKey "subdevices"
#define kAudioAggregateDeviceMainSubDeviceKey "master"
#define kAudioSubDeviceUIDKey "uid"
#define kAudioSubDeviceDriftCompensationKey "drift"

// §2.3: what a device can actually deliver. CoreAudio keeps this on the
// STREAM, not the device, so finding it is two hops: ask the device for its
// streams, then ask a stream for its available physical formats. These are
// Apple's own selector values, as every other one in this file is.
constexpr AudioObjectPropertySelector kAudioDevicePropertyStreams = mmaFourCC ('s', 't', 'm', '#');
constexpr AudioObjectPropertySelector kAudioStreamPropertyAvailablePhysicalFormats = mmaFourCC ('p', 'f', 't', 'a');

using AudioFormatID = UInt32;
using AudioFormatFlags = UInt32;

constexpr AudioFormatID kAudioFormatLinearPCM = mmaFourCC ('l', 'p', 'c', 'm');

struct AudioObjectPropertyAddress
{
    AudioObjectPropertySelector mSelector;
    AudioObjectPropertyScope mScope;
    AudioObjectPropertyElement mElement;
};

struct AudioValueRange
{
    Float64 mMinimum;
    Float64 mMaximum;
};

// Apple's field order and widths, because the backend reads mBitsPerChannel out
// of an array of these by index -- a layout that differs from the real one
// would read the wrong field and the simulation would prove nothing.
struct AudioStreamBasicDescription
{
    Float64 mSampleRate;
    AudioFormatID mFormatID;
    AudioFormatFlags mFormatFlags;
    UInt32 mBytesPerPacket;
    UInt32 mFramesPerPacket;
    UInt32 mBytesPerFrame;
    UInt32 mChannelsPerFrame;
    UInt32 mBitsPerChannel;
    UInt32 mReserved;
};

struct AudioStreamRangedDescription
{
    AudioStreamBasicDescription mFormat;
    AudioValueRange mSampleRateRange;
};

// --- Buffers ----------------------------------------------------------------
struct AudioBuffer
{
    UInt32 mNumberChannels;
    UInt32 mDataByteSize;
    void* mData;
};

// Apple declares the trailing array as [1] and over-allocates. The backend
// indexes past it for multi-buffer devices, which is correct against Apple's
// layout, so the same declaration is used here rather than a "safer" one that
// would not reproduce the real memory layout.
struct AudioBufferList
{
    UInt32 mNumberBuffers;
    AudioBuffer mBuffers[1];
};

struct AudioTimeStamp
{
    Float64 mSampleTime;
    uint64_t mHostTime;
    Float64 mRateScalar;
    uint64_t mWordClockTime;
    UInt32 mFlags;
    UInt32 mReserved;
};

// --- Calls ------------------------------------------------------------------
using AudioObjectPropertyListenerProc = OSStatus (*) (AudioObjectID,
                                                      UInt32,
                                                      const AudioObjectPropertyAddress*,
                                                      void*);

using AudioDeviceIOProc = OSStatus (*) (AudioObjectID,
                                        const AudioTimeStamp*,
                                        const AudioBufferList*,
                                        const AudioTimeStamp*,
                                        AudioBufferList*,
                                        const AudioTimeStamp*,
                                        void*);

using AudioDeviceIOProcID = AudioDeviceIOProc;

OSStatus AudioObjectGetPropertyDataSize (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         UInt32 qualifierDataSize,
                                         const void* qualifierData,
                                         UInt32* outSize);

OSStatus AudioObjectGetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32 qualifierDataSize,
                                     const void* qualifierData,
                                     UInt32* ioSize,
                                     void* outData);

OSStatus AudioObjectSetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32 qualifierDataSize,
                                     const void* qualifierData,
                                     UInt32 inSize,
                                     const void* inData);

Boolean AudioObjectHasProperty (AudioObjectID object, const AudioObjectPropertyAddress* address);

OSStatus AudioObjectAddPropertyListener (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         AudioObjectPropertyListenerProc listener,
                                         void* clientData);

OSStatus AudioObjectRemovePropertyListener (AudioObjectID object,
                                            const AudioObjectPropertyAddress* address,
                                            AudioObjectPropertyListenerProc listener,
                                            void* clientData);

OSStatus AudioDeviceCreateIOProcID (AudioObjectID device,
                                    AudioDeviceIOProc proc,
                                    void* clientData,
                                    AudioDeviceIOProcID* outProcId);

OSStatus AudioDeviceDestroyIOProcID (AudioObjectID device, AudioDeviceIOProcID procId);
OSStatus AudioDeviceStart (AudioObjectID device, AudioDeviceIOProcID procId);
OSStatus AudioDeviceStop (AudioObjectID device, AudioDeviceIOProcID procId);

OSStatus AudioHardwareCreateAggregateDevice (CFDictionaryRef description, AudioObjectID* outDevice);
OSStatus AudioHardwareDestroyAggregateDevice (AudioObjectID device);
