// Runs the real CoreAudioBackend against a virtual CoreAudio HAL.
//
// The backend source is compiled unmodified; only the OS headers are replaced
// (Simulation/CoreAudio). Every scenario here is a shape of real hardware that
// the backend once got wrong, or that it must keep getting right.

#include "../Simulation/CoreAudio/FakeCoreAudio.h"
#include "../Source/Core/HeadphoneRouting.h"
#include "../Source/Core/OutputDeviceSelector.h"
#include "../Source/Core/SampleFormat.h"
#include "../Source/Platform/CoreAudioBackend.h"
#include "../Source/Platform/MacCoreAudioQueries.h"
#include "../Source/Platform/SystemAggregateDevice.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <memory>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

// ---------------------------------------------------------------------------
// The timing budgets these tests run on, as one stated relationship rather
// than the same magic numbers repeated at a dozen call sites.
//
// Every "cannot freeze launch" check answers one question: when a HAL call
// hangs, does the caller bound itself, or does it wait for the hang? It can
// only answer that if the two outcomes are far apart. They were not. The stall
// was 250 ms and the budget 200 ms, so a correct answer and a wrong one were
// 50 ms apart -- and the macOS runners add well over 100 ms of scheduling
// latency of their own. The same closeAllStreams call measured 94 ms, then
// 144 ms, then 162 ms across three runs of one commit; the third crossed the
// budget and failed a release build on a commit that had just passed CI.
//
// So the stall moved, not the budget alone. A bounded call returns in tens of
// milliseconds and is allowed the better part of a second; a call that really
// waits takes two full seconds. Nothing in between is ambiguous.
// ---------------------------------------------------------------------------

/// How long a simulated HAL call hangs for.
constexpr int kStuckHalMilliseconds = 2000;

/// What a correctly bounded call may take. kHalTransactionTimeout is 250 ms in
/// simulation, so this is roughly five times the worst latency yet seen on a
/// loaded runner, and still well under half the stall above.
constexpr auto kBoundedReturn = std::chrono::milliseconds (800);

/// A call refused without touching the HAL does no waiting at all; this only
/// has to stay far below the stall.
constexpr auto kNoHalRoundTrip = std::chrono::milliseconds (200);

/// Waiting for the worker that owns a stuck HAL call through to completion.
/// It cannot finish before the stall does, so this must exceed it.
constexpr int kWorkerSettleMilliseconds = 6000;

void check (bool condition, const std::string& what)
{
    ++checks;
    if (condition)
    {
        std::printf ("  PASS  %s\n", what.c_str());
    }
    else
    {
        ++failures;
        std::printf ("  FAIL  %s\n", what.c_str());
    }
}

