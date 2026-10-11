#include "TestFramework.h"
#include "Core/FeedbackDetector.h"
#include "Core/MonitorBus.h"
#include <cmath>
#include <random>
#include <vector>

using namespace mma;

namespace {

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

/// Feeds `seconds` of a signal to the detector in 64-sample blocks, the way
/// the bus arrives; returns the time it tripped, or -1.
template <typename Signal>
double runDetector (FeedbackDetector& d, double seconds, Signal signal)
{
    std::vector<float> block (64);
    long long n = 0;

    while (n < static_cast<long long> (seconds * kRate))
    {
        for (auto& s : block)
            s = signal (n++);

        if (d.process (block.data(), static_cast<int> (block.size())))
            return static_cast<double> (n) / kRate;
    }

    return -1.0;
}

double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

} // namespace

TEST_CASE (FeedbackDetector_AHowlBuildingInOneBandIsCaught)
{
    // A 2 kHz tone climbing 20 dB a second from -50 dBFS: a headphone leaking
    // into a microphone with the loop gain just over one. Caught within
    // about half a second of it standing out, and well before it reaches the
    // limiter's ceiling, where only the 500 ms runaway cut would stop it.
    FeedbackDetector d (kRate);
    const double at = runDetector (d, 3.0, [] (long long n)
    {
        const double t = static_cast<double> (n) / kRate;
        return static_cast<float> (dbToGain (-50.0 + 20.0 * t) * std::sin (kTwoPi * 2000.0 * t));
    });

    REQUIRE (at > 0.0);
    REQUIRE (at < 1.2);
    REQUIRE_NEAR (d.getTriggeredFrequencyHz(), 2000.0, 1.0);
}

TEST_CASE (FeedbackDetector_AHowlBetweenTwoBandCentresIsCaughtToo)
{
    // 1.12 kHz sits on the edge between the 1 k and 1.25 k bands.
    FeedbackDetector d (kRate);
    const double at = runDetector (d, 3.0, [] (long long n)
    {
        const double t = static_cast<double> (n) / kRate;
        return static_cast<float> (dbToGain (-50.0 + 20.0 * t) * std::sin (kTwoPi * 1122.0 * t));
    });

    REQUIRE (at > 0.0);
    REQUIRE (at < 1.5);
}

TEST_CASE (FeedbackDetector_ASteadyToneIsNotFeedback)
{
    // A held note, a test tone, a hum: narrow and loud, but not growing.
    FeedbackDetector d (kRate);
    REQUIRE (runDetector (d, 5.0, [] (long long n)
    {
        return static_cast<float> (0.3 * std::sin (kTwoPi * 440.0 * static_cast<double> (n) / kRate));
    }) < 0.0);
}

TEST_CASE (FeedbackDetector_AToneStartingFromSilenceIsNotFeedback)
{
    // Someone whistles: a step from nothing to loud. The rise happens before
    // the band has stood out for 500 ms, so there is no rise across a window
    // in which it stood out the whole time.
    FeedbackDetector d (kRate);
    REQUIRE (runDetector (d, 4.0, [] (long long n)
    {
        const double t = static_cast<double> (n) / kRate;
        return t < 1.0 ? 0.0f : static_cast<float> (0.5 * std::sin (kTwoPi * 1500.0 * t));
    }) < 0.0);
}

TEST_CASE (FeedbackDetector_BroadbandSoundGettingLouderIsNotFeedback)
{
    // Noise -- the spread spectrum and high crest factor of a voice or a room
    // getting louder -- rising 30 dB a second. No one band carries it.
    FeedbackDetector d (kRate);
    std::mt19937 rng (7);
    std::normal_distribution<float> noise (0.0f, 1.0f);

    REQUIRE (runDetector (d, 2.0, [&] (long long n)
    {
        const double t = static_cast<double> (n) / kRate;
        return static_cast<float> (dbToGain (-70.0 + 30.0 * t) * 0.25) * noise (rng);
    }) < 0.0);
}

TEST_CASE (FeedbackDetector_AHowlRisingOutOfSpeechIsCaughtOnceItStandsOut)
{
    // Feedback rarely starts in silence: it climbs out from under the people
    // talking. Until it dominates, the band is not within 6 dB of the peak;
    // once it does and keeps growing at §5.5's rate (10 dB in 500 ms or
    // faster), it is caught.
    FeedbackDetector d (kRate);
    std::mt19937 rng (11);
    std::normal_distribution<float> noise (0.0f, 1.0f);

    const double at = runDetector (d, 4.0, [&] (long long n)
    {
        const double t = static_cast<double> (n) / kRate;
        const double talk = 0.05 * noise (rng);
        const double howl = dbToGain (-60.0 + 25.0 * t) * std::sin (kTwoPi * 3150.0 * t);
        return static_cast<float> (talk + howl);
    });

    REQUIRE (at > 0.0);
    REQUIRE (at < 3.0);
}

