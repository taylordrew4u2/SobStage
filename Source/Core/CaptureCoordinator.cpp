#include "CaptureCoordinator.h"
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined (__linux__) || defined (__APPLE__)
 #include <pthread.h>
 #include <sched.h>
#endif

namespace mma {

CaptureCoordinator::CaptureCoordinator (IAudioBackend& b, double rate, int bufferSizeSamples)
    : backend (b), sampleRate (rate), bufferSize (bufferSizeSamples),
      monitorBus (rate), feedbackGuard (rate, monitorBus), mixMeter (rate)
{
    for (auto& gain : outputChannelGains)
        gain.store (1.0f, std::memory_order_relaxed);
}

void CaptureCoordinator::setOutputChannelGains (const std::vector<float>& gains) noexcept
{
    for (size_t ch = 0; ch < outputChannelGains.size(); ++ch)
        outputChannelGains[ch].store (ch < gains.size() ? gains[ch] : 1.0f, std::memory_order_relaxed);
}

void CaptureCoordinator::layJackGainsFor (const std::string& outputDeviceId) noexcept
{
    if (! outputDeviceId.empty() && outputDeviceId == jackGainsDeviceId)
        setOutputChannelGains (jackGains);
    else
        setOutputChannelGains ({});
}

void CaptureCoordinator::setHeadphoneJackGains (const std::string& outputDeviceId, std::vector<float> gains)
{
    jackGainsDeviceId = outputDeviceId;
    jackGains = std::move (gains);

    // Onto the stream open now, whatever output is chosen: mid-take a
    // sub-device leaving the combined device moves every later jack, and
    // these are the gains for where they are now.
    layJackGainsFor (openOutputDeviceId);
}

CaptureCoordinator::~CaptureCoordinator()
{
    stopRecording();
    stopMonitoring();
}

bool CaptureCoordinator::startMonitoring (const std::vector<CaptureChannel>& chans,
                                          const std::string& outputDeviceId)
{
    stopMonitoring();

    channels = chans;
    monitorProblem.clear();
    devicesThatFailedToOpen.clear();

    // A new output stream is the clock again until told otherwise. Cleared
    // before anything opens, so its first callback pulls.
    outputAbandoned.store (false, std::memory_order_release);
    outputCarriesChannels = false;
    openOutputDeviceId.clear();

    // The jack gains of the device about to open, before it opens, so its
    // first callback already keeps the mix off a switched-off jack; any other
    // device gets the mix on every channel. An output that refuses comes back
    // through here with none (continueInputOnly).
    layJackGainsFor (outputDeviceId);

    channelMeters.clear();
    for (size_t i = 0; i < channels.size(); ++i)
        channelMeters.push_back (std::make_unique<Metering> (sampleRate));

    // Alignment state, sized here while no stream is open: the consumer reads
    // the latencies only once latenciesReady says they are filled in.
    latenciesReady.store (false, std::memory_order_relaxed);
    channelInputLatency.assign (chans.size(), 0);
    channelReportedLatency.assign (chans.size(), -1);
    latencyClamped = false;
    recordingOffsets.assign (chans.size(), 0);
    recordingOffsetKinds.assign (chans.size(), StemOffsetKind::settled);
    heldBlockSeen.assign (chans.size(), 0);
    stemReferenceFloor = 0;
    recordingOffsetView = std::make_unique<std::atomic<int>[]> (std::max<size_t> (1, chans.size()));
    for (size_t i = 0; i < std::max<size_t> (1, chans.size()); ++i)
        recordingOffsetView[i].store (0, std::memory_order_relaxed);

    // Sized here so the audio callback never allocates (§11).
    //
    // Floored at the largest callback CoreAudio sizes its own scratch for: an
    // output whose IO size is not the one asked for still mixes in one pass.
    mixScratch.assign (std::max (static_cast<size_t> (std::max (1, bufferSize)) * 8,
                                 static_cast<size_t> (DeviceInputStream::kLargestDeviceBlock)),
                       0.0f);
    busScratch.assign (mixScratch.size(), 0.0f);
    alarmScratch.assign (mixScratch.size(), 0.0f);
    trimFrame.assign (std::max<size_t> (1, channels.size()), 0.0f);

    // §3.2: one capture path per device, each with its own ring and PI loop.
    deviceStreams.clear();
    channelLayouts.clear();
    for (size_t i = 0; i < channels.size(); ++i)
    {
        auto stream = std::make_unique<DeviceInputStream> (sampleRate);
        stream->prepare (sampleRate, bufferSize);
        deviceStreams.push_back (std::move (stream));

        // §2.1 starts afresh for each opened device: the analyzer's 60-second
        // timeout is measured from the moment the device is seen.
        auto layout = std::make_unique<ChannelLayout>();
        layout->analyzer = ChannelLayoutAnalyzer (sampleRate);
        if (channels[i].collapseStereoPair)
            layout->source.store (channels[i].monoSourceChannel == 1 ? 1 : 0,
                                  std::memory_order_relaxed);
        channelLayouts.push_back (std::move (layout));
    }

    // §3.1: until the caller says otherwise, the first included mic is the
    // reference §3.3's drift figures are quoted against.
    setMasterChannel (channels.empty() ? -1 : 0);

    // Sized with the same headroom CoreAudioBackend gives its own scratch, and
    // for the same reason: the HAL is allowed to hand the callback a larger
    // slice than the nominal buffer. Sized to the nominal buffer alone, one
    // oversized slice made this whole function return without pulling a single
    // frame -- no audio, no meters, no error, which is the silent failure §0.1
    // exists to forbid. The guard below still stands for anything beyond even
    // this, but it is now a bound rather than an ordinary occurrence.
    constexpr int kCallbackSizeHeadroom = 2;

    //
    // And floored at the largest callback CoreAudio itself allows for, for the
    // drift loop's sake as much as the mix's. A pull is what DeviceInputStream
    // sizes its target fill from, so an output running at 1024 frames against
    // a nominal 64 has to be pulled 1024 at a time: sliced into 128-frame
    // pulls, each ring was held at two slices -- far less than the callback
    // takes at once -- and the loop sat pinned at its clamp to keep up, with
    // no correction left for a microphone's real drift.
    deviceScratch.assign (std::max<size_t> (1, channels.size())
                              * std::max (static_cast<size_t> (std::max (1, bufferSize))
                                              * kCallbackSizeHeadroom,
                                          static_cast<size_t> (DeviceInputStream::kLargestDeviceBlock)),
                          0.0f);
    devicePointers.assign (std::max<size_t> (1, channels.size()), nullptr);

    // Precomputed so the callback never calls a dB->linear conversion per sample.
    trimGains.clear();
    for (const auto& ch : channels)
        trimGains.push_back (MonitorBus::trimDbToLinearGain (ch.trimDb));

    // §2.1 meets §5.1. Most USB microphones present two inputs carrying the
    // same voice, and a fresh one keeps both as separate channels until the
    // analyzer has heard three seconds of it -- for the whole take, if Record
    // comes first, because the layout is only rebuilt while idle. Summed at
    // unity, that one voice reached the headphones twice: +6 dB, enough on
    // its own to push ordinary speech into the safety limiter. So in the
    // headphones (never in the stems) each side of a pair still being
    // analysed is heard at half, which is exactly the one voice for a
    // duplicated microphone and only 6 dB low for a real two-input interface
    // for those few seconds. Once the verdict is in, the callback follows it
    // straight away, mid-take included: Mono plays the chosen side alone,
    // Stereo both at unity. Found here, while nothing is running, so the
    // callback only reads these.
    monitorPairAnalyzer.assign (channels.size(), -1);
    monitorPairSide.assign (channels.size(), 0);
    for (size_t i = 0; i < channels.size(); ++i)
    {
        if (! channels[i].analyzeStereoPair || channels[i].deviceChannel != 0)
            continue;

        for (size_t j = 0; j < channels.size(); ++j)
        {
            if (j == i || channels[j].deviceId != channels[i].deviceId || channels[j].deviceChannel != 1)
                continue;

            monitorPairAnalyzer[i] = static_cast<int> (i);
            monitorPairAnalyzer[j] = static_cast<int> (i);
            monitorPairSide[j] = 1;
            break;
        }
    }

    monitorPairWeight.assign (channels.size(), 1.0f);
    monitorPairTarget.assign (channels.size(), 1.0f);
    for (size_t i = 0; i < channels.size(); ++i)
        if (monitorPairAnalyzer[i] >= 0)
            monitorPairWeight[i] = monitorPairTarget[i] = kUnsettledPairMonitorGain;

    // A headphone/capture-card output is never allowed to make the recording
    // inputs unavailable. Enumeration can say an output supports a rate and
    // the driver can still refuse the real exclusive open. Re-enter once with
    // no output so every input is rebuilt cleanly under the software clock,
    // then retain the output failure as a visible warning. The empty-output
    // call cannot recurse back here.
    const auto continueInputOnly = [this, &chans] (std::string outputProblem)
    {
        const bool inputsOpened = startMonitoring (chans, {});
        const auto inputProblem = monitorProblem;

        if (inputsOpened)
        {
            monitorProblem = std::move (outputProblem)
                           + " Recording is available, but live headphone monitoring is off.";

            // A microphone that would not open is still news when the output
            // is the thing being explained. This used to keep only the output's
            // sentence, so a rig with a shared output AND a dead microphone was
            // told about the headphones and nothing about the silent track.
            if (! inputProblem.empty())
                monitorProblem += " " + inputProblem;

            return true;
        }

        monitorProblem = std::move (outputProblem);
        if (! inputProblem.empty())
            monitorProblem += " " + inputProblem;
        return false;
    };

    // §5.4: the monitor path must be exclusive-mode. If it is not available,
    // say so and name the cause rather than silently delivering 40 ms. The
    // microphones still reopen input-only; monitoring failure is not recording
    // failure.
    monitoringLatencyMs = 0.0;
    alignedInputLatencyFrames = 0;

    if (! outputDeviceId.empty())
    {
        const auto capability = backend.checkExclusiveModeCapability (outputDeviceId, sampleRate, bufferSize);

        if (! capability.exclusiveModeAvailable)
        {
            monitorProblem = capability.unavailableReason.empty()
                ? std::string ("Low-latency monitoring isn't available on this sound output.")
                : capability.unavailableReason;
            return continueInputOnly (monitorProblem);
        }

        // Kept rather than dropped. All three backends work this figure out and
        // every one of them threw it away here, so Application's
        // measuredLatencyMs was never assigned by anything: the Advanced panel
        // reported monitoring latency as "0.0 ms" -- which is not a small
        // number, it is an impossible one -- and every take's session.json
        // recorded 0.0 as a permanent fact about how the take was made.
        //
        // §5.4 is about this number. It is what someone singing to a click
        // reads to decide whether they can work through the headphones at all.
        monitoringLatencyMs = capability.measuredOrEstimatedLatencyMs;
    }

    // Grouped before the output opens, because a mixer that is also the
    // headphone output is ONE device and must be opened once.
    //
    // A small livestream mixer presents its microphone inputs and its monitor
    // output as a single duplex device. Opening it for output, taking hog mode
    // on it, and then opening it again for input asks macOS for a second IOProc
    // on a device this process has just claimed exclusively -- and the refusal
    // arrives as "couldn't be opened for recording" against a microphone that
    // is plugged in and working.
    //
    // One IOProc is also simply how CoreAudio means a duplex device to be
    // driven: it hands that single proc both halves of the same cycle.
    std::map<std::string, std::vector<size_t>> byDevice;

    for (size_t i = 0; i < channels.size(); ++i)
        byDevice[channels[i].deviceId].push_back (i);

    // The take channels, if any, that come off the very device feeding the
    // headphones. Empty on a rig where the microphones and the output are
    // different boxes, which leaves that rig working exactly as before.
    std::vector<std::pair<int, int>> outputDeviceRouting;

    if (! outputDeviceId.empty())
    {
        const auto shared = byDevice.find (outputDeviceId);

        if (shared != byDevice.end())
        {
            outputDeviceRouting = routingFor (shared->second);
            byDevice.erase (shared);
        }
    }

    // §5.2: exactly one output stream, ever. It is also the clock -- §3.1 needs
    // one timebase, and the device feeding the headphones is the one whose
    // deadline actually matters.
    auto outputCallback = [this, outputDeviceRouting] (const float* const* inputs, int numInputs,
                                                       float* const* outputs, int numOutputs,
                                                       int numSamples)
    {
        // Set aside (abandonOutputClock), or the platform has just said this
        // device changed its rate and the app has yet to hear of it
        // (IAudioBackend::isOutputOffRate): either way it is calling back at
        // a rate the rig is not running at, and the software clock is
        // pulling. Silence, before the stamp, so the takeover's catch-up
        // starts from the last block this callback actually pulled.
        //
        // The backend's word is taken before the app's: in the half second
        // until the app's next poll, this callback went on pulling every
        // microphone at the other rate. That shortfall was never pulled back
        // -- the takeover started from a callback a moment old -- so every
        // track ran that much short of the wall clock and the cameras for
        // the rest of the take, with every ring left full behind it.
        //
        // A mixer whose microphones come in on this callback goes quiet with
        // it for that moment. Their rate is the output's, so their audio is
        // no longer the take's format either, and the take stops for them
        // (OutputRateChangeAction::stopTake).
        if (outputAbandoned.load (std::memory_order_acquire) || backend.isOutputOffRate())
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs != nullptr && outputs[ch] != nullptr)
                    std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);
            return;
        }

        // The software clock watches this stamp: a callback that stops
        // arriving hands the rig over to it within a few buffer periods.
        lastOutputCallbackNs.store (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                                    std::memory_order_relaxed);

        // Both halves of one cycle on a duplex mixer. The microphones are read
        // from the same callback that fills the headphones, because they are
        // the same device and there is only one stream open on it.
        if (! outputDeviceRouting.empty())
            fanOutDeviceInputs (outputDeviceRouting, inputs, numInputs, numSamples, false);

        // Never block here (§11). If the software clock is mid-pull -- only
        // possible in the moment the output comes back -- this cycle's
        // headphone buffer is silence and the rings are pulled on the next.
        if (pulling.exchange (true, std::memory_order_acq_rel))
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs != nullptr && outputs[ch] != nullptr)
                    std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);
            return;
        }

        processOutputBlock (outputs, numOutputs, numSamples);
        pulling.store (false, std::memory_order_release);
    };

    if (! outputDeviceId.empty()
        && ! backend.openExclusiveOutputStream (outputDeviceId, sampleRate, bufferSize, outputCallback))
    {
        // Prefer whatever the backend can say about the specific device; the
        // generic line leaves the user with no next step.
        const auto backendReason = backend.getLastOpenError();
        monitorProblem = backendReason.empty()
            ? std::string ("Couldn't open your headphones for low-latency playback.")
            : backendReason;
        backend.closeAllStreams();

        // The preflight said yes and the open said no, so the figure it
        // produced describes a stream that does not exist. Reporting it would
        // put a monitoring latency beside monitoring that is switched off.
        monitoringLatencyMs = 0.0;
        return continueInputOnly (monitorProblem);
    }

    // The estimate above came from the buffer size we ASKED for. Now that the
    // stream exists, the device can be asked what it actually granted -- which
    // is routinely different, since a driver is free to align the request up to
    // its own period. CoreAudio already tells the user there is "a little more
    // delay than usual" when that happens; the number beside that sentence was
    // still the figure for the buffer the device had refused.
    //
    // Zero means the backend cannot say, and then the estimate stands: a
    // latency of nothing is the one answer that is certainly wrong.
    monitorOutputFrames = 0;
    monitorPresentationFrames = 0;

    if (! outputDeviceId.empty())
    {
        const int granted = backend.getGrantedOutputBufferFrames();
        monitorOutputFrames = granted > 0 ? granted : std::max (1, bufferSize);
        monitorPresentationFrames = std::max (0, backend.getOutputPresentationLatencyFrames());

        if (granted > 0 && sampleRate > 0.0)
        {
            // Plus what the device adds after the buffers. Bluetooth reports
            // well over 100 ms there, and leaving it out printed about 3 ms.
            monitoringLatencyMs = ((2.0 * granted + monitorPresentationFrames) / sampleRate) * 1000.0;
        }
    }

    // One stream per remaining DEVICE, not per channel.
    //
    // An audio interface with four microphones plugged into it is one device
    // presenting four inputs, and four take channels come off it. Opening that
    // device once per channel would ask the OS for the same exclusive stream
    // four times; on macOS the second open is refused and the take dies with a
    // message naming a microphone that is plugged in and working.
    std::vector<std::string> failedDevices;
    std::string firstFailure;

    for (const auto& [deviceId, channelIndices] : byDevice)
    {
        // Captured by value into the callback so the audio thread never reaches
        // back into a container the UI thread can touch.
        const auto routing = routingFor (channelIndices);

        auto inputCallback = [this, routing] (const float* const* inputs, int numInputs,
                                              float* const*, int, int numSamples)
        {
            fanOutDeviceInputs (routing, inputs, numInputs, numSamples, true);
        };

        if (! backend.openInputStream (deviceId, sampleRate, bufferSize, inputCallback))
        {
            // §0.1: the backend knows why and this used to throw it away, so the
            // user was told a microphone "couldn't be opened" and left to guess
            // between a dead cable, a rate mismatch, a missing permission and
            // another app holding the device. The monitor path above has always
            // reported the cause; the microphone path is no different.
            const auto reason = backend.getLastOpenError();

            // One microphone's failure is not every microphone's failure.
            //
            // This used to closeAllStreams() and give up the moment ANY device
            // refused, so a four-microphone rig with one dead cable recorded
            // NOTHING -- not the three that were working perfectly. The button
            // then read "The microphones aren't open", which was true only
            // because this function had just closed them. Being unable to
            // record at all is the largest loss §0.1 can take, and a broken
            // microphone is the most ordinary way for a gig to go wrong.
            //
            // continueInputOnly above already settles the principle for the
            // output -- "monitoring failure is not recording failure" -- and an
            // input is no different: carry on with the devices that opened,
            // keep the ones that did not as a named warning, and refuse only
            // when NOTHING opened.
            failedDevices.push_back (deviceId);

            if (firstFailure.empty())
                firstFailure = channels[channelIndices.front()].displayName
                             + " couldn't be opened for recording."
                             + (reason.empty() ? std::string() : " " + reason);

            continue;
        }
    }

    // Nothing opened at all. This is the case the record button's guard is
    // really for: a take now would write nothing but empty files with the clock
    // running.
    //
    // A mixer that is also the headphone output is carrying microphones of its
    // own through outputDeviceRouting, and it HAS opened. A second mic failing
    // beside it used to count as "nothing opened" and closed the whole rig,
    // mixer included -- Record refused while every mixer channel was live.
    if (! byDevice.empty() && failedDevices.size() == byDevice.size()
        && outputDeviceRouting.empty())
    {
        monitorProblem = firstFailure;
        backend.closeAllStreams();
        return false;
    }

    // The software clock runs for the life of the rig. With an output stream
    // it only steps in when that stream goes quiet; without one it is the
    // clock from the first block.
    outputStreamOpen = ! outputDeviceId.empty();
    outputCarriesChannels = outputStreamOpen && ! outputDeviceRouting.empty();
    openOutputDeviceId = outputDeviceId;
    outputClockLost.store (false, std::memory_order_relaxed);
    lastOutputCallbackNs.store (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                    std::chrono::steady_clock::now().time_since_epoch()).count(),
                                std::memory_order_relaxed);
    if (softwareClockEnabled)
    {
        clockRunning.store (true, std::memory_order_release);
        softwareClock = std::thread ([this] { runSoftwareClock(); });
    }

    // Input latency. Each device hands its audio over some fixed time after
    // the microphone heard it -- its own latency, its safety offset, its
    // stream's -- and two interfaces seldom agree: a clap both microphones
    // heard at once arrives in the two channels that many samples apart. The
    // headphones take each as it comes: holding the quicker ones back to the
    // slowest would put the slowest device's delay in everyone's ears
    // (§5.4). The writer lines the stems up instead, from these figures and
    // each device's IO block (updateRecordingOffsets), so the tracks land
    // as the room heard them. Devices that did not open take no part; their
    // channels are silent anyway.
    {
        // Far past any wired interface's figure, and a bound on what a
        // confused driver's report can do to the arithmetic below.
        constexpr int kLargestCredibleInputLatency = 24000;

        int longest = 0;

        for (size_t i = 0; i < channels.size() && i < channelInputLatency.size(); ++i)
        {
            const bool failed = std::find (failedDevices.begin(), failedDevices.end(), channels[i].deviceId)
                                != failedDevices.end();

            if (failed)
            {
                channelInputLatency[i] = -1;
                continue;
            }

            // A figure past the bound is not believed, and the stems cannot
            // be lined up by it: the take's record keeps what the driver
            // said and says the stems are not exactly aligned, so they get
            // checked in an editor. Lining up to the bound instead put a
            // clap 26000 samples apart in two stems, twice in the mix, under
            // a record saying it was exact.
            const int reported = backend.getInputLatencyFrames (channels[i].deviceId);
            channelReportedLatency[i] = reported;
            channelInputLatency[i] = std::clamp (reported, 0, kLargestCredibleInputLatency);
            latencyClamped = latencyClamped || channelInputLatency[i] != reported;
            longest = std::max (longest, channelInputLatency[i]);
        }

        alignedInputLatencyFrames = longest;
        latenciesReady.store (true, std::memory_order_release);

        // Monitoring rebuilt under a take still running: that take's stems
        // are lined up by these figures from here on.
        if (pipeline != nullptr)
            latencyClampedThisTake = latencyClampedThisTake || latencyClamped;
    }

    monitoring = true;

    // §5.5: the headphone mix is watched for feedback for as long as it
    // plays. Input-only, there is no headphone mix to watch, and no thread
    // waking a hundred times a second for nothing.
    if (! outputDeviceId.empty())
        feedbackGuard.start();

    // The devices that would not open, now that monitoring is genuinely up.
    //
    // Marked not-live so their channels write silence rather than a held
    // sample -- the same answer §6.5 already gives a microphone that goes away
    // MID-take, because a microphone that never arrived is the same problem one
    // step earlier. Recorded by id as well, so the take's own record can say
    // why that stem is silent instead of leaving it to be discovered in the
    // files afterwards.
    devicesThatFailedToOpen.clear();

    for (const auto& deviceId : failedDevices)
    {
        devicesThatFailedToOpen.push_back (deviceId);
        setChannelLive (deviceId, false);
    }

    if (! failedDevices.empty())
        monitorProblem = firstFailure
                       + " Recording is available from the other microphones; this one's"
                         " track will be silent.";

    return true;
}