std::vector<float> ramp (int frames, float scale)
{
    std::vector<float> v (static_cast<size_t> (frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t> (i)] = scale * static_cast<float> (i);
    return v;
}

/// Captures whatever the backend hands its AudioCallback, so a scenario can
/// assert on the audio rather than on the absence of a crash.
struct Capture
{
    int callbackCount = 0;
    int lastChannelCount = 0;
    int lastFrameCount = 0;
    std::vector<std::vector<float>> lastBlock;

    mma::AudioCallback callback()
    {
        return [this] (const float* const* inputs, int numInputs,
                       float* const*, int, int numSamples)
        {
            ++callbackCount;
            lastChannelCount = numInputs;
            lastFrameCount = numSamples;
            lastBlock.assign (static_cast<size_t> (numInputs), {});

            for (int ch = 0; ch < numInputs; ++ch)
                if (inputs[ch] != nullptr)
                    lastBlock[static_cast<size_t> (ch)].assign (inputs[ch], inputs[ch] + numSamples);
        };
    }
};

fakeca::DeviceSpec microphone (const std::string& name, const std::string& uid,
                               int channels, fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.inputChannels = channels;
    spec.shape = shape;
    return spec;
}

fakeca::DeviceSpec headphones (const std::string& name, const std::string& uid,
                               int channels, fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.outputChannels = channels;
    spec.shape = shape;
    return spec;
}

// --- Scenarios --------------------------------------------------------------

/// The defect that made a stereo USB microphone record silence: the IOProc
/// discarded any buffer that was not exactly one channel, so the callback never
/// fired at all and nothing reported an error.
void interleavedStereoMicrophoneDeliversBothChannels()
{
    std::printf ("\nA stereo USB mic that hands over one interleaved buffer\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Interleaved Mic", "uid-interleaved", 2,
                                                   fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-interleaved", 48000.0, 256, capture.callback()),
           "the stream opens");
    check (fakeca::isRunning (id), "the IOProc is started");

    const std::vector<std::vector<float>> signal { ramp (128, 1.0f), ramp (128, -1.0f) };
    fakeca::pumpInput (id, signal);

    check (capture.callbackCount == 1, "the audio callback actually fires");
    check (capture.lastChannelCount == 2, "both channels reach the callback");
    check (capture.lastFrameCount == 128, "the frame count survives de-interleaving");

    bool exact = capture.lastChannelCount == 2;
    for (int ch = 0; ch < capture.lastChannelCount && exact; ++ch)
        exact = capture.lastBlock[static_cast<size_t> (ch)] == signal[static_cast<size_t> (ch)];

    check (exact, "each channel carries its own samples, not the other's");
    backend.closeAllStreams();
}

/// The shape the backend always handled. Kept so a fix for the interleaved case
/// cannot quietly break the one that used to work.
void oneChannelPerBufferStillWorks()
{
    std::printf ("\nA mic that hands over one buffer per channel\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Split Mic", "uid-split", 2,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-split", 48000.0, 256, capture.callback()), "the stream opens");

    const std::vector<std::vector<float>> signal { ramp (64, 2.0f), ramp (64, 3.0f) };
    fakeca::pumpInput (id, signal);

    check (capture.lastChannelCount == 2, "both channels reach the callback");
    check (capture.lastBlock == signal, "the samples pass through unchanged");
    backend.closeAllStreams();
}

/// Playback had the same defect, and it is the half a recording test would
/// never catch: the monitor mix would simply be silent.
void interleavedOutputCarriesTheMonitorMix()
{
    std::printf ("\nAn interface whose output is one interleaved buffer\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (headphones ("Interleaved Out", "uid-out", 2,
                                                   fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;

    // Writes a different constant per channel, so a packing error shows up as
    // the wrong value in the wrong place rather than as silence.
    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = 0.25f * static_cast<float> (ch + 1);
    };

    check (backend.openExclusiveOutputStream ("uid-out", 48000.0, 256, writer), "the output stream opens");

    std::vector<std::vector<float>> written;
    check (fakeca::pumpOutput (id, 96, written), "the IOProc runs");
    check (written.size() == 2, "both output channels are written");

    bool correct = written.size() == 2;
    for (size_t ch = 0; ch < written.size() && correct; ++ch)
        for (float v : written[ch])
            if (std::fabs (v - 0.25f * static_cast<float> (ch + 1)) > 1.0e-6f)
            {
                correct = false;
                break;
            }

    check (correct, "each channel gets its own signal, correctly re-interleaved");
    backend.closeAllStreams();
}

/// A device with a sample-rate converter advertises one continuous range rather
/// than a list. Reading only its maximum hid every rate in between, so §2.2
/// negotiation would route around a device that supports 48 kHz perfectly well.
void continuousSampleRateRangeIsExpanded()
{
    std::printf ("\nA device advertising a continuous 44.1-96 kHz range\n");
    fakeca::reset();

    auto spec = microphone ("Range Mic", "uid-range", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = { { 44100.0, 96000.0 } };
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    check (devices.size() == 1, "the device enumerates");

    if (devices.empty())
        return;

    const auto& rates = devices.front().supportedSampleRates;
    auto has = [&rates] (uint32_t r) { return std::find (rates.begin(), rates.end(), r) != rates.end(); };

    check (has (44100) && has (48000) && has (88200) && has (96000),
           "every standard rate inside the range is reported");
    check (! has (192000), "a rate outside the range is not");

    // §2.2 stays on the rate the hardware is already using, and it can only do
    // that if the backend says what that rate is. Nothing checked this, so a
    // field left at 0 would have silently reverted the whole rule to
    // highest-common -- which is exactly how a fix for a 44.1 kHz interface
    // shipped unable to fire on it.
    std::printf ("  current rate reported: %u Hz\n", devices.front().currentSampleRate);
    check (devices.front().currentSampleRate == 48000,
           "and the rate the device is running at RIGHT NOW is reported, not left at 0");
}

/// Another process holding the device makes the rate write fail even when the
/// device already runs at exactly the rate we want. Treating that as fatal
/// turned a working microphone into one that would not open.
void aDeviceAlreadyAtTheRequestedRateStillOpens()
{
    std::printf ("\nA mic whose rate is fixed at the rate we want anyway\n");
    fakeca::reset();

    auto spec = microphone ("Fixed Mic", "uid-fixed", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowRateChange = false;
    spec.currentRate = 48000.0;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-fixed", 48000.0, 256, capture.callback()),
           "the stream opens despite the refused write");
    check (fakeca::isRunning (id), "and actually starts");
    backend.closeAllStreams();
}

/// CoreAudio acknowledges a nominal-rate write before the new value is always
/// visible. The backend must wait for that asynchronous change rather than
/// rejecting a device on the first stale read-back.
void aDelayedRateChangeSettlesBeforeTheStreamOpens()
{
    std::printf ("\nA nominal-rate change that CoreAudio applies asynchronously\n");
    fakeca::reset();

    auto spec = microphone ("Slow Rate Mic", "uid-slow-rate", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    // One stale read, not three.
    //
    // The claim here is that a rate the HAL applies asynchronously is waited
    // for rather than read back once and treated as a refusal -- and a single
    // stale read proves exactly that: the first read after the write still
    // returns 44.1, so an implementation without the confirmation poll still
    // fails this test. Three reads proved nothing further and cost three poll
    // intervals, which is what made this the one test that failed on macOS.
    //
    // The poll is 10 ms inside a 50 ms settle window, so five polls are
    // available -- and that window sits inside the 75 ms HAL transaction
    // deadline this was written against (now 250 ms), so the real budget was smaller still.
    // Needing four of those five left nothing for a sleep that overshoots,
    // which is what sleep_until does on the macOS runners: the open was
    // refused at 51 ms with the correct rate one read away. Widening the
    // settle window is not the fix -- it has to stay inside the transaction
    // that contains it, the way production's 500 ms sits inside 1000 ms.
    spec.rateChangeDelayReads = 1;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    // Measured, not just asserted. This test is the one that fails on macOS and
    // passes everywhere else, and a bare FAIL line says nothing about which of
    // the two bounded waits ran out -- the rate-settle poll or the HAL
    // transaction deadline around the whole open. The numbers separate them.
    const auto openBegan = std::chrono::steady_clock::now();
    const bool opened = backend.openInputStream ("uid-slow-rate", 48000.0, 256,
                                                 capture.callback());
    const auto openTook = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - openBegan);

    std::printf ("  [measured] the open took %lld ms; the device now reads %.0f Hz\n",
                 (long long) openTook.count(), fakeca::nominalRate (id));

    if (! opened)
        std::printf ("  [measured] refused with: %s\n", backend.getLastOpenError().c_str());

    check (opened, "the stream waits for the delayed rate and opens");
    check (fakeca::nominalRate (id) == 48000.0,
           "the stream starts only after the requested rate is visible");
    check (fakeca::isRunning (id), "the IOProc actually starts after confirmation");
    backend.closeAllStreams();
}

/// A broken driver can acknowledge the property write without applying it.
/// Waiting forever would hang launch, so the confirmation path has a 500 ms
/// ceiling and still refuses to open at the wrong rate when that expires.
void aRateChangeThatNeverSettlesTimesOut()
{
    std::printf ("\nA nominal-rate write that never becomes visible\n");
    fakeca::reset();

    auto spec = microphone ("Never Settles", "uid-never-settles", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    spec.rateChangeDelayReads = 100000;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    const auto started = std::chrono::steady_clock::now();
    const bool opened = backend.openInputStream ("uid-never-settles", 48000.0, 256,
                                                 capture.callback());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check (! opened, "the open is refused rather than using the stale rate");
    check (! fakeca::isRunning (id), "no IOProc starts at the wrong rate");

    // This device never applies the rate, so its worker keeps polling for the
    // whole settle window after the open has already given up. Waiting for it
    // here is not politeness: the next test calls fakeca::reset(), and the
    // harness has no lock, so a worker still reading a device while the next
    // test replaces it is a genuine race -- ThreadSanitizer reports it the
    // moment the settle window outlasts the gap between the two tests. Every
    // other test that abandons a worker already waits for it exactly here.
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the abandoned rate-confirmation worker settles before the next test");
    check (elapsed < std::chrono::milliseconds (650),
           "confirmation returns at its 500 ms bound instead of hanging launch");
}

/// The reported rig: an interface sitting at 44.1 kHz. The backend must say so,
/// because §2.2's whole "stay put" rule is built on that one number.
void aDeviceAt44100ReportsThatAsItsCurrentRate()
{
    std::printf ("\nAn interface running at 44.1 kHz\n");
    fakeca::reset();

    auto spec = microphone ("PUP Mixer", "uid-pup", 2, fakeca::BufferShape::interleaved);
    spec.rateRanges = { { 44100.0, 48000.0 } };
    spec.currentRate = 44100.0;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    check (devices.size() == 1, "the interface enumerates");

    if (devices.empty())
        return;

    std::printf ("  current rate reported: %u Hz\n", devices.front().currentSampleRate);
    check (devices.front().currentSampleRate == 44100,
           "and reports 44.1 kHz, which is what lets the take stay there");
}

/// The converse: a device that cannot reach the requested rate must fail rather
/// than run at the wrong one, which would be a silent, permanent drift source.
void aDeviceThatCannotReachTheRateIsRefused()
{
    std::printf ("\nA mic stuck at 44.1 kHz when 48 kHz was negotiated\n");
    fakeca::reset();

    auto spec = microphone ("Stuck Mic", "uid-stuck", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowRateChange = false;
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 } };
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("uid-stuck", 48000.0, 256, capture.callback()),
           "the open is refused rather than silently running at the wrong rate");

    // §0.1: refusing is only half the job. "Couldn't be opened for recording"
    // sends someone hunting through cables for a fault that is one number in a
    // settings pane, and the app knew the number the whole time.
    const auto reason = backend.getLastOpenError();
    std::printf ("  reason: %s\n", reason.c_str());

    check (! reason.empty(), "and says why, rather than leaving the user to guess");
    check (reason.find ("44.1 kHz") != std::string::npos,
           "naming the rate the interface is actually running at");
    check (reason.find ("48 kHz") != std::string::npos,
           "and the rate the recording wants");
}

/// A microphone unplugged between being listed and being opened. The message
/// has to say THAT, not offer a sample-rate lecture about a device that is no
/// longer there.
void aMicrophoneThatVanishedSaysSo()
{
    std::printf ("\nA mic that was unplugged between being listed and being opened\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("uid-gone", 48000.0, 256, capture.callback()),
           "the open is refused");

    const auto reason = backend.getLastOpenError();
    std::printf ("  reason: %s\n", reason.c_str());

    check (reason.find ("no longer connected") != std::string::npos,
           "and says the microphone is gone, not that some rate is wrong");
}

/// Pulled while the open waited for its rate change to settle. The rate read
/// then fails and comes back as 0, and the refusal used to read "This interface
/// is running at 0 kHz ... Set the recording to 0 kHz in Settings".
void aMicUnpluggedWhileItsRateSettlesSaysItDisconnected()
{
    std::printf ("\nA mic unplugged while its sample rate is being changed\n");
    fakeca::reset();

    auto spec = microphone ("Pulled Mic", "uid-pulled", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    spec.unpluggedDuringRateChange = true;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("uid-pulled", 48000.0, 256, capture.callback()),
           "the open is refused");

    const auto reason = backend.getLastOpenError();
    std::printf ("  reason: %s\n", reason.c_str());

    check (reason.find ("0 kHz") == std::string::npos,
           "and never tells the user to set anything to 0 kHz");
    check (reason.find ("disconnected") != std::string::npos,
           "but says the interface disconnected");
}

/// §5.4: the monitor path is exclusive or it is nothing. Reporting success
/// without hog mode handed the user a shared output while the app believed
/// otherwise.
void hogModeRefusalFailsTheOpenAndExplainsItself()
{
    std::printf ("\nAn output that will not grant exclusive use\n");
    fakeca::reset();

    auto spec = headphones ("Shared Out", "uid-shared", 2, fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowHogMode = false;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;

    const auto capability = backend.checkExclusiveModeCapability ("uid-shared", 48000.0, 256);
    check (! capability.exclusiveModeAvailable, "the preflight reports it as unavailable");
    check (! capability.unavailableReason.empty(), "and names a cause");

    check (! backend.openExclusiveOutputStream ("uid-shared", 48000.0, 256,
                                                [] (const float* const*, int, float* const*, int, int) {}),
           "the open fails rather than claiming an exclusive path");
    check (! backend.getLastOpenError().empty(), "and leaves a message naming a next step");
    check (! fakeca::isRunning (id), "no IOProc is left running behind the failure");
}

/// The normal case, and the one that proves the hog-mode check is a gate rather
/// than a blanket refusal.
void hogModeIsTakenAndReleased()
{
    std::printf ("\nAn output that does grant exclusive use\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (headphones ("Exclusive Out", "uid-excl", 2,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;

    check (backend.openExclusiveOutputStream ("uid-excl", 48000.0, 256,
                                              [] (const float* const*, int, float* const*, int, int) {}),
           "the open succeeds");
    check (fakeca::hogModeHeld (id), "hog mode is actually held while open");

    // "Somebody holds it" is not the §5.4 claim. Exclusive monitoring means
    // THIS process holds it, and a backend that wrote a bogus or stale pid
    // would satisfy the line above while owning nothing it could release.
    check (fakeca::hogOwnerPid (id) == static_cast<int> (getpid()),
           "and it is this process that holds it, not merely someone");

    backend.closeAllStreams();
    check (! fakeca::hogModeHeld (id), "and released on close, so other apps get the device back");
    check (fakeca::hogOwnerPid (id) == -1, "with the owner cleared, not just changed");
}

/// Hog mode is per device, not per direction. Taken on a microphone's own
/// headphone jack it locked the microphone too, and Zoom or OBS recording from
/// it -- or from the combined device it belongs to -- lost it. A plain output
/// is still taken exclusively, so the rule is a carve-out, not a retreat.
void hogModeIsNotTakenOnAMicrophonesOwnOutput()
{
    std::printf ("\nHog mode on an output that is also a microphone\n");
    fakeca::reset();

    auto yeti = microphone ("Yeti", "uid-yeti", 2, fakeca::BufferShape::interleaved);
    yeti.outputChannels = 2;
    const auto yetiId = fakeca::addDevice (yeti);
    const auto plainId = fakeca::addDevice (headphones ("Interface Out", "uid-plain", 2,
                                                        fakeca::BufferShape::oneChannelPerBuffer));

    // An output-only endpoint the combined device lists as a sub-device: the
    // inputs check alone would miss it.
    const auto jackId = fakeca::addDevice (headphones ("Mic Jack", "uid-jack", 2,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    fakeca::DeviceSpec combined;
    combined.name = "SobStage";
    combined.uid = mma::kOurAggregateUid;
    combined.inputChannels = 2;
    combined.transportType = kAudioDeviceTransportTypeAggregate;
    combined.subDeviceUids = { "uid-yeti", "uid-jack" };
    fakeca::addDevice (combined);

    const auto silent = [] (const float* const*, int, float* const*, int, int) {};

    {
        mma::CoreAudioBackend backend;
        check (backend.openExclusiveOutputStream ("uid-yeti", 48000.0, 256, silent),
               "the microphone's headphone jack still opens for monitoring");
        check (fakeca::isRunning (yetiId), "and plays");
        check (! fakeca::hogModeHeld (yetiId), "without hog mode, so other apps keep the microphone");
        backend.closeAllStreams();
    }

    {
        mma::CoreAudioBackend backend;
        check (backend.openExclusiveOutputStream ("uid-jack", 48000.0, 256, silent),
               "a sub-device of the combined device opens");
        check (! fakeca::hogModeHeld (jackId), "without hog mode either");
        backend.closeAllStreams();
    }

    {
        mma::CoreAudioBackend backend;
        check (backend.openExclusiveOutputStream ("uid-plain", 48000.0, 256, silent),
               "a plain output opens");
        check (fakeca::hogOwnerPid (plainId) == static_cast<int> (getpid()),
               "and is still held exclusively by this process");
        backend.closeAllStreams();
        check (! fakeca::hogModeHeld (plainId), "until it is closed");
    }
}

/// The combined device is built by the HAL after the create call returns, and
/// read too soon it listed no sub-devices -- so no headphone jacks, and
/// monitoring went to another output. The shared layout reads, against an
/// aggregate whose sub-devices activate late, settle within the bounded wait.
void aCombinedDeviceThatActivatesLateIsWaitedFor()
{
    std::printf ("\nA combined device whose sub-devices activate late\n");
    fakeca::reset();

    auto yetiA = microphone ("Yeti", "uid-yeti-a", 1, fakeca::BufferShape::oneChannelPerBuffer);
    yetiA.outputChannels = 2;
    fakeca::addDevice (yetiA);
    auto yetiB = microphone ("Yeti", "uid-yeti-b", 1, fakeca::BufferShape::oneChannelPerBuffer);
    yetiB.outputChannels = 2;
    fakeca::addDevice (yetiB);

    fakeca::DeviceSpec combined;
    combined.name = "SobStage";
    combined.uid = mma::kOurAggregateUid;
    combined.transportType = kAudioDeviceTransportTypeAggregate;
    combined.subDeviceUids = { "uid-yeti-a", "uid-yeti-b" };
    combined.subDeviceActivationDelayReads = 4;
    const auto combinedId = fakeca::addDevice (combined);

    namespace q = mma::macaudio;

    // What MacSystemAggregateDevice::getOutputLayout() reads, through the
    // same shared queries.
    const auto readLayout = [combinedId]
    {
        std::vector<mma::CombinedDeviceOutputs> layout;
        for (auto sub : q::readActiveSubDevices (combinedId))
            layout.push_back ({ q::readStringProperty (sub, kAudioDevicePropertyDeviceUID),
                                q::countChannels (sub, false) });
        return layout;
    };

    check (q::findDeviceByUID (mma::kOurAggregateUid) == combinedId, "the combined device is found by UID");
    check (! mma::combinedDeviceHasHeadphones (readLayout()),
           "read at once, it has no jacks yet -- the defect");

    int pauses = 0;
    const bool settled = mma::waitForCombinedLayout (readLayout, { "uid-yeti-a", "uid-yeti-b" }, 50,
                                                     [&pauses] { ++pauses; });
    check (settled, "waiting lets every sub-device activate");
    check (pauses > 0 && pauses < 50, "within the bound");

    const auto layout = readLayout();
    check (mma::combinedDeviceHasHeadphones (layout), "and then both headphone jacks are there");
    check (layout.size() == 2 && layout[0].outputChannels == 2 && layout[1].outputChannels == 2,
           "each with its own two output channels");
}

/// The combined device's headphone channels are numbered over the microphones
/// active in it now. One the HAL activates after the device was made (slower
/// than the publish wait and the one recheck after it), or one that drops out
/// of it, moves every later jack onto other channel numbers -- and the device
/// list does not change, so a client listening only to that kept the per-jack
/// headphone gains laid over the wrong people. The backend watches the
/// combined device's own shape and reports it through the same callback.
void theCombinedDeviceChangingShapeReachesTheApp()
{
    std::printf ("\nThe combined device changing shape without a device-list change\n");
    fakeca::reset();
    namespace q = mma::macaudio;

    auto yetiA = microphone ("Yeti", "uid-yeti-a", 1, fakeca::BufferShape::oneChannelPerBuffer);
    yetiA.outputChannels = 2;
    const auto yetiAId = fakeca::addDevice (yetiA);
    auto yetiB = microphone ("Yeti", "uid-yeti-b", 1, fakeca::BufferShape::oneChannelPerBuffer);
    yetiB.outputChannels = 2;
    fakeca::addDevice (yetiB);

    // Made, but its microphones not active in it yet, and not about to be on
    // their own.
    fakeca::DeviceSpec combined;
    combined.name = "SobStage";
    combined.uid = mma::kOurAggregateUid;
    combined.transportType = kAudioDeviceTransportTypeAggregate;
    combined.subDeviceUids = { "uid-yeti-a", "uid-yeti-b" };
    combined.subDeviceActivationDelayReads = 1000000;
    const auto combinedId = fakeca::addDevice (combined);

    // Someone's own aggregate from Audio MIDI Setup: not the app's business.
    fakeca::DeviceSpec theirs;
    theirs.name = "Aggregate Device";
    theirs.uid = "user-made-aggregate";
    theirs.outputChannels = 2;
    theirs.transportType = kAudioDeviceTransportTypeAggregate;
    theirs.subDeviceUids = { "uid-yeti-a" };
    const auto theirsId = fakeca::addDevice (theirs);

    std::atomic<int> notifications { 0 };
    AudioObjectID remadeId = kAudioObjectUnknown;

    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([&notifications]
        {
            notifications.fetch_add (1, std::memory_order_relaxed);
        });

        (void) backend.enumerateOutputDevices(); // the rescan's pass
        check (q::readActiveSubDevices (combinedId).empty(),
               "the combined device has no active microphones yet, so no headphone jacks");
        check (fakeca::propertyListenerCount (combinedId) == 2,
               "its active microphones and its output layout are watched, though it has no outputs yet");
        check (fakeca::propertyListenerCount (theirsId) == 0, "another aggregate is not watched");
        check (fakeca::propertyListenerCount (yetiAId) == 0, "nor is a microphone's own layout");

        auto before = notifications.load();
        fakeca::activateSubDevices (combinedId);
        check (notifications.load() > before,
               "its microphones activating late reaches the app with no device-list change");
        check (q::readActiveSubDevices (combinedId).size() == 2,
               "and the re-read that follows finds both jacks");

        (void) backend.enumerateOutputDevices();
        check (fakeca::propertyListenerCount (combinedId) == 2, "a later rescan does not watch it twice");

        // coreaudiod restarts: listeners on devices go with the old service.
        fakeca::restartService();
        check (fakeca::propertyListenerCount (combinedId) == 0, "a coreaudiod restart drops the watch");
        (void) backend.enumerateOutputDevices(); // the rescan the restart causes
        check (fakeca::propertyListenerCount (combinedId) == 2, "and the rescan after it watches it again");

        before = notifications.load();
        fakeca::activateSubDevices (combinedId);
        check (notifications.load() > before, "so a change after the restart is heard too");

        backend.setDeviceChangeCallback (nullptr);
        check (fakeca::propertyListenerCount (combinedId) == 0, "removing the callback removes the watch");

        before = notifications.load();
        fakeca::activateSubDevices (combinedId);
        check (notifications.load() == before, "and a later change calls nobody");

        backend.setDeviceChangeCallback ([&notifications]
        {
            notifications.fetch_add (1, std::memory_order_relaxed);
        });
        check (fakeca::propertyListenerCount (combinedId) == 2, "a new callback watches it again straight away");

        // Replaced (a rename, or a rig change, destroys and re-makes it): the
        // old one's registration goes on the next rescan, and the new one is
        // watched.
        fakeca::removeDevice (combinedId);
        remadeId = fakeca::addDevice (combined);
        (void) backend.enumerateOutputDevices();
        check (fakeca::propertyListenerCount (remadeId) == 2, "a re-made combined device is watched");
    }

    check (fakeca::propertyListenerCount (remadeId) == 0, "destroying the backend removes the watch");

    const auto before = notifications.load();
    fakeca::activateSubDevices (remadeId);
    check (notifications.load() == before, "and a change after the backend is gone reaches nothing");
}

/// Re-checking an output this app already holds must report it as available.
/// The hog-mode property reports an owning pid, and "owned by us" and "owned by
/// someone else" are the same value shape -- so telling them apart is the
/// difference between a working monitor and a false "another app has it".
void anOutputWeAlreadyHoldIsStillReportedAsAvailable()
{
    std::printf ("\nRe-checking an output this app already holds\n");
    fakeca::reset();

    fakeca::addDevice (headphones ("Held Out", "uid-held", 2, fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;

    check (backend.openExclusiveOutputStream ("uid-held", 48000.0, 256,
                                              [] (const float* const*, int, float* const*, int, int) {}),
           "the output opens and takes hog mode");

    const auto capability = backend.checkExclusiveModeCapability ("uid-held", 48000.0, 256);
    check (capability.exclusiveModeAvailable,
           "a second check sees our own hog mode as ours, not as another app's");

    backend.closeAllStreams();
}

/// §5.4: the latency the preflight reports is the round trip, in and out.
///
/// Every backend computes this and nothing read it: CaptureCoordinator dropped
/// it, so Application::measuredLatencyMs was never assigned by anything and the
/// Advanced panel reported monitoring latency as "0.0 ms" -- not a small
/// number, an impossible one -- while every take's session.json recorded 0.0
/// for good. Now that it reaches the user, the three platforms have to agree on
/// what it means. ALSA counted a single buffer and reported half.
void theReportedLatencyIsTheRoundTrip()
{
    std::printf ("\nWhat the preflight says the monitor path costs\n");
    fakeca::reset();

    fakeca::addDevice (headphones ("Latency Out", "uid-lat", 2,
                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;

    const auto cap = backend.checkExclusiveModeCapability ("uid-lat", 48000.0, 256);

    check (cap.exclusiveModeAvailable, "the output is capable");
    check (cap.measuredOrEstimatedLatencyMs > 0.0,
           "and the preflight reports a latency rather than leaving it at zero");

    const double oneBuffer = (256.0 / 48000.0) * 1000.0;
    check (std::abs (cap.measuredOrEstimatedLatencyMs - oneBuffer * 2.0) < 1e-9,
           "which is the round trip -- one buffer in, one buffer out");

    // It has to follow the buffer size, or it is a constant dressed as a
    // measurement.
    const auto small = backend.checkExclusiveModeCapability ("uid-lat", 48000.0, 128);
    const auto large = backend.checkExclusiveModeCapability ("uid-lat", 48000.0, 512);

    check (std::abs (small.measuredOrEstimatedLatencyMs
                         - cap.measuredOrEstimatedLatencyMs / 2.0) < 1e-9,
           "halving the buffer halves it");
    check (std::abs (large.measuredOrEstimatedLatencyMs
                         - cap.measuredOrEstimatedLatencyMs * 2.0) < 1e-9,
           "and doubling the buffer doubles it");
}

/// §5.4: the preflight must answer the question the open will ask.
///
/// It asked only about hog mode, so a fixed-rate 44.1 kHz interface -- ordinary
/// hardware, and most USB mics -- was reported as ready for exclusive
/// monitoring and then refused the open with a rate-mismatch message. The
/// preflight exists so the user hears that while there is still time to act on
/// it, not at the top of a take.
///
/// This is the third platform to have a defect in checkExclusiveModeCapability:
/// WASAPI probed a single channel layout and called capable devices incapable,
/// ALSA accepted only "hw:" names and refused every card its own picker
/// offered, and this one promised what it could not deliver. Different
/// polarities, one root -- the capability check not asking what the open asks.
void aFixedRateOutputIsNotPromisedForMonitoring()
{
    std::printf ("\nAn output that cannot run at the take's sample rate\n");
    fakeca::reset();

    auto spec = headphones ("Fixed 44k1 Out", "uid-44k1", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = { { 44100.0, 44100.0 } };
    spec.currentRate = 44100.0;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;

    const auto capability = backend.checkExclusiveModeCapability ("uid-44k1", 48000.0, 256);
    check (! capability.exclusiveModeAvailable,
           "the preflight refuses it rather than promising monitoring");
    check (capability.unavailableReason.find ("44.1 kHz") != std::string::npos,
           "and names the rate the interface is actually running at");
    check (capability.unavailableReason.find ("48 kHz") != std::string::npos,
           "and the rate the recording wants, so both halves of the fix are visible");

    // The preflight and the open must agree. A refusal that the open would have
    // allowed is its own bug, and this is what says the two are answering the
    // same question.
    check (! backend.openExclusiveOutputStream ("uid-44k1", 48000.0, 256,
                                                [] (const float* const*, int, float* const*, int, int) {}),
           "and the open refuses it too, so preflight and open agree");
}

/// The control, and the half that keeps the fix from being a blanket refusal: a
/// device that can do the rate is still offered. Without this, returning
/// "unavailable" unconditionally would pass the case above and turn monitoring
/// off for everybody.
void anOutputThatSupportsTheRateIsStillOffered()
{
    std::printf ("\nAn output that can run at the take's sample rate\n");
    fakeca::reset();

    // Discrete: the device lists exactly the rate asked for.
    auto exact = headphones ("48k Out", "uid-48k", 2, fakeca::BufferShape::oneChannelPerBuffer);
    exact.rateRanges = { { 48000.0, 48000.0 } };
    exact.currentRate = 48000.0;
    fakeca::addDevice (exact);

    // Continuous: a device with a sample-rate converter advertises a span, and
    // 48000 sits inside it without ever appearing as one of its endpoints.
    // querySupportedSampleRates walks common rates through the span for exactly
    // this reason, and a capability check that only compared endpoints would
    // refuse this device.
    auto span = headphones ("44k1-96k Out", "uid-span", 2, fakeca::BufferShape::oneChannelPerBuffer);
    span.rateRanges = { { 44100.0, 96000.0 } };
    span.currentRate = 44100.0;
    fakeca::addDevice (span);

    mma::CoreAudioBackend backend;

    const auto exactCap = backend.checkExclusiveModeCapability ("uid-48k", 48000.0, 256);
    check (exactCap.exclusiveModeAvailable, "a device listing the rate is offered");
    check (exactCap.unavailableReason.empty(), "with no reason attached to a yes");

    const auto spanCap = backend.checkExclusiveModeCapability ("uid-span", 48000.0, 256);
    check (spanCap.exclusiveModeAvailable,
           "and so is one whose continuous range merely contains the rate");

    check (backend.openExclusiveOutputStream ("uid-48k", 48000.0, 256,
                                              [] (const float* const*, int, float* const*, int, int) {}),
           "and the open agrees with the yes");
}

/// A preflight may only say no when it is sure. A device whose rate list comes
/// back empty is a property read that told us nothing, not a device that
/// supports nothing -- refusing on that would turn one unreadable property into
/// no monitoring at all, and the open is the authority either way.
void anOutputThatReportsNoRatesIsNotRefused()
{
    std::printf ("\nAn output whose rate list cannot be read\n");
    fakeca::reset();

    auto spec = headphones ("Silent About Rates", "uid-norates", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = {};
    spec.currentRate = 48000.0;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;

    const auto capability = backend.checkExclusiveModeCapability ("uid-norates", 48000.0, 256);
    check (capability.exclusiveModeAvailable,
           "an unreadable rate list is not treated as a refusal");
}

/// §2: hotplug arrives from the OS, never from a timer. The backend registers a
/// property listener, so adding a device must reach it without anything polling.
void hotplugArrivesThroughTheOsListener()
{
    std::printf ("\nA mic plugged in after launch\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    int notifications = 0;
    backend.setDeviceChangeCallback ([&notifications] { ++notifications; });

    fakeca::addDevice (microphone ("Late Mic", "uid-late", 1, fakeca::BufferShape::oneChannelPerBuffer));
    check (notifications >= 1, "the backend is told, with no timer involved");

    check (backend.enumerateInputDevices().size() == 1, "and the device is there when it re-enumerates");

    const auto before = notifications;
    fakeca::removeDevice (100);
    check (notifications > before, "unplugging notifies too");
}

/// H11: another app can change a live interface's nominal rate without adding
/// or removing a device. The per-device property listener must catch it, name
/// both rates, and hand the change back to the normal re-enumeration path.
void aLiveSampleRateChangeIsReportedAndReenumerated()
{
    std::printf ("\nA live interface whose sample rate another app changes\n");
    fakeca::reset();

    auto spec = microphone ("Rate Change Mic", "uid-rate-change", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    int deviceChanges = 0;
    backend.setDeviceChangeCallback ([&deviceChanges] { ++deviceChanges; });

    check (backend.openInputStream ("uid-rate-change", 48000.0, 256, capture.callback()),
           "the stream opens at 48 kHz");
    check (fakeca::propertyListenerCount (id) == 3,
           "nominal-rate, device-alive, and processor-overload listeners are installed");

    check (fakeca::setNominalRateExternally (id, 44100.0),
           "Audio MIDI Setup changes the live device to 44.1 kHz");

    const auto first = backend.takeStreamFailures();
    check (first.size() == 1, "the mismatch is reported on the next message-thread poll");

    if (! first.empty())
    {
        check (first.front().deviceId == "uid-rate-change",
               "the report identifies the affected microphone");
        check (first.front().kind == mma::StreamFailureKind::sampleRateChanged,
               "the event is typed so the app can stop a take without parsing prose");
        check (first.front().reason.find ("44.1 kHz") != std::string::npos
               && first.front().reason.find ("48 kHz") != std::string::npos,
               "the report names both the new hardware rate and SobStage's rate");
    }

    check (deviceChanges == 1,
           "the property event is bridged into the ordinary re-enumeration callback");
    check (backend.takeStreamFailures().empty(),
           "an unchanged mismatch is not repeated on every status poll");

    fakeca::setNominalRateExternally (id, 48000.0);
    check (backend.takeStreamFailures().empty(),
           "returning to the stream rate clears the mismatch without a false failure");

    fakeca::setNominalRateExternally (id, 44100.0);
    check (backend.takeStreamFailures().size() == 1,
           "a later, separate rate mismatch can be reported again");

    backend.closeAllStreams();
    check (fakeca::propertyListenerCount (id) == 0,
           "closing the stream removes every per-device listener");
}

/// The headphone output, not a microphone, switched to another rate by
/// another app. An output with inputs of its own is not hogged, so any app
/// may. The report has to be told apart from a microphone's -- the take's
/// files are not at risk, so the app carries the take on without the output
/// rather than stopping it -- and must not carry advice to "start a new take"
/// that the app may not follow. Its return to the stream's rate is news too:
/// it is how an output the app stopped using gets taken back.
void aLiveOutputRateChangeIsReportedAsTheOutputsAndSoIsItsReturn()
{
    std::printf ("\nHeadphones on an interface that another app switches to another rate\n");
    fakeca::reset();

    auto spec = headphones ("Rate Change Out", "uid-rate-out", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.inputChannels = 2; // a duplex interface: left un-hogged, so others can switch it
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    int deviceChanges = 0;
    backend.setDeviceChangeCallback ([&deviceChanges] { ++deviceChanges; });

    auto silent = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);
    };

    check (backend.openExclusiveOutputStream ("uid-rate-out", 48000.0, 256, silent),
           "the output opens at 48 kHz");

    check (fakeca::setNominalRateExternally (id, 44100.0),
           "another app switches the output to 44.1 kHz");

    const auto changed = backend.takeStreamFailures();
    check (changed.size() == 1, "the change is reported on the next message-thread poll");

    if (! changed.empty())
    {
        const auto& f = changed.front();
        check (f.deviceId.empty() && f.subject == "Your sound output",
               "it is reported as the sound output's, naming no microphone");
        check (mma::streamFailureIsOutputRateChange (f),
               "and typed so the app can tell it from a microphone's rate change");
        check (f.reason.find ("44.1 kHz") != std::string::npos
               && f.reason.find ("48 kHz") != std::string::npos,
               "the report names both the new rate and SobStage's rate");
        check (f.reason.find ("new take") == std::string::npos,
               "and says nothing about starting a new take: the app decides what happens to the take");
    }

    check (deviceChanges == 1, "the change is still bridged into re-enumeration");
    check (backend.takeStreamFailures().empty(), "an unchanged switch is not repeated on every poll");

    fakeca::setNominalRateExternally (id, 48000.0);
    const auto back = backend.takeStreamFailures();
    check (back.size() == 1, "its return to the stream's rate is reported");

    if (! back.empty())
    {
        const auto& f = back.front();
        check (f.kind == mma::StreamFailureKind::resumed && f.deviceId.empty()
               && f.subject == "Your sound output",
               "as the sound output being usable again");
        check (f.reason.find ("48 kHz") != std::string::npos, "naming the rate it is back at");
    }

    check (backend.takeStreamFailures().empty(), "and only once");

    fakeca::setNominalRateExternally (id, 44100.0);
    check (backend.takeStreamFailures().size() == 1, "a later switch is reported again");

    backend.closeAllStreams();
    check (fakeca::propertyListenerCount (id) == 0,
           "closing the stream removes every per-device listener");
}

/// H11: kAudioDevicePropertyDeviceIsAlive reaches the backend before the
/// system device list necessarily drops the AudioObjectID. This must report a
/// dead stream immediately rather than waiting five seconds for the IOProc
/// watchdog, and recovery must release the one-shot latch.
void aDeviceAliveChangeIsReportedImmediately()
{
    std::printf ("\nA live AudioObject that becomes unusable before disappearing\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Dying Mic", "uid-dying", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-dying", 48000.0, 256, capture.callback()),
           "the stream opens");

    check (fakeca::setDeviceAlive (id, false),
           "the HAL marks the still-present device unusable");

    const auto dead = backend.takeStreamFailures();
    check (dead.size() == 1, "the device-alive listener reports it without a five-second wait");

    if (! dead.empty())
    {
        check (dead.front().deviceId == "uid-dying", "the dead-device report names the mic");
        check (dead.front().kind == mma::StreamFailureKind::deviceUnavailable,
               "the event is typed for app-level dropout handling");
        check (dead.front().reason.find ("no longer available") != std::string::npos,
               "the reason describes an unavailable device, not generic silence");
    }

    check (backend.takeStreamFailures().empty(),
           "the same dead state is reported once");
    check (backend.enumerateInputDevices().empty(),
           "an alive=false AudioObject is omitted before the system list removes it");

    fakeca::setDeviceAlive (id, true);
    check (backend.takeStreamFailures().empty(),
           "the same AudioObject can recover without a false failure");
    check (backend.enumerateInputDevices().size() == 1,
           "a revived AudioObject returns to enumeration");

    fakeca::setDeviceAlive (id, false);
    check (backend.takeStreamFailures().size() == 1,
           "a second genuine death after recovery is reported again");
    backend.closeAllStreams();
}

/// A USB hub re-settling can drop DeviceIsAlive to 0 and back to 1 on the same
/// AudioObjectID. The app silences the mic's channel on the "no longer
/// available" report, and mid-take only a `resumed` report gives it back --
/// which only the five-second watchdog used to arm, so a mic whose alive flag
/// dipped recorded silence for the rest of the take while its audio arrived.
void aMicWhoseAliveFlagDipsAndReturnsIsReportedAsResumed()
{
    std::printf ("\nA mic whose alive flag dips and comes back on the same AudioObject\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Hub Mic", "uid-hub", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-hub", 48000.0, 256, capture.callback()),
           "the stream opens");
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });

    fakeca::setDeviceAlive (id, false);
    const auto dead = backend.takeStreamFailures();
    check (dead.size() == 1 && dead.front().kind == mma::StreamFailureKind::deviceUnavailable,
           "the dip is reported as the device becoming unavailable");

    fakeca::setDeviceAlive (id, true);
    check (backend.takeStreamFailures().empty(),
           "coming back alive alone is not yet proof that audio is arriving");

    // Strictly after the report on the backend's clock.
    std::this_thread::sleep_for (std::chrono::milliseconds (2));
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });

    const auto resumed = backend.takeStreamFailures();
    check (resumed.size() == 1 && resumed.front().kind == mma::StreamFailureKind::resumed
               && resumed.front().deviceId == "uid-hub",
           "audio arriving after the dip is reported as the mic resuming");

    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });
    check (backend.takeStreamFailures().empty(), "and the resume is reported once");
    backend.closeAllStreams();
}

/// Processor-overload is normally notified from CoreAudio's IO thread. Input
/// overruns must be surfaced as possible take loss; output overruns feed the
/// monitor-glitch counter and must not be mislabelled as recorded-audio loss.
void processorOverloadsAreCountedOnTheRightPath()
{
    std::printf ("\nCoreAudio processor-overload events on input and output\n");
    fakeca::reset();

    const auto input = fakeca::addDevice (microphone ("Busy Mic", "uid-busy-in", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    const auto output = fakeca::addDevice (headphones ("Busy Out", "uid-busy-out", 2,
                                                        fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-busy-in", 48000.0, 256, capture.callback()),
           "the input stream opens");
    check (backend.openExclusiveOutputStream (
               "uid-busy-out", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "the output stream opens");

    fakeca::fireProcessorOverload (input);
    fakeca::fireProcessorOverload (input);

    const auto inputFailures = backend.takeStreamFailures();
    check (inputFailures.size() == 1,
           "one poll reports the accumulated input deadline misses once");
    check (! inputFailures.empty()
           && inputFailures.front().kind == mma::StreamFailureKind::processorOverload
           && inputFailures.front().reason.find ("audio processing deadline") != std::string::npos,
           "the input warning is typed and explains possible recording loss");
    check (backend.getOutputGlitchCount() == 0,
           "input overloads do not inflate the monitor-glitch counter");
    check (backend.takeStreamFailures().empty(),
           "input overloads are not repeated without a new event");

    fakeca::fireProcessorOverload (output);
    check (backend.getOutputGlitchCount() == 1,
           "an output deadline miss increments the monitor-glitch counter");
    check (backend.takeStreamFailures().empty(),
           "an output glitch is not also reported as microphone loss");

    backend.closeAllStreams();
    check (fakeca::propertyListenerCount (input) == 0
           && fakeca::propertyListenerCount (output) == 0,
           "both streams remove their listeners during teardown");
}

/// Processor-overload listeners normally execute on CoreAudio's IO thread
/// while the message thread reads the counter. Exercise that actual overlap so
/// ThreadSanitizer verifies the listener shares only atomics with the reader.
void processorOverloadNotificationIsThreadSafe()
{
    std::printf ("\nProcessor-overload callbacks racing the message-thread reader\n");
    fakeca::reset();

    const auto output = fakeca::addDevice (headphones ("Threaded Out", "uid-threaded-out", 2,
                                                        fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    check (backend.openExclusiveOutputStream (
               "uid-threaded-out", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "the output stream opens");

    constexpr uint64_t eventCount = 2000;
    std::atomic<bool> producerDone { false };

    std::thread producer ([&]
    {
        for (uint64_t i = 0; i < eventCount; ++i)
            fakeca::fireProcessorOverload (output);

        producerDone.store (true, std::memory_order_release);
    });

    uint64_t observed = 0;
    while (! producerDone.load (std::memory_order_acquire))
        observed = std::max (observed, backend.getOutputGlitchCount());

    producer.join();
    observed = std::max (observed, backend.getOutputGlitchCount());

    check (observed == eventCount,
           "every IO-thread overload is retained while the message thread reads");
    backend.closeAllStreams();
}

/// A third-party driver may refuse one or every listener. Continuing in total
/// silence would recreate H11 on that hardware, so the open remains compatible
/// but the missing safety watch is reported once.
void aDeviceThatRefusesSafetyListenersSaysSo()
{
    std::printf ("\nA device driver that refuses per-device safety listeners\n");
    fakeca::reset();

    fakeca::addDevice (microphone ("Opaque Mic", "uid-opaque", 1,
                                   fakeca::BufferShape::oneChannelPerBuffer));
    fakeca::setPropertyListenersAllowed (false);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-opaque", 48000.0, 256, capture.callback()),
           "the unusual device remains usable");

    const auto problem = backend.takeStreamFailures();
    check (problem.size() == 1, "the missing safety listeners are not silent");
    check (! problem.empty()
           && problem.front().kind == mma::StreamFailureKind::safetyMonitoringUnavailable
           && problem.front().reason.find ("can't report every sample-rate") != std::string::npos,
           "the report is typed and names the safety information that is unavailable");
    check (backend.takeStreamFailures().empty(),
           "the listener problem is reported once rather than on every poll");
    backend.closeAllStreams();
}

/// A microphone that stops sending audio after it was opened -- the HAL keeps
/// the stream, the IOProc simply never runs again. Nothing about that is
/// visible from anywhere else in the app, so if the watchdog does not report
/// it, a take goes on recording silence for that channel and says nothing.
void aMicrophoneThatGoesQuietAfterOpeningIsReported()
{
    std::printf ("\nA mic whose IOProc stops running after it opened\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Quiet Mic", "uid-quiet", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-quiet", 48000.0, 256, capture.callback()),
           "the mic opens");

    // One callback, so the stream is known to have been alive; then nothing.
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });

    check (backend.takeStreamFailures().empty(), "a mic that just delivered audio is not accused");

    // The watchdog waits 5 s before calling a stream dead, because the HAL may
    // legitimately pause around a device or format change.
    std::this_thread::sleep_for (std::chrono::milliseconds (5400));

    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 1, "the silent stream is reported once");

    if (! reported.empty())
    {
        std::printf ("  reason: %s\n", reported.front().reason.c_str());
        check (reported.front().deviceId == "uid-quiet", "and names the microphone it happened to");
        check (reported.front().reason.find ("stopped sending audio") != std::string::npos,
               "and says what the user can do about it");
    }

    check (backend.takeStreamFailures().empty(),
           "and is not repeated on every poll for as long as it stays dead");

    // Audio arriving again clears the latch, so a device that recovers and
    // dies a second time is reported a second time.
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });

    // And the recovery itself is reported. The HAL resumes a paused IOProc on
    // the same device -- after sleep, a hog-mode grab, a clock re-lock -- and
    // the app had silenced that mic's track on the dead report. Told nothing,
    // it wrote silence for the rest of the take while the audio arrived.
    const auto resumed = backend.takeStreamFailures();
    check (resumed.size() == 1, "the stream delivering again is reported once");

    if (! resumed.empty())
    {
        check (resumed.front().deviceId == "uid-quiet", "and names the microphone that came back");
        check (resumed.front().kind == mma::StreamFailureKind::resumed,
               "and is typed as a resume, not another failure");
    }

    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });
    check (backend.takeStreamFailures().empty(), "a resume is not repeated while audio keeps arriving");

    std::this_thread::sleep_for (std::chrono::milliseconds (5400));

    const auto secondDeath = backend.takeStreamFailures();
    check (secondDeath.size() == 1, "a second death is reported again");
    check (! secondDeath.empty() && secondDeath.front().kind != mma::StreamFailureKind::resumed,
           "and as a death, not a resume");
}

/// AudioDeviceStart can return success without the driver ever invoking its
/// IOProc. A watchdog based only on the most recent callback sees the initial
/// zero timestamp and skips that stream forever, leaving a take silently empty.
void aMicrophoneWhoseFirstCallbackNeverArrivesIsReported()
{
    std::printf ("\nA mic whose first IOProc never arrives\n");
    fakeca::reset();

    fakeca::addDevice (microphone ("Never Started Mic", "uid-never-callback", 1,
                                  fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-never-callback", 48000.0, 256, capture.callback()),
           "the HAL reports that the mic started");
    check (backend.takeStreamFailures().empty(),
           "the first callback receives the same five-second grace period");

    std::this_thread::sleep_for (std::chrono::milliseconds (5400));

    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 1, "a missing first callback is reported once");

    if (! reported.empty())
    {
        check (reported.front().deviceId == "uid-never-callback",
               "the first-callback failure names the microphone");
        check (reported.front().reason.find ("stopped sending audio") != std::string::npos,
               "the first-callback failure gives the same recovery guidance");
    }

    check (backend.takeStreamFailures().empty(),
           "the missing first callback is not repeated on every poll");
}

/// A microphone and a pair of headphones open together, with a way to run one
/// IOProc on each, for the sleep scenarios below.
struct SleepRig
{
    AudioObjectID mic = kAudioObjectUnknown;
    AudioObjectID phones = kAudioObjectUnknown;
    std::string micUid;
    Capture capture;

    void open (mma::CoreAudioBackend& backend, const std::string& name)
    {
        micUid = "uid-" + name + "-mic";
        mic = fakeca::addDevice (microphone (name + " Mic", micUid, 1,
                                             fakeca::BufferShape::oneChannelPerBuffer));
        phones = fakeca::addDevice (headphones (name + " Phones", "uid-" + name + "-out", 2,
                                                fakeca::BufferShape::oneChannelPerBuffer));

        check (backend.openInputStream (micUid, 48000.0, 256, capture.callback()), "the mic opens");
        check (backend.openExclusiveOutputStream (
                   "uid-" + name + "-out", 48000.0, 256,
                   [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
                   {
                       for (int ch = 0; ch < numOutputs; ++ch)
                           std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);
                   }),
               "the headphones open");
    }

    void runMic() { fakeca::pumpInput (mic, { std::vector<float> (256, 0.25f) }); }

    void runPhones()
    {
        std::vector<std::vector<float>> written;
        fakeca::pumpOutput (phones, 256, written);
    }
};

/// Sleep stops every IOProc, and the watchdog used a clock that kept counting
/// through it: an hour with the lid closed read as an hour of silence, so on
/// waking every mic was "stopped sending audio. Unplug it and plug it back
/// in.", the headphones "stopped accepting audio", and the app tore the whole
/// rig down and reopened it while its USB devices were still waking up. The
/// awake seconds around the sleep -- the Mac taking a moment to fall asleep
/// after IO stopped, the devices taking a moment to start again after waking
/// -- added up to the same false report.
void aMacThatSleepsAndWakesDoesNotAccuseItsDevices()
{
    std::printf ("\nThe Mac sleeps with the rig open, then wakes\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    SleepRig rig;
    rig.open (backend, "sleepy");

    rig.runMic();
    rig.runPhones();
    check (backend.takeStreamFailures().empty(), "a running rig is not accused");

    // The lid closes. The HAL stops IO, and the Mac takes a few seconds,
    // awake, to actually go to sleep.
    fakeca::passTime (3.0);
    check (backend.takeStreamFailures().empty(), "the moments before the Mac falls asleep are not a failure");

    fakeca::sleepSystem (3600.0);
    check (backend.takeStreamFailures().empty(),
           "waking after an hour asleep does not call every device dead");

    // USB devices take their time to start again after a wake.
    fakeca::passTime (4.0);
    check (backend.takeStreamFailures().empty(), "nor while the devices are starting again");
    fakeca::passTime (4.0);
    check (backend.takeStreamFailures().empty(), "however long they take");

    rig.runMic();
    rig.runPhones();
    check (backend.takeStreamFailures().empty(),
           "audio coming back is not news, since nothing was said about it going");

    // After a wake the watchdog is as watchful as before.
    fakeca::passTime (5.5);
    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 2, "a rig that stops after waking is still reported, both devices");
    backend.closeAllStreams();
}

/// A dark wake: the Mac wakes with the lid shut for maintenance, the app runs
/// and polls, and audio stays off -- for a minute, sometimes. That is not every
/// device failing either. Once the Mac is properly awake and the rest of the
/// rig is running again, a mic that did not come back is reported as usual.
void aDarkWakeIsNotAFailureButAMicThatNeverReturnsIs()
{
    std::printf ("\nA dark wake with the lid shut, then a mic that never comes back\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    SleepRig rig;
    rig.open (backend, "dark");

    rig.runMic();
    rig.runPhones();
    check (backend.takeStreamFailures().empty(), "a running rig is not accused");

    fakeca::sleepSystem (1800.0);

    bool quiet = true;
    for (int second = 0; second < 45; ++second)
    {
        fakeca::passTime (1.0);
        quiet = quiet && backend.takeStreamFailures().empty();
    }

    check (quiet, "45 seconds of a dark wake, audio off, are not a failure");

    fakeca::sleepSystem (1800.0);
    check (backend.takeStreamFailures().empty(), "nor is going back to sleep and then waking properly");

    // Properly awake: the headphones start again, the mic never does.
    rig.runPhones();
    check (backend.takeStreamFailures().empty(), "the headphones starting again is not news");

    fakeca::passTime (5.5);
    rig.runPhones();
    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 1 && reported.front().deviceId == rig.micUid,
           "the mic that never came back is reported once the rest of the rig is running");
    check (! reported.empty()
               && reported.front().reason.find ("stopped sending audio") != std::string::npos,
           "with the usual advice");
    backend.closeAllStreams();
}

/// Holding the watchdog until audio runs again must not hold it forever: a Mac
/// whose audio never starts again after waking still hears about it.
void aWakeAfterWhichNothingStartsAgainIsStillReported()
{
    std::printf ("\nA Mac that wakes and whose audio never starts again\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Stuck Mic", "uid-stuck-after-wake", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));
    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-stuck-after-wake", 48000.0, 256, capture.callback()), "the mic opens");
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });
    check (backend.takeStreamFailures().empty(), "a running mic is not accused");

    fakeca::sleepSystem (600.0);
    check (backend.takeStreamFailures().empty(), "waking is not a failure");

    constexpr double kWait = mma::CallbackWatchdogSleepGuard::kMaxWaitForAudioSeconds;
    fakeca::passTime (kWait - 1.0);
    check (backend.takeStreamFailures().empty(), "the watchdog waits for audio to start again");

    fakeca::passTime (2.0);
    check (backend.takeStreamFailures().empty(), "then gives the mic the usual few seconds");

    fakeca::passTime (5.5);
    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 1 && reported.front().deviceId == "uid-stuck-after-wake",
           "and a mic that never came back is reported");
    backend.closeAllStreams();
}

/// A mic that really had stopped before the Mac slept, and is sending again
/// after it wakes, has to be told as sending again -- its channel was
/// silenced on the dead report, and only the resume gives it back. The resume
/// compares the report and the callback on one clock; re-arming the watchdog
/// by rewriting stamps would have broken that.
void aMicCalledDeadBeforeSleepThatReturnsAfterWakeIsResumed()
{
    std::printf ("\nA mic called dead before the Mac slept, back after it wakes\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Napping Mic", "uid-napping", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));
    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-napping", 48000.0, 256, capture.callback()), "the mic opens");
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });
    check (backend.takeStreamFailures().empty(), "a running mic is not accused");

    fakeca::passTime (5.5);
    const auto dead = backend.takeStreamFailures();
    check (dead.size() == 1 && dead.front().kind != mma::StreamFailureKind::resumed,
           "a mic that stops while the Mac is awake is reported");

    fakeca::sleepSystem (600.0);
    check (backend.takeStreamFailures().empty(), "and is not reported again on waking");

    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });
    const auto resumed = backend.takeStreamFailures();
    check (resumed.size() == 1 && resumed.front().kind == mma::StreamFailureKind::resumed
               && resumed.front().deviceId == "uid-napping",
           "its audio after the wake is reported as the mic sending again");
    backend.closeAllStreams();
}

