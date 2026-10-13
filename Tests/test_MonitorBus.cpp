#include "TestFramework.h"
#include "Core/MonitorBus.h"
#include <atomic>
#include <cmath>
#include <thread>

using namespace mma;

namespace {

const float kCeiling = std::pow (10.0f, MonitorBus::kLimiterCeilingDb / 20.0f);

/// processSample now returns what reaches the ears: the bus after the master
/// volume and the limiter. At full volume that is the bus itself, so the
/// tests about summing, the ceiling and the runaway cut run there.
MonitorBus& atFullVolume (MonitorBus& bus)
{
    bus.setMasterVolume (100.0);
    return bus;
}

} // namespace

TEST_CASE (MonitorBus_SumsChannelsAtUnity)
{
    MonitorBus bus (48000.0);
    atFullVolume (bus);
    float out = bus.processSample ({ 0.1f, 0.1f, 0.1f });
    REQUIRE_NEAR (out, 0.3, 1e-5);
}

TEST_CASE (MonitorBus_LimitsAboveMinus3dBFS)
{
    // Pinned the limiter at the default volume before. The ceiling is now
    // tested after the volume (on what reaches the ears), and at the default
    // volume 2.7 reaches them at 0.34 -- under it -- so this runs at full.
    MonitorBus bus (48000.0);
    atFullVolume (bus);
    float out = bus.processSample ({ 0.9f, 0.9f, 0.9f }); // sums to 2.7, way over ceiling
    REQUIRE_NEAR (std::abs (out), kCeiling, 1e-4);
}

TEST_CASE (MonitorBus_GlobalMuteSilencesOutput)
{
    MonitorBus bus (48000.0);
    bus.setGlobalMute (true);
    float out = bus.processSample ({ 0.5f });
    REQUIRE_NEAR (out, 0.0, 1e-9);
    REQUIRE (bus.isMuted());
}

TEST_CASE (MonitorBus_RunawayCutEngagesAfter500msOfContinuousLimiting)
{
    MonitorBus bus (48000.0);
    atFullVolume (bus);
    const int samplesFor500ms = static_cast<int> (48000.0 * 0.5);
    for (int i = 0; i < samplesFor500ms + 10; ++i)
        bus.processSample ({ 5.0f }); // sustained massive overload

    REQUIRE (bus.isRunawayMuted());
    REQUIRE (bus.isMuted());
}

TEST_CASE (MonitorBus_RunawayCutRequiresManualUnmute)
{
    MonitorBus bus (48000.0);
    atFullVolume (bus);
    const int samplesFor500ms = static_cast<int> (48000.0 * 0.5);
    for (int i = 0; i < samplesFor500ms + 10; ++i)
        bus.processSample ({ 5.0f });
    REQUIRE (bus.isRunawayMuted());

    // Even quiet input afterward doesn't clear it automatically.
    bus.processSample ({ 0.0f });
    REQUIRE (bus.isRunawayMuted());

    bus.manuallyUnmute();
    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_AMuteClearsTheRunawayEvidence)
{
    // 400 ms of limiting, then the spacebar. While muted nothing reaches the
    // room, so nothing is running away; the evidence used to sit frozen
    // through the mute and resume afterwards, so 100 ms of ordinary loud
    // audio after unmuting -- however long later -- completed a "500 ms"
    // run and cut everyone's headphones.
    MonitorBus bus (48000.0);
    atFullVolume (bus);
    const int samplesFor400ms = static_cast<int> (48000.0 * 0.4);
    for (int i = 0; i < samplesFor400ms; ++i)
        bus.processSample ({ 5.0f });
    REQUIRE_FALSE (bus.isRunawayMuted());

    bus.setGlobalMute (true);
    for (int i = 0; i < 4800; ++i)
        bus.processSample ({ 5.0f }); // still loud at the microphones, silent in the room
    bus.setGlobalMute (false);

    const int samplesFor200ms = static_cast<int> (48000.0 * 0.2);
    for (int i = 0; i < samplesFor200ms; ++i)
        bus.processSample ({ 5.0f });
    REQUIRE_FALSE (bus.isRunawayMuted());

    // A real runaway after the mute is still caught, timed from the unmute.
    for (int i = 0; i < samplesFor400ms; ++i)
        bus.processSample ({ 5.0f });
    REQUIRE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_BriefLimitingDoesNotTriggerRunawayCut)
{
    MonitorBus bus (48000.0);
    atFullVolume (bus);
    for (int i = 0; i < 100; ++i) // ~2ms, well under 500ms
        bus.processSample ({ 5.0f });

    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_VolumeMappingIsMonotonicAndLogarithmic)
{
    float g0 = MonitorBus::monitorVolumeToLinearGain (0.0);
    float g50 = MonitorBus::monitorVolumeToLinearGain (50.0);
    float g70 = MonitorBus::monitorVolumeToLinearGain (MonitorBus::kDefaultMonitorVolume);
    float g100 = MonitorBus::monitorVolumeToLinearGain (100.0);

    REQUIRE_NEAR (g0, 0.0, 1e-9);
    REQUIRE (g50 < g70);
    REQUIRE (g70 < g100);
    REQUIRE_NEAR (g100, 1.0, 1e-4);
}

