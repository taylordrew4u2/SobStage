// Runs the real WasapiAsioBackend against a virtual WASAPI endpoint layer.
//
// The backend source is compiled unmodified; only the OS headers are replaced
// (Simulation/Wasapi). The backend's own worker thread runs for real, so the
// event handshake, the exclusive-mode negotiation and the fixed-point
// conversion are all exercised rather than reasoned about.

#include "../Simulation/Wasapi/FakeWasapi.h"
#include "../Source/Core/SampleFormat.h"
#include "../Source/Platform/WasapiAsioBackend.h"

// The simulated Windows headers, for the endpoint states and error codes.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

/// How long the wedged driver hangs, and what a bounded caller is allowed.
/// Kept far apart on purpose: a runner under load must not be able to turn
/// "returned bounded" into "waited for the driver".
constexpr int kStuckDriverMilliseconds = 8000;
constexpr auto kBoundedOpen = std::chrono::milliseconds (3000);

void check (bool condition, const std::string& what)
{
    ++checks;
    std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (! condition)
        ++failures;
}

std::vector<float> tone (int frames, float amplitude, float phase)
{
    constexpr float kTwoPi = 6.283185307179586f;
    std::vector<float> v (static_cast<size_t> (frames));

    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t> (i)] =
            amplitude * std::sin (kTwoPi * (static_cast<float> (i) / 64.0f) + phase);

    return v;
}

struct Capture
{
    std::atomic<int> callbackCount { 0 };
    std::mutex mutex;
    std::vector<std::vector<float>> lastBlock;
    int lastChannelCount = 0;

    mma::AudioCallback callback()
    {
        return [this] (const float* const* inputs, int numInputs,
                       float* const*, int, int numSamples)
        {
            std::lock_guard<std::mutex> lock (mutex);
            lastChannelCount = numInputs;
            lastBlock.assign (static_cast<size_t> (numInputs), {});

            for (int ch = 0; ch < numInputs; ++ch)
                lastBlock[static_cast<size_t> (ch)].assign (inputs[ch], inputs[ch] + numSamples);

            callbackCount.fetch_add (1);
        };
    }

    std::vector<std::vector<float>> block()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return lastBlock;
    }
};

fakewasapi::EndpointSpec microphone (const std::string& id, const std::string& name,
                                     std::vector<fakewasapi::Format> formats)
{
    fakewasapi::EndpointSpec spec;
    spec.id = id;
    spec.friendlyName = name;
    spec.isCapture = true;
    spec.exclusiveFormats = std::move (formats);
    if (! spec.exclusiveFormats.empty())
        spec.mixFormat = spec.exclusiveFormats.front();
    return spec;
}

fakewasapi::EndpointSpec headphones (const std::string& id, const std::string& name,
                                     std::vector<fakewasapi::Format> formats)
{
    fakewasapi::EndpointSpec spec;
    spec.id = id;
    spec.friendlyName = name;
    spec.isCapture = false;
    spec.exclusiveFormats = std::move (formats);
    if (! spec.exclusiveFormats.empty())
        spec.mixFormat = spec.exclusiveFormats.front();
    return spec;
}

bool closeEnough (const std::vector<float>& a, const std::vector<float>& b, float tolerance)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs (a[i] - b[i]) > tolerance)
            return false;

    return true;
}

// --- Scenarios --------------------------------------------------------------

/// The defect that stopped most USB microphones opening at all: exclusive mode
/// performs no conversion, and the backend offered only float32, so a 24-bit
/// device's IsFormatSupported refused and the mic was simply unusable.
void a24BitOnlyMicrophoneOpensAndDeliversAudio()
{
    std::printf ("\nA 24-bit-only USB microphone (what most of them are)\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-24", "24-bit Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-24", 48000.0, 256, capture.callback()),
           "the stream opens instead of being refused");

    const auto negotiated = fakewasapi::negotiatedFormat ("mic-24");
    check (negotiated.containerBits == 24 && ! negotiated.isFloat,
           "24-bit PCM is what was negotiated");
    check (fakewasapi::openedExclusive ("mic-24"), "and exclusively, per §5.4");

    const auto signal = tone (128, 0.5f, 0.0f);
    check (fakewasapi::pushCapture ("mic-24", { signal }), "a packet reaches the worker thread");
    check (capture.callbackCount.load() == 1, "and the audio callback fires");

    const auto block = capture.block();
    check (block.size() == 1 && closeEnough (block[0], signal, 1.0e-6f),
           "the samples survive the 24-bit round trip");

    backend.closeAllStreams();
}

/// 16-bit is the other common case, and the one where a scale or sign error is
/// loudest.
void a16BitMicrophoneRoundTripsWithinItsQuantisation()
{
    std::printf ("\nA 16-bit microphone\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-16", "16-bit Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-16", 48000.0, 256, capture.callback()), "the stream opens");
    check (fakewasapi::negotiatedFormat ("mic-16").containerBits == 16, "16-bit PCM negotiated");

    const auto signal = tone (128, 0.8f, 1.0f);
    fakewasapi::pushCapture ("mic-16", { signal });

    const auto block = capture.block();
    check (block.size() == 1 && closeEnough (block[0], signal, 2.0f / 32768.0f),
           "the samples survive within one quantisation step");

    backend.closeAllStreams();
}

/// A device that does speak float must still get float: the fallback is a
/// descent, not a downgrade.
void aFloatCapableDeviceStillGetsFloat()
{
    std::printf ("\nAn interface that accepts float32\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-f32", "Float Mic",
                                         { fakewasapi::Format::floatFormat (1, 48000.0),
                                           fakewasapi::Format::pcm (1, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-f32", 48000.0, 256, capture.callback()), "the stream opens");

    const auto negotiated = fakewasapi::negotiatedFormat ("mic-f32");
    check (negotiated.isFloat && negotiated.containerBits == 32,
           "float32 is preferred over the 16-bit fallback it also offers");

    backend.closeAllStreams();
}

/// Many microphones present as stereo only. The backend asks for one channel
/// first and must fall back to the device's own count rather than give up.
void aStereoOnlyMicrophoneIsOpenedAsStereo()
{
    std::printf ("\nA microphone that will only do stereo\n");
    fakewasapi::reset();

    auto spec = microphone ("mic-stereo", "Stereo Only Mic",
                            { fakewasapi::Format::pcm (2, 24, 48000.0) });
    spec.mixFormat = fakewasapi::Format::pcm (2, 24, 48000.0);
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-stereo", 48000.0, 256, capture.callback()),
           "the stream opens on the device's own channel count");
    check (fakewasapi::negotiatedFormat ("mic-stereo").channels == 2, "two channels negotiated");

    const auto left = tone (96, 0.4f, 0.0f);
    const auto right = tone (96, 0.2f, 2.0f);
    fakewasapi::pushCapture ("mic-stereo", { left, right });

    const auto block = capture.block();
    check (block.size() == 2, "both channels reach the callback");
    check (block.size() == 2 && closeEnough (block[0], left, 1.0e-6f)
                             && closeEnough (block[1], right, 1.0e-6f),
           "and neither channel carries the other's audio");

    backend.closeAllStreams();
}

/// A device that accepts nothing exclusively must fail with an explanation
/// rather than silently falling back to shared mode (§5.4).
void aDeviceThatAcceptsNothingFailsWithAReason()
{
    std::printf ("\nA device that refuses every exclusive format\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-none", "Busy Mic", {}));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("mic-none", 48000.0, 256, capture.callback()),
           "the open fails rather than dropping to shared mode");
    check (! backend.getLastOpenError().empty(), "and leaves a message naming a next step");
    check (! fakewasapi::isRunning ("mic-none"), "with no worker thread left running");
}