/// Some class-compliant USB drivers block the caller inside AudioDeviceStart
/// for minutes. The app window is created only after audio initialisation, so
/// an unbounded call makes the whole application appear never to launch.
void aStuckInputStartIsBoundedAndCleanedUp()
{
    std::printf ("\nA USB mic whose HAL start call stalls\n");
    fakeca::reset();

    const auto outputId = fakeca::addDevice (headphones (
        "Working Output", "uid-working-output", 2,
        fakeca::BufferShape::oneChannelPerBuffer));
    const auto workingInputId = fakeca::addDevice (microphone (
        "Working Mic", "uid-working-input", 1,
        fakeca::BufferShape::oneChannelPerBuffer));

    auto spec = microphone ("Stuck Start Mic", "uid-stuck-start", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.startDelayMilliseconds = kStuckHalMilliseconds;
    spec.callbackBeforeStartReturns = true;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture, workingCapture;
    std::atomic<int> callbacksAfterOwnerRelease { 0 };
    auto callbackOwner = std::make_shared<int> (1);
    std::weak_ptr<int> weakCallbackOwner = callbackOwner;
    auto lifetimeCheckedCallback = [&callbacksAfterOwnerRelease, weakCallbackOwner] (
        const float* const*, int, float* const*, int, int)
    {
        if (weakCallbackOwner.expired())
            callbacksAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
    };
    check (backend.openExclusiveOutputStream (
               "uid-working-output", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "a monitor output is already live before the failure");
    check (backend.openInputStream (
               "uid-working-input", 48000.0, 256, workingCapture.callback()),
           "another microphone is already live before the failure");

    const auto began = std::chrono::steady_clock::now();

    check (! backend.openInputStream (spec.uid, 48000.0, 256, lifetimeCheckedCallback),
           "the backend stops waiting for the stalled input");
    callbackOwner.reset();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - began);
    check (elapsed < kBoundedReturn,
           "the simulated launch remains bounded well below the HAL delay");
    check (backend.getLastOpenError().find ("took too long") != std::string::npos,
           "the failure explains that macOS timed out rather than blaming a cable silently");

    const auto retryBegan = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "device churn does not launch a second stuck attempt for the same input");
    check (std::chrono::steady_clock::now() - retryBegan < kNoHalRoundTrip,
           "the duplicate attempt is rejected without touching the HAL again");

    const auto closeBegan = std::chrono::steady_clock::now();
    backend.closeAllStreams();
    const auto closeTook = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - closeBegan);

    // 200 ms, matching every sibling bound in this file rather than the 100 ms
    // this check alone carried. closeAllStreams() is DESIGNED to wait out
    // kHalTransactionTimeout (250 ms in simulation) whenever the HAL is stuck,
    // so 100 ms left 25 ms for scheduling jitter -- and this check has been
    // passing and failing run to run on the macOS runners because of it.
    // What the check is really for is unchanged: 200 ms is still far below the
    // 250 ms stall, so a close that genuinely waits for the stuck HAL to finish
    // still fails here.
    std::printf ("  [measured] closeAllStreams returned in %lld ms (stall is %d ms)\n",
                 (long long) closeTook.count(), spec.startDelayMilliseconds);
    check (closeTook < kBoundedReturn,
           "previously-open streams are quarantined without blocking behind the stuck HAL");

    // Synchronizes with the detached owner after Start returns and it performs
    // Stop/listener removal/Destroy. This is also what makes the TSan scenario
    // a real lifetime proof rather than a sleep that merely tends to pass.
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the abandoned worker eventually completes its own cleanup");
    check (! fakeca::isRunning (id), "the late-started IOProc is stopped");
    check (callbacksAfterOwnerRelease.load (std::memory_order_relaxed) == 0,
           "a callback that begins behind the timeout gate cannot reach its former owner");
    check (! fakeca::isRunning (workingInputId) && ! fakeca::isRunning (outputId),
           "the quarantined healthy streams are stopped by their owning worker");
    check (fakeca::propertyListenerCount (id) == 0,
           "no listener points at the retired stream");
    check (fakeca::propertyListenerCount (workingInputId) == 0
           && fakeca::propertyListenerCount (outputId) == 0,
           "the quarantined streams also remove every listener");
}

