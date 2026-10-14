#include "TestFramework.h"
#include "Core/CaptureCoordinator.h"
#include <map>
#include <set>
#include "Core/StreamingTargets.h"
#include "Core/PolarPatternDetector.h"
#include "Core/SlowMicrophoneNotice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>

using namespace mma;

namespace {

// What the resampler that puts each microphone on the output's clock adds to
// every headphone figure, on top of the ring's cushion: it plays a sample up
// to two samples after the ring gives it up (it works between the middle two
// of four), and the figure counts both.
constexpr double kResampler = DeviceInputStream::kResamplerDelaySamples;

std::string tempDir()
{
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);
        if (dir != nullptr && *dir != '\0')
            return std::string (dir);
    }
    return "/tmp";
}

// A shell `mkdir -p` is not there on Windows, where every test that made its
// take folder that way failed before recording a sample.
bool makeDir (const std::string& dir)
{
    std::error_code ec;
    std::filesystem::create_directories (std::filesystem::u8path (dir), ec);
    return ! ec && std::filesystem::is_directory (std::filesystem::u8path (dir), ec);
}

// §6.1 writes BWF: RIFF 12 + fmt (8+16) + bext (8+602) + "data" tag 4.
constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;
constexpr std::streamoff kAudioDataOffset = kDataSizeOffset + 4;

uint32_t readU32LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

/// Stands in for a real device. The platform backends' OS calls still need
/// hardware, but everything this project wires together does not.
class FakeBackend : public IAudioBackend
{
public:
    bool exclusiveAvailable = true;
    std::string exclusiveReason;
    /// What the backend says the monitor path costs. Every real backend fills
    /// this in; the fake did not, which is part of why nothing noticed that the
    /// coordinator was dropping it.
    double exclusiveLatencyMs = 0.0;
    /// What the device GRANTS, as distinct from what was asked for. Zero means
    /// "cannot say", which is what every backend returned before this existed.
    int grantedOutputBufferFrames = 0;
    bool failOutputOpen = false;
    bool failInputOpen = false;
    /// Devices that refuse to open, by id -- so a test can fail ONE microphone
    /// of several. failInputOpen fails them all, which is a different question.
    std::set<std::string> failInputDevices;
    std::string inputOpenError;

    int inputStreamsOpened = 0;
    int outputStreamsOpened = 0;
    int closeAllCalls = 0;
    AudioCallback captured;              // the LAST stream opened, output or input
    AudioCallback outputCallback;        // the output stream's callback (the clock), only
    std::vector<AudioCallback> inputCallbacks; // one per device, in open order

    std::string getBackendName() const override { return "Fake"; }
    int getGrantedOutputBufferFrames() const override { return grantedOutputBufferFrames; }
    int outputPresentationLatencyFrames = 0;
    int getOutputPresentationLatencyFrames() const override { return outputPresentationLatencyFrames; }
    /// Input latency per device id, as CoreAudio reports it.
    std::map<std::string, int> inputLatencyFrames;
    int getInputLatencyFrames (const std::string& deviceId) const override
    {
        const auto found = inputLatencyFrames.find (deviceId);
        return found != inputLatencyFrames.end() ? found->second : 0;
    }
    std::vector<AudioDeviceDescriptor> enumerateInputDevices() override { return {}; }
    std::vector<AudioDeviceDescriptor> enumerateOutputDevices() override { return {}; }
    void setDeviceChangeCallback (DeviceChangeCallback) override {}

    ExclusiveModeCapability checkExclusiveModeCapability (const std::string&, double, int) override
    {
        ExclusiveModeCapability c;
        c.exclusiveModeAvailable = exclusiveAvailable;
        c.unavailableReason = exclusiveReason;
        c.measuredOrEstimatedLatencyMs = exclusiveLatencyMs;
        return c;
    }

    bool openExclusiveOutputStream (const std::string&, double, int, AudioCallback cb) override
    {
        if (failOutputOpen)
            return false;
        ++outputStreamsOpened;
        outputCallback = cb;
        captured = std::move (cb);
        return true;
    }

    bool openInputStream (const std::string& deviceId, double, int, AudioCallback cb) override
    {
        if (failInputOpen || failInputDevices.count (deviceId) > 0)
            return false;
        ++inputStreamsOpened;
        inputCallbacks.push_back (cb);
        captured = std::move (cb);
        return true;
    }

    std::string getLastOpenError() const override { return inputOpenError; }

    void closeAllStreams() override { ++closeAllCalls; }

    /// The platform's word that the output's rate has changed, as CoreAudio's
    /// nominal-rate listener raises it: before any poll has read the report.
    std::atomic<bool> outputOffRate { false };
    bool isOutputOffRate() const noexcept override { return outputOffRate.load(); }
};

std::vector<CaptureChannel> twoMics()
{
    return { { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
             { "dev-b", "Couch",   "02_Couch",   0.0f } };
}

} // namespace

TEST_CASE (CaptureCoordinator_OpensOneOutputAndOneInputPerMic)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.isMonitoring());

    // §5.2: exactly one output stream, ever.
    REQUIRE (backend.outputStreamsOpened == 1);
    REQUIRE (backend.inputStreamsOpened == 2);
}

TEST_CASE (CaptureCoordinator_OutputFailureFallsBackToInputOnlyRecording)
{
    FakeBackend backend;
    backend.exclusiveAvailable = false;
    backend.exclusiveReason = "Another app has taken exclusive control of your headphones.";

    CaptureCoordinator c (backend, 48000.0, 64);

    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    // §5.4: never ship a 40 ms mix silently. Inputs stay live under the
    // software clock, while the missing headphone path is named.
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.isMonitoring());
    REQUIRE_FALSE (c.hasOutputStream());
    REQUIRE (backend.inputStreamsOpened == 2);
    REQUIRE (c.getMonitorProblem().find ("exclusive control") != std::string::npos);
    REQUIRE (c.getMonitorProblem().find ("Recording is available") != std::string::npos);

    // Capability probes can lie: the runtime open is the final authority. A
    // fixed-rate HDMI endpoint which fails there gets the same safe fallback.
    FakeBackend runtimeBackend;
    runtimeBackend.failOutputOpen = true;
    runtimeBackend.inputOpenError = "This output stayed at 48 kHz and refused 44.1 kHz.";

    CaptureCoordinator runtime (runtimeBackend, 44100.0, 64);
    runtime.setSoftwareClockEnabled (false);

    REQUIRE (runtime.startMonitoring (twoMics(), "hdmi-output"));
    REQUIRE (runtime.isMonitoring());
    REQUIRE_FALSE (runtime.hasOutputStream());
    REQUIRE (runtimeBackend.inputStreamsOpened == 2);
    REQUIRE (runtime.getMonitorProblem().find ("48 kHz") != std::string::npos);
    REQUIRE (runtime.getMonitorProblem().find ("headphone monitoring is off")
             != std::string::npos);
}

TEST_CASE (CaptureCoordinator_ClosesStreamsWhenEveryInputFailsToOpen)
{
    // EVERY input, which is the case this refusal is for: a take now would
    // write nothing but empty files with the clock running. One microphone of
    // several failing is a different question and is answered two tests below.
    FakeBackend backend;
    backend.failInputOpen = true;

    CaptureCoordinator c (backend, 48000.0, 64);

    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE_FALSE (c.startMonitoring (twoMics(), "out-device"));

    // A half-open set of streams would leave the device hogged.
    REQUIRE (backend.closeAllCalls >= 1);
    REQUIRE_FALSE (c.getMonitorProblem().empty());
}

TEST_CASE (CaptureCoordinator_OneMicThatWillNotOpenDoesNotSilenceTheRest)
{
    // §0.1, at its largest: this used to closeAllStreams() and give up the
    // moment ANY device refused, so a rig with one dead cable recorded NOTHING
    // -- not the microphones that were working perfectly. Being unable to
    // record at all is the biggest loss this app can take, and it arrived by
    // the most ordinary way for a gig to go wrong.
    FakeBackend backend;
    backend.failInputDevices.insert ("dev-b");
    backend.inputOpenError = "This microphone took too long to connect.";

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // The working microphone is open, and nothing closed it on the way past.
    REQUIRE (backend.inputStreamsOpened == 1);
    REQUIRE (backend.closeAllCalls == 0);

    // The one that refused is named, so the take's record can say why that
    // stem is silent rather than leaving it to be found in the files.
    const auto& failed = c.getDevicesThatFailedToOpen();
    REQUIRE (failed.size() == 1u);
    REQUIRE (failed.front() == "dev-b");

    // §6.5: its channel writes silence, the same answer a microphone that goes
    // away MID-take already gets. The working one is untouched.
    REQUIRE (c.isChannelLive (0));
    REQUIRE_FALSE (c.isChannelLive (1));

    // And it is still a visible problem, not a silent degradation.
    const auto problem = c.getMonitorProblem();
    REQUIRE (problem.find ("Couch") != std::string::npos);
    REQUIRE (problem.find ("took too long") != std::string::npos);
}

TEST_CASE (CaptureCoordinator_ADeadMicIsStillNamedWhenTheOutputIsRefusedToo)
{
    // A shared output and a dead microphone at once. The output's sentence
    // used to replace the microphone's, so the rig was told about its
    // headphones and nothing about the track that would be silent.
    FakeBackend backend;
    backend.exclusiveAvailable = false;
    backend.exclusiveReason = "This sound output is shared with other apps.";
    backend.failInputDevices.insert ("dev-b");
    backend.inputOpenError = "This microphone took too long to connect.";

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.isMonitoring());
    REQUIRE_FALSE (c.hasOutputStream());
    REQUIRE (c.getDevicesThatFailedToOpen().size() == 1u);

    const auto problem = c.getMonitorProblem();
    REQUIRE (problem.find ("shared with other apps") != std::string::npos);
    REQUIRE (problem.find ("headphone monitoring is off") != std::string::npos);
    REQUIRE (problem.find ("Couch") != std::string::npos);
    REQUIRE (problem.find ("took too long") != std::string::npos);
}

TEST_CASE (CaptureCoordinator_ARigWhereOnlyOneMicOpensStillRecordsThatOne)
{
    // The same thing from the other side: it is the SURVIVING count that has to
    // be right. Carrying on without a device must not also mean reporting it as
    // live -- a count wrong in the user's favour is worse than no count.
    FakeBackend backend;
    backend.failInputDevices.insert ("dev-a");

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (backend.inputStreamsOpened == 1);
    REQUIRE (c.getDevicesThatFailedToOpen().size() == 1u);
    REQUIRE_FALSE (c.isChannelLive (0));
    REQUIRE (c.isChannelLive (1));
}

TEST_CASE (CaptureCoordinator_OpensAMixerThatIsAlsoTheOutputExactlyOnce)
{
    // A small livestream mixer presents its microphone inputs and its monitor
    // output as ONE duplex device. Opening it for output, taking hog mode on
    // it, then opening it again for input asks macOS for a second IOProc on a
    // device this process has just claimed exclusively -- and the refusal
    // arrives as "couldn't be opened for recording" against a microphone that
    // is plugged in and working.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    std::vector<CaptureChannel> mixerChannels {
        { "mixer", "PUP 1", "01_PUP-1", 0.0f },
        { "mixer", "PUP 2", "02_PUP-2", 0.0f },
    };
    mixerChannels[1].deviceChannel = 1;

    REQUIRE (c.startMonitoring (mixerChannels, "mixer"));

    REQUIRE (backend.outputStreamsOpened == 1);
    // The microphones ride on the output stream, so nothing opens the device
    // a second time.
    REQUIRE (backend.inputStreamsOpened == 0);
}

TEST_CASE (CaptureCoordinator_AFailingMicBesideAMixerOutputDoesNotCloseTheMixer)
{
    FakeBackend backend;
    backend.failInputDevices.insert ("usb-mic");
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    std::vector<CaptureChannel> channels {
        { "mixer", "PUP 1", "01_PUP-1", 0.0f },
        { "usb-mic", "Guest", "02_Guest", 0.0f },
    };

    // The mixer opened (it is the output) and carries PUP 1; only the guest's
    // separate mic refused. That is a rig with a microphone, not none.
    REQUIRE (c.startMonitoring (channels, "mixer"));
    REQUIRE (c.isMonitoring());
    REQUIRE (backend.closeAllCalls == 0);

    const auto& failed = c.getDevicesThatFailedToOpen();
    REQUIRE (failed.size() == 1u);
    REQUIRE (failed[0] == std::string ("usb-mic"));
}

TEST_CASE (CaptureCoordinator_AMixerThatIsAlsoTheOutputStillRecordsBothMics)
{
    // Opening once must not cost the microphones: the one callback carries both
    // halves of the cycle, so both people still reach their own channel.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    std::vector<CaptureChannel> mixerChannels {
        { "mixer", "PUP 1", "01_PUP-1", 0.0f },
        { "mixer", "PUP 2", "02_PUP-2", 0.0f },
    };
    mixerChannels[1].deviceChannel = 1;

    REQUIRE (c.startMonitoring (mixerChannels, "mixer"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> in0 (64, 0.10f), in1 (64, 0.80f);
    std::vector<float> outL (64, 0.0f), outR (64, 0.0f);
    const float* ins[] = { in0.data(), in1.data() };
    float* outs[] = { outL.data(), outR.data() };

    // The output stream's callback, which on this rig is the only one there is:
    // it reads the microphones and fills the headphones in the same cycle.
    REQUIRE (backend.captured != nullptr);

    // Enough cycles to clear pre-roll (§5.4: kPreRollBlocks of the buffer).
    for (int i = 0; i < 16; ++i)
        backend.captured (ins, 2, outs, 2, 64);

    for (int i = 0; i < 30; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
        c.getChannelMetering (1)->tick (1.0 / 60.0);
    }

    // Each microphone reached its OWN channel: input 1 carries the louder
    // signal, so channel 1 must read higher than channel 0. One collapsed into
    // the other, or either dropped, fails this.
    REQUIRE (c.getChannelMetering (1)->getDisplayedLevelDb()
             > c.getChannelMetering (0)->getDisplayedLevelDb() + 3.0f);

    // And the headphones were still filled from the same callback.
    bool anyOutput = false;
    for (int i = 0; i < 64; ++i)
        if (outL[i] != 0.0f) { anyOutput = true; break; }
    REQUIRE (anyOutput);
}

TEST_CASE (CaptureCoordinator_ASliceOverTwiceTheNominalBufferIsStillRecorded)
{
    // CoreAudio is allowed to hand the callback more frames than the buffer
    // size that was asked for, and CoreAudioBackend sizes its own scratch for
    // exactly that. This one did not: an oversized slice made the whole pull
    // return early, so no audio, no meters and no error -- the silent failure
    // §0.1 forbids.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    REQUIRE (backend.inputCallbacks.size() == 2);

    constexpr int callbackFrames = 193;
    std::vector<float> loud (callbackFrames, 0.80f), quiet (callbackFrames, 0.05f);
    const float* loudIn[] = { loud.data() };
    const float* quietIn[] = { quiet.data() };

    std::vector<float> out (callbackFrames, 0.0f);
    float* outs[] = { out.data() };

    // 193 frames against a 64-frame nominal buffer: more than the scratch's
    // two-block headroom. The callback must be sliced rather than discarded.
    for (int i = 0; i < 16; ++i)
    {
        backend.inputCallbacks[0] (loudIn, 1, nullptr, 0, callbackFrames);
        backend.inputCallbacks[1] (quietIn, 1, nullptr, 0, callbackFrames);
        c.pullOutputBlock (outs, 1, callbackFrames);
    }

    for (int i = 0; i < 60; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
        c.getChannelMetering (1)->tick (1.0 / 60.0);
    }

    // The audio arrived rather than being dropped on the floor.
    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb() > Metering::kMinDb + 6.0f);
    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb()
             > c.getChannelMetering (1)->getDisplayedLevelDb() + 6.0f);
    REQUIRE (out.back() != 0.0f);
    REQUIRE (c.getFramesMissedByLayout() == 0u);
}

namespace {
int64_t largeCallbackNs = 0;
int64_t largeCallbackClock() { return largeCallbackNs; }

// Back to the zero clock Tests/main.cpp installs for every other test, which
// keeps the bit-for-bit comparisons elsewhere in this file deterministic.
struct LargeCallbackClockScope
{
    LargeCallbackClockScope() { largeCallbackNs = 1; DeviceInputStream::setClockForTesting (largeCallbackClock); }
    ~LargeCallbackClockScope() { DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; }); }
};
} // namespace

TEST_CASE (CaptureCoordinator_AnOutputRunningAtItsOwnLargerBufferKeepsTheLoopFree)
{
    // CoreAudio leaves a device at its own IO size when it refuses the one
    // asked for, and an output then calls back with, say, 1024 frames against
    // a nominal 64. Pulled from each ring 128 frames at a time, each ring was
    // held at two slices -- far less than the callback takes at once -- and
    // every drift loop sat at its 200 PPM clamp just to keep up at matched
    // clocks, with nothing left over for a microphone's real drift.
    LargeCallbackClockScope clock;

    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (backend.inputCallbacks.size() == 2);

    constexpr int inFrames = 64, outFrames = 1024;
    std::vector<float> a (inFrames, 0.1f), b (inFrames, 0.2f);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    std::vector<float> out (outFrames, 0.0f);
    float* outs[] = { out.data() };

    double nextIn = 0.0, nextOut = 0.0005;
    while (nextIn < 60.0)
    {
        if (nextIn <= nextOut)
        {
            largeCallbackNs = static_cast<int64_t> (nextIn * 1.0e9) + 1;
            backend.inputCallbacks[0] (aIn, 1, nullptr, 0, inFrames);
            backend.inputCallbacks[1] (bIn, 1, nullptr, 0, inFrames);
            nextIn += inFrames / 48000.0;
        }
        else
        {
            largeCallbackNs = static_cast<int64_t> (nextOut * 1.0e9) + 1;
            c.pullOutputBlock (outs, 1, outFrames);
            nextOut += outFrames / 48000.0;
        }
    }

    const double loop0 = c.getChannelRawDriftPpm (0);
    const double loop1 = c.getChannelRawDriftPpm (1);
    const auto underruns = c.getUnderrunSamples();
    c.stopMonitoring();

    REQUIRE (underruns == 0u);
    REQUIRE (std::abs (loop0) < 50.0);
    REQUIRE (std::abs (loop1) < 50.0);
}

namespace {

/// Index of the loudest sample of a 16-bit mono stem.
long long loudestSampleIn (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.is_open());
    const auto bytes = readU32LE (f, kDataSizeOffset);
    f.seekg (kAudioDataOffset);

    long long best = -1;
    int bestMagnitude = 0;

    for (uint32_t i = 0; i < bytes / 2; ++i)
    {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        const auto v = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
        if (std::abs (static_cast<int> (v)) > bestMagnitude)
        {
            bestMagnitude = std::abs (static_cast<int> (v));
            best = static_cast<long long> (i);
        }
    }

    return best;
}

/// Where a 16-bit mono stem's sound starts, and how many silent samples it
/// has after that: a gap in a channel that carries a steady level.
std::pair<long long, long long> silenceAfterFirstSoundIn (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.is_open());
    const auto bytes = readU32LE (f, kDataSizeOffset);
    f.seekg (kAudioDataOffset);

    long long first = -1, silent = 0;

    for (uint32_t i = 0; i < bytes / 2; ++i)
    {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        const auto v = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));

        if (v != 0 && first < 0)
            first = static_cast<long long> (i);
        else if (v == 0 && first >= 0)
            ++silent;
    }

    return { first, silent };
}

} // namespace

TEST_CASE (CaptureCoordinator_MicsOnDevicesWithDifferentInputLatencyLineUpInTheFiles)
{
    // One clap, heard by two microphones at the same instant. The second
    // interface reports 37 frames more input latency (device latency, safety
    // offset and stream latency, as CoreAudio gives them), so its callback
    // hands the clap over 37 samples later than the first's does. Without
    // compensation the two stems carried it 37 samples apart.
    const auto dir = tempDir();
    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = 12;
    backend.inputLatencyFrames["dev-b"] = 12 + 37;

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // The stems are lined up to the slower device; the headphones are not.
    // Their figure is the quicker microphone's own path -- the round trip,
    // the ring's cushion (256 samples at 64: one pull and three more of
    // late delivery, where it was two pulls and too thin to ride out a late
    // IOProc), the resampler's two samples and its 12 frames -- with nothing
    // of Couch's extra 37 frames in it.
    REQUIRE (c.getAlignedInputLatencyFrames() == 49);
    REQUIRE_NEAR (c.getMonitoringLatencyMs(), (2.0 * 64 + 256 + kResampler + 12) / 48000.0 * 1000.0, 1e-9);

    REQUIRE (c.startRecording (dir, 16, "2026-10-07T00:00:00Z"));

    constexpr long long clapAt = 20000; // in device A's delivered samples
    std::vector<float> a (64), b (64), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    for (long long block = 0; block < 600; ++block)
    {
        for (int i = 0; i < 64; ++i)
        {
            const long long n = block * 64 + i;
            a[static_cast<size_t> (i)] = n == clapAt ? 0.9f : 0.0f;
            b[static_cast<size_t> (i)] = n == clapAt + 37 ? 0.9f : 0.0f;
        }

        backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 64);
        c.pullOutputBlock (outs, 1, 64);
    }

    // The writer holds Kitchen back by the difference; Couch not at all.
    REQUIRE (c.getDeviceAlignmentDelayFrames ("dev-a") == 37);
    REQUIRE (c.getDeviceAlignmentDelayFrames ("dev-b") == 0);

    c.stopRecording();
    c.stopMonitoring();

    REQUIRE (c.areStemsAligned());
    REQUIRE (c.getDeviceAlignmentSilenceFramesThisTake ("dev-a") == 0);
    REQUIRE (c.getDeviceAlignmentDroppedFramesThisTake ("dev-a") == 0);

    // The latencies were known before the first block, so that is the
    // silence Kitchen's stem opened with, as the take's record has it.
    REQUIRE (c.getDeviceAlignmentStartFrames ("dev-a") == 37);
    REQUIRE (c.getDeviceAlignmentStartFrames ("dev-b") == 0);

    const auto kitchen = loudestSampleIn (dir + "/01_Kitchen.wav");
    const auto couch = loudestSampleIn (dir + "/02_Couch.wav");
    const auto mix = loudestSampleIn (dir + "/MIX.wav");
    REQUIRE (kitchen > 0);
    REQUIRE (couch > 0);
    REQUIRE (std::llabs (kitchen - couch) <= 1);

    // The mix is summed from the aligned stems: one clap, where both are.
    REQUIRE (std::llabs (mix - couch) <= 1);
}

