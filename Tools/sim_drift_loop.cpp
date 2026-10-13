// The §3.2 loop against devices that deliver the way devices deliver: whole
// blocks, at their own clock's cadence, on a time axis the consumer's clock
// does not share. Deterministic and fast -- five simulated minutes take well
// under a second -- so it can sweep gains and be a test.
//
//   ./sim_drift_loop [seconds] [block] [ppm-list] [kp-ppm/sample] [ki] [jitter-ms] [stall-ms] [stall-per-s] [virtual 0|1] [slew-ppm/s]
//
// Defaults: 300 s, 64, "150,-150,60,-60,0", the compensator's own gains, no
// jitter, virtual fill on. Exits 1 if any device loses audio after its first
// minute, its loop sits more than 20 PPM from its clock over the last minute
// (RMS), or it is measured more than 10 PPM wrong -- the three things
// §3.2/§3.3 promise.
//
// Over the last minute, not at the last instant: under jitter and stalls the
// loop's figure moves with every stall the level shows it, and one sample of
// it is luck. At 512 samples with 2 ms of jitter and 8 ms stalls the loop is
// beyond 20 PPM for 10-16% of the time whatever its slew -- 5 PPM/s or 20 --
// and which side of that the run's last tick fell on decided the test.
//
// Why Tools/soak_drift.cpp did not find what this finds: it feeds drift as
// one extra or missing sample folded into a block that is pushed and pulled
// in lockstep, so the ring level moves one sample at a time. A real device's
// blocks land at a slipping time phase against the pull clock, so the level
// a pull sees moves in steps of a whole block -- the staircase the loop was
// unstable against.
#include "Core/DeviceInputStream.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mma;

namespace {

int64_t simNowNs = 0;
int64_t simClock() { return simNowNs; }

// A small deterministic generator, so a run is a run.
struct Rng
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    double uniform() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (s >> 11) * (1.0 / 9007199254740992.0); }
};

struct Jitter
{
    double jitterS = 0.0;     // every event lands late by uniform [0, jitterS]
    double stallS = 0.0;      // a stall delays an event by uniform [0, stallS]
    double stallsPerSecond = 0.0;
};

struct Device
{
    double ppm = 0.0;
    double periodS = 0.0;
    int64_t k = 0;            // blocks pushed
    double nextDueS = 0.0;
    DeviceInputStream stream { 48000.0 };
    std::vector<float> in;

    uint64_t underAtMinute = 0, overAtMinute = 0;
    double measuredAtMinute = 0.0;
    bool measuredAtMinuteValid = false;
    double lastOutsideS = -1.0;  // last time the loop ppm was more than 10 off its clock
    double minFillAfterMinute = 1.0, maxFillAfterMinute = 0.0;

    // The device delivers a ramp -- each sample its own index -- so the
    // output says which source sample it came from. A step between one
    // output sample and the next that is neither the ramp's own slope, give
    // or take a sample of resampling, nor silence, is a seam: audio out of
    // order, which no counter here would otherwise see.
    int64_t pushed = 0;
    float lastOut = -1.0f;
    uint64_t seams = 0, seamsAfterMinute = 0;

    // The loop's figure against the device's clock over the run's last
    // minute: the sum of squares and the count of reporting ticks.
    double lastMinuteSquares = 0.0;
    int lastMinuteTicks = 0;
};

std::vector<double> parsePpm (const char* text)
{
    std::vector<double> out;
    std::string s (text);
    size_t start = 0;
    while (start <= s.size())
    {
        auto end = s.find (',', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) out.push_back (std::atof (s.substr (start, end - start).c_str()));
        start = end + 1;
    }
    return out;
}

} // namespace