void aDriverThatRetainsListenerClientDataIsQuarantined()
{
    std::printf ("\nA driver that refuses to release listener clientData\n");
    fakeca::reset();

    auto spec = microphone ("Unsafe Teardown Mic", "uid-unsafe-teardown", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowPropertyListenerRemoval = false;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "the unusual device opens before teardown");
    backend.closeAllStreams();

    const auto retryBegan = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "a retained listener keeps the device quarantined");
    check (std::chrono::steady_clock::now() - retryBegan < kNoHalRoundTrip,
           "the quarantine refuses a retry without touching the unsafe driver");
}

/// Unplugging one interface of a live rig invalidates its AudioObjectID, so
/// Stop and Destroy on it fail with kAudioHardwareBadObjectError. That is the
/// HAL saying the registration no longer exists, not a refusal to drop it. It
/// used to be counted as unsafe cleanup and quarantined the backend until the
/// app restarted, refusing the interface that was still plugged in.
void anUnpluggedInterfaceDoesNotQuarantineTheSurvivor()
{
    std::printf ("\nOne interface of a live rig is unplugged before close\n");
    fakeca::reset();

    auto survivorSpec = microphone ("Survivor Mic", "uid-unplug-survivor", 1,
                                    fakeca::BufferShape::oneChannelPerBuffer);
    auto unpluggedSpec = microphone ("Unplugged Mic", "uid-unplug-gone", 1,
                                     fakeca::BufferShape::oneChannelPerBuffer);
    const auto survivor = fakeca::addDevice (survivorSpec);
    const auto unplugged = fakeca::addDevice (unpluggedSpec);

    mma::CoreAudioBackend backend;
    Capture survivorCapture, unpluggedCapture;
    check (backend.openInputStream (survivorSpec.uid, 48000.0, 256, survivorCapture.callback())
               && backend.openInputStream (unpluggedSpec.uid, 48000.0, 256,
                                           unpluggedCapture.callback()),
           "both interfaces open");

    fakeca::removeDevice (unplugged);
    backend.closeAllStreams();
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "teardown of the rig settles");
    check (! fakeca::isRunning (survivor) && fakeca::openIoProcCount (survivor) == 0,
           "the surviving interface is fully released");

    Capture retryCapture;
    check (backend.openInputStream (survivorSpec.uid, 48000.0, 256, retryCapture.callback()),
           "the surviving interface reopens after its partner was unplugged");
    backend.closeAllStreams();

    // Control: the same refusal on a device that is still present is a real
    // retained registration and must keep the quarantine.
    std::printf ("\nA live interface refuses teardown in the same rig\n");
    fakeca::reset();

    auto refusingSpec = microphone ("Refusing Mic", "uid-unplug-refusing", 1,
                                    fakeca::BufferShape::oneChannelPerBuffer);
    refusingSpec.allowPropertyListenerRemoval = false;
    fakeca::addDevice (survivorSpec);
    fakeca::addDevice (refusingSpec);

    mma::CoreAudioBackend controlBackend;
    check (controlBackend.openInputStream (survivorSpec.uid, 48000.0, 256, survivorCapture.callback())
               && controlBackend.openInputStream (refusingSpec.uid, 48000.0, 256,
                                                  unpluggedCapture.callback()),
           "both interfaces open");
    controlBackend.closeAllStreams();
    controlBackend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds);
    check (! controlBackend.openInputStream (survivorSpec.uid, 48000.0, 256,
                                             retryCapture.callback()),
           "a refusal from a device that is still present still quarantines");
}

