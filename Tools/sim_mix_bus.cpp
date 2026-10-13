// The summed mix and the headphone mix, driven with the signals a real rig
// produces on a bad night: eight microphones all hitting full scale at once, a
// trim pushed hard, transients, and a driver that hands over a NaN or an
// infinity. Every take's MIX.wav and every headphone feed goes through one of
// the two limiters exercised here, and until this simulator neither had a test.
//
// What it asserts, from the files on disk rather than from the code's opinion
// of itself:
//   - every stem is the microphone's own signal, bit for bit after quantising,
//     whatever the trim or the other channels are doing (§4);
//   - MIX.wav never exceeds the -1 dBFS ceiling and never wraps (§6.1), and a
//     loud overlap is turned down there rather than squared off;
//   - a NaN or infinity from one microphone becomes silence in that
//     microphone's stem and in the mix, never a full-scale click, and never
//     poisons the loudness figure reported for the take;
//   - the headphone bus never outputs a non-finite or over-ceiling sample,
//     whatever it is fed (§5), and at the default volume leaves a loud bus
//     to the volume rather than the limiter.
//
//   sim_mix_bus            (no arguments; exits non-zero on any failure)

#include "Core/MonitorBus.h"
#include "Core/MixBusLimiter.h"
#include "Core/WritePipeline.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace mma;

namespace {

int failures = 0;

void check (bool condition, const std::string& what)
{
    std::printf (condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (! condition)
        ++failures;
}

constexpr double kRate = 48000.0;
constexpr float kPi = 3.14159265358979f;
const float kCeiling = std::pow (10.0f, MixBusLimiter::kCeilingDb / 20.0f);

std::filesystem::path makeTempFolder (const std::string& label)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path()
              / ("sobstage-sim-mix-" + label + "-" + std::to_string (nonce));
    std::filesystem::create_directories (path);
    return path;
}

/// A WAV's samples, normalised to [-1, 1]. Chunks are walked rather than
/// assumed, because the writer puts a BWF bext chunk before the data.
struct Wav
{
    bool ok = false;
    int bits = 0;
    int channels = 0;
    std::vector<int64_t> raw;       // integer sample values as written
    std::vector<double> samples;    // raw / full scale
    int64_t fullScale = 0;
};

Wav readWav (const std::filesystem::path& path)
{
    Wav wav;
    std::ifstream f (path, std::ios::binary);

    if (! f)
        return wav;

    const auto u32 = [&f]() -> uint32_t
    {
        unsigned char b[4] {};
        f.read (reinterpret_cast<char*> (b), 4);
        return b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t> (b[3]) << 24);
    };

    char tag[4] {};
    f.read (tag, 4);
    if (std::string (tag, 4) != "RIFF")
        return wav;
    u32();
    f.read (tag, 4);
    if (std::string (tag, 4) != "WAVE")
        return wav;

    while (f.read (tag, 4))
    {
        const uint32_t size = u32();
        const std::string id (tag, 4);

        if (id == "fmt ")
        {
            std::vector<unsigned char> fmt (size);
            f.read (reinterpret_cast<char*> (fmt.data()), size);
            wav.channels = fmt[2] | (fmt[3] << 8);
            wav.bits = fmt[14] | (fmt[15] << 8);
        }
        else if (id == "data")
        {
            const int bytes = wav.bits / 8;
            if (bytes < 2 || bytes > 4)
                return wav;

            wav.fullScale = (int64_t { 1 } << (wav.bits - 1)) - 1;
            const size_t count = size / static_cast<uint32_t> (bytes);
            std::vector<unsigned char> data (size);
            f.read (reinterpret_cast<char*> (data.data()), size);

            for (size_t i = 0; i < count; ++i)
            {
                int64_t v = 0;
                for (int b = 0; b < bytes; ++b)
                    v |= static_cast<int64_t> (data[i * static_cast<size_t> (bytes) + static_cast<size_t> (b)]) << (8 * b);

                const int64_t sign = int64_t { 1 } << (wav.bits - 1);
                if (v & sign)
                    v -= sign << 1;

                wav.raw.push_back (v);
                wav.samples.push_back (static_cast<double> (v) / static_cast<double> (wav.fullScale));
            }

            wav.ok = true;
            return wav;
        }
        else
        {
            f.seekg (size + (size & 1u), std::ios::cur);
        }
    }

    return wav;
}

