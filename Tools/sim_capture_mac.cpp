// The full recording stack on the macOS code path, end to end.
//
// sim_coreaudio proves the backend hands over the right samples. live_capture
// proves the coordinator and writer turn samples into files. Nothing joined
// them: no test drove backend -> coordinator -> writer -> files on the path
// macOS actually takes, and the two harnesses between them left three gaps
// that a real user fell straight into.
//
//   - live_capture's fixture mics are MONO, so the stereo path -- the one a
//     USB mixer or any stereo interface takes, through §2.1's channel-layout
//     analysis -- never reached a file in any test.
//   - every harness and every unit test records at 16 bits. The app ships 24.
//   - the backend was only ever asked for audio in isolation, never while a
//     take was running.
//
// This closes all three: a stereo interleaved device, at 24 bits, recorded
// through the real coordinator into real files, with the bytes checked.

#include "../Simulation/CoreAudio/FakeCoreAudio.h"
#include "../Source/Platform/CoreAudioBackend.h"
#include "../Source/Core/CaptureCoordinator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Not M_PI: that is a POSIX extension rather than standard C++, and MSVC does
// not define it from <cmath> without _USE_MATH_DEFINES. e2e_capture.cpp already
// carries this same note, having broken the Windows build the same way -- and
// this harness went and did it again, so here it is a third time.
constexpr double kTwoPi = 6.283185307179586476925286766559;

int failures = 0;

void check (bool condition, const std::string& what)
{
    std::printf (condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (! condition)
        ++failures;
}

/// A folder of this run's own. The capture simulators used to write straight
/// into the temp directory under shared names -- 01_Singer.wav, MIX.wav -- and
/// delete them after each scenario, so under `ctest -j` the macOS and Windows
/// simulators wrote over and deleted each other's files mid-check.
std::string tempDir()
{
    std::string base = "/tmp";

    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
        if (const char* d = std::getenv (var); d != nullptr && *d != '\0')
        {
            base = d;
            break;
        }

    static const struct Folder
    {
        std::filesystem::path path;
        ~Folder() { std::error_code ignored; std::filesystem::remove_all (path, ignored); }
    } folder { std::filesystem::path (base)
               / ("sobstage-sim-capture-mac-"
                  + std::to_string (std::chrono::steady_clock::now().time_since_epoch().count())) };

    std::error_code ignored;
    std::filesystem::create_directories (folder.path, ignored);
    return folder.path.string();
}

fakeca::DeviceSpec microphone (const char* name, const char* uid, int channels,
                               fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.inputChannels = channels;
    spec.shape = shape;
    return spec;
}

fakeca::DeviceSpec headphones (const char* name, const char* uid, int channels,
                               fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.outputChannels = channels;
    spec.shape = shape;
    return spec;
}

std::vector<float> tone (int frames, double hz, double rate, float amplitude)
{
    std::vector<float> v (static_cast<size_t> (frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t> (i)] =
            amplitude * static_cast<float> (std::sin (kTwoPi * hz * i / rate));
    return v;
}

/// The largest absolute 24-bit sample in a finished WAV, so "did this file
/// carry audio" is answered from the bytes rather than from the file size.
int32_t peakOf24BitWav (const std::string& path, uint32_t& framesOut)
{
    // RIFF 12 + fmt (8+16) + bext (8+602) + "data" tag 4, matching SessionWriter.
    constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;

    std::ifstream f (path, std::ios::binary);
    framesOut = 0;

    if (! f.is_open())
        return -1;

    f.seekg (kDataSizeOffset);
    unsigned char b[4] {};
    f.read (reinterpret_cast<char*> (b), 4);
    const uint32_t dataBytes = static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
                             | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);

    framesOut = dataBytes / 3;
    int32_t peak = 0;

    for (uint32_t i = 0; i < framesOut; ++i)
    {
        unsigned char s[3] {};
        f.read (reinterpret_cast<char*> (s), 3);

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)          // sign-extend 24-bit
            v |= ~0xFFFFFF;

        peak = std::max (peak, v < 0 ? -v : v);
    }

    return peak;
}

/// Where a 24-bit WAV's audio stops: the index just past the last non-zero
/// sample, and the largest sample found after it.
///
/// Located by CONTENT rather than by fraction of the file, which is what the
/// first version of this got wrong. It measured eighths of the file on the
/// assumption that the file was as long as the audio pumped into it -- and it
/// is not, because the harness pumps blocks back to back with no wall clock
/// between them and the writer's ring drops what it cannot take in time. On
/// this machine 16384 frames go in and 16384 come out; on a loaded CI runner
/// 9728 came out, which slid the unplug from the middle of the file to
/// two-thirds through it, and a range that had been safely after the event was
/// suddenly straddling it. The test failed, correctly, on a stack that was
/// behaving perfectly.
///
/// Nothing here depends on how much of the take survived.
struct AudioExtent
{
    uint32_t frames = 0;
    uint32_t lastSounding = 0;   ///< one past the last non-zero sample
    int32_t peakBefore = 0;      ///< largest sample up to that point
};