TEST_CASE (MonitorBus_TrimRangeClampedToPlusMinus20dB)
{
    float gainAt20 = MonitorBus::trimDbToLinearGain (20.0f);
    float gainAt100 = MonitorBus::trimDbToLinearGain (100.0f); // out of range, should clamp
    REQUIRE_NEAR (gainAt20, gainAt100, 1e-4);
}

TEST_CASE (MonitorBus_FeedbackDetectionTriggersOnSustainedNarrowbandRise)
{
    MonitorBus bus (48000.0);
    // Baseline band level close to broadband peak, then a >10dB rise over a
    // continuous 500ms window while staying within 6dB of the broadband peak.
    REQUIRE_FALSE (bus.processFeedbackCandidate (-40.0, -40.0, 0.0));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-35.0, -35.0, 0.1));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-30.0, -30.0, 0.1));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-27.0, -27.0, 0.1));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-25.0, -25.0, 0.1));
    bool triggered = bus.processFeedbackCandidate (-20.0, -20.0, 0.1);

    REQUIRE (triggered);
    REQUIRE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_FeedbackDetectionDoesNotTriggerWhenFarFromBroadbandPeak)
{
    MonitorBus bus (48000.0);
    bool triggered = false;
    for (int i = 0; i < 10; ++i)
        triggered = bus.processFeedbackCandidate (-40.0 + i * 3.0, -5.0, 0.1); // always >6dB below peak

    REQUIRE_FALSE (triggered);
    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_AnOlderManualResetCannotClearANewerFeedbackTrip)
{
    MonitorBus bus (48000.0);

    // Establish and trip one feedback window, then acknowledge it. That posts
    // the limiter-counter reset the audio thread will consume next.
    bus.processFeedbackCandidate (-40.0, -40.0, 0.0);
    REQUIRE (bus.processFeedbackCandidate (-20.0, -20.0, 0.5));
    bus.manuallyUnmute();
    REQUIRE_FALSE (bus.isRunawayMuted());

    // A new detector trip can arrive before the next audio sample. Consuming
    // the older counter-reset request must not erase this newer safety event.
    REQUIRE (bus.processFeedbackCandidate (-5.0, -5.0, 0.5));
    REQUIRE (bus.isRunawayMuted());
    REQUIRE (bus.processSample ({ 0.0f }) == 0.0f);
    REQUIRE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_ControlsCanChangeWhileTheAudioThreadRuns)
{
    MonitorBus bus (48000.0);
    std::atomic<bool> start { false };
    float lastOutput = 0.0f;

    std::thread audio ([&] {
        while (! start.load (std::memory_order_acquire)) {}
        for (int i = 0; i < 20000; ++i)
            lastOutput = bus.processSample ({ 0.1f });
    });

    start.store (true, std::memory_order_release);
    for (int i = 0; i < 2000; ++i)
    {
        bus.setMasterVolume (static_cast<double> (i % 101));
        bus.setGlobalMute ((i % 7) == 0);
        if ((i % 31) == 0)
            bus.manuallyUnmute();
    }

    audio.join();
    REQUIRE (std::isfinite (lastOutput));
}

