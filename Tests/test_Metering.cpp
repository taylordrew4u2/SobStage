#include "TestFramework.h"
#include "Core/Metering.h"
#include "Core/ElapsedClock.h"
#include <vector>

using namespace mma;

TEST_CASE (Metering_StartsAtMinDb)
{
    Metering m (48000.0);
    REQUIRE_NEAR (m.getDisplayedLevelDb(), Metering::kMinDb, 1e-6);
}

TEST_CASE (Metering_RisesTowardFullScaleQuicklyOnAttack)
{
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 128);
    // 10ms attack time-constant: after ~50ms (5 time constants) should be very close to 0dBFS.
    float level = 0.0f;
    for (int i = 0; i < 50; ++i)
        level = m.tick (0.001);
    REQUIRE (level > -1.0f);
}

TEST_CASE (Metering_DecaysSlowlyAfterSignalStops)
{
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 128);
    for (int i = 0; i < 50; ++i)
        m.tick (0.001);
    REQUIRE (m.getDisplayedLevelDb() > -1.0f);

    m.pushBlockStats (0.0f, 128);
    float levelAfter10ms = m.tick (0.010);
    // 1.5s decay time-constant: 10ms in, level should barely have moved.
    REQUIRE (levelAfter10ms < 0.0f);
    REQUIRE (levelAfter10ms > -3.0f);
}

TEST_CASE (Metering_ClipLatchesAtThreeConsecutiveSamplesAboveThreshold)
{
    Metering m (48000.0);
    std::vector<float> samples = { 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };
    m.processAudioBlock (samples.data(), static_cast<int> (samples.size()));
    REQUIRE (m.isClipped());
    REQUIRE (m.getClipCount() == 1);
}

TEST_CASE (Metering_DoesNotClipOnTwoConsecutiveSamples)
{
    Metering m (48000.0);
    std::vector<float> samples = { 1.0f, 1.0f, 0.0f, 0.0f };
    m.processAudioBlock (samples.data(), static_cast<int> (samples.size()));
    REQUIRE_FALSE (m.isClipped());
}

TEST_CASE (Metering_AcknowledgeClipClearsLatch)
{
    Metering m (48000.0);
    std::vector<float> samples = { 1.0f, 1.0f, 1.0f };
    m.processAudioBlock (samples.data(), static_cast<int> (samples.size()));
    REQUIRE (m.isClipped());
    m.acknowledgeClip();
    REQUIRE_FALSE (m.isClipped());
}

TEST_CASE (Metering_PeakHoldStaysUpThenDecaysAfterTwoSeconds)
{
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 128); // peak at 0dBFS
    m.tick (0.001);
    REQUIRE_NEAR (m.getPeakHoldDb(), 0.0, 0.5);

    m.pushBlockStats (0.0f, 128);
    for (int i = 0; i < 190; ++i) // 1.9s, still within the 2s hold
        m.tick (0.010);
    REQUIRE_NEAR (m.getPeakHoldDb(), 0.0, 0.5);

    for (int i = 0; i < 200; ++i) // past the hold, now decaying at 20dB/s
        m.tick (0.010);
    REQUIRE (m.getPeakHoldDb() < 0.0f);
}

TEST_CASE (Metering_ShortPeakBetweenUiTicksStillReachesPeakHold)
{
    // At 64-sample buffers the audio thread delivers ~12 blocks per 60Hz UI
    // tick. A loud transient confined to one of them must still register.
    Metering m (48000.0);
    std::vector<float> loud (64, 0.0f);
    loud[10] = 1.0f;
    std::vector<float> quiet (64, 0.001f);

    m.processAudioBlock (loud.data(), static_cast<int> (loud.size()));
    for (int i = 0; i < 11; ++i)
        m.processAudioBlock (quiet.data(), static_cast<int> (quiet.size()));
    m.tick (1.0 / 60.0);

    REQUIRE (m.getPeakHoldDb() > -1.5f);
}

TEST_CASE (Metering_PeakConsumedByTickIsNotReportedAgain)
{
    // The max is taken since the last UI read, not for all time: once a tick
    // has seen the transient, later quiet blocks must be what the next tick sees.
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 64);
    m.pushBlockStats (0.0f, 64);
    m.tick (1.0 / 60.0);
    m.pushBlockStats (0.0f, 64);
    for (int i = 0; i < 600; ++i) // 10s: hold expires and decays to the floor
        m.tick (1.0 / 60.0);
    REQUIRE (m.getPeakHoldDb() < -50.0f);
}

namespace {

double fakeNowSeconds = 0.0;
double fakeNow() { return fakeNowSeconds; }

} // namespace