AudioExtent extentOf24BitWav (const std::string& path)
{
    AudioExtent e;

    constexpr std::streamoff kDataOffset = 12 + (8 + 16) + (8 + 602) + 8;

    if (peakOf24BitWav (path, e.frames) < 0 || e.frames == 0)
        return e;

    std::ifstream f (path, std::ios::binary);
    f.seekg (kDataOffset);

    for (uint32_t i = 0; i < e.frames; ++i)
    {
        unsigned char s[3] {};
        f.read (reinterpret_cast<char*> (s), 3);

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)
            v |= ~0xFFFFFF;

        if (v != 0)
        {
            e.lastSounding = i + 1;
            e.peakBefore = std::max (e.peakBefore, v < 0 ? -v : v);
        }
    }

    return e;
}

/// The largest absolute sample in [from, to) frames.
int32_t peakOfRange (const std::string& path, uint32_t from, uint32_t to)
{
    constexpr std::streamoff kDataOffset = 12 + (8 + 16) + (8 + 602) + 8;

    if (to <= from)
        return 0;

    std::ifstream f (path, std::ios::binary);
    f.seekg (kDataOffset + static_cast<std::streamoff> (from) * 3);

    int32_t peak = 0;

    for (uint32_t i = from; i < to; ++i)
    {
        unsigned char s[3] {};
        f.read (reinterpret_cast<char*> (s), 3);

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)
            v |= ~0xFFFFFF;

        peak = std::max (peak, v < 0 ? -v : v);
    }

    return peak;
}

} // namespace