/// The integer the writer should produce for a sample at this depth.
int64_t expectedInteger (float s, int bits)
{
    if (! std::isfinite (s))
        return 0;

    // The same arithmetic the writer uses: single precision at 16 and 24 bits,
    // double at 32, so a rounding tie lands the same way on both sides.
    const float c = std::max (-1.0f, std::min (1.0f, s));

    if (bits == 16)
        return std::lround (c * 32767.0f);
    if (bits == 24)
        return std::lround (c * 8388607.0f);
    return std::llround (static_cast<double> (c) * 2147483647.0);
}

bool waitForFrames (const WritePipeline& p, uint64_t frames)
{
    for (int i = 0; i < 5000; ++i)
    {
        if (p.getFramesWritten() >= frames)
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

/// One take through the real WritePipeline: each channel's signal is produced
/// by `source (channel, frame)`, pushed in `block`-sized pieces as the audio
/// callback would, then the files are read back.
struct TakeResult
{
    bool started = false;
    bool allFramesWritten = false;
    std::vector<Wav> stems;
    Wav mix;
    double lufs = 0.0;
    double truePeak = 0.0;
};

template <typename Source, typename OnBlock>
TakeResult runTake (const std::string& label, int channels, int bits, int frames,
                    std::vector<float> trims, Source source, OnBlock onBlock)
{
    TakeResult result;
    const auto folder = makeTempFolder (label);

    std::vector<WriteChannelSpec> specs;
    for (int ch = 0; ch < channels; ++ch)
    {
        char name[32];
        std::snprintf (name, sizeof (name), "%02d_Mic", ch + 1);
        specs.push_back ({ name, trims.empty() ? 0.0f : trims[static_cast<size_t> (ch)] });
    }

    WritePipeline pipeline;
    result.started = pipeline.start (folder.string(), specs, kRate, bits, "2026-09-27T12:00:00");

    if (! result.started)
        return result;

    constexpr int kBlock = 256;
    std::vector<std::vector<float>> buffers (static_cast<size_t> (channels), std::vector<float> (kBlock));
    std::vector<const float*> pointers (static_cast<size_t> (channels));

    for (int start = 0; start < frames; start += kBlock)
    {
        const int n = std::min (kBlock, frames - start);

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int i = 0; i < n; ++i)
                buffers[static_cast<size_t> (ch)][static_cast<size_t> (i)] = source (ch, start + i);
            pointers[static_cast<size_t> (ch)] = buffers[static_cast<size_t> (ch)].data();
        }

        onBlock (pipeline, start);

        // The real callback never blocks; the simulator waits for room so a
        // slow disk here is not mistaken for a pipeline fault.
        while (! pipeline.pushBlock (pointers.data(), channels, n))
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    result.allFramesWritten = waitForFrames (pipeline, static_cast<uint64_t> (frames));
    result.lufs = pipeline.getIntegratedLufs();
    result.truePeak = pipeline.getTruePeakDbtp();
    pipeline.stop();

    for (const auto& specChannel : specs)
        result.stems.push_back (readWav (folder / (specChannel.fileName + ".wav")));
    result.mix = readWav (folder / "MIX.wav");

    std::error_code ignored;
    std::filesystem::remove_all (folder, ignored);
    return result;
}

double peakOf (const Wav& wav)
{
    double peak = 0.0;
    for (auto s : wav.samples)
        peak = std::max (peak, std::abs (s));
    return peak;
}

/// Every sample of a stem is exactly what the writer should make of the
/// microphone's own signal.
template <typename Source>
bool stemMatchesSource (const Wav& stem, int channel, int frames, Source source)
{
    if (! stem.ok || static_cast<int> (stem.raw.size()) != frames)
        return false;

    for (int i = 0; i < frames; ++i)
        if (stem.raw[static_cast<size_t> (i)] != expectedInteger (source (channel, i), stem.bits))
            return false;

    return true;
}

void noBlockHook (WritePipeline&, int) {}

// ---------------------------------------------------------------------------

void eightMicsAtFullScale()
{
    std::printf ("-- eight microphones all at full scale, in phase --\n");

    static constexpr int kChannels = 8, kFrames = 48000;
    const auto source = [] (int, int i) { return std::sin (2.0f * kPi * 440.0f * static_cast<float> (i) / 48000.0f); };

    for (int bits : { 16, 24, 32 })
    {
        const auto take = runTake ("fullscale" + std::to_string (bits), kChannels, bits, kFrames, {}, source, noBlockHook);
        const auto tag = std::to_string (bits) + "-bit: ";

        check (take.started && take.allFramesWritten, tag + "the take starts and every frame is written");

        bool stemsExact = true;
        for (int ch = 0; ch < kChannels; ++ch)
            stemsExact = stemsExact && stemMatchesSource (take.stems[static_cast<size_t> (ch)], ch, kFrames, source);
        check (stemsExact, tag + "all eight stems are each microphone's own signal, untouched");

        const double peak = peakOf (take.mix);
        check (take.mix.ok && static_cast<int> (take.mix.samples.size()) == kFrames, tag + "MIX.wav holds every frame");
        check (peak <= kCeiling + 1.0 / static_cast<double> (take.mix.fullScale),
               tag + "MIX.wav never exceeds -1 dBFS (peak " + std::to_string (20.0 * std::log10 (peak)) + " dBFS)");
        check (peak > kCeiling * 0.99, tag + "MIX.wav reaches the ceiling rather than being turned down");

        // A wrap would show as a sample of the opposite sign at a crest.
        bool wrapped = false;
        for (int i = 0; i < kFrames; ++i)
        {
            const float s = source (0, i);
            if (std::abs (s) > 0.5f && (s > 0) != (take.mix.samples[static_cast<size_t> (i)] > 0))
                wrapped = true;
        }
        check (! wrapped, tag + "no sample in MIX.wav wraps to the opposite sign");
        check (std::isfinite (take.lufs), tag + "the take's loudness is a number (" + std::to_string (take.lufs) + " LUFS)");
    }
}

void trimMovesTheMixNotTheStem()
{
    std::printf ("-- trim moves the mix, never the stem --\n");

    constexpr int kFrames = 48000;
    // Mic 1 quiet, mic 2 silent; mic 1 trimmed +12 dB, then down to -24 dB
    // halfway through as a user would drag it mid-take.
    const auto source = [] (int ch, int i)
    {
        return ch == 0 ? 0.1f * std::sin (2.0f * kPi * 1000.0f * static_cast<float> (i) / 48000.0f) : 0.0f;
    };
    // The writer applies trim as it drains, so the drag waits until everything
    // pushed so far is on disk -- what real time does for free, since the ring
    // holds a few milliseconds, not the whole take.
    const auto dragTrim = [] (WritePipeline& p, int start)
    {
        if (start == 24064)
        {
            waitForFrames (p, 24064);
            p.setChannelTrimDb (0, -24.0f);
        }
    };

    const auto take = runTake ("trim", 2, 24, kFrames, { 12.0f, 0.0f }, source, dragTrim);
    check (take.started && take.allFramesWritten, "the take starts and every frame is written");
    check (stemMatchesSource (take.stems[0], 0, kFrames, source), "mic 1's stem is unity gain throughout, whatever the trim");

    double earlyPeak = 0.0, latePeak = 0.0;
    for (int i = 0; i < 20000; ++i)
        earlyPeak = std::max (earlyPeak, std::abs (take.mix.samples[static_cast<size_t> (i)]));
    for (int i = 30000; i < kFrames; ++i)
        latePeak = std::max (latePeak, std::abs (take.mix.samples[static_cast<size_t> (i)]));

    const double earlyDb = 20.0 * std::log10 (earlyPeak / 0.1);
    const double lateDb = 20.0 * std::log10 (latePeak / 0.1);
    check (std::abs (earlyDb - 12.0) < 0.2, "+12 dB trim puts the mix 12 dB above the stem (measured " + std::to_string (earlyDb) + ")");
    check (std::abs (lateDb + 24.0) < 0.2, "dragging the trim to -24 dB mid-take follows in the mix (measured " + std::to_string (lateDb) + ")");
}

void transients()
{
    std::printf ("-- a dropped microphone: full-scale clicks on every channel --\n");

    static constexpr int kChannels = 4, kFrames = 24000;
    const auto source = [] (int, int i) { return (i % 997 == 0) ? ((i / 997) % 2 ? 1.0f : -1.0f) : 0.0f; };
    const auto take = runTake ("clicks", kChannels, 24, kFrames, {}, source, noBlockHook);

    check (take.started && take.allFramesWritten, "the take starts and every frame is written");
    check (peakOf (take.mix) <= kCeiling + 1e-6, "four summed full-scale clicks stay at the ceiling in MIX.wav");
    check (stemMatchesSource (take.stems[2], 2, kFrames, source), "each stem keeps its click at full scale");
}

void nonFiniteSamples()
{
    std::printf ("-- a driver hands over NaN and infinity mid-take --\n");

    static constexpr int kChannels = 3, kFrames = 48000;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Mic 2 goes bad for a stretch: NaN, then +inf, then -inf, then a run of
    // NaN long enough to matter to the loudness meter.
    const auto source = [nan, inf] (int ch, int i)
    {
        const float tone = 0.25f * std::sin (2.0f * kPi * 220.0f * static_cast<float> (i) / 48000.0f);

        if (ch != 1)
            return tone;
        if (i == 10000)
            return nan;
        if (i == 10001)
            return inf;
        if (i == 10002)
            return -inf;
        if (i >= 20000 && i < 20480)
            return nan;
        return tone;
    };

    for (int bits : { 16, 24, 32 })
    {
        const auto tag = std::to_string (bits) + "-bit: ";
        const auto take = runTake ("nonfinite" + std::to_string (bits), kChannels, bits, kFrames, {}, source, noBlockHook);
        check (take.started && take.allFramesWritten, tag + "the take starts and every frame is written");

        const auto& bad = take.stems[1];
        const bool silentWhereBad = bad.ok
            && bad.raw[10000] == 0 && bad.raw[10001] == 0 && bad.raw[10002] == 0
            && bad.raw[20000] == 0 && bad.raw[20479] == 0;
        check (silentWhereBad, tag + "the bad microphone's stem is silent where it sent NaN or infinity");
        check (stemMatchesSource (bad, 1, kFrames, source), tag + "and is its own signal everywhere else");
        check (stemMatchesSource (take.stems[0], 0, kFrames, source)
                   && stemMatchesSource (take.stems[2], 2, kFrames, source),
               tag + "the other microphones' stems are untouched");

        // In the mix, the bad samples contribute nothing: the mix at those
        // frames is the two good microphones and nothing else. Together they
        // peak at -6 dBFS, under the -1 dBFS ceiling, so the take's limiter
        // never acts and their plain sum is what the file must hold.
        const auto expectedMix = [&source] (int i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < kChannels; ++ch)
            {
                const float s = source (ch, i);
                sum += std::isfinite (s) ? s : 0.0f;
            }
            return sum;
        };

        bool mixClean = take.mix.ok;
        for (int i : { 10000, 10001, 10002, 20000, 20240, 20479 })
            mixClean = mixClean && take.mix.raw[static_cast<size_t> (i)] == expectedInteger (expectedMix (i), bits);
        check (mixClean, tag + "MIX.wav carries only the good microphones at those frames, no click");
        // Two microphones at -12 dBFS read around -10 LUFS. A figure near the
        // meter's floor means the bad samples wiped the measurement out.
        check (std::isfinite (take.lufs) && take.lufs > -20.0 && take.lufs < 0.0,
               tag + "the take's loudness still measures the good microphones (" + std::to_string (take.lufs) + " LUFS)");
        check (std::isfinite (take.truePeak), tag + "and so is its true peak (" + std::to_string (take.truePeak) + " dBTP)");
    }
}