TEST_CASE (MonitorBus_DefaultMasterVolumeIsSeventy)
{
    MonitorBus bus (48000.0);
    REQUIRE_NEAR (bus.getMasterVolume(), MonitorBus::kDefaultMonitorVolume, 1e-9);
}

TEST_CASE (MonitorBus_MasterVolumeIsClampedToZeroHundred)
{
    MonitorBus bus (48000.0);

    bus.setMasterVolume (250.0);
    REQUIRE_NEAR (bus.getMasterVolume(), 100.0, 1e-9);

    bus.setMasterVolume (-40.0);
    REQUIRE_NEAR (bus.getMasterVolume(), 0.0, 1e-9);
}

TEST_CASE (MonitorBus_TheBusBeforeTheVolumeIsTheSameAtEveryVolume)
{
    // Replaces MasterVolumeNeverAffectsTheBusItself, which pinned the volume
    // as an output stage after the limiter -- the arrangement that let the
    // -3 dBFS ceiling square off the full-level bus whatever the listener's
    // volume. The volume now comes before the limiter, so what reaches the
    // ears moves with it; the summed bus the meter and the feedback guard
    // read still does not.
    MonitorBus loud (48000.0), quiet (48000.0);
    loud.setMasterVolume (100.0);
    quiet.setMasterVolume (5.0);

    const std::vector<float> in { 0.2f, 0.1f };
    float loudBus = 0.0f, quietBus = 0.0f;
    const float loudOut = loud.processSample (in, &loudBus);
    const float quietOut = quiet.processSample (in, &quietBus);

    REQUIRE_NEAR (loudBus, 0.3f, 1e-6);
    REQUIRE_NEAR (quietBus, 0.3f, 1e-6);
    REQUIRE (quietOut < loudOut);
}

TEST_CASE (MonitorBus_TurningTheVolumeDownTakesTheMixOutOfTheLimiter)
{
    // The user report: several microphones in one room, the headphones
    // crackle, and turning them down does not help. The bus ran into the
    // -3 dBFS clip BEFORE the volume, so a bus at +3.5 dBFS was squared off
    // and then made quieter. With the volume first, the default volume puts
    // that bus at -14.5 dBFS in the ears: nothing touches it.
    MonitorBus bus (48000.0); // default volume, 70
    const float gain = MonitorBus::monitorVolumeToLinearGain (MonitorBus::kDefaultMonitorVolume);

    float worstError = 0.0f;
    for (int i = 0; i < 4800; ++i)
    {
        const float x = 1.5f * std::sin (6.2831853f * 200.0f * static_cast<float> (i) / 48000.0f);
        const float out = bus.processSample ({ x * 0.5f, x * 0.5f });
        worstError = std::max (worstError, std::abs (out - x * gain));
    }

    REQUIRE (worstError < 1e-6f);
    REQUIRE (bus.getLimiterEngagementCount() == 0u);
    REQUIRE_FALSE (bus.isRunawayMuted());

    // The same bus at full volume does reach the ceiling, and is held to it.
    MonitorBus full (48000.0);
    atFullVolume (full);
    float loudest = 0.0f;
    for (int i = 0; i < 4800; ++i)
    {
        const float x = 1.5f * std::sin (6.2831853f * 200.0f * static_cast<float> (i) / 48000.0f);
        loudest = std::max (loudest, std::abs (full.processSample ({ x })));
    }

    REQUIRE (loudest <= kCeiling + 1e-6f);
    REQUIRE (full.getLimiterEngagementCount() == 1u);
}