TEST_CASE (CaptureCoordinator_TheTakesStartingOffsetIsRightBeforeTheWriterReachesIt)
{
    // The take's start-time session.json is written the moment the files
    // open, before the writer -- which sleeps between chunks -- has reached
    // a single channel. It read each device's starting offset from the
    // writer, which is zero until then, so the record said Kitchen's stem
    // starts unshifted while being held back by 37: the record a crash in the
    // take's first half-minute leaves, and the one a user is told to slide
    // the stems by when they are not called aligned.
    const auto dir = tempDir() + "/start_offset_before_writer";
    REQUIRE (makeDir (dir));

    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = 12;
    backend.inputLatencyFrames["dev-b"] = 12 + 37;

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.0f), b (64, 0.0f), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    auto runBlocks = [&] (int count)
    {
        for (int block = 0; block < count; ++block)
        {
            backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
            backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 64);
            c.pullOutputBlock (outs, 1, 64);
        }
    };

    // Monitoring has been running, as it always has by the time Record is
    // pressed.
    runBlocks (20);

    REQUIRE (c.startRecording (dir, 16, "2026-10-09T00:00:00Z"));

    // Not one block has reached the writer yet.
    REQUIRE (c.getDeviceAlignmentDelayFrames ("dev-a") == 37);
    REQUIRE (c.getDeviceAlignmentStartFrames ("dev-a") == 37);
    REQUIRE (c.getDeviceAlignmentStartFrames ("dev-b") == 0);

    // Nor does it move once the writer has started the channels.
    runBlocks (200);
    c.stopRecording();
    c.stopMonitoring();

    REQUIRE (c.getDeviceAlignmentStartFrames ("dev-a") == 37);
    REQUIRE (c.getDeviceAlignmentStartFrames ("dev-b") == 0);
}

TEST_CASE (CaptureCoordinator_ALatencyPastBeliefIsBoundedAndTheStemsAreNotCalledAligned)
{
    // Couch's driver reports 50000 frames of input latency -- over a second.
    // It is not believed: the stems are lined up by the 24000-frame bound,
    // and since that cannot be right either, the take says its stems are not
    // exactly aligned, with the driver's own figure beside the bound, so the
    // user is told to check them rather than shown a clap twice in the mix
    // under a record that calls it exact.
    const auto dir = tempDir() + "/latency_past_belief";
    REQUIRE (makeDir (dir));

    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = 12;
    backend.inputLatencyFrames["dev-b"] = 50000;

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    REQUIRE (c.getDeviceInputLatencyFrames ("dev-b") == 24000);
    REQUIRE (c.getDeviceReportedInputLatencyFrames ("dev-b") == std::optional<int> (50000));
    REQUIRE (c.getDeviceReportedInputLatencyFrames ("dev-a") == std::optional<int> (12));
    REQUIRE (c.getAlignedInputLatencyFrames() == 24000);

    REQUIRE (c.startRecording (dir, 16, "2026-10-07T00:00:00Z"));

    std::vector<float> a (64, 0.1f), b (64, 0.1f), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    for (int block = 0; block < 100; ++block)
    {
        backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 64);
        c.pullOutputBlock (outs, 1, 64);
    }

    REQUIRE (! c.areStemsAligned());
    c.stopRecording();
    REQUIRE (! c.areStemsAligned());
    c.stopMonitoring();
    REQUIRE (! c.getDeviceReportedInputLatencyFrames ("dev-b").has_value());

    // A rig whose figures are believable is aligned as before.
    backend.inputLatencyFrames["dev-b"] = 49;
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-10-07T00:00:01Z"));

    for (int block = 0; block < 100; ++block)
    {
        backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 64);
        c.pullOutputBlock (outs, 1, 64);
    }

    c.stopRecording();
    REQUIRE (c.areStemsAligned());
    c.stopMonitoring();
}

namespace {

int64_t ioSizeClockNs = 0;
int64_t ioSizeClock() { return ioSizeClockNs; }

struct IoSizeTakeResult
{
    long long kitchenClap = -1, couchClap = -1, mixClap = -1;
    long long headphoneClap = -1; // loudest headphone sample, in output frames
    uint64_t kitchenUnderruns = 0, couchUnderruns = 0;
    uint64_t kitchenStreamShift = 0, couchStreamShift = 0; // the rings' own moves
    int couchBlock = 0, kitchenAlignment = 0, couchAlignment = 0;
    int kitchenAlignmentStart = -1, couchAlignmentStart = -1; // as the take's record has them
    int kitchenAlignmentAtStop = -1;                          // likewise
    int kitchenShift = 0, couchShift = 0;     // writer silence written mid-take
    int kitchenDropped = 0, couchDropped = 0; // writer samples taken out mid-take
    int kitchenIoShift = 0, couchIoShift = 0; // a stream's own shift left in its stem
    bool stemsAligned = false;
    double headphoneLatencyMs = 0.0;
    double slowestHeadphoneLatencyMs = 0.0;
    std::string slowestDevice;
    long long kitchenSoundStarts = -1, kitchenSilentAfter = 0; // with a steady level on Kitchen
};

/// Two microphones hearing one clap at the same instant. Couch's device runs
/// at `couchBefore` frames until `switchAt` samples, then `couchAfter` -- an
/// IO size macOS moved because another app asked for a different one.
/// Kitchen's runs at 64 frames, or moves to `kitchenAfter` at
/// `kitchenSwitchAt` (an app or aggregate setting the size on both devices,
/// each taking it up at its own next period). Each device hands a block over
/// when its last sample is captured; the output pulls 64 at a time between
/// deliveries. Each device reports `...Latency` frames of input latency, and
/// so hands the clap over that much later. With `couchHearsIt` false only
/// Kitchen's microphone carries the clap, so the headphone output says
/// exactly when Kitchen reaches the headphones. `kitchenLevel` puts a steady
/// level on Kitchen's microphone between claps, so a gap in its stem shows.
IoSizeTakeResult recordWithIoSizes (int couchBefore, int couchAfter, long long switchAt, long long clapAt,
                                    int kitchenAfter = 64, long long kitchenSwitchAt = 0,
                                    int kitchenLatency = 0, int couchLatency = 0, bool couchHearsIt = true,
                                    float kitchenLevel = 0.0f)
{
    const auto dir = tempDir() + "/io_size_" + std::to_string (couchBefore) + "_" + std::to_string (couchAfter)
                   + "_" + std::to_string (kitchenAfter) + "_" + std::to_string (kitchenSwitchAt)
                   + "_" + std::to_string (switchAt) + "_" + std::to_string (couchLatency);
    std::remove ((dir + "/01_Kitchen.wav").c_str());
    std::remove ((dir + "/02_Couch.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
    REQUIRE (makeDir (dir));

    DeviceInputStream::setClockForTesting (ioSizeClock);
    ioSizeClockNs = 1;

    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = kitchenLatency;
    backend.inputLatencyFrames["dev-b"] = couchLatency;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);
    REQUIRE (c.startRecording (dir, 16, "2026-10-07T00:00:00Z"));

    std::vector<float> a (4096), b (4096), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    // A device with more input latency hands the same instant over later.
    const auto fill = [] (std::vector<float>& v, long long first, int n, long long heardAt, float level)
    {
        for (int i = 0; i < n; ++i)
            v[static_cast<size_t> (i)] = first + i == heardAt ? 0.9f : level;
    };

    const auto nsAt = [] (long long sample) { return static_cast<int64_t> (sample * 1.0e9 / 48000.0) + 1; };

    long long aNext = 0, bNext = 0, outFrame = 0;
    float headphonePeak = 0.0f;
    IoSizeTakeResult r;
    const long long end = clapAt + 48000;
    const auto kitchenBlock = [&] { return kitchenSwitchAt > 0 && aNext >= kitchenSwitchAt ? kitchenAfter : 64; };

    for (long long pullAt = 32; pullAt < end; pullAt += 64)
    {
        // Every delivery whose last sample was captured before this pull.
        while (aNext + kitchenBlock() <= pullAt || bNext + (bNext >= switchAt ? couchAfter : couchBefore) <= pullAt)
        {
            const int aBlock = kitchenBlock();
            const int bBlock = bNext >= switchAt ? couchAfter : couchBefore;

            if (aNext + aBlock <= bNext + bBlock)
            {
                ioSizeClockNs = nsAt (aNext + aBlock);
                fill (a, aNext, aBlock, clapAt + kitchenLatency, kitchenLevel);
                backend.inputCallbacks[0] (aIn, 1, nullptr, 0, aBlock);
                aNext += aBlock;
            }
            else
            {
                ioSizeClockNs = nsAt (bNext + bBlock);
                fill (b, bNext, bBlock, couchHearsIt ? clapAt + couchLatency : -1, 0.0f);
                backend.inputCallbacks[1] (bIn, 1, nullptr, 0, bBlock);
                bNext += bBlock;
            }
        }

        ioSizeClockNs = nsAt (pullAt);
        c.pullOutputBlock (outs, 1, 64);

        for (int i = 0; i < 64; ++i, ++outFrame)
            if (std::abs (out[static_cast<size_t> (i)]) > headphonePeak)
            {
                headphonePeak = std::abs (out[static_cast<size_t> (i)]);
                r.headphoneClap = outFrame;
            }
    }

    r.kitchenUnderruns = c.getUnderrunSamples (0);
    r.couchUnderruns = c.getUnderrunSamples (1);
    r.kitchenStreamShift = c.getChannelShiftSilenceSamples (0);
    r.couchStreamShift = c.getChannelShiftSilenceSamples (1);
    r.couchBlock = c.getDeviceIoBlockFrames ("dev-b");
    r.kitchenAlignment = c.getDeviceAlignmentDelayFrames ("dev-a");
    r.couchAlignment = c.getDeviceAlignmentDelayFrames ("dev-b");
    r.headphoneLatencyMs = c.getMonitoringLatencyMs();
    r.slowestHeadphoneLatencyMs = c.getSlowestMonitoringLatencyMs();
    r.slowestDevice = c.getSlowestMonitoringDeviceId();

    c.stopRecording();

    r.kitchenShift = c.getDeviceAlignmentSilenceFramesThisTake ("dev-a");
    r.couchShift = c.getDeviceAlignmentSilenceFramesThisTake ("dev-b");
    r.kitchenDropped = c.getDeviceAlignmentDroppedFramesThisTake ("dev-a");
    r.couchDropped = c.getDeviceAlignmentDroppedFramesThisTake ("dev-b");
    r.kitchenIoShift = c.getDeviceIoShiftFramesThisTake ("dev-a");
    r.couchIoShift = c.getDeviceIoShiftFramesThisTake ("dev-b");
    r.kitchenAlignmentStart = c.getDeviceAlignmentStartFrames ("dev-a");
    r.couchAlignmentStart = c.getDeviceAlignmentStartFrames ("dev-b");
    r.kitchenAlignmentAtStop = c.getDeviceAlignmentDelayFrames ("dev-a");
    r.stemsAligned = c.areStemsAligned();

    c.stopMonitoring();
    DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; });

    r.kitchenClap = loudestSampleIn (dir + "/01_Kitchen.wav");
    r.couchClap = loudestSampleIn (dir + "/02_Couch.wav");
    r.mixClap = loudestSampleIn (dir + "/MIX.wav");
    std::tie (r.kitchenSoundStarts, r.kitchenSilentAfter) = silenceAfterFirstSoundIn (dir + "/01_Kitchen.wav");
    return r;
}

} // namespace

TEST_CASE (CaptureCoordinator_MicsOnDevicesAtDifferentIoSizesLineUpInTheFiles)
{
    // Asked for 64, one device runs at 1156 -- CoreAudio leaves a device at
    // its own size when it refuses the request. A sample waits up to a whole
    // block in the device before it is handed over, so that stem carried
    // every sound 1092 samples (23 ms) after the other's: a slapback in the
    // mix wherever the two mics hear each other. The writer lines the stems
    // (and the mix summed from them) up.
    const auto r = recordWithIoSizes (1156, 1156, 0, 30000);

    REQUIRE (r.kitchenClap > 0);
    REQUIRE (r.couchClap > 0);
    REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);
    REQUIRE (r.kitchenUnderruns == 0u);
    REQUIRE (r.couchUnderruns == 0u);
    REQUIRE (r.stemsAligned);

    // And the record says why the Kitchen stem is held back.
    REQUIRE (r.couchBlock == 1156);
    REQUIRE (r.couchAlignment == 0);
    REQUIRE (r.kitchenAlignment == 1092);

    // The take began before either device had delivered, so Kitchen's stem
    // opened unheld and was held back as Couch's block settled, still in
    // pre-roll: the record's two ends and the silence between them agree.
    REQUIRE (r.kitchenAlignmentStart + r.kitchenShift - r.kitchenDropped == r.kitchenAlignment);
    REQUIRE (r.couchAlignmentStart == 0);

    // Which the headphones do not hear: two 64-frame buffers, the ring's
    // cushion (256 at 64), the resampler's two samples, and Kitchen's own
    // path -- nothing of Couch's block.
    REQUIRE_NEAR (r.headphoneLatencyMs, (2.0 * 64 + 256 + kResampler) / 48000.0 * 1000.0, 1e-6);
}

TEST_CASE (CaptureCoordinator_AnIoSizeThatGrowsMidTakeKeepsEveryStemInStep)
{
    // Couch's device goes from 64 to 1156 frames a second into the take.
    // That channel has to fall 1092 samples later -- the device now holds its
    // audio that long -- and the gap that opens is counted. The writer holds
    // Kitchen's stem back with it, so a clap afterwards still lands on the
    // same frame in both, and in the mix.
    const auto r = recordWithIoSizes (64, 1156, 48000, 96000);

    REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);
    REQUIRE (r.couchUnderruns >= 1092u - 2u);
    REQUIRE (r.couchUnderruns <= 1092u + 2u);
    REQUIRE (r.stemsAligned);

    // Kitchen's own audio was all there: it is held back, not lost -- and
    // the take's record says how much silence moving it put in its file.
    REQUIRE (r.kitchenUnderruns == 0u);
    REQUIRE (r.kitchenStreamShift == 0u);
    REQUIRE (r.kitchenAlignment == 1092);
    REQUIRE (r.kitchenShift == 1092);
    REQUIRE (r.kitchenDropped == 0);
    REQUIRE (r.couchShift == 0);
    REQUIRE (r.couchAlignment == 0);

    // The record keeps both ends: Kitchen's stem opened with no silence
    // (the devices matched then) and ended held back 1092, the silence
    // written in between.
    REQUIRE (r.kitchenAlignmentStart == 0);
    REQUIRE (r.kitchenAlignmentAtStop == 1092);
    REQUIRE (r.couchAlignmentStart == 0);
}

TEST_CASE (CaptureCoordinator_AGrowthOnTheQuickerDeviceComesOutOfItsOwnStem)
{
    // Couch's interface has 1400 frames more input latency than Kitchen's,
    // so Kitchen's stem is held back 1400 behind it. Then Kitchen's device
    // grows from 64 to 1156: its channel falls 1092 later (a counted gap in
    // the headphones), and the writer takes the same 1092 back out of
    // Kitchen's stem -- the silence its stream put in -- rather than holding
    // Couch back too. Both stems carry the later clap on one frame.
    const auto r = recordWithIoSizes (64, 64, 0, 96000, 1156, 48000, 0, 1400);

    REQUIRE (r.kitchenClap > 0);
    REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);
    REQUIRE (r.stemsAligned);

    REQUIRE (r.kitchenUnderruns + r.kitchenStreamShift >= 1092u - 2u);
    REQUIRE (r.kitchenUnderruns + r.kitchenStreamShift <= 1092u + 2u);
    REQUIRE (r.kitchenAlignment == 1400 - 1092);
    REQUIRE (r.kitchenDropped == 1092);
    REQUIRE (r.kitchenShift == 0);

    // Couch never moved, and nothing was done to its stem.
    REQUIRE (r.couchUnderruns == 0u);
    REQUIRE (r.couchAlignment == 0);
    REQUIRE (r.couchShift == 0);
    REQUIRE (r.couchDropped == 0);
}

TEST_CASE (CaptureCoordinator_AGrowthOnTheQuickerDeviceLeavesNoGapInItsStem)
{
    // The same growth with a steady level on Kitchen's microphone. Its
    // headphone feed had a gap -- the device held its audio 1092 samples
    // longer, and nothing could cover that -- but the writer took the
    // silence of that move back out of the stem, so the stem runs on without
    // one: once its sound starts, not a single silent sample. Likewise for a growth the ring's cushion covered
    // (64 -> 96), whose move is silence written at once rather than a dry
    // gap.
    for (const int grownTo : { 1156, 96 })
    {
        const auto r = recordWithIoSizes (64, 64, 0, 96000, grownTo, 48000, 0, 1400, true, 0.25f);

        REQUIRE (r.kitchenUnderruns + r.kitchenStreamShift >= static_cast<uint64_t> (grownTo - 64 - 2));
        REQUIRE (r.kitchenDropped >= grownTo - 64 - 2);
        REQUIRE (r.kitchenDropped <= grownTo - 64 + 2);
        // Couch's 1400 extra frames, and the stream's own pre-roll at the top
        // of the take, before any sound.
        REQUIRE (r.kitchenSoundStarts >= 1400);
        REQUIRE (r.kitchenSoundStarts < 1400 + 1024);
        REQUIRE (r.kitchenSilentAfter == 0);
        REQUIRE (r.kitchenIoShift == 0); // what its stream put in, the writer took out
        REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    }
}

TEST_CASE (CaptureCoordinator_AGrowthOnTheSlowestDeviceIsOnRecordWhereItLeftSilence)
{
    // Both devices start alike at 64 frames, and Kitchen's grows by less than
    // its ring's cushion (to 96, or 160). Its stream moves later by writing
    // the growth as silence -- no audio lost, no underrun -- and Kitchen is
    // now the slowest, so the writer holds Couch back by the same amount
    // rather than taking anything out of Kitchen. Kitchen's stem keeps that
    // gap, and the take's record says so, as Couch's says how much silence
    // it was given to match.
    for (const int grownTo : { 96, 160 })
    {
        const auto r = recordWithIoSizes (64, 64, 0, 96000, grownTo, 48000, 0, 0, true, 0.25f);

        REQUIRE (r.kitchenUnderruns == 0u);
        REQUIRE (r.kitchenStreamShift > 0u);
        REQUIRE (r.kitchenSilentAfter > 0);
        REQUIRE (r.kitchenDropped == 0);
        REQUIRE (r.kitchenIoShift == static_cast<int> (r.kitchenStreamShift));
        REQUIRE (std::llabs (r.kitchenIoShift - r.kitchenSilentAfter) <= 1);
        REQUIRE (std::llabs (r.couchShift - (grownTo - 64)) <= 1);
        REQUIRE (r.couchIoShift == 0);
        REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
        REQUIRE (r.stemsAligned);
    }
}

TEST_CASE (CaptureCoordinator_TwoDevicesGrowingAMomentApartEachMoveOnce)
{
    // Another app (or an aggregate) sets 1156 frames on both devices, and
    // each takes it up at its own next period: Couch a few hundred samples
    // after Kitchen, or more than a large block after. Each stream moves
    // once, by its own growth -- a counted gap where its ring ran dry, a
    // shift where its cushion covered it, never both for the same move --
    // and no stream is moved for the other's. The writer follows each move,
    // so the clap after both lands on one frame in both stems.
    for (const long long apart : { 320LL, 640LL, 1216LL, 1600LL })
    {
        const auto r = recordWithIoSizes (64, 1156, 48000 + apart, 96000, 1156, 48000);

        REQUIRE (r.kitchenClap > 0);
        REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
        REQUIRE (r.stemsAligned);

        REQUIRE (r.couchBlock == 1156);
        REQUIRE (r.kitchenUnderruns + r.kitchenStreamShift >= 1092u - 2u);
        REQUIRE (r.kitchenUnderruns + r.kitchenStreamShift <= 1092u + 2u);
        REQUIRE (r.couchUnderruns + r.couchStreamShift >= 1092u - 2u);
        REQUIRE (r.couchUnderruns + r.couchStreamShift <= 1092u + 2u);

        // Both ended where they started against each other.
        REQUIRE (r.kitchenAlignment == 0);
        REQUIRE (r.couchAlignment == 0);
    }
}

