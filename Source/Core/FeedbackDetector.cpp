#include "FeedbackDetector.h"
#include "MonitorBus.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace mma {

namespace {

// ISO 266 third-octave centres across the range a headphone-to-microphone
// loop can howl in.
constexpr double kCentres[] = { 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0,
                                800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0,
                                6300.0, 8000.0, 10000.0, 12500.0 };

constexpr double kPi = 3.14159265358979323846;

// Band filter memory smaller than this is cleared at each frame end. See
// endFrame().
constexpr double kFilterMemoryFloor = 1.0e-20;

double toDb (double power) noexcept
{
    return power > 1.0e-20 ? 10.0 * std::log10 (power) : -200.0;
}

} // namespace

static_assert (static_cast<int> (sizeof (kCentres) / sizeof (kCentres[0])) <= FeedbackDetector::kMaxBands,
               "the band table must fit the detector's fixed storage");

FeedbackDetector::FeedbackDetector (double rate) noexcept
    : sampleRate (rate > 0.0 ? rate : 48000.0)
{
    frameLength = std::max (1, static_cast<int> (std::lround (sampleRate * kFrameSeconds)));

    for (const double centre : kCentres)
    {
        // A band whose upper edge is near Nyquist cannot be measured.
        if (centre * 1.2 >= sampleRate * 0.5)
            break;

        // RBJ band-pass, 0 dB at the centre.
        auto& band = bands[static_cast<size_t> (numBands++)];
        const double w0 = 2.0 * kPi * centre / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * kBandQ);
        const double a0 = 1.0 + alpha;

        band.centreHz = centre;
        band.b0 = alpha / a0;
        band.b2 = -alpha / a0;
        band.a1 = -2.0 * std::cos (w0) / a0;
        band.a2 = (1.0 - alpha) / a0;
    }

    reset();
}

void FeedbackDetector::reset() noexcept
{
    for (int b = 0; b < numBands; ++b)
    {
        auto& band = bands[static_cast<size_t> (b)];
        band.z1 = band.z2 = 0.0;
        band.energy = 0.0;
        band.qualifyingFrames = 0;
        band.history.fill (-200.0);
    }

    frameFill = 0;
    framePeak = 0.0f;
    historyIndex = 0;
    triggered = false;
    triggeredHz = 0.0;
}

bool FeedbackDetector::filtersAreAtRest() const noexcept
{
    for (int b = 0; b < numBands; ++b)
    {
        const auto& band = bands[static_cast<size_t> (b)];

        if (band.z1 != 0.0 || band.z2 != 0.0)
            return false;
    }

    return true;
}

bool FeedbackDetector::process (const float* samples, int numSamples) noexcept
{
    if (samples == nullptr)
        return triggered;

    for (int i = 0; i < numSamples; ++i)
    {
        const double x = std::isfinite (samples[i]) ? static_cast<double> (samples[i]) : 0.0;
        framePeak = std::max (framePeak, static_cast<float> (std::abs (x)));

        for (int b = 0; b < numBands; ++b)
        {
            auto& band = bands[static_cast<size_t> (b)];

            // Transposed direct form II; b1 is zero.
            const double y = band.b0 * x + band.z1;
            band.z1 = -band.a1 * y + band.z2;
            band.z2 = band.b2 * x - band.a2 * y;
            band.energy += y * y;
        }

        if (++frameFill >= frameLength)
            endFrame();
    }

    return triggered;
}