void CaptureCoordinator::runSoftwareClock()
{
    using clock = std::chrono::steady_clock;

    // The same scheduling class as the device threads that fill the rings this
    // drains. Left at normal priority, the consumer was the one thread in the
    // audio path the producers could preempt: with an 8 ms margin in each
    // ring, an ordinary scheduling delay overflowed it, and the delays read as
    // drift. Best effort -- where the OS refuses, the clock runs as before.
   #if defined (__linux__) || defined (__APPLE__)
    {
        sched_param param {};
        param.sched_priority = std::min (80, sched_get_priority_max (SCHED_FIFO));
        pthread_setschedparam (pthread_self(), SCHED_FIFO, &param);
    }
   #endif

    const auto period = std::chrono::nanoseconds (
        static_cast<int64_t> (1.0e9 * static_cast<double> (std::max (1, bufferSize)) / std::max (1.0, sampleRate)));

    // "Gone quiet" is several periods, with a floor so a tiny buffer does not
    // declare a healthy output dead on scheduler jitter. A real output that
    // has not called back for a tenth of a second has stopped.
    const auto lostAfter = std::max (period * 8, std::chrono::nanoseconds (100'000'000));

    // How far behind the clock may fall and still be caught up: well past
    // scheduler jitter, well inside the rings' capacity.
    constexpr auto kMaxCatchUp = std::chrono::milliseconds (100);

    // The one catch-up that is allowed to be longer: a takeover's, which
    // starts from the dead output's last callback and so is always at least
    // lostAfter behind -- plus the standby loop's quarter window, plus slack.
    const auto takeoverCatchUp = lostAfter + lostAfter / 4 + kMaxCatchUp;

    // A wake this late is a stall the whole process shared, not this thread's
    // alone. The device threads woke from it at the same instant, each with a
    // driver ring full of audio to hand over; pulled first, the catch-up ticks
    // below would find the rings dry and write silence for audio that arrives
    // a moment later -- audio the streams then skip to stay in step, so the
    // stall costs the take twice over. A moment's grace lets the producers
    // land their bursts first. Once per stall, on entering catch-up.
    constexpr auto kBurstGrace = std::chrono::milliseconds (1);
    const auto sharedStall = period * 2;

    auto next = clock::now() + period;
    bool wasTakingOver = ! outputStreamOpen;
    bool catchingUp = false;
    bool handingOver = false; // pulling back a takeover's owed ticks

    while (clockRunning.load (std::memory_order_acquire))
    {
        const auto now = clock::now();

        // An abandoned output goes on calling back -- at the wrong rate -- so
        // its stamp never goes stale; the flag is the only way to know. One
        // the platform has just said changed rate is taken over from the
        // same word, at this look rather than a stale window later: its
        // callback has stopped pulling, and the rings would back up behind
        // it for a tenth of a second.
        bool takeOver = ! outputStreamOpen || outputAbandoned.load (std::memory_order_acquire)
                     || backend.isOutputOffRate();

        if (! takeOver)
        {
            const auto last = std::chrono::nanoseconds (lastOutputCallbackNs.load (std::memory_order_relaxed));
            const auto sinceLast = std::chrono::duration_cast<std::chrono::nanoseconds> (now.time_since_epoch()) - last;
            takeOver = sinceLast > lostAfter;
        }

        outputClockLost.store (outputStreamOpen && takeOver, std::memory_order_relaxed);

        // A takeover picks up where the dead output left off: the first tick
        // it owes is one period after that output's last callback, not one
        // after the loss was noticed. Starting from `now` threw away the
        // lostAfter-plus that it took to notice -- never pulled, so every
        // stem and the mix came out a tenth of a second shorter than the wall
        // clock, and the camera. Those ticks are pulled back to back below.
        //
        // The rings hold only a fraction of that gap, so most of it is gone
        // before the takeover starts: the ring kept the oldest of it and the
        // rest overflowed, counted. The catch-up plays what the ring kept in
        // its place and writes the overflowed span as counted silence, so the
        // take stays wall-clock length and the audio after the gap lands where
        // it happened. Nothing that was lost is made to look recorded.
        if (takeOver && ! wasTakingOver)
        {
            next = now + period;
            handingOver = false; // not one left over from an earlier takeover

            if (outputStreamOpen)
            {
                const auto last = clock::time_point (std::chrono::duration_cast<clock::duration> (
                    std::chrono::nanoseconds (lastOutputCallbackNs.load (std::memory_order_relaxed))));

                if (last + period < next && now - last < takeoverCatchUp)
                {
                    next = last + period;
                    handingOver = true;
                }
            }
        }

        wasTakingOver = takeOver;

        if (! takeOver)
        {
            // The output is doing its job. Look again well before it could
            // have been declared lost, and reset the tick origin so the first
            // pull after a loss is not a burst of catch-up ticks.
            std::this_thread::sleep_for (lostAfter / 4);
            next = clock::now() + period;
            continue;
        }

        // Absolute deadlines, and a late wake is caught up rather than
        // forgiven. This used to restart the schedule whenever a wake came
        // more than one period late -- routine at a 1.3 ms period on a busy
        // machine -- which silently threw those ticks away. The clock then ran
        // slow against real time, so every microphone measured fast, drift
        // correction sat pinned at its +200 ppm clamp, and the rings
        // overflowed for as long as the take lasted. Found by running the app
        // against microphones on real, independent clocks
        // (Tools/e2e_realtime_mics.sh).
        //
        // Missed ticks are pulled back to back instead: the audio they stand
        // for is already waiting in the rings. Only a stall far beyond any
        // scheduling jitter -- the process suspended, the machine asleep -- is
        // treated as a new start, since catching that up would be a burst.
        std::this_thread::sleep_until (next);

        // How late this wake was: the grace above, and diagnostics -- a
        // relaxed max on an atomic, nothing §11 forbids.
        {
            const auto woke = clock::now();
            const bool late = woke > next && woke - next >= period;

            // A takeover's owed ticks are late by design, not by scheduling.
            if (woke > next && ! handingOver)
            {
                const auto lateUs = static_cast<uint64_t> (
                    std::chrono::duration_cast<std::chrono::microseconds> (woke - next).count());
                auto seen = clockMaxWakeLateUs.load (std::memory_order_relaxed);
                while (lateUs > seen
                       && ! clockMaxWakeLateUs.compare_exchange_weak (seen, lateUs, std::memory_order_relaxed)) {}
                if (late)
                    clockCatchUpTicks.fetch_add (1, std::memory_order_relaxed);
            }

            if (late && ! catchingUp && woke - next >= sharedStall)
                std::this_thread::sleep_for (kBurstGrace);

            catchingUp = late;
        }

        next += period;
        if (clock::now() - next > (handingOver ? takeoverCatchUp : kMaxCatchUp))
            next = clock::now() + period;
        if (next > clock::now())
            handingOver = false;

        if (pulling.exchange (true, std::memory_order_acq_rel))
            continue;

        // No headphone buffer to fill: pull the rings, meter, record.
        processOutputBlock (nullptr, 0, bufferSize);
        pulling.store (false, std::memory_order_release);
    }
}