TEST_CASE (CaptureCoordinator_TheHeadphonesNeverWaitForASlowerDevice)
{
    // §5.4: audio monitoring comes first. Kitchen claps; Couch hears nothing.
    // Whatever Couch's device does -- report more input latency, run at a
    // larger IO block from the start, or grow its block mid-take -- Kitchen
    // reaches the headphones at exactly the same output frame as on a rig
    // where Couch is just like it. Lining the two up is the writer's job,
    // and the stems are still lined up.
    const auto alone = recordWithIoSizes (64, 64, 0, 96000, 64, 0, 0, 0, false);
    REQUIRE (alone.headphoneClap > 96000);

    const auto laggier = recordWithIoSizes (64, 64, 0, 96000, 64, 0, 0, 2000, false);
    const auto largerBlock = recordWithIoSizes (1156, 1156, 0, 96000, 64, 0, 0, 0, false);
    const auto grows = recordWithIoSizes (64, 1156, 48000, 96000, 64, 0, 0, 0, false);

    for (const auto* r : { &laggier, &largerBlock, &grows })
    {
        REQUIRE (r->headphoneClap == alone.headphoneClap);
        REQUIRE (r->kitchenUnderruns == 0u);
        REQUIRE_NEAR (r->headphoneLatencyMs, alone.headphoneLatencyMs, 1e-9);
        REQUIRE (r->stemsAligned);
    }

    // The slower device is slower in its own channel, and that is said too:
    // the headline figure alone would read the same for all four rigs.
    REQUIRE_NEAR (alone.slowestHeadphoneLatencyMs, alone.headphoneLatencyMs, 1e-9);
    REQUIRE_NEAR (laggier.slowestHeadphoneLatencyMs, alone.headphoneLatencyMs + 2000.0 / 48.0, 1e-6);
    REQUIRE_NEAR (largerBlock.slowestHeadphoneLatencyMs, alone.headphoneLatencyMs + 1092.0 / 48.0, 1e-6);
    REQUIRE_NEAR (grows.slowestHeadphoneLatencyMs, alone.headphoneLatencyMs + 1092.0 / 48.0, 1e-6);
    REQUIRE (largerBlock.slowestDevice == "dev-b");
    REQUIRE (laggier.slowestDevice == "dev-b");

    // The stems are where they were: Kitchen held back to the slower device.
    REQUIRE (laggier.kitchenAlignment == 2000);
    REQUIRE (largerBlock.kitchenAlignment == 1092);
    REQUIRE (grows.kitchenAlignment == 1092);
    REQUIRE (alone.kitchenAlignment == 0);
}

namespace {

/// A 16-bit mono stem's samples.
std::vector<int16_t> stemSamples (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.is_open());
    const auto bytes = readU32LE (f, kDataSizeOffset);
    f.seekg (kAudioDataOffset);
    std::vector<int16_t> v (bytes / 2);
    f.read (reinterpret_cast<char*> (v.data()), static_cast<std::streamsize> (v.size() * 2));
    return v;
}

struct BacklogTakeResult
{
    std::vector<int16_t> kitchen, couch;
    int kitchenShift = 0, kitchenDropped = 0, couchShift = 0, couchDropped = 0;
    int kitchenAlignment = 0;
    uint64_t kitchenUnderruns = 0;
    bool stemsAligned = false;
};

/// Kitchen's driver goes quiet at `backlogAt` and then hands the `backlog`
/// samples it held over in one delivery, going straight back to 64-frame
/// blocks: a late reader, not a device that grew its IO size. Both devices
/// otherwise run at 64. Kitchen's microphone carries a ramp (sample n of the
/// room reads n % 30000 + 1 in 16-bit steps) or, with `kitchenRamp` false,
/// a clap at `clapAt`; Couch's carries `couchLevel` and the same clap. Couch
/// reports `couchLatency` frames more input latency than Kitchen and hands
/// the room over that much later.
BacklogTakeResult recordBacklog (int couchLatency, long long backlogAt, int backlog,
                                 bool kitchenRamp, float couchLevel, long long clapAt)
{
    const auto dir = tempDir() + "/backlog_" + std::to_string (couchLatency) + "_" + std::to_string (backlog)
                   + (kitchenRamp ? "_ramp" : "_clap");
    std::remove ((dir + "/01_Kitchen.wav").c_str());
    std::remove ((dir + "/02_Couch.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
    REQUIRE (makeDir (dir));

    DeviceInputStream::setClockForTesting (ioSizeClock);
    ioSizeClockNs = 1;

    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = 0;
    backend.inputLatencyFrames["dev-b"] = couchLatency;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-10-07T00:00:00Z"));

    std::vector<float> a (4096), b (64), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };
    const auto nsAt = [] (long long sample) { return static_cast<int64_t> (sample * 1.0e9 / 48000.0) + 1; };

    long long aNext = 0, bNext = 0;

    for (long long pullAt = 32; pullAt < clapAt + 48000; pullAt += 64)
    {
        for (;;)
        {
            const int aBlock = aNext == backlogAt ? backlog : 64;
            const bool aDue = aNext + aBlock <= pullAt;
            const bool bDue = bNext + 64 <= pullAt;

            if (! aDue && ! bDue)
                break;

            if (aDue && (! bDue || aNext + aBlock <= bNext + 64))
            {
                ioSizeClockNs = nsAt (aNext + aBlock);

                for (int i = 0; i < aBlock; ++i)
                {
                    const long long n = aNext + i;
                    a[static_cast<size_t> (i)] = kitchenRamp ? static_cast<float> (n % 30000 + 1) / 32768.0f
                                                             : (n == clapAt ? 0.9f : 0.0f);
                }

                backend.inputCallbacks[0] (aIn, 1, nullptr, 0, aBlock);
                aNext += aBlock;
            }
            else
            {
                ioSizeClockNs = nsAt (bNext + 64);

                for (int i = 0; i < 64; ++i)
                    b[static_cast<size_t> (i)] = bNext + i == clapAt + couchLatency ? 0.9f : couchLevel;

                backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 64);
                bNext += 64;
            }
        }

        ioSizeClockNs = nsAt (pullAt);
        c.pullOutputBlock (outs, 1, 64);
    }

    BacklogTakeResult r;
    r.kitchenUnderruns = c.getUnderrunSamples (0);
    r.kitchenAlignment = c.getDeviceAlignmentDelayFrames ("dev-a");

    c.stopRecording();

    r.kitchenShift = c.getDeviceAlignmentSilenceFramesThisTake ("dev-a");
    r.kitchenDropped = c.getDeviceAlignmentDroppedFramesThisTake ("dev-a");
    r.couchShift = c.getDeviceAlignmentSilenceFramesThisTake ("dev-b");
    r.couchDropped = c.getDeviceAlignmentDroppedFramesThisTake ("dev-b");
    r.stemsAligned = c.areStemsAligned();

    c.stopMonitoring();
    DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; });

    r.kitchen = stemSamples (dir + "/01_Kitchen.wav");
    r.couch = stemSamples (dir + "/02_Couch.wav");
    return r;
}

long long loudestOf (const std::vector<int16_t>& s)
{
    long long best = -1;
    int magnitude = 0;

    for (size_t i = 0; i < s.size(); ++i)
        if (std::abs (static_cast<int> (s[i])) > magnitude)
        {
            magnitude = std::abs (static_cast<int> (s[i]));
            best = static_cast<long long> (i);
        }

    return best;
}

} // namespace

TEST_CASE (CaptureCoordinator_ADriverBacklogMovesNoOtherStem)
{
    // Kitchen's driver hands over 1152 samples in one piece and goes back to
    // 64-frame blocks. Until its next delivery that looks like a device that
    // grew its IO size, so Kitchen's own stream holds room for it -- but the
    // rig's reference follows settled blocks only, so Couch's stem, carrying
    // a steady level, is never held back for it: not one silent sample once
    // its sound starts, nothing written into it or taken out, and the clap
    // after it lands on one frame in both stems.
    const auto r = recordBacklog (0, 48000, 1152, false, 0.25f, 96000);

    long long first = -1, silent = 0;

    for (size_t i = 0; i < r.couch.size(); ++i)
    {
        if (r.couch[i] != 0 && first < 0)
            first = static_cast<long long> (i);
        else if (r.couch[i] == 0 && first >= 0)
            ++silent;
    }

    REQUIRE (first >= 0);
    REQUIRE (silent == 0);
    REQUIRE (r.couchShift == 0);
    REQUIRE (r.couchDropped == 0);
    REQUIRE (r.kitchenUnderruns > 0u); // Kitchen's gap was real, and counted
    REQUIRE (std::llabs (loudestOf (r.kitchen) - loudestOf (r.couch)) <= 1);
    REQUIRE (r.stemsAligned);
}

namespace {

struct HeldRoomTakeResult
{
    long long kitchenClap = -1, couchClap = -1, mixClap = -1;
    long long headphoneClapSpread = -1;   // first to last loud headphone frame
    uint64_t releases = 0, released = 0;
    size_t couchHeldAtEnd = 0;
    double quickMs = 0.0, slowestWhileHeld = 0.0, slowestAtEnd = 0.0, quickAtEnd = 0.0;
    int kitchenDropped = 0, kitchenShift = 0, couchDropped = 0, couchShift = 0;
    bool aligned = false;
    int nextTakeKitchenStart = -1, nextTakeCouchStart = -1;

    // With a tone: the last frame of it in each stem, and the longest run of
    // digital silence inside it in Couch's.
    long long kitchenToneEnd = -1, couchToneEnd = -1, couchSilenceInTone = -1;
};

// Another app holds Couch's device at 1156 frames for a second, then lets it
// go back to 64. Silent but for one clap both microphones hear at once, after
// the room has been given back -- and, when `toneUntil` is given, a steady
// 1 kHz tone at -12 dBFS both hear from the start until then (the clap is
// louder). Couch's deliveries may run `couchLate` samples late for a moment
// around when the room is given back (3.875 s to 4.25 s); after the take, a
// second one starts on the same streams.
HeldRoomTakeResult recordAnIoSizeExcursion (int kitchenLatency, int couchLate = 0, long long toneUntil = 0)
{
    const auto dir = tempDir() + "/io_size_return_" + std::to_string (kitchenLatency) + "_" + std::to_string (couchLate)
                   + "_" + std::to_string (toneUntil);
    std::remove ((dir + "/01_Kitchen.wav").c_str());
    std::remove ((dir + "/02_Couch.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
    REQUIRE (makeDir (dir));
    const auto nextDir = dir + "_next";
    REQUIRE (makeDir (nextDir));

    DeviceInputStream::setClockForTesting (ioSizeClock);
    ioSizeClockNs = 1;

    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = kitchenLatency;
    backend.inputLatencyFrames["dev-b"] = 0;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);
    REQUIRE (c.startRecording (dir, 16, "2026-10-12T00:00:00Z"));

    HeldRoomTakeResult r;
    r.quickMs = c.getMonitoringLatencyMs();

    constexpr long long growAt = 48000, returnAt = 96000, clapAt = 288000, end = 336000;
    constexpr long long lateFrom = 186000, lateUntil = 204000;
    std::vector<float> a (4096, 0.0f), b (4096, 0.0f), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    const auto nsAt = [] (long long sample) { return static_cast<int64_t> (sample * 1.0e9 / 48000.0) + 1; };
    const auto couchBlock = [] (long long next) { return next >= growAt && next < returnAt ? 1156 : 64; };
    const auto couchDue = [&] (long long next)
    {
        return next + couchBlock (next) + (next >= lateFrom && next < lateUntil ? couchLate : 0);
    };
    const auto fill = [toneUntil] (std::vector<float>& v, long long first, int n, long long heardAt)
    {
        for (int i = 0; i < n; ++i)
        {
            // Source time: the clap is at clapAt, heard `heardAt - clapAt` late.
            const long long t = first + i - (heardAt - clapAt);
            const float tone = t >= 0 && t < toneUntil
                                   ? static_cast<float> (0.25 * std::sin (2.0 * 3.141592653589793 * 1000.0
                                                                          * static_cast<double> (t) / 48000.0))
                                   : 0.0f;
            v[static_cast<size_t> (i)] = first + i == heardAt ? 0.3f : tone;
        }
    };

    long long aNext = 0, bNext = 0, outFrame = 0, firstLoud = -1, lastLoud = -1, pullAt = 32;

    const auto runUntil = [&] (long long until)
    {
        for (; pullAt < until; pullAt += 64)
        {
            while (aNext + 64 <= pullAt || couchDue (bNext) <= pullAt)
            {
                const int bBlock = couchBlock (bNext);

                if (aNext + 64 <= couchDue (bNext))
                {
                    ioSizeClockNs = nsAt (aNext + 64);
                    fill (a, aNext, 64, clapAt + kitchenLatency);
                    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
                    aNext += 64;
                }
                else
                {
                    ioSizeClockNs = nsAt (couchDue (bNext));
                    fill (b, bNext, bBlock, clapAt);
                    backend.inputCallbacks[1] (bIn, 1, nullptr, 0, bBlock);
                    bNext += bBlock;
                }
            }

            ioSizeClockNs = nsAt (pullAt);
            c.pullOutputBlock (outs, 1, 64);

            // Back at 64, but not yet for two seconds: the room is still held.
            if (pullAt > returnAt + 48000 && pullAt < returnAt + 72000)
                r.slowestWhileHeld = c.getSlowestMonitoringLatencyMs();

            // The clap, not the tone: nothing reaches the headphones before
            // it is heard.
            for (int i = 0; i < 64; ++i, ++outFrame)
                if (outFrame >= clapAt && std::abs (out[static_cast<size_t> (i)]) > 0.1f)
                {
                    firstLoud = firstLoud < 0 ? outFrame : firstLoud;
                    lastLoud = outFrame;
                }
        }
    };

    runUntil (end);

    const auto couch = c.getChannelHeldRoom (1);
    r.releases = couch.releases;
    r.released = couch.releasedSamples;
    r.couchHeldAtEnd = couch.heldBlock;
    r.slowestAtEnd = c.getSlowestMonitoringLatencyMs();
    r.quickAtEnd = c.getMonitoringLatencyMs();
    r.headphoneClapSpread = firstLoud < 0 ? -1 : lastLoud - firstLoud;

    c.stopRecording();
    r.kitchenDropped = c.getDeviceAlignmentDroppedFramesThisTake ("dev-a");
    r.kitchenShift = c.getDeviceAlignmentSilenceFramesThisTake ("dev-a");
    r.couchDropped = c.getDeviceAlignmentDroppedFramesThisTake ("dev-b");
    r.couchShift = c.getDeviceAlignmentSilenceFramesThisTake ("dev-b");
    r.aligned = c.areStemsAligned();

    // The next take, on the same streams: lined up to the rig as it stands.
    runUntil (end + 4800);
    REQUIRE (c.startRecording (nextDir, 16, "2026-10-12T00:01:00Z"));
    runUntil (end + 24000);
    r.nextTakeKitchenStart = c.getDeviceAlignmentStartFrames ("dev-a");
    r.nextTakeCouchStart = c.getDeviceAlignmentStartFrames ("dev-b");
    c.stopRecording();

    c.stopMonitoring();
    DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; });

    r.kitchenClap = loudestSampleIn (dir + "/01_Kitchen.wav");
    r.couchClap = loudestSampleIn (dir + "/02_Couch.wav");
    r.mixClap = loudestSampleIn (dir + "/MIX.wav");

    if (toneUntil > 0)
    {
        // Loud enough to be the tone (-30 dBFS), and before the clap.
        const auto lastOfTone = [] (const std::vector<int16_t>& stem, long long before, long long& first)
        {
            long long last = -1;
            first = -1;

            for (long long i = 0; i < std::min (before, static_cast<long long> (stem.size())); ++i)
                if (std::abs (static_cast<int> (stem[static_cast<size_t> (i)])) > 1000)
                {
                    first = first < 0 ? i : first;
                    last = i;
                }

            return last;
        };

        const auto kitchen = stemSamples (dir + "/01_Kitchen.wav");
        const auto couch = stemSamples (dir + "/02_Couch.wav");
        long long kitchenFirst = -1, couchFirst = -1;
        r.kitchenToneEnd = lastOfTone (kitchen, r.kitchenClap - 4800, kitchenFirst);
        r.couchToneEnd = lastOfTone (couch, r.couchClap - 4800, couchFirst);

        long long run = 0;
        r.couchSilenceInTone = 0;

        for (long long i = couchFirst; i >= 0 && i <= r.couchToneEnd; ++i)
        {
            run = couch[static_cast<size_t> (i)] == 0 ? run + 1 : 0;
            r.couchSilenceInTone = std::max (r.couchSilenceInTone, run);
        }
    }

    return r;
}

} // namespace

TEST_CASE (CaptureCoordinator_RoomHeldForABlockTheDeviceLeftIsGivenBackAndTheStemsStayInLine)
{
    // The room Couch's ring held for 1156 used to stay held for the rest of
    // the session: Couch 23 ms later than Kitchen in the headphones for
    // good -- the talker hearing themselves late, and their voice in the mix
    // twice, 23 ms apart, through Kitchen's microphone. Two seconds after the
    // device is back at 64 the room is given back at the first quiet moment,
    // and the stems stay in line through it: Couch's is held back by what its
    // stream dropped, and nothing is taken out of Kitchen's.
    const auto r = recordAnIoSizeExcursion (0);

    // Given back, once, exactly the room, and the headphone delay the app
    // names said so: Couch's own path was 1092 samples longer while it was
    // held, and is Kitchen's again now.
    REQUIRE (r.releases == 1u);
    REQUIRE (r.released == 1092u);
    REQUIRE_NEAR (r.slowestWhileHeld - r.quickMs, 1092.0 / 48.0, 1e-6);
    REQUIRE_NEAR (r.slowestAtEnd, r.quickAtEnd, 1e-9);
    REQUIRE (r.couchHeldAtEnd == 64u);

    // In the headphones the clap is one clap: both microphones' at the same
    // frame, not two 23 ms apart.
    REQUIRE (r.headphoneClapSpread >= 0);
    REQUIRE (r.headphoneClapSpread <= 2);

    // In the files: one frame in both stems and the mix. Couch was the
    // slowest device while it ran at 1156, and the stems stay lined up to
    // that: Couch's stem is held back by what its stream dropped -- silence
    // where a moment of quiet was -- and nothing came out of Kitchen's.
    REQUIRE (r.kitchenClap > 0);
    REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);
    REQUIRE (r.kitchenDropped == 0);
    REQUIRE (static_cast<uint64_t> (r.couchShift) == r.released);
    REQUIRE (r.aligned);

    // That 1092 the stems were lined up to is for the take it happened in.
    // The next take starts from the rig as it stands, both devices at 64:
    // neither stem opens 23 ms late.
    REQUIRE (r.nextTakeKitchenStart == 0);
    REQUIRE (r.nextTakeCouchStart == 0);
}

TEST_CASE (CaptureCoordinator_RoomGivenBackWhileADeliveryIsLateKeepsTheStemsInLine)
{
    // Couch's deliveries run a millisecond late just as its room is given
    // back, so its ring reads 48 samples lower than it will a moment later.
    // Dropping what the level read above the smaller target moved Couch by
    // 48 samples less than the 1092 the writer held its stem back by: the
    // clap 48 samples apart in the files. Exactly the room is dropped.
    const auto r = recordAnIoSizeExcursion (0, 48);

    REQUIRE (r.releases == 1u);
    REQUIRE (r.released == 1092u);
    REQUIRE (r.kitchenClap > 0);
    REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);
    REQUIRE (r.headphoneClapSpread <= 2);
    REQUIRE (r.aligned);
}

TEST_CASE (CaptureCoordinator_RoomGivenBackOnTheQuickerDeviceIsNotTakenForARefusedBlock)
{
    // Kitchen's interface reports 1400 frames more input latency, so Couch is
    // the quicker device throughout: when it grew to 1156 the writer took
    // the move's silence back out of its stem, provisionally, then for good
    // once the device confirmed the size. Giving the room back later is a
    // move of its own -- the held block falls from a settled one -- and the
    // writer must hold the stem back for it, not "undo" the growth by
    // putting that silence back where it was taken from (which also struck
    // the 1092 samples from the take's record of what it had taken out).
    const auto r = recordAnIoSizeExcursion (1400);

    REQUIRE (r.releases == 1u);
    REQUIRE (r.released == 1092u);

    // In the headphones Couch is back to its own path: its clap 1400
    // samples before Kitchen's, not 308.
    REQUIRE (std::llabs (r.headphoneClapSpread - 1400) <= 2);

    REQUIRE (r.kitchenClap > 0);
    REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
    REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);

    // The record: the growth's silence taken out of Couch's stem, the room
    // given back as silence written into it, and Kitchen's stem untouched.
    REQUIRE (r.couchDropped >= 1092 - 2);
    REQUIRE (r.couchDropped <= 1092 + 2);
    REQUIRE (static_cast<uint64_t> (r.couchShift) == r.released);
    REQUIRE (r.kitchenDropped == 0);
    REQUIRE (r.kitchenShift == 0);
    REQUIRE (r.aligned);

    // Kitchen is the slowest device in the next take too: Couch opens 1400
    // samples late, Kitchen on time.
    REQUIRE (r.nextTakeKitchenStart == 0);
    REQUIRE (r.nextTakeCouchStart == 1400);
}

TEST_CASE (CaptureCoordinator_RoomGivenBackOnTheQuickerDeviceLeavesNoGapInItsSound)
{
    // Couch is the quicker device, held back in its stem behind Kitchen
    // (which reports 1400 frames more input latency, then 3000): 308 samples
    // while Couch runs at 1156, 1908 at 3000. Both microphones hear a tone
    // until a moment before Couch's room is given back. Couch's stream
    // skips that much quiet at the newest sample it has handed over, so the
    // silence the writer puts in for it belongs there, behind the samples
    // already waiting in Couch's line. Written at the head of the line it
    // landed inside the tone -- 1092 samples of digital silence, with tone
    // either side of it -- and the waiting tone came out 1092 samples late:
    // Couch's tone ended 23 ms after Kitchen's in the files.
    for (const int kitchenLatency : { 1400, 3000 })
    {
        const auto r = recordAnIoSizeExcursion (kitchenLatency, 0, 200000);

        REQUIRE (r.releases == 1u);
        REQUIRE (r.released == 1092u);

        REQUIRE (r.couchToneEnd > 0);
        REQUIRE (r.couchSilenceInTone >= 0);
        REQUIRE (r.couchSilenceInTone <= 2); // a sine's own zero crossing
        REQUIRE (std::llabs (r.kitchenToneEnd - r.couchToneEnd) <= 1);

        REQUIRE (std::llabs (r.kitchenClap - r.couchClap) <= 1);
        REQUIRE (std::llabs (r.mixClap - r.couchClap) <= 1);
        REQUIRE (static_cast<uint64_t> (r.couchShift) == r.released);
        REQUIRE (r.kitchenDropped == 0);
        REQUIRE (r.kitchenShift == 0);
        REQUIRE (r.aligned);
    }
}

