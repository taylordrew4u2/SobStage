#pragma once

namespace mma {

/// PI-loop ASRC ratio controller for one input, per spec §3.2. Drives a
/// resample ratio from measured ring-buffer fill error so a device's clock is
/// continuously pulled into sync with the clock consuming it, without audible
/// pitch jumps (bounded slew) or excursions beyond a safety clamp.
///
/// Every input gets one, the §3.1 clock master included -- see §3.1's note on
/// why the master is a reporting reference rather than an exemption.
class DriftCompensator
{
public:
    // Five PPM per fill sample reaches the +/-200 PPM safety clamp at 40
    // samples of phase debt: under 1 ms even at 44.1 kHz. The old 1 PPM per
    // sample gain needed 200 samples, so an ordinary short take could remain
    // over §3.4's alignment ceiling even though a long soak eventually settled.
    // The output is still independently bounded to a 20 PPM/s slew below.
    //
    // These gains are only sound against a fill error that moves smoothly.
    // Driven by the raw ring level at pull time -- which a real device moves
    // in steps of a whole block -- one step is 320 PPM of request, and the
    // loop limit-cycled: see DeviceInputStream::fillErrorNow for what it is
    // driven by instead, and Tools/sim_drift_loop.cpp for the evidence.
    static constexpr double kKp = 5.0e-6;
    static constexpr double kKi = 1.0e-8;               // integral gain, per sample of fill error
    static constexpr double kMaxRatioDeviationPpm = 200.0; // clamp: max +/-200ppm ratio deviation
    // Never correct instantaneously -- but not so slowly that a microphone
    // far off the output's clock empties its ring before the loop arrives.
    // At 5 PPM/s a device 160 PPM slow took half a minute to catch and ran
    // its ring down by 120 samples on the way: a gap in the headphones and
    // the stem at the top of every take. At 20 the same device is caught in
    // eight seconds, 30 samples down. The pitch change is nothing anyone can
    // hear: 20 PPM is 0.035 cent, the whole +/-200 PPM range 0.35 cent, and
    // the ratio moves through it smoothly, never in a step.
    static constexpr double kMaxSlewPpmPerSecond = 20.0;

    explicit DriftCompensator (double sampleRate) noexcept;

    void reset() noexcept;

    /// Test and tuning hook: the gains this instance runs with. The simulator
    /// sweeps them; production code never calls this.
    void setGains (double proportional, double integral) noexcept
    {
        kp = proportional;
        ki = integral;
    }

    /// Test and tuning hook: the slew limit this instance runs with.
    void setSlewForTesting (double ppmPerSecond) noexcept { slewPpmPerSecond = ppmPerSecond; }

    /// Feed one control-loop update. fillError is (measured fill samples - target fill samples);
    /// positive means the buffer is filling faster than it drains (device running fast).
    /// blockSizeSamples is the number of samples represented by this update, used to convert
    /// the PPM/second slew limit into a per-call step limit.
    void update (double fillError, int blockSizeSamples) noexcept;

    /// Current resample ratio to apply to this stream, e.g. 1.0 + deviation.
    double getRatio() const noexcept { return 1.0 + currentPpm * 1.0e-6; }

    /// Current deviation from unity, in parts-per-million. Used for §3.3 drift reporting.
    double getPpm() const noexcept { return currentPpm; }

    /// True once |ppm| has stayed above 100 for a sustained period (§3.3 "unreliable" flag).
    bool isSustainedExcessDrift() const noexcept { return sustainedExcessDrift; }

    /// Feed elapsed wall-clock seconds since the last drift-flag check (call once per reporting tick).
    /// referencePpm is the clock master's own deviation: §3.3 judges a device against the master, so
    /// what is tested is this loop's PPM relative to that. It defaults to zero, which judges against
    /// the stream that pulls this one instead.
    void updateSustainedDriftFlag (double elapsedSeconds, double referencePpm = 0.0) noexcept;

private:
    double sampleRate;
    double kp = kKp;
    double ki = kKi;
    double slewPpmPerSecond = kMaxSlewPpmPerSecond;
    double integralTerm = 0.0;
    double currentPpm = 0.0;
    double excessDriftSeconds = 0.0;
    bool sustainedExcessDrift = false;

    static constexpr double kExcessDriftThresholdPpm = 100.0;
    // Spec says "100 PPM sustained" without naming a duration; §3.3 reports drift every 10s,
    // so we sustain-check on that same cadence (judgment call, documented in README).
    static constexpr double kExcessDriftSustainSeconds = 10.0;
};

} // namespace mma