void CaptureCoordinator::stopSoftwareClock()
{
    clockRunning.store (false, std::memory_order_release);

    if (softwareClock.joinable())
        softwareClock.join();

    outputClockLost.store (false, std::memory_order_relaxed);
    outputStreamOpen = false;
}

bool CaptureCoordinator::abandonOutputClock (const std::string& why)
{
    if (! monitoring || ! outputStreamOpen || outputCarriesChannels
        || ! clockRunning.load (std::memory_order_acquire))
        return false;

    if (! outputAbandoned.exchange (true, std::memory_order_acq_rel))
    {
        // Said now rather than on the clock's next look, so the app routes
        // its own sounds away from the headphones straight away.
        outputClockLost.store (true, std::memory_order_relaxed);

        // Ahead of a problem already there (a microphone that did not open),
        // which still stands.
        monitorProblem = monitorProblem.empty() ? why : why + " " + monitorProblem;
    }

    return true;
}

uint64_t CaptureCoordinator::getOverrunSamples() const noexcept
{
    uint64_t total = 0;

    for (const auto& stream : deviceStreams)
        total += stream->getOverrunSamples();

    return total;
}

uint64_t CaptureCoordinator::getUnderrunSamples() const noexcept
{
    uint64_t total = 0;

    for (const auto& stream : deviceStreams)
        total += stream->getUnderrunSamples();

    return total;
}

uint64_t CaptureCoordinator::getWorstChannelOverrunThisTake() const noexcept
{
    uint64_t worst = 0;

    for (size_t i = 0; i < deviceStreams.size(); ++i)
    {
        const auto total = deviceStreams[i]->getOverrunSamples();

        // A stream rebuilt since the take began has a counter below its
        // baseline; everything it has counted is then this take's.
        const auto base = i < overrunBaselinePerStream.size() ? overrunBaselinePerStream[i] : 0;
        const auto sinceStart = total >= base ? total - base : total;

        worst = std::max (worst, sinceStart);
    }

    return worst;
}

bool CaptureCoordinator::isChannelLive (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return false;

    return deviceStreams[static_cast<size_t> (index)]->isLive();
}

std::vector<std::pair<int, int>> CaptureCoordinator::routingFor (const std::vector<size_t>& channelIndices) const
{
    std::vector<std::pair<int, int>> routing; // { device input, take channel }
    routing.reserve (channelIndices.size());

    for (const auto i : channelIndices)
        routing.push_back ({ channels[i].deviceChannel, static_cast<int> (i) });

    return routing;
}

void CaptureCoordinator::fanOutDeviceInputs (const std::vector<std::pair<int, int>>& routing,
                                             const float* const* inputs, int numInputs,
                                             int numSamples, bool fromInputStream) noexcept
{
    if (inputs == nullptr || numInputs <= 0)
    {
        // A microphone stream that hands over no inputs at all is telling us
        // its device produced numSamples that were lost before they could be
        // delivered -- a driver ring that overflowed while the reader thread
        // was not running. The backend counts and reports that loss itself
        // (getFramesDroppedByBackend, "before recording"); here it reaches the
        // streams, whose §3.3 measurement counts what the device's clock
        // produced, delivered or not, so a stall does not read as the clock
        // running slow. It used to be added to the layout figure as well, a
        // second count of the same loss under the wrong name. The duplex
        // output callback reaching here means only that this cycle had no
        // input half, which is ordinary and loses nothing.
        if (fromInputStream && numSamples > 0)
            for (const auto& [deviceInput, takeChannel] : routing)
                if (takeChannel >= 0 && takeChannel < static_cast<int> (deviceStreams.size()))
                    deviceStreams[static_cast<size_t> (takeChannel)]->noteSamplesLostBeforeDelivery (numSamples);
        return;
    }

    // A fresh two-channel device has to be heard before §2.1 can tell whether
    // it is one stereo-presenting microphone or two independent interface
    // sockets. Observe only the exact, fully selected physical pair. Routing
    // still falls through to the ordinary loop below, preserving both inputs
    // until Application persists the verdict and rebuilds the idle capture.
    if (routing.size() == 2 && numInputs >= 2 && inputs[0] != nullptr && inputs[1] != nullptr)
    {
        int analyzerChannel = -1;
        bool hasPhysicalInput0 = false;
        bool hasPhysicalInput1 = false;

        for (const auto& [deviceInput, takeChannel] : routing)
        {
            hasPhysicalInput0 = hasPhysicalInput0 || deviceInput == 0;
            hasPhysicalInput1 = hasPhysicalInput1 || deviceInput == 1;

            if (takeChannel >= 0
                && takeChannel < static_cast<int> (channels.size())
                && channels[static_cast<size_t> (takeChannel)].analyzeStereoPair)
                analyzerChannel = takeChannel;
        }

        if (analyzerChannel >= 0 && hasPhysicalInput0 && hasPhysicalInput1)
            analyzeStereoPair (analyzerChannel, inputs[0], inputs[1], numSamples, false);
    }

    // Route only the physical inputs the take explicitly selected. A one-entry
    // route for input 0 used to inspect input 1 as well and, when input 0 was
    // silent, substitute input 1. That made a disabled/unselected socket part
    // of the recording despite the fixed channel plan. A stereo device still
    // records both sides when both appear in `routing`; the ordinary loop below
    // sends each selected physical input to its own take channel. The sole
    // exception requires the channel plan's persisted, explicit verdict that
    // the pair is one stereo-presenting microphone rather than an interface.
    if (routing.size() == 1)
    {
        const int takeChannel = routing[0].second;
        if (takeChannel >= 0
            && takeChannel < static_cast<int> (channels.size())
            && channels[static_cast<size_t> (takeChannel)].collapseStereoPair
            && numInputs >= 2)
        {
            pushDeviceBlockMultiChannel (takeChannel, inputs, numInputs, numSamples);
            return;
        }
    }

    // One block, one count. Adding per missing channel inflated the figure by
    // however many channels the device came up short, which is not how long the
    // audio was.
    bool missedAChannel = false;

    for (const auto& [deviceInput, takeChannel] : routing)
    {
        if (deviceInput < numInputs && inputs[deviceInput] != nullptr)
        {
            pushDeviceBlock (takeChannel, inputs[deviceInput], numSamples);
        }
        else
        {
            missedAChannel = true;
            // The device delivered fewer inputs than this take was planned
            // around -- an interface renegotiating, a driver handing over two
            // of four. That channel gets nothing and writes silence, which is
            // the right behaviour (§6.5 fixes the layout for the take), but it
            // used to leave no trace at all: audio that should have been
            // recorded simply was not, and §0.1 does not allow that to be
            // invisible. Counted here, on the same counter the take reports.
        }
    }

    if (missedAChannel && numSamples > 0)
        framesMissedByLayout.fetch_add (static_cast<uint64_t> (numSamples),
                                        std::memory_order_relaxed);
}