/// This is the ordering that matters after a launch timeout: the abandoned
/// input discovers that CoreAudio retained listener clientData, while an
/// already-live stream is queued for asynchronous teardown. Successful cleanup
/// of that other stream must never erase the abandoned input's sticky quarantine.
void aTimedOutUnsafeInputCannotBeUnquarantinedByOtherCleanup()
{
    std::printf ("\nA timed-out input whose listener clientData is retained\n");
    fakeca::reset();

    fakeca::addDevice (headphones (
        "Working Output", "uid-sticky-output", 2,
        fakeca::BufferShape::oneChannelPerBuffer));

    auto spec = microphone ("Stuck Unsafe Mic", "uid-stuck-unsafe", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.startDelayMilliseconds = kStuckHalMilliseconds;
    spec.allowPropertyListenerRemoval = false;
    const auto inputId = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openExclusiveOutputStream (
               "uid-sticky-output", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "a healthy stream is live before the unsafe timeout");
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "the unsafe input still observes the bounded open deadline");

    backend.closeAllStreams();

    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the timed-out worker and queued healthy cleanup both settle behind a sticky quarantine");
    check (fakeca::propertyListenerCount (inputId) == 3,
           "retained listener clientData keeps its inert stream storage alive");

    const auto retryBegan = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "successful cleanup of another stream cannot reopen the unsafe device gate");
    check (std::chrono::steady_clock::now() - retryBegan < kNoHalRoundTrip,
           "the sticky quarantine rejects the retry without another HAL transaction");
}

/// CA-1. The output worker may spend the whole rate-settle window inside
/// setNominalSampleRate before hog mode, IOProc creation and Start. Its caller
/// used to give up after 150 ms (75 ms in simulation) while that same worker was
/// allowed 500 ms (50 ms) to settle the rate, so an ordinary 44.1 <-> 48 kHz
/// switch on the headphone output was reported as "took too long ... unplug
/// it". The open deadline must contain the settle window plus the HAL calls
/// that follow it.
void aSlowButHealthyOutputRateSwitchOpens()
{
    std::printf ("\nHeadphones that take a while to switch 44.1 -> 48 kHz and start\n");
    fakeca::reset();

    auto spec = headphones ("Slow Switch Phones", "uid-slow-switch-phones", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    // One stale read (a real rate switch, well inside the simulated 50 ms
    // settle window even on a slow macOS runner, where three 10 ms polls
    // overran it) and then a Start that takes 110 ms: past the old 75 ms
    // simulated output deadline and well inside the new one.
    spec.rateChangeDelayReads = 1;
    spec.startDelayMilliseconds = 110;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    const auto began = std::chrono::steady_clock::now();
    const bool opened = backend.openExclusiveOutputStream (
        spec.uid, 48000.0, 256,
        [] (const float* const*, int, float* const*, int, int) {});
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - began);
    std::printf ("  [measured] the output open took %lld ms\n", (long long) took.count());

    if (! opened)
        std::printf ("  [measured] refused with: %s\n", backend.getLastOpenError().c_str());

    check (opened, "a slow but healthy output rate switch opens instead of timing out");
    check (fakeca::nominalRate (id) == 48000.0, "the output runs at the requested rate");
    check (fakeca::isRunning (id), "the output IOProc is running");
    backend.closeAllStreams();
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the slow output's worker settles before the next test");
}

