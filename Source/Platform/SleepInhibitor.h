#pragma once

#include <memory>
#include <optional>

#include "../Core/BatteryRecordingNotice.h"

namespace mma {

/// §6.6: a laptop that idles to sleep mid-take ends the take. While a take is
/// recording this holds macOS power assertions that stop idle display sleep,
/// idle system sleep and -- on AC power, on most Macs -- the sleep a closed
/// lid would otherwise cause (IOPMAssertionCreateWithName, released with
/// IOPMAssertionRelease). On battery macOS sleeps on lid close regardless;
/// BatteryRecordingNotice tells the performer. Elsewhere it is a no-op.
///
/// setHeld() is idempotent and the destructor releases, so the owner can
/// simply mirror "is a take recording?" into it from any tick and still never
/// leak an assertion on quit. The OS calls sit behind Backend so the
/// bookkeeping is unit-tested on every platform.
class SleepInhibitor
{
public:
    class Backend
    {
    public:
        virtual ~Backend() = default;
        /// True when the OS is now holding the machine awake.
        virtual bool acquire() = 0;
        /// Only called after a successful acquire().
        virtual void release() = 0;
    };

    /// The platform's backend: IOKit power assertions on macOS, a no-op
    /// (acquire() returns false) anywhere else.
    static std::unique_ptr<Backend> createPlatformBackend();

    /// What is powering the computer now (IOPSGetProvidingPowerSourceType on
    /// macOS). Unknown anywhere else, and whenever the OS gives no answer.
    static PowerSource queryPowerSource();

    SleepInhibitor();
    explicit SleepInhibitor (std::unique_ptr<Backend> backend);
    ~SleepInhibitor();

    SleepInhibitor (const SleepInhibitor&) = delete;
    SleepInhibitor& operator= (const SleepInhibitor&) = delete;

    /// Acquire when `shouldHold` and not already held; release when not
    /// `shouldHold` and held. A failed acquire is retried on the next call.
    void setHeld (bool shouldHold);

    /// Whether an assertion is currently held.
    bool isHeld() const noexcept { return held; }

private:
    std::unique_ptr<Backend> backend;
    bool held = false;
};

/// Notices that the computer has slept, from two clocks: one that stops while
/// it sleeps and one that keeps counting. Time on the second that is not on
/// the first was spent asleep. On a Mac CoreAudio says so itself; Windows and
/// Linux say nothing -- each stream simply dies on waking -- so this is how
/// a wake is told apart from microphones losing power (§14.2).
class WakeDetector
{
public:
    struct Clocks
    {
        double awakeMs = 0.0;      ///< stops while the computer sleeps
        double withSleepMs = 0.0;  ///< keeps counting through it
    };

    /// This computer's two clocks now: QueryUnbiasedInterruptTime and
    /// GetTickCount64 on Windows, CLOCK_UPTIME_RAW and CLOCK_MONOTONIC_RAW on
    /// a Mac, CLOCK_MONOTONIC and CLOCK_BOOTTIME on Linux. Nothing when they
    /// cannot be read.
    static std::optional<Clocks> read() noexcept;

    /// True when at least kMinSleepMs was spent asleep since the last call.
    /// The first call only takes its bearings.
    bool justWoke (const Clocks& now) noexcept;

    /// More than the two clocks' own disagreement (GetTickCount64 moves in
    /// 16 ms steps), less than any sleep that ends a stream.
    static constexpr double kMinSleepMs = 2000.0;

private:
    std::optional<double> lastAsleepMs;
};

} // namespace mma