void CaptureCoordinator::stopMonitoring()
{
    if (! monitoring)
        return;

    // The clock first: it pulls the rings that the streams below feed, and it
    // must not be mid-pull while the streams are torn down.
    stopSoftwareClock();
    backend.closeAllStreams();
    outputAbandoned.store (false, std::memory_order_release);
    outputCarriesChannels = false;
    openOutputDeviceId.clear();
    monitoring = false;
    latenciesReady.store (false, std::memory_order_relaxed);
    feedbackGuard.stop();

    // Nothing is monitoring, so there is no monitoring latency to report. A
    // figure left standing here would outlive the stream it describes.
    monitoringLatencyMs = 0.0;
    monitorOutputFrames = 0;
    monitorPresentationFrames = 0;
    alignedInputLatencyFrames = 0;
}

void CaptureCoordinator::stopMirroring()
{
    if (pipeline != nullptr)
        pipeline->stopMirroring();
}

bool CaptureCoordinator::startRecording (const std::string& sessionFolder, int bitDepth,
                                         const std::string& originTimestamp,
                                         const std::string& mirrorFolder)
{
    recordingProblem.clear();

    if (channels.empty())
    {
        recordingProblem = "There are no external microphones to record. Plug in a USB microphone "
                           "or audio interface and try again.";
        return false;
    }

    if (isRecording())
        return false;

    std::vector<WriteChannelSpec> specs;
    specs.reserve (channels.size());

    for (const auto& ch : channels)
        specs.push_back ({ ch.fileName, ch.trimDb, ch.bitDepth });

    // Opening the files is the take's first write to the card, so it runs on
    // a disposable worker with a deadline. A card pulled at that instant used
    // to hold the caller -- the message thread -- inside open() indefinitely.
    struct StartState
    {
        std::mutex mutex;
        std::condition_variable done;
        std::unique_ptr<WritePipeline> pipeline = std::make_unique<WritePipeline>();
        bool finished = false;
        bool started = false;
        bool abandoned = false;
    };

    auto state = std::make_shared<StartState>();
    if (writerChunkHookForTesting)
        state->pipeline->setChunkHookForTesting (writerChunkHookForTesting);
    const double rate = sampleRate;
    auto stall = filesystemStallForTesting;

    std::thread ([state, specs, rate, bitDepth, sessionFolder, originTimestamp, mirrorFolder, stall]
    {
        if (stall)
            stall();

        const bool ok = state->pipeline->start (sessionFolder, specs, rate, bitDepth,
                                                originTimestamp, mirrorFolder);

        std::unique_lock<std::mutex> lock (state->mutex);
        state->started = ok;
        state->finished = true;

        if (state->abandoned)
        {
            // Nobody will publish it: close what was opened, here, off the
            // caller's thread.
            auto orphan = std::move (state->pipeline);
            lock.unlock();
            orphan.reset();
            return;
        }

        lock.unlock();
        state->done.notify_all();
    }).detach();

    std::unique_ptr<WritePipeline> p;
    {
        std::unique_lock<std::mutex> lock (state->mutex);

        if (! state->done.wait_for (lock, filesystemDeadline, [&state] { return state->finished; }))
        {
            state->abandoned = true;
            recordingProblem = "The card stopped answering while the take's files were being opened, "
                               "so recording didn't start. Check the card is plugged in, or choose "
                               "somewhere else to record.";
            return false;
        }

        if (! state->started)
        {
            // Carried up rather than collapsed back into a bool. The pipeline
            // knows which file it could not open and where; by the time a bare
            // false reaches the UI that is gone, and the user gets a record
            // button that does nothing for no stated reason.
            recordingProblem = state->pipeline->getStartProblem();
            return false;
        }

        p = std::move (state->pipeline);
    }

    // Published only once fully started, so the audio thread never sees a
    // half-built pipeline. The release store pairs with the acquire load in
    // mixAndPublish: a callback that sees the pointer also sees every write
    // start() made to the object behind it.
    // Zeroed here, not at the top of this function: above the guards it also
    // zeroed a RUNNING take's counter whenever startRecording was called again
    // and refused -- mid-take contamination in the other direction. This is the
    // point at which a take genuinely begins.
    //
    // Zeroed at all because the counter used to run for the life of the
    // coordinator, so take three's record carried take one's losses plus
    // everything that happened while merely monitoring.
    framesMissedByLayout.store (0, std::memory_order_relaxed);
    overrunAtTakeStart = getOverrunSamples();
    underrunAtTakeStart = getUnderrunSamples();

    // Same reasoning as the counter above: without this, the figures from the
    // previous take would stand as this one's until enough blocks had gone by.
    lastTakeCardWriteProblem.clear();
    lastTakeLiveSessionFolder.clear();
    lastTakeCardWriteFailed = false;
    lastTakeMirrorWriteFailed = false;
    lastTakeLufs = LoudnessMeter::kSilenceLufs;
    lastTakeTruePeakDbtp = LoudnessMeter::kSilenceLufs;
    lastTakeLoudnessBlocks = 0;
    lastTakeFramesDropped = 0;

    overrunBaselinePerStream.clear();
    overrunBaselinePerStream.reserve (deviceStreams.size());

    for (const auto& stream : deviceStreams)
        overrunBaselinePerStream.push_back (stream->getOverrunSamples());

    lastTakeAlignmentSilence.clear();
    lastTakeAlignmentDropped.clear();
    lastTakeAlignmentStart.clear();
    lastTakeStemsAligned = false;
    latencyClampedThisTake = latencyClamped;

    // A stream's shift counter runs for as long as the stream does, across
    // takes; this take's share is measured from here.
    shiftSilenceBaselinePerStream.clear();
    shiftSilenceBaselinePerStream.reserve (deviceStreams.size());

    for (const auto& stream : deviceStreams)
        shiftSilenceBaselinePerStream.push_back (stream->getAlignmentSilenceSamples());

    lastTakeShiftSilence.clear();

    pipeline = std::move (p);
    activePipeline.store (pipeline.get(), std::memory_order_release);
    return true;
}

void CaptureCoordinator::stopRecording()
{
    if (pipeline == nullptr)
        return;

    // Retire the pointer first, then wait for any callback already past the
    // load to finish with it. Without this the audio thread could be inside
    // pushBlock while stop() joined the writer thread and freed the ring out
    // from under it -- a use-after-free that would present as an intermittent
    // crash on stopping a take, which is the worst possible moment for one.
    // Sequentially consistent, deliberately: this is a store on one thread
    // followed by a load, against a load-after-increment on the other. With
    // release/acquire alone x86 may reorder the store past the load and both
    // threads can conclude they are alone -- a use-after-free window that is
    // nanoseconds wide and only on Intel and Windows. seq_cst closes it.
    activePipeline.store (nullptr, std::memory_order_seq_cst);

    while (pipelineUsers.load (std::memory_order_seq_cst) != 0)
        std::this_thread::yield();

    // The final drain and close run on a disposable worker with a deadline.
    // The writer's last flush goes to the card, and a card pulled during Stop
    // used to hold the message thread inside that join indefinitely. The
    // audio thread has already let go of the pipeline above, so abandoning it
    // to the worker is safe; it is released whenever the card answers.
    struct StopResult
    {
        std::string cardWriteProblem;
        std::string liveSessionFolder;
        bool cardWriteFailed = false;
        bool mirrorWriteFailed = false;
        double lufs = LoudnessMeter::kSilenceLufs;
        double truePeakDbtp = LoudnessMeter::kSilenceLufs;
        int loudnessBlocks = 0;
        uint64_t framesDropped = 0;
        std::vector<uint64_t> alignmentSilence, alignmentDropped;
        std::vector<int> alignmentStart;
        bool stemsAligned = false;
    };

    struct StopState
    {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        StopResult result;
    };

    auto state = std::make_shared<StopState>();

    // Read here too, for the timed-out path below: the counter is atomic and
    // the audio thread has let go, so this is every drop but the final drain's.
    lastTakeFramesDropped = pipeline->getFramesDropped();
    lastTakeLiveSessionFolder = pipeline->getLiveSessionFolder();

    // So is the alignment account, bar whatever the final drain changes;
    // the stop's own snapshot below replaces it when the stop finishes.
    {
        const auto count = static_cast<int> (channels.size());
        lastTakeAlignmentSilence.assign (channels.size(), 0);
        lastTakeAlignmentDropped.assign (channels.size(), 0);
        lastTakeAlignmentStart.assign (channels.size(), 0);

        for (int ch = 0; ch < count; ++ch)
        {
            lastTakeAlignmentSilence[static_cast<size_t> (ch)] = pipeline->getChannelAlignmentSilence (ch);
            lastTakeAlignmentDropped[static_cast<size_t> (ch)] = pipeline->getChannelAlignmentDropped (ch);
            lastTakeAlignmentStart[static_cast<size_t> (ch)] = pipeline->getChannelStartAlignmentOffset (ch);
        }

        lastTakeStemsAligned = pipeline->isAlignmentExact() && ! latencyClampedThisTake;

        // The streams run on after the take; what they had shifted by now is
        // the take's.
        lastTakeShiftSilence.assign (deviceStreams.size(), 0);

        for (size_t i = 0; i < deviceStreams.size(); ++i)
            lastTakeShiftSilence[i] = channelShiftSilenceThisTake (i);
    }

    std::shared_ptr<WritePipeline> p (std::move (pipeline));
    const auto channelCount = static_cast<int> (channels.size());
    auto stall = filesystemStallForTesting;

    std::thread ([state, p, stall, channelCount]
    {
        if (stall)
            stall();

        p->stop();

        // AFTER stop(). The loudness meter is fed on the writer thread and
        // stop() performs the final flush, so a snapshot taken before it would
        // miss the end of the take -- on a short take, most of it. stop() is
        // also where every writer's final close() happens, so a header rewrite
        // that failed at the very end is included here rather than dying with
        // the object that noticed it.
        StopResult r;
        r.cardWriteProblem = p->getCardWriteProblem();
        r.liveSessionFolder = p->getLiveSessionFolder();
        r.cardWriteFailed = p->hasCardWriteFailed();
        r.mirrorWriteFailed = p->hasMirrorWriteFailed();
        r.lufs = p->getIntegratedLufs();
        r.truePeakDbtp = p->getTruePeakDbtp();
        r.loudnessBlocks = p->getLoudnessBlockCount();
        r.framesDropped = p->getFramesDropped();

        for (int ch = 0; ch < channelCount; ++ch)
        {
            r.alignmentSilence.push_back (p->getChannelAlignmentSilence (ch));
            r.alignmentDropped.push_back (p->getChannelAlignmentDropped (ch));
            r.alignmentStart.push_back (p->getChannelStartAlignmentOffset (ch));
        }

        r.stemsAligned = p->isAlignmentExact();

        {
            const std::lock_guard<std::mutex> lock (state->mutex);
            state->result = std::move (r);
            state->finished = true;
        }

        state->done.notify_all();
    }).detach();

    // The deadline runs from the writer's last sign of life, not from Stop.
    // A card with a backlog -- the "drive is falling behind" case -- can need
    // well over five seconds to drain and close everything, and calling that
    // dead skipped the final session.json and cut the combined video short.
    // A card that has really stopped answering makes no progress and still
    // times out after the same deadline.
    std::unique_lock<std::mutex> lock (state->mutex);
    {
        auto lastProgress = p->getProgress();
        const auto stopStarted = std::chrono::steady_clock::now();
        auto lastMove = stopStarted;

        // Still bounded: Stop runs on the message thread, and a card crawling
        // along a few bytes at a time must not hold the app up indefinitely.
        constexpr auto kMostAStopMayTake = std::chrono::seconds (60);
        const auto poll = std::min<std::chrono::milliseconds> (
            std::chrono::milliseconds (250),
            std::chrono::duration_cast<std::chrono::milliseconds> (filesystemDeadline));

        while (! state->finished)
        {
            state->done.wait_for (lock, std::max (poll, std::chrono::milliseconds (1)),
                                  [&state] { return state->finished; });

            if (state->finished)
                break;

            const auto now = std::chrono::steady_clock::now();
            if (const auto seen = p->getProgress(); seen != lastProgress)
            {
                lastProgress = seen;
                lastMove = now;
            }
            else if (now - lastMove >= filesystemDeadline)
            {
                break;
            }

            if (now - stopStarted >= kMostAStopMayTake)
                break;
        }
    }
    lastStopTimedOut = ! state->finished;

    if (lastStopTimedOut)
    {
        // Without the snapshot the figures died with the pipeline and §10's
        // delivery advice reverted to "not enough sound" -- but here there is
        // no snapshot to take. Say what happened instead of implying a clean
        // take: the end of it may not have reached the card.
        lastTakeCardWriteProblem = "The card stopped answering while the take was being finished, so "
                                   "the end of the recording may not have been saved to it.";
        lastTakeCardWriteFailed = true;
        lastTakeMirrorWriteFailed = false;
        lastTakeLufs = LoudnessMeter::kSilenceLufs;
        lastTakeTruePeakDbtp = LoudnessMeter::kSilenceLufs;
        lastTakeLoudnessBlocks = 0;
        return;
    }

    lastTakeCardWriteProblem = state->result.cardWriteProblem;
    if (! state->result.liveSessionFolder.empty())
        lastTakeLiveSessionFolder = state->result.liveSessionFolder;
    lastTakeCardWriteFailed = state->result.cardWriteFailed;
    lastTakeMirrorWriteFailed = state->result.mirrorWriteFailed;
    lastTakeLufs = state->result.lufs;
    lastTakeTruePeakDbtp = state->result.truePeakDbtp;
    lastTakeLoudnessBlocks = state->result.loudnessBlocks;
    lastTakeFramesDropped = state->result.framesDropped;
    lastTakeAlignmentSilence = state->result.alignmentSilence;
    lastTakeAlignmentDropped = state->result.alignmentDropped;
    lastTakeAlignmentStart = state->result.alignmentStart;
    lastTakeStemsAligned = state->result.stemsAligned && ! latencyClampedThisTake;
}

