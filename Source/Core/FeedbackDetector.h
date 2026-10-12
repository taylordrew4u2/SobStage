#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include "RingBuffer.h"

namespace mma {

class MonitorBus;

/// §5.5 feedback protection: "any 1/3-octave band rising more than 10 dB over
/// 500 ms while remaining within 6 dB of the broadband peak".
///
/// A bank of third-octave band-pass filters over the monitor bus, measured in
/// 20 ms frames. A band qualifies in a frame when its level is within 6 dB of
/// the frame's broadband peak -- one narrow band carrying nearly all of the
/// signal, which is what a howl is and what speech, with its spread spectrum
/// and its 12-20 dB crest factor, is not -- and is above a floor below which
/// nothing is loud enough to be feeding back. A band that has qualified for
/// the whole of the last 500 ms and has risen more than 10 dB across it is
/// feedback building.
///
/// Analysis only: nothing here touches the signal, so §5.4's "nothing on the
/// monitor bus but summing, trim and the limiter" still holds. Not real-time
/// code -- FeedbackGuard runs it on its own thread.
class FeedbackDetector
{
public:
    static constexpr double kBandRiseDb = 10.0;          // §5.5
    static constexpr double kWindowSeconds = 0.5;        // §5.5
    static constexpr double kWithinBroadbandDb = 6.0;    // §5.5
    static constexpr double kFrameSeconds = 0.02;
    static constexpr int kWindowFrames = 25;             // kWindowSeconds / kFrameSeconds

    /// Below this a band is not loud enough to be a howl in anyone's ears;
    /// without it, two frames of near-silence could "rise" by any amount.
    static constexpr double kFloorDb = -60.0;

    /// Wider than a strict third octave (Q 4.3), so a tone exactly between
    /// two centres still reads within about 1.7 dB of its level in one of
    /// them, rather than 3 dB down in both -- which, with a sine's own 3 dB
    /// between RMS and peak, would put it at the edge of the 6 dB rule.
    static constexpr double kBandQ = 3.0;

    static constexpr int kMaxBands = 24;

    explicit FeedbackDetector (double sampleRate) noexcept;

    /// Feed monitor-bus samples. True once a band has met §5.5's rule; the
    /// result latches until reset(). No allocation.
    bool process (const float* samples, int numSamples) noexcept;

    bool hasTriggered() const noexcept { return triggered; }

    /// The centre frequency of the band that tripped, for the record.
    double getTriggeredFrequencyHz() const noexcept { return triggeredHz; }

    /// Forget all history: after a cut, and after any break in the audio the
    /// analysis was fed (a 500 ms rise across a gap is not a rise).
    void reset() noexcept;

    int getNumBands() const noexcept { return numBands; }

    /// True when every band filter's memory is exactly zero, which is where a
    /// silent bus has to leave it (see endFrame). For tests.
    bool filtersAreAtRest() const noexcept;

private:
    struct Band
    {
        double centreHz = 0.0;
        double b0 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // b1 is zero for a band-pass
        double z1 = 0.0, z2 = 0.0;
        double energy = 0.0;
        int qualifyingFrames = 0;
        std::array<double, kWindowFrames + 1> history {};
    };

    double sampleRate;
    std::array<Band, kMaxBands> bands {};
    int numBands = 0;

    int frameLength = 960;
    int frameFill = 0;
    float framePeak = 0.0f;
    int historyIndex = 0;

    bool triggered = false;
    double triggeredHz = 0.0;

    void endFrame() noexcept;
};

/// Carries the monitor bus off the audio thread and runs FeedbackDetector on
/// it, cutting the bus (MonitorBus::engageRunawayCut -- the same latched cut,
/// visible reason and one-tap Unmute as the limiter's runaway cut) when a band
/// trips.
///
/// The audio thread only copies the block into a preallocated lock-free ring
/// (§11: no allocation, lock or log there). The analysis thread wakes every
/// 10 ms and drains it. If it ever falls a whole second behind, or the bus
/// stops running for a while (noteGap), what is queued is dropped and the
/// analysis restarts from the next block: a rise is never measured across a
/// hole.
class FeedbackGuard
{
public:
    FeedbackGuard (double sampleRate, MonitorBus& bus);
    ~FeedbackGuard();

    /// Audio thread. Real-time safe.
    void push (const float* samples, int numSamples) noexcept;

    /// Audio thread: the bus did not run for a stretch -- a cycle with no
    /// headphone buffers to fill, the software clock standing in for a lost
    /// output -- so the next block pushed does not follow the last one, and
    /// no rise may be measured across the two. One atomic store.
    void noteGap() noexcept { discontinuity.store (true, std::memory_order_release); }

    /// Starts and stops the analysis thread. Idempotent; message thread.
    void start();
    void stop();
    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    /// Drains what the audio thread has handed over and analyses it. The
    /// analysis thread's whole job; callable directly only while that thread
    /// is stopped (there is one consumer). True if this call cut the bus.
    bool analyzePending() noexcept;

    /// Times this guard has cut the monitor, and the band it last cut for.
    uint64_t getCutCount() const noexcept { return cuts.load (std::memory_order_relaxed); }
    double getLastCutFrequencyHz() const noexcept { return lastCutHz.load (std::memory_order_relaxed); }

    /// Samples the analysis thread fell too far behind to see.
    uint64_t getDroppedSamples() const noexcept { return dropped.load (std::memory_order_relaxed); }

    /// Samples handed over and not yet analysed. For harnesses that run the
    /// audio side faster than real time and must let the analysis catch up.
    size_t getPendingSamples() const noexcept { return ring.availableForRead(); }

private:
    MonitorBus& bus;
    FeedbackDetector detector;
    RingBuffer ring;
    std::array<float, 1024> scratch {};

    std::atomic<bool> discontinuity { false };
    std::atomic<uint64_t> dropped { 0 };
    std::atomic<uint64_t> cuts { 0 };
    std::atomic<double> lastCutHz { 0.0 };

    std::atomic<bool> running { false };
    std::thread worker;
    std::mutex wakeLock;
    std::condition_variable wake;
    bool stopRequested = false;

    void run();
};

} // namespace mma