/// CA-2. One microphone that is slow past its deadline (its worker is still
/// finishing) used to make every later microphone in the same rebuild fail
/// instantly with "still finishing an earlier audio-rig operation". A healthy
/// second microphone must wait (bounded) for the first to finish, then open.
void aSlowMicDoesNotRefuseTheNextHealthyMic()
{
    std::printf ("\nA mic slow past its deadline, then a healthy mic in the same rebuild\n");
    fakeca::reset();

    auto slowSpec = microphone ("Slow Mic A", "uid-slow-mic-a", 1,
                                fakeca::BufferShape::oneChannelPerBuffer);
    // Past the simulated 250 ms input deadline, but it does finish: its worker
    // releases the transaction about 125 ms after the caller gives up, inside
    // the next mic's 250 ms wait. That leaves ~125 ms of slack on both sides:
    // 400 ms left only ~100 ms for the healthy mic (a loaded runner waited
    // 261 ms and was refused), and 300 ms left only 50 ms for the slow mic's
    // own deadline (a loaded runner woke late and saw it open).
    slowSpec.startDelayMilliseconds = 375;
    const auto slowId = fakeca::addDevice (slowSpec);
    const auto healthyId = fakeca::addDevice (microphone (
        "Healthy Mic B", "uid-healthy-mic-b", 1, fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture slowCapture, healthyCapture;
    check (! backend.openInputStream (slowSpec.uid, 48000.0, 256, slowCapture.callback()),
           "the slow mic still observes its bounded open deadline");

    const auto began = std::chrono::steady_clock::now();
    const bool healthyOpened = backend.openInputStream ("uid-healthy-mic-b", 48000.0, 256,
                                                        healthyCapture.callback());
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - began);
    std::printf ("  [measured] the healthy mic open took %lld ms\n", (long long) took.count());

    if (! healthyOpened)
        std::printf ("  [measured] refused with: %s\n", backend.getLastOpenError().c_str());

    check (healthyOpened, "the healthy mic opens once the slow mic's worker finishes");
    check (fakeca::isRunning (healthyId), "the healthy mic's IOProc is running");
    check (took < kBoundedReturn, "waiting for the earlier attempt stays bounded");

    backend.closeAllStreams();
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the slow mic's abandoned worker settles");
    check (! fakeca::isRunning (slowId), "the abandoned slow mic's late IOProc is stopped");

    // Control: an attempt that never finishes still refuses the next mic, but
    // only after a bounded wait -- never by hanging behind the wedged HAL.
    std::printf ("\nA wedged mic, then a healthy mic in the same rebuild\n");
    fakeca::reset();

    auto wedgedSpec = microphone ("Wedged Mic A", "uid-wedged-mic-a", 1,
                                  fakeca::BufferShape::oneChannelPerBuffer);
    wedgedSpec.startDelayMilliseconds = kStuckHalMilliseconds;
    fakeca::addDevice (wedgedSpec);
    fakeca::addDevice (microphone ("Healthy Mic C", "uid-healthy-mic-c", 1,
                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend wedgedBackend;
    check (! wedgedBackend.openInputStream (wedgedSpec.uid, 48000.0, 256, slowCapture.callback()),
           "the wedged mic times out");

    const auto wedgedBegan = std::chrono::steady_clock::now();
    const bool nextOpened = wedgedBackend.openInputStream ("uid-healthy-mic-c", 48000.0, 256,
                                                           healthyCapture.callback());
    check (! nextOpened, "a mic queued behind a wedged HAL is refused");
    check (std::chrono::steady_clock::now() - wedgedBegan < kBoundedReturn,
           "the refusal behind a wedged HAL is bounded");
    check (wedgedBackend.getLastOpenError().find ("still finishing") != std::string::npos,
           "the refusal says an earlier operation is still finishing");
    check (wedgedBackend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the wedged worker eventually settles");
}

void anInputPropertyCallCannotFreezeLaunch()
{
    std::printf ("\nA USB mic whose UID property call stalls\n");
    fakeca::reset();

    auto spec = microphone ("Stuck Property Mic", "uid-stuck-property", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.uidReadDelayMilliseconds = kStuckHalMilliseconds;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    const auto began = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "a property call before IOProc creation observes the same open deadline");
    check (std::chrono::steady_clock::now() - began < kBoundedReturn,
           "the stalled property query cannot hold the launch thread");
    check (backend.getLastOpenError().find ("took too long") != std::string::npos,
           "the bounded property failure explains that macOS timed out");
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the property worker eventually settles under its own lifetime");
    check (! fakeca::isRunning (id) && fakeca::propertyListenerCount (id) == 0,
           "a late property result cannot leave an IOProc or listener behind");
}

void stuckOutputCreateAndStartCallsAreBounded()
{
    std::printf ("\nOutput IOProc creation and start calls that stall\n");

    for (const bool stallCreate : { true, false })
    {
        fakeca::reset();
        auto spec = headphones (stallCreate ? "Stuck Create Out" : "Stuck Start Out",
                                stallCreate ? "uid-stuck-create-out" : "uid-stuck-start-out", 2,
                                fakeca::BufferShape::interleaved);
        if (stallCreate)
            spec.createDelayMilliseconds = kStuckHalMilliseconds;
        else
            spec.startDelayMilliseconds = kStuckHalMilliseconds;
        const auto id = fakeca::addDevice (spec);

        mma::CoreAudioBackend backend;
        const auto began = std::chrono::steady_clock::now();
        check (! backend.openExclusiveOutputStream (
                   spec.uid, 48000.0, 256,
                   [] (const float* const*, int, float* const*, int, int) {}),
               stallCreate ? "a stalled output CreateIOProc is bounded"
                           : "a stalled output Start is bounded");
        check (std::chrono::steady_clock::now() - began < kBoundedReturn,
               "the bad output cannot freeze launch");
        check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
               "the abandoned output worker owns cleanup through completion");
        check (! fakeca::isRunning (id),
               "the abandoned output has no running IOProc");
        check (! fakeca::hogModeHeld (id),
               "the abandoned output gives hog mode back");
        check (fakeca::propertyListenerCount (id) == 0,
               "the abandoned output removes all clientData listeners");
    }
}

void stuckStopAndDestroyCannotFreezeClose()
{
    std::printf ("\nOutput Stop and Destroy calls that stall\n");

    for (const bool stallStop : { true, false })
    {
        fakeca::reset();
        auto spec = headphones (stallStop ? "Stuck Stop Out" : "Stuck Destroy Out",
                                stallStop ? "uid-stuck-stop-out" : "uid-stuck-destroy-out", 2,
                                fakeca::BufferShape::interleaved);
        if (stallStop)
            spec.stopDelayMilliseconds = kStuckHalMilliseconds;
        else
            spec.destroyDelayMilliseconds = kStuckHalMilliseconds;
        const auto id = fakeca::addDevice (spec);

        mma::CoreAudioBackend backend;
        check (backend.openExclusiveOutputStream (
                   spec.uid, 48000.0, 256,
                   [] (const float* const*, int, float* const*, int, int) {}),
               "the output opens before the teardown fault");

        const auto began = std::chrono::steady_clock::now();
        backend.closeAllStreams();
        check (std::chrono::steady_clock::now() - began < kBoundedReturn,
               stallStop ? "a stalled Stop cannot freeze close"
                         : "a stalled Destroy cannot freeze close");
        check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
               "the detached close owner eventually finishes the HAL teardown");
        check (! fakeca::isRunning (id) && ! fakeca::hogModeHeld (id),
               "late teardown stops the IOProc and releases hog mode");
        check (fakeca::propertyListenerCount (id) == 0,
               "late teardown removes every clientData listener");
    }
}

void cleanupThreadCreationFailureRetainsInertClientData()
{
    std::printf ("\nA timed-out rig cannot create its detached cleanup owner\n");
    fakeca::reset();

    auto spec = microphone ("Cleanup Thread Failure", "uid-cleanup-thread-failure", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    const auto id = fakeca::addDevice (spec);
    auto stuckSpec = microphone ("Timed-out Before Cleanup", "uid-timeout-before-cleanup", 1,
                                 fakeca::BufferShape::oneChannelPerBuffer);
    stuckSpec.startDelayMilliseconds = kStuckHalMilliseconds;
    fakeca::addDevice (stuckSpec);

    mma::CoreAudioBackend backend;
    std::atomic<int> callbacksAfterOwnerRelease { 0 };
    auto owner = std::make_shared<int> (1);
    std::weak_ptr<int> weakOwner = owner;
    check (backend.openInputStream (
               spec.uid, 48000.0, 256,
               [&callbacksAfterOwnerRelease, weakOwner] (
                   const float* const*, int, float* const*, int, int)
               {
                   if (weakOwner.expired())
                       callbacksAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
               }),
           "the input is live before cleanup ownership cannot be transferred");

    Capture stuckCapture;
    check (! backend.openInputStream (
               stuckSpec.uid, 48000.0, 256, stuckCapture.callback()),
           "another interface times out before the rig is retired");

    backend.failNextCleanupWorkerStartForTesting();
    backend.closeAllStreams();
    owner.reset();

    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the original timed-out worker eventually completes independently");

    check (fakeca::isRunning (id) && fakeca::propertyListenerCount (id) == 3,
           "live HAL registrations retain their closed-gate storage instead of dangling");
    check (fakeca::pumpInput (id, { { 0.5f, 0.25f } }),
           "the simulated HAL can still call the retained IOProc");
    check (callbacksAfterOwnerRelease.load (std::memory_order_relaxed) == 0,
           "the retained IOProc is inert after its callback owner is gone");

    Capture retryCapture;
    check (! backend.openInputStream (spec.uid, 48000.0, 256, retryCapture.callback()),
           "the failed cleanup permanently quarantines further HAL transactions");
}

void systemListenerClientDataSurvivesFailedRemoval()
{
    std::printf ("\nA HAL that retains system-listener clientData\n");
    fakeca::reset();
    fakeca::setSystemPropertyListenerRemovalAllowed (false);

    std::atomic<int> callsAfterOwnerRelease { 0 };
    auto owner = std::make_shared<int> (1);
    std::weak_ptr<int> weakOwner = owner;
    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([&callsAfterOwnerRelease, weakOwner]
        {
            if (weakOwner.expired())
                callsAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
        });
        check (fakeca::systemPropertyListenerCount() == 1,
               "the system hot-plug listener is installed");
        backend.setDeviceChangeCallback (nullptr);
        check (! backend.getHotplugProblem().empty(),
               "the refused listener removal is checked and reported");
    }

    owner.reset();
    fakeca::fireDeviceListChange();
    check (fakeca::systemPropertyListenerCount() == 1,
           "the broken HAL really did retain the raw clientData");
    check (callsAfterOwnerRelease.load (std::memory_order_relaxed) == 0,
           "a retained system listener is inert after backend destruction");
}

void destructionDrainsAnActiveSystemListener()
{
    std::printf ("\nBackend destruction racing an active hot-plug callback\n");
    fakeca::reset();

    auto backend = std::make_unique<mma::CoreAudioBackend>();
    std::atomic<bool> callbackEntered { false };
    std::atomic<bool> releaseCallback { false };
    std::atomic<bool> destructionFinished { false };

    backend->setDeviceChangeCallback ([&]
    {
        callbackEntered.store (true, std::memory_order_release);
        while (! releaseCallback.load (std::memory_order_acquire))
            std::this_thread::yield();
    });

    std::thread notifier ([] { fakeca::fireDeviceListChange(); });
    while (! callbackEntered.load (std::memory_order_acquire))
        std::this_thread::yield();

    std::thread destroyer ([&]
    {
        backend.reset();
        destructionFinished.store (true, std::memory_order_release);
    });

    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    check (! destructionFinished.load (std::memory_order_acquire),
           "destruction waits for the callback lease already in flight");
    releaseCallback.store (true, std::memory_order_release);
    notifier.join();
    destroyer.join();
    check (destructionFinished.load (std::memory_order_acquire),
           "destruction completes after the active callback drains");

    fakeca::fireDeviceListChange();
    check (fakeca::systemPropertyListenerCount() == 0,
           "successful removal leaves no callback for later notifications");
}

/// A Mac that refuses the device-list listener leaves the app deaf to the rig:
/// a microphone plugged in is never noticed, and one pulled out MID-TAKE is
/// never reported, so a take that lost a channel looks like a clean one. The
/// result of installing the listener used to be discarded entirely.
void aMacThatWillNotWatchTheRigSaysSo()
{
    std::printf ("\nA Mac that refuses the device-list listener\n");
    fakeca::reset();

    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([] {});

        check (backend.getHotplugProblem().empty(),
               "a Mac that CAN watch the rig reports no problem");
    }

    fakeca::setPropertyListenersAllowed (false);

    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([] {});

        check (! backend.getHotplugProblem().empty(),
               "a Mac that cannot watch the rig says so rather than going quiet");
    }

    fakeca::setPropertyListenersAllowed (true);
}

/// The HAL is allowed to hand the IOProc a larger slice than the nominal buffer.
/// The scratch is sized at open time (§11 forbids allocating in the callback),
/// so this checks the headroom is real rather than nominal.
void aLargerThanRequestedCallbackIsStillDelivered()
{
    std::printf ("\nA device that delivers more frames than the nominal buffer\n");
    fakeca::reset();

    auto spec = microphone ("Big Block Mic", "uid-big", 2, fakeca::BufferShape::interleaved);
    spec.bufferFrameSize = 256;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-big", 48000.0, 256, capture.callback()), "the stream opens");

    fakeca::pumpInput (id, { ramp (512, 1.0f), ramp (512, 2.0f) });
    check (capture.lastFrameCount == 512, "a double-size block is de-interleaved rather than dropped");
    backend.closeAllStreams();
}

/// If one interleaved buffer is too large for the preallocated scratch, the
/// backend must not let a later valid buffer impersonate one of its channels.
void anOversizedBufferKeepsItsPhysicalChannelSlots()
{
    std::printf ("\nAn oversized interleaved buffer followed by a valid mono buffer\n");
    fakeca::reset();

    auto spec = microphone ("Mixed Buffer Mic", "uid-mixed-big", 3,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.bufferFrameSize = 256;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-mixed-big", 48000.0, 256, capture.callback()),
           "the mixed-layout stream opens");

    constexpr int frames = 4097; // one beyond the backend's fixed scratch capacity
    std::vector<float> oversizedStereo (static_cast<size_t> (frames) * 2, 0.5f);
    std::vector<float> trailingMono (static_cast<size_t> (frames), 0.75f);

    check (fakeca::pumpInputBuffers (id, { { 2, std::move (oversizedStereo) },
                                           { 1, trailingMono } }),
           "the virtual HAL delivers the mixed AudioBufferList");
    check (capture.lastChannelCount == 3,
           "the dropped stereo buffer still occupies its two physical slots");
    check (capture.lastBlock.size() == 3
           && capture.lastBlock[0].empty()
           && capture.lastBlock[1].empty(),
           "the oversized channels are explicit null placeholders");
    check (capture.lastBlock.size() == 3
           && capture.lastBlock[2] == trailingMono,
           "the later mono buffer remains physical channel three");
    check (backend.getFramesDroppedByBackend() == frames,
           "the oversized buffer is still reported as dropped");
    backend.closeAllStreams();
}

/// Eight microphones is the §1 ceiling, and the shape most likely to expose a
/// scratch-sizing or channel-indexing error that two devices would not.
void eightMicrophonesEachKeepTheirOwnAudio()
{
    std::printf ("\nEight interleaved stereo mics at once (the §1 ceiling)\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    std::vector<AudioObjectID> ids;
    std::vector<std::unique_ptr<Capture>> captures;

    for (int i = 0; i < 8; ++i)
    {
        const auto uid = "uid-mic-" + std::to_string (i);
        ids.push_back (fakeca::addDevice (microphone ("Mic " + std::to_string (i), uid, 2,
                                                      fakeca::BufferShape::interleaved)));
        captures.push_back (std::make_unique<Capture>());

        if (! backend.openInputStream (uid, 48000.0, 256, captures.back()->callback()))
        {
            check (false, "mic " + std::to_string (i) + " opens");
            return;
        }
    }

    check (true, "all eight open");

    // Each mic gets a distinct constant; a crossed pointer shows up as the
    // wrong mic's value rather than as silence.
    for (int i = 0; i < 8; ++i)
    {
        const float value = 0.1f * static_cast<float> (i + 1);
        fakeca::pumpInput (ids[static_cast<size_t> (i)],
                           { std::vector<float> (64, value), std::vector<float> (64, -value) });
    }

    bool separated = true;
    for (int i = 0; i < 8 && separated; ++i)
    {
        const auto& c = *captures[static_cast<size_t> (i)];
        const float expected = 0.1f * static_cast<float> (i + 1);

        separated = c.callbackCount == 1 && c.lastChannelCount == 2
                 && std::fabs (c.lastBlock[0][0] - expected) < 1.0e-6f
                 && std::fabs (c.lastBlock[1][0] + expected) < 1.0e-6f;
    }

    check (separated, "every mic's audio arrives on its own stream, none crossed");
    backend.closeAllStreams();
}

} // namespace