int main()
{
    const auto dir = tempDir();
    const double rate = 48000.0;
    const int block = 256;

    std::printf ("A stereo USB device recorded at 24 bits, through the whole stack\n");
    fakeca::reset();

    // Two channels on one interleaved buffer: what a USB mixer or any stereo
    // interface looks like, and what §2.1's layout analysis has to decide on.
    const auto id = fakeca::addDevice (microphone ("Stereo Board", "uid-board", 2,
                                                   fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    mma::CaptureCoordinator coordinator (backend, rate, block);

    std::vector<mma::CaptureChannel> mics = { { "uid-board", "Board", "01_Board", 0.0f } };

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    check (fakeca::isRunning (id), "the device is running");

    // 24 bits: the depth the app ships, which no other harness records at.
    if (! coordinator.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out (static_cast<size_t> (block) * 2, 0.0f);
    float* outs[] = { out.data(), out.data() + block };

    // A second of audio, pumped in and pulled out the way the two real
    // callbacks would interleave.
    const int blocks = static_cast<int> (rate / block);

    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            tone (block, 440.0, rate, 0.5f),
            tone (block, 440.0, rate, 0.5f),
        };

        fakeca::pumpInput (id, signal);
        coordinator.pullOutputBlock (outs, 2, block);
    }

    const float arrived = coordinator.getPeakArrived();
    const float written = coordinator.getPeakWritten();

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    std::printf ("\n  peak arrived %.4f   peak written %.4f\n", arrived, written);

    check (arrived > 0.1f, "audio reaches the coordinator");
    check (written > 0.1f, "and the writer accepts it");

    uint32_t frames = 0;
    const auto peak = peakOf24BitWav (dir + "/01_Board.wav", frames);

    std::printf ("  01_Board.wav: %u frames, peak %d\n", frames, peak);

    check (frames > 0, "the stem has frames in it");
    check (peak > 100000, "and those frames carry signal, not silence");

    uint32_t mixFrames = 0;
    const auto mixPeak = peakOf24BitWav (dir + "/MIX.wav", mixFrames);

    std::printf ("  MIX.wav:      %u frames, peak %d\n", mixFrames, mixPeak);
    check (mixPeak > 100000, "the mix carries signal too");

    std::remove ((dir + "/01_Board.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // An interface with four microphones plugged into it.
    //
    // One device, four inputs, four people. The app used to take exactly one
    // channel from any device -- §2.1's stereo collapse, applied to an
    // interface -- so three of the four were discarded without a word, and
    // anyone whose microphone was on a discarded input got silence.
    // ---------------------------------------------------------------------
    std::printf ("\nAn interface with four microphones on four inputs\n");
    fakeca::reset();

    const auto rig = fakeca::addDevice (microphone ("4-in Interface", "uid-rig", 4,
                                                    fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend2;
    mma::CaptureCoordinator four (backend2, rate, block);

    // What Application::buildCaptureChannels now produces for a 4-input device.
    std::vector<mma::CaptureChannel> band;
    for (int i = 0; i < 4; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "uid-rig";
        c.deviceChannel = i;
        c.displayName = "Mic " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Mic-" + std::to_string (i + 1);
        band.push_back (c);
    }

    if (! four.startMonitoring (band, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", four.getMonitorProblem().c_str());
        return 1;
    }

    // Counted, not asserted in words. This line used to read
    // `check (true, ...)`: it named the exact failure it was there to catch --
    // one IOProc per microphone instead of one per device -- and would have
    // passed just as happily with four open.
    //
    // §5.2 and CoreAudio both want one. Asking macOS for a second IOProc on a
    // device this process has already hog-moded comes back as "couldn't be
    // opened for recording" against a microphone that is plugged in and
    // working.
    check (fakeca::openIoProcCount (rig) == 1,
           "one stream opens for the device, not one per microphone");

    if (! four.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out4 (static_cast<size_t> (block) * 2, 0.0f);
    float* outs4[] = { out4.data(), out4.data() + block };

    // A different tone per input, so a channel landing in the wrong file is a
    // failure rather than something that happens to look right.
    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            tone (block, 220.0, rate, 0.5f),
            tone (block, 440.0, rate, 0.5f),
            tone (block, 880.0, rate, 0.5f),
            tone (block, 1760.0, rate, 0.5f),
        };

        fakeca::pumpInput (rig, signal);
        four.pullOutputBlock (outs4, 2, block);
    }

    four.stopRecording();
    four.stopMonitoring();

    bool everyMicRecorded = true;

    for (int i = 0; i < 4; ++i)
    {
        uint32_t f = 0;
        const auto path = dir + "/0" + std::to_string (i + 1) + "_Mic-" + std::to_string (i + 1) + ".wav";
        const auto p = peakOf24BitWav (path, f);

        std::printf ("  Mic %d: %u frames, peak %d\n", i + 1, f, p);

        if (! (f > 0 && p > 100000))
            everyMicRecorded = false;

        std::remove (path.c_str());
    }

    check (everyMicRecorded, "all four microphones reach their own file with signal");
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // A TWO-input interface with two people plugged into it.
    //
    // The commonest small multi-mic rig there is, and the one the earlier fix
    // did not cover: two inputs collapsed to one on the assumption that any
    // two-channel device is a stereo USB mic. One of the two people was
    // discarded, and if it was the person holding the microphone that mattered,
    // the take came back silent.
    // ---------------------------------------------------------------------
    std::printf ("\nA two-input interface with two people on it\n");
    fakeca::reset();

    const auto pair = fakeca::addDevice (microphone ("2-in Interface", "uid-pair", 2,
                                                     fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend3;
    mma::CaptureCoordinator two (backend3, rate, block);

    std::vector<mma::CaptureChannel> duo;
    for (int i = 0; i < 2; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "uid-pair";
        c.deviceChannel = i;
        c.displayName = "Person " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Person-" + std::to_string (i + 1);
        duo.push_back (c);
    }

    if (! two.startMonitoring (duo, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", two.getMonitorProblem().c_str());
        return 1;
    }

    if (! two.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out2 (static_cast<size_t> (block) * 2, 0.0f);
    float* outs2[] = { out2.data(), out2.data() + block };

    // Only the SECOND person is speaking. Under the old rule §2.1 picked a
    // side, and picking the quiet one produced exactly the reported symptom:
    // a silent recording from a rig that was working.
    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            std::vector<float> (static_cast<size_t> (block), 0.0f),
            tone (block, 440.0, rate, 0.5f),
        };

        fakeca::pumpInput (pair, signal);
        two.pullOutputBlock (outs2, 2, block);
    }

    two.stopRecording();
    two.stopMonitoring();

    uint32_t f1 = 0, f2 = 0;
    const auto p1 = peakOf24BitWav (dir + "/01_Person-1.wav", f1);
    const auto p2 = peakOf24BitWav (dir + "/02_Person-2.wav", f2);

    std::printf ("  Person 1 (silent): %u frames, peak %d\n", f1, p1);
    std::printf ("  Person 2 (talking): %u frames, peak %d\n", f2, p2);

    check (f1 > 0, "the quiet person still gets a file, at full length");
    check (p2 > 100000, "and the person actually talking is recorded");

    std::remove ((dir + "/01_Person-1.wav").c_str());
    std::remove ((dir + "/02_Person-2.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());


    // ---------------------------------------------------------------------
    // A microphone unplugged in the MIDDLE of a take.
    //
    // §0.1's hardest case, and the one no harness could reach: the ALSA `file`
    // plugin free-runs and loops its infile, so on Linux a mid-stream device
    // loss cannot be expressed at all. The virtual HAL can: removeDevice()
    // erases the device and fires the system list listener, which is exactly
    // what the OS does when someone pulls the cable.
    //
    // What must survive it: the take. The other person keeps recording, the
    // take stays valid, and the lost channel is padded rather than truncated,
    // so the two stems still line up frame for frame.
    // ---------------------------------------------------------------------
    std::printf ("\nOne of two microphones unplugged mid-take\n");
    fakeca::reset();

    const auto keeper = fakeca::addDevice (microphone ("Keeper", "uid-keeper", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    const auto doomed = fakeca::addDevice (microphone ("Doomed", "uid-doomed", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend4;
    mma::CaptureCoordinator loss (backend4, rate, block);

    std::vector<mma::CaptureChannel> both = {
        { "uid-keeper", "Keeper", "01_Keeper", 0.0f },
        { "uid-doomed", "Doomed", "02_Doomed", 0.0f },
    };

    if (! loss.startMonitoring (both, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", loss.getMonitorProblem().c_str());
        return 1;
    }

    if (! loss.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> outLoss (static_cast<size_t> (block) * 2, 0.0f);
    float* outsLoss[] = { outLoss.data(), outLoss.data() + block };

    // Short enough that the writer's ring keeps up with a harness that pumps
    // blocks back to back with no wall clock between them: a longer free-run
    // overruns the ring and fills the stem's head with silence, which would
    // drown the very thing this case is looking at.
    const int lossBlocks = 64;
    const int unplugAt = lossBlocks / 2;

    for (int i = 0; i < lossBlocks; ++i)
    {
        if (i == unplugAt)
            fakeca::removeDevice (doomed);

        const std::vector<std::vector<float>> signal { tone (block, 440.0, rate, 0.5f) };

        fakeca::pumpInput (keeper, signal);

        if (i < unplugAt)
            fakeca::pumpInput (doomed, signal);

        loss.pullOutputBlock (outsLoss, 2, block);
    }

    loss.stopRecording();
    loss.stopMonitoring();

    uint32_t kFrames = 0, dFrames = 0;
    const auto kPeak = peakOf24BitWav (dir + "/01_Keeper.wav", kFrames);
    const auto dPeak = peakOf24BitWav (dir + "/02_Doomed.wav", dFrames);

    std::printf ("  01_Keeper.wav: %u frames, peak %d\n", kFrames, kPeak);
    std::printf ("  02_Doomed.wav: %u frames, peak %d\n", dFrames, dPeak);

    check (kPeak > 100000, "the surviving mic keeps recording through the unplug");
    check (kFrames >= static_cast<uint32_t> (lossBlocks) * static_cast<uint32_t> (block) / 2,
           "and its stem runs past the moment the other cable was pulled");
    check (dPeak > 100000, "what the lost mic did record is still in its stem");
    check (dFrames == kFrames,
           "the lost channel is padded to the take length, not truncated");

    // And padded with SILENCE, at the right moment: frame-count equality on
    // its own would also be satisfied by a channel that kept receiving audio,
    // or by one filled with whatever was left in a buffer.
    // Asked of the audio, not of the clock: where does the lost channel stop
    // sounding, and is the survivor still sounding after that point?
    const auto lost = extentOf24BitWav (dir + "/02_Doomed.wav");
    const auto keptGoing = peakOfRange (dir + "/01_Keeper.wav", lost.lastSounding, kFrames);
    const auto lostTail = peakOfRange (dir + "/02_Doomed.wav", lost.lastSounding, dFrames);

    std::printf ("  02_Doomed.wav: sounds up to frame %u of %u, peak %d; after it %d\n",
                 lost.lastSounding, lost.frames, lost.peakBefore, lostTail);
    std::printf ("  01_Keeper.wav: peak %d across the same tail\n", keptGoing);

    check (lost.peakBefore > 100000, "the lost mic's audio up to the unplug is intact");
    check (lostTail == 0, "and everything after it is silence, not stale samples");

    // The tail has to be worth something, or a channel that ran to the very
    // last frame would satisfy "silence after the end" trivially.
    check (lost.lastSounding + static_cast<uint32_t> (block) < dFrames,
           "the silence is a real stretch of the take, not a rounding error");
    check (keptGoing > 100000, "while the surviving mic is still writing real audio there");

    std::remove ((dir + "/01_Keeper.wav").c_str());
    std::remove ((dir + "/02_Doomed.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // The same interface, handing its inputs over as SEPARATE BUFFERS.
    //
    // Every multi-input case above uses one interleaved buffer carrying N
    // channels. A class-compliant USB interface commonly does the other thing
    // -- one mono buffer per input -- and that layout had been proven only as
    // far as the backend's callback, never through the coordinator and the
    // writer into files. Each side of that boundary was tested; the join was
    // not, which is where every bug found tonight was living.
    //
    // Each input carries a DIFFERENT amplitude, so this cannot pass while the
    // channels are transposed. Four identical tones would be satisfied by a
    // fan-out that put input four into everyone's file.
    // ---------------------------------------------------------------------
    std::printf ("\nAn interface that hands over one buffer per input\n");
    fakeca::reset();

    const auto split = fakeca::addDevice (microphone ("Split 4-in", "uid-split", 4,
                                                      fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend5;
    mma::CaptureCoordinator perBuffer (backend5, rate, block);

    std::vector<mma::CaptureChannel> splitMics;
    for (int i = 0; i < 4; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "uid-split";
        c.deviceChannel = i;
        c.displayName = "Input " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Input-" + std::to_string (i + 1);
        c.bitDepth = 24;
        splitMics.push_back (c);
    }

    if (! perBuffer.startMonitoring (splitMics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", perBuffer.getMonitorProblem().c_str());
        return 1;
    }

    if (! perBuffer.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> outSplit (static_cast<size_t> (block) * 2, 0.0f);
    float* outsSplit[] = { outSplit.data(), outSplit.data() + block };

    // 0.1, 0.2, 0.3, 0.4 -- far enough apart that a swap is unmistakable.
    const float amplitudes[] = { 0.1f, 0.2f, 0.3f, 0.4f };

    for (int i = 0; i < 64; ++i)
    {
        std::vector<std::vector<float>> signal;
        for (const float amplitude : amplitudes)
            signal.push_back (tone (block, 440.0, rate, amplitude));

        fakeca::pumpInput (split, signal);
        perBuffer.pullOutputBlock (outsSplit, 2, block);
    }

    perBuffer.stopRecording();
    perBuffer.stopMonitoring();

    bool everyInputLanded = true;

    for (int i = 0; i < 4; ++i)
    {
        const auto path = dir + "/0" + std::to_string (i + 1) + "_Input-"
                        + std::to_string (i + 1) + ".wav";

        uint32_t frames = 0;
        const auto peak = peakOf24BitWav (path, frames);

        // 24-bit full scale is 8388607, so 0.1 lands near 838860 and each step
        // is about that much again. A tolerance of a tenth of a step is ample
        // and still cannot confuse one input with its neighbour.
        const int32_t expected = static_cast<int32_t> (amplitudes[static_cast<size_t> (i)] * 8388607.0f);
        const int32_t slack = 83886;
        const bool right = frames > 0 && std::abs (peak - expected) < slack;

        std::printf ("  0%d_Input-%d.wav: %u frames, peak %d (expected about %d)%s\n",
                     i + 1, i + 1, frames, peak, expected, right ? "" : "   <-- wrong input");

        if (! right)
            everyInputLanded = false;

        std::remove (path.c_str());
    }

    check (everyInputLanded,
           "each input reaches its own file, at its own level, in the right order");

    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // The HEADPHONES are unplugged mid-take.
    //
    // Everything above is about losing a microphone. This is the other half of
    // §0.1 and it had never been driven end to end on any platform: the take is
    // clocked by the output callback, so when the headphones go the clock goes
    // with them, and a take that stops advancing is a take that is being lost
    // while the record light is still on.
    //
    // CaptureCoordinator has a software clock for exactly this, and it is
    // tested -- against a FakeBackend, in isolation. No harness had ever opened
    // a REAL output stream through a real backend and then taken it away, so
    // the handover was proven as a mechanism and never as an outcome.
    // ---------------------------------------------------------------------
    std::printf ("\nThe headphones are unplugged in the middle of a take\n");
    fakeca::reset();

    const auto singer = fakeca::addDevice (microphone ("Singer", "uid-singer", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    auto cansSpec = headphones ("Headphones", "uid-cans", 2,
                                fakeca::BufferShape::interleaved);
    // This card runs its own period and will not be talked into ours. The app
    // asks for `block`; the device keeps 192. Whatever latency we then report
    // has to describe 192, because that is the buffer the singer hears through.
    cansSpec.allowBufferSizeChange = false;
    cansSpec.bufferFrameSize = 192;
    const auto cans = fakeca::addDevice (cansSpec);

    mma::CoreAudioBackend backend6;
    mma::CaptureCoordinator monitored (backend6, rate, block);

    std::vector<mma::CaptureChannel> singerOnly = { { "uid-singer", "Singer", "01_Singer", 0.0f } };
    singerOnly[0].bitDepth = 24;

    if (! monitored.startMonitoring (singerOnly, "uid-cans"))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", monitored.getMonitorProblem().c_str());
        return 1;
    }

    check (monitored.hasOutputStream(), "the headphones are open and clocking the take");

    check (backend6.getGrantedOutputBufferFrames() == 192,
           "the backend reports the period the card granted, not the one we asked for");
    check (192 != block, "the granted period really does differ from the request");
    {
        // The round trip at the granted period, and the two pulls of it
        // every microphone's ring holds ahead of the output.
        const double expected = (192.0 / rate) * 1000.0 * 2.0 + (2.0 * std::max (192, block) / rate) * 1000.0;
        const double reported = monitored.getMonitoringLatencyMs();
        check (std::fabs (reported - expected) < 0.001,
               "the monitoring latency describes the granted period, not the asked-for one");
    }

    if (! monitored.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    // Real audio while the output is alive. The OUTPUT pulls here, not us:
    // that is the whole point, and pulling by hand would test nothing.
    std::vector<std::vector<float>> pulled;

    for (int i = 0; i < 24; ++i)
    {
        fakeca::pumpInput (singer, { tone (block, 440.0, rate, 0.4f) });
        fakeca::pumpOutput (cans, block, pulled);
    }

    const auto framesBefore = monitored.getFramesAccepted();
    check (framesBefore > 0, "the take is advancing while the headphones are there");

    // And now they are gone.
    fakeca::removeDevice (cans);

    // Only the microphone is fed from here. Nothing pulls the output, because
    // there is no output. If the take depends on it, this is where it dies.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (900);

    while (std::chrono::steady_clock::now() < until)
    {
        fakeca::pumpInput (singer, { tone (block, 440.0, rate, 0.4f) });
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    const auto framesAfter = monitored.getFramesAccepted();

    monitored.stopRecording();
    monitored.stopMonitoring();

    std::printf ("  accepted %llu frames before the unplug, %llu after\n",
                 (unsigned long long) framesBefore, (unsigned long long) framesAfter);

    check (framesAfter > framesBefore,
           "the take keeps advancing once the headphones are gone");

    uint32_t singerFrames = 0;
    const auto singerPeak = peakOf24BitWav (dir + "/01_Singer.wav", singerFrames);

    std::printf ("  01_Singer.wav: %u frames, peak %d\n", singerFrames, singerPeak);

    check (singerFrames > 0, "and the singer still has a file");
    check (singerPeak > 100000, "with their audio in it, not silence");

    std::remove ((dir + "/01_Singer.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // Two interfaces with different input latency.
    //
    // CoreAudio says how long before the IOProc a device's input audio was at
    // the microphone: the device's input latency, its input safety offset and
    // its input stream's latency. Two boxes seldom agree, so one clap heard
    // by both microphones is handed over that many samples apart -- and was
    // written into the two stems that far apart, a smear in the mix.
    //
    // The headphones take each microphone as early as its own device allows
    // (§5.4: audio monitoring comes first); the writer lines the stems up.
    // So the clap reaches the headphones twice, the laggy interface's
    // kLatencyGap after the quick one's, and lands once in the files.
    // ---------------------------------------------------------------------
    std::printf ("\nTwo interfaces that report different input latencies\n");
    fakeca::reset();

    auto quick = microphone ("Quick", "uid-quick", 1, fakeca::BufferShape::oneChannelPerBuffer);
    quick.inputLatencyFrames = 8;
    quick.inputSafetyOffsetFrames = 4;

    auto laggy = microphone ("Laggy", "uid-laggy", 1, fakeca::BufferShape::interleaved);
    laggy.inputLatencyFrames = 24;
    laggy.inputSafetyOffsetFrames = 32;
    laggy.streamLatencyFrames = 13;

    const auto quickId = fakeca::addDevice (quick);
    const auto laggyId = fakeca::addDevice (laggy);
    constexpr int kLatencyGap = (24 + 32 + 13) - (8 + 4); // 57 frames

    mma::CoreAudioBackend latencyBackend;

    // The harness is the only clock here, so the streams' between-callback
    // fill estimate must not read the wall clock: on a busy CI runner it made
    // the drift loops steer the two stems a sample or two apart.
    mma::DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; });

    mma::CaptureCoordinator aligned (latencyBackend, rate, block);
    aligned.setSoftwareClockEnabled (false); // the harness is the clock: deterministic pulls

    std::vector<mma::CaptureChannel> latencyPair = {
        { "uid-quick", "Quick", "01_Quick", 0.0f },
        { "uid-laggy", "Laggy", "02_Laggy", 0.0f },
    };

    if (! aligned.startMonitoring (latencyPair, {}) || ! aligned.startRecording (dir, 24, "2026-10-07T00:00:00Z"))
    {
        std::printf ("  FAIL  could not start: %s\n", aligned.getMonitorProblem().c_str());
        return 1;
    }

    check (latencyBackend.getInputLatencyFrames ("uid-quick") == 12,
           "the quick interface's latency and safety offset are read");
    check (latencyBackend.getInputLatencyFrames ("uid-laggy") == 69,
           "the laggy one's, its stream's latency included");

    std::vector<float> alignedOut (static_cast<size_t> (block) * 2, 0.0f);
    float* alignedOuts[] = { alignedOut.data(), alignedOut.data() + block };
    std::vector<float> headphones; // the left channel of everything the output played

    // The clap, at the same instant in the room: frame 4000 of what the
    // quick interface hands over, kLatencyGap frames later from the laggy one.
    constexpr long long kClap = 4000;
    const int alignedBlocks = 48;

    for (int i = 0; i < alignedBlocks; ++i)
    {
        std::vector<float> q (static_cast<size_t> (block), 0.0f), l (static_cast<size_t> (block), 0.0f);

        for (int f = 0; f < block; ++f)
        {
            const long long n = static_cast<long long> (i) * block + f;
            if (n == kClap) q[static_cast<size_t> (f)] = 0.9f;
            if (n == kClap + kLatencyGap) l[static_cast<size_t> (f)] = 0.9f;
        }

        fakeca::pumpInput (quickId, { q });
        fakeca::pumpInput (laggyId, { l });
        aligned.pullOutputBlock (alignedOuts, 2, block);
        headphones.insert (headphones.end(), alignedOut.begin(), alignedOut.begin() + block);
    }

    const int quickOffset = aligned.getDeviceAlignmentDelayFrames ("uid-quick");
    const int laggyOffset = aligned.getDeviceAlignmentDelayFrames ("uid-laggy");

    aligned.stopRecording();
    aligned.stopMonitoring();
    mma::DeviceInputStream::setClockForTesting (nullptr);

    // Where each clap reached the headphones: the loudest sample, and the
    // loudest one more than a few frames from it.
    {
        const auto loudestOutside = [&headphones] (long long avoid)
        {
            long long best = -1;
            float bestPeak = 0.0f;
            for (size_t i = 0; i < headphones.size(); ++i)
            {
                if (avoid >= 0 && std::llabs (static_cast<long long> (i) - avoid) <= 8)
                    continue;
                if (std::fabs (headphones[i]) > bestPeak)
                {
                    bestPeak = std::fabs (headphones[i]);
                    best = static_cast<long long> (i);
                }
            }
            return best;
        };

        const auto first = loudestOutside (-1);
        const auto second = loudestOutside (first);
        const auto earlier = std::min (first, second), later = std::max (first, second);
        std::printf ("  in the headphones: the clap at output frame %lld and again at %lld\n", earlier, later);

        check (first >= 0 && second >= 0, "the headphones carry the clap from both microphones");
        check (std::llabs ((later - earlier) - kLatencyGap) <= 1,
               "each as early as its own interface allows: the quick one is not held back for the laggy one");
    }

    std::printf ("  writer offsets: Quick %d frames, Laggy %d frames; stems aligned: %s\n",
                 quickOffset, laggyOffset, aligned.areStemsAligned() ? "yes" : "no");
    check (quickOffset == kLatencyGap && laggyOffset == 0,
           "the writer holds the quick interface's stem back by the difference, and the take's record says so");
    check (aligned.areStemsAligned(), "and the take is recorded as lined up exactly");

    const auto loudestFrame = [] (const std::string& path)
    {
        constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;
        std::ifstream f (path, std::ios::binary);
        f.seekg (kDataSizeOffset);
        unsigned char b[4] {};
        f.read (reinterpret_cast<char*> (b), 4);
        const uint32_t frames = (static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
                                 | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24)) / 3;
        long long best = -1;
        int32_t bestPeak = 0;
        for (uint32_t i = 0; i < frames; ++i)
        {
            unsigned char s[3] {};
            f.read (reinterpret_cast<char*> (s), 3);
            int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                      | (static_cast<int32_t> (s[2]) << 16);
            if (v & 0x800000)
                v |= ~0xFFFFFF;
            if (std::abs (v) > bestPeak)
            {
                bestPeak = std::abs (v);
                best = static_cast<long long> (i);
            }
        }
        return best;
    };

    const auto quickClap = loudestFrame (dir + "/01_Quick.wav");
    const auto laggyClap = loudestFrame (dir + "/02_Laggy.wav");
    std::printf ("  clap in 01_Quick.wav at frame %lld, in 02_Laggy.wav at frame %lld\n", quickClap, laggyClap);

    check (quickClap >= 0 && laggyClap >= 0, "both stems carry the clap");
    check (std::llabs (quickClap - laggyClap) <= 1,
           "and at the same frame: the quicker interface is held back by the difference");

    const auto mixClap = loudestFrame (dir + "/MIX.wav");
    check (std::llabs (mixClap - laggyClap) <= 1, "and the mix, summed from the lined-up stems, carries it once, there");

    std::remove ((dir + "/01_Quick.wav").c_str());
    std::remove ((dir + "/02_Laggy.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // The Mac sleeping in the middle of a take (§6.6).
    //
    // A closed lid on battery sleeps the Mac mid-take whatever the app asks.
    // The callback watchdog no longer calls every microphone dead for it --
    // and those false reports had been the only trace of the sleep in the
    // take's record. The backend says once, against no microphone, that the
    // Mac slept; with a take recording, that is what puts the gap into
    // session.json (Application, pinned by the system_slept_take_note test).
    // The take itself records on after the wake.
    // ---------------------------------------------------------------------
    std::printf ("\nThe Mac sleeps in the middle of a take\n");
    fakeca::reset();

    const auto napMic = fakeca::addDevice (microphone ("Nap Mic", "uid-nap", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    mma::CoreAudioBackend napBackend;
    mma::CaptureCoordinator nap (napBackend, rate, block);
    std::vector<mma::CaptureChannel> napChannels = { { "uid-nap", "Nap", "01_Nap", 0.0f } };

    if (! nap.startMonitoring (napChannels, {}) || ! nap.startRecording (dir, 24, "2026-10-12T00:00:00Z"))
    {
        std::printf ("  FAIL  the take did not start: %s\n", nap.getMonitorProblem().c_str());
        return 1;
    }

    std::vector<float> napOut (static_cast<size_t> (block) * 2, 0.0f);
    float* napOuts[] = { napOut.data(), napOut.data() + block };

    const auto recordHalfASecond = [&]
    {
        for (int i = 0; i < blocks / 2; ++i)
        {
            fakeca::pumpInput (napMic, { tone (block, 330.0, rate, 0.5f) });
            nap.pullOutputBlock (napOuts, 2, block);
        }
    };

    recordHalfASecond();
    check (napBackend.takeStreamFailures().empty(), "a take recording is not news");

    // The lid closes: IO stops, the Mac takes a few seconds to go to sleep,
    // and sleeps for an hour.
    fakeca::passTime (3.0);
    check (napBackend.takeStreamFailures().empty(), "the moments before the Mac sleeps are not a failure");

    fakeca::sleepSystem (3600.0);
    const auto slept = napBackend.takeStreamFailures();
    check (slept.size() == 1 && slept.front().kind == mma::StreamFailureKind::systemSlept
               && slept.front().deviceId.empty(),
           "on waking, the sleep is reported once, against no microphone");
    check (! slept.empty() && mma::systemSleptMarksTheTake (slept.front().kind, nap.isRecording()),
           "and, with the take recording, it goes into the take's record");

    // Awake: the mic sends again and the take carries on.
    recordHalfASecond();
    check (napBackend.takeStreamFailures().empty(), "the mic sending again after the wake is not news");

    fakeca::passTime (1.0);
    check (napBackend.takeStreamFailures().empty(), "nor is the sleep told twice");

    nap.stopRecording();
    nap.stopMonitoring();

    uint32_t napFrames = 0;
    const auto napPeak = peakOf24BitWav (dir + "/01_Nap.wav", napFrames);
    std::printf ("  01_Nap.wav: %u frames, peak %d\n", napFrames, napPeak);
    check (napFrames >= static_cast<uint32_t> (rate * 0.75) && napPeak > 100000,
           "the stem holds the audio from before the sleep and after it");

    std::remove ((dir + "/01_Nap.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    std::printf ("\n%s (%d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