bool CaptureCoordinator::isDeviceDelivering (const std::string& deviceId,
                                             std::chrono::milliseconds within) const
{
    const auto windowNs = std::chrono::duration_cast<std::chrono::nanoseconds> (within).count();

    for (size_t i = 0; i < channels.size() && i < deviceStreams.size(); ++i)
        if (channels[i].deviceId == deviceId && deviceStreams[i]->deliveredWithin (windowNs))
            return true;

    return false;
}

void CaptureCoordinator::setChannelLive (const std::string& deviceId, bool live)
{
    for (size_t i = 0; i < channels.size(); ++i)
    {
        if (channels[i].deviceId != deviceId)
            continue;

        if (pipeline != nullptr)
            pipeline->setChannelLive (static_cast<int> (i), live);

        if (i < deviceStreams.size())
            deviceStreams[i]->setLive (live);

        // No early return: an interface contributes several channels under
        // one deviceId, and stopping at the first left the rest writing a
        // held sample for the whole take and replaying stale audio on
        // reconnect.
    }
}

void CaptureCoordinator::setChannelTrimDb (int index, float trimDb) noexcept
{
    if (index < 0 || index >= static_cast<int> (channels.size())
        || index >= static_cast<int> (trimGains.size()))
        return;

    channels[static_cast<size_t> (index)].trimDb = trimDb;

    // One aligned float store, so the callback either sees the old gain or the
    // new one -- never a torn value and never a resized vector (§11).
    trimGains[static_cast<size_t> (index)] = MonitorBus::trimDbToLinearGain (trimDb);

    if (pipeline != nullptr)
        pipeline->setChannelTrimDb (index, trimDb);
}

float CaptureCoordinator::getChannelTrimDb (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channels.size()))
        return 0.0f;

    return channels[static_cast<size_t> (index)].trimDb;
}

Metering* CaptureCoordinator::getChannelMetering (int index) noexcept
{
    if (index < 0 || index >= static_cast<int> (channelMeters.size()))
        return nullptr;

    return channelMeters[static_cast<size_t> (index)].get();
}

void CaptureCoordinator::pushDeviceBlock (int deviceIndex, const float* samples, int numSamples) noexcept
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int> (deviceStreams.size()))
    {
        // Audio that arrived for a channel this coordinator does not have.
        // Nowhere to put it, so it is lost -- and, until now, lost in silence.
        framesMissedByLayout.fetch_add (static_cast<uint64_t> (std::max (0, numSamples)),
                                        std::memory_order_relaxed);
        return;
    }

    // §3.2: straight into this device's own ring. The output clock decides when
    // it is consumed, and at what rate.
    deviceStreams[static_cast<size_t> (deviceIndex)]->pushBlock (samples, numSamples);
}

void CaptureCoordinator::pushDeviceBlockMultiChannel (int deviceIndex, const float* const* inputs,
                                                      [[maybe_unused]] int numInputs,
                                                      int numSamples) noexcept
{
    // inputs and numInputs are guaranteed by the only caller, which checks both
    // before routing here; the index and sample count are the ones worth
    // testing. Kept as a guard rather than an assertion because this runs on
    // the audio thread, where being wrong must not be fatal.
    if (deviceIndex < 0 || deviceIndex >= static_cast<int> (channelLayouts.size())
        || numSamples <= 0)
    {
        if (numSamples > 0)
            framesMissedByLayout.fetch_add (static_cast<uint64_t> (numSamples),
                                            std::memory_order_relaxed);
        return;
    }

    const float* left = inputs[0];
    const float* right = inputs[1];

    if (left == nullptr || right == nullptr)
    {
        // Falling back to the first pointer is right when it is the right one
        // that is missing. When the FIRST is null there is nothing to fall back
        // to: pushBlock discards a null block, so this path lost a block
        // without counting it while every neighbouring path counted.
        if (left == nullptr)
        {
            if (numSamples > 0)
                framesMissedByLayout.fetch_add (static_cast<uint64_t> (numSamples),
                                                std::memory_order_relaxed);
            return;
        }

        return pushDeviceBlock (deviceIndex, left, numSamples);
    }

    analyzeStereoPair (deviceIndex, left, right, numSamples, true);

    const int side = channelLayouts[static_cast<size_t> (deviceIndex)]->source.load (
        std::memory_order_relaxed);

    // By pointer: collapsing to mono is choosing which channel to read, not
    // copying one.
    pushDeviceBlock (deviceIndex, side == 1 ? right : left, numSamples);
}

void CaptureCoordinator::analyzeStereoPair (int channelIndex, const float* left,
                                            const float* right, int numSamples,
                                            bool freezeDuringRecording) noexcept
{
    if (channelIndex < 0 || channelIndex >= static_cast<int> (channelLayouts.size())
        || left == nullptr || right == nullptr || numSamples <= 0)
        return;

    auto& layout = *channelLayouts[static_cast<size_t> (channelIndex)];

    // §11: two passes over the block, no allocation, no locking. §2.1 wants a
    // peak per channel, their correlation, and the difference in their RMS.
    float peakLeft = 0.0f, peakRight = 0.0f;
    double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right[i];

        peakLeft = std::max (peakLeft, std::abs (l));
        peakRight = std::max (peakRight, std::abs (r));

        sumLL += static_cast<double> (l) * l;
        sumRR += static_cast<double> (r) * r;
        sumLR += static_cast<double> (l) * r;
    }

    const auto toDb = [] (float linear)
    {
        // A floor rather than -inf, so the analyzer's comparisons stay ordered
        // and a digitally silent channel is simply very quiet.
        return linear > 1.0e-10f ? 20.0f * std::log10 (linear) : -200.0f;
    };

    const double blockSeconds = sampleRate > 0.0
                                    ? static_cast<double> (numSamples) / sampleRate
                                    : 0.0;

    layout.analyzer.processBlockEnergies (toDb (peakLeft), toDb (peakRight),
                                          sumLL, sumRR, sumLR, numSamples,
                                          blockSeconds);

    // For an already-collapsed microphone, the side is free to move only until
    // §2.1 has decided and never once a take is running: swapping which input
    // feeds a stem mid-file would put a discontinuity in the recording. A fresh
    // pair may finish observing during a take because both inputs keep their
    // independent routing; its verdict cannot affect shape until the idle
    // Application rebuild (§6.5).
    if (! layout.frozen)
    {
        // Checked before the store, not after. Reversing these leaves a
        // one-block window in which a take that has just started still adopts a
        // new side -- which is exactly the discontinuity the freeze exists to
        // prevent, made rarer and therefore harder to find.
        const bool recordingFreezesSource = freezeDuringRecording
            && activePipeline.load (std::memory_order_acquire) != nullptr;

        // Do not publish a side change into a running stem, but do not
        // permanently freeze the analyzer either. It may finish gathering
        // valid evidence during this take; the first callback after stop can
        // safely apply that answer for the next one.
        if (! recordingFreezesSource)
        {
            // A remembered right-side source survives ordinary silence. Only
            // actual one-sided signal may replace it; a fresh analyzer still
            // gets the historical left default until it hears evidence.
            if (layout.source.load (std::memory_order_relaxed) < 0
                || layout.analyzer.hasMonoSourceEvidence())
                layout.source.store (layout.analyzer.getMonoSourceChannel(),
                                     std::memory_order_relaxed);

            layout.decision.store (static_cast<int> (layout.analyzer.getDecision()),
                                   std::memory_order_release);
            // This release publishes both the decision and its selected source
            // to the message thread. A provisional timeout remains false and
            // can therefore never reach disk or trigger a capture rebuild.
            layout.decisionPersistable.store (layout.analyzer.isDecisionPersistable(),
                                              std::memory_order_release);

            if (layout.analyzer.isDecisionPersistable())
                layout.frozen = true;
        }
    }
}

float CaptureCoordinator::pairMonitorTarget (size_t channel) const noexcept
{
    if (channel >= monitorPairAnalyzer.size() || monitorPairAnalyzer[channel] < 0)
        return 1.0f;

    const auto analyzer = static_cast<size_t> (monitorPairAnalyzer[channel]);

    if (analyzer >= channelLayouts.size())
        return 1.0f;

    const auto& layout = *channelLayouts[analyzer];

    // Only a verdict from three seconds of signal. The sixty-second
    // no-signal Mono is provisional (both sides are quiet, and a quiet
    // two-input interface must still be able to prove Stereo), so the pair
    // stays at half each until a real answer. The acquire pairs with the
    // release that published the decision and its side.
    if (! layout.decisionPersistable.load (std::memory_order_acquire))
        return kUnsettledPairMonitorGain;

    switch (static_cast<ChannelLayoutDecision> (layout.decision.load (std::memory_order_acquire)))
    {
        case ChannelLayoutDecision::Stereo:
            return 1.0f;

        case ChannelLayoutDecision::Mono:
        {
            const int source = layout.source.load (std::memory_order_relaxed) == 1 ? 1 : 0;
            return monitorPairSide[channel] == source ? 1.0f : 0.0f;
        }

        case ChannelLayoutDecision::Pending:
        default:
            return kUnsettledPairMonitorGain;
    }
}

float CaptureCoordinator::getChannelMonitorWeight (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (monitorPairWeight.size()))
        return 1.0f;

    return monitorPairWeight[static_cast<size_t> (index)];
}

int CaptureCoordinator::getChannelLayoutSource (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channelLayouts.size()))
        return -1;

    return channelLayouts[static_cast<size_t> (index)]->source.load (std::memory_order_relaxed);
}

ChannelLayoutDecision CaptureCoordinator::getChannelLayoutDecision (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channelLayouts.size()))
        return ChannelLayoutDecision::Pending;

    return static_cast<ChannelLayoutDecision> (
        channelLayouts[static_cast<size_t> (index)]->decision.load (std::memory_order_acquire));
}

bool CaptureCoordinator::isChannelLayoutDecisionPersistable (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channelLayouts.size()))
        return false;

    return channelLayouts[static_cast<size_t> (index)]->decisionPersistable.load (
        std::memory_order_acquire);
}

bool CaptureCoordinator::pullOutputBlock (float* const* outputs, int numOutputs,
                                          int numSamples) noexcept
{
    // The same hand-off the output callback and the software clock take, so a
    // caller outside those two cannot pull a ring the clock is already pulling.
    if (pulling.exchange (true, std::memory_order_acq_rel))
        return false;

    processOutputBlock (outputs, numOutputs, numSamples);
    pulling.store (false, std::memory_order_release);
    return true;
}