void onlyDirectlyAttachedHardwareEnumeratesAsInput()
{
    std::printf ("\nOnly directly attached hardware appears as a microphone\n");
    fakeca::reset();

    const auto addInput = [] (const char* name, const char* uid, UInt32 transport)
    {
        auto spec = microphone (name, uid, 1, fakeca::BufferShape::oneChannelPerBuffer);
        spec.transportType = transport;
        fakeca::addDevice (spec);
    };

    addInput ("USB interface", "usb", kAudioDeviceTransportTypeUSB);
    addInput ("FireWire interface", "firewire", kAudioDeviceTransportTypeFireWire);
    addInput ("Thunderbolt interface", "thunderbolt", kAudioDeviceTransportTypeThunderbolt);

    addInput ("Mac microphone", "built-in", kAudioDeviceTransportTypeBuiltIn);
    addInput ("iPhone wired", "phone-wired", kAudioDeviceTransportTypeContinuityCaptureWired);
    addInput ("iPhone wireless", "phone-wireless", kAudioDeviceTransportTypeContinuityCaptureWireless);
    addInput ("Older iPhone", "phone-legacy", kAudioDeviceTransportTypeContinuityCapture);
    addInput ("Bluetooth microphone", "bluetooth", kAudioDeviceTransportTypeBluetooth);
    addInput ("Bluetooth LE microphone", "bluetooth-le", kAudioDeviceTransportTypeBluetoothLE);
    addInput ("AirPlay input", "airplay", kAudioDeviceTransportTypeAirPlay);
    addInput ("Network microphone", "avb", kAudioDeviceTransportTypeAVB);
    addInput ("Internal sound card", "pci", kAudioDeviceTransportTypePCI);
    addInput ("Virtual cable", "virtual", kAudioDeviceTransportTypeVirtual);
    addInput ("User aggregate", "aggregate", kAudioDeviceTransportTypeAggregate);
    addInput ("Unknown source", "unknown", 0);
    addInput ("SobStage aggregate", "com.multimicaggregator.combined",
              kAudioDeviceTransportTypeAggregate);

    mma::CoreAudioBackend backend;
    const auto inputs = backend.enumerateInputDevices();

    check (inputs.size() == 3, "built-in, phone, wireless, virtual, and unknown inputs are excluded");

    bool usb = false, firewire = false, thunderbolt = false, unexpected = false;
    for (const auto& d : inputs)
    {
        usb |= d.usbLocationId == "usb";
        firewire |= d.usbLocationId == "firewire";
        thunderbolt |= d.usbLocationId == "thunderbolt";
        unexpected |= d.usbLocationId != "usb"
                   && d.usbLocationId != "firewire"
                   && d.usbLocationId != "thunderbolt";
    }

    check (usb && firewire && thunderbolt && ! unexpected,
           "USB, FireWire, and Thunderbolt inputs remain available");

    auto speakers = headphones ("Mac speakers", "speakers", 2,
                                fakeca::BufferShape::oneChannelPerBuffer);
    speakers.transportType = kAudioDeviceTransportTypeBuiltIn;
    fakeca::addDevice (speakers);

    auto wirelessHeadphones = headphones ("Bluetooth headphones", "headphones", 2,
                                          fakeca::BufferShape::interleaved);
    wirelessHeadphones.transportType = kAudioDeviceTransportTypeBluetooth;
    fakeca::addDevice (wirelessHeadphones);

    const auto outputs = backend.enumerateOutputDevices();
    check (outputs.size() == 2,
           "the input policy does not hide built-in or wireless monitor outputs");
}

void openingAnInputRechecksTheExternalHardwarePolicy()
{
    std::printf ("\nOpening a disallowed CoreAudio input directly\n");
    fakeca::reset();

    auto phone = microphone ("Continuity phone", "phone-direct-open", 1,
                             fakeca::BufferShape::oneChannelPerBuffer);
    phone.transportType = kAudioDeviceTransportTypeContinuityCaptureWired;
    const auto id = fakeca::addDevice (phone);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (! backend.openInputStream (phone.uid, 48000.0, 256, capture.callback()),
           "a caller cannot bypass discovery and open a Continuity input by UID");
    check (! fakeca::isRunning (id),
           "the rejected input never starts an IOProc");
    check (backend.getLastOpenError().find ("directly connected external") != std::string::npos,
           "the refusal explains the external-hardware policy");
}

void bitDepthFollowsWhatTheDeviceCanActuallyDeliver()
{
    // §2.3, which had no implementation at all until the depth was made to
    // follow the hardware: "do not upconvert -- it adds file size and no
    // information". §14.1's own hardware is the case that costs, and it is the
    // case a file-backed ALSA fixture cannot express, because that plugin
    // accepts every format asked of it. A simulated HAL can simply say 16.
    std::printf ("\nBit depth follows the device (§2.3)\n");
    fakeca::reset();

    auto yeti = microphone ("Blue Yeti", "uid-yeti", 1, fakeca::BufferShape::oneChannelPerBuffer);
    yeti.bitDepths = { 16 };                 // §14.1: verified 16-bit hardware
    fakeca::addDevice (yeti);

    auto iface = microphone ("Scarlett", "uid-iface", 2, fakeca::BufferShape::oneChannelPerBuffer);
    iface.bitDepths = { 16, 24 };
    fakeca::addDevice (iface);

    auto silentOnTheSubject = microphone ("No Streams", "uid-nostream", 1, fakeca::BufferShape::oneChannelPerBuffer);
    silentOnTheSubject.bitDepths = {};       // a device with nothing to ask
    fakeca::addDevice (silentOnTheSubject);

    mma::CoreAudioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    const auto depthsOf = [&devices] (const std::string& name) {
        for (const auto& d : devices)
            if (d.name == name)
                return d.supportedBitDepths;
        return std::vector<int>{};
    };

    const auto yetiDepths = depthsOf ("Blue Yeti");
    check (yetiDepths == std::vector<int> { 16 },
           "a 16-bit microphone reports 16 and not the old hardcoded list");
    check (mma::SampleFormat::chooseRecordingBitDepth (yetiDepths, 24) == 16,
           "so its stem is written at 16, not padded out to 24 for nothing");

    const auto ifaceDepths = depthsOf ("Scarlett");
    check (mma::SampleFormat::chooseRecordingBitDepth (ifaceDepths, 24) == 24,
           "an interface that can do 24 still gets 24");

    // The safe direction. A device the HAL cannot be asked about must not be
    // read as a limited one: empty means "not reported", and guessing low here
    // would quietly halve the depth of a recording.
    check (depthsOf ("No Streams").empty(),
           "a device with no stream to ask reports nothing rather than guessing");
    check (mma::SampleFormat::chooseRecordingBitDepth (depthsOf ("No Streams"), 24) == 24,
           "and that falls back to the take's depth, unchanged");
}

void outputsCarryWhatSection53NeedsToFindTheHeadphones()
{
    // §5.3 on a Mac. Nothing here used to report a headphone jack, the macOS
    // default output, or a wireless transport, so with no saved choice the
    // selector fell through to "first built-in output" -- on a MacBook Air M2
    // that is usually "MacBook Air Speakers", and the live-mic mix played into
    // the room while the performers' headphones stayed silent.
    std::printf ("\nOutputs report headphone jack, macOS default, and wireless (§5.3)\n");
    fakeca::reset();

    const auto fourCC = [] (char a, char b, char c, char d)
    {
        return (static_cast<UInt32> (static_cast<unsigned char> (a)) << 24)
             | (static_cast<UInt32> (static_cast<unsigned char> (b)) << 16)
             | (static_cast<UInt32> (static_cast<unsigned char> (c)) << 8)
             |  static_cast<UInt32> (static_cast<unsigned char> (d));
    };

    // Apple Silicon: two separate built-in devices. The speakers exist from
    // boot, so they enumerate first.
    auto speakers = headphones ("MacBook Air Speakers", "BuiltInSpeakerDevice", 2,
                                fakeca::BufferShape::oneChannelPerBuffer);
    speakers.transportType = kAudioDeviceTransportTypeBuiltIn;
    speakers.outputDataSource = fourCC ('i', 's', 'p', 'k');
    fakeca::addDevice (speakers);

    auto jack = headphones ("External Headphones", "BuiltInHeadphoneOutputDevice", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    jack.transportType = kAudioDeviceTransportTypeBuiltIn;
    fakeca::addDevice (jack);

    // Intel: one "Built-in Output" whose data source follows the jack.
    auto intel = headphones ("Built-in Output", "AppleHDAEngineOutput:1B,0,1,1:0", 2,
                             fakeca::BufferShape::oneChannelPerBuffer);
    intel.transportType = kAudioDeviceTransportTypeBuiltIn;
    intel.outputDataSource = fourCC ('h', 'd', 'p', 'n');
    fakeca::addDevice (intel);

    // A USB headphone amp and a capture card: USB outputs are not marked as
    // jacks, or the capture-card rule in OutputDeviceSelector would break.
    auto amp = headphones ("USB Headphone Amp", "usb-amp", 2,
                           fakeca::BufferShape::oneChannelPerBuffer);
    const auto ampId = fakeca::addDevice (amp);

    auto airpods = headphones ("AirPods", "airpods", 2, fakeca::BufferShape::interleaved);
    airpods.transportType = kAudioDeviceTransportTypeBluetooth;
    fakeca::addDevice (airpods);

    auto bleSpeaker = headphones ("LE speaker", "ble-speaker", 2, fakeca::BufferShape::interleaved);
    bleSpeaker.transportType = kAudioDeviceTransportTypeBluetoothLE;
    fakeca::addDevice (bleSpeaker);

    auto appleTv = headphones ("Living Room", "airplay", 2, fakeca::BufferShape::interleaved);
    appleTv.transportType = kAudioDeviceTransportTypeAirPlay;
    fakeca::addDevice (appleTv);

    fakeca::setDefaultOutputDevice (ampId);

    mma::CoreAudioBackend backend;
    auto outputs = backend.enumerateOutputDevices();

    const auto find = [&outputs] (const std::string& uid) -> const mma::AudioDeviceDescriptor*
    {
        for (const auto& d : outputs)
            if (d.usbLocationId == uid)
                return &d;
        return nullptr;
    };

    const auto* s = find ("BuiltInSpeakerDevice");
    const auto* h = find ("BuiltInHeadphoneOutputDevice");
    const auto* i = find ("AppleHDAEngineOutput:1B,0,1,1:0");
    const auto* a = find ("usb-amp");
    const auto* bt = find ("airpods");
    const auto* le = find ("ble-speaker");
    const auto* ap = find ("airplay");

    check (s && h && i && a && bt && le && ap, "every output is enumerated");
    if (! (s && h && i && a && bt && le && ap))
        return;

    check (h->hasPhysicalHeadphoneJack, "Apple Silicon External Headphones is a headphone jack");
    check (! s->hasPhysicalHeadphoneJack, "the MacBook speakers are not");
    check (i->hasPhysicalHeadphoneJack, "an Intel built-in output routed to headphones is a headphone jack");
    check (! a->hasPhysicalHeadphoneJack, "a USB output is not marked as a jack (capture-card rule)");

    check (a->isSystemDefault, "the macOS default output is reported");
    check (! s->isSystemDefault && ! h->isSystemDefault && ! bt->isSystemDefault,
           "and only that one");

    check (bt->isWireless && le->isWireless && ap->isWireless,
           "Bluetooth, Bluetooth LE and AirPlay outputs are marked wireless");
    check (! s->isWireless && ! h->isWireless && ! a->isWireless,
           "built-in and USB outputs are not");

    // An Intel output with the speakers selected is not a jack, and the
    // default follows macOS when it moves to the headphones.
    fakeca::reset();
    intel.outputDataSource = fourCC ('i', 's', 'p', 'k');
    fakeca::addDevice (intel);
    const auto headphonesId = fakeca::addDevice (jack);
    fakeca::setDefaultOutputDevice (headphonesId);

    outputs = backend.enumerateOutputDevices();
    const auto* intelSpeakers = find ("AppleHDAEngineOutput:1B,0,1,1:0");
    const auto* jackAgain = find ("BuiltInHeadphoneOutputDevice");
    check (intelSpeakers != nullptr && ! intelSpeakers->hasPhysicalHeadphoneJack
               && ! intelSpeakers->isSystemDefault,
           "an Intel built-in output on its speakers is neither a jack nor the default");
    check (jackAgain != nullptr && jackAgain->isSystemDefault,
           "the default output follows macOS");

    // Inputs never carry output-only flags, even on a duplex device that is
    // also the default output.
    fakeca::reset();
    auto duplex = microphone ("Mixer", "mixer", 2, fakeca::BufferShape::oneChannelPerBuffer);
    duplex.outputChannels = 2;
    const auto mixerId = fakeca::addDevice (duplex);
    fakeca::setDefaultOutputDevice (mixerId);
    const auto inputs = backend.enumerateInputDevices();
    check (inputs.size() == 1 && ! inputs[0].isSystemDefault && ! inputs[0].hasPhysicalHeadphoneJack,
           "an input descriptor never carries the output default or jack flags");
}

/// What Application::reselectOutputDevice does with the backend's outputs, kept
/// across passes the way the app keeps it: the tracker, the last pick, and
/// whether that pick was a headphone jack.
struct MonitorPicker
{
    mma::OutputDeviceTracker tracker;
    std::string current;
    bool currentWasJack = false;

    mma::OutputSelection pick (mma::CoreAudioBackend& backend)
    {
        std::vector<mma::OutputDeviceCandidate> snapshot;
        for (const auto& d : backend.enumerateOutputDevices())
            snapshot.push_back (mma::OutputDeviceSelector::candidateFromDescriptor (d, 48000, {}));

        const auto candidates = tracker.observe (std::move (snapshot));
        const auto selection = mma::OutputDeviceSelector::select (
            candidates, {}, mma::OutputDeviceSelector::currentIdToKeep (candidates, current, currentWasJack));

        current = selection.id;
        currentWasJack = std::any_of (candidates.begin(), candidates.end(),
                                      [&selection] (const mma::OutputDeviceCandidate& c)
                                      { return c.id == selection.id && c.hasPhysicalHeadphoneJack; });
        return selection;
    }
};

/// An Intel Mac's headphone jack. Plugging in moves "Built-in Output" from its
/// speakers ('ispk') to headphones ('hdpn') and changes no device list, so the
/// device-list listener alone never heard it: the monitor stayed where it was
/// until some unrelated device came or went.
void headphonesInAnIntelJackMoveTheMonitorWithoutADeviceListChange()
{
    std::printf ("\nHeadphones plugged into an Intel Mac's built-in jack\n");
    fakeca::reset();

    const UInt32 speakersSource = mmaFourCC ('i', 's', 'p', 'k');
    const UInt32 headphonesSource = mmaFourCC ('h', 'd', 'p', 'n');

    // The macOS default is a USB amp; the built-in output is on its speakers.
    const auto ampId = fakeca::addDevice (headphones ("USB Headphone Amp", "usb-amp", 2,
                                                      fakeca::BufferShape::oneChannelPerBuffer));
    fakeca::setDefaultOutputDevice (ampId);

    auto intel = headphones ("Built-in Output", "AppleHDAEngineOutput:1B,0,1,1:0", 2,
                             fakeca::BufferShape::oneChannelPerBuffer);
    intel.transportType = kAudioDeviceTransportTypeBuiltIn;
    intel.outputDataSource = speakersSource;
    const auto intelId = fakeca::addDevice (intel);

    auto owner = std::make_shared<int> (1);
    std::weak_ptr<int> weakOwner = owner;
    std::atomic<int> notifications { 0 };
    std::atomic<int> callsAfterOwnerRelease { 0 };

    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([&notifications, &callsAfterOwnerRelease, weakOwner]
        {
            if (weakOwner.expired())
                callsAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
            notifications.fetch_add (1, std::memory_order_relaxed);
        });

        MonitorPicker picker;
        auto selection = picker.pick (backend); // the launch pass
        check (selection.found && selection.id == "usb-amp",
               "with the jack empty the monitor starts on the macOS default (the amp)");
        check (fakeca::propertyListenerCount (intelId) == 1,
               "the built-in output's data source is watched");
        check (fakeca::propertyListenerCount (ampId) == 0,
               "an output with no data source or jack property is not");

        const auto outputsBefore = backend.enumerateOutputDevices().size();

        // Plug in. Only the data source changes.
        auto before = notifications.load();
        fakeca::setOutputDataSource (intelId, headphonesSource);
        check (notifications.load() > before,
               "plugging into the jack reaches the app with no device-list change");
        check (backend.enumerateOutputDevices().size() == outputsBefore,
               "and the device list really is unchanged");

        selection = picker.pick (backend); // what the app's re-evaluation does
        check (selection.id == intel.uid
                   && selection.reason == mma::OutputSelectionReason::PhysicalHeadphoneJack,
               "the re-evaluation moves the monitor to the headphones in the built-in jack");

        selection = picker.pick (backend);
        check (selection.id == intel.uid, "and an unrelated later pass keeps it there");

        // Unplug. The same device, back on its speakers.
        before = notifications.load();
        fakeca::setOutputDataSource (intelId, speakersSource);
        check (notifications.load() > before, "unplugging reaches the app too");

        selection = picker.pick (backend);
        check (selection.id == "usb-amp"
                   && selection.reason == mma::OutputSelectionReason::SystemDefault,
               "unplugged, the monitor goes back to the macOS default, not the room speakers");

        // coreaudiod restarts: the route listener went with the old service,
        // though the built-in output kept its AudioObjectID.
        before = notifications.load();
        fakeca::restartService();
        check (notifications.load() > before && fakeca::propertyListenerCount (intelId) == 0,
               "a coreaudiod restart is heard, and the old route listener is gone");
        (void) picker.pick (backend); // the rescan the restart causes
        check (fakeca::propertyListenerCount (intelId) == 1,
               "the rescan after the restart watches the jack again");
        before = notifications.load();
        fakeca::setOutputDataSource (intelId, headphonesSource);
        check (notifications.load() > before && picker.pick (backend).id == intel.uid,
               "and the jack moves the monitor after the restart too");
        fakeca::setOutputDataSource (intelId, speakersSource);
        (void) picker.pick (backend);

        // A Mac that also reports the jack itself.
        auto jackMac = headphones ("Built-in Line Output", "builtin-line", 2,
                                   fakeca::BufferShape::oneChannelPerBuffer);
        jackMac.transportType = kAudioDeviceTransportTypeBuiltIn;
        jackMac.outputDataSource = speakersSource;
        jackMac.jackConnected = 0;
        const auto jackMacId = fakeca::addDevice (jackMac);
        (void) picker.pick (backend); // the rescan the arrival causes

        check (fakeca::propertyListenerCount (jackMacId) == 2,
               "an output hot-plugged later is watched for its data source and its jack");

        before = notifications.load();
        fakeca::setJackConnected (jackMacId, true);
        check (notifications.load() > before, "a JackIsConnected change reaches the app as well");

        // Gone: its registration is dropped on the next rescan. (The HAL
        // refuses the remove for an object that no longer exists, which the
        // backend must tolerate.)
        fakeca::removeDevice (jackMacId);
        check (! backend.enumerateOutputDevices().empty(),
               "a rescan after a watched output left still works");

        // Stopping the watch removes the route listeners before the state goes.
        backend.setDeviceChangeCallback (nullptr);
        check (fakeca::propertyListenerCount (intelId) == 0,
               "removing the callback removes the route listeners");

        before = notifications.load();
        fakeca::setOutputDataSource (intelId, headphonesSource);
        check (notifications.load() == before, "and a later jack change calls nobody");

        // Watching again, then destroying the backend with it in place.
        backend.setDeviceChangeCallback ([&notifications, &callsAfterOwnerRelease, weakOwner]
        {
            if (weakOwner.expired())
                callsAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
            notifications.fetch_add (1, std::memory_order_relaxed);
        });
        check (fakeca::propertyListenerCount (intelId) == 1,
               "a new callback watches the outputs again straight away");
    }

    check (fakeca::propertyListenerCount (intelId) == 0,
           "destroying the backend removes its route listeners");

    owner.reset();
    fakeca::setOutputDataSource (intelId, speakersSource);
    check (callsAfterOwnerRelease.load() == 0,
           "a jack change after the backend is gone reaches nothing");
}