TEST_CASE (MonitorBus_ALoudPassageIsTurnedDownNotSquaredOff)
{
    // A 200 Hz tone 6 dB over the ceiling. The hard clip held a third of
    // every cycle flat on the ceiling, for as long as it lasted. The limiter
    // sets its gain on the first crest and holds it, so every later cycle
    // is the same sine, turned down, with exactly one sample per crest on
    // the ceiling.
    MonitorBus bus (48000.0);
    atFullVolume (bus);

    const float amplitude = 2.0f * kCeiling;
    int flatOnFirstCrest = 0, flatAfterwards = 0;
    float worstShapeError = 0.0f;
    float previous = 0.0f;

    for (int i = 0; i < 48000 / 4; ++i)
    {
        const float x = amplitude * std::sin (6.2831853f * 200.0f * static_cast<float> (i) / 48000.0f);
        const float out = bus.processSample ({ x });

        if (std::abs (out) >= kCeiling * 0.99999f && std::abs (previous) >= kCeiling * 0.99999f)
            ++(i < 240 ? flatOnFirstCrest : flatAfterwards);

        if (i >= 240) // after the first cycle
            worstShapeError = std::max (worstShapeError, std::abs (out - x * (kCeiling / amplitude)));

        previous = out;
    }

    // With no lookahead, the rise of the very first crest is all the limiter
    // cannot see coming (40 samples at 200 Hz). The hard clip did the same on
    // every crest of every cycle: 80 samples in each 240, here 4000.
    REQUIRE (flatOnFirstCrest <= 40);
    REQUIRE (flatAfterwards == 0);
    REQUIRE (worstShapeError < 1e-3f);   // the waveform, turned down
    REQUIRE (bus.getLimiterEngagementCount() == 1u);
}

TEST_CASE (MonitorBus_TheLimiterLetsGoAfterTheLoudPassage)
{
    // Instant attack, a 10 ms hold, a 50 ms release: a moment of overload
    // turns the headphones down for a fraction of a second, then they come
    // back to unity -- and the next loud passage is counted again.
    MonitorBus bus (48000.0);
    atFullVolume (bus);

    REQUIRE_NEAR (bus.processSample ({ 2.0f * kCeiling }), kCeiling, 1e-6);
    REQUIRE (bus.getLimiterEngagementCount() == 1u);

    // Inside the hold, a quiet sample is still turned down by half.
    REQUIRE_NEAR (bus.processSample ({ 0.1f }), 0.05f, 1e-4);

    // Half a second later it is back to unity.
    for (int i = 0; i < 24000; ++i)
        bus.processSample ({ 0.0f });
    REQUIRE_NEAR (bus.processSample ({ 0.1f }), 0.1f, 1e-5);

    bus.processSample ({ 2.0f * kCeiling });
    REQUIRE (bus.getLimiterEngagementCount() == 2u);
    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_TheRunawayCutIsTimedOnWhatReachesTheEars)
{
    // A sustained overload that the volume keeps under the ceiling is not a
    // runaway in anyone's ears, and is not cut...
    MonitorBus quiet (48000.0);
    quiet.setMasterVolume (40.0); // -36 dB
    for (int i = 0; i < 48000; ++i)
        quiet.processSample ({ 5.0f });
    REQUIRE_FALSE (quiet.isRunawayMuted());

    // ...but a fault loud enough to reach the ceiling at the default volume
    // still is, after the same 500 ms as ever: eight microphones at full
    // scale reach the ears at 0 dBFS through the default -18 dB.
    MonitorBus atDefault (48000.0);
    const std::vector<float> eightAtFullScale (8, 1.0f);
    const int samplesFor500ms = static_cast<int> (48000.0 * 0.5);
    for (int i = 0; i < samplesFor500ms - 10; ++i)
        atDefault.processSample (eightAtFullScale);
    REQUIRE_FALSE (atDefault.isRunawayMuted());
    for (int i = 0; i < 20; ++i)
        atDefault.processSample (eightAtFullScale);
    REQUIRE (atDefault.isRunawayMuted());
    REQUIRE (atDefault.processSample (eightAtFullScale) == 0.0f);
}

TEST_CASE (MonitorBus_TheBusBeforeTheVolumeIsSilentWhileNothingReachesTheEars)
{
    MonitorBus bus (48000.0);
    float before = -1.0f;

    REQUIRE (bus.processSample ({ 0.2f, 0.3f }, &before) > 0.0f);
    REQUIRE_NEAR (before, 0.5f, 1e-6);

    bus.setGlobalMute (true);
    REQUIRE (bus.processSample ({ 0.2f, 0.3f }, &before) == 0.0f);
    REQUIRE (before == 0.0f);
    bus.setGlobalMute (false);

    before = -1.0f;
    REQUIRE (bus.processSample ({ std::nanf (""), 0.3f }, &before) == 0.0f);
    REQUIRE (before == 0.0f);
}