void CaptureCoordinator::processOutputBlock (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const auto callbackStart = std::chrono::steady_clock::now();

    const int channelCount = static_cast<int> (deviceStreams.size());
    if (channelCount <= 0)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs != nullptr && outputs[ch] != nullptr)
                std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);

        return noteCallbackLoad (callbackStart, numSamples);
    }

    // Sized at startMonitoring(); never grow here (§11). Should that invariant
    // ever fail, count the whole callback and silence the output rather than
    // returning with stale headphones and no record that audio was lost.
    if (devicePointers.size() < static_cast<size_t> (channelCount)
        || deviceScratch.size() < static_cast<size_t> (channelCount))
    {
        framesMissedByLayout.fetch_add (static_cast<uint64_t> (numSamples),
                                        std::memory_order_relaxed);

        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs != nullptr && outputs[ch] != nullptr)
                std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);

        return noteCallbackLoad (callbackStart, numSamples);
    }

    // The scratch has bounded, pre-allocated headroom. CoreAudio can legally
    // deliver a callback larger than that bound, so consume it in slices that
    // fit rather than returning and silently losing the entire callback. This
    // stays real-time safe: each slice reuses the same storage and the output
    // offset points at the corresponding part of the caller's buffers.
    const size_t framesPerSlice = deviceScratch.size() / static_cast<size_t> (channelCount);
    int frameOffset = 0;

    while (frameOffset < numSamples)
    {
        const int frames = static_cast<int> (std::min (
            framesPerSlice, static_cast<size_t> (numSamples - frameOffset)));

        // §3.2: every device is pulled onto this callback's timebase here. This
        // is the single point where the independent USB clocks become one
        // frame, and every channel is corrected onto it -- including the one
        // §3.1 names as clock master, whose crystal is no more this clock than
        // any other mic's is. Each channel lands in it as early as its own
        // device allows; the stems are lined up later, by the writer.
        for (int ch = 0; ch < channelCount; ++ch)
        {
            float* destination = deviceScratch.data() + static_cast<size_t> (ch) * frames;
            deviceStreams[static_cast<size_t> (ch)]->pull (destination, frames);
            devicePointers[static_cast<size_t> (ch)] = destination;
        }

        // Each channel is handed on as early as its own device allows; the
        // writer is told how far to hold each back in the stems, as the
        // streams stand after this pull.
        updateRecordingOffsets();

        const bool offsetsFit = recordingOffsets.size() >= static_cast<size_t> (channelCount)
                             && recordingOffsetKinds.size() >= static_cast<size_t> (channelCount);
        mixAndPublish (devicePointers.data(), channelCount, outputs, numOutputs,
                       frames, frameOffset,
                       offsetsFit ? recordingOffsets.data() : nullptr,
                       offsetsFit ? recordingOffsetKinds.data() : nullptr);
        frameOffset += frames;
    }

    noteCallbackLoad (callbackStart, numSamples);
}

int CaptureCoordinator::channelOwnLatencyFrames (size_t index, size_t blockFloor) const noexcept
{
    if (index >= deviceStreams.size() || index >= channelInputLatency.size()
        || channelInputLatency[index] < 0)
        return -1;

    // The block the stream sits at (DeviceInputStream: the cushion
    // plus the block it holds room for), or the floor where that is larger
    // -- the size asked for, before the device has settled on one. The held
    // block rather than the largest the device has ever run at: room held
    // for a size the device has left is given back (at a quiet moment), and
    // from then on the channel is that much sooner in the headphones.
    const auto block = std::min (std::max (deviceStreams[index]->getHeldBlockSamples(), blockFloor),
                                 static_cast<size_t> (DeviceInputStream::kLargestDeviceBlock) * 16);
    return channelInputLatency[index] + static_cast<int> (block);
}

void CaptureCoordinator::updateRecordingOffsets() noexcept
{
    // A sample reaches its ring up to one device block after it was captured,
    // on top of the device's own input latency, and every stream holds the
    // same cushion beyond that. So each channel's place in time is its
    // latency plus its block, and the stems line up when every channel is
    // held back to the largest of those across the rig. The writer does the
    // holding back (StemAligner), from these figures, so the headphones --
    // fed from the same pull -- never wait for it.
    //
    // Worked out on the one thread that pulls every stream, before every
    // block the writer is handed. The rig's reference is each device's
    // settled block (two deliveries at it; a single large delivery may be a
    // driver handing over a backlog, which moves nothing), so the slowest
    // device growing moves every other stem's alignment once its new size
    // settles -- about one of its blocks after its own stream moved -- and a
    // backlog moves none. A channel's own place is where its stream really
    // is, a block it has just moved for included, so when it grows without
    // becoming the slowest its stem is brought forward in the very block its
    // stream moved, while the silence that move put in is still at hand to
    // take back out (StemAligner::setOffset).
    //
    // That block is provisional until its device's next delivery: one large
    // delivery is also what a driver handing over a backlog looks like. So
    // each change is tagged for the writer: provisional while the stream
    // holds a block its device has not confirmed, and refused when that
    // block is taken back (the held block falls straight after being
    // provisional). The writer then puts back what the provisional change
    // took out, where it took it from, and the backlog moves nothing in the
    // stem either. A held block that falls from a settled one is room given
    // back for a block the device has left (DeviceInputStream drops exactly
    // that much at a quiet moment): settled, a move of its own, which the
    // writer answers with that much silence where the stream dropped it.
    // Nothing allocates or locks (§11): the vectors were sized at
    // startMonitoring().
    const size_t count = std::min ({ deviceStreams.size(), recordingOffsets.size(),
                                     recordingOffsetKinds.size(), heldBlockSeen.size() });
    const bool ready = latenciesReady.load (std::memory_order_acquire) && channelInputLatency.size() >= count;
    int reference = 0;

    for (size_t i = 0; i < count; ++i)
        if (ready && channelInputLatency[i] >= 0)
            reference = std::max (reference, channelInputLatency[i]
                                                 + static_cast<int> (deviceStreams[i]->getSettledBlockSamples()));

    // The reference never falls while a take is being written. It only could
    // when the slowest device gives back room it held for a larger block, and
    // its stream then moves sooner by dropping a moment of quiet: that
    // channel's own offset grows by the same amount and its stem stays in
    // line. Had the reference fallen with it, every other stem would have
    // been brought forward too, and the writer takes that out of the newest
    // samples waiting -- audio, cut from every other microphone's track at
    // whatever moment the room was given back. Between takes nothing is
    // being lined up, so the next take starts from the rig as it stands
    // rather than from a block some device ran at long before it.
    if (ready)
    {
        if (activePipeline.load (std::memory_order_acquire) == nullptr)
            stemReferenceFloor = 0;

        reference = std::max (reference, stemReferenceFloor);
        stemReferenceFloor = reference;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const auto& stream = *deviceStreams[i];
        const auto held = stream.getHeldBlockSamples();

        // Refused: the held block falls from a provisional one. A held block
        // that falls from a settled one is room given back for a block the
        // device no longer runs at -- a move of its own, settled.
        const bool wasProvisional = recordingOffsetKinds[i] == StemOffsetKind::provisional;
        recordingOffsetKinds[i] = held > stream.getSettledBlockSamples()  ? StemOffsetKind::provisional
                                : held < heldBlockSeen[i] && wasProvisional ? StemOffsetKind::refused
                                                                            : StemOffsetKind::settled;
        heldBlockSeen[i] = held;

        // A device that did not open is silent; it is not moved.
        recordingOffsets[i] = 0;

        if (ready && channelInputLatency[i] >= 0)
            recordingOffsets[i] = std::max (0, reference - channelInputLatency[i] - static_cast<int> (held));

        if (recordingOffsetView != nullptr)
            recordingOffsetView[i].store (recordingOffsets[i], std::memory_order_relaxed);
    }
}

double CaptureCoordinator::monitorPathMs (double roundTripMs, int bufferSizeFrames,
                                          int outputFrames, int inputFrames) const noexcept
{
    // Zero means nothing is monitoring, and stays zero.
    if (roundTripMs <= 0.0 || sampleRate <= 0.0)
        return roundTripMs;

    const int size = std::max (1, bufferSizeFrames);

    // Every microphone's ring holds this much ahead of the output, so the
    // loop can absorb the drift between the two clocks and a device can hand
    // its audio over a few milliseconds late without a gap
    // (DeviceInputStream's target fill: cushionSamples of the largest pull,
    // which is the output block). A sample waits it out before the output
    // takes it. Leaving it out made the Advanced panel read two blocks
    // quicker than the buffer-ladder notice that had just announced the
    // same figure. The same function the streams size their target with, so
    // the figure the app names cannot drift from what the rings hold.
    const auto cushion = static_cast<int> (DeviceInputStream::cushionSamples (
        static_cast<size_t> (std::max (size, outputFrames)), sampleRate));

    // The round trip counts one input block at the size asked for. A
    // microphone's own path adds its device's input latency, and whatever
    // larger block its device runs at.
    const int extra = std::max (0, inputFrames - size);

    return roundTripMs + 1000.0 * static_cast<double> (cushion + extra) / sampleRate;
}

double CaptureCoordinator::getMonitoringLatencyMs() const noexcept
{
    const int size = std::max (1, bufferSize);
    return monitorPathMs (monitoringLatencyMs, size, monitorOutputFrames, getMonitorInputFrames (size));
}

double CaptureCoordinator::getSlowestMonitoringLatencyMs() const noexcept
{
    const int size = std::max (1, bufferSize);
    return monitorPathMs (monitoringLatencyMs, size, monitorOutputFrames, getSlowestMonitorInputFrames (size));
}

double CaptureCoordinator::getMonitoringLatencyMsAt (int bufferSizeFrames) const noexcept
{
    if (monitoringLatencyMs <= 0.0 || sampleRate <= 0.0)
        return 0.0;

    // The round trip as startMonitoring will work it out from the size the
    // device grants, taken to be the size asked for; what the output device
    // adds after the buffers does not change with them.
    const int size = std::max (1, bufferSizeFrames);
    const double roundTripMs = (2.0 * size + monitorPresentationFrames) / sampleRate * 1000.0;
    return monitorPathMs (roundTripMs, size, size, getMonitorInputFrames (size));
}

double CaptureCoordinator::getSlowestMonitoringLatencyMsAt (int bufferSizeFrames) const noexcept
{
    if (monitoringLatencyMs <= 0.0 || sampleRate <= 0.0)
        return 0.0;

    const int size = std::max (1, bufferSizeFrames);
    const double roundTripMs = (2.0 * size + monitorPresentationFrames) / sampleRate * 1000.0;
    return monitorPathMs (roundTripMs, size, size, getSlowestMonitorInputFrames (size));
}

std::string CaptureCoordinator::getSlowestMonitoringDeviceId() const
{
    int channel = -1;
    monitorInputFrames (std::max (1, bufferSize), true, &channel);

    return channel >= 0 && static_cast<size_t> (channel) < channels.size()
               ? channels[static_cast<size_t> (channel)].deviceId : std::string();
}

int CaptureCoordinator::getMonitorInputFrames (int bufferSizeFrames) const noexcept
{
    return monitorInputFrames (bufferSizeFrames, false, nullptr);
}

int CaptureCoordinator::getSlowestMonitorInputFrames (int bufferSizeFrames) const noexcept
{
    return monitorInputFrames (bufferSizeFrames, true, nullptr);
}

int CaptureCoordinator::monitorInputFrames (int bufferSizeFrames, bool slowest, int* channel) const noexcept
{
    const auto size = static_cast<size_t> (std::max (1, bufferSizeFrames));
    int best = -1, bestChannel = -1;

    // Each device's block the larger of this size and the one it has been
    // running at; each channel's own path, since no channel waits for any
    // other.
    if (latenciesReady.load (std::memory_order_acquire))
        for (size_t i = 0; i < deviceStreams.size() && i < channelInputLatency.size(); ++i)
            if (const int own = channelOwnLatencyFrames (i, size);
                own >= 0 && (best < 0 || (slowest ? own > best : own < best)))
            {
                best = own;
                bestChannel = static_cast<int> (i);
            }

    if (channel != nullptr)
        *channel = bestChannel;

    return best >= 0 ? best : static_cast<int> (size);
}

int CaptureCoordinator::channelIndexForDevice (const std::string& deviceId) const noexcept
{
    for (size_t i = 0; i < channels.size() && i < deviceStreams.size(); ++i)
        if (channels[i].deviceId == deviceId)
            return static_cast<int> (i);

    return -1;
}

int CaptureCoordinator::getDeviceInputLatencyFrames (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0 || ! latenciesReady.load (std::memory_order_acquire)
        || static_cast<size_t> (i) >= channelInputLatency.size())
        return -1;

    return channelInputLatency[static_cast<size_t> (i)];
}