TEST_CASE (ElapsedClock_MeasuresRealTimeNotCallbacks)
{
    fakeNowSeconds = 100.0;
    ElapsedClock clock (1.0 / 60.0, 5.0, &fakeNow);

    // Nothing to measure from yet.
    REQUIRE_NEAR (clock.tick(), 1.0 / 60.0, 1e-12);

    fakeNowSeconds += 0.25; // a timer asked for 60 Hz, firing at 4
    REQUIRE_NEAR (clock.tick(), 0.25, 1e-12);

    // A machine asleep for an hour is one capped step, not an hour of
    // observation handed to a detector.
    fakeNowSeconds += 3600.0;
    REQUIRE_NEAR (clock.tick(), 5.0, 1e-12);

    // A cadence by the clock: due once its period has passed, however many
    // callbacks that took.
    REQUIRE_FALSE (clock.isDue (0.5));
    fakeNowSeconds += 0.49;
    REQUIRE_FALSE (clock.isDue (0.5));
    fakeNowSeconds += 0.02;
    REQUIRE (clock.isDue (0.5));
}

TEST_CASE (Metering_PeakHoldLastsTwoRealSecondsWhenTheTimerIsThrottled)
{
    // App Nap throttles the 60 Hz meter timer to a few callbacks a second. At
    // 1/60 s a callback the 2 s hold lasted 120 callbacks -- half a minute at
    // 4 Hz -- so a peak from long ago sat on the meter as if it were now.
    fakeNowSeconds = 50.0;
    ElapsedClock clock (1.0 / 60.0, 5.0, &fakeNow);
    Metering m (48000.0);

    m.pushBlockStats (1.0f, 64);
    m.tick (clock.tick());
    REQUIRE (m.getPeakHoldDb() > -1.0f);

    // Three real seconds at 4 callbacks a second, the room silent.
    for (int i = 0; i < 12; ++i)
    {
        fakeNowSeconds += 0.25;
        m.pushBlockStats (0.0f, 64);
        m.tick (clock.tick());
    }

    // Held for two seconds, then falling at 20 dB/s for one.
    REQUIRE (m.getPeakHoldDb() < -15.0f);
    REQUIRE (m.getPeakHoldDb() > -25.0f);
}

TEST_CASE (Metering_ALongStepOnlyDecaysThePartPastTheHold)
{
    // One 2.5 s step -- a stalled message thread -- is two seconds of hold
    // and half a second of decay, not two and a half seconds of decay.
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 64);
    m.tick (1.0 / 60.0);
    m.pushBlockStats (0.0f, 64);
    m.tick (2.5);
    REQUIRE_NEAR (m.getPeakHoldDb(), -10.0, 0.5);
}

TEST_CASE (Metering_ALevelOnlyMeterShowsTheLevelButNeverLatchesItsOwnClip)
{
    // The mix bar: its CLIP is the headphone limiter's, not the -0.1 dBFS
    // rule, so its own samples running past full scale do not latch it.
    Metering m (48000.0);
    const std::vector<float> hot (64, 1.5f);
    m.processLevelOnly (hot.data(), static_cast<int> (hot.size()));
    for (int i = 0; i < 30; ++i)
        m.tick (1.0 / 60.0);

    REQUIRE_NEAR (m.getDisplayedLevelDb(), Metering::kMaxDb, 0.01);
    REQUIRE_FALSE (m.isClipped());
    REQUIRE (m.getClipCount() == 0);
}

TEST_CASE (Metering_ALevelOnlyMeterShowsTheSamplesAtTheGainItIsGiven)
{
    // The mix bar is handed the bus and the headphone volume's gain, and
    // shows the bus at that volume: what the safety limiter judges.
    Metering m (48000.0);
    const std::vector<float> bus (64, 1.5f);
    m.processLevelOnly (bus.data(), static_cast<int> (bus.size()), 0.25f);
    for (int i = 0; i < 30; ++i)
        m.tick (1.0 / 60.0);

    REQUIRE_NEAR (m.getDisplayedLevelDb(), 20.0f * std::log10 (1.5f * 0.25f), 0.05);
    REQUIRE_NEAR (m.getPeakHoldDb(), 20.0f * std::log10 (1.5f * 0.25f), 0.05);
    REQUIRE_FALSE (m.isClipped());
}

TEST_CASE (Metering_AClipNotedFromOutsideLatchesAndCounts)
{
    // The headphone limiter's engagements, handed to the mix bar.
    Metering m (48000.0);
    m.noteClipEvents (0);
    REQUIRE_FALSE (m.isClipped());

    m.noteClipEvents (2);
    REQUIRE (m.isClipped());
    REQUIRE (m.getClipCount() == 2);

    m.acknowledgeClip();
    REQUIRE_FALSE (m.isClipped());

    m.noteClipEvents (1);
    REQUIRE (m.isClipped());
    REQUIRE (m.getClipCount() == 3);
}