TEST_CASE (CaptureCoordinator_ADriverBacklogMovesNothingInItsOwnStem)
{
    // The same backlog on Kitchen, now the quicker device: Couch reports more
    // input latency, so Kitchen's stem is held back behind it. For the one
    // device period the large delivery is provisional, Kitchen's own place
    // includes it, and the writer brings Kitchen's stem forward by it -- the
    // gap's silence out of the newest end of the line. The device refuses
    // it, and the writer puts that silence back where it was: Kitchen's stem
    // is exactly the one it has on a rig of like devices, held back by the
    // difference. The audio waiting from before the gap stays before it, and
    // nothing is recorded as alignment, since no IO size changed.
    const auto reference = recordBacklog (0, 48000, 1152, true, 0.0f, 96000);
    REQUIRE (reference.kitchenUnderruns > 0u);

    for (const int couchLatency : { 1400, 300 })
    {
        const auto r = recordBacklog (couchLatency, 48000, 1152, true, 0.0f, 96000);

        REQUIRE (r.kitchenAlignment == couchLatency);
        REQUIRE (r.kitchenShift == 0);
        REQUIRE (r.kitchenDropped == 0);
        REQUIRE (r.stemsAligned);

        REQUIRE (r.kitchen.size() == reference.kitchen.size());
        for (size_t i = 0; i < static_cast<size_t> (couchLatency); ++i)
            REQUIRE (r.kitchen[i] == 0);
        for (size_t i = static_cast<size_t> (couchLatency); i < r.kitchen.size(); ++i)
            REQUIRE (r.kitchen[i] == reference.kitchen[i - static_cast<size_t> (couchLatency)]);
    }
}

TEST_CASE (CaptureCoordinator_SaysWhatALargerBufferWillCostTheHeadphones)
{
    // The buffer ladder tells the user the headphone delay at the size it is
    // about to rebuild at: the quickest microphone's own path, since no
    // channel waits for any other. A device that refuses the size asked for
    // and runs at 1156 frames costs its own channel its block, not the
    // figure.
    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.inputLatencyFrames["dev-a"] = 100;
    backend.inputLatencyFrames["dev-b"] = 40;

    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // Before any device delivers, its block is the size asked for, and Couch
    // has the shorter input latency; Kitchen is the slower path.
    REQUIRE (c.getMonitorInputFrames (128) == 40 + 128);
    REQUIRE (c.getSlowestMonitorInputFrames (128) == 100 + 128);
    REQUIRE (c.getSlowestMonitoringDeviceId() == "dev-a");
    // Each with the round trip, the cushion every ring holds (256 at 64)
    // and the resampler's two samples.
    REQUIRE_NEAR (c.getMonitoringLatencyMs(), (2.0 * 64 + 256 + kResampler + 40) / 48000.0 * 1000.0, 1e-9);
    REQUIRE_NEAR (c.getSlowestMonitoringLatencyMs(), (2.0 * 64 + 256 + kResampler + 100) / 48000.0 * 1000.0, 1e-9);

    std::vector<float> a (64, 0.0f), b (1156, 0.0f), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    for (int i = 0; i < 2; ++i)
        backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 1156);
    for (int i = 0; i < 4; ++i)
        backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
    c.pullOutputBlock (outs, 1, 64);

    REQUIRE (c.getDeviceIoBlockFrames ("dev-b") == 1156);

    // Couch's block now outweighs Kitchen's longer latency, so Kitchen is the
    // quicker path; at 2048 the new size is the block everywhere.
    REQUIRE (c.getMonitorInputFrames (128) == 100 + 128);
    REQUIRE (c.getMonitorInputFrames (2048) == 40 + 2048);

    // Couch is now the slower path, by its block, and the ladder says so.
    REQUIRE (c.getSlowestMonitorInputFrames (128) == 40 + 1156);
    REQUIRE (c.getSlowestMonitorInputFrames (2048) == 100 + 2048);
    REQUIRE (c.getSlowestMonitoringDeviceId() == "dev-b");

    // At the size it runs at now it is what the headphone figure carries:
    // Kitchen's 100 frames of input latency, none of Couch's 1156.
    REQUIRE_NEAR (c.getMonitoringLatencyMs(), (2.0 * 64 + 256 + kResampler + 100) / 48000.0 * 1000.0, 1e-9);

    // And the slowest figure is Couch's own path: its block in place of the
    // one asked for, and its input latency.
    REQUIRE_NEAR (c.getSlowestMonitoringLatencyMs(), (64.0 + 256 + kResampler + 1156 + 40) / 48000.0 * 1000.0, 1e-9);

    // What the ladder says a step to 128 will cost: the round trip and the
    // cushion at 128 (a pull and 6 ms: 416), and each path's own input side
    // at it.
    REQUIRE_NEAR (c.getMonitoringLatencyMsAt (128), (2.0 * 128 + 416 + kResampler + 100) / 48000.0 * 1000.0, 1e-9);
    REQUIRE_NEAR (c.getSlowestMonitoringLatencyMsAt (128), (128.0 + 416 + kResampler + 1156 + 40) / 48000.0 * 1000.0, 1e-9);

    c.stopMonitoring();
    REQUIRE (c.getMonitorInputFrames (128) == 128);
    REQUIRE (c.getSlowestMonitorInputFrames (128) == 128);
    REQUIRE (c.getSlowestMonitoringDeviceId().empty());

    // Nothing monitoring, no headphone delay to announce.
    REQUIRE (c.getMonitoringLatencyMsAt (128) == 0.0);
    REQUIRE (c.getSlowestMonitoringLatencyMsAt (128) == 0.0);
}

TEST_CASE (CaptureCoordinator_ARigTheLadderTookPastTheCeilingNamesNoMicrophoneForABlock)
{
    // At 128 frames every microphone's own path is past 10 ms (round trip,
    // the ring's cushion and its input side: 14 ms). Couch's interface
    // reports 48 frames more input latency than Kitchen's -- one millisecond
    // -- and was named in the activity log as running "at a larger buffer
    // than the others", with advice to close apps and replug it.
    FakeBackend backend;
    backend.grantedOutputBufferFrames = 128;
    backend.inputLatencyFrames["dev-a"] = 0;
    backend.inputLatencyFrames["dev-b"] = 48;

    CaptureCoordinator c (backend, 48000.0, 128);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    REQUIRE (c.getMonitoringLatencyMs() > SlowMicrophoneNotice::kCeilingMs);
    REQUIRE_NEAR (c.getSlowestMonitoringLatencyMs() - c.getMonitoringLatencyMs(), 1.0, 1e-9);
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (c.getMonitoringLatencyMs(), c.getSlowestMonitoringLatencyMs()));

    // Another app holding Couch's interface at 1156 frames is still named.
    std::vector<float> a (1156, 0.0f), b (1156, 0.0f), out (128);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    for (int i = 0; i < 2; ++i)
        backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 1156);
    for (int i = 0; i < 4; ++i)
        backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 128);
    c.pullOutputBlock (outs, 1, 128);

    REQUIRE (c.getSlowestMonitoringDeviceId() == "dev-b");
    REQUIRE (SlowMicrophoneNotice::isFarBehind (c.getMonitoringLatencyMs(), c.getSlowestMonitoringLatencyMs()));
}

TEST_CASE (CaptureCoordinator_TheLadderAnnouncesTheHeadphoneDelayThePanelThenShows)
{
    // The buffer-ladder notice said "the headphone delay is now about X ms"
    // from a sum of its own -- three blocks and the quickest input side --
    // while the Advanced panel, session.json and the slow-microphone check
    // read the coordinator's, which had no ring cushion in it: at 128 the
    // notice said 10.7 ms and the panel, a moment later, 5.3. Both now come
    // from one sum, and the figure announced before the step is the one
    // shown after it.
    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.outputPresentationLatencyFrames = 30;
    backend.inputLatencyFrames["dev-a"] = 100;
    backend.inputLatencyFrames["dev-b"] = 40;

    std::vector<float> a (2048, 0.0f), b (2048, 0.0f), out (2048);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };

    // Couch's interface is held at 1156 by another app; Kitchen runs at the
    // size asked for.
    auto deliver = [&] (CaptureCoordinator& c, int size)
    {
        for (int i = 0; i < 2; ++i)
            backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 1156);
        for (int i = 0; i < 4; ++i)
            backend.inputCallbacks[0] (aIn, 1, nullptr, 0, size);
        c.pullOutputBlock (outs, 1, size);
    };

    CaptureCoordinator before (backend, 48000.0, 64);
    before.setSoftwareClockEnabled (false);
    REQUIRE (before.startMonitoring (twoMics(), "out-device"));
    deliver (before, 64);

    const double announced = before.getMonitoringLatencyMsAt (128);
    const double announcedSlowest = before.getSlowestMonitoringLatencyMsAt (128);
    REQUIRE (announced > before.getMonitoringLatencyMs());
    REQUIRE (announcedSlowest - announced >= 1.0);
    before.stopMonitoring();

    // The rig rebuilt at 128, the way the app does it: a new coordinator,
    // the output granted the size asked for.
    backend.grantedOutputBufferFrames = 128;
    backend.inputCallbacks.clear();
    CaptureCoordinator after (backend, 48000.0, 128);
    after.setSoftwareClockEnabled (false);
    REQUIRE (after.startMonitoring (twoMics(), "out-device"));
    deliver (after, 128);

    REQUIRE_NEAR (after.getMonitoringLatencyMs(), announced, 1e-9);
    REQUIRE_NEAR (after.getSlowestMonitoringLatencyMs(), announcedSlowest, 1e-9);
}

namespace {

/// Plays `seconds` of `signal` into dev-a with the output pulling, faster
/// than real time but waiting for the feedback guard's own thread to catch up
/// every fifth of a second of audio; true once the headphones were cut.
/// `outPeakAfterCut` is the loudest headphone sample after it.
template <typename Signal>
bool playIntoTheHeadphones (CaptureCoordinator& c, FakeBackend& backend, double seconds,
                            Signal signal, float* outPeakAfterCut = nullptr)
{
    std::vector<float> a (64), b (64, 0.0f), out (64);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    float* outs[] = { out.data() };
    long long n = 0;
    bool cut = false;
    float peakAfter = 0.0f;

    for (int block = 0; block < static_cast<int> (seconds * 48000.0 / 64); ++block)
    {
        for (auto& s : a)
            s = signal (n++);

        backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 64);
        c.pullOutputBlock (outs, 1, 64);

        if (cut)
            for (const auto s : out)
                peakAfter = std::max (peakAfter, std::abs (s));

        cut = cut || c.getMonitorBus().isRunawayMuted();

        // Never so far ahead of the guard's thread that its one-second ring
        // overflows (which would restart the analysis): wait until it has
        // analysed everything so far, however slowly a loaded machine
        // schedules it. A thread that never runs fails the test here.
        if (block % 150 == 149 || block + 1 == static_cast<int> (seconds * 48000.0 / 64))
        {
            for (int wait = 0; wait < 1000 && c.getFeedbackGuard().getPendingSamples() > 0; ++wait)
                std::this_thread::sleep_for (std::chrono::milliseconds (5));

            REQUIRE (c.getFeedbackGuard().getPendingSamples() == 0u);
            cut = cut || c.getMonitorBus().isRunawayMuted();
        }
    }

    REQUIRE (c.getFeedbackGuard().getDroppedSamples() == 0u);

    // The last block read out of the ring may still be in the analysis.
    for (int wait = 0; wait < 50 && ! cut; ++wait)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
        cut = c.getMonitorBus().isRunawayMuted();
    }

    if (outPeakAfterCut != nullptr)
        *outPeakAfterCut = peakAfter;

    return cut;
}

} // namespace

TEST_CASE (CaptureCoordinator_FeedbackBuildingInTheHeadphonesCutsThem)
{
    // §5.5, in the shipping path: a 1.6 kHz howl climbing 20 dB a second from
    // -50 dBFS, the whole way under the limiter's -3 dBFS ceiling. The 500 ms
    // runaway cut never sees it until it is already at full level in
    // everyone's ears; the band detector does. MonitorBus's detector was only
    // ever called by a UI test script, so the app shipped without it.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    float peakAfterCut = 1.0f;
    const bool cut = playIntoTheHeadphones (c, backend, 2.2, [] (long long n)
    {
        const double t = static_cast<double> (n) / 48000.0;
        return static_cast<float> (std::pow (10.0, (-50.0 + 20.0 * t) / 20.0)
                                   * std::sin (6.283185307179586 * 1600.0 * t));
    }, &peakAfterCut);

    REQUIRE (cut);
    REQUIRE (c.getFeedbackGuard().getCutCount() == 1u);
    REQUIRE_NEAR (c.getFeedbackGuard().getLastCutFrequencyHz(), 1600.0, 1.0);
    REQUIRE (peakAfterCut == 0.0f);
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_ASteadyLoudToneDoesNotCutTheHeadphones)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    REQUIRE_FALSE (playIntoTheHeadphones (c, backend, 1.5, [] (long long n)
    {
        return static_cast<float> (0.4 * std::sin (6.283185307179586 * 440.0 * static_cast<double> (n) / 48000.0));
    }));
    REQUIRE (c.getFeedbackGuard().getCutCount() == 0u);

    // Watched for as long as the headphones play, and no longer.
    REQUIRE (c.getFeedbackGuard().isRunning());
    c.stopMonitoring();
    REQUIRE_FALSE (c.getFeedbackGuard().isRunning());

    // Input-only, there is no headphone mix to watch.
    REQUIRE (c.startMonitoring (twoMics(), ""));
    REQUIRE_FALSE (c.getFeedbackGuard().isRunning());
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_SaysWhyAMicrophoneWouldNotOpen)
{
    // §0.1: the backend knows the cause and the coordinator used to discard it,
    // leaving the user to guess between a dead cable, a sample-rate mismatch, a
    // missing macOS permission and another app holding the interface. Those
    // have four different fixes and one message.
    FakeBackend backend;
    backend.failInputOpen = true;
    backend.inputOpenError = "This interface is running at 48 kHz and won't change to the 44.1 kHz "
                             "this recording uses.";

    CaptureCoordinator c (backend, 48000.0, 64);

    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE_FALSE (c.startMonitoring (twoMics(), "out-device"));

    const auto problem = c.getMonitorProblem();

    // Still names the microphone, so the user knows which one.
    REQUIRE (problem.find ("Kitchen") != std::string::npos);
    // And now says what to do about it.
    REQUIRE (problem.find ("48 kHz") != std::string::npos);
    REQUIRE (problem.find ("44.1 kHz") != std::string::npos);
}

TEST_CASE (CaptureCoordinator_ProducesTheMonitorMixFromEveryMic)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0); // unity output stage, so this test sees the bus itself

    std::vector<float> a (64, 0.10f), b (64, 0.20f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    c.processAudioBlock (ins, 2, outs, 1, 64);

    // §5.1: the mix contains every microphone, summed at unity. Both are
    // present, so the output must exceed either one alone.
    REQUIRE (outL[0] > 0.20f);
}

TEST_CASE (CaptureCoordinator_EveryOutputChannelGetsTheSameMix)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.15f), b (64, 0.15f);
    std::vector<float> outL (64, 0.0f), outR (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data(), outR.data() };

    c.processAudioBlock (ins, 2, outs, 2, 64);

    // §5.1: one mix, identical for every listener. No per-listener variation.
    REQUIRE_NEAR (outL[0], outR[0], 1e-9);
}

TEST_CASE (CaptureCoordinator_ASwitchedOffHeadphoneJackGetsSilenceAndTheRestTheMix)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // Two microphones' jacks on the combined device, two channels each; the
    // second person has switched theirs off.
    c.setHeadphoneJackGains ("out-device", { 1.0f, 1.0f, 0.0f, 0.0f });

    std::vector<float> a (64, 0.15f), b (64, 0.15f);
    std::vector<float> o0 (64, 0.5f), o1 (64, 0.5f), o2 (64, 0.5f), o3 (64, 0.5f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { o0.data(), o1.data(), o2.data(), o3.data() };

    c.processAudioBlock (ins, 2, outs, 4, 64);

    REQUIRE (std::abs (o0[10]) > 0.01f);
    REQUIRE_NEAR (o0[10], o1[10], 1e-9);
    REQUIRE (o2[10] == 0.0f);
    REQUIRE (o3[10] == 0.0f);

    // Switched back on while running.
    c.setHeadphoneJackGains ("out-device", {});
    c.processAudioBlock (ins, 2, outs, 4, 64);
    REQUIRE_NEAR (o3[10], o0[10], 1e-9);
}

namespace
{
    float loudestOf (const std::vector<float>& block)
    {
        float peak = 0.0f;
        for (float v : block)
            peak = std::max (peak, std::abs (v));
        return peak;
    }
}

