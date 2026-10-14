#include "SleepInhibitor.h"

#include <utility>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <time.h>
#endif

#if defined(__APPLE__)
 // Plain C API: no Objective-C needed, so this stays a .cpp file.
 #include <IOKit/pwr_mgt/IOPMLib.h>
 #include <IOKit/ps/IOPowerSources.h>
 #include <IOKit/ps/IOPSKeys.h>
 // NSProcessInfo through the runtime, the way SystemPermissions.cpp reaches
 // AVCaptureDevice, rather than turning this file into Objective-C++.
 #include <objc/message.h>
 #include <objc/runtime.h>
#endif

namespace mma {

namespace {

#if defined(__APPLE__)

// IOPMAssertionID is a uint32_t; 0 is never a valid assertion (the SDK's
// kIOPMNullAssertionID), spelled here to depend on as little as possible.
constexpr IOPMAssertionID kNoAssertion = 0;

// App Nap. The power assertions keep the Mac awake; they do not stop macOS
// throttling an app that is hidden or covered -- timers coalesced, threads
// deprioritised -- and a take runs with the window behind a lyrics sheet or a
// browser as a matter of course. The watchdog, the alarms and the disk writer
// all live outside the audio callback and were exposed to that. A
// user-initiated, latency-critical activity is how an app says "not now".
// The values are NSActivityOptions from Foundation's NSProcessInfo.h.
constexpr unsigned long long kNSActivityUserInitiated = 0x00FFFFFFULL | (1ULL << 20);
constexpr unsigned long long kNSActivityLatencyCritical = 0xFF00000000ULL;

id beginRecordingActivity()
{
    const auto processInfoClass = reinterpret_cast<id> (objc_getClass ("NSProcessInfo"));
    const auto stringClass = reinterpret_cast<id> (objc_getClass ("NSString"));

    // Foundation is not loaded in every binary that links this (the unit
    // tests); without it there is nothing to ask and nothing to throttle.
    if (processInfoClass == nil || stringClass == nil)
        return nil;

    using SendId = id (*) (id, SEL);
    using SendStringFromUtf8 = id (*) (id, SEL, const char*);
    using SendBegin = id (*) (id, SEL, unsigned long long, id);

    const auto processInfo = reinterpret_cast<SendId> (objc_msgSend) (
        processInfoClass, sel_registerName ("processInfo"));
    const auto beginSelector = sel_registerName ("beginActivityWithOptions:reason:");

    if (processInfo == nil || ! class_respondsToSelector (object_getClass (processInfo), beginSelector))
        return nil;

    const auto reason = reinterpret_cast<SendStringFromUtf8> (objc_msgSend) (
        stringClass, sel_registerName ("stringWithUTF8String:"), "SobStage is recording");

    const auto activity = reinterpret_cast<SendBegin> (objc_msgSend) (
        processInfo, beginSelector, kNSActivityUserInitiated | kNSActivityLatencyCritical, reason);

    // The token comes back autoreleased; it has to outlive this call.
    return activity != nil ? reinterpret_cast<SendId> (objc_msgSend) (activity, sel_registerName ("retain"))
                           : nil;
}

void endRecordingActivity (id activity)
{
    if (activity == nil)
        return;

    using SendId = id (*) (id, SEL);
    using SendEnd = void (*) (id, SEL, id);

    const auto processInfoClass = reinterpret_cast<id> (objc_getClass ("NSProcessInfo"));
    if (processInfoClass != nil)
        if (const auto processInfo = reinterpret_cast<SendId> (objc_msgSend) (
                processInfoClass, sel_registerName ("processInfo")); processInfo != nil)
            reinterpret_cast<SendEnd> (objc_msgSend) (processInfo, sel_registerName ("endActivity:"), activity);

    reinterpret_cast<void (*) (id, SEL)> (objc_msgSend) (activity, sel_registerName ("release"));
}

class MacPowerAssertions final : public SleepInhibitor::Backend
{
public:
    bool acquire() override
    {
        const CFStringRef name = CFSTR ("SobStage recording");

        IOPMAssertionID display = kNoAssertion;
        if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                         kIOPMAssertionLevelOn, name, &display)
            != kIOReturnSuccess)
            return false;