void headphoneBus()
{
    std::printf ("-- the headphone bus, fed the same bad samples --\n");

    // At full volume the headphones get the bus itself, so the figures below
    // read directly. (The volume now comes before the ceiling; at the
    // default it would scale every one of them.)
    MonitorBus bus (kRate);
    bus.setMasterVolume (100.0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    bool allFiniteAndCapped = true;
    int silentOnBad = 0;
    const std::vector<std::vector<float>> feeds {
        { 0.3f, nan, 0.2f },
        { 0.3f, inf, 0.2f },
        { -inf, 0.1f, 0.0f },
        { nan, nan, nan },
    };

    for (const auto& feed : feeds)
    {
        const float out = bus.processSample (feed);
        allFiniteAndCapped = allFiniteAndCapped && std::isfinite (out) && std::abs (out) <= 1.0f;
        if (out == 0.0f)
            ++silentOnBad;
    }

    check (allFiniteAndCapped, "a NaN or infinity on any input never reaches the headphones as a non-finite sample");
    check (silentOnBad == static_cast<int> (feeds.size()), "a sample containing one is silence, not a full-scale click");

    // Good audio afterwards plays normally: one bad sample must not mute the
    // monitor or trip the runaway cut.
    const float after = bus.processSample ({ 0.1f, 0.2f });
    check (std::abs (after - 0.3f) < 1e-6f && ! bus.isRunawayMuted(), "ordinary audio right after plays normally");

    // Half a second of all-NaN input is a device fault, not a runaway: the
    // headphones stay silent throughout and nothing is left latched.
    bool silentThroughout = true;
    for (int i = 0; i < 24000; ++i)
        silentThroughout = silentThroughout && bus.processSample ({ nan, 0.1f }) == 0.0f;
    check (silentThroughout, "half a second of NaN from one microphone is silent in the headphones throughout");

    const float recovered = bus.processSample ({ 0.1f, 0.2f });
    check (std::abs (recovered - 0.3f) < 1e-6f, "and the monitor carries on when the microphone recovers");

    // Loud but finite input is still the limiter's job, as before -- judged
    // on what reaches the ears, after the volume.
    const float monitorCeiling = std::pow (10.0f, MonitorBus::kLimiterCeilingDb / 20.0f);
    MonitorBus loud (kRate);
    loud.setMasterVolume (100.0);
    float loudest = 0.0f;
    for (int i = 0; i < 100; ++i)
        loudest = std::max (loudest, std::abs (loud.processSample ({ 1.0f, 1.0f, 1.0f, 1.0f })));
    check (loudest <= monitorCeiling + 1e-6f && loudest > monitorCeiling * 0.99f,
           "four full-scale microphones at full volume are held at the -3 dBFS ceiling");

    // At the default volume the same four reach the ears at -6 dBFS, under
    // the ceiling: they are turned down by the volume, not squared off.
    MonitorBus atDefault (kRate);
    const float expected = 4.0f * MonitorBus::monitorVolumeToLinearGain (MonitorBus::kDefaultMonitorVolume);
    float worstError = 0.0f;
    for (int i = 0; i < 100; ++i)
        worstError = std::max (worstError, std::abs (atDefault.processSample ({ 1.0f, 1.0f, 1.0f, 1.0f }) - expected));
    check (worstError < 1e-6f && atDefault.getLimiterEngagementCount() == 0,
           "at the default volume they reach the headphones untouched by the limiter");
}

void loudOverlapIsTurnedDown()
{
    std::printf ("-- four microphones in phase, 2.6 dB over the MIX.wav ceiling --\n");

    // Every microphone in one room hears the same vowel, so the unity sum
    // runs over -1 dBFS on every crest. The mix limiter used to square each
    // crest off; it now turns the whole passage down and keeps its shape.
    static constexpr int kChannels = 4, kFrames = 48000;
    const auto source = [] (int, int i) { return 0.3f * std::sin (2.0f * kPi * 220.0f * static_cast<float> (i) / 48000.0f); };
    const auto take = runTake ("overlap", kChannels, 24, kFrames, {}, source, noBlockHook);
    check (take.started && take.allFramesWritten, "the take starts and every frame is written");

    bool stemsExact = true;
    for (int ch = 0; ch < kChannels; ++ch)
        stemsExact = stemsExact && stemMatchesSource (take.stems[static_cast<size_t> (ch)], ch, kFrames, source);
    check (stemsExact, "every stem is its microphone's own signal, untouched by the mix limiter");

    // Flat tops: two neighbouring samples both on the ceiling, to within a
    // couple of steps of the 24-bit file. A hard clip at -1 dBFS of a
    // 1.2-peak sine holds nearly half of every cycle there.
    int flat = 0;
    const double onTheCeiling = kCeiling - 2.0 / static_cast<double> (take.mix.fullScale);
    for (size_t i = 1; i < take.mix.samples.size(); ++i)
        if (std::abs (take.mix.samples[i]) >= onTheCeiling && std::abs (take.mix.samples[i - 1]) >= onTheCeiling)
            ++flat;
    // The very first crest's rise is the one thing a limiter with no
    // lookahead cannot see coming: about 25 samples, once. Then none.
    check (flat < 50, "MIX.wav squares off at most the rise of the first crest ("
                          + std::to_string (flat) + " flat-topped samples in a second)");

    // After the first crest the file is the same sine, scaled to the ceiling.
    double worstShapeError = 0.0;
    const double scale = kCeiling / 1.2;
    for (int i = 4800; i < kFrames; ++i)
        worstShapeError = std::max (worstShapeError,
                                    std::abs (take.mix.samples[static_cast<size_t> (i)] - scale * 4.0 * source (0, i)));
    check (worstShapeError < 1e-3, "and after its first crest it is the same waveform, turned down to the ceiling (worst error "
                                       + std::to_string (worstShapeError) + ")");
    check (peakOf (take.mix) <= kCeiling + 1.0 / static_cast<double> (take.mix.fullScale), "MIX.wav never exceeds -1 dBFS");
}

} // namespace

int main()
{
    eightMicsAtFullScale();
    trimMovesTheMixNotTheStem();
    transients();
    nonFiniteSamples();
    headphoneBus();
    loudOverlapIsTurnedDown();

    std::printf ("\n%s: %d failure%s\n", failures == 0 ? "OK" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