TEST_CASE (CaptureCoordinator_ASwitchedOffJackStillGetsTheAlarmButNeverTheMix)
{
    // A jack switched off from the mix used to get zeros, the alarm included,
    // so the person who switched theirs off never heard the siren. They get
    // the app's own sounds alone: the mix still never reaches them (§5.2),
    // and the jacks that hear the mix get the mix with the siren over it.
    FakeBackend backend, referenceBackend;
    CaptureCoordinator c (backend, 48000.0, 64), reference (referenceBackend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    reference.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (reference.startMonitoring (twoMics(), "out-device"));
    c.setHeadphoneJackGains ("out-device", { 1.0f, 1.0f, 0.0f, 0.0f });
    reference.setHeadphoneJackGains ("out-device", { 1.0f, 1.0f, 0.0f, 0.0f });

    std::vector<float> a (64, 0.15f), b (64, 0.15f);
    const float* ins[] = { a.data(), b.data() };
    std::vector<float> o0 (64), o1 (64), o2 (64), o3 (64);
    std::vector<float> r0 (64), r1 (64), r2 (64), r3 (64);
    float* outs[] = { o0.data(), o1.data(), o2.data(), o3.data() };
    float* refOuts[] = { r0.data(), r1.data(), r2.data(), r3.data() };

    c.processAudioBlock (ins, 2, outs, 4, 64);
    reference.processAudioBlock (ins, 2, refOuts, 4, 64);
    REQUIRE (loudestOf (o0) > 0.01f);
    REQUIRE (loudestOf (o2) == 0.0f); // no alarm yet: silence

    c.getAlarm().setFault (true);
    float switchedOffPeak = 0.0f;

    for (int block = 0; block < 8; ++block)
    {
        const auto renderedBefore = c.getAlarm().getSamplesRendered();

        c.processAudioBlock (ins, 2, outs, 4, 64);
        reference.processAudioBlock (ins, 2, refOuts, 4, 64);

        // Rendered once for the block, not once per kind of channel: a second
        // render would move the siren on and play a different stretch of it.
        REQUIRE (c.getAlarm().getSamplesRendered() - renderedBefore == 64u);

        switchedOffPeak = std::max (switchedOffPeak, loudestOf (o2));

        for (size_t s = 0; s < 64; ++s)
        {
            REQUIRE (o2[s] == o3[s]);
            REQUIRE_NEAR (o0[s], o1[s], 1e-9);

            // The switched-off jack carries the alarm and nothing else: what
            // the hearing jack gets on top of the alarm-free mix is exactly
            // what the switched-off one gets.
            REQUIRE_NEAR (o0[s], std::clamp (r0[s] + o2[s], -1.0f, 1.0f), 1e-6);
            REQUIRE (r2[s] == 0.0f);
        }
    }

    REQUIRE (switchedOffPeak > 0.2f);
    c.getAlarm().setFault (false);
}

TEST_CASE (CaptureCoordinator_EveryJackSwitchedOffStillSoundsTheAlarm)
{
    // Every microphone's jack switched off: the output stream is still open,
    // so the app keeps its sounds on it rather than the computer's speaker --
    // and they reached no channel at all. The start chirp and the fault siren
    // must still come out of every jack, and the mix out of none.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.setHeadphoneJackGains ("out-device", { 0.0f, 0.0f, 0.0f, 0.0f });

    std::vector<float> a (64, 0.3f), b (64, 0.3f);
    const float* ins[] = { a.data(), b.data() };
    std::vector<float> o0 (64), o1 (64), o2 (64), o3 (64);
    float* outs[] = { o0.data(), o1.data(), o2.data(), o3.data() };

    // The loudest each jack got over a run of blocks.
    const auto peaksOver = [&] (int blocks)
    {
        std::vector<float> peaks (4, 0.0f);
        for (int block = 0; block < blocks; ++block)
        {
            c.processAudioBlock (ins, 2, outs, 4, 64);
            peaks[0] = std::max (peaks[0], loudestOf (o0));
            peaks[1] = std::max (peaks[1], loudestOf (o1));
            peaks[2] = std::max (peaks[2], loudestOf (o2));
            peaks[3] = std::max (peaks[3], loudestOf (o3));
        }
        return peaks;
    };

    for (float peak : peaksOver (4))
        REQUIRE (peak == 0.0f); // the mix reaches nobody

    c.getAlarm().trigger (AlarmTone::Kind::Started);
    for (float peak : peaksOver (8))
        REQUIRE (peak > 0.2f);

    c.getAlarm().setFault (true);
    for (float peak : peaksOver (8))
    {
        REQUIRE (peak > 0.2f);
        REQUIRE (peak <= AlarmTone::kSirenLevel + 1e-6f); // the siren alone, no mix under it
    }

    c.getAlarm().setFault (false);
    for (float peak : peaksOver (2))
        REQUIRE (peak == 0.0f);
}

TEST_CASE (CaptureCoordinator_JackGainsReachOnlyTheStreamOpenOnTheirDevice)
{
    // The per-jack gains describe the combined device's channels, and which
    // device the output stream is open on is the coordinator's to know, not
    // the caller's: mid-take the output chosen can move while the stream
    // stays where it is until Stop, and a refused output leaves none open.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    std::vector<float> a (64, 0.15f), b (64, 0.15f);
    const float* ins[] = { a.data(), b.data() };
    std::vector<float> o0 (64), o1 (64), o2 (64), o3 (64);
    float* outs[] = { o0.data(), o1.data(), o2.data(), o3.data() };

    // Which of four output channels get the mix in one block.
    using Hearing = std::vector<bool>;
    const auto hearing = [&]
    {
        c.processAudioBlock (ins, 2, outs, 4, 64);
        return Hearing { loudestOf (o0) > 0.01f, loudestOf (o1) > 0.01f,
                         loudestOf (o2) > 0.01f, loudestOf (o3) > 0.01f };
    };

    // Laid before anything is open, as a layout read can be: the stream then
    // opened on the combined device has them from its first block.
    c.setHeadphoneJackGains ("combined", { 1.0f, 1.0f, 0.0f, 0.0f });
    REQUIRE (c.startMonitoring (twoMics(), "combined"));
    REQUIRE (c.isOutputOpenOn ("combined"));
    REQUIRE (hearing() == Hearing ({ true, true, false, false }));

    // Mid-take a microphone drops out of the combined device and the jacks
    // move: laid again at once on the stream still running, whatever output
    // has been chosen meanwhile.
    c.setHeadphoneJackGains ("combined", { 0.0f, 0.0f, 1.0f, 1.0f });
    REQUIRE (hearing() == Hearing ({ false, false, true, true }));

    // Reopened on another output (the choice moved, and Stop paid the
    // reopen): its channels 2-3 are nobody's jack, and get the mix without a
    // word from the caller.
    REQUIRE (c.startMonitoring (twoMics(), "scarlett"));
    REQUIRE_FALSE (c.isOutputOpenOn ("combined"));
    REQUIRE (c.isOutputOpenOn ("scarlett"));
    REQUIRE (hearing() == Hearing ({ true, true, true, true }));

    // The combined device's layout read again while the interface plays: not
    // laid over the interface's channels.
    c.setHeadphoneJackGains ("combined", { 1.0f, 1.0f, 0.0f, 0.0f });
    REQUIRE (hearing() == Hearing ({ true, true, true, true }));

    // Back on the combined device: the gains as last laid, from its first
    // block.
    REQUIRE (c.startMonitoring (twoMics(), "combined"));
    REQUIRE (hearing() == Hearing ({ true, true, false, false }));

    // Stopped, or the output refused and the rig input-only: nothing is open
    // on any device.
    c.stopMonitoring();
    REQUIRE_FALSE (c.isOutputOpenOn ("combined"));
    backend.failOutputOpen = true;
    REQUIRE (c.startMonitoring (twoMics(), "combined"));
    REQUIRE_FALSE (c.isOutputOpenOn ("combined"));
    REQUIRE_FALSE (c.isOutputOpenOn (""));
}

// A mic still handing over audio is plainly plugged in. One device-list pass
// that came back short used to latch it as unplugged for the rest of the take.
TEST_CASE (CaptureCoordinator_KnowsWhichMicsAreStillDeliveringAudio)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (backend.inputCallbacks.size() == 2u);

    std::vector<float> block (64, 0.1f);
    const float* ins[] = { block.data() };

    REQUIRE_FALSE (c.isDeviceDelivering (twoMics()[0].deviceId));

    backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
    REQUIRE (c.isDeviceDelivering (twoMics()[0].deviceId));
    REQUIRE_FALSE (c.isDeviceDelivering (twoMics()[1].deviceId));

    // Gone quiet: no longer delivering once the window has passed.
    std::this_thread::sleep_for (std::chrono::milliseconds (60));
    REQUIRE_FALSE (c.isDeviceDelivering (twoMics()[0].deviceId, std::chrono::milliseconds (20)));
}

TEST_CASE (CaptureCoordinator_MetersRunWithoutRecording)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.5f), b (64, 0.0f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    for (int i = 0; i < 40; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    // §8.1: meters run from launch, not from record.
    REQUIRE_FALSE (c.isRecording());
    auto* m = c.getChannelMetering (0);
    REQUIRE (m != nullptr);

    // §8.2 splits this on purpose: the audio thread only stores block stats into
    // atomics, and the UI thread advances the ballistics at 60 Hz. The
    // coordinator must not call tick() itself -- that would be the audio thread
    // doing UI work.
    for (int i = 0; i < 10; ++i)
        m->tick (1.0 / 60.0);

    REQUIRE (m->getDisplayedLevelDb() > Metering::kMinDb);
}

TEST_CASE (CaptureCoordinator_RecordsAudioThroughToTheFiles)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (c.isRecording());

    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    for (int i = 0; i < 16; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();

    // 16 blocks x 64 frames = 1024 frames, 2 bytes each at 16-bit mono.
    std::ifstream f (dir + "/01_Kitchen.wav", std::ios::binary);
    REQUIRE (f.is_open());
    REQUIRE (readU32LE (f, kDataSizeOffset) == 1024 * 2);

    // Real signal, not zeros: this is the end-to-end proof that audio reaches
    // the file rather than the pipeline merely being constructed.
    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    const int16_t sample = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
    REQUIRE (sample > 12000);
}

// A slow card still working through its backlog is not a dead one. Stop used
// to give the whole drain five seconds; a card that needed longer was called
// unresponsive, the final session.json was skipped, and a combined video was
// cut from a MIX.wav whose header had not been finished.
TEST_CASE (CaptureCoordinator_ASlowCardThatIsStillWritingIsWaitedFor)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    c.setFilesystemDeadline (std::chrono::milliseconds (400));
    c.setWriterChunkHookForTesting ([] { std::this_thread::sleep_for (std::chrono::milliseconds (40)); });

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-10-01T00:00:00Z"));

    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    constexpr int kBlocks = 1500; // two seconds of audio: about 24 chunks of 4096
    for (int i = 0; i < kBlocks; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    const auto before = std::chrono::steady_clock::now();
    c.stopRecording();
    const auto waited = std::chrono::steady_clock::now() - before;

    REQUIRE_FALSE (c.didLastStopTimeOut());
    REQUIRE_FALSE (c.hasCardWriteFailed());

    std::ifstream f (dir + "/01_Kitchen.wav", std::ios::binary);
    REQUIRE (f.is_open());
    REQUIRE (readU32LE (f, kDataSizeOffset) == static_cast<uint32_t> (kBlocks * 64 * 2));

    // And it really did take longer than the deadline, so the old rule would
    // have given up on it.
    if (waited < std::chrono::milliseconds (400))
        std::printf ("  note: the drain finished inside the deadline (%lld ms)\n",
                     (long long) std::chrono::duration_cast<std::chrono::milliseconds> (waited).count());
    c.setWriterChunkHookForTesting ({});
}

TEST_CASE (CaptureCoordinator_ACardThatStopsAnsweringAtStartDoesNotHoldTheCaller)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    c.setFilesystemDeadline (std::chrono::milliseconds (50));
    c.setFilesystemStallForTesting ([] { std::this_thread::sleep_for (std::chrono::milliseconds (400)); });

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    const auto before = std::chrono::steady_clock::now();
    REQUIRE_FALSE (c.startRecording (tempDir(), 16, "2026-09-24T00:00:00Z"));
    const auto waited = std::chrono::steady_clock::now() - before;

    REQUIRE (waited < std::chrono::milliseconds (300));
    REQUIRE_FALSE (c.isRecording());
    REQUIRE (c.getRecordingProblem().find ("stopped answering") != std::string::npos);

    // The abandoned worker finishes on its own and releases what it opened;
    // a later take on a card that answers starts normally.
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    c.setFilesystemStallForTesting ({});
    c.setFilesystemDeadline (std::chrono::milliseconds (5000));
    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-24T00:00:01Z"));
    c.stopRecording();
    REQUIRE_FALSE (c.didLastStopTimeOut());
}

TEST_CASE (CaptureCoordinator_ACardThatStopsAnsweringAtStopIsReportedNotWaitedOn)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-24T00:00:00Z"));

    c.setFilesystemDeadline (std::chrono::milliseconds (50));
    c.setFilesystemStallForTesting ([] { std::this_thread::sleep_for (std::chrono::milliseconds (400)); });

    const auto before = std::chrono::steady_clock::now();
    c.stopRecording();
    const auto waited = std::chrono::steady_clock::now() - before;

    REQUIRE (waited < std::chrono::milliseconds (300));
    REQUIRE (c.didLastStopTimeOut());
    REQUIRE_FALSE (c.isRecording());
    REQUIRE (c.hasCardWriteFailed());
    REQUIRE (c.getCardWriteProblem().find ("stopped answering") != std::string::npos);

    // Monitoring is untouched (§5.1), and the abandoned writer is left to
    // finish on its own worker.
    REQUIRE (c.isMonitoring());
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
}

TEST_CASE (CaptureCoordinator_MixFileIsWrittenAlongsideTheStems)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.25f), b (64, 0.25f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };
    c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();

    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (mix.is_open());
    REQUIRE (readU32LE (mix, kDataSizeOffset) == 64 * 2);
}

TEST_CASE (CaptureCoordinator_MonitoringSurvivesStoppingTheRecording)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (tempDir(), 16, "2026-08-27T00:00:00Z"));
    c.stopRecording();

    // §5.1: monitoring is independent of record state.
    REQUIRE (c.isMonitoring());
    REQUIRE_FALSE (c.isRecording());

    std::vector<float> a (64, 0.3f), b (64, 0.3f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };
    c.processAudioBlock (ins, 2, outs, 1, 64);

    REQUIRE (outL[0] > 0.0f);
}

TEST_CASE (CaptureCoordinator_UnpluggedMicWritesSilenceIntoItsChannel)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));

    // §6.5: the channel stays, it just goes quiet.
    c.setChannelLive ("dev-a", false);

    std::vector<float> a (64, 0.9f), b (64, 0.9f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };
    c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();

    std::ifstream f (dir + "/01_Kitchen.wav", std::ios::binary);
    REQUIRE (f.is_open());
    // Frames present, and zero.
    REQUIRE (readU32LE (f, kDataSizeOffset) == 64 * 2);

    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    REQUIRE (lo == 0);
    REQUIRE (hi == 0);
}

TEST_CASE (CaptureCoordinator_NoOutputBuffersIsNotAFailure)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    const float* ins[] = { a.data(), b.data() };

    // An input-only callback is normal on backends that split the directions.
    c.processAudioBlock (ins, 2, nullptr, 0, 64);
    REQUIRE (c.isMonitoring());
}

TEST_CASE (CaptureCoordinator_StoppingMonitoringClosesTheStreams)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.stopMonitoring();

    REQUIRE_FALSE (c.isMonitoring());
    REQUIRE (backend.closeAllCalls >= 1);
}

TEST_CASE (CaptureCoordinator_RecordingRefusesWithNoChannels)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring ({}, "out-device"));
    REQUIRE_FALSE (c.startRecording (tempDir(), 16, "2026-08-27T00:00:00Z"));
}

TEST_CASE (CaptureCoordinator_MasterVolumeChangesTheMonitorButNotTheStems)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    const float* ins[] = { a.data(), b.data() };

    std::vector<float> loudOut (64, 0.0f), quietOut (64, 0.0f);

    c.getMonitorBus().setMasterVolume (100.0);
    float* loud[] = { loudOut.data() };
    c.processAudioBlock (ins, 2, loud, 1, 64);

    c.getMonitorBus().setMasterVolume (20.0);
    float* quiet[] = { quietOut.data() };
    c.processAudioBlock (ins, 2, quiet, 1, 64);

    // §5.1: master volume is a listening level. It must move the headphones...
    REQUIRE (quietOut[0] < loudOut[0]);
    REQUIRE (quietOut[0] > 0.0f);
}

TEST_CASE (CaptureCoordinator_TrimAffectsTheMonitorMix)
{
    FakeBackend backend;

    auto quiet = twoMics();
    quiet[1].trimDb = -20.0f; // §4 trim range floor

    CaptureCoordinator c (backend, 48000.0, 64);

    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (quiet, "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    std::vector<float> out (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { out.data() };

    c.processAudioBlock (ins, 2, outs, 1, 64);

    // §4: trim is a mix-side decision, so pulling one mic down 20 dB must land
    // the sum well below the 0.20 two-mics-at-unity would give.
    REQUIRE (out[0] < 0.20f);
    REQUIRE (out[0] > 0.10f);
}

TEST_CASE (CaptureCoordinator_TrimCanBeChangedWhileRunning)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    const float* ins[] = { a.data(), b.data() };

    std::vector<float> before (64, 0.0f), after (64, 0.0f);
    float* outBefore[] = { before.data() };
    float* outAfter[] = { after.data() };

    c.processAudioBlock (ins, 2, outBefore, 1, 64);

    // §4: the user turns one mic down mid-session and the monitor follows
    // immediately -- no restart, no gap.
    c.setChannelTrimDb (1, -20.0f);
    c.processAudioBlock (ins, 2, outAfter, 1, 64);

    REQUIRE (after[0] < before[0]);
    REQUIRE_NEAR (c.getChannelTrimDb (1), -20.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelTrimDb (0), 0.0f, 1e-6);
}

TEST_CASE (CaptureCoordinator_OutOfRangeTrimIndexIsIgnored)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // Must not write past the end: the UI can outlive a channel that just
    // disappeared, and a stray index here would corrupt the audio thread's
    // gain table.
    c.setChannelTrimDb (-1, -6.0f);
    c.setChannelTrimDb (99, -6.0f);

    REQUIRE_NEAR (c.getChannelTrimDb (0), 0.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelTrimDb (1), 0.0f, 1e-6);
}

TEST_CASE (CaptureCoordinator_ReportsAudioCallbackLoad)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    std::vector<float> out (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { out.data() };

    for (int i = 0; i < 50; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    // §6.6 needs a real load figure to warn from. A 64-sample block at 48k has
    // 1.33ms to work with and this does almost nothing, so the load must be a
    // sane fraction rather than zero or nonsense.
    const auto load = c.getAudioCallbackLoad();
    REQUIRE (load > 0.0);
    REQUIRE (load < 1.0);
}

TEST_CASE (CaptureCoordinator_EachDeviceLandsInItsOwnChannel)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    REQUIRE (backend.inputCallbacks.size() == 2);

    // Each USB device delivers only its own audio on its own callback. Passing
    // one shared callback to every device -- which is what this replaces --
    // wrote every microphone into channel 0 and recorded one mic N times.
    // Enough to clear pre-roll (§5.4: kPreRollBlocks of the 64-sample buffer).
    std::vector<float> loud (256, 0.8f), quiet (256, 0.1f);
    const float* loudIn[] = { loud.data() };
    const float* quietIn[] = { quiet.data() };

    backend.inputCallbacks[0] (loudIn, 1, nullptr, 0, 256);
    backend.inputCallbacks[1] (quietIn, 1, nullptr, 0, 256);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.pullOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
        c.getChannelMetering (1)->tick (1.0 / 60.0);
    }

    // Channel 0 got the loud mic and channel 1 the quiet one -- not the same
    // mic twice, which is what the shared callback produced.
    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb()
             > c.getChannelMetering (1)->getDisplayedLevelDb() + 6.0f);
}

TEST_CASE (CaptureCoordinator_OutputClockPullsEveryDeviceIntoTheMix)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (256, 0.10f), b (256, 0.20f);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };

    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 256);
    backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 256);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.pullOutputBlock (outs, 1, 64);

    // §5.1: the mix contains every mic, summed at unity, so it must exceed
    // either one alone.
    REQUIRE (out[0] > 0.20f);
}

TEST_CASE (CaptureCoordinator_FirstMicIsTheClockMasterByDefault)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // §3.1: a rig with no master would resample every device against nothing.
    REQUIRE (c.getMasterChannel() == 0);

    c.setMasterChannel (1);
    REQUIRE (c.getMasterChannel() == 1);

    // Out of range clears it rather than silently keeping a stale index -- the
    // master can leave the rig (§3.3).
    c.setMasterChannel (99);
    REQUIRE (c.getMasterChannel() == -1);
}

TEST_CASE (CaptureCoordinator_MasterReportsNoDriftAgainstItself)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.setMasterChannel (0);

    std::vector<float> a (2048, 0.1f);
    const float* aIn[] = { a.data() };
    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 2048);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    for (int i = 0; i < 100; ++i)
        c.pullOutputBlock (outs, 1, 64);

    // §3.1: the timebase is never corrected against itself, however its ring
    // happens to sit.
    REQUIRE_NEAR (c.getChannelDriftPpm (0), 0.0, 1e-12);
}

TEST_CASE (CaptureCoordinator_UnpluggedDeviceStillYieldsItsChannel)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (512, 0.5f), b (512, 0.5f);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 512);
    backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 512);

    // §6.5: the mic goes away mid-session; its channel does not.
    c.setChannelLive ("dev-b", false);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.pullOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
        c.getChannelMetering (1)->tick (1.0 / 60.0);

    // Channel 1 is present and silent, not gone and not replaying stale audio.
    REQUIRE (c.getChannelMetering (1) != nullptr);
    REQUIRE_NEAR (c.getChannelMetering (1)->getDisplayedLevelDb(), Metering::kMinDb, 1.0f);
}

namespace {

/// Three mics on three crystals, driven for a simulated take. Optionally the
/// clock master is unplugged a sixth of the way in (§6.5), and optionally the
/// take fails over onto another channel.
struct TakeResult
{
    double driftPpm[3] = { 0.0, 0.0, 0.0 };
    uint64_t underruns[3] = { 0, 0, 0 };
    double rawDriftPpm[3] = { 0.0, 0.0, 0.0 };
};

TakeResult runTake (bool unplugMaster, int failoverTo, double seconds, double unplugAtSeconds = 5.0)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    const std::vector<CaptureChannel> mics = {
        { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
        { "dev-b", "Couch",   "02_Couch",   0.0f },
        { "dev-c", "Desk",    "03_Desk",    0.0f },
    };

    REQUIRE (c.startMonitoring (mics, "out-device"));
    c.setMasterChannel (0);

    // Three dissimilar USB crystals, which is the case §3 exists for.
    const double ppm[3] = { 0.0, +40.0, -60.0 };
    double owed[3] = { 0.0, 0.0, 0.0 };

    std::vector<float> in (128, 0.25f);
    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    const long long blocks = static_cast<long long> (seconds * 48000.0 / 64.0);
    const long long unplugAt = static_cast<long long> (unplugAtSeconds * 48000.0 / 64.0);
    bool unplugged = false;

    for (long long i = 0; i < blocks; ++i)
    {
        if (unplugMaster && ! unplugged && i == unplugAt)
        {
            // §6.5: the channel stays and goes silent. The channel list is
            // fixed for the take, so only liveness moves.
            c.setChannelLive ("dev-a", false);

            if (failoverTo >= 0)
                c.setMasterChannel (failoverTo);

            unplugged = true;
        }

        for (int d = 0; d < 3; ++d)
        {
            if (unplugged && d == 0)
                continue; // the mic is gone; nothing more arrives from it

            owed[d] += 64.0 * (1.0 + ppm[d] * 1.0e-6);
            const int n = static_cast<int> (owed[d]);
            owed[d] -= n;

            const float* block[] = { in.data() };
            backend.inputCallbacks[static_cast<size_t> (d)] (block, 1, nullptr, 0, n);
        }

        c.pullOutputBlock (outs, 1, 64);
    }

    TakeResult r;
    for (int d = 0; d < 3; ++d)
    {
        r.driftPpm[d] = c.getChannelDriftPpm (d);
        // Each channel's own loop as well, for the test that asks whether one
        // channel's arithmetic ever enters another's: the relative figure
        // subtracts the master's state by design, so it cannot answer that.
        r.rawDriftPpm[d] = c.getChannelRawDriftPpm (d);
        r.underruns[d] = c.getUnderrunSamples (d);
    }
    return r;
}

} // namespace

TEST_CASE (CaptureCoordinator_UnpluggedMasterLeavesTheOtherChannelsUntouched)
{
    // The question §6.5's "clock master unplugged" row raises: with the master
    // gone silent and still flagged as master, is everything else now being
    // resampled onto silence?
    //
    // It is not, and this pins down why. The master is not a signal any other
    // channel reads -- DeviceInputStream drives each channel's ratio from that
    // stream's own ring fill against the output clock, with no channel's audio
    // entering another's arithmetic. So a master that stops delivering removes
    // nothing from anyone else's loop.
    //
    // Run to a minute so both live loops are well past settling.
    const auto control = runTake (false, -1, 60.0);
    const auto masterGone = runTake (true, -1, 60.0);

    // Bit-identical, not merely close: the two runs execute the same arithmetic
    // on the live channels. Anything else would mean a coupling that is not
    // supposed to exist.
    for (int d = 1; d < 3; ++d)
    {
        REQUIRE_NEAR (masterGone.rawDriftPpm[d], control.rawDriftPpm[d], 1e-12);
        REQUIRE (masterGone.underruns[d] == control.underruns[d]);
    }

    // And the loops were genuinely working, so the equality above means
    // something.
    REQUIRE (control.driftPpm[1] > 20.0);
    REQUIRE (control.driftPpm[2] < -20.0);
    REQUIRE (masterGone.underruns[1] == 0);
    REQUIRE (masterGone.underruns[2] == 0);
}