        IOPMAssertionID system = kNoAssertion;
        if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleSystemSleep,
                                         kIOPMAssertionLevelOn, name, &system)
            != kIOReturnSuccess)
        {
            IOPMAssertionRelease (display);
            return false;
        }

        displayAssertion = display;
        systemAssertion = system;

        // The two above are about IDLE sleep. Closing the lid is not idle, and
        // a performer who shuts the laptop to get it out of the way mid-take
        // ended the take. This one holds the system up on AC power, lid closed
        // included on most Macs; on battery macOS sleeps on lid close whatever
        // is asked, which is why the app warns then instead. Best effort, so a
        // Mac that refuses it is still kept from idling as before.
        IOPMAssertionID lid = kNoAssertion;
        if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventSystemSleep,
                                         kIOPMAssertionLevelOn, name, &lid)
            == kIOReturnSuccess)
            preventSleepAssertion = lid;

        // Best effort: a Mac that will not grant it still records, as before.
        appNapActivity = beginRecordingActivity();
        return true;
    }

    void release() override
    {
        if (displayAssertion != kNoAssertion)
            IOPMAssertionRelease (displayAssertion);
        if (systemAssertion != kNoAssertion)
            IOPMAssertionRelease (systemAssertion);
        if (preventSleepAssertion != kNoAssertion)
            IOPMAssertionRelease (preventSleepAssertion);

        displayAssertion = kNoAssertion;
        systemAssertion = kNoAssertion;
        preventSleepAssertion = kNoAssertion;

        endRecordingActivity (appNapActivity);
        appNapActivity = nil;
    }

private:
    IOPMAssertionID displayAssertion = kNoAssertion;
    IOPMAssertionID systemAssertion = kNoAssertion;
    IOPMAssertionID preventSleepAssertion = kNoAssertion;
    id appNapActivity = nil;
};

#else

class NoSleepInhibition final : public SleepInhibitor::Backend
{
public:
    bool acquire() override { return false; }
    void release() override {}
};

#endif

} // namespace

std::unique_ptr<SleepInhibitor::Backend> SleepInhibitor::createPlatformBackend()
{
#if defined(__APPLE__)
    return std::make_unique<MacPowerAssertions>();
#else
    return std::make_unique<NoSleepInhibition>();
#endif
}

PowerSource SleepInhibitor::queryPowerSource()
{
#if defined(__APPLE__)
    const CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr)
        return PowerSource::Unknown;

    auto source = PowerSource::Unknown;

    // Not owned: the string lives inside `info`, so it is read before release.
    if (const CFStringRef type = IOPSGetProvidingPowerSourceType (info); type != nullptr)
    {
        if (CFStringCompare (type, CFSTR (kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo)
            source = PowerSource::Battery;
        else if (CFStringCompare (type, CFSTR (kIOPMACPowerKey), 0) == kCFCompareEqualTo)
            source = PowerSource::AC;
    }

    CFRelease (info);
    return source;
#else
    return PowerSource::Unknown;
#endif
}

SleepInhibitor::SleepInhibitor() : SleepInhibitor (createPlatformBackend()) {}

SleepInhibitor::SleepInhibitor (std::unique_ptr<Backend> b) : backend (std::move (b)) {}

SleepInhibitor::~SleepInhibitor()
{
    setHeld (false);
}

void SleepInhibitor::setHeld (bool shouldHold)
{
    if (backend == nullptr || shouldHold == held)
        return;

    if (shouldHold)
    {
        held = backend->acquire();
    }
    else
    {
        backend->release();
        held = false;
    }
}

std::optional<WakeDetector::Clocks> WakeDetector::read() noexcept
{
   #if defined(_WIN32)
    ULONGLONG unbiased = 0; // 100 ns units, not counting sleep or hibernation
    if (QueryUnbiasedInterruptTime (&unbiased) == 0)
        return std::nullopt;

    return Clocks { static_cast<double> (unbiased) / 10000.0, static_cast<double> (GetTickCount64()) };
   #else
    #if defined(__APPLE__)
    constexpr clockid_t awakeClock = CLOCK_UPTIME_RAW;
    constexpr clockid_t withSleepClock = CLOCK_MONOTONIC_RAW;
    #else
    constexpr clockid_t awakeClock = CLOCK_MONOTONIC;
    constexpr clockid_t withSleepClock = CLOCK_BOOTTIME;
    #endif

    timespec awake {}, withSleep {};
    if (clock_gettime (awakeClock, &awake) != 0 || clock_gettime (withSleepClock, &withSleep) != 0)
        return std::nullopt;

    const auto ms = [] (const timespec& t)
    {
        return static_cast<double> (t.tv_sec) * 1000.0 + static_cast<double> (t.tv_nsec) / 1.0e6;
    };

    return Clocks { ms (awake), ms (withSleep) };
   #endif
}

bool WakeDetector::justWoke (const Clocks& now) noexcept
{
    const double asleepMs = now.withSleepMs - now.awakeMs;
    const bool woke = lastAsleepMs.has_value() && asleepMs - *lastAsleepMs >= kMinSleepMs;
    lastAsleepMs = asleepMs;
    return woke;
}

} // namespace mma