/// `sudo killall coreaudiod`, or the combined device deleted in Audio MIDI
/// Setup. The app went on believing it existed, and the "nothing changed"
/// guard never made it again until the rig changed.
void aCombinedDeviceTakenAwayOutsideTheAppIsMadeAgain()
{
    std::printf ("\nThe combined device removed outside the app\n");
    fakeca::reset();
    namespace q = mma::macaudio;

    auto micA = microphone ("Mic A", "mic-a", 1, fakeca::BufferShape::oneChannelPerBuffer);
    micA.outputChannels = 2;
    auto micB = microphone ("Mic B", "mic-b", 1, fakeca::BufferShape::oneChannelPerBuffer);
    micB.outputChannels = 2;
    fakeca::addDevice (micA);
    fakeca::addDevice (micB);

    mma::CoreAudioBackend backend;
    std::atomic<int> notifications { 0 };
    backend.setDeviceChangeCallback ([&notifications] { notifications.fetch_add (1); });
    check (fakeca::serviceRestartListenerCount() == 1, "coreaudiod restarts are watched");

    auto aggregate = mma::createSystemAggregateDevice();
    const std::vector<std::string> uids { "mic-a", "mic-b" };

    // Application::rescanDevices on every notification: forget a lost device,
    // then publish only when what would be published differs.
    std::vector<std::string> published;
    const auto rescan = [&]
    {
        if (aggregate->forgetIfLost())
            published.clear();

        if (uids != published && aggregate->publish ("SobStage", uids, {}))
            published = uids;
    };

    rescan();
    const auto first = q::findDeviceByUID (mma::kOurAggregateUid);
    check (first != kAudioObjectUnknown && fakeca::aggregatesCreated() == 1,
           "the combined device is published");

    rescan();
    check (fakeca::aggregatesCreated() == 1 && ! aggregate->forgetIfLost(),
           "while it is there, nothing is re-made");

    // Deleted outside the app: the device-list listener fires.
    auto before = notifications.load();
    fakeca::removeDevice (first);
    check (notifications.load() > before, "its removal reaches the app");
    rescan();
    check (fakeca::aggregatesCreated() == 2 && aggregate->isPublished()
               && q::findDeviceByUID (mma::kOurAggregateUid) != kAudioObjectUnknown,
           "the app forgets it and makes it again");
    check (mma::combinedLayoutIsComplete (aggregate->getOutputLayout(), uids),
           "with every microphone's headphone jack in it");

    // coreaudiod restarts, and that notification is the only one.
    before = notifications.load();
    fakeca::restartService();
    check (notifications.load() > before,
           "a coreaudiod restart alone reaches the app, with no device-list change");
    check (q::findDeviceByUID (mma::kOurAggregateUid) == kAudioObjectUnknown,
           "and the combined device is gone");
    rescan();
    check (fakeca::aggregatesCreated() == 3 && aggregate->isPublished()
               && q::findDeviceByUID (mma::kOurAggregateUid) != kAudioObjectUnknown,
           "the app makes it again after the restart");

    // A restart that brings ours back under a new AudioObjectID: the ID held
    // is stale, and the one now there would refuse a create with our UID.
    fakeca::removeDevice (q::findDeviceByUID (mma::kOurAggregateUid));
    fakeca::DeviceSpec restored;
    restored.name = "SobStage";
    restored.uid = mma::kOurAggregateUid;
    restored.transportType = kAudioDeviceTransportTypeAggregate;
    restored.subDeviceUids = uids;
    fakeca::addDevice (restored);
    rescan();
    check (fakeca::aggregatesCreated() == 4 && aggregate->isPublished(),
           "one restored under a new ID is cleared and made afresh");

    int ours = 0;
    for (const auto& d : backend.enumerateOutputDevices())
        if (d.usbLocationId == mma::kOurAggregateUid)
            ++ours;
    check (ours == 0, "and the backend still never lists it as an output");

    aggregate.reset();
    check (q::findDeviceByUID (mma::kOurAggregateUid) == kAudioObjectUnknown,
           "the combined device is taken away at quit");

    backend.setDeviceChangeCallback (nullptr);
    check (fakeca::serviceRestartListenerCount() == 0,
           "the restart listener is removed with the device-list listener");
}

int main()
{
    std::printf ("CoreAudio backend, driven against a virtual HAL\n");
    std::printf ("===============================================\n");

    onlyDirectlyAttachedHardwareEnumeratesAsInput();
    outputsCarryWhatSection53NeedsToFindTheHeadphones();
    openingAnInputRechecksTheExternalHardwarePolicy();
    interleavedStereoMicrophoneDeliversBothChannels();
    oneChannelPerBufferStillWorks();
    interleavedOutputCarriesTheMonitorMix();
    continuousSampleRateRangeIsExpanded();
    aDeviceAlreadyAtTheRequestedRateStillOpens();
    aDelayedRateChangeSettlesBeforeTheStreamOpens();
    aRateChangeThatNeverSettlesTimesOut();
    aDeviceThatCannotReachTheRateIsRefused();
    aMicrophoneThatVanishedSaysSo();
    aMicUnpluggedWhileItsRateSettlesSaysItDisconnected();
    aDeviceAt44100ReportsThatAsItsCurrentRate();
    hogModeRefusalFailsTheOpenAndExplainsItself();
    hogModeIsTakenAndReleased();
    hogModeIsNotTakenOnAMicrophonesOwnOutput();
    aCombinedDeviceThatActivatesLateIsWaitedFor();
    theCombinedDeviceChangingShapeReachesTheApp();
    theReportedLatencyIsTheRoundTrip();
    aFixedRateOutputIsNotPromisedForMonitoring();
    anOutputThatSupportsTheRateIsStillOffered();
    anOutputThatReportsNoRatesIsNotRefused();
    anOutputWeAlreadyHoldIsStillReportedAsAvailable();
    hotplugArrivesThroughTheOsListener();
    aLiveSampleRateChangeIsReportedAndReenumerated();
    aLiveOutputRateChangeIsReportedAsTheOutputsAndSoIsItsReturn();
    aDeviceAliveChangeIsReportedImmediately();
    aMicWhoseAliveFlagDipsAndReturnsIsReportedAsResumed();
    processorOverloadsAreCountedOnTheRightPath();
    processorOverloadNotificationIsThreadSafe();
    aDeviceThatRefusesSafetyListenersSaysSo();
    aMacThatWillNotWatchTheRigSaysSo();
    aMicrophoneThatGoesQuietAfterOpeningIsReported();
    aMicrophoneWhoseFirstCallbackNeverArrivesIsReported();
    aMacThatSleepsAndWakesDoesNotAccuseItsDevices();
    aDarkWakeIsNotAFailureButAMicThatNeverReturnsIs();
    aWakeAfterWhichNothingStartsAgainIsStillReported();
    aMicCalledDeadBeforeSleepThatReturnsAfterWakeIsResumed();
    aStuckInputStartIsBoundedAndCleanedUp();
    aDriverThatRetainsListenerClientDataIsQuarantined();
    anUnpluggedInterfaceDoesNotQuarantineTheSurvivor();
    aTimedOutUnsafeInputCannotBeUnquarantinedByOtherCleanup();
    aSlowButHealthyOutputRateSwitchOpens();
    aSlowMicDoesNotRefuseTheNextHealthyMic();
    anInputPropertyCallCannotFreezeLaunch();
    stuckOutputCreateAndStartCallsAreBounded();
    stuckStopAndDestroyCannotFreezeClose();
    cleanupThreadCreationFailureRetainsInertClientData();
    systemListenerClientDataSurvivesFailedRemoval();
    destructionDrainsAnActiveSystemListener();
    aLargerThanRequestedCallbackIsStillDelivered();
    anOversizedBufferKeepsItsPhysicalChannelSlots();
    bitDepthFollowsWhatTheDeviceCanActuallyDeliver();
    eightMicrophonesEachKeepTheirOwnAudio();
    headphonesInAnIntelJackMoveTheMonitorWithoutADeviceListChange();
    aCombinedDeviceTakenAwayOutsideTheAppIsMadeAgain();

    // Tear the last scenario down so a leak check sees only what the
    // backend failed to release, not what the harness never cleaned up.
    fakeca::reset();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