TEST_CASE (CaptureCoordinator_FailingOverMidTakeCostsTheLiveChannelsNothing)
{
    // §6.5's "clock master unplugged -> failover per §3.3", and the regression
    // guard for what used to make it unsafe.
    //
    // Under the old exemption, promoting a live channel took one the PI loop was
    // holding at its target fill and stopped steering it, leaving its ring to
    // run to one end for the rest of the take. Failing over cost more than not
    // failing over, which is why the mid-take path did not do it.
    //
    // Every channel is corrected onto the output clock now, so the title carries
    // no correction with it and the promotion is free.
    const auto noFailover = runTake (true, -1, 60.0, 4.0);
    const auto failedOver = runTake (true, 1, 60.0, 4.0);

    // Channel 1 takes over the reference and keeps being steered: its loop
    // reaches the correction its +40 PPM crystal needs either way. (Its own
    // reported figure is relative to itself once it is the master, hence
    // reading it off the control run.)
    REQUIRE (noFailover.driftPpm[1] > 35.0);
    REQUIRE_NEAR (failedOver.driftPpm[1], 0.0, 1e-12);

    // And nothing was lost anywhere: no channel underran in either run.
    for (int d = 1; d < 3; ++d)
    {
        REQUIRE (noFailover.underruns[d] == 0);
        REQUIRE (failedOver.underruns[d] == 0);
    }
}

TEST_CASE (CaptureCoordinator_DriftIsQuotedRelativeToTheClockMaster)
{
    // §3.3: "positive means this device runs fast relative to the master."
    //
    // Each stream's own loop measures itself against the output clock, whose
    // skew is common to every channel. Moving the reference therefore re-bases
    // every figure by the same amount, and the master always reads zero against
    // itself -- which is what makes these numbers mean what §3.3 says they mean
    // rather than "how far this mic is from the headphones".
    const auto control = runTake (false, -1, 60.0);

    REQUIRE_NEAR (control.driftPpm[0], 0.0, 1e-12);

    // Channels at +40 and -60 PPM against a master at 0.
    REQUIRE (control.driftPpm[1] > 35.0);
    REQUIRE (control.driftPpm[2] < -55.0);
}

namespace {

/// Drives one device's input callback with a stereo block, the way a USB
/// microphone that presents two channels does.
void pushStereo (FakeBackend& backend, int device,
                 const std::vector<float>& left, const std::vector<float>& right)
{
    const float* channels[] = { left.data(), right.data() };
    backend.inputCallbacks[static_cast<size_t> (device)] (channels, 2, nullptr, 0,
                                                          static_cast<int> (left.size()));
}

} // namespace

TEST_CASE (CaptureCoordinator_DoesNotSubstituteAnUnselectedPhysicalInput)
{
    // The take selected physical input 0 only. Input 1 can carry unrelated
    // audio from a disabled socket, so its being louder must not make it part
    // of the recording. The old one-route stereo special case inspected both
    // pointers and substituted input 1 when input 0 was silent.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    CaptureChannel selected { "device", "Selected input", "01_Selected", 0.0f };
    selected.deviceChannel = 0;
    REQUIRE (c.startMonitoring ({ selected }, "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    const std::vector<float> silent (512, 0.0f);
    const std::vector<float> signal (512, 0.4f);

    // Only the right/unselected socket carries signal.
    pushStereo (backend, 0, silent, signal);
    REQUIRE (c.getChannelLayoutSource (0) == -1);
    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Pending);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.pullOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
    }

    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb() == Metering::kMinDb);
    for (const auto sample : out)
        REQUIRE (sample == 0.0f);
}

TEST_CASE (CaptureCoordinator_FreshStereoPairIsAnalyzedWithoutCollapsingEitherInput)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 512);
    c.setSoftwareClockEnabled (false);

    CaptureChannel left { "device", "Fresh USB 1", "01_Fresh-USB-1", 0.0f };
    left.deviceChannel = 0;
    left.analyzeStereoPair = true;
    CaptureChannel right { "device", "Fresh USB 2", "02_Fresh-USB-2", 0.0f };
    right.deviceChannel = 1;

    REQUIRE (c.startMonitoring ({ left, right }, "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    const std::vector<float> silent (512, 0.0f);
    const std::vector<float> signal (512, 0.4f);
    std::vector<float> out (512, 0.0f);
    float* outs[] = { out.data() };

    // Three seconds with one live side is §2.1's Mono verdict. Pull every
    // block so the per-channel rings and meters show what was actually routed.
    for (int block = 0; block < 282; ++block)
    {
        pushStereo (backend, 0, silent, signal);
        c.pullOutputBlock (outs, 1, 512);
    }

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Mono);
    REQUIRE (c.getChannelLayoutDecision (1) == ChannelLayoutDecision::Pending);

    // The verdict is evidence for the NEXT rebuilt plan. Until then this live
    // capture still owns both exact physical channels: the silent left remains
    // silent and the right remains audible rather than being folded together.
    for (int i = 0; i < 10; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
        c.getChannelMetering (1)->tick (1.0 / 60.0);
    }

    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb() == Metering::kMinDb);
    REQUIRE (c.getChannelMetering (1)->getDisplayedLevelDb() > Metering::kMinDb + 20.0f);
    REQUIRE (c.getChannels().size() == 2);
    REQUIRE_FALSE (c.getChannels()[0].collapseStereoPair);
    REQUIRE_FALSE (c.getChannels()[1].collapseStereoPair);
}

TEST_CASE (CaptureCoordinator_SilentTimeoutCannotPermanentlyCollapseATrueStereoPair)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 100.0, 10);
    c.setSoftwareClockEnabled (false);

    CaptureChannel left { "device", "Fresh USB 1", "01_Fresh-USB-1", 0.0f };
    left.deviceChannel = 0;
    left.analyzeStereoPair = true;
    CaptureChannel right { "device", "Fresh USB 2", "02_Fresh-USB-2", 0.0f };
    right.deviceChannel = 1;
    REQUIRE (c.startMonitoring ({ left, right }, "out-device"));

    const std::vector<float> silence (6000, 0.0f);
    pushStereo (backend, 0, silence, silence);

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Mono);
    REQUIRE_FALSE (c.isChannelLayoutDecisionPersistable (0));
    REQUIRE (c.getChannels().size() == 2);

    const std::vector<float> stereoLeft (10, 0.4f);
    const std::vector<float> stereoRight { 0.2f, -0.2f, 0.2f, -0.2f, 0.2f,
                                          -0.2f, 0.2f, -0.2f, 0.2f, -0.2f };
    for (int block = 0; block < 30; ++block)
        pushStereo (backend, 0, stereoLeft, stereoRight);

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Stereo);
    REQUIRE (c.isChannelLayoutDecisionPersistable (0));
    REQUIRE (c.getChannels().size() == 2);
    REQUIRE_FALSE (c.getChannels()[0].collapseStereoPair);
    REQUIRE_FALSE (c.getChannels()[1].collapseStereoPair);
}

TEST_CASE (CaptureCoordinator_OneDuplicatedFinalBlockCannotCollapseMostlyStereoAudio)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 1000.0, 10);
    c.setSoftwareClockEnabled (false);

    CaptureChannel left { "device", "Fresh USB 1", "01_Fresh-USB-1", 0.0f };
    left.deviceChannel = 0;
    left.analyzeStereoPair = true;
    CaptureChannel right { "device", "Fresh USB 2", "02_Fresh-USB-2", 0.0f };
    right.deviceChannel = 1;
    REQUIRE (c.startMonitoring ({ left, right }, "out-device"));

    const std::vector<float> stereoLeft (10, 0.4f);
    const std::vector<float> stereoRight { 0.4f, -0.4f, 0.4f, -0.4f, 0.4f,
                                          -0.4f, 0.4f, -0.4f, 0.4f, -0.4f };
    for (int block = 0; block < 299; ++block)
        pushStereo (backend, 0, stereoLeft, stereoRight);

    // A coincidentally duplicated last callback must contribute its fraction
    // of the evidence, not replace the preceding 2.99 seconds.
    pushStereo (backend, 0, stereoLeft, stereoLeft);

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Stereo);
    REQUIRE (c.isChannelLayoutDecisionPersistable (0));
    REQUIRE (c.getChannels().size() == 2);
}

namespace {

/// The largest |sample| in a 16-bit mono stem.
int loudestMagnitudeIn (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.is_open());
    const auto bytes = readU32LE (f, kDataSizeOffset);
    f.seekg (kAudioDataOffset);

    int loudest = 0;

    for (uint32_t i = 0; i < bytes / 2; ++i)
    {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        const auto v = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
        loudest = std::max (loudest, std::abs (static_cast<int> (v)));
    }

    return loudest;
}

/// A fresh two-input USB device, both inputs selected, its layout unknown:
/// what ChannelPlan hands over before §2.1 has heard it.
std::vector<CaptureChannel> freshStereoPair()
{
    CaptureChannel left { "device", "Fresh USB 1", "01_Fresh-USB-1", 0.0f };
    left.deviceChannel = 0;
    left.analyzeStereoPair = true;
    CaptureChannel right { "device", "Fresh USB 2", "02_Fresh-USB-2", 0.0f };
    right.deviceChannel = 1;
    return { left, right };
}

} // namespace

TEST_CASE (CaptureCoordinator_AFreshDuplicatedPairIsHeardOnceNotTwice)
{
    // Most USB microphones present two inputs carrying the same voice. Until
    // §2.1 has heard three seconds of it, both are kept as separate channels
    // -- and both were summed into the headphones at unity: the one voice
    // twice, +6 dB, into the safety limiter. Each side of an unsettled pair
    // is now heard at half, which is the voice once.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (freshStereoPair(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    const std::vector<float> voice (64, 0.2f);
    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    for (int block = 0; block < 100; ++block) // 0.13 s: long before any verdict
    {
        pushStereo (backend, 0, voice, voice);
        c.pullOutputBlock (outs, 1, 64);
    }

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Pending);
    REQUIRE_NEAR (c.getChannelMonitorWeight (0), 0.5f, 1e-6);
    REQUIRE_NEAR (c.getChannelMonitorWeight (1), 0.5f, 1e-6);
    REQUIRE_NEAR (out.back(), 0.2f, 1e-5);

    // Past three seconds §2.1 calls it Mono (the sides are identical) and the
    // headphones take the chosen side alone, straight away -- the capture
    // still has both channels until the idle rebuild.
    for (int block = 0; block < 2300; ++block)
    {
        pushStereo (backend, 0, voice, voice);
        c.pullOutputBlock (outs, 1, 64);
    }

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Mono);
    REQUIRE (c.getChannels().size() == 2);
    REQUIRE_NEAR (c.getChannelMonitorWeight (0), 1.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelMonitorWeight (1), 0.0f, 1e-6);
    REQUIRE_NEAR (out.back(), 0.2f, 1e-5);
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_AVerdictReachedMidTakeReachesTheHeadphonesButNotTheStems)
{
    // Record pressed before the verdict: the layout is only rebuilt while
    // idle, so the doubled voice used to last the whole take. The verdict now
    // reaches the headphones the moment it is reached, and the stems -- both
    // of them, each at unity -- are exactly what they were.
    const auto dir = tempDir() + "/fresh_pair_mid_take";
    std::filesystem::create_directories (dir);
    std::remove ((dir + "/01_Fresh-USB-1.wav").c_str());
    std::remove ((dir + "/02_Fresh-USB-2.wav").c_str());

    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (freshStereoPair(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);
    REQUIRE (c.startRecording (dir, 16, "2026-10-13T00:00:00Z"));

    // The capsule is wired to the right input; the left is silent.
    const std::vector<float> silent (64, 0.0f);
    const std::vector<float> voice (64, 0.4f);
    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    for (int block = 0; block < 100; ++block)
    {
        pushStereo (backend, 0, silent, voice);
        c.pullOutputBlock (outs, 1, 64);
    }

    REQUIRE_NEAR (out.back(), 0.2f, 1e-5); // half, while it is being worked out

    for (int block = 0; block < 2300; ++block)
    {
        pushStereo (backend, 0, silent, voice);
        c.pullOutputBlock (outs, 1, 64);
    }

    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Mono);
    REQUIRE (c.getChannelLayoutSource (0) == 1);
    REQUIRE_NEAR (c.getChannelMonitorWeight (0), 0.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelMonitorWeight (1), 1.0f, 1e-6);
    REQUIRE_NEAR (out.back(), 0.4f, 1e-5); // the live side, alone, at unity

    c.stopRecording();
    c.stopMonitoring();

    REQUIRE (loudestMagnitudeIn (dir + "/01_Fresh-USB-1.wav") == 0);
    REQUIRE (std::abs (loudestMagnitudeIn (dir + "/02_Fresh-USB-2.wav") - static_cast<int> (std::lround (0.4 * 32767.0))) <= 1);

    std::remove ((dir + "/01_Fresh-USB-1.wav").c_str());
    std::remove ((dir + "/02_Fresh-USB-2.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (CaptureCoordinator_AStereoVerdictBringsBothSidesBackToUnity)
{
    // A real two-input interface, two different people: half each for the
    // three seconds it takes to tell, then both at unity -- moved over 10 ms,
    // not in one 6 dB step.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (freshStereoPair(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> left (64), right (64), out (64, 0.0f);
    float* outs[] = { out.data() };
    float biggestStep = 0.0f, previous = 0.0f;
    bool sawVerdict = false;

    for (int block = 0; block < 2400; ++block)
    {
        for (int i = 0; i < 64; ++i)
        {
            const double t = static_cast<double> (block * 64 + i) / 48000.0;
            left[static_cast<size_t> (i)] = static_cast<float> (0.2 * std::sin (6.283185307179586 * 3.0 * t));
            right[static_cast<size_t> (i)] = 0.1f;
        }

        pushStereo (backend, 0, left, right);
        c.pullOutputBlock (outs, 1, 64);

        if (! sawVerdict && c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Stereo)
            sawVerdict = true;

        for (const auto sample : out)
        {
            if (block >= 200) // once the ring is playing out
                biggestStep = std::max (biggestStep, std::abs (sample - previous));
            previous = sample;
        }
    }

    REQUIRE (sawVerdict);
    REQUIRE_NEAR (c.getChannelMonitorWeight (0), 1.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelMonitorWeight (1), 1.0f, 1e-6);
    // A 3 Hz tone moves 0.0008 a sample at most; the 6 dB jump of the right
    // side's 0.1 would have been a 0.05 step in one sample.
    REQUIRE (biggestStep < 0.002f);
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_APairWithOneSideLeftOutIsNotTurnedDown)
{
    // Half each is only for a pair the analyzer is actually hearing. With one
    // of the two inputs not in the take there is nothing to analyse and no
    // verdict to wait for, so the input that is there plays at unity.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    auto pair = freshStereoPair();
    pair.pop_back();
    REQUIRE (c.startMonitoring (pair, "out-device"));
    REQUIRE_NEAR (c.getChannelMonitorWeight (0), 1.0f, 1e-6);
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_TheMixBarShowsTheBusAndItsClipIsTheLimiter)
{
    // The mix bar read the headphone signal after the volume, so at the
    // default volume a bus squared off at -3 dBFS showed as about -21 dBFS,
    // and its CLIP (three samples at -0.1 dBFS) could never be reached under
    // a -3 dBFS ceiling: the crackle had no visible cause. It now shows the
    // bus before the volume, and CLIP whenever the safety limiter has had to
    // turn the headphones down.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.3f), b (64, 0.3f), out (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { out.data() };

    // Default volume: a 0.6 bus (-4.4 dBFS) reaches the ears at -22.4 dBFS.
    c.processAudioBlock (ins, 2, outs, 1, 64);
    for (int i = 0; i < 30; ++i)
        c.getMixMetering().tick (1.0 / 60.0);

    REQUIRE_NEAR (c.getMixMetering().getDisplayedLevelDb(), 20.0f * std::log10 (0.6f), 0.1f);
    REQUIRE_FALSE (c.getMixMetering().isClipped());

    // Full volume: the same bus doubled reaches the ceiling, and the bar says so.
    c.getMonitorBus().setMasterVolume (100.0);
    std::fill (a.begin(), a.end(), 0.6f);
    std::fill (b.begin(), b.end(), 0.6f);
    c.processAudioBlock (ins, 2, outs, 1, 64);

    REQUIRE (c.getMixMetering().isClipped());
    REQUIRE (c.getMixMetering().getClipCount() >= 1);
    REQUIRE (std::abs (out[63]) <= std::pow (10.0f, MonitorBus::kLimiterCeilingDb / 20.0f) + 1e-6f);

    // Tapped, it clears, and a bus that is merely hot at a sane volume does
    // not bring it back: a bus over full scale at the default volume is
    // turned down by the volume, not clipped.
    c.getMixMetering().acknowledgeClip();
    for (int i = 0; i < 48000 / 64; ++i) // let the limiter come back to unity
        c.processAudioBlock (ins, 2, outs, 1, 64);
    c.getMixMetering().acknowledgeClip();
    c.getMonitorBus().setMasterVolume (MonitorBus::kDefaultMonitorVolume);
    c.processAudioBlock (ins, 2, outs, 1, 64);
    REQUIRE_FALSE (c.getMixMetering().isClipped());
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_ExplicitStereoMicVerdictStillUsesItsLiveSide)
{
    // A persisted analyzer verdict distinguishes this known stereo-presenting
    // microphone from an interface with an unselected adjacent socket. Only
    // that explicit bit authorizes looking at both physical inputs.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    CaptureChannel mic { "device", "Stereo USB mic", "01_Stereo-USB-mic", 0.0f };
    mic.deviceChannel = 0;
    mic.collapseStereoPair = true;
    REQUIRE (c.startMonitoring ({ mic }, "out-device"));

    const std::vector<float> silent (512, 0.0f);
    const std::vector<float> signal (512, 0.4f);
    pushStereo (backend, 0, silent, signal);

    REQUIRE (c.getChannelLayoutSource (0) == 1);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.pullOutputBlock (outs, 1, 64);

    bool heardSignal = false;
    for (const auto sample : out)
        heardSignal = heardSignal || sample != 0.0f;
    REQUIRE (heardSignal);
}

TEST_CASE (CaptureCoordinator_PersistedRightSideSurvivesAnImmediateRecording)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    CaptureChannel mic { "device", "Right-wired USB mic", "01_Right-wired", 0.0f };
    mic.collapseStereoPair = true;
    mic.monoSourceChannel = 1;
    REQUIRE (c.startMonitoring ({ mic }, "out-device"));

    // No monitoring callback has arrived yet. The remembered side must already
    // be live before the record button can freeze this take's selection.
    REQUIRE (c.getChannelLayoutSource (0) == 1);

    const std::vector<float> quiet (64, 0.0f);
    pushStereo (backend, 0, quiet, quiet);

    // Silence is not evidence for changing a remembered side. This was the
    // real failure: the ordinary idle callback reset it to left before the
    // user had a chance to press record.
    REQUIRE (c.getChannelLayoutSource (0) == 1);
    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-01T00:00:00Z"));

    // Fill the rest of the eight-block input ring without overrunning it; the
    // initial quiet block stays ahead of this signal exactly as a real idle
    // callback would.
    const std::vector<float> silent (448, 0.0f);
    const std::vector<float> signal (448, 0.4f);
    pushStereo (backend, 0, silent, signal);

    REQUIRE (c.getChannelLayoutSource (0) == 1);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    bool heardSignal = false;
    for (int block = 0; block < 20; ++block)
    {
        std::fill (out.begin(), out.end(), 0.0f);
        c.pullOutputBlock (outs, 1, 64);
        for (const auto sample : out)
            heardSignal = heardSignal || sample != 0.0f;
    }

    REQUIRE (heardSignal);
    c.stopRecording();

    std::ifstream stem (tempDir() + "/01_Right-wired.wav", std::ios::binary);
    REQUIRE (stem.is_open());
    const auto dataBytes = readU32LE (stem, kDataSizeOffset);
    REQUIRE (dataBytes > 0);

    stem.seekg (kAudioDataOffset);
    bool wroteSignal = false;
    for (uint32_t i = 0; i + 1 < dataBytes; i += 2)
    {
        unsigned char lo = 0, hi = 0;
        stem.read (reinterpret_cast<char*> (&lo), 1);
        stem.read (reinterpret_cast<char*> (&hi), 1);
        const int16_t sample = static_cast<int16_t> (
            static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
        wroteSignal = wroteSignal || sample > 1000;
    }
    REQUIRE (wroteSignal);
}

TEST_CASE (CaptureCoordinator_MonoDeviceIsUntouchedByChannelLayout)
{
    // A device presenting a single channel has nothing to decide, and must not
    // be routed through the two-channel path at all -- there is no second
    // pointer to read, and reading one would be a fault rather than a wrong
    // answer.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> mono (512, 0.3f);
    const float* one[] = { mono.data() };
    backend.inputCallbacks[0] (one, 1, nullptr, 0, 512);

    // Never analysed, so never claimed a side.
    REQUIRE (c.getChannelLayoutSource (0) == -1);
    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Pending);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.pullOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
        c.getChannelMetering (0)->tick (1.0 / 60.0);

    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb() > Metering::kMinDb + 20.0f);
}

TEST_CASE (CaptureCoordinator_ChannelSideNeverMovesOnceRecording)
{
    // §6.5 fixes the take's shape for its duration. Swapping which channel
    // feeds a stem mid-file would put a discontinuity in the middle of the
    // recording -- a worse failure than the one the side-picking fixes.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    const std::vector<float> silent (512, 0.0f);
    const std::vector<float> signal (512, 0.4f);

    // Opens in a quiet room: nothing heard from either side yet, so the side is
    // still the default and §2.1 has not decided. This is deliberately the
    // state in which the side is most free to move.
    const float* quietPair[] = { silent.data(), silent.data() };
    c.pushDeviceBlockMultiChannel (0, quietPair, 2, static_cast<int> (silent.size()));
    REQUIRE (c.getChannelLayoutSource (0) == 0);
    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Pending);

    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-01T00:00:00Z"));

    // Now the right starts carrying everything while the left stays silent --
    // the exact evidence that would move the side, arriving after the take has
    // begun. It must not move: the freeze has to be checked before the side is
    // recomputed, or the first block of the take still adopts the new one.
    const float* rightPair[] = { silent.data(), signal.data() };
    for (int i = 0; i < 400; ++i)
        c.pushDeviceBlockMultiChannel (0, rightPair, 2, static_cast<int> (silent.size()));

    REQUIRE (c.getChannelLayoutSource (0) == 0);

    c.stopRecording();

    // The take was fixed, not the rest of the monitoring session. Once the
    // writer is gone, the evidence gathered during it may select the live side
    // for the next take.
    c.pushDeviceBlockMultiChannel (0, rightPair, 2, static_cast<int> (silent.size()));
    REQUIRE (c.getChannelLayoutSource (0) == 1);
}