int main (int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof (argv[1]) : 300.0;
    const int block = argc > 2 ? std::atoi (argv[2]) : 64;
    const auto ppms = parsePpm (argc > 3 ? argv[3] : "150,-150,60,-60,0");
    const double kpPpm = argc > 4 ? std::atof (argv[4]) : DriftCompensator::kKp * 1.0e6;
    const double ki = argc > 5 ? std::atof (argv[5]) : DriftCompensator::kKi;
    Jitter jitter;
    jitter.jitterS = (argc > 6 ? std::atof (argv[6]) : 0.0) / 1000.0;
    jitter.stallS = (argc > 7 ? std::atof (argv[7]) : 0.0) / 1000.0;
    jitter.stallsPerSecond = argc > 8 ? std::atof (argv[8]) : 0.0;
    const bool virtualFill = argc > 9 ? std::atoi (argv[9]) != 0 : true;
    const double slew = argc > 10 ? std::atof (argv[10]) : DriftCompensator::kMaxSlewPpmPerSecond;

    const double rate = 48000.0;
    const double consumerPeriodS = static_cast<double> (block) / rate;

    DeviceInputStream::setClockForTesting (simClock);
    DeviceInputStream::setVirtualFillForTesting (virtualFill);

    std::vector<Device> devices (ppms.size());
    for (size_t i = 0; i < devices.size(); ++i)
    {
        auto& d = devices[i];
        d.ppm = ppms[i];
        d.periodS = consumerPeriodS / (1.0 + d.ppm * 1.0e-6);
        d.stream.prepare (rate, block);
        d.in.assign (static_cast<size_t> (block), 0.1f);
        // Every device starts at a different phase, as it would.
        d.nextDueS = d.periodS * (0.13 + 0.71 * static_cast<double> (i)) ;
        while (d.nextDueS >= d.periodS) d.nextDueS -= d.periodS;
    }

    // A negative gain argument keeps the compensator's own default.
    for (auto& d : devices)
    {
        d.stream.setLoopGainsForTesting (kpPpm >= 0.0 ? kpPpm * 1.0e-6 : DriftCompensator::kKp,
                                         ki >= 0.0 ? ki : DriftCompensator::kKi);
        d.stream.setLoopSlewForTesting (slew);
    }

    std::vector<float> out (static_cast<size_t> (block));
    Rng rng;
    double consumerDueS = consumerPeriodS * 0.5;
    double consumerActualS = consumerDueS;
    double nextReportS = 0.1;
    double simS = 0.0;

    auto lateBy = [&] (double& stallCarry)
    {
        double late = rng.uniform() * jitter.jitterS;
        if (jitter.stallsPerSecond > 0.0 && rng.uniform() < jitter.stallsPerSecond * consumerPeriodS)
            stallCarry = rng.uniform() * jitter.stallS;
        late += stallCarry;
        // A stall carries into the events that follow it until the schedule
        // is caught up, the way a descheduled thread bursts afterwards.
        stallCarry = std::max (0.0, stallCarry - consumerPeriodS);
        return late;
    };

    std::vector<double> deviceStall (devices.size(), 0.0);
    double consumerStall = 0.0;
    std::vector<double> deviceActualS (devices.size());
    for (size_t i = 0; i < devices.size(); ++i)
        deviceActualS[i] = devices[i].nextDueS;

    while (simS < seconds)
    {
        // Next event: the earliest actual (jittered) time among producers and
        // the consumer.
        size_t who = devices.size(); // devices.size() means the consumer
        double when = consumerActualS;
        for (size_t i = 0; i < devices.size(); ++i)
            if (deviceActualS[i] < when) { when = deviceActualS[i]; who = i; }

        simS = when;
        simNowNs = static_cast<int64_t> (simS * 1.0e9);

        if (who < devices.size())
        {
            auto& d = devices[who];
            for (int n = 0; n < block; ++n)
                d.in[static_cast<size_t> (n)] = static_cast<float> (d.pushed + n);
            d.pushed += block;
            d.stream.pushBlock (d.in.data(), block);
            ++d.k;
            d.nextDueS += d.periodS;
            deviceActualS[who] = std::max (d.nextDueS + lateBy (deviceStall[who]), simS);
        }
        else
        {
            for (auto& d : devices)
            {
                d.stream.pull (out.data(), block);

                if (! d.stream.hasStarted())
                    continue;

                float prev = d.lastOut;
                for (int n = 0; n < block; ++n)
                {
                    const float v = out[static_cast<size_t> (n)];
                    // Silence written for a dry ring is loss, counted elsewhere;
                    // the ramp resumes after it wherever the ring resumes.
                    if (v != 0.0f && prev >= 0.0f && (v - prev < 0.0f || v - prev > 2.5f))
                    {
                        ++d.seams;
                        if (simS >= 60.0) ++d.seamsAfterMinute;
                    }
                    prev = v != 0.0f ? v : -1.0f;
                }
                d.lastOut = prev;
            }

            consumerDueS += consumerPeriodS;
            consumerActualS = std::max (consumerDueS + lateBy (consumerStall), simS);
        }

        if (simS >= nextReportS)
        {
            for (auto& d : devices)
            {
                d.stream.tickDriftReporting (0.1, 0.0);

                if (std::abs (d.stream.getDriftPpm() - d.ppm) > 10.0)
                    d.lastOutsideS = simS;

                if (simS >= seconds - 60.0)
                {
                    const double error = d.stream.getDriftPpm() - d.ppm;
                    d.lastMinuteSquares += error * error;
                    ++d.lastMinuteTicks;
                }

                if (simS >= 60.0)
                {
                    d.minFillAfterMinute = std::min (d.minFillAfterMinute, d.stream.getFillFraction());
                    d.maxFillAfterMinute = std::max (d.maxFillAfterMinute, d.stream.getFillFraction());
                }

                if (! d.measuredAtMinuteValid && d.stream.hasDriftMeasurement())
                {
                    d.measuredAtMinuteValid = true;
                    d.measuredAtMinute = d.stream.getMeasuredDriftPpm();
                    d.underAtMinute = d.stream.getUnderrunSamples();
                    d.overAtMinute = d.stream.getOverrunSamples();
                }
            }
            nextReportS += 0.1;
        }
    }

    std::printf ("block %d, %.0f s, kp %.2f ppm/sample, ki %.1e, slew %.0f ppm/s, jitter %.1f ms, stalls %.1f ms x %.2f/s, %s fill\n\n",
                 block, seconds, kpPpm, ki, slew, jitter.jitterS * 1000.0, jitter.stallS * 1000.0,
                 jitter.stallsPerSecond, virtualFill ? "virtual" : "raw");
    std::printf ("%8s %9s %9s %9s %9s %10s %10s %10s %10s %12s %12s %8s %8s\n",
                 "clock", "loop@end", "loop_rms", "meas@1min", "meas@end", "settle_s", "under<1m", "over<1m", "loss>1m", "fill_min>1m", "fill_max>1m", "seams", "seams>1m");

    int failures = 0;

    for (auto& d : devices)
    {
        const auto under = d.stream.getUnderrunSamples();
        const auto over = d.stream.getOverrunSamples();
        const auto lossAfterMinute = (under - d.underAtMinute) + (over - d.overAtMinute);
        const double loopErr = d.lastMinuteTicks > 0 ? std::sqrt (d.lastMinuteSquares / d.lastMinuteTicks) : 1.0e9;
        const double measErr = d.stream.hasDriftMeasurement() ? std::abs (d.stream.getMeasuredDriftPpm() - d.ppm) : 1.0e9;

        std::printf ("%+8.0f %+9.1f %9.1f %+9.1f %+9.1f %10.1f %10llu %10llu %10llu %12.3f %12.3f %8llu %8llu\n",
                     d.ppm, d.stream.getDriftPpm(), loopErr,
                     d.measuredAtMinuteValid ? d.measuredAtMinute : 0.0,
                     d.stream.hasDriftMeasurement() ? d.stream.getMeasuredDriftPpm() : 0.0,
                     d.lastOutsideS,
                     static_cast<unsigned long long> (d.underAtMinute),
                     static_cast<unsigned long long> (d.overAtMinute),
                     static_cast<unsigned long long> (lossAfterMinute),
                     d.minFillAfterMinute, d.maxFillAfterMinute,
                     static_cast<unsigned long long> (d.seams),
                     static_cast<unsigned long long> (d.seamsAfterMinute));

        if (lossAfterMinute > 0) ++failures;
        if (d.seamsAfterMinute > 0) ++failures;
        if (loopErr > 20.0) ++failures;
        if (measErr > 10.0) ++failures;
    }

    std::printf ("\n%s\n", failures == 0 ? "PASS: no loss or seam after the first minute, loop within 20 PPM over the last minute, measurement within 10 PPM"
                                          : "FAIL: see the rows above");
    return failures == 0 ? 0 : 1;
}