std::optional<int> CaptureCoordinator::getDeviceReportedInputLatencyFrames (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0 || ! latenciesReady.load (std::memory_order_acquire)
        || static_cast<size_t> (i) >= channelReportedLatency.size()
        || static_cast<size_t> (i) >= channelInputLatency.size()
        || channelInputLatency[static_cast<size_t> (i)] < 0)
        return std::nullopt;

    return channelReportedLatency[static_cast<size_t> (i)];
}

int CaptureCoordinator::getDeviceIoBlockFrames (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);
    return i < 0 ? 0 : static_cast<int> (deviceStreams[static_cast<size_t> (i)]->getDeviceBlockSamples());
}

int CaptureCoordinator::getDeviceAlignmentDelayFrames (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0 || getDeviceInputLatencyFrames (deviceId) < 0)
        return 0;

    // Every channel of one device shares its place in time, so the first
    // channel's offset is the device's.
    return getChannelRecordingOffset (i);
}

int CaptureCoordinator::getDeviceAlignmentStartFrames (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0)
        return 0;

    // The writer's own record of it: the offset in force when the channel's
    // first sample reached it (none, for a device that did not open). Until
    // the writer has reached the channel -- it sleeps between chunks, and the
    // take's start-time record is written before it wakes -- the offset the
    // audio thread is handing it now, which is what its stem will open with:
    // the writer's own figure is zero until the first change reaches it, and
    // a record saying "starts at 0, held back by 37" is one no editor can
    // follow.
    if (pipeline != nullptr)
        return pipeline->hasChannelAlignmentStarted (i) ? pipeline->getChannelStartAlignmentOffset (i)
                                                        : getChannelRecordingOffset (i);

    const auto index = static_cast<size_t> (i);
    return index < lastTakeAlignmentStart.size() ? lastTakeAlignmentStart[index] : 0;
}

int CaptureCoordinator::getChannelRecordingOffset (int index) const noexcept
{
    if (index < 0 || static_cast<size_t> (index) >= recordingOffsets.size() || recordingOffsetView == nullptr)
        return 0;

    return recordingOffsetView[static_cast<size_t> (index)].load (std::memory_order_relaxed);
}

uint64_t CaptureCoordinator::getChannelShiftSilenceSamples (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0;

    return deviceStreams[static_cast<size_t> (index)]->getAlignmentSilenceSamples();
}

namespace {

int clampedFrames (uint64_t frames) noexcept
{
    return static_cast<int> (std::min<uint64_t> (frames, static_cast<uint64_t> (std::numeric_limits<int>::max())));
}

} // namespace

int CaptureCoordinator::getDeviceAlignmentSilenceFramesThisTake (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0)
        return 0;

    const auto index = static_cast<size_t> (i);

    if (pipeline != nullptr)
        return clampedFrames (pipeline->getChannelAlignmentSilence (i));

    return index < lastTakeAlignmentSilence.size() ? clampedFrames (lastTakeAlignmentSilence[index]) : 0;
}

int CaptureCoordinator::getDeviceAlignmentDroppedFramesThisTake (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0)
        return 0;

    const auto index = static_cast<size_t> (i);

    if (pipeline != nullptr)
        return clampedFrames (pipeline->getChannelAlignmentDropped (i));

    return index < lastTakeAlignmentDropped.size() ? clampedFrames (lastTakeAlignmentDropped[index]) : 0;
}

uint64_t CaptureCoordinator::channelShiftSilenceThisTake (size_t index) const noexcept
{
    if (pipeline == nullptr)
        return index < lastTakeShiftSilence.size() ? lastTakeShiftSilence[index] : 0;

    if (index >= deviceStreams.size())
        return 0;

    // A stream rebuilt since the take began counts from zero; all of it is
    // then this take's.
    const auto total = deviceStreams[index]->getAlignmentSilenceSamples();
    const auto base = index < shiftSilenceBaselinePerStream.size() ? shiftSilenceBaselinePerStream[index] : 0;
    return total >= base ? total - base : total;
}

int CaptureCoordinator::getDeviceIoShiftFramesThisTake (const std::string& deviceId) const noexcept
{
    const int i = channelIndexForDevice (deviceId);

    if (i < 0)
        return 0;

    const auto index = static_cast<size_t> (i);
    const auto shift = channelShiftSilenceThisTake (index);

    // What the writer took out of this channel's stems is, for the most part,
    // the silence its own stream put in as it moved (StemAligner takes the
    // move's silence first), so the rest of the shift is what stayed.
    const auto dropped = pipeline != nullptr ? pipeline->getChannelAlignmentDropped (i)
                                             : (index < lastTakeAlignmentDropped.size() ? lastTakeAlignmentDropped[index] : 0);

    return clampedFrames (shift > dropped ? shift - dropped : 0);
}

bool CaptureCoordinator::areStemsAligned() const noexcept
{
    return pipeline != nullptr ? pipeline->isAlignmentExact() && ! latencyClampedThisTake
                               : lastTakeStemsAligned;
}

void CaptureCoordinator::processAudioBlock (const float* const* inputs, int numInputs,
                                            float* const* outputs, int numOutputs,
                                            int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    // A clock read, not a syscall, on both shipping platforms -- no allocation,
    // no lock, nothing §11 forbids.
    const auto callbackStart = std::chrono::steady_clock::now();

    // Aggregate path: the OS has already aligned these channels, so they go
    // straight to the mixer without passing through the per-device rings.
    const int channelCount = std::min (numInputs, static_cast<int> (channels.size()));

    // Already lined up by the OS, so the writer leaves every stem where it is.
    mixAndPublish (inputs, channelCount, outputs, numOutputs, numSamples, 0, nullptr, nullptr);
    noteCallbackLoad (callbackStart, numSamples);
}

void CaptureCoordinator::mixAndPublish (const float* const* inputs, int channelCount,
                                        float* const* outputs, int numOutputs,
                                        int numSamples, int outputFrameOffset,
                                        const int* recordingOffsetsForBlock,
                                        const StemOffsetKind* recordingOffsetKindsForBlock) noexcept
{
    if (inputs == nullptr || channelCount <= 0)
        return;

    // §6: recording gets the raw channels. §4 keeps stems at unity and applies
    // trim only to the mix, which the pipeline does itself.
    //
    // The busy count is taken before the load and released after the call, so
    // stopRecording cannot free the pipeline between the two.
    pipelineUsers.fetch_add (1, std::memory_order_seq_cst);

    if (auto* activeWriter = activePipeline.load (std::memory_order_seq_cst))
    {
        // Handed over even when the count does not match the take's channel
        // list. This used to be guarded by channelCount == channels.size(),
        // which silently skipped the write: a block that could not be recorded
        // simply never reached the pipeline, so framesDropped stayed at zero
        // and the take looked healthy while audio went missing. §0.1 makes
        // unreported loss the one unacceptable failure, and the pipeline
        // rejects a mismatched block anyway -- what it does with it now is
        // count it.
        activeWriter->pushBlock (inputs, channelCount, numSamples, recordingOffsetsForBlock,
                                 recordingOffsetKindsForBlock);
    }

    pipelineUsers.fetch_sub (1, std::memory_order_release);

    // The loudest sample that ARRIVED, measured here -- before the writer gets
    // a say, and on the same buffer the meters are about to read.
    //
    // The writer already tracks what it managed to WRITE. The pair is what
    // makes a silent take diagnosable: audio arriving and nothing written can
    // only be the writer refusing blocks, which is a fault in this app, and
    // saying so is the difference between a user checking their microphone for
    // an hour and knowing at a glance that it is not their rig.
    {
        float arrived = 0.0f;

        for (int ch = 0; ch < channelCount; ++ch)
        {
            if (inputs[ch] == nullptr)
                continue;

            for (int i = 0; i < numSamples; ++i)
            {
                const float magnitude = inputs[ch][i] < 0.0f ? -inputs[ch][i] : inputs[ch][i];

                if (magnitude > arrived)
                    arrived = magnitude;
            }
        }

        if (arrived > peakArrived.load (std::memory_order_relaxed))
            peakArrived.store (arrived, std::memory_order_relaxed);
    }

    // §14.4: measured here because this is the one place every channel is in
    // the same frame block -- the per-device path has just pulled them onto
    // one timebase, and the aggregate path is handed them that way. On the
    // per-device path each is as early as its own device allows rather than
    // lined up (that is the writer's job); microphones of a kind, which is
    // what this advice is about, hand their audio over within a few samples
    // of each other.
    measurePolarPattern (inputs, channelCount, numSamples);

    // §8.2: the audio thread does only max-abs per block into the meter.
    for (int ch = 0; ch < channelCount; ++ch)
        if (inputs[ch] != nullptr && ch < static_cast<int> (channelMeters.size()))
            channelMeters[static_cast<size_t> (ch)]->processAudioBlock (inputs[ch], numSamples);

    if (outputs == nullptr || numOutputs <= 0)
    {
        // No headphone mix this cycle, so the feedback analysis has a hole.
        feedbackGuard.noteGap();
        return;
    }

    // §5.1: one mix, containing every microphone including the listener's own,
    // summed at unity with no attenuation for channel count.
    // Sized at startMonitoring(); never grow here (§11). Should either guard
    // ever trip, the headphones get silence rather than whatever the output
    // buffer last held: CoreAudioBackend hands an interleaved device its own
    // repack scratch, so an unwritten block replays the previous one -- a
    // buzz at the callback rate for as long as the condition lasts.
    if (mixScratch.size() < static_cast<size_t> (numSamples)
        || busScratch.size() < static_cast<size_t> (numSamples)
        || alarmScratch.size() < static_cast<size_t> (numSamples)
        || static_cast<int> (trimFrame.size()) < channelCount
        || static_cast<int> (trimGains.size()) < channelCount
        || static_cast<int> (monitorPairWeight.size()) < channelCount
        || static_cast<int> (monitorPairTarget.size()) < channelCount)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                std::fill (outputs[ch] + outputFrameOffset,
                           outputs[ch] + outputFrameOffset + numSamples, 0.0f);
        return;
    }

    // trimFrame is sized to the channel list, but a block can carry fewer
    // channels than that -- processAudioBlock takes min(numInputs, channels).
    // MonitorBus::processSample sums the whole vector, so anything past
    // channelCount that this block does not overwrite is a sample from an
    // earlier, wider block, added to every sample of the mix from here on: a
    // fixed offset in the listener's headphones from a channel that is no
    // longer arriving. Zeroing the tail is a handful of stores and costs
    // nothing when there is no tail (§11).
    for (size_t ch = static_cast<size_t> (channelCount); ch < trimFrame.size(); ++ch)
        trimFrame[ch] = 0.0f;

    // Where each analysed pair's headphone weight is headed this block (see
    // startMonitoring). Read once per block: a few atomic loads per pair.
    bool pairWeightsMoving = false;
    for (int ch = 0; ch < channelCount; ++ch)
    {
        const auto i = static_cast<size_t> (ch);
        monitorPairTarget[i] = pairMonitorTarget (i);
        pairWeightsMoving = pairWeightsMoving || monitorPairWeight[i] != monitorPairTarget[i];
    }

    // A verdict moves the weights over 10 ms rather than in one step: a pair
    // going from half each to both at unity is a 6 dB jump, a click if it
    // lands mid-waveform. A duplicated pair crossfading to one side stays the
    // same level throughout.
    const float pairWeightStep = sampleRate > 0.0
                                   ? static_cast<float> (1.0 / (kPairWeightRampSeconds * sampleRate))
                                   : 1.0f;

    for (int s = 0; s < numSamples; ++s)
    {
        // §4: trim is applied to the mix only. The stems the pipeline already
        // received above stay at unity, which is what makes trim a monitoring
        // decision the recording cannot be damaged by. So is the pair weight.
        for (int ch = 0; ch < channelCount; ++ch)
        {
            const auto i = static_cast<size_t> (ch);
            float weight = monitorPairWeight[i];

            if (pairWeightsMoving && weight != monitorPairTarget[i])
            {
                const float target = monitorPairTarget[i];
                weight = weight < target ? std::min (target, weight + pairWeightStep)
                                         : std::max (target, weight - pairWeightStep);
                monitorPairWeight[i] = weight;
            }

            trimFrame[i] = (inputs[ch] != nullptr) ? inputs[ch][s] * trimGains[i] * weight : 0.0f;
        }

        // §5.1/§5.5: the bus, then the listener's volume, then the safety
        // limiter on what reaches the ears -- all inside processSample. The
        // volume used to be applied here, after the limiter, which left the
        // -3 dBFS ceiling acting on the full-level bus: ordinary speech from
        // a few microphones was squared off, and no volume setting helped.
        float busBeforeVolume = 0.0f;
        mixScratch[static_cast<size_t> (s)] = monitorBus.processSample (trimFrame, &busBeforeVolume);
        busScratch[static_cast<size_t> (s)] = busBeforeVolume;
    }

    // §5.5 feedback protection: the bus as summed, before the listener's
    // volume, the limiter and the app's own tones (pure tones, which a
    // narrowband detector would take for a howl), so the -60 dBFS floor and
    // the 10 dB rise mean the same at every volume. A copy into a
    // preallocated ring; the analysis runs on the guard's own thread (§11).
    feedbackGuard.push (busScratch.data(), numSamples);

    // §8.1: the mix bar shows that same bus, before the volume. It used to
    // read the signal after it, so at the default volume a bus being
    // squared off at -3 dBFS read about -21 dBFS -- plenty of headroom, to
    // look at -- and its CLIP, at -0.1 dBFS, could never be reached under a
    // -3 dBFS ceiling. Its CLIP now latches from the safety limiter: each
    // time it starts turning the headphones down, the bar says so, and the
    // user can see why the mix sounds squashed and turn the trims down.
    mixMeter.processLevelOnly (busScratch.data(), numSamples);

    if (const auto engagements = monitorBus.getLimiterEngagementCount();
        engagements != meteredLimiterEngagements)
    {
        const auto fresh = engagements - meteredLimiterEngagements;
        mixMeter.noteClipEvents (static_cast<int> (std::min<uint64_t> (fresh, 1000000)));
        meteredLimiterEngagements = engagements;
    }

    // The app's own sounds go in after the meter, so the mix bar shows the
    // room and not the siren, and after the mute: a fault alarm has to reach
    // the headphones however the monitor is set. Clamped, because a siren over
    // a mix at the ceiling would otherwise clip in the driver.
    //
    // Rendered on their own first, and only once: render() moves the tone on,
    // so a second call would play the next stretch of it instead. A jack
    // switched off from the mix gets these alone. It used to get zeros, siren
    // included, so with every jack switched off the chirps and the fault siren
    // played nowhere -- the output stream was still open, so nothing sent them
    // anywhere else either.
    std::fill (alarmScratch.begin(), alarmScratch.begin() + numSamples, 0.0f);
    alarm.render (alarmScratch.data(), numSamples, sampleRate);
    for (int s = 0; s < numSamples; ++s)
    {
        const auto i = static_cast<size_t> (s);
        mixScratch[i] = std::clamp (mixScratch[i] + alarmScratch[i], -1.0f, 1.0f);
        alarmScratch[i] = std::clamp (alarmScratch[i], -1.0f, 1.0f);
    }

    // §5.2: the same mix to every output channel -- no per-listener variation.
    // A channel can be switched off (a person who wants their headphones
    // quiet), never given a different mix; it still hears the alarm.
    for (int ch = 0; ch < numOutputs; ++ch)
    {
        if (outputs[ch] == nullptr)
            continue;

        const bool hears = ch >= kMaxOutputChannelGains
                        || outputChannelGains[static_cast<size_t> (ch)].load (std::memory_order_relaxed) > 0.0f;

        const auto& source = hears ? mixScratch : alarmScratch;
        std::copy (source.begin(), source.begin() + numSamples, outputs[ch] + outputFrameOffset);
    }
}