namespace {

std::vector<CaptureChannel> threeMics()
{
    return { { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
             { "dev-b", "Couch",   "02_Couch",   0.0f },
             { "dev-c", "Desk",    "03_Desk",    0.0f } };
}

/// Runs one already-aligned frame block through the aggregate path, which is
/// where §14.4's measurement sits.
void pushAligned (CaptureCoordinator& c, const std::vector<std::vector<float>>& channels)
{
    std::vector<const float*> pointers;
    for (const auto& ch : channels)
        pointers.push_back (ch.data());

    c.processAudioBlock (pointers.data(), static_cast<int> (pointers.size()),
                         nullptr, 0, static_cast<int> (channels[0].size()));
}

} // namespace

TEST_CASE (CaptureCoordinator_SpotsTwoMicrophonesHearingTheSameRoom)
{
    // §14.4, "the sleeper failure": two microphones in omni pick up the whole
    // room, so they hear the same thing, while a third microphone with nobody
    // in front of it hears nothing. That shape is what the detector looks for,
    // and it had never been fed anything.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));

    std::vector<float> shared (64), silent (64, 0.0f);
    for (size_t i = 0; i < shared.size(); ++i)
        shared[i] = 0.4f * std::sin (static_cast<float> (i) * 0.3f);

    // A and B carry the same room; C is quiet.
    pushAligned (c, { shared, shared, silent });

    REQUIRE (c.getPolarPairCorrelation() > PolarPatternDetector::kCorrelationThreshold);
    REQUIRE (c.getPolarThirdChannelPeakDb() < PolarPatternDetector::kThirdChannelSilenceDb);
}

TEST_CASE (CaptureCoordinator_DoesNotCallThreePeopleTalkingARoomProblem)
{
    // The case that must never fire. Three people each on their own cardioid
    // microphone: the channels are uncorrelated and nobody is silent. Calling
    // that a pattern problem would send someone to turn a knob that was already
    // right, which is worse than staying quiet.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));

    std::vector<float> a (64), b (64), third (64);
    for (size_t i = 0; i < a.size(); ++i)
    {
        a[i] = 0.4f * std::sin (static_cast<float> (i) * 0.30f);
        b[i] = 0.4f * std::sin (static_cast<float> (i) * 1.10f + 2.0f);
        third[i] = 0.4f * std::sin (static_cast<float> (i) * 0.70f + 4.0f);
    }

    pushAligned (c, { a, b, third });

    // Either test failing alone is enough to keep §14.4 quiet, and both do.
    REQUIRE (c.getPolarThirdChannelPeakDb() > PolarPatternDetector::kThirdChannelSilenceDb);
}

TEST_CASE (CaptureCoordinator_PolarPatternNeedsAThirdMicrophone)
{
    // §14.4's rule is stated over three channels: a correlated pair *and* a
    // third that hears nothing. With two microphones there is no uninvolved
    // one, and two people at one table correlate perfectly well.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> shared (64);
    for (size_t i = 0; i < shared.size(); ++i)
        shared[i] = 0.4f * std::sin (static_cast<float> (i) * 0.3f);

    pushAligned (c, { shared, shared });

    REQUIRE_NEAR (c.getPolarPairCorrelation(), 0.0f, 1e-9);
}

TEST_CASE (CaptureCoordinator_ANarrowerBlockDoesNotLeaveAnOldChannelInTheMix)
{
    // trimFrame is sized to the take's channel list, but a block can carry
    // fewer channels than that: the aggregate path takes
    // min(numInputs, channels.size()). MonitorBus::processSample sums the whole
    // vector, so any entry past the block's own width kept whatever an earlier,
    // wider block left in it -- a fixed sample added to every sample of the mix
    // from then on, from a microphone that is no longer arriving. In the
    // listener's headphones that is a DC offset that does not go away.
    FakeBackend backend;

    CaptureCoordinator c (backend, 48000.0, 64);

    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (64, 0.0f), b (64, 0.0f), loud (64, 0.4f);
    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    // Three channels, and the third one is the only one making any sound.
    const float* three[] = { a.data(), b.data(), loud.data() };
    c.processAudioBlock (three, 3, outs, 1, 64);
    REQUIRE (out[0] > 0.3f);

    // The third channel stops arriving. What is left is silent, so the mix must
    // be silent -- not still carrying the last sample the third one delivered.
    const float* two[] = { a.data(), b.data() };
    c.processAudioBlock (two, 2, outs, 1, 64);

    for (int i = 0; i < 64; ++i)
        REQUIRE (std::abs (out[i]) < 1.0e-6f);
}

TEST_CASE (CaptureCoordinator_PolarVerdictDoesNotOutliveItsMeasurement)
{
    // Every way out of measurePolarPattern clears the correlation except the
    // one that gives up on a null channel, which returned with the last verdict
    // still published. §14.4's advice reads that number, so it would keep
    // firing off a measurement that had stopped being made.
    FakeBackend backend;

    CaptureCoordinator c (backend, 48000.0, 64);

    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));

    // Two microphones hearing the same room, with a third hearing nothing:
    // §14.4's shape, and a correlation well above zero.
    std::vector<float> room (64, 0.0f), quiet (64, 0.0f);
    for (int i = 0; i < 64; ++i)
        room[static_cast<size_t> (i)] = std::sin (static_cast<float> (i) * 0.2f) * 0.5f;

    pushAligned (c, { room, room, quiet });
    REQUIRE (c.getPolarPairCorrelation() > 0.9f);

    // A channel stops being handed over at all.
    const float* withHole[] = { room.data(), nullptr, quiet.data() };
    c.processAudioBlock (withHole, 3, nullptr, 0, 64);

    REQUIRE (c.getPolarPairCorrelation() == 0.0f);
}

// ---------------------------------------------------------------------------
// A take that cannot start says why.
// ---------------------------------------------------------------------------

TEST_CASE (CaptureCoordinator_AFailedRecordStartCarriesTheReasonUp)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 256);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // A folder that is not there stands in for the card that was pulled,
    // filled, or locked between arming and pressing record.
    REQUIRE_FALSE (c.startRecording ("/mma-no-such-folder-4b21/take", 24, "2026-08-27T00:00:00Z"));

    const auto& problem = c.getRecordingProblem();
    REQUIRE_FALSE (problem.empty());
    REQUIRE (problem.find ("/mma-no-such-folder-4b21/take") != std::string::npos);

    // And the take genuinely did not start, so nothing claims one.
    REQUIRE_FALSE (c.isRecording());
}

TEST_CASE (CaptureCoordinator_RecordingWithNoMicrophonesSaysSoRatherThanNothing)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 256);

    REQUIRE_FALSE (c.startRecording ("/tmp", 24, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (c.getRecordingProblem().empty());
}

// ---------------------------------------------------------------------------
// The software clock: a take never depends on the headphone output.
// ---------------------------------------------------------------------------

namespace {
void pushInputsForAWhile (FakeBackend& backend, int blocks, int microsecondsPerBlock)
{
    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    const float* ins[] = { a.data() };
    const float* insB[] = { b.data() };

    for (int i = 0; i < blocks; ++i)
    {
        backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (insB, 1, nullptr, 0, 64);
        std::this_thread::sleep_for (std::chrono::microseconds (microsecondsPerBlock));
    }
}
} // namespace

TEST_CASE (CaptureCoordinator_RecordsWithoutAnOutputDevice)
{
    // No headphones selected at all. This used to open the microphones and
    // then nothing ever pulled them: flat meters, header-only files, and no
    // error -- the record button was not even disabled.
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), ""));
    REQUIRE (backend.outputStreamsOpened == 0);
    REQUIRE (! c.hasOutputStream());
    REQUIRE (c.startRecording (dir, 16, "2026-09-07T00:00:00Z"));

    pushInputsForAWhile (backend, 150, 1333); // ~200 ms of real time at 64/48k

    const auto accepted = c.getFramesAccepted();
    const auto peak = c.getPeakWritten();
    c.stopRecording();

    REQUIRE (accepted > 64 * 20);
    REQUIRE (peak > 0.4f);
    REQUIRE (! c.isOutputClockLost()); // nothing was lost: there never was one
}

TEST_CASE (CaptureCoordinator_KeepsRecordingWhenTheOutputClockStops)
{
    // The headphones are unplugged mid-take. The output callback stops; the
    // software clock notices within a few periods and pulls instead, and the
    // rig says so. This used to freeze the take with every counter at zero.
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.hasOutputStream());
    REQUIRE (c.startRecording (dir, 16, "2026-09-07T00:00:00Z"));

    std::vector<float> a (64, 0.5f), outL (64, 0.0f);
    const float* ins[] = { a.data() };
    float* outs[] = { outL.data() };

    // Driven until eight blocks have landed, rather than assuming that eight
    // callbacks deliver eight blocks. They need not, and this test lost that
    // bet on a CI runner while passing twenty times in a row on a fast one.
    //
    // At least two things can make a callback contribute nothing, both of them
    // shipping behaviour rather than faults. A callback that collides with the
    // software clock mid-pull fills its headphone buffer with silence and
    // pulls nothing, because two things must never pull one ring. And a block
    // the writer's ring has no room for is REJECTED -- counted as dropped and
    // refused -- so it never reaches the accepted count at all.
    //
    // Which of those the runner hit is not established: neither CPU starvation
    // nor a stall past the loss threshold reproduced it here. So this does not
    // claim a cause. It removes the assumption instead, and the cap keeps the
    // assertion's teeth: a take that is genuinely frozen never reaches eight
    // blocks however long it is driven, and fails here, which is the bug this
    // whole case exists to catch.
    constexpr int64_t kEightBlocks = 64 * 8;
    constexpr int kMostCallbacksAHealthyTakeShouldNeed = 200;

    int callbacks = 0;

    while (c.getFramesAccepted() < kEightBlocks
           && callbacks < kMostCallbacksAHealthyTakeShouldNeed)
    {
        backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (ins, 1, nullptr, 0, 64);
        backend.outputCallback (nullptr, 0, outs, 1, 64);
        ++callbacks;
    }

    const auto beforeLoss = c.getFramesAccepted();

    // Said out loud, because the failure that started this arrived as a bare
    // "REQUIRE failed: beforeLoss" and cost a long evening of guessing. The
    // next one names how hard it was driven and what the take did with it.
    if (beforeLoss < kEightBlocks)
        std::printf ("  drove %d callbacks; accepted %llu frames of %lld, dropped %llu, "
                     "output clock lost: %d\n",
                     callbacks, (unsigned long long) beforeLoss, (long long) kEightBlocks,
                     (unsigned long long) c.getFramesDropped(), (int) c.isOutputClockLost());

    REQUIRE (beforeLoss >= kEightBlocks);

    // Then it stops. Only inputs arrive for a quarter of a second.
    const auto lastOutputCallback = std::chrono::steady_clock::now();
    pushInputsForAWhile (backend, 190, 1333);

    REQUIRE (c.isOutputClockLost());
    REQUIRE (c.getFramesAccepted() > beforeLoss + 64 * 20);

    // And the take covers ALL of that quarter second, not just the part after
    // the loss was noticed. The software clock takes over a tenth of a second
    // after the last callback, and it used to start pulling from that moment:
    // the tenth of a second before it was never pulled, so every stem and the
    // mix came out that much shorter than the wall clock -- and the camera.
    {
        const auto elapsed = std::chrono::steady_clock::now() - lastOutputCallback;
        const auto accepted = static_cast<int64_t> (c.getFramesAccepted() - beforeLoss);
        const auto wallFrames = static_cast<int64_t> (
            std::chrono::duration<double> (elapsed).count() * 48000.0);

        // Scheduling slack on a busy runner, not the tenth of a second (and
        // more) this is here to catch. Windows' ~15 ms timer means the clock
        // can trail by two coarse wakes; a Windows runner was measured 31 ms
        // short on a correct build, while the bug leaves it 117 ms short.
        constexpr int64_t kTolerance = 48000 * 60 / 1000;

        // Nor can it take more than the microphones gave. Each block's sleep
        // can overshoot on a busy runner, so the inputs can fall behind the
        // wall clock; the take is then owed what they delivered, not more.
        const auto owed = std::min (wallFrames, static_cast<int64_t> (190 * 64));

        if (std::llabs (accepted - wallFrames) > kTolerance)
            std::printf ("  across the output loss: accepted %lld frames in %lld frames of wall time, %lld delivered\n",
                         (long long) accepted, (long long) wallFrames, (long long) (190 * 64));

        REQUIRE (accepted >= owed - kTolerance);
        REQUIRE (accepted <= wallFrames + 64);
    }

    // And it comes back: the output callback resumes and reclaims the pull.
    for (int i = 0; i < 100; ++i)
    {
        backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
        backend.inputCallbacks[1] (ins, 1, nullptr, 0, 64);
        backend.outputCallback (nullptr, 0, outs, 1, 64);
        std::this_thread::sleep_for (std::chrono::microseconds (1333));
    }

    REQUIRE (! c.isOutputClockLost());
    c.stopRecording();
}

TEST_CASE (CaptureCoordinator_StopsOfferingItsOutputOnceTheOutputClockIsLost)
{
    // The headphones are unplugged mid-take and the fault siren goes on. The
    // app routes its own sounds by hasOutputStream(): into the headphone mix
    // while it says yes, through the computer's default output (AlarmSpeaker)
    // when it says no. It went on saying yes after the output had stopped
    // calling back, so the siren was set on an alarm only the output callback
    // renders -- the software clock pulls with no headphone buffer, and the
    // mix returns before the alarm when there is none. The siren meant to tell
    // the operator the headphones had died was the one sound nobody could
    // hear, and the speaker that exists for exactly this was held shut.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.hasOutputStream());

    c.getAlarm().setFault (true);

    std::vector<float> a (64, 0.5f), outL (64, 0.0f);
    const float* ins[] = { a.data() };
    float* outs[] = { outL.data() };

    const auto driveWithTheOutput = [&] (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
            backend.inputCallbacks[1] (ins, 1, nullptr, 0, 64);
            backend.outputCallback (nullptr, 0, outs, 1, 64);
            std::this_thread::sleep_for (std::chrono::microseconds (1333));
        }
    };

    // While the headphones are there, they carry the siren.
    driveWithTheOutput (50);
    REQUIRE (c.hasOutputStream());
    REQUIRE (c.getAlarm().getSamplesRendered() > 0);

    // Then they stop taking audio. Only the microphones arrive.
    pushInputsForAWhile (backend, 190, 1333);
    REQUIRE (c.isOutputClockLost());

    // Nothing renders the siren now...
    const auto renderedAtLoss = c.getAlarm().getSamplesRendered();
    pushInputsForAWhile (backend, 40, 1333);
    REQUIRE (c.getAlarm().getSamplesRendered() == renderedAtLoss);

    // ...so the rig must stop offering an output to carry it.
    REQUIRE_FALSE (c.hasOutputStream());

    // And once the headphones are back, they carry it again.
    driveWithTheOutput (100);
    REQUIRE (! c.isOutputClockLost());
    REQUIRE (c.hasOutputStream());
    REQUIRE (c.getAlarm().getSamplesRendered() > renderedAtLoss);

    c.getAlarm().setFault (false);
}