/// The device rejects the requested period and names its own; the backend must
/// retry at that size rather than abandon exclusive mode.
void aBufferAlignmentRejectionIsRetried()
{
    std::printf ("\nA device that rejects the period and names its own\n");
    fakewasapi::reset();

    auto spec = microphone ("mic-align", "Picky Mic", { fakewasapi::Format::pcm (1, 32, 48000.0) });
    spec.alignedFrames = 480;
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-align", 48000.0, 256, capture.callback()),
           "the retry at the device's own size succeeds");
    check (fakewasapi::isRunning ("mic-align"), "and the stream starts");

    backend.closeAllStreams();
}

/// A device whose re-Activate fails between the rejected period and the retry.
/// The open fails -- and must say so: the backend returned without a reason.
void aFailedReactivationSaysWhy()
{
    std::printf ("\nA device that goes away between the rejected period and the retry\n");
    fakewasapi::reset();

    auto spec = microphone ("mic-gone", "Vanishing Mic", { fakewasapi::Format::pcm (1, 32, 48000.0) });
    spec.alignedFrames = 480;
    spec.failActivateAfterAlignment = true;
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("mic-gone", 48000.0, 256, capture.callback()),
           "the open fails");
    check (! backend.getLastOpenError().empty(), "and says why, rather than nothing");
}