void CaptureCoordinator::measurePolarPattern (const float* const* inputs, int channelCount,
                                              int numSamples) noexcept
{
    // §14.4's rule needs a third channel to check. With two microphones there
    // is no "uninvolved" one, and a correlated pair is just two people at one
    // table -- reporting anything here would be guessing.
    if (inputs == nullptr || channelCount < 3 || numSamples <= 0)
    {
        polarCorrelation.store (0.0f, std::memory_order_relaxed);
        return;
    }

    constexpr int kMaxChannels = 8; // §1's ceiling
    const int count = std::min (channelCount, kMaxChannels);

    double energy[kMaxChannels] = {};
    float peakDb[kMaxChannels] = {};

    for (int ch = 0; ch < count; ++ch)
    {
        const float* samples = inputs[ch];

        if (samples == nullptr)
        {
            // Every other way out of this function clears the correlation. This
            // one returned with it untouched, so the last verdict measured kept
            // being reported after the measurement had stopped -- and §14.4's
            // advice would keep firing from it.
            polarCorrelation.store (0.0f, std::memory_order_relaxed);
            return;
        }

        double sum = 0.0;
        float peak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float v = samples[i];
            sum += static_cast<double> (v) * v;
            peak = std::max (peak, std::abs (v));
        }

        energy[ch] = sum;
        peakDb[ch] = peak > 1.0e-10f ? 20.0f * std::log10 (peak) : -200.0f;
    }

    // Strongest correlated pair in the rig. At §1's eight-microphone ceiling
    // that is 28 dot products over the block -- a few thousand multiply-adds,
    // which is nothing beside the resampling already done here, and it costs
    // no allocation (§11).
    float bestCorrelation = 0.0f;
    int bestA = -1, bestB = -1;

    for (int a = 0; a < count; ++a)
    {
        for (int b = a + 1; b < count; ++b)
        {
            const double denominator = std::sqrt (energy[a]) * std::sqrt (energy[b]);

            if (denominator <= 1.0e-12)
                continue;

            double cross = 0.0;
            for (int i = 0; i < numSamples; ++i)
                cross += static_cast<double> (inputs[a][i]) * inputs[b][i];

            const float correlation = static_cast<float> (std::abs (cross) / denominator);

            if (correlation > bestCorrelation)
            {
                bestCorrelation = correlation;
                bestA = a;
                bestB = b;
            }
        }
    }

    if (bestA < 0)
    {
        polarCorrelation.store (0.0f, std::memory_order_relaxed);
        return;
    }

    // The quietest channel outside the pair. §14.4 reads a third microphone
    // hearing nothing as evidence that what the pair share is the room rather
    // than someone speaking across all three.
    float quietestOtherDb = 0.0f;
    bool haveOther = false;

    for (int ch = 0; ch < count; ++ch)
    {
        if (ch == bestA || ch == bestB)
            continue;

        if (! haveOther || peakDb[ch] < quietestOtherDb)
        {
            quietestOtherDb = peakDb[ch];
            haveOther = true;
        }
    }

    if (! haveOther)
    {
        polarCorrelation.store (0.0f, std::memory_order_relaxed);
        return;
    }

    // Instant attack, slow release: a loud block on the third channel has to
    // survive until the 2 Hz UI tick looks, or the evidence against a false
    // trigger is exactly what gets missed.
    const double blockSeconds = sampleRate > 0.0
                                    ? static_cast<double> (numSamples) / sampleRate
                                    : 0.0;
    const auto decayDb = static_cast<float> (60.0 * blockSeconds); // ~60 dB/second

    polarThirdPeakHeldDb = quietestOtherDb > polarThirdPeakHeldDb
                               ? quietestOtherDb
                               : std::max (quietestOtherDb, polarThirdPeakHeldDb - decayDb);

    polarCorrelation.store (bestCorrelation, std::memory_order_relaxed);
    polarThirdPeakDb.store (polarThirdPeakHeldDb, std::memory_order_relaxed);
}

double CaptureCoordinator::getIntegratedLufs() const
{
    return pipeline != nullptr ? pipeline->getIntegratedLufs() : lastTakeLufs;
}

double CaptureCoordinator::getTruePeakDbtp() const
{
    return pipeline != nullptr ? pipeline->getTruePeakDbtp() : lastTakeTruePeakDbtp;
}

int CaptureCoordinator::getLoudnessBlockCount() const
{
    return pipeline != nullptr ? pipeline->getLoudnessBlockCount() : lastTakeLoudnessBlocks;
}

void CaptureCoordinator::setMasterChannel (int index) noexcept
{
    // §3.1: exactly one reference. Nothing about the capture path changes here
    // -- every channel is corrected onto the output clock either way (§3.2) --
    // so this only moves which channel §3.3's drift figures are measured from,
    // and which device Application publishes as the aggregate's clock source.
    // That makes it safe to call mid-take, unlike a change to the channel set.
    masterChannel = (index >= 0 && index < static_cast<int> (deviceStreams.size())) ? index : -1;
}

double CaptureCoordinator::getMasterDriftPpm() const noexcept
{
    if (masterChannel < 0 || masterChannel >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    return deviceStreams[static_cast<size_t> (masterChannel)]->getDriftPpm();
}

void CaptureCoordinator::tickDriftReporting (double elapsedSeconds) noexcept
{
    // The master's measured clock, once it has one; §3.3's flag is judged
    // against the master, and before the master is measured nothing is
    // flagged (each stream withholds its flag until its own measurement
    // exists, and the master's is the reference for all of them).
    double reference = 0.0;

    if (masterChannel >= 0 && masterChannel < static_cast<int> (deviceStreams.size())
        && deviceStreams[static_cast<size_t> (masterChannel)]->hasDriftMeasurement())
        reference = deviceStreams[static_cast<size_t> (masterChannel)]->getMeasuredDriftPpm();

    for (auto& stream : deviceStreams)
        stream->tickDriftReporting (elapsedSeconds, reference);
}

double CaptureCoordinator::getChannelMeasuredDriftPpm (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    double reference = 0.0;

    if (masterChannel >= 0 && masterChannel < static_cast<int> (deviceStreams.size())
        && deviceStreams[static_cast<size_t> (masterChannel)]->hasDriftMeasurement())
        reference = deviceStreams[static_cast<size_t> (masterChannel)]->getMeasuredDriftPpm();

    return deviceStreams[static_cast<size_t> (index)]->getMeasuredDriftPpm() - reference;
}

bool CaptureCoordinator::hasChannelDriftMeasurement (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return false;

    return deviceStreams[static_cast<size_t> (index)]->hasDriftMeasurement();
}

double CaptureCoordinator::getChannelMeasurementSeconds (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    return deviceStreams[static_cast<size_t> (index)]->getMeasurementSeconds();
}

uint64_t CaptureCoordinator::getRingLossEvents() const noexcept
{
    uint64_t total = 0;

    for (const auto& stream : deviceStreams)
        total += stream->getLossEvents();

    return total;
}

double CaptureCoordinator::getChannelDriftPpm (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    // §3.3: "positive means this device runs fast relative to the master". Each
    // stream's own figure is its correction against the output clock, and that
    // clock's skew is common to all of them, so the difference is exactly the
    // device-against-master number §3.3 asks for -- and the master reports zero
    // against itself by construction.
    return deviceStreams[static_cast<size_t> (index)]->getDriftPpm() - getMasterDriftPpm();
}

double CaptureCoordinator::getChannelFillFraction (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    return deviceStreams[static_cast<size_t> (index)]->getFillFraction();
}

double CaptureCoordinator::getChannelRawDriftPpm (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    return deviceStreams[static_cast<size_t> (index)]->getDriftPpm();
}

uint64_t CaptureCoordinator::getChannelOverrunSamples (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0;

    return deviceStreams[static_cast<size_t> (index)]->getOverrunSamples();
}

bool CaptureCoordinator::hasSustainedExcessDrift (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return false;

    return deviceStreams[static_cast<size_t> (index)]->hasSustainedExcessDrift();
}

uint64_t CaptureCoordinator::getUnderrunSamples (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0;

    return deviceStreams[static_cast<size_t> (index)]->getUnderrunSamples();
}

void CaptureCoordinator::noteCallbackLoad (std::chrono::steady_clock::time_point start,
                                           int numSamples) noexcept
{
    if (sampleRate <= 0.0)
        return;

    const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const auto available = static_cast<double> (numSamples) / sampleRate;

    {
        const auto us = static_cast<uint64_t> (elapsed * 1.0e6);
        auto seen = clockMaxPullUs.load (std::memory_order_relaxed);
        while (us > seen && ! clockMaxPullUs.compare_exchange_weak (seen, us, std::memory_order_relaxed)) {}
    }

    if (available <= 0.0)
        return;

    // Smoothed a little: a single long callback is normal, a sustained high
    // average is what §6.6 warns about.
    const auto instant = elapsed / available;
    const auto previous = callbackLoad.load (std::memory_order_relaxed);

    callbackLoad.store (previous + 0.05 * (instant - previous), std::memory_order_relaxed);
}

} // namespace mma