TEST_CASE (FeedbackDetector_SilenceLeavesTheBandFiltersAtExactlyZero)
{
    // The guard analyses the headphone mix the whole time it is open, and a
    // muted mix -- the spacebar, a latched cut, every input silent -- is
    // exact zeros, sometimes for a whole show. A recursive filter fed zeros
    // does not reach zero on its own: its memory decays into denormal
    // doubles and then circulates there, and on an Intel Mac every
    // arithmetic step on one is dozens of times slower. It has to settle to
    // exactly zero instead, at every rate the monitor can run at.
    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        FeedbackDetector d (rate);
        std::mt19937 rng (5);
        std::normal_distribution<float> noise (0.0f, 0.25f);
        std::vector<float> block (64);

        // Something played first, so every band has memory to lose.
        for (int i = 0; i < static_cast<int> (2.0 * rate / 64); ++i)
        {
            for (auto& s : block)
                s = noise (rng);

            d.process (block.data(), static_cast<int> (block.size()));
        }

        REQUIRE_FALSE (d.filtersAreAtRest());

        // Then the mix is muted. Long enough for every band to be well into
        // the denormal range if nothing clears it (about 8 s at 48 kHz for
        // the slowest, 80 Hz, band).
        std::fill (block.begin(), block.end(), 0.0f);

        for (int i = 0; i < static_cast<int> (20.0 * rate / 64); ++i)
            d.process (block.data(), static_cast<int> (block.size()));

        REQUIRE (d.filtersAreAtRest());

        // And a howl after the silence is still caught.
        const double at = [&]
        {
            long long n = 0;

            while (n < static_cast<long long> (3.0 * rate))
            {
                for (auto& s : block)
                {
                    const double t = static_cast<double> (n++) / rate;
                    s = static_cast<float> (dbToGain (-50.0 + 20.0 * t) * std::sin (kTwoPi * 2000.0 * t));
                }

                if (d.process (block.data(), static_cast<int> (block.size())))
                    return static_cast<double> (n) / rate;
            }

            return -1.0;
        }();

        REQUIRE (at > 0.0);
        REQUIRE (at < 1.2);
    }
}

TEST_CASE (FeedbackGuard_CutsTheBusAndCountsIt)
{
    MonitorBus bus (kRate);
    FeedbackGuard guard (kRate, bus);

    std::vector<float> block (64);
    long long n = 0;
    bool cut = false;

    // Handed over in blocks as the audio thread would, analysed between them.
    for (int i = 0; i < static_cast<int> (2.0 * kRate / 64) && ! cut; ++i)
    {
        for (auto& s : block)
        {
            const double t = static_cast<double> (n++) / kRate;
            s = static_cast<float> (dbToGain (-50.0 + 20.0 * t) * std::sin (kTwoPi * 800.0 * t));
        }

        guard.push (block.data(), static_cast<int> (block.size()));

        if (i % 16 == 0)
            cut = guard.analyzePending();
    }

    REQUIRE (cut);
    REQUIRE (bus.isRunawayMuted());
    REQUIRE (guard.getCutCount() == 1u);
    REQUIRE_NEAR (guard.getLastCutFrequencyHz(), 800.0, 1.0);

    // The same one-tap Unmute as the limiter's cut.
    bus.manuallyUnmute();
    REQUIRE_FALSE (bus.isMuted());
}

namespace {

/// Pushes `seconds` of a 1 kHz tone at `db` into the guard in 64-sample
/// blocks, continuing from sample `n`.
void pushTone (FeedbackGuard& guard, long long& n, double seconds, double db)
{
    std::vector<float> block (64);

    for (int i = 0; i < static_cast<int> (seconds * kRate / 64); ++i)
    {
        for (auto& s : block)
            s = static_cast<float> (dbToGain (db) * std::sin (kTwoPi * 1000.0 * static_cast<double> (n++) / kRate));

        guard.push (block.data(), static_cast<int> (block.size()));
    }
}

} // namespace

TEST_CASE (FeedbackGuard_ARiseIsNotMeasuredAcrossAHoleInTheBus)
{
    // A tone that stood out quietly, then the headphone mix stopped running
    // (the output went away and the software clock stood in), then the same
    // tone 20 dB louder. Side by side the two halves read as a 20 dB rise in
    // one band; they are not one signal, and cutting the headphones the moment
    // they come back would be a false alarm.
    MonitorBus bus (kRate);
    FeedbackGuard guard (kRate, bus);
    long long n = 0;

    pushTone (guard, n, 0.8, -40.0);
    REQUIRE_FALSE (guard.analyzePending());

    guard.noteGap();
    pushTone (guard, n, 0.8, -20.0);
    REQUIRE_FALSE (guard.analyzePending());
    REQUIRE_FALSE (bus.isRunawayMuted());

    // Without the hole the same step is exactly what the rule describes.
    MonitorBus control (kRate);
    FeedbackGuard joined (kRate, control);
    long long m = 0;
    pushTone (joined, m, 0.8, -40.0);
    REQUIRE_FALSE (joined.analyzePending());
    pushTone (joined, m, 0.8, -20.0);
    REQUIRE (joined.analyzePending());
}

TEST_CASE (FeedbackGuard_FallingASecondBehindDropsWhatWasQueuedRatherThanSplicingIt)
{
    // The analysis thread did not run for a second and a half: the ring kept
    // the first second of the quiet tone and dropped the rest. What arrives
    // after that is not continuous with what was kept, so the kept second is
    // let go and analysis starts again from the new audio.
    MonitorBus bus (kRate);
    FeedbackGuard guard (kRate, bus);
    long long n = 0;

    pushTone (guard, n, 1.5, -40.0);
    REQUIRE (guard.getDroppedSamples() > 0u);

    // Nothing of the kept second is analysed into the history.
    REQUIRE_FALSE (guard.analyzePending());
    REQUIRE (guard.getPendingSamples() == 0u);

    pushTone (guard, n, 0.8, -20.0);
    REQUIRE_FALSE (guard.analyzePending());
    REQUIRE_FALSE (bus.isRunawayMuted());
}