void FeedbackDetector::endFrame() noexcept
{
    // Peak in dBFS against band level as RMS in dBFS: a pure tone sits 3 dB
    // below its own peak, speech a dozen or more.
    const double peakDb = 20.0 * std::log10 (std::max (1.0e-10, static_cast<double> (framePeak)));
    const int oldest = (historyIndex + 1) % (kWindowFrames + 1);

    for (int b = 0; b < numBands; ++b)
    {
        auto& band = bands[static_cast<size_t> (b)];
        const double levelDb = toDb (band.energy / static_cast<double> (frameLength));
        band.energy = 0.0;

        // A silent bus -- the headphone mix muted with the spacebar, held by a
        // latched cut, every input quiet -- is exact zeros, sometimes for a
        // whole show. Zeros do not take a recursive filter's memory to zero:
        // it decays into denormal doubles and then circulates there for as
        // long as the silence lasts, and on an Intel Mac every step of
        // arithmetic on a denormal is dozens of times slower. That held this
        // thread at around a tenth of a core while nothing was playing.
        //
        // Memory this small is far below anything measured: 1e-20 is about
        // -400 dBFS, and its energy reads as silence in toDb. Once it is
        // zero, zeros keep it there. And it cannot reach the denormal range
        // (below about 2.2e-308) between two frame ends: within one 20 ms
        // frame even the fastest-decaying band, 12.5 kHz, falls by no more
        // than about 114 orders of magnitude, at any sample rate.
        if (std::abs (band.z1) < kFilterMemoryFloor)
            band.z1 = 0.0;

        if (std::abs (band.z2) < kFilterMemoryFloor)
            band.z2 = 0.0;

        const bool qualifies = levelDb >= kFloorDb && peakDb - levelDb <= kWithinBroadbandDb;
        band.qualifyingFrames = qualifies ? band.qualifyingFrames + 1 : 0;
        band.history[static_cast<size_t> (historyIndex)] = levelDb;

        // "Remaining within": both ends of the 500 ms, and every frame
        // between, qualified. The rise is measured end to end.
        if (! triggered && band.qualifyingFrames > kWindowFrames
            && levelDb - band.history[static_cast<size_t> (oldest)] > kBandRiseDb)
        {
            triggered = true;
            triggeredHz = band.centreHz;
        }
    }

    historyIndex = oldest;
    frameFill = 0;
    framePeak = 0.0f;
}

//==============================================================================

FeedbackGuard::FeedbackGuard (double sampleRate, MonitorBus& b)
    : bus (b),
      detector (sampleRate),
      // A second of the bus: the analysis thread wakes every 10 ms, so this
      // is a hundred wakes of slack before anything is dropped.
      ring (static_cast<size_t> (std::max (4096.0, sampleRate > 0.0 ? sampleRate : 48000.0)))
{
}

FeedbackGuard::~FeedbackGuard()
{
    stop();
}

void FeedbackGuard::push (const float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0)
        return;

    const auto written = ring.write (samples, static_cast<size_t> (numSamples));

    if (written < static_cast<size_t> (numSamples))
    {
        dropped.fetch_add (static_cast<uint64_t> (numSamples) - written, std::memory_order_relaxed);
        discontinuity.store (true, std::memory_order_release);
    }
}

bool FeedbackGuard::analyzePending() noexcept
{
    bool cut = false;

    for (;;)
    {
        // A hole in what was handed over. Everything still waiting is from
        // before it, and §5.5's rise may not be measured across the two: it
        // goes, and the detector starts again from what comes next. Resetting
        // alone was not enough -- the second of audio queued before the hole
        // rebuilt the history, and the first block after it was compared with
        // that.
        if (discontinuity.exchange (false, std::memory_order_acq_rel))
        {
            ring.clear();
            detector.reset();
        }

        const auto got = ring.read (scratch.data(), scratch.size());
        if (got == 0)
            break;

        if (detector.process (scratch.data(), static_cast<int> (got)))
        {
            const double hz = detector.getTriggeredFrequencyHz();
            detector.reset();

            // Already cut (by the limiter, or by this guard a moment ago and
            // the bus still emptying): nothing to add.
            if (! bus.isRunawayMuted())
            {
                bus.engageRunawayCut();
                cuts.fetch_add (1, std::memory_order_relaxed);
                lastCutHz.store (hz, std::memory_order_relaxed);
                cut = true;
            }
        }
    }

    return cut;
}

void FeedbackGuard::start()
{
    if (running.load (std::memory_order_acquire))
        return;

    // What piled up while nothing was analysing is old; start from now. The
    // analysis thread is not running, so this is the ring's one consumer.
    ring.clear();
    detector.reset();
    discontinuity.store (false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock (wakeLock);
        stopRequested = false;
    }

    running.store (true, std::memory_order_release);
    worker = std::thread ([this] { run(); });
}

void FeedbackGuard::stop()
{
    if (! running.load (std::memory_order_acquire))
        return;

    {
        std::lock_guard<std::mutex> lock (wakeLock);
        stopRequested = true;
    }
    wake.notify_all();

    if (worker.joinable())
        worker.join();

    running.store (false, std::memory_order_release);
}

void FeedbackGuard::run()
{
    std::unique_lock<std::mutex> lock (wakeLock);

    while (! stopRequested)
    {
        lock.unlock();
        analyzePending();
        lock.lock();

        wake.wait_for (lock, std::chrono::milliseconds (10), [this] { return stopRequested; });
    }
}

} // namespace mma