TEST_CASE (MonitorBus_MasterVolumeScalesTheOutputMonotonically)
{
    MonitorBus bus (48000.0);

    bus.setMasterVolume (100.0);
    const float atFull = bus.processSample ({ 0.5f });

    bus.setMasterVolume (50.0);
    const float atHalf = bus.processSample ({ 0.5f });

    bus.setMasterVolume (0.0);
    const float atZero = bus.processSample ({ 0.5f });

    REQUIRE (atFull > atHalf);
    REQUIRE (atHalf > atZero);
    REQUIRE_NEAR (atZero, 0.0f, 1e-6);
}

TEST_CASE (MonitorBus_MasterVolumeAtFullIsUnityGain)
{
    MonitorBus bus (48000.0);
    bus.setMasterVolume (100.0);
    REQUIRE_NEAR (bus.processSample ({ 0.5f }), 0.5f, 1e-4);
}

TEST_CASE (MonitorBus_SeparateBurstsDoNotCombineIntoARunawayCut)
{
    // Regression: the 1 ms gap rule (then named kLimiterReleaseSeconds, now
    // kRunawayGapSeconds) was declared and never used. Instead of
    // clearing 1 ms after the last clip, the engagement accumulator drained one
    // sample-time per quiet sample -- so it took as long to unwind as it took to
    // build. An engagement that ended long ago kept most of its credit, and the
    // next unrelated burst resumed from there.
    //
    // Two 300 ms bursts 10 ms apart is the case: neither is anywhere near the
    // §5 threshold of 500 ms *continuous* engagement, but the old accumulator
    // only shed 10 ms in the gap and crossed 500 ms partway through the second
    // burst -- cutting the monitor and forcing a manual unmute, mid-take, over
    // two ordinary loud moments.
    MonitorBus bus (48000.0);
    atFullVolume (bus);

    const int burstSamples = static_cast<int> (48000.0 * 0.3);   // 300 ms, under the threshold
    const int gapSamples   = static_cast<int> (48000.0 * 0.01);  // 10 ms, well over the 1 ms release

    for (int i = 0; i < burstSamples; ++i)
        bus.processSample ({ 5.0f });

    REQUIRE_FALSE (bus.isRunawayMuted());

    for (int i = 0; i < gapSamples; ++i)
        bus.processSample ({ 0.0f });

    for (int i = 0; i < burstSamples; ++i)
        bus.processSample ({ 5.0f });

    // The gap ended the first engagement outright, so the second burst is judged
    // on its own 300 ms and must not cut.
    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_ClippingSurvivesGapsShorterThanTheRelease)
{
    // The flip side: a gap shorter than the 1 ms release is part of the same
    // engagement, so genuinely sustained overload still cuts even though it is
    // not clipping on literally every sample.
    MonitorBus bus (48000.0);
    atFullVolume (bus);

    // 10 clipped samples, then 4 quiet ones (~83 us, well inside the 1 ms
    // release), repeated well past 500 ms of accumulated engagement.
    const int cycles = 60000;
    for (int c = 0; c < cycles && ! bus.isRunawayMuted(); ++c)
    {
        for (int i = 0; i < 10; ++i)
            bus.processSample ({ 5.0f });
        for (int i = 0; i < 4; ++i)
            bus.processSample ({ 0.0f });
    }

    REQUIRE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_MasterVolumeGainTracksTheSetting)
{
    // processSample reads a cached gain rather than recomputing a std::pow
    // per sample; the cache must follow every setMasterVolume call.
    MonitorBus bus (48000.0);

    bus.setMasterVolume (100.0);
    REQUIRE_NEAR (bus.getMasterGain(), 1.0f, 1e-4);
    REQUIRE_NEAR (bus.processSample ({ 0.5f }), 0.5f, 1e-4);

    bus.setMasterVolume (0.0);
    REQUIRE_NEAR (bus.getMasterGain(), 0.0f, 1e-6);
    REQUIRE_NEAR (bus.processSample ({ 0.5f }), 0.0f, 1e-6);

    bus.setMasterVolume (70.0);
    REQUIRE_NEAR (bus.getMasterGain(), MonitorBus::monitorVolumeToLinearGain (70.0), 1e-6);
    REQUIRE_NEAR (bus.processSample ({ 0.5f }), 0.5f * MonitorBus::monitorVolumeToLinearGain (70.0), 1e-6);
}