TEST_CASE (CaptureCoordinator_AnOutputSwitchedToAnotherRateStopsClockingTheTake)
{
    // Another app switches the headphone output to 44.1 kHz mid-take. Its
    // callback keeps coming -- at the new rate -- and it was the clock every
    // microphone is pulled on, so it pulled them all 8% slow and every ring
    // overflowed; the only remedy the app had was to stop the take, though no
    // microphone's file had changed. Set aside, it pulls nothing, the
    // software clock carries the take on at the take's own rate, and the
    // headphones get silence rather than a mix played at the wrong speed.
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.getOutputDeviceId() == "out-device");
    REQUIRE_FALSE (c.outputCarriesTakeChannels());
    REQUIRE (c.startRecording (dir, 16, "2026-09-07T00:00:00Z"));

    std::vector<float> a (64, 0.5f), outL (64, 0.0f);
    const float* ins[] = { a.data() };
    float* outs[] = { outL.data() };

    const auto driveWithTheOutput = [&] (int blocks, int outputFrames)
    {
        for (int i = 0; i < blocks; ++i)
        {
            backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
            backend.inputCallbacks[1] (ins, 1, nullptr, 0, 64);
            std::fill (outL.begin(), outL.end(), 0.25f);
            backend.outputCallback (nullptr, 0, outs, 1, outputFrames);
            std::this_thread::sleep_for (std::chrono::microseconds (1333));
        }
    };

    driveWithTheOutput (50, 64);
    REQUIRE (c.hasOutputStream());
    REQUIRE_FALSE (c.isOutputAbandoned());

    const std::string why = "Your sound output changed to 44.1 kHz while SobStage is using 48 kHz.";
    REQUIRE (c.abandonOutputClock (why));

    // Said at once, not a clock tick later: the app's own sounds go another
    // way from this moment.
    REQUIRE (c.isOutputAbandoned());
    REQUIRE (c.isOutputClockLost());
    REQUIRE_FALSE (c.hasOutputStream());
    REQUIRE (c.getMonitorProblem() == why);

    const auto before = c.getFramesAccepted();
    const auto started = std::chrono::steady_clock::now();

    // A quarter of a second: the microphones at 48 kHz, the output still
    // calling back, now taking 59 frames a period -- 44.1 kHz's pace.
    //
    // Looked at well inside the tenth of a second after which an output that
    // had stopped calling back would be called lost anyway: this one has not
    // stopped, and still it stays set aside rather than flickering back.
    driveWithTheOutput (40, 59);
    REQUIRE (c.isOutputClockLost());
    REQUIRE_FALSE (c.hasOutputStream());

    driveWithTheOutput (150, 59);

    // The output was handed silence, not a mix pulled at its rate...
    for (size_t i = 0; i < 59; ++i)
        REQUIRE (outL[i] == 0.0f);

    // ...it stayed set aside, however recently it called back...
    REQUIRE (c.isOutputClockLost());
    REQUIRE_FALSE (c.hasOutputStream());

    // ...and the take is the quarter second long, pulled once, by the
    // software clock: not short, and not pulled twice over.
    {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto accepted = static_cast<int64_t> (c.getFramesAccepted() - before);
        const auto wallFrames = static_cast<int64_t> (
            std::chrono::duration<double> (elapsed).count() * 48000.0);

        // The same scheduling slack as the clock-loss test above.
        constexpr int64_t kTolerance = 48000 * 60 / 1000;

        // What the microphones delivered bounds what the take can hold: a
        // busy runner's sleeps overshoot, and the inputs fall behind the wall.
        const auto owed = std::min (wallFrames, static_cast<int64_t> ((40 + 150) * 64));

        if (std::llabs (accepted - wallFrames) > kTolerance)
            std::printf ("  with the output set aside: accepted %lld frames in %lld frames of wall time, %lld delivered\n",
                         (long long) accepted, (long long) wallFrames, (long long) ((40 + 150) * 64));

        REQUIRE (accepted >= owed - kTolerance);
        REQUIRE (accepted <= wallFrames + kTolerance);
    }

    c.stopRecording();

    // Reopened, as Stop does: the new output stream is the clock again.
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE_FALSE (c.isOutputAbandoned());
    REQUIRE (c.hasOutputStream());
    REQUIRE (c.getMonitorProblem().empty());
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_AnOutputStopsClockingTheTakeOnThePlatformsWordOfARateChange)
{
    // Another app switches the headphone output to 44.1 kHz mid-take. The
    // app hears of it on its next poll, up to half a second later; until then
    // the output's callback went on pulling every microphone at its new rate
    // -- 59 frames a period where each microphone delivered 64. Set aside at
    // the poll, the software clock picked up from that callback, a moment
    // old, owing nothing: the shortfall was never pulled, so every track ran
    // that much short of the wall clock and the cameras for the rest of the
    // take, and every ring was left full for the drift loop to drain over
    // minutes, overflowing -- counted against the computer -- meanwhile.
    //
    // The platform's word (the backend's rate listener) now holds the output
    // off the clock at once, before any poll: it pulls nothing, and the
    // software clock takes over from the last block it really pulled.
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-09-07T00:00:00Z"));

    std::vector<float> a (64, 0.5f), outL (64, 0.0f);
    const float* ins[] = { a.data() };
    float* outs[] = { outL.data() };

    int64_t blocksDriven = 0;

    const auto driveWithTheOutput = [&] (int blocks, int outputFrames)
    {
        for (int i = 0; i < blocks; ++i, ++blocksDriven)
        {
            backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
            backend.inputCallbacks[1] (ins, 1, nullptr, 0, 64);
            std::fill (outL.begin(), outL.end(), 0.25f);
            backend.outputCallback (nullptr, 0, outs, 1, outputFrames);
            std::this_thread::sleep_for (std::chrono::microseconds (1333));
        }
    };

    // Whether every ring is down near its target fill (an eighth) at some
    // point over a few more blocks: a ring backed up behind an output pulling
    // slow stays pinned near full, while one a busy machine's scheduling
    // briefly fills comes straight back down.
    const auto ringsAreWhereTheyWere = [&]
    {
        double lowest[2] = { 1.0, 1.0 };

        for (int i = 0; i < 20; ++i)
        {
            driveWithTheOutput (1, backend.outputOffRate.load() ? 59 : 64);
            for (int ch = 0; ch < 2; ++ch)
                lowest[ch] = std::min (lowest[ch], c.getChannelFillFraction (ch));
        }

        for (int ch = 0; ch < 2; ++ch)
            if (lowest[ch] >= 0.5)
            {
                std::printf ("  channel %d's ring stayed at least %.0f%% full\n", ch, lowest[ch] * 100.0);
                return false;
            }
        return true;
    };

    driveWithTheOutput (100, 64);
    REQUIRE (c.hasOutputStream());
    REQUIRE_FALSE (c.isOutputOffRate());
    REQUIRE (ringsAreWhereTheyWere());

    const auto before = c.getFramesAccepted();
    const auto started = std::chrono::steady_clock::now();
    const auto blocksBefore = blocksDriven;
    const uint64_t overrunBefore[2] = { c.getChannelOverrunSamples (0), c.getChannelOverrunSamples (1) };

    // The listener's word. Nothing has read the new rate yet, and the app
    // has decided nothing.
    backend.outputOffRate = true;

    // At once: the app's own sounds go another way.
    REQUIRE (c.isOutputOffRate());
    REQUIRE_FALSE (c.hasOutputStream());
    REQUIRE_FALSE (c.isOutputAbandoned());

    // Half a second, the longest the poll can take to come round: the
    // microphones at 48 kHz, the output calling back at 44.1 kHz's pace.
    driveWithTheOutput (375, 59);

    // The output pulled nothing at its new rate -- it was handed silence --
    // and the software clock is pulling instead...
    for (size_t i = 0; i < 59; ++i)
        REQUIRE (outL[i] == 0.0f);
    REQUIRE (c.isOutputClockLost());

    // ...so no ring backed up behind it...
    REQUIRE (ringsAreWhereTheyWere());

    // ...and the software clock took over on that same word, not a stale
    // window later: waiting for the output's stamp to go stale left a tenth
    // of a second with nothing pulling, most of it overflowing every ring.
    // The ring's own headroom covers the clock's next look at the smallest
    // buffer, all but a few milliseconds; this allows for a busy machine.
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto lost = c.getChannelOverrunSamples (ch) - overrunBefore[ch];
        if (lost >= 48000 * 40 / 1000)
            std::printf ("  channel %d lost %llu samples over the hand-over\n", ch, (unsigned long long) lost);
        REQUIRE (lost < 48000 * 40 / 1000);
    }

    // The poll reads the report and sets the output aside for the take.
    REQUIRE (c.abandonOutputClock ("Your sound output changed to 44.1 kHz while SobStage is using 48 kHz."));
    driveWithTheOutput (75, 59);
    REQUIRE (ringsAreWhereTheyWere());

    // The take is as long as the wall clock says, from the moment of the
    // switch: nothing short, nothing pulled twice.
    {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto accepted = static_cast<int64_t> (c.getFramesAccepted() - before);
        const auto wallFrames = static_cast<int64_t> (
            std::chrono::duration<double> (elapsed).count() * 48000.0);

        // The same scheduling slack as the clock-loss test above.
        constexpr int64_t kTolerance = 48000 * 60 / 1000;

        // What the microphones delivered bounds what the take can hold: a
        // busy runner's sleeps overshoot, and the inputs fall behind the wall.
        const auto delivered = (blocksDriven - blocksBefore) * 64;
        const auto owed = std::min (wallFrames, delivered);

        if (std::llabs (accepted - wallFrames) > kTolerance)
            std::printf ("  across the switch: accepted %lld frames in %lld frames of wall time, %lld delivered\n",
                         (long long) accepted, (long long) wallFrames, (long long) delivered);

        REQUIRE (accepted >= owed - kTolerance);
        REQUIRE (accepted <= wallFrames + kTolerance);
    }

    c.stopRecording();
    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_AnOutputBackAtItsRateBeforeThePollClocksTheRigAgain)
{
    // Switched away and straight back inside one poll -- or the platform
    // telling of a change that changed nothing. The backend reads the rate
    // back as the stream's and lets go; nothing set the output aside, so it
    // is the clock again and the headphones hear the mix.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.5f), outL (64, 0.0f);
    const float* ins[] = { a.data() };
    float* outs[] = { outL.data() };

    const auto drive = [&] (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);
            backend.inputCallbacks[1] (ins, 1, nullptr, 0, 64);
            backend.outputCallback (nullptr, 0, outs, 1, 64);
            std::this_thread::sleep_for (std::chrono::microseconds (1333));
        }
    };

    drive (50);
    REQUIRE (c.hasOutputStream());

    backend.outputOffRate = true;
    drive (60);
    REQUIRE (c.isOutputClockLost());
    REQUIRE_FALSE (c.hasOutputStream());

    backend.outputOffRate = false;
    drive (60);
    REQUIRE_FALSE (c.isOutputOffRate());
    REQUIRE_FALSE (c.isOutputClockLost());
    REQUIRE (c.hasOutputStream());
    REQUIRE_FALSE (c.isOutputAbandoned());
    REQUIRE (c.getMonitorProblem().empty());

    c.stopMonitoring();
}

TEST_CASE (CaptureCoordinator_AnOutputIsOnlySetAsideWhenSomethingElseCanClockTheTake)
{
    // No software clock to take over: setting the output aside would leave
    // nothing pulling the take, so it is refused and the caller stops instead.
    {
        FakeBackend backend;
        CaptureCoordinator c (backend, 48000.0, 64);
        c.setSoftwareClockEnabled (false);
        REQUIRE (c.startMonitoring (twoMics(), "out-device"));
        REQUIRE_FALSE (c.abandonOutputClock ("changed rate"));
        REQUIRE_FALSE (c.isOutputAbandoned());
        REQUIRE (c.hasOutputStream());
        REQUIRE (c.getMonitorProblem().empty());
    }

    // No output stream at all: nothing to set aside.
    {
        FakeBackend backend;
        CaptureCoordinator c (backend, 48000.0, 64);
        REQUIRE (c.startMonitoring (twoMics(), ""));
        REQUIRE (c.getOutputDeviceId().empty());
        REQUIRE_FALSE (c.abandonOutputClock ("changed rate"));
        REQUIRE_FALSE (c.isOutputAbandoned());
        c.stopMonitoring();
    }

    // An output that refused to open, so the rig went input-only.
    {
        FakeBackend backend;
        backend.failOutputOpen = true;
        CaptureCoordinator c (backend, 48000.0, 64);
        REQUIRE (c.startMonitoring (twoMics(), "out-device"));
        REQUIRE (c.getOutputDeviceId().empty());
        REQUIRE_FALSE (c.abandonOutputClock ("changed rate"));
        c.stopMonitoring();
    }

    // A mixer that is the headphones and two of the microphones: its output
    // callback carries them, and silencing it would silence them.
    {
        FakeBackend backend;
        CaptureCoordinator c (backend, 48000.0, 64);

        std::vector<CaptureChannel> mixerChannels {
            { "mixer", "PUP 1", "01_PUP-1", 0.0f },
            { "mixer", "PUP 2", "02_PUP-2", 0.0f },
        };
        mixerChannels[1].deviceChannel = 1;

        REQUIRE (c.startMonitoring (mixerChannels, "mixer"));
        REQUIRE (c.outputCarriesTakeChannels());
        REQUIRE_FALSE (c.abandonOutputClock ("changed rate"));
        REQUIRE_FALSE (c.isOutputAbandoned());
        c.stopMonitoring();
        REQUIRE_FALSE (c.outputCarriesTakeChannels());
    }
}

TEST_CASE (CaptureCoordinator_CountsRingOverruns)
{
    // §0.1: audio the rings had to throw away is counted, not swallowed.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.getOverrunSamples() == 0);

    std::vector<float> a (64, 0.5f);
    const float* ins[] = { a.data() };

    // Far more than the ring holds, with nothing pulling in between.
    for (int i = 0; i < 40; ++i)
        backend.inputCallbacks[0] (ins, 1, nullptr, 0, 64);

    REQUIRE (c.getOverrunSamples() > 0);
    REQUIRE (c.getOverrunSamples() < 40 * 64);
}

TEST_CASE (CaptureCoordinator_UnpluggingAnInterfaceSilencesAllItsSockets)
{
    // Two people on one interface: the same deviceId, two device channels.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false); // simulated time; see the software-clock tests below

    CaptureChannel left, right;
    left.deviceId = "iface";  left.displayName = "Alex"; left.fileName = "01_Alex"; left.deviceChannel = 0;
    right.deviceId = "iface"; right.displayName = "Sam"; right.fileName = "02_Sam"; right.deviceChannel = 1;

    REQUIRE (c.startMonitoring ({ left, right }, "out-device"));
    REQUIRE (c.isChannelLive (0));
    REQUIRE (c.isChannelLive (1));

    c.setChannelLive ("iface", false);

    // This used to stop at the first match: Alex went silent, Sam kept a
    // held sample for the rest of the take.
    REQUIRE (! c.isChannelLive (0));
    REQUIRE (! c.isChannelLive (1));

    c.setChannelLive ("iface", true);
    REQUIRE (c.isChannelLive (0));
    REQUIRE (c.isChannelLive (1));
}

TEST_CASE (CaptureCoordinator_ABlockThatDoesNotFitTheLayoutIsCountedNotJustDropped)
{
    // §0.1: a device handing over fewer inputs than the take was planned around
    // leaves those channels writing silence. That is the right behaviour -- the
    // layout is fixed for the take -- but it used to leave no trace anywhere,
    // so audio that should have been recorded simply was not and nothing said
    // so. Same rule the write pipeline already applies one layer up.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.getFramesMissedByLayout() == 0u);

    // A channel index this coordinator does not have: the audio has nowhere to
    // go, which is exactly the case that was silent.
    std::vector<float> block (64, 0.5f);
    c.pushDeviceBlock (99, block.data(), 64);

    REQUIRE (c.getFramesMissedByLayout() == 64u);
}

TEST_CASE (CaptureCoordinator_TheWorstChannelIsWhatBecomesSeconds)
{
    // Summing every channel answers "how many samples were thrown away", which
    // is right for a record of the loss and wrong for a clock: four rings
    // overflowing together for one second lose one second of recording, not
    // four. The alert that says "about N seconds lost so far" was fed the sum.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.getWorstChannelOverrunThisTake() == 0u);
    REQUIRE (c.getOverrunSamplesThisTake() == 0u);
}

// §5.4: the monitoring latency the backend worked out has to reach the caller.
//
// Every backend computes it in checkExclusiveModeCapability, and the
// coordinator dropped it on the floor -- so Application::measuredLatencyMs was
// never assigned by anything, the Advanced panel reported "0.0 ms", and every
// take's session.json recorded 0.0 as a permanent fact about how the take was
// made. Zero is not a small latency; it is an impossible one.
TEST_CASE (CaptureCoordinator_TheMonitoringLatencyReachesTheCaller)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    REQUIRE (coordinator.startMonitoring ({ mic }, "out-1"));

    // The backend's round trip, the cushion every microphone's ring holds
    // ahead of the output -- at 256 a pull and 6 ms of late delivery, 544
    // samples -- and the resampler's two.
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMs() - (10.67 + (544.0 + kResampler) / 48000.0 * 1000.0)) < 1e-9);
}

// Nothing monitoring means no monitoring latency. A figure left standing would
// outlive the stream it describes and sit in the panel beside monitoring that
// is switched off.
TEST_CASE (CaptureCoordinator_NoMonitorOutputMeansNoLatencyToReport)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    // Recording with no monitor output at all: the capability is never asked.
    REQUIRE (coordinator.startMonitoring ({ mic }, {}));
    REQUIRE (coordinator.getMonitoringLatencyMs() == 0.0);

    // And a monitor that opened, then stopped.
    REQUIRE (coordinator.startMonitoring ({ mic }, "out-1"));
    REQUIRE (coordinator.getMonitoringLatencyMs() > 0.0);
    coordinator.stopMonitoring();
    REQUIRE (coordinator.getMonitoringLatencyMs() == 0.0);
}

// The preflight said yes and the open said no, so the figure describes a stream
// that does not exist. Reporting it would put a monitoring latency beside
// monitoring that is switched off -- the same lie as 0.0, pointing the other
// way.
TEST_CASE (CaptureCoordinator_AMonitorThatFailedToOpenReportsNoLatency)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;
    backend.failOutputOpen = true;

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    // Monitoring is refused but recording carries on, which is the §5.4 rule.
    coordinator.startMonitoring ({ mic }, "out-1");
    REQUIRE (! coordinator.getMonitorProblem().empty());
    REQUIRE (coordinator.getMonitoringLatencyMs() == 0.0);
}

// An output the preflight refuses never opens, so it has no latency either.
TEST_CASE (CaptureCoordinator_AnOutputRefusedByThePreflightReportsNoLatency)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;
    backend.exclusiveAvailable = false;
    backend.exclusiveReason = "This sound output is shared with other apps.";

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    coordinator.startMonitoring ({ mic }, "out-1");
    REQUIRE (! coordinator.getMonitorProblem().empty());
    REQUIRE (coordinator.getMonitoringLatencyMs() == 0.0);
}

// §5.4: the latency has to describe the buffer the device GRANTED, not the one
// it was asked for.
//
// The figure came from checkExclusiveModeCapability, which runs before the
// stream exists and can only estimate. A driver is free to align a request up
// to its own period -- CoreAudio already tells the user there is "a little more
// delay than usual" when that happens -- and the number printed beside that
// sentence was still the one for the buffer the device had just refused.
TEST_CASE (CaptureCoordinator_TheLatencyDescribesTheBufferTheDeviceGranted)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;      // the estimate for the 256 asked for
    backend.grantedOutputBufferFrames = 512; // what the device actually handed back

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    REQUIRE (coordinator.startMonitoring ({ mic }, "out-1"));

    // 512 frames at 48 kHz is 10.667 ms one way, so the round trip is 21.333 --
    // twice the estimate, because the device gave twice the buffer -- and
    // every microphone's ring is pulled 512 at a time, so it holds two of
    // those ahead of the output (two pulls of 512 already ride out more
    // than the 6 ms of late delivery the cushion is sized for), and the
    // resampler adds its two samples.
    const double expected = (512.0 / 48000.0) * 1000.0 * 2.0 + ((2.0 * 512.0 + kResampler) / 48000.0) * 1000.0;
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMs() - expected) < 1e-9);

    // And it is emphatically not the estimate's any more.
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMs() - (10.67 + 544.0 / 48000.0 * 1000.0)) > 1.0);
}

// Bluetooth headphones add well over 100 ms after the buffers. The figure
// used to leave that out and print about 3 ms for AirPods.
TEST_CASE (CaptureCoordinator_TheLatencyIncludesWhatTheOutputDeviceAdds)
{
    FakeBackend backend;
    backend.grantedOutputBufferFrames = 64;
    backend.outputPresentationLatencyFrames = 7200; // 150 ms at 48 kHz

    CaptureCoordinator coordinator (backend, 48000.0, 64);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    REQUIRE (coordinator.startMonitoring ({ mic }, "out-1"));

    const double expected = ((2.0 * 64.0 + 256.0 + kResampler + 7200.0) / 48000.0) * 1000.0;
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMs() - expected) < 1e-9);
    REQUIRE (coordinator.getMonitoringLatencyMs() > 150.0);

    // And it is still there after a buffer step: the ladder's figure for 128
    // keeps what the device adds.
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMsAt (128)
                       - ((2.0 * 128.0 + 416.0 + kResampler + 7200.0) / 48000.0) * 1000.0) < 1e-9);
}

// A backend that cannot say keeps the estimate. Zero is not a small latency,
// and reporting one would be worse than reporting an approximate one.
TEST_CASE (CaptureCoordinator_ABackendThatCannotSayKeepsTheEstimate)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;
    backend.grantedOutputBufferFrames = 0;   // every backend, before this existed

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    REQUIRE (coordinator.startMonitoring ({ mic }, "out-1"));
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMs() - (10.67 + (544.0 + kResampler) / 48000.0 * 1000.0)) < 1e-9);
}

// A device that granted exactly what was asked for reports exactly the
// estimate, so the new path cannot quietly shift a correct figure.
TEST_CASE (CaptureCoordinator_AGrantedBufferMatchingTheRequestChangesNothing)
{
    FakeBackend backend;
    backend.exclusiveLatencyMs = 10.67;
    backend.grantedOutputBufferFrames = 256;  // exactly what was requested

    CaptureCoordinator coordinator (backend, 48000.0, 256);

    CaptureChannel mic;
    mic.deviceId = "mic-1";
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";

    REQUIRE (coordinator.startMonitoring ({ mic }, "out-1"));

    const double expected = (256.0 / 48000.0) * 1000.0 * 2.0 + ((544.0 + kResampler) / 48000.0) * 1000.0;
    REQUIRE (std::abs (coordinator.getMonitoringLatencyMs() - expected) < 1e-9);
}

TEST_CASE (CaptureCoordinator_TheTakesLoudnessSurvivesTheTake)
{
    // §10's delivery advice is read AFTER the take, not during it -- that is
    // when the user decides whether to re-record or how to master. It was
    // readable only during: stopRecording moves the WritePipeline out and
    // destroys it, the getters were pipeline-conditional, and so the block
    // count fell to zero the instant Stop was pressed and adviseForTarget
    // reverted to "Not enough sound yet to judge how loud this is."
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));

    // A real tone, not a constant. BS.1770's K-weighting pre-filter is a
    // high-pass, so a DC block measures as silence no matter how large it is --
    // which is a good way to write a loudness test that proves nothing.
    std::vector<float> a (64, 0.0f), b (64, 0.0f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    // Four seconds, comfortably past kMinimumBlocksToJudge (30 hops = 3 s),
    // so the advice would have a verdict to give rather than a shrug.
    int phase = 0;
    for (int i = 0; i < 3000; ++i)
    {
        for (int f = 0; f < 64; ++f, ++phase)
        {
            const auto v = 0.4f * std::sin (6.283185307f * 440.0f
                                            * static_cast<float> (phase) / 48000.0f);
            a[static_cast<size_t> (f)] = v;
            b[static_cast<size_t> (f)] = v;
        }

        c.processAudioBlock (ins, 2, outs, 1, 64);
    }

    c.stopRecording();

    // The take is over and the pipeline is gone. These are the figures the
    // delivery advice reads, and they have to still be here.
    REQUIRE (c.getLoudnessBlockCount() >= kMinimumBlocksToJudge);
    REQUIRE (c.getIntegratedLufs() > LoudnessMeter::kAbsoluteGateLufs);
    REQUIRE (c.getTruePeakDbtp() > LoudnessMeter::kSilenceLufs);

    // And that is enough for the advice itself to say something real rather
    // than the not-enough-sound line, which is the point of the whole fix.
    const auto advice = adviseForTarget (streamingTargets().front(),
                                         c.getIntegratedLufs(),
                                         c.getTruePeakDbtp(),
                                         c.getLoudnessBlockCount());
    REQUIRE (advice.measurable);
    REQUIRE (advice.summary.find ("Not enough sound") == std::string::npos);
}

TEST_CASE (CaptureCoordinator_ANewTakeDoesNotInheritTheLastOnesLoudness)
{
    // The counterpart. A snapshot that outlived the NEXT take's start would be
    // worse than none: the user would read take two's number under take three.
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    for (int i = 0; i < 3000; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();
    REQUIRE (c.getLoudnessBlockCount() > 0);

    const auto second = tempDir();
    REQUIRE (c.startRecording (second, 16, "2026-08-27T00:01:00Z"));
    REQUIRE (c.getLoudnessBlockCount() == 0);
    c.stopRecording();
}

TEST_CASE (CaptureCoordinator_TheTakesDroppedFramesSurviveTheTake)
{
    // session.json is rewritten at Stop, after stopRecording() has moved the
    // WritePipeline out and destroyed it. The dropped-frame getter read the
    // pipeline, so the one record written at the end of a take that lost audio
    // said it had lost none.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    c.setSoftwareClockEnabled (false);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-29T00:00:00Z"));

    // One block larger than the writer's whole ring: it cannot fit, so the
    // pipeline counts every frame of it as dropped -- deterministically, with
    // no race against the writer thread draining.
    const int frames = static_cast<int> (RingBuffer::minimumCapacitySamples (48000.0, 2) / 2) + 64;
    std::vector<float> a (static_cast<size_t> (frames), 0.1f), b (static_cast<size_t> (frames), 0.1f);
    const float* ins[] = { a.data(), b.data() };
    c.processAudioBlock (ins, 2, nullptr, 0, frames);

    REQUIRE (c.getFramesDropped() == static_cast<uint64_t> (frames));

    c.stopRecording();
    REQUIRE (c.getFramesDropped() == static_cast<uint64_t> (frames));

    // And the next take starts from nothing rather than inheriting it.
    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-29T00:00:01Z"));
    REQUIRE (c.getFramesDropped() == 0u);
    c.stopRecording();
    REQUIRE (c.getFramesDropped() == 0u);
}