/// §14.2: a bus-powered microphone on a starved hub browns out as it starts
/// drawing current -- that is, while it is being opened. Windows still hands
/// the backend the endpoint (GetDevice finds ones no longer present), so the
/// open used to fail later and be called "not external" or "permission",
/// which the power warning never counts. Either way it is a device gone.
void aMicrophoneThatDropsOffWhileOpeningCountsTowardThePowerWarning()
{
    std::printf ("\nA microphone that drops off the bus while it is being opened\n");

    {
        fakewasapi::reset();
        auto spec = microphone ("mic-brownout", "Kitchen", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.deviceState = DEVICE_STATE_NOTPRESENT;
        spec.devNodeLookupAvailable = false; // its device node went with it
        fakewasapi::addEndpoint (spec);

        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream ("mic-brownout", 48000.0, 256, capture.callback()),
               "an endpoint no longer present does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::gone, "it is called gone");
        check (mma::countsAsPossiblePowerLoss (backend.getLastOpenRefusal()),
               "and counts toward the power warning");
    }

    {
        fakewasapi::reset();
        auto spec = microphone ("mic-invalidated", "Hall", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.activateError = static_cast<long> (AUDCLNT_E_DEVICE_INVALIDATED);
        fakewasapi::addEndpoint (spec);

        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream ("mic-invalidated", 48000.0, 256, capture.callback()),
               "a microphone invalidated as it is activated does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::gone, "it is called gone, not permission");
        check (backend.getLastOpenError().find ("Privacy") == std::string::npos,
               "and the user is not sent to the privacy settings");
    }

    // Activated, then gone while its formats are asked about: every
    // IsFormatSupported answers "invalidated". It was called a refused sample
    // rate -- settingsRefused, which the power warning never counts -- and the
    // user was told to change a setting that had nothing to do with it.
    {
        fakewasapi::reset();
        auto spec = microphone ("mic-format-gone", "Porch", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.formatQuestionError = static_cast<long> (AUDCLNT_E_DEVICE_INVALIDATED);
        fakewasapi::addEndpoint (spec);

        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream ("mic-format-gone", 48000.0, 256, capture.callback()),
               "a microphone invalidated while its formats are asked about does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::gone,
               "it is called gone, not a refused sample rate");
        check (mma::countsAsPossiblePowerLoss (backend.getLastOpenRefusal()),
               "and counts toward the power warning");
        check (backend.getLastOpenError().find ("sample rate") == std::string::npos,
               "and the user is not told to change the sample rate");
    }

    // The same, seen only in its state: the format questions are refused
    // plainly, but the endpoint is no longer present once they are done.
    {
        fakewasapi::reset();
        auto spec = microphone ("mic-format-left", "Porch", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.formatQuestionError = static_cast<long> (AUDCLNT_E_UNSUPPORTED_FORMAT);
        spec.goneAfterFormatQuestion = true;
        fakewasapi::addEndpoint (spec);

        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream ("mic-format-left", 48000.0, 256, capture.callback()),
               "a microphone gone by the end of the format questions does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::gone, "it is called gone too");
    }

    // A real refusal of every format, the device still there: still the setting.
    {
        fakewasapi::reset();
        auto spec = microphone ("mic-no-format", "Porch", { fakewasapi::Format::pcm (1, 24, 44100.0) });
        fakewasapi::addEndpoint (spec);

        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream ("mic-no-format", 48000.0, 256, capture.callback()),
               "a microphone that takes no format at this rate does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::settingsRefused,
               "and that is still the setting, not power");
    }

    {
        fakewasapi::reset();
        auto spec = microphone ("mic-denied", "Stage", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.activateError = static_cast<long> (E_ACCESSDENIED);
        fakewasapi::addEndpoint (spec);

        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream ("mic-denied", 48000.0, 256, capture.callback()),
               "a refused microphone does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::permission,
               "an access refusal is still permission");
        check (! mma::countsAsPossiblePowerLoss (backend.getLastOpenRefusal()),
               "which more power would not fix");
    }
}

/// §14.2: Windows' microphone privacy settings refuse a desktop app where
/// Windows chooses -- most often at IAudioClient::Initialize, Activate having
/// succeeded ("Failed to initialize audio client: 80070005"), and sometimes
/// in answer to the format questions or a second Activate. Each is the
/// privacy setting, never a microphone that lost power: called "unknown", it
/// counted toward the power warning, and three refused microphones read as a
/// hub that needed its own power adapter.
void aMicrophoneThePrivacySettingsRefuseIsNeverCountedAsPower()
{
    std::printf ("\nA microphone Windows' privacy settings refuse\n");

    const auto refusedAs = [] (const char* where, const fakewasapi::EndpointSpec& spec)
    {
        mma::WasapiAsioBackend backend;
        Capture capture;
        check (! backend.openInputStream (spec.id, 48000.0, 256, capture.callback()),
               std::string ("refused at ") + where + ": it does not open");
        check (backend.getLastOpenRefusal() == mma::OpenRefusal::permission,
               std::string ("refused at ") + where + ": it is called permission");
        check (! mma::countsAsPossiblePowerLoss (backend.getLastOpenRefusal()),
               std::string ("refused at ") + where + ": it does not count toward the power warning");
        check (backend.getLastOpenError().find ("Let desktop apps access your microphone") != std::string::npos,
               std::string ("refused at ") + where + ": the user is sent to the privacy switch for desktop apps");
        check (backend.getLastOpenError().find ("plug it back in") == std::string::npos
                   && backend.getLastOpenError().find ("sample rate") == std::string::npos,
               std::string ("refused at ") + where + ": not to unplug it, nor to change the sample rate");
    };

    // Three microphones on one hub, each refused at Initialize: none of them
    // is a power loss.
    {
        fakewasapi::reset();
        const char* names[][2] = { { "mic-kitchen", "Kitchen" }, { "mic-hall", "Hall" }, { "mic-door", "Door" } };
        for (const auto& n : names)
        {
            auto spec = microphone (n[0], n[1], { fakewasapi::Format::pcm (1, 24, 48000.0) });
            spec.initializeError = static_cast<long> (E_ACCESSDENIED);
            fakewasapi::addEndpoint (spec);
        }

        int countedAsPower = 0;
        for (const auto& n : names)
        {
            mma::WasapiAsioBackend backend;
            Capture capture;
            (void) backend.openInputStream (n[0], 48000.0, 256, capture.callback());
            countedAsPower += mma::countsAsPossiblePowerLoss (backend.getLastOpenRefusal()) ? 1 : 0;
        }
        check (countedAsPower == 0, "three microphones refused at Initialize: none counts toward the power warning");

        auto spec = microphone ("mic-kitchen", "Kitchen", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.initializeError = static_cast<long> (E_ACCESSDENIED);
        refusedAs ("Initialize", spec);
    }

    {
        fakewasapi::reset();
        auto spec = microphone ("mic-asked", "Stage", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.formatQuestionError = static_cast<long> (E_ACCESSDENIED);
        fakewasapi::addEndpoint (spec);
        refusedAs ("the format questions", spec);
    }

    {
        fakewasapi::reset();
        auto spec = microphone ("mic-again", "Stage", { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.alignedFrames = 480;
        spec.failActivateAfterAlignment = true;
        spec.reactivateError = static_cast<long> (E_ACCESSDENIED);
        fakewasapi::addEndpoint (spec);
        refusedAs ("the second Activate", spec);
    }
}

/// A buffer below the device's minimum period. Windows rejects it with
/// AUDCLNT_E_INVALID_DEVICE_PERIOD; the backend must ask GetDevicePeriod and
/// open at the minimum instead, not blame another app for holding the device.
void aBufferBelowTheDeviceMinimumOpensAtTheMinimum()
{
    std::printf ("\nA buffer smaller than the device's minimum period\n");
    fakewasapi::reset();

    auto spec = microphone ("mic-min", "Slow Mic", { fakewasapi::Format::pcm (1, 32, 48000.0) });
    spec.minimumPeriodFrames = 128;
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-min", 48000.0, 64, capture.callback()),
           "a 64-sample request on a 128-sample device still opens");
    check (backend.getLastOpenError().empty(), "with no error left behind");
    check (fakewasapi::isRunning ("mic-min"), "and the stream starts");
    backend.closeAllStreams();
}

/// JUCE's message thread is a single-threaded apartment (OleInitialize), so
/// the backend's CoInitializeEx(MULTITHREADED) there fails with
/// RPC_E_CHANGED_MODE. Its open worker and stream threads then belong to no
/// apartment, and COM calls from them fail unless they join the MTA
/// themselves. On Windows that read as every microphone refusing to open.
void microphonesOpenWhenTheMessageThreadIsSingleThreaded()
{
    std::printf ("\nThe backend built on a single-threaded-apartment message thread\n");
    fakewasapi::reset();
    fakewasapi::addEndpoint (microphone ("mic-sta", "STA Mic", { fakewasapi::Format::pcm (1, 32, 48000.0) }));

    fakewasapi::enterSingleThreadedApartment();
    {
        mma::WasapiAsioBackend backend;
        Capture capture;

        check (backend.openInputStream ("mic-sta", 48000.0, 256, capture.callback()),
               "the microphone still opens");
        check (backend.getLastOpenError().empty(), "with no error");
        check (fakewasapi::isRunning ("mic-sta"), "and its stream runs");
        backend.closeAllStreams();
    }
    fakewasapi::leaveApartment();
}

/// §5.4 quotes the singer a monitoring latency. Quoting it from the period we
/// ASKED for is a lie whenever the device names its own: the app must report
/// the granted size. Nothing else in this file opens an output at a size the
/// device refuses, so without this the reporting path is untested on Windows.
void theGrantedOutputPeriodIsWhatTheBackendReports()
{
    std::printf ("\nAn output that rejects the period and names its own\n");
    fakewasapi::reset();

    auto spec = headphones ("out-align", "Picky Out",
                            { fakewasapi::Format::pcm (2, 24, 48000.0) });
    spec.alignedFrames = 480;
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;

    check (backend.getGrantedOutputBufferFrames() == 0,
           "with nothing open the backend says it cannot tell");

    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = 0.0f;
    };

    check (backend.openExclusiveOutputStream ("out-align", 48000.0, 256, writer),
           "the retry at the device's own size succeeds");
    check (backend.getGrantedOutputBufferFrames() == 480,
           "and the backend reports 480, not the 256 we asked for");

    backend.closeAllStreams();

    check (backend.getGrantedOutputBufferFrames() == 0,
           "once it is closed there is nothing to report again");
}

/// AUDCLNT_BUFFERFLAGS_SILENT means the buffer contents are undefined. Reading
/// it anyway turns a dropout into full-scale noise.
void aSilentFlaggedPacketIsTreatedAsSilence()
{
    std::printf ("\nA packet the device flags as silent\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-silent", "Silent Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-silent", 48000.0, 256, capture.callback()), "the stream opens");
    check (fakewasapi::pushSilentCapture ("mic-silent", 64), "the packet is consumed");

    const auto block = capture.block();
    bool silent = ! block.empty();

    for (const auto& channel : block)
        for (float v : channel)
            if (std::fabs (v) > 1.0e-6f)
                silent = false;

    check (silent, "the callback sees silence, not the undefined bytes");
    backend.closeAllStreams();
}

/// Counts what a CaptureCoordinator would be told: delivered blocks, and the
/// produced-and-lost signal (no inputs, that many frames) in the order they
/// arrived.
struct LossAwareCapture
{
    std::mutex mutex;
    std::vector<std::pair<bool, int>> events;   ///< { isLossSignal, frames }

    mma::AudioCallback callback()
    {
        return [this] (const float* const* inputs, int numInputs,
                       float* const*, int, int numSamples)
        {
            std::lock_guard<std::mutex> lock (mutex);
            events.emplace_back (inputs == nullptr && numInputs == 0, numSamples);
        };
    }

    std::vector<std::pair<bool, int>> log()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return events;
    }
};

/// A driver overrun on Windows is not an error return: the next packet simply
/// starts further along the device's clock, flagged DATA_DISCONTINUITY. The
/// worker read neither, so the missing stretch was spliced out of the file,
/// reported as zero dropped frames, and read by drift as the clock running slow.
void aCaptureOverrunIsCountedAndReportedAsLost()
{
    std::printf ("\nA capture overrun the driver flags as a discontinuity\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-overrun", "Overrun Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    LossAwareCapture capture;

    check (backend.openInputStream ("mic-overrun", 48000.0, 256, capture.callback()), "the stream opens");

    const auto signal = tone (128, 0.25f, 0.0f);
    check (fakewasapi::pushCapture ("mic-overrun", { signal }), "a first packet arrives");
    const auto droppedBefore = backend.getFramesDroppedByBackend();

    check (fakewasapi::pushCaptureAfterOverrun ("mic-overrun", 256, { signal }),
           "the packet after a 256-frame overrun arrives");

    check (backend.getFramesDroppedByBackend() - droppedBefore == 256,
           "the 256 frames the driver lost are counted as dropped");

    const auto log = capture.log();
    const bool signalled = log.size() == 3 && log[1].first && log[1].second == 256
                        && ! log[2].first && log[2].second == 128;
    check (signalled,
           "the callback is told 256 frames were produced and lost, before the packet itself");

    backend.closeAllStreams();
}

/// Windows commonly flags the first packet after Start as discontinuous: there
/// is nothing before it to be discontinuous with. That must not read as loss.
void aDiscontinuityOnTheFirstPacketIsNotLoss()
{
    std::printf ("\nA discontinuity flag on the first packet after Start\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-first", "First-packet Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    LossAwareCapture capture;

    check (backend.openInputStream ("mic-first", 48000.0, 256, capture.callback()), "the stream opens");
    check (fakewasapi::pushCaptureAfterOverrun ("mic-first", 0, { tone (128, 0.25f, 0.0f) }),
           "the flagged first packet arrives");

    check (backend.getFramesDroppedByBackend() == 0, "nothing is counted as dropped");

    const auto log = capture.log();
    check (log.size() == 1 && ! log[0].first && log[0].second == 128,
           "and the callback sees only the packet, with no loss signal");

    backend.closeAllStreams();
}

/// The monitor path. A packing error here is a silent or crossed monitor mix,
/// which no recording test would catch.
void theMonitorMixIsWrittenInTheNegotiatedFormat()
{
    std::printf ("\nA 24-bit stereo monitor output\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-24", "24-bit Out",
                                         { fakewasapi::Format::pcm (2, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;

    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = 0.3f * static_cast<float> (ch + 1);
    };

    check (backend.openExclusiveOutputStream ("out-24", 48000.0, 256, writer), "the output opens");
    check (fakewasapi::openedExclusive ("out-24"), "exclusively");

    std::vector<std::vector<float>> written;
    check (fakewasapi::pullRender ("out-24", written), "a render period completes");
    check (written.size() == 2, "both channels are written");

    bool correct = written.size() == 2;
    for (size_t ch = 0; ch < written.size() && correct; ++ch)
        for (float v : written[ch])
            if (std::fabs (v - 0.3f * static_cast<float> (ch + 1)) > 1.0e-4f)
            {
                correct = false;
                break;
            }

    check (correct, "each channel carries its own signal, correctly encoded and interleaved");
    backend.closeAllStreams();
}

/// §5: an over-range monitor sum must clip rather than wrap. A wrapped integer
/// is full-scale noise in the performer's headphones.
void anOverRangeMonitorSumClipsRatherThanWraps()
{
    std::printf ("\nA monitor sum that runs over full scale\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-16", "16-bit Out",
                                         { fakewasapi::Format::pcm (2, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;

    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = (i % 2 == 0) ? 3.5f : -3.5f;
    };

    check (backend.openExclusiveOutputStream ("out-16", 48000.0, 256, writer), "the output opens");

    std::vector<std::vector<float>> written;
    check (fakewasapi::pullRender ("out-16", written), "a render period completes");

    bool clipped = ! written.empty();
    for (const auto& channel : written)
        for (size_t i = 0; i < channel.size(); ++i)
        {
            const float expected = (i % 2 == 0) ? 1.0f : -1.0f;
            if (std::fabs (channel[i] - expected) > 0.01f)
                clipped = false;
        }

    check (clipped, "every sample lands at the rail, none wrapped to the opposite sign");
    backend.closeAllStreams();
}

/// §2: hotplug comes from the OS. The backend registers an IMMNotificationClient,
/// so a device appearing must reach it with nothing polling.
void hotplugArrivesThroughTheNotificationClient()
{
    std::printf ("\nA mic plugged in after launch\n");
    fakewasapi::reset();

    mma::WasapiAsioBackend backend;
    std::atomic<int> notifications { 0 };
    backend.setDeviceChangeCallback ([&notifications] { notifications.fetch_add (1); });

    fakewasapi::addEndpoint (microphone ("mic-late", "Late Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    check (notifications.load() >= 1, "the backend is told, with no timer involved");
    check (backend.enumerateInputDevices().size() == 1, "and the device is there when it re-enumerates");

    const auto before = notifications.load();
    fakewasapi::removeEndpoint ("mic-late");
    check (notifications.load() > before, "unplugging notifies too");

    // Registration is dropped here; the reference count must survive it.
    backend.setDeviceChangeCallback (nullptr);
    check (true, "unregistering the notification client does not crash");
}

/// Enumeration has to report what the endpoints actually say, and keep capture
/// and render apart.
void enumerationReportsNamesAndSeparatesDirections()
{
    std::printf ("\nEnumeration of a mixed set of endpoints\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-a", "Podcast Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));
    fakewasapi::addEndpoint (headphones ("out-a", "Studio Interface",
                                         { fakewasapi::Format::pcm (2, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;

    const auto inputs = backend.enumerateInputDevices();
    const auto outputs = backend.enumerateOutputDevices();

    check (inputs.size() == 1 && outputs.size() == 1, "each direction reports only its own endpoints");
    check (! inputs.empty() && inputs.front().name == "Podcast Mic", "the friendly name is read");
    check (! inputs.empty() && inputs.front().usbLocationId == "mic-a", "the endpoint id is kept as identity");
    check (! inputs.empty() && inputs.front().isMicrophone, "inputs are marked as microphones");
}

/// A capture endpoint being active does not make it an eligible SobStage mic.
/// The physical filter behind it must be directly attached external hardware;
/// laptop, phone, Bluetooth, and software inputs stay out of the recording.
void onlyDirectlyAttachedHardwareEnumeratesAsInput()
{
    std::printf ("\nOnly directly attached hardware appears as a microphone\n");
    fakewasapi::reset();

    const auto addInput = [] (const char* id, const char* name,
                              std::vector<fakewasapi::DeviceNodeSpec> chain)
    {
        auto spec = microphone (id, name, { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.physicalInstanceId = chain.empty() ? std::string() : chain.front().instanceId;
        spec.deviceNodeChain = std::move (chain);
        fakewasapi::addEndpoint (spec);
    };

    // The audio function is commonly not marked removable; Windows marks the
    // top-most physical parent instead. Both positive properties must be on the
    // same node so a fixed internal USB function cannot borrow one bit from an
    // unrelated ancestor.
    addInput ("usb", "USB interface",
              { { "USB\\VID_1234&PID_5678&MI_00", false, false, true },
                { "USB\\VID_1234&PID_5678", true, true, true } });
    addInput ("firewire", "FireWire interface",
              { { "1394\\VENDOR&MODEL", true, true, true } });
    addInput ("thunderbolt", "Thunderbolt interface",
              { { "HDAUDIO\\FUNC_01&VEN_EXT", false, false, true },
                { "PCI\\VEN_EXT&DEV_AUDIO", false, false, true },
                { "PCI\\VEN_TBT&DEV_BRIDGE", true, true, true } });

    addInput ("usb-built-in", "Internal webcam microphone",
              { { "USB\\VID_INTERNAL&PID_CAMERA", false, false, true } });
    addInput ("usb-phone", "Phone USB audio",
              { { "USB\\VID_PHONE&PID_AUDIO", false, false, true } });
    addInput ("generic-removable-uac", "Generic removable USB audio",
              { { "USB\\VID_GENERIC&PID_UAC", true, true, true } });
    addInput ("split-proof", "Malformed removable USB audio",
              { { "USB\\VID_SPLIT&PID_AUDIO", true, false, true },
                { "USB\\VID_SPLIT", false, true, true } });
    addInput ("built-in", "Laptop microphone",
              { { "HDAUDIO\\FUNC_01&VEN_1234", false, false, true },
                { "PCI\\VEN_INTERNAL&DEV_AUDIO", false, false, true } });
    addInput ("bluetooth", "Bluetooth headset",
              { { "BTHHFENUM\\BTHHFPAUDIO", true, true, true } });
    addInput ("software", "Virtual cable",
              { { "SWD\\MMDEVAPI\\VIRTUAL", true, true, true } });
    addInput ("phone", "Continuity phone microphone",
              { { "ROOT\\CONTINUITY_AUDIO", true, true, true } });
    addInput ("unknown", "Unknown input",
              { { "", true, true, true } });

    auto speakers = headphones ("speakers", "Laptop speakers",
                                { fakewasapi::Format::pcm (2, 24, 48000.0) });
    speakers.deviceNodeChain = {
        { "HDAUDIO\\FUNC_01&VEN_SPEAKER", false, false, true },
        { "PCI\\VEN_INTERNAL&DEV_SPEAKER", false, false, true }
    };
    fakewasapi::addEndpoint (speakers);

    mma::WasapiAsioBackend backend;
    const auto inputs = backend.enumerateInputDevices();
    const auto outputs = backend.enumerateOutputDevices();

    check (inputs.size() == 4,
           "fixed/malformed USB, built-in, known phone, Bluetooth, software, and unknown inputs are excluded");

    bool usb = false, firewire = false, thunderbolt = false;
    bool genericRemovableUac = false, unexpected = false;
    for (const auto& device : inputs)
    {
        std::printf ("    accepted input: %s\n", device.usbLocationId.c_str());
        usb |= device.usbLocationId == "usb";
        firewire |= device.usbLocationId == "firewire";
        thunderbolt |= device.usbLocationId == "thunderbolt";
        genericRemovableUac |= device.usbLocationId == "generic-removable-uac";
        unexpected |= device.usbLocationId != "usb"
                   && device.usbLocationId != "firewire"
                   && device.usbLocationId != "thunderbolt"
                   && device.usbLocationId != "generic-removable-uac";
    }

    check (usb && firewire && thunderbolt && ! unexpected,
           "removable USB, FireWire, and Thunderbolt inputs remain available");
    check (genericRemovableUac,
           "generic removable USB Audio has no form-factor signal (documented release limitation)");
    check (outputs.size() == 1 && outputs.front().usbLocationId == "speakers",
           "the recording-input policy does not hide built-in monitor outputs");
}

/// Every leg of Windows' identity proof can fail in the real world: an old
/// driver may expose no topology, a filter may omit its instance id, or the PnP
/// tree may have malformed properties. None may turn into a name-based fallback.
/// An unplugged or disabled interface is still in the Windows registry.
///
/// EnumAudioEndpoints takes a state mask, and the backend asks for
/// DEVICE_STATE_ACTIVE. The fake discarded that mask and reported ACTIVE for
/// every endpoint, so asking for active devices and asking for all of them
/// returned the same list -- and a backend that passed the wrong mask would
/// have offered the user a microphone that is unplugged, disabled, or gone.
/// Picking it produces a take of silence, which is the §0.1 failure.
void onlyActiveEndpointsAreOffered()
{
    std::printf ("\nInterfaces Windows still lists but will not open\n");
    fakewasapi::reset();

    const auto add = [] (const char* id, unsigned long state)
    {
        auto spec = microphone (id, id, { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.deviceNodeChain = { { std::string ("USB\\") + id, true, true, true } };
        spec.deviceState = state;
        fakewasapi::addEndpoint (spec);
    };

    // The numeric values of the DEVICE_STATE_* flags, spelled out because this
    // file does not include the Windows headers.
    add ("live", 0x1);        // ACTIVE
    add ("unplugged", 0x8);   // UNPLUGGED
    add ("disabled", 0x2);    // DISABLED
    add ("gone", 0x4);        // NOTPRESENT

    mma::WasapiAsioBackend backend;
    const auto inputs = backend.enumerateInputDevices();

    check (inputs.size() == 1, "only the active interface is offered");
    check (! inputs.empty() && inputs[0].name == "live",
           "and it is the one that is actually plugged in");
}

/// The §2.4 identity walk has to actually be walked.
///
/// Windows keeps three distinct strings here -- the endpoint id, the topology
/// device id the connector names, and the physical filter's PnP instance id --
/// and only the last one says what the hardware is. The fake used to return the
/// endpoint id for all three, so a backend that read PKEY_Device_InstanceId
/// straight off the endpoint, skipping GetConnector and GetDeviceIdConnectedTo
/// entirely, produced identical output and every check in this file still
/// passed.
///
/// It would not pass on a real machine. The endpoint's own instance id is
/// SWD\MMDEVAPI\..., which is not an eligible transport, so that shortcut
/// classifies EVERY microphone as not-external and hides the whole rig.
///
/// This pins the three apart by name so the shortcut cannot come back quietly.
void thePhysicalIdentityComesFromTheConnectedNodeNotTheEndpoint()
{
    std::printf ("\nThe identity walk reaches the physical node, not the endpoint\n");
    fakewasapi::reset();

    auto spec = microphone ("endpoint-id", "USB interface",
                            { fakewasapi::Format::pcm (1, 24, 48000.0) });
    spec.deviceNodeChain = { { "USB\\VID_AAAA&PID_BBBB", true, true, true } };
    spec.physicalInstanceId = "USB\\VID_AAAA&PID_BBBB";

    // Named explicitly rather than derived, so the test states the shape it
    // depends on instead of trusting a default to stay different.
    spec.connectedDeviceId = "{2}.\\\\?\\USB#VID_AAAA&PID_BBBB#TOPOLOGY";
    spec.endpointInstanceId = "SWD\\MMDEVAPI\\endpoint-id";

    fakewasapi::addEndpoint (spec);

    check (spec.connectedDeviceId != spec.id,
           "the connector names something other than the endpoint id");
    check (spec.endpointInstanceId != spec.physicalInstanceId,
           "and the endpoint's own instance id is not the physical one");

    mma::WasapiAsioBackend backend;
    const auto inputs = backend.enumerateInputDevices();

    // The only route from the endpoint to USB\VID_AAAA... is the topology
    // walk. Reading the instance id off the endpoint yields SWD\MMDEVAPI\...,
    // which fails the transport test, and this input disappears.
    check (inputs.size() == 1,
           "the interface is offered, which only the full walk can establish");
}

void missingExternalEvidenceFailsClosed()
{
    std::printf ("\nMissing Windows external-device evidence fails closed\n");
    fakewasapi::reset();

    const auto addFailure = [] (const char* id, auto mutate)
    {
        auto spec = microphone (id, id, { fakewasapi::Format::pcm (1, 24, 48000.0) });
        spec.deviceNodeChain = { { std::string ("USB\\") + id, true, true, true } };
        mutate (spec);
        fakewasapi::addEndpoint (spec);
    };

    addFailure ("no-topology", [] (auto& s) { s.topologyAvailable = false; });
    addFailure ("no-connector", [] (auto& s) { s.connectorAvailable = false; });
    addFailure ("no-connected-id", [] (auto& s) { s.connectedDeviceIdAvailable = false; });
    addFailure ("no-store", [] (auto& s) { s.propertyStoreAvailable = false; });
    addFailure ("no-instance-id", [] (auto& s) { s.instanceIdPropertyAvailable = false; });
    addFailure ("no-devnode", [] (auto& s) { s.devNodeLookupAvailable = false; });
    addFailure ("bad-properties", [] (auto& s) { s.deviceNodeChain.front().propertiesReadable = false; });

    mma::WasapiAsioBackend backend;
    check (backend.enumerateInputDevices().empty(),
           "topology, property, devnode, and removal-proof failures expose no inputs");
}

void openingAnInputRechecksTheExternalHardwarePolicy()
{
    std::printf ("\nOpening a disallowed WASAPI input directly\n");
    fakewasapi::reset();

    auto phone = microphone ("phone-direct-open", "Continuity phone",
                             { fakewasapi::Format::pcm (1, 24, 48000.0) });
    phone.physicalInstanceId = "ROOT\\CONTINUITY_AUDIO";
    phone.deviceNodeChain = { { phone.physicalInstanceId, true, true, true } };
    fakewasapi::addEndpoint (phone);

    mma::WasapiAsioBackend backend;
    Capture capture;
    check (! backend.openInputStream (phone.id, 48000.0, 256, capture.callback()),
           "a caller cannot bypass discovery and open a phone input by endpoint id");
    check (! fakewasapi::isRunning (phone.id),
           "the rejected endpoint never starts a WASAPI stream");
    check (backend.getLastOpenError().find ("directly connected external") != std::string::npos,
           "the refusal explains the external-hardware policy");
}

/// Eight microphones, the §1 ceiling, each on its own worker thread and each in
/// a different wire format. This is where a shared-state or channel-indexing
/// error surfaces that two devices would hide.
void eightMicrophonesInMixedFormatsStaySeparate()
{
    std::printf ("\nEight mics at once, in four different wire formats\n");
    fakewasapi::reset();

    const std::vector<fakewasapi::Format> formats {
        fakewasapi::Format::pcm (1, 16, 48000.0),
        fakewasapi::Format::pcm (1, 24, 48000.0),
        fakewasapi::Format::pcm (1, 32, 48000.0),
        fakewasapi::Format::floatFormat (1, 48000.0)
    };

    mma::WasapiAsioBackend backend;
    std::vector<std::unique_ptr<Capture>> captures;
    std::vector<std::string> ids;

    for (int i = 0; i < 8; ++i)
    {
        const auto id = "mic-" + std::to_string (i);
        ids.push_back (id);
        fakewasapi::addEndpoint (microphone (id, "Mic " + std::to_string (i),
                                             { formats[static_cast<size_t> (i % 4)] }));
        captures.push_back (std::make_unique<Capture>());

        if (! backend.openInputStream (id, 48000.0, 256, captures.back()->callback()))
        {
            check (false, "mic " + std::to_string (i) + " opens");
            return;
        }
    }

    check (true, "all eight open, each negotiating its own format");

    for (int i = 0; i < 8; ++i)
    {
        const float value = 0.1f * static_cast<float> (i + 1);
        fakewasapi::pushCapture (ids[static_cast<size_t> (i)], { std::vector<float> (64, value) });
    }

    bool separated = true;
    for (int i = 0; i < 8 && separated; ++i)
    {
        const auto block = captures[static_cast<size_t> (i)]->block();
        const float expected = 0.1f * static_cast<float> (i + 1);

        separated = block.size() == 1 && ! block[0].empty()
                 && std::fabs (block[0][0] - expected) < 2.0f / 32768.0f;
    }

    check (separated, "every mic's audio arrives on its own stream, none crossed");
    backend.closeAllStreams();
}

/// Closing must stop the worker threads and release the endpoints. A backend
/// that leaked a running thread would hang here rather than pass.
void closingStopsEveryStream()
{
    std::printf ("\nClosing the streams\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-close", "Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));
    fakewasapi::addEndpoint (headphones ("out-close", "Out",
                                         { fakewasapi::Format::pcm (2, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    backend.openInputStream ("mic-close", 48000.0, 256, capture.callback());
    backend.openExclusiveOutputStream ("out-close", 48000.0, 256,
                                       [] (const float* const*, int, float* const*, int, int) {});

    check (fakewasapi::isRunning ("mic-close") && fakewasapi::isRunning ("out-close"),
           "both streams are running");

    backend.closeAllStreams();

    check (! fakewasapi::isRunning ("mic-close") && ! fakewasapi::isRunning ("out-close"),
           "and both are stopped, with their worker threads joined");
}

} // namespace

/// §0.1: a microphone that stops waking its event has stopped sending audio.
/// The worker used to spin on that forever -- no audio, no error, no end -- so
/// a mic that died mid-take had its track written as silence for the rest of
/// the take and nothing anywhere said so.
///
/// Six seconds of real waiting, because that is what the backend does before it
/// is willing to call a device dead, and a test that shortened it would not be
/// testing the shipped decision.
void aStalledMicrophoneIsReportedRatherThanSpunOnForever()
{
    std::printf ("\nA microphone that stops waking its event\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-stall", "Dying Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-stall", 48000.0, 256, capture.callback()),
           "the stream opens");
    check (backend.takeStreamFailures().empty(), "and a healthy stream reports no failure");

    // Nothing is ever pushed, so the event never signals -- exactly what a
    // microphone that has stopped delivering looks like from in here.
    std::this_thread::sleep_for (std::chrono::milliseconds (6800));

    const auto failed = backend.takeStreamFailures();

    check (failed.size() == 1, "the stall is reported once");
    check (failed.size() == 1 && failed[0].deviceId == "mic-stall",
           "against the device that stopped, by id");
    check (failed.size() == 1 && failed[0].reason.find ("stopped sending audio") != std::string::npos,
           "in words the user can act on");
    check (! fakewasapi::isRunning ("mic-stall"),
           "and the dead stream lets go of the device rather than holding it open");

    // Taken, not read: the same failure must not be reported twice.
    check (backend.takeStreamFailures().empty(), "and it is not reported again");

    backend.closeAllStreams();
}

/// §0.1: a microphone whose device is pulled mid-take. Windows answers every
/// call on the stream with AUDCLNT_E_DEVICE_INVALIDATED from then on -- and on
/// a driver that goes on signalling the ready event, that lands in the one gap
/// the worker had no exit from.
///
/// The stall timeout cannot see it: the event keeps firing, so the timeout
/// counter resets on every pass. The GetBuffer failure count cannot see it
/// either: GetNextPacketSize is what fails, so the loop that counts GetBuffer
/// failures is never entered. Both of the worker's other two deaths -- the
/// stalled wait and the dead render -- carry a comment saying this exact shape
/// of bug was fixed there. This is the same bug through the third door.
void anInvalidatedMicrophoneIsReportedRatherThanSpunOnForever()
{
    std::printf ("\nA microphone whose device is invalidated while its event keeps firing\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-gone", "Pulled Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-gone", 48000.0, 256, capture.callback()),
           "the stream opens");

    fakewasapi::invalidateEndpoint ("mic-gone");

    // Keeping the event alive is the whole point: a device that stops signalling
    // is already handled, and testing that again would prove nothing. Six and a
    // half seconds is past the stall timeout, so anything still spinning here is
    // spinning for good.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (6500);
    while (std::chrono::steady_clock::now() < deadline && fakewasapi::isRunning ("mic-gone"))
    {
        fakewasapi::pulseReadyEvent ("mic-gone");
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    const auto failed = backend.takeStreamFailures();

    check (failed.size() == 1, "the loss is reported once");
    check (failed.size() == 1 && failed[0].deviceId == "mic-gone",
           "against the device that went away, by id");
    check (failed.size() == 1 && failed[0].reason.find ("stopped sending audio") != std::string::npos,
           "in words the user can act on");
    check (! fakewasapi::isRunning ("mic-gone"),
           "and the worker stops rather than spinning on a device that is gone");

    backend.closeAllStreams();
}

/// §5.4's capability probe, which had no test at all -- and that is how it came
/// to ask a narrower question than the open it is supposed to predict.
///
/// It asked IsFormatSupported for float32 only, while buildExclusiveStream goes
/// on to try 32/32, 32/24, 24/24 and 16/16. The comment above findExclusiveFormat
/// says "use the same layouts for capability discovery and stream opening"; this
/// was the one caller that did not.
///
/// The devices that pays for are ordinary ones: plenty of USB interfaces and
/// DACs accept exclusive output only in their native INTEGER format and refuse
/// float32. Each was declared incapable, and the user told to turn on exclusive
/// mode in Windows sound settings -- a setting that was already on, for hardware
/// the app could have opened on the very next layout it never asked about.
void anIntegerOnlyOutputIsNotCalledIncapable()
{
    std::printf ("\nHeadphones that take 24-bit exclusive but not float\n");
    fakewasapi::reset();

    // No float32 offered: exactly what a great many interfaces expose.
    fakewasapi::addEndpoint (headphones ("out-int24", "Integer-only Out",
                                         { fakewasapi::Format::pcm (2, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    const auto cap = backend.checkExclusiveModeCapability ("out-int24", 48000.0, 256);

    check (cap.exclusiveModeAvailable,
           "a device that refuses float but takes 24-bit integer is still capable");
    check (cap.unavailableReason.empty(),
           "and is not handed advice about a Windows setting that is already right");

    // The claim has to be true, not merely optimistic: the open must succeed
    // on the same device, or the probe is lying in the other direction.
    Capture capture;
    check (backend.openExclusiveOutputStream ("out-int24", 48000.0, 256, capture.callback()),
           "and the stream the probe promised actually opens");
    check (fakewasapi::openedExclusive ("out-int24"),
           "in exclusive mode, as §5.4 requires");

    backend.closeAllStreams();
}

/// The probe asked about stereo alone, while the open goes on to try mono and
/// the device's own channel count. Hardware that is mono-only, or a
/// four-channel interface that takes nothing narrower, was declared incapable
/// -- and blamed on a Windows setting -- although it opened on the next try.
void aNonStereoOutputIsNotCalledIncapable()
{
    std::printf ("\nHeadphone outputs that take only mono, or only four channels\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-quad", "Four-channel Out",
                                         { fakewasapi::Format::pcm (4, 24, 48000.0) }));
    fakewasapi::addEndpoint (headphones ("out-mono", "Mono Out",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    for (const char* id : { "out-quad", "out-mono" })
    {
        mma::WasapiAsioBackend backend;
        const auto cap = backend.checkExclusiveModeCapability (id, 48000.0, 256);

        check (cap.exclusiveModeAvailable,
               std::string (id) + ": the probe tries the layouts the open will try");

        Capture capture;
        check (backend.openExclusiveOutputStream (id, 48000.0, 256, capture.callback()),
               std::string (id) + ": and the stream it promised opens");
        check (fakewasapi::openedExclusive (id), std::string (id) + ": exclusively");

        backend.closeAllStreams();
    }
}

/// 16-bit-only hardware is the same argument one layout further down.
void aSixteenBitOnlyOutputIsNotCalledIncapable()
{
    std::printf ("\nHeadphones that take only 16-bit exclusive\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-int16", "16-bit Out",
                                         { fakewasapi::Format::pcm (2, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    const auto cap = backend.checkExclusiveModeCapability ("out-int16", 48000.0, 256);

    check (cap.exclusiveModeAvailable, "16-bit-only output is capable too");

    Capture capture;
    check (backend.openExclusiveOutputStream ("out-int16", 48000.0, 256, capture.callback()),
           "and it opens");

    backend.closeAllStreams();
}

/// The other direction matters just as much: §5.4 never falls back to shared
/// mode, so a device that genuinely refuses every exclusive layout must be
/// reported as unavailable, with the cause named rather than a silent failure.
/// §5.4: the latency the probe reports is the round trip, in and out.
///
/// Every backend computes this figure, and until recently nothing read it --
/// CaptureCoordinator dropped it, so Application::measuredLatencyMs was never
/// assigned and the Advanced panel reported "0.0 ms". Now that it reaches the
/// user, what it says has to be right, and the three platforms have to agree:
/// ALSA counted a single buffer and so reported half of what this and macOS
/// give for identical settings.
void theReportedLatencyIsTheRoundTrip()
{
    std::printf ("\nWhat the probe says the monitor path costs\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-lat", "Latency Out",
                                         { fakewasapi::Format::floatFormat (2, 48000.0) }));

    mma::WasapiAsioBackend backend;

    // One buffer at 48 kHz: 256 frames is 5.333 ms, so the round trip is 10.667.
    const auto cap = backend.checkExclusiveModeCapability ("out-lat", 48000.0, 256);

    check (cap.exclusiveModeAvailable, "the output is capable");
    check (cap.measuredOrEstimatedLatencyMs > 0.0,
           "and the probe reports a latency rather than leaving it at zero");

    const double oneBuffer = (256.0 / 48000.0) * 1000.0;
    check (std::abs (cap.measuredOrEstimatedLatencyMs - oneBuffer * 2.0) < 1e-9,
           "which is the round trip -- one buffer in, one buffer out");

    // It has to follow the buffer size, or it is a constant dressed as a
    // measurement.
    const auto small = backend.checkExclusiveModeCapability ("out-lat", 48000.0, 128);
    const auto large = backend.checkExclusiveModeCapability ("out-lat", 48000.0, 512);

    check (std::abs (small.measuredOrEstimatedLatencyMs
                         - cap.measuredOrEstimatedLatencyMs / 2.0) < 1e-9,
           "halving the buffer halves it");
    check (std::abs (large.measuredOrEstimatedLatencyMs
                         - cap.measuredOrEstimatedLatencyMs * 2.0) < 1e-9,
           "and doubling the buffer doubles it");
}

void anOutputThatRefusesEveryLayoutIsStillReported()
{
    std::printf ("\nHeadphones that refuse exclusive mode altogether\n");
    fakewasapi::reset();

    // No exclusive formats at all.
    fakewasapi::addEndpoint (headphones ("out-shared", "Shared-only Out", {}));

    mma::WasapiAsioBackend backend;
    const auto cap = backend.checkExclusiveModeCapability ("out-shared", 48000.0, 256);

    check (! cap.exclusiveModeAvailable,
           "a device that refuses every layout is not called capable");
    check (cap.unavailableReason.find ("exclusive mode") != std::string::npos,
           "and the reason names what to change");
}

// The rig-watch registration is allowed to fail on Windows -- a locked-down
// session, a dying audio service -- and when it does, a microphone plugged in
// is never noticed and one pulled out MID-TAKE is never reported. That silence
// is worse than the failure: a take that lost a channel looks like a clean one.
void aSessionThatCannotWatchTheRigSaysSo()
{
    std::printf ("\nA session Windows will not let the app watch the rig from\n");
    fakewasapi::reset();

    {
        mma::WasapiAsioBackend backend;
        backend.setDeviceChangeCallback ([] {});

        check (backend.getHotplugProblem().empty(),
               "a session that CAN watch the rig reports no problem");
    }

    fakewasapi::setNotificationRegistrationAllowed (false);

    {
        mma::WasapiAsioBackend backend;
        backend.setDeviceChangeCallback ([] {});

        check (! backend.getHotplugProblem().empty(),
               "a session that cannot watch the rig says so rather than going quiet");
    }

    fakewasapi::setNotificationRegistrationAllowed (true);
}

void enumerationPreservesEveryInputAndSupportedRate()
{
    fakewasapi::reset();
    auto spec = microphone ("four", "Four inputs",
        { fakewasapi::Format::pcm (4, 24, 44100), fakewasapi::Format::pcm (4, 24, 96000),
          fakewasapi::Format::pcm (1, 24, 44100) });
    fakewasapi::addEndpoint (spec);
    auto blocked = microphone ("blocked", "Unavailable", {});
    blocked.allowActivate = false;
    fakewasapi::addEndpoint (blocked);
    mma::WasapiAsioBackend backend;
    const auto devices = backend.enumerateInputDevices();
    check (devices.size() == 2, "an unavailable device remains visible");
    for (const auto& d : devices)
    {
        if (d.usbLocationId == "four")
        {
            check (d.maxInputChannels == 4, "enumeration preserves all four sockets");
            check (d.currentSampleRate == 44100, "the current engine rate is reported");
            check (d.supportedSampleRates == std::vector<uint32_t> ({ 44100, 96000 }),
                   "only rates accepted in exclusive mode are advertised");
        }
        else
            check (d.supportedSampleRates.empty(), "failed activation invents no rates");
    }
    auto converted = microphone ("converted", "Shared mixer resamples",
                                  { fakewasapi::Format::pcm (1, 24, 44100) });
    converted.mixFormat = fakewasapi::Format::floatFormat (1, 48000);
    fakewasapi::addEndpoint (converted);
    for (const auto& d : backend.enumerateInputDevices())
        if (d.usbLocationId == "converted")
            check (d.currentSampleRate == 0 && d.supportedSampleRates == std::vector<uint32_t> ({ 44100 }),
                   "an unsupported shared-engine rate cannot win automatic negotiation");
    Capture capture;
    check (backend.openInputStream ("four", 44100, 256, capture.callback()), "four inputs open");
    check (fakewasapi::negotiatedFormat ("four").channels == 4,
           "a driver accepting mono still opens all four sockets");
    check (fakewasapi::pushCapture ("four", { { .1f, .1f }, { .2f, .2f }, { .3f, .3f }, { .4f, .4f } }),
           "four distinct inputs reach the callback");
    const auto audio = capture.block();
    check (audio.size() == 4 && audio[3].size() == 2 && std::fabs (audio[3][0] - .4f) < 1e-5f,
           "the last socket retains its signal");
    backend.closeAllStreams();
}

/// Some class-compliant USB drivers block the caller inside the open itself --
/// on Windows that is IAudioClient::Initialize rather than macOS's
/// AudioDeviceStart, but it is the same hazard, and SobStage creates its window
/// only after audio initialisation. An unbounded open therefore makes the whole
/// application appear never to launch.
///
/// This is the Windows half of sim_coreaudio's stuck-start family. Before it
/// existed, a driver stalling three seconds blocked the caller for three
/// seconds and then reported success.
void aStuckOpenIsBoundedRatherThanHoldingLaunch()
{
    std::printf ("\nA USB microphone whose driver hangs inside Initialize\n");
    fakewasapi::reset();

    auto spec = microphone ("stuck-open", "Wedged USB Mic",
                            { fakewasapi::Format::floatFormat (1, 48000.0) });
    spec.initializeDelayMilliseconds = kStuckDriverMilliseconds;
    fakewasapi::addEndpoint (spec);

    fakewasapi::addEndpoint (microphone ("healthy", "Working Mic",
                                         { fakewasapi::Format::floatFormat (1, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture, healthy;

    check (backend.openInputStream ("healthy", 48000.0, 256, healthy.callback()),
           "a working microphone is already live before the failure");

    const auto began = std::chrono::steady_clock::now();
    const bool opened = backend.openInputStream ("stuck-open", 48000.0, 256, capture.callback());
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - began);

    std::printf ("  [measured] the open returned in %lld ms (the driver hangs for %d ms)\n",
                 (long long) took.count(), kStuckDriverMilliseconds);

    check (! opened, "the backend stops waiting for the stalled driver");
    check (took < kBoundedOpen,
           "and launch is bounded well below the driver's stall");
    check (backend.getLastOpenError().find ("took too long") != std::string::npos,
           "the failure says Windows timed out rather than blaming a cable silently");

    // The point of bounding it: the rig that already worked still works.
    check (fakewasapi::pushCapture ("healthy", { { 0.25f } }),
           "the healthy microphone is unaffected by its neighbour hanging");

    backend.closeAllStreams();

    // The worker is still inside the driver. Waiting for it here is not
    // politeness: the next test calls fakewasapi::reset() and destroys the
    // endpoint it is reading, which ThreadSanitizer reports as the race it is.
    check (backend.waitForPendingOpensForTesting (kStuckDriverMilliseconds + 4000),
           "the abandoned open worker settles on its own once the driver lets go");
}

void bitDepthFollowsWhatTheEndpointAccepts()
{
    // §2.3: "do not upconvert -- it adds file size and no information." The
    // depth a stem is written at follows the hardware, and WASAPI can be asked
    // the question directly -- the same IsFormatSupported the rate probe next
    // to it already uses, narrowed to widths.
    std::printf ("\nBit depth follows the endpoint (§2.3)\n");
    fakewasapi::reset();

    // §14.1's own hardware: 16-bit, and its stem was being written at 24.
    fakewasapi::addEndpoint (microphone ("mic-yeti", "Blue Yeti",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    // The ordinary USB microphone, which is 24-bit and must stay 24-bit.
    fakewasapi::addEndpoint (microphone ("mic-24", "24-bit Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    // A device that accepts nothing exclusively -- held by another process --
    // must report nothing rather than look like a limited one.
    fakewasapi::addEndpoint (microphone ("mic-busy", "Busy Mic", {}));

    mma::WasapiAsioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    const auto depthsOf = [&devices] (const std::string& name) {
        for (const auto& d : devices)
            if (d.name == name)
                return d.supportedBitDepths;
        return std::vector<int>{};
    };

    check (depthsOf ("Blue Yeti") == std::vector<int> { 16 },
           "a 16-bit endpoint reports 16");
    check (mma::SampleFormat::chooseRecordingBitDepth (depthsOf ("Blue Yeti"), 24) == 16,
           "so its stem is written at 16 rather than padded to 24 for nothing");

    check (mma::SampleFormat::chooseRecordingBitDepth (depthsOf ("24-bit Mic"), 24) == 24,
           "a 24-bit endpoint still gets 24");

    check (depthsOf ("Busy Mic").empty(),
           "an endpoint that accepts nothing reports nothing rather than guessing");
    check (mma::SampleFormat::chooseRecordingBitDepth (depthsOf ("Busy Mic"), 24) == 24,
           "and that falls back to the take's depth, unchanged");
}

int main()
{
    std::printf ("WASAPI backend, driven against a virtual endpoint layer\n");
    std::printf ("=======================================================\n");

    enumerationPreservesEveryInputAndSupportedRate();
    onlyDirectlyAttachedHardwareEnumeratesAsInput();
    onlyActiveEndpointsAreOffered();
    thePhysicalIdentityComesFromTheConnectedNodeNotTheEndpoint();
    missingExternalEvidenceFailsClosed();
    openingAnInputRechecksTheExternalHardwarePolicy();
    a24BitOnlyMicrophoneOpensAndDeliversAudio();
    bitDepthFollowsWhatTheEndpointAccepts();
    a16BitMicrophoneRoundTripsWithinItsQuantisation();
    aFloatCapableDeviceStillGetsFloat();
    aStereoOnlyMicrophoneIsOpenedAsStereo();
    aDeviceThatAcceptsNothingFailsWithAReason();
    aBufferAlignmentRejectionIsRetried();
    aFailedReactivationSaysWhy();
    aMicrophoneThatDropsOffWhileOpeningCountsTowardThePowerWarning();
    aMicrophoneThePrivacySettingsRefuseIsNeverCountedAsPower();
    aBufferBelowTheDeviceMinimumOpensAtTheMinimum();
    microphonesOpenWhenTheMessageThreadIsSingleThreaded();
    aSilentFlaggedPacketIsTreatedAsSilence();
    aCaptureOverrunIsCountedAndReportedAsLost();
    aDiscontinuityOnTheFirstPacketIsNotLoss();
    theMonitorMixIsWrittenInTheNegotiatedFormat();
    anOverRangeMonitorSumClipsRatherThanWraps();
    hotplugArrivesThroughTheNotificationClient();
    enumerationReportsNamesAndSeparatesDirections();
    eightMicrophonesInMixedFormatsStaySeparate();
    closingStopsEveryStream();
    theGrantedOutputPeriodIsWhatTheBackendReports();
    anIntegerOnlyOutputIsNotCalledIncapable();
    aSixteenBitOnlyOutputIsNotCalledIncapable();
    aNonStereoOutputIsNotCalledIncapable();
    anOutputThatRefusesEveryLayoutIsStillReported();
    theReportedLatencyIsTheRoundTrip();
    aStalledMicrophoneIsReportedRatherThanSpunOnForever();
    anInvalidatedMicrophoneIsReportedRatherThanSpunOnForever();
    aSessionThatCannotWatchTheRigSaysSo();
    aStuckOpenIsBoundedRatherThanHoldingLaunch();

    // Tear the last scenario down so a leak check sees only what the
    // backend failed to release, not what the harness never cleaned up.
    fakewasapi::reset();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
