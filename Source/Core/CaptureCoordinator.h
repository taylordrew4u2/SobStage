#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../Platform/IAudioBackend.h"
#include "MonitorBus.h"
#include "FeedbackDetector.h"
#include "AlarmTone.h"
#include "DeviceInputStream.h"
#include "ChannelLayoutAnalyzer.h"
#include "Metering.h"
#include "WritePipeline.h"

namespace mma {

struct CaptureChannel
{
    std::string deviceId;
    std::string displayName;
    std::string fileName; // §6.2 sanitized, e.g. "01_Yeti-Kitchen"
    float trimDb = 0.0f;

    /// §2.3: the depth this channel's stem is written at, chosen from its own
    /// device's capability. Zero means "use the take's depth".
    int bitDepth = 0;

    /// Which input of that device this channel takes.
    ///
    /// One device is not one microphone. An audio interface with four mics
    /// plugged into it is a single device presenting four inputs, and the app
    /// used to take exactly one channel from any device -- §2.1's stereo
    /// collapse, applied to an interface, silently discarded every input but
    /// one. Somebody recording two people through one interface got one of
    /// them, and if their microphone happened to be on the discarded input,
    /// they got silence.
    ///
    /// §2.1 already said so: collapse to mono when a side is silent or
    /// duplicated, "otherwise record true stereo". The otherwise was never
    /// implemented.
    int deviceChannel = 0;

    /// A persisted §2.1 verdict that this is one stereo-presenting USB mic,
    /// not a multi-input interface. Only this explicit evidence permits the
    /// coordinator to inspect both physical inputs for one take channel.
    bool collapseStereoPair = false;

    /// This is input 0 of a fresh, fully selected two-channel device whose
    /// layout has no persisted verdict yet. The callback may observe both
    /// inputs for §2.1, but it must continue routing each physical input to its
    /// own channel until Application persists the verdict and rebuilds while
    /// idle.
    bool analyzeStereoPair = false;

    /// Persisted side for a collapsed microphone, so a take started before
    /// this connection's analyzer hears signal still records the live side.
    int monoSourceChannel = 0;
};

/// Opens the audio streams and routes their callbacks. This is the piece that
/// makes the app actually hear and actually record: without it every subsystem
/// exists and nothing reaches a device.
///
/// It takes IAudioBackend by reference rather than constructing one, so the
/// whole path can be driven by a fake backend in tests. The platform backends'
/// own OS calls still need real hardware, but the wiring does not.
class CaptureCoordinator
{
public:
    CaptureCoordinator (IAudioBackend& backend, double sampleRate, int bufferSizeSamples);
    ~CaptureCoordinator();

    /// §5.1: monitoring is live from launch, independent of record state, so
    /// this opens one input stream per microphone plus the single output stream
    /// §5.2 permits. If the output cannot provide §5.4 exclusive mode, it opens
    /// the inputs under the software clock and returns true with a visible
    /// warning; false is reserved for a recording-input failure.
    bool startMonitoring (const std::vector<CaptureChannel>& channels,
                          const std::string& outputDeviceId);

    /// Which take channel each of a device's inputs belongs to.
    std::vector<std::pair<int, int>> routingFor (const std::vector<size_t>& channelIndices) const;

    /// Hands one device's inputs to the take channels that asked for them.
    ///
    /// Shared by the ordinary microphone streams and by the output stream on a
    /// duplex mixer, so a rig where the microphones and the headphones are the
    /// same box routes audio through exactly the same code as one where they
    /// are not.
    /// `fromInputStream` says whether this callback is a microphone delivering
    /// audio, or the duplex output callback that also carries the input half.
    ///
    /// It decides what "no inputs at all" means, and the two answers are
    /// opposite. On an input stream it is a device that delivered nothing --
    /// every planned channel lost. On the output callback it is an ordinary
    /// playback-only cycle, which loses nothing: counting those reported
    /// millions of dropped frames on a take that recorded perfectly, and a
    /// loud false alarm is worse than the silence it replaced.
    void fanOutDeviceInputs (const std::vector<std::pair<int, int>>& routing,
                             const float* const* inputs, int numInputs,
                             int numSamples, bool fromInputStream) noexcept;

    void stopMonitoring();

    bool isMonitoring() const noexcept { return monitoring; }
    const std::string& getMonitorProblem() const noexcept { return monitorProblem; }

    /// §5.4: round-trip monitoring latency for the open monitor stream, or zero
    /// when nothing is monitoring.
    ///
    /// Every backend works this out in checkExclusiveModeCapability and this
    /// class used to drop it on the floor, so Application::measuredLatencyMs
    /// was never assigned by anything: the Advanced panel reported "0.0 ms" and
    /// every take's session.json recorded 0.0 for good. Zero is not a small
    /// latency, it is an impossible one -- and this is the number someone
    /// singing to a click reads to decide whether they can work through the
    /// headphones at all.
    ///
    /// The headphones are never held back to line the rig up: every
    /// microphone reaches them as early as its own device allows, and the
    /// stems are lined up afterwards, on the writer thread (StemAligner). So
    /// a device that reports more input latency, or runs at a larger IO block
    /// than the rest, delays its own channel and no one else's. This figure
    /// is the quickest microphone's own path -- its device's input latency,
    /// and the IO block it runs at where that is larger than the one asked
    /// for, then the cushion its ring holds against the drift between its
    /// clock and the output's (two pulls), the output block and what the
    /// output device adds -- which on a rig of like devices is everyone's. A
    /// slower device's own path is getSlowestMonitoringLatencyMs().
    double getMonitoringLatencyMs() const noexcept;

    /// The slowest open microphone's own path through the headphones, on the
    /// same terms: a device at a larger IO block, or reporting more input
    /// latency, than the rest is that much later in the headphones -- in its
    /// own channel only. The same as getMonitoringLatencyMs() on a rig of
    /// like devices. §5.4's ceiling applies to it too: a figure that is only
    /// ever the quickest path can read 3 ms while one person hears
    /// themselves 45 ms late, so this is shown and recorded beside it.
    double getSlowestMonitoringLatencyMs() const noexcept;

    /// Which device that is, by id; empty when nothing is monitoring.
    std::string getSlowestMonitoringDeviceId() const;

    /// The slowest open input's own latency, in frames: what the stems are
    /// lined up to (with that device's IO block). The headphones do not wait
    /// for it. Zero on a backend that does not report input latency.
    int getAlignedInputLatencyFrames() const noexcept { return alignedInputLatencyFrames; }

    /// §5.4's two headphone figures as they will be once the rig has been
    /// rebuilt at `bufferSizeFrames` with the output granted that size: what
    /// the buffer-ladder notice says before the step, on exactly the terms
    /// getMonitoringLatencyMs() and getSlowestMonitoringLatencyMs() give them
    /// after it. Zero when nothing is monitoring. Message thread.
    double getMonitoringLatencyMsAt (int bufferSizeFrames) const noexcept;
    double getSlowestMonitoringLatencyMsAt (int bufferSizeFrames) const noexcept;

    /// The quickest microphone's own input side were the rig running at
    /// `bufferSizeFrames`: its device's input latency plus its IO block, the
    /// block taken as the larger of that size and the one the device has been
    /// seen running at -- a device that refused the size asked for, or that
    /// another app holds at its own, runs there whatever this app asks next.
    /// That size alone when no device has opened. For saying what a buffer
    /// change will cost the headphones before the rig is rebuilt at it, on
    /// the same terms as getMonitoringLatencyMs(). Message thread.
    int getMonitorInputFrames (int bufferSizeFrames) const noexcept;

    /// The same for the slowest microphone's own input side.
    int getSlowestMonitorInputFrames (int bufferSizeFrames) const noexcept;

    /// For the take's record (session.json), per device: the input latency
    /// the backend reported (-1 when the device did not open or monitoring is
    /// not up), the largest IO block it has delivered, and how far its
    /// channels are held back in the stems (not the headphones) so they line
    /// up with the slowest device -- now, which at the end of a take is the
    /// offset its stems end with. Message thread; reads atomics only.
    int getDeviceInputLatencyFrames (const std::string& deviceId) const noexcept;
    int getDeviceIoBlockFrames (const std::string& deviceId) const noexcept;

    /// What the driver itself reported as the device's input latency, before
    /// it was bounded: a figure past half a second (24000 frames), or below
    /// zero, is not believed, the stems are lined up by the bound, and
    /// areStemsAligned() is false for the take. Nothing when the device did
    /// not open or monitoring is not up.
    std::optional<int> getDeviceReportedInputLatencyFrames (const std::string& deviceId) const noexcept;
    int getDeviceAlignmentDelayFrames (const std::string& deviceId) const noexcept;

    /// The offset the device's stems started the current (or just finished)
    /// take with: the silence that opens each of them. It differs from
    /// getDeviceAlignmentDelayFrames() when an IO block grew mid-take; the
    /// silence and cuts in between are the two counts below. Before the
    /// writer has reached the device's channels, the offset they are being
    /// handed over with now -- what the stems will open with -- so a record
    /// written at the take's start agrees with the delay beside it.
    int getDeviceAlignmentStartFrames (const std::string& deviceId) const noexcept;

    /// The writer's mid-take changes to this device's alignment, since the
    /// current take began (or in the take just finished): silence written
    /// into its stems to hold it back further when another device's IO block
    /// grew, and samples taken out to bring it forward when its own did --
    /// for the most part the silence its own stream put in when it moved
    /// (DeviceInputStream moves once, by the growth). Not lost audio either
    /// way, and in no underrun figure, but a gap or a cut in the file, so the
    /// take's record says where the time went.
    int getDeviceAlignmentSilenceFramesThisTake (const std::string& deviceId) const noexcept;
    int getDeviceAlignmentDroppedFramesThisTake (const std::string& deviceId) const noexcept;

    /// Silence this device's own stream left in its stems during the current
    /// (or just finished) take. When its IO block grew by less than its
    /// ring's cushion, the stream moved later by writing that much silence
    /// (DeviceInputStream::getAlignmentSilenceSamples): no audio was lost, so
    /// it is in no underrun figure, but it is a gap in the stems all the
    /// same. The writer takes it back out when the device is not the slowest
    /// (alignmentDropped); what this counts is what it could not -- the
    /// device was, or became, the slowest -- and so left where it fell, at
    /// the same frame as the silence every other stem was given for it.
    int getDeviceIoShiftFramesThisTake (const std::string& deviceId) const noexcept;

    /// Whether the current (or just finished) take's stems were lined up
    /// exactly as asked: false when an offset had to be clamped or a change
    /// landed late (WritePipeline::isAlignmentExact), when a device's
    /// reported input latency was past belief and had to be bounded (see
    /// getDeviceReportedInputLatencyFrames), or when no take has recorded. The take's record says so, with each device's offset, so
    /// whatever the writer could not do can be done in an editor.
    bool areStemsAligned() const noexcept;

    /// Harness diagnostics: how far channel `index` is held back in its stem
    /// now, as the audio thread last handed it to the writer.
    int getChannelRecordingOffset (int index) const noexcept;

    /// Harness diagnostics: the silence channel `index`'s own stream wrote to
    /// move later when its device's IO block grew by less than its cushion
    /// (DeviceInputStream::getAlignmentSilenceSamples), since monitoring
    /// began. With getUnderrunSamples (index) it says how far the stream
    /// moved, and that it moved once.
    uint64_t getChannelShiftSilenceSamples (int index) const noexcept;

    /// Devices that refused to open when monitoring started, by id.
    ///
    /// Their channels are live-false and write silence. §0.1: the take's record
    /// has to be able to say WHY a stem is silent, and "the microphone never
    /// opened" is a different answer from "it was unplugged part way through".
    const std::vector<std::string>& getDevicesThatFailedToOpen() const noexcept
    {
        return devicesThatFailedToOpen;
    }

    /// §6: begins writing. Monitoring continues untouched -- §5.1 makes the two
    /// independent, and §6.1 keeps a monitor mute from silencing the recording.
    /// mirrorFolder is §6.3's local copy; empty means card-only.
    bool startRecording (const std::string& sessionFolder, int bitDepth,
                         const std::string& originTimestamp,
                         const std::string& mirrorFolder = {});

    /// §6.3: the internal drive ran low mid-take. Stops the copy and keeps the
    /// card write going.
    void stopMirroring();
    bool isMirroring() const noexcept { return pipeline != nullptr && pipeline->isMirroring(); }
    void stopRecording();
    bool isRecording() const noexcept { return pipeline != nullptr && pipeline->isRunning(); }

    /// How long startRecording() waits for the take's files to open, and
    /// stopRecording() for the writer to drain and close them. Both touch the
    /// card, and a card pulled or wedged at that instant can block a file call
    /// forever; past the deadline the pipeline is abandoned to its own
    /// detached worker (which releases it whenever the call returns) and the
    /// take is reported rather than the calling thread freezing.
    void setFilesystemDeadline (std::chrono::milliseconds limit) noexcept { filesystemDeadline = limit; }

    /// True when the last stopRecording() gave up waiting on the card.
    bool didLastStopTimeOut() const noexcept { return lastStopTimedOut; }

    /// Runs on the worker just before the pipeline's start() or stop(). Tests
    /// only: it stands in for a card that stops answering.
    void setFilesystemStallForTesting (std::function<void()> stall) { filesystemStallForTesting = std::move (stall); }
    /// Handed to each take's writer: runs after every chunk it writes.
    void setWriterChunkHookForTesting (std::function<void()> hook) { writerChunkHookForTesting = std::move (hook); }

    /// §6.5: an unplugged mic keeps its channel and writes silence. Applies
    /// to EVERY channel the device contributes: an interface with four
    /// people on it goes silent as four channels, not one.
    void setChannelLive (const std::string& deviceId, bool live);

    /// This device's stream handed over audio within `within`. A mic whose
    /// audio is still arriving is plainly still plugged in, whatever one
    /// device-list pass said about it.
    bool isDeviceDelivering (const std::string& deviceId,
                             std::chrono::milliseconds within = std::chrono::milliseconds (150)) const;
    bool isChannelLive (int index) const noexcept;

    /// True while the output device that should be clocking the rig has
    /// stopped calling back and the software clock is pulling instead. The
    /// take carries on; the headphones are silent until the output returns.
    bool isOutputClockLost() const noexcept { return outputClockLost.load (std::memory_order_relaxed); }

    /// Whether the rig has an output stream that is actually taking audio:
    /// false without one, when the software clock drives everything from the
    /// start, and false while an opened one has stopped calling back
    /// (isOutputClockLost). Anything mixed into the headphones then reaches
    /// nobody -- the software clock pulls with no buffer to fill -- so the
    /// app's own sounds must go another way until the output returns.
    bool hasOutputStream() const noexcept
    {
        return outputStreamOpen && ! isOutputClockLost() && ! isOutputAbandoned() && ! isOutputOffRate();
    }

    /// §5.3: the platform has said the open output's device changed its
    /// sample rate, and has not yet read it back at the rig's
    /// (IAudioBackend::isOutputOffRate). Its callback pulls nothing and the
    /// software clock pulls instead, from that word alone, before the app
    /// has decided anything; abandonOutputClock() then makes it for good.
    bool isOutputOffRate() const noexcept { return outputStreamOpen && backend.isOutputOffRate(); }

    /// §5.3: stops using the open output as the rig's clock, for good, until
    /// the next startMonitoring(). Its callback fills silence and pulls
    /// nothing, and the software clock pulls every microphone instead -- so a
    /// headphone output that another app has switched to a different sample
    /// rate (and which would pull the rings at that rate) cannot cost the take
    /// any audio. `why` becomes the monitor problem, ahead of any already
    /// there. Message thread.
    ///
    /// Refused (false) when there is no output stream, when the output also
    /// carries take channels (a duplex mixer: silencing its callback would
    /// silence those microphones), or when no software clock is running to
    /// take over.
    bool abandonOutputClock (const std::string& why);
    bool isOutputAbandoned() const noexcept { return outputAbandoned.load (std::memory_order_acquire); }

    /// The open output stream also carries take channels: a mixer that is
    /// both the headphones and some of the microphones, opened once.
    bool outputCarriesTakeChannels() const noexcept { return outputCarriesChannels; }

    /// The device the open output stream was opened on; empty without one
    /// (none asked for, or it refused and the rig went input-only).
    const std::string& getOutputDeviceId() const noexcept { return openOutputDeviceId; }

    /// The open output stream is on `deviceId`. Mid-take the output the user
    /// has chosen can move while the stream stays where it is until Stop, so
    /// anything that depends on what is playing through a device -- its
    /// per-jack gains, or whether it can be destroyed -- asks this, not the
    /// choice.
    bool isOutputOpenOn (const std::string& deviceId) const noexcept
    {
        return ! deviceId.empty() && openOutputDeviceId == deviceId;
    }

    /// The per-jack headphone gains of one output device's channels, per
    /// output channel: 1 sends the mix there, 0 keeps the mix off it. Lets the
    /// combined device feed every microphone's headphone jack while each
    /// person can still be switched off. A switched-off channel still gets
    /// the app's own sounds (getAlarm(): the start and stop chirps and the
    /// fault siren), so turning every jack off cannot silence an alarm.
    /// Channels past the list get the mix.
    ///
    /// Kept for that device and laid on the output stream only while the
    /// stream is open on it (isOutputOpenOn): from the first callback of a
    /// stream startMonitoring() opens on it, at once if it is open already.
    /// A stream on any other device, or none, gets the mix on every channel.
    /// Which device is open is this class's to say, never the caller's: laid
    /// over another device's channels the gains would silence the wrong
    /// ones, and taken off the combined device while it still plays they
    /// would give the mix back to whoever switched theirs off. Message
    /// thread; safe while running.
    static constexpr int kMaxOutputChannelGains = 64;
    void setHeadphoneJackGains (const std::string& outputDeviceId, std::vector<float> gains);

    /// On by default. Off for harnesses that drive the output callback in
    /// simulated time, where a real-time thread deciding the output has
    /// gone quiet would pull the rings underneath the simulation.
    void setSoftwareClockEnabled (bool enabled) noexcept { softwareClockEnabled = enabled; }

    /// §0.1: samples the per-device rings threw away because nothing pulled
    /// them in time, summed over every device. Zero on a healthy take.
    uint64_t getOverrunSamples() const noexcept;

    /// §4: trim, live. Applies to the monitor mix and the mix file; the stems
    /// stay at unity either way. Safe to call while the callback is running --
    /// it stores one float per channel into an already-sized vector.
    void setChannelTrimDb (int index, float trimDb) noexcept;
    float getChannelTrimDb (int index) const noexcept;

    /// The channel list this take was opened with. §6.5 fixes it for the
    /// duration of a recording, so callers reacting to a hot-plug must work
    /// from this rather than from the current device list.
    const std::vector<CaptureChannel>& getChannels() const noexcept { return channels; }

    MonitorBus& getMonitorBus() noexcept { return monitorBus; }

    /// §5.5 feedback protection on the headphone mix. Analysed on its own
    /// thread from a copy the output callback hands over; a band that grows
    /// the way a howl does cuts the bus exactly as the limiter's runaway cut
    /// does. Runs while monitoring.
    const FeedbackGuard& getFeedbackGuard() const noexcept { return feedbackGuard; }

    /// The app's own sounds -- take started, take stopped, something is
    /// wrong -- mixed into the headphone output by the callback.
    AlarmTone& getAlarm() noexcept { return alarm; }
    const AlarmTone& getAlarm() const noexcept { return alarm; }
    Metering* getChannelMetering (int index) noexcept;
    Metering& getMixMetering() noexcept { return mixMeter; }

    /// The weight a channel is heard at in the headphones on top of its trim:
    /// 1, except for a fresh two-input device whose stereo/mono layout §2.1 is
    /// still deciding (each side at 0.5) or has decided Mono (the chosen side
    /// at 1, the other at 0). Never applied to the stems. As of the last
    /// block mixed; for tests and diagnostics.
    float getChannelMonitorWeight (int index) const noexcept;

    /// §6.6: the fraction of each callback's available time actually spent in
    /// it. This is the load that matters for dropouts -- overall machine CPU
    /// can look calm while the audio thread is already missing its deadline.
    double getAudioCallbackLoad() const noexcept { return callbackLoad.load (std::memory_order_relaxed); }

    /// Frames the writer could not take this take. Still answers after the
    /// take has stopped: session.json is rewritten at Stop, after
    /// stopRecording() has destroyed the pipeline, and read as zero there.
    uint64_t getFramesDropped() const noexcept
    {
        return pipeline != nullptr ? pipeline->getFramesDropped() : lastTakeFramesDropped;
    }

    /// The loudest sample the current take has written, or -1 when there is no
    /// pipeline to have measured one. Negative means "not measured" to
    /// judgeTakeAudio, which never reports silence on a reading nobody took.
    float getPeakWritten() const noexcept { return pipeline != nullptr ? pipeline->getPeakWritten() : -1.0f; }

    /// The loudest sample that reached this coordinator, whether or not the
    /// writer accepted it. Paired with getPeakWritten() it separates "no audio
    /// arrived" from "audio arrived and this app dropped it".
    float getPeakArrived() const noexcept { return peakArrived.load (std::memory_order_relaxed); }

    /// Frames the writer could not take. Zero on a healthy take.
    uint64_t getFramesAcceptedCount() const noexcept
    { return pipeline != nullptr ? pipeline->getFramesAccepted() : 0; }

    void resetArrivalPeak() noexcept { peakArrived.store (0.0f, std::memory_order_relaxed); }

    /// §6.5: shed the stems and keep the mix when the ring is nearly full.
    /// The mirror is downstream of the same ring, so it cannot protect stems
    /// from a ring overflow and does not change this decision.
    void fallBackToMixOnly() noexcept { if (pipeline != nullptr) pipeline->fallBackToMixOnly(); }
    bool isMixOnly() const noexcept { return pipeline != nullptr && pipeline->isMixOnly(); }

    /// Frames taken from the audio thread so far -- §6.5 wants the exact sample
    /// position where degradation began, and this is that clock.
    uint64_t getFramesAccepted() const noexcept { return pipeline != nullptr ? pipeline->getFramesAccepted() : 0; }

    /// §6.5 "target card removed": the destination stopped accepting writes.
    /// The take is over -- the owner stops and finalizes, and tells the user.
    /// True while the take's card writes are failing, and still true after the
    /// take has stopped -- the failure and its account outlive the pipeline.
    ///
    /// They did not, and that cost the user the only sentence that told them
    /// what was actually wrong. stopRecording() destroys the WritePipeline, so
    /// every one of these read as "nothing wrong" the moment a take ended,
    /// and a caller reading them after the stop -- which the card-failure path
    /// in Application did -- could never see anything.
    bool hasCardWriteFailed() const noexcept
    {
        return pipeline != nullptr ? pipeline->hasCardWriteFailed() : lastTakeCardWriteFailed;
    }

    /// The worst single channel's overrun since the take began, in frames.
    ///
    /// Summing every channel answers "how many samples were thrown away", which
    /// is the right question for a record of the loss. It is the WRONG number
    /// to turn into seconds: four rings overflowing together for one second
    /// lose one second of recording, not four. The alert that says "about N
    /// seconds lost so far" needs the worst channel, and used to get the sum --
    /// overstating by the channel count on exactly the rigs this app is for.
    uint64_t getWorstChannelOverrunThisTake() const noexcept;

    /// Overrun samples since the current take began, summed across channels.
    /// getOverrunSamples() is the whole monitoring session's; this is the one a
    /// take may report.
    uint64_t getOverrunSamplesThisTake() const noexcept
    {
        const auto total = getOverrunSamples();
        return total >= overrunAtTakeStart ? total - overrunAtTakeStart : total;
    }

    /// The other way a ring loses audio: the pull came and the device's block
    /// had not, so the stem got silence. Summed across channels for the whole
    /// monitoring session, and since the current take began. The stream
    /// counted this from the first day and nothing ever asked, so a take
    /// whose microphone ran dry every other block reported no loss at all.
    uint64_t getUnderrunSamples() const noexcept;
    uint64_t getUnderrunSamplesThisTake() const noexcept
    {
        const auto total = getUnderrunSamples();
        return total >= underrunAtTakeStart ? total - underrunAtTakeStart : total;
    }

    /// §0.1: audio that arrived from a device and had nowhere to go, because
    /// the block did not match the layout this take was opened with.
    ///
    /// The take's channel list is fixed for its duration (§6.5) and that is
    /// right, but a device handing over two inputs where four were planned then
    /// leaves two channels writing silence -- which used to happen with nothing
    /// counting it anywhere. Wall-clock frames, counted once per block, and
    /// zeroed when a take begins.
    uint64_t getFramesMissedByLayout() const noexcept
    {
        return framesMissedByLayout.load (std::memory_order_relaxed);
    }

    /// A more specific account of a card write failure than "it stopped
    /// accepting writes", when the writer has one. Empty otherwise.
    std::string getCardWriteProblem() const
    {
        return pipeline != nullptr ? pipeline->getCardWriteProblem() : lastTakeCardWriteProblem;
    }
    bool mirrorRanOutOfSpace() const { return pipeline != nullptr && pipeline->mirrorRanOutOfSpace(); }

    /// Where the take's folder is now -- see WritePipeline::getLiveSessionFolder.
    /// Survives stopRecording(), so the stop-time files follow a folder that
    /// was renamed during the take. Empty before any take.
    std::string getLiveSessionFolder() const
    {
        return pipeline != nullptr ? pipeline->getLiveSessionFolder() : lastTakeLiveSessionFolder;
    }

    /// §6.3: the mirror's equivalent. The pipeline already stops mirroring on
    /// a failed write and deliberately leaves the card write alone -- what this
    /// exposes is the fact that it happened, so the take's owner can say so and
    /// put it in the record.
    bool hasMirrorWriteFailed() const noexcept
    {
        return pipeline != nullptr ? pipeline->hasMirrorWriteFailed() : lastTakeMirrorWriteFailed;
    }

    /// §6.3: a mirror was asked for and could not be opened, so this take has
    /// no second copy at all. Distinct from a mirror that stopped mid-take.
    bool hasMirrorFailedToOpen() const noexcept { return pipeline != nullptr && pipeline->hasMirrorFailedToOpen(); }

    /// Why the last startRecording() returned false, in the user's words.
    /// Empty when the take started. The bool was the whole report until now,
    /// which is why a take that could not open its files failed in silence.
    const std::string& getRecordingProblem() const noexcept { return recordingProblem; }
    double getRingFillFraction() const noexcept { return pipeline != nullptr ? pipeline->getFillFraction() : 0.0; }

    /// The block size the open streams actually run at. The §5.4 ladder can
    /// have moved on from this mid-take, since a step taken then is applied
    /// when the take ends; what a take's record should carry is this.
    int getBufferSizeSamples() const noexcept { return bufferSize; }

    /// BS.1770 loudness of the mix as written. What every streaming platform
    /// normalises against, and the only figure that says how loud a take will
    /// actually sound -- peak level says nothing about it.
    /// The take's loudness figures. During a take these come from the live
    /// pipeline; after it, from the snapshot taken as the take stopped.
    ///
    /// Without that snapshot the pipeline was destroyed by stopRecording and
    /// the block count fell to zero, so the delivery-target advice reverted to
    /// "Not enough sound yet to judge how loud this is." the instant the take
    /// ended -- which is the exact moment the user wants to read it.
    double getIntegratedLufs() const;
    double getTruePeakDbtp() const;
    int getLoudnessBlockCount() const;

    /// One microphone's audio callback (§3.2). Separate USB devices run on
    /// independent clocks, so each one delivers on its own thread and into its
    /// own ring rather than as one aligned block.
    void pushDeviceBlock (int deviceIndex, const float* samples, int numSamples) noexcept;

    /// §2.1: one microphone's callback when the device presents more than one
    /// channel, which many USB microphones do -- as stereo with one silent side,
    /// or with a single capsule duplicated across both.
    ///
    /// Picks the side the signal is actually on and pushes that. Taking channel
    /// 0 regardless is how a microphone wired to the right records silence:
    /// nothing warns, the meter sits at the floor, and the stem is empty.
    ///
    /// Real-time safe: the analysis is two passes over the block with no
    /// allocation, and the chosen channel is pushed by pointer, so collapsing to
    /// mono costs no copy (§11).
    void pushDeviceBlockMultiChannel (int deviceIndex, const float* const* inputs,
                                      int numInputs, int numSamples) noexcept;

    /// §2.1: which channel of the device this take is taking as mono, and what
    /// the analyzer concluded. -1 for a device that presented only one channel.
    int getChannelLayoutSource (int index) const noexcept;
    ChannelLayoutDecision getChannelLayoutDecision (int index) const noexcept;
    bool isChannelLayoutDecisionPersistable (int index) const noexcept;

    /// Pulls one block the way the real output callback does, taking the same
    /// hand-off gate first. Returns false when the software clock already held
    /// it and this block was therefore left to the clock.
    ///
    /// Anything that is not the audio callback or the software clock has to
    /// come through here. processOutputBlock() assumes the gate is already
    /// held, and it used to be public: all four harnesses called it directly
    /// while the software clock thread was running, so a test and the clock
    /// pulled the same rings at once. ThreadSanitizer reports 150 data races
    /// in sim_capture_mac for it, and the peaks those tests assert on could be
    /// written by either puller.
    bool pullOutputBlock (float* const* outputs, int numOutputs, int numSamples) noexcept;

    /// Aggregate-device path: every channel arriving in one already-aligned
    /// callback, as a CoreAudio aggregate or an ASIO device delivers it. No
    /// drift correction is applied because the OS has already done it.
    /// §11: no allocation, locking, logging or file I/O in here.
    void processAudioBlock (const float* const* inputs, int numInputs,
                            float* const* outputs, int numOutputs, int numSamples) noexcept;

    /// §3.1 / §3.3: which channel is the clock reference. Out-of-range clears it.
    ///
    /// This does not change what the capture path does to the audio. The stream
    /// every device is pulled by is the output device's (§3.2), so all channels
    /// are corrected onto that regardless -- exempting the master would leave it
    /// uncorrected against a clock it has no relationship to, not make it the
    /// timebase. What the reference selects is the channel §3.3's drift figures
    /// are quoted against, and the clock source Application hands the OS
    /// aggregate. Both are safe to move mid-take.
    void setMasterChannel (int index) noexcept;
    int getMasterChannel() const noexcept { return masterChannel; }

    /// §14.4: the strongest inter-channel correlation seen across the rig, and
    /// the quietest channel outside that pair.
    ///
    /// Two microphones hearing the same thing while a third hears nothing is
    /// what an omni or stereo pattern looks like from the outside -- the pair
    /// are picking up the room rather than the person in front of them. Only
    /// the audio path can measure it: correlation is a sample-level quantity
    /// and the per-channel peaks the advisor is otherwise fed cannot carry it.
    ///
    /// Meaningless below three channels, where §14.4's rule has no third
    /// channel to check, and reported as zero correlation so it cannot trigger.
    float getPolarPairCorrelation() const noexcept
    {
        return polarCorrelation.load (std::memory_order_relaxed);
    }

    float getPolarThirdChannelPeakDb() const noexcept
    {
        return polarThirdPeakDb.load (std::memory_order_relaxed);
    }

    /// §3.3 drift reporting, driven from the UI tick rather than the callback.
    void tickDriftReporting (double elapsedSeconds) noexcept;

    /// §3.3, relative to the clock master: positive means this device runs fast
    /// against it. The master reports zero against itself.
    double getChannelDriftPpm (int index) const noexcept;

    /// §3.3's figure as measured: this channel's clock against the master's,
    /// from DeviceInputStream::getMeasuredDriftPpm (each side measured against
    /// the pulling clock; the pulling clock's own skew cancels in the
    /// difference). Meaningful only once hasChannelDriftMeasurement().
    double getChannelMeasuredDriftPpm (int index) const noexcept;
    bool hasChannelDriftMeasurement (int index) const noexcept;
    double getChannelMeasurementSeconds (int index) const noexcept;

    /// §5.4: blocks in which any channel's ring lost audio, summed over the
    /// channels, as events. The buffer ladder counts these.
    uint64_t getRingLossEvents() const noexcept;

    /// Diagnostics for the clock harnesses: one channel's loop state as it
    /// stands, with nothing subtracted. Ring fill 0..1, the stream's own PPM
    /// against the clock pulling it, and what that ring has thrown away.
    double getChannelFillFraction (int index) const noexcept;
    double getChannelRawDriftPpm (int index) const noexcept;
    uint64_t getChannelOverrunSamples (int index) const noexcept;

    /// How the consumer clock is keeping time, for the same harnesses. All
    /// three are running maxima/counts since monitoring began: the latest a
    /// software-clock tick woke after its deadline, how many ticks were pulled
    /// while a period or more behind, and the longest single pull. Relaxed
    /// atomics written on the audio threads; nothing allocates or locks (§11).
    struct ClockDiagnostics
    {
        uint64_t maxWakeLateUs = 0;
        uint64_t catchUpTicks = 0;
        uint64_t maxPullUs = 0;
    };
    ClockDiagnostics getClockDiagnostics() const noexcept
    {
        return { clockMaxWakeLateUs.load (std::memory_order_relaxed),
                 clockCatchUpTicks.load (std::memory_order_relaxed),
                 clockMaxPullUs.load (std::memory_order_relaxed) };
    }
    bool hasSustainedExcessDrift (int index) const noexcept;
    uint64_t getUnderrunSamples (int index) const noexcept;

    /// Harness diagnostics: the interpolator's seams on one channel.
    struct ChannelSeams { uint64_t primes = 0, holds = 0, skips = 0; };
    ChannelSeams getChannelSeams (int index) const noexcept
    {
        if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
            return {};
        const auto& s = *deviceStreams[static_cast<size_t> (index)];
        return { s.getPrimeCount(), s.getHoldCount(), s.getSkipCount() };
    }

    /// Harness diagnostics: the device block one channel's ring holds room
    /// for, and the room it has given back for blocks its device left
    /// (DeviceInputStream::getReleasedHeldSamples).
    struct ChannelHeldRoom { size_t heldBlock = 0; uint64_t releases = 0, releasedSamples = 0; };
    ChannelHeldRoom getChannelHeldRoom (int index) const noexcept
    {
        if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
            return {};
        const auto& s = *deviceStreams[static_cast<size_t> (index)];
        return { s.getHeldBlockSamples(), s.getHeldReleases(), s.getReleasedHeldSamples() };
    }

private:
    IAudioBackend& backend;
    double sampleRate;
    int bufferSize;

    std::vector<CaptureChannel> channels;
    MonitorBus monitorBus;
    FeedbackGuard feedbackGuard;
    AlarmTone alarm;
    std::vector<std::unique_ptr<Metering>> channelMeters;
    std::vector<std::unique_ptr<DeviceInputStream>> deviceStreams;

    // The software clock. The rig is pulled onto the output device's callback
    // (§3.2), which meant a rig with no output, or one whose output stopped,
    // recorded nothing and said nothing. This thread ticks at the buffer
    // period and pulls whenever the output is absent or has gone quiet, so
    // the take never depends on the headphones. `pulling` is the hand-off:
    // whichever of the two clocks holds it does the pull; the other skips.
    std::thread softwareClock;
    std::atomic<bool> clockRunning { false };
    std::atomic<bool> pulling { false };

    /// The output device's callback, which is the clock everything else is
    /// pulled onto (§3.1). Sums the drift-corrected mics, meters them, feeds
    /// the writer, and fills the headphone buffers. `pulling` must already be
    /// held: use pullOutputBlock() from anywhere that does not hold it.
    void processOutputBlock (float* const* outputs, int numOutputs, int numSamples) noexcept;
    std::atomic<bool> outputClockLost { false };
    /// abandonOutputClock(): the output callback pulls nothing and the
    /// software clock pulls instead, however recently the output called back.
    std::atomic<bool> outputAbandoned { false };
    std::array<std::atomic<float>, kMaxOutputChannelGains> outputChannelGains;
    /// setHeadphoneJackGains(): which device's channels the gains describe,
    /// and the gains. Message thread only; the callback reads the atomics.
    std::string jackGainsDeviceId;
    std::vector<float> jackGains;
    /// Lays the jack gains on the output channels if `outputDeviceId` is the
    /// device they belong to, and the mix on every channel otherwise.
    void layJackGainsFor (const std::string& outputDeviceId) noexcept;
    void setOutputChannelGains (const std::vector<float>& gains) noexcept;
    std::atomic<int64_t> lastOutputCallbackNs { 0 };
    bool outputStreamOpen = false;
    bool outputCarriesChannels = false;
    std::string openOutputDeviceId;
    bool softwareClockEnabled = true;
    void runSoftwareClock();
    void stopSoftwareClock();

    /// Loudest sample seen arriving, across the whole take.
    std::atomic<float> peakArrived { 0.0f };

    /// §2.1 per device, for the ones that arrive with more than one channel.
    ///
    /// Touched only from that device's own audio callback, which is the single
    /// producer for it, so no lock is needed. `source` is read by the UI too and
    /// is therefore atomic -- a plain int written on the audio thread and read
    /// on the message thread is a data race even when every value is valid.
    struct ChannelLayout
    {
        ChannelLayoutAnalyzer analyzer { 48000.0 };
        std::atomic<int> source { -1 };
        std::atomic<int> decision { static_cast<int> (ChannelLayoutDecision::Pending) };
        std::atomic<bool> decisionPersistable { false };
        bool frozen = false;
    };

    std::vector<std::unique_ptr<ChannelLayout>> channelLayouts;

    /// Updates one preallocated analyzer from a physical stereo pair. It does
    /// not route audio, allocate, or lock. `freezeDuringRecording` is true for
    /// an already-collapsed microphone, whose selected side must not move in a
    /// take; false for bootstrap observation, which cannot alter either of the
    /// two independently routed channels.
    void analyzeStereoPair (int channelIndex, const float* left, const float* right,
                            int numSamples, bool freezeDuringRecording) noexcept;
    int masterChannel = -1;
    Metering mixMeter;

    // Owned by the UI thread. The audio thread never reads this handle -- it
    // reads activePipeline below, which is a plain pointer it can load
    // atomically. A std::unique_ptr is three words with no atomicity guarantee
    // of any kind, so the callback testing `pipeline != nullptr` while
    // startRecording/stopRecording moved it was a data race that could hand the
    // audio thread a pointer to a pipeline being destroyed underneath it.
    std::unique_ptr<WritePipeline> pipeline;

    // The audio thread's view of the pipeline: published with release once the
    // pipeline is fully started, cleared with release before it is torn down.
    std::atomic<WritePipeline*> activePipeline { nullptr };

    // Non-zero while a callback is inside the region that dereferences
    // activePipeline. stopRecording waits for this to drain before destroying
    // the pipeline, so a callback that loaded the pointer just before the store
    // still finishes against a live object.
    std::atomic<int> pipelineUsers { 0 };

    bool monitoring = false;
    std::string monitorProblem;
    double monitoringLatencyMs = 0.0;
    int alignedInputLatencyFrames = 0;

    // The monitor output's block -- what the device granted, or the size
    // asked for where it cannot say -- and what it adds after the buffers.
    // Each microphone's ring is pulled a block at a time, so its cushion is
    // counted in these; and a buffer change keeps the second.
    int monitorOutputFrames = 0;
    int monitorPresentationFrames = 0;

    // Per channel: the device's input latency (-1 for a device that did not
    // open), filled in once the streams are open and published by
    // latenciesReady.
    std::vector<int> channelInputLatency;
    std::atomic<bool> latenciesReady { false };

    // Message thread: what each driver reported before it was bounded, and
    // whether any figure had to be -- for the monitoring session, and for
    // the take running or just finished (areStemsAligned()).
    std::vector<int> channelReportedLatency;
    bool latencyClamped = false;
    bool latencyClampedThisTake = false;

    // Per channel, how far it is held back in its stem to line up with the
    // slowest device: worked out by the consumer before every block it hands
    // the writer (recordingOffsets, consumer-owned) and published for the
    // message thread (recordingOffsetView). Both sized at startMonitoring().
    std::vector<int> recordingOffsets;
    std::unique_ptr<std::atomic<int>[]> recordingOffsetView;

    // Consumer-owned, sized with recordingOffsets: whether each channel's
    // offset is for a block its device has not confirmed, or takes one back
    // (StemOffsetKind, handed to the writer beside the offsets), and the
    // block each stream held at the last update, which is how a refusal is
    // seen: a held block only ever falls when a provisional one is refused.
    std::vector<StemOffsetKind> recordingOffsetKinds;
    std::vector<size_t> heldBlockSeen;

    // Consumer-owned, reset with the vectors above: the largest reference the
    // stems have been lined up to during the take being written. It does not
    // fall when the slowest device gives back held room, and is let go
    // between takes (updateRecordingOffsets).
    int stemReferenceFloor = 0;
    void updateRecordingOffsets() noexcept;

    /// A channel's own place in time beyond the cushion every stream shares:
    /// its device's input latency plus the IO block it runs at (the one
    /// asked for, until the device has settled on one). -1 for a device that
    /// did not open.
    int channelOwnLatencyFrames (size_t index, size_t blockFloor) const noexcept;

    /// The quickest (or slowest) channel's own input side at this buffer
    /// size, and which channel that is (-1 when none has opened).
    int monitorInputFrames (int bufferSizeFrames, bool slowest, int* channel) const noexcept;

    /// The one sum behind every headphone figure the app gives (§5.4), so
    /// the buffer-ladder notice and the Advanced panel cannot disagree:
    /// `roundTripMs` -- an input block at `bufferSizeFrames`, the output
    /// block and what the output device adds after it, as the stream was
    /// worked out -- plus the ring's cushion (DeviceInputStream holds
    /// cushionSamples of a pull -- one pull plus a few milliseconds of late
    /// delivery -- a pull being the output block or the size asked for where
    /// that is larger, before the output takes a sample), plus
    /// whatever of the microphone's own input side (`inputFrames`) goes past
    /// the one input block already counted. Zero stays zero: nothing is
    /// monitoring.
    double monitorPathMs (double roundTripMs, int bufferSizeFrames, int outputFrames, int inputFrames) const noexcept;
    int channelIndexForDevice (const std::string& deviceId) const noexcept;

    std::vector<std::string> devicesThatFailedToOpen;
    std::string recordingProblem;
    std::atomic<uint64_t> framesMissedByLayout { 0 };

    // The finished take's loudness, captured while the pipeline was still
    // alive. Read only after it has gone; cleared when the next take begins so
    // take three cannot show take two's number.
    // Silence, not zero: before the first take these are what the getters
    // return, and zero LUFS would read as a deafening mix rather than nothing.
    // The take's write outcome, captured as it stopped. Cleared when the next
    // take begins. See the note on hasCardWriteFailed().
    std::chrono::milliseconds filesystemDeadline { 5000 };
    std::function<void()> filesystemStallForTesting;
    std::function<void()> writerChunkHookForTesting;
    bool lastStopTimedOut = false;

    std::string lastTakeCardWriteProblem;
    std::string lastTakeLiveSessionFolder;
    bool lastTakeCardWriteFailed = false;
    bool lastTakeMirrorWriteFailed = false;

    double lastTakeLufs = LoudnessMeter::kSilenceLufs;
    double lastTakeTruePeakDbtp = LoudnessMeter::kSilenceLufs;
    int lastTakeLoudnessBlocks = 0;
    uint64_t lastTakeFramesDropped = 0;

    /// The overrun total when the current take began. prepare() zeroes each
    /// stream's counter, but streams are prepared when monitoring starts, not
    /// when a take does -- so the take's own figure is measured from here.
    uint64_t overrunAtTakeStart = 0;
    uint64_t underrunAtTakeStart = 0;

    /// Per-stream overrun totals when the take began, so the worst channel can
    /// be measured against its own starting point rather than the rig's.
    std::vector<uint64_t> overrunBaselinePerStream;

    /// The writer's alignment account of the take just finished, per channel,
    /// captured as it stopped (the pipeline is gone after that); see
    /// getDeviceAlignmentSilenceFramesThisTake() and areStemsAligned().
    std::vector<uint64_t> lastTakeAlignmentSilence;
    std::vector<uint64_t> lastTakeAlignmentDropped;
    std::vector<int> lastTakeAlignmentStart;
    bool lastTakeStemsAligned = false;

    /// Each stream's own shift silence (getAlignmentSilenceSamples) when the
    /// take began, and how much of it the take had when it stopped; see
    /// getDeviceIoShiftFramesThisTake().
    std::vector<uint64_t> shiftSilenceBaselinePerStream;
    std::vector<uint64_t> lastTakeShiftSilence;
    uint64_t channelShiftSilenceThisTake (size_t index) const noexcept;

    // Scratch for the summed monitor mix and the per-sample trim frame, both
    // sized at startMonitoring(). §11 forbids the callback allocating, and a
    // per-block vector here would do exactly that.
    std::vector<float> mixScratch;

    // The bus before master volume, per sample of the block, for the
    // feedback guard. Sized with mixScratch.
    std::vector<float> busScratch;

    // The app's own sounds alone (AlarmTone), for an output channel switched
    // off from the mix: it still gets the chirps and the siren. Sized with
    // mixScratch.
    std::vector<float> alarmScratch;
    std::vector<float> trimFrame;
    std::vector<float> trimGains;

    // §2.1 in the headphones: for each channel of a fresh two-input device
    // whose layout is still being analysed, the channel that analyses the pair
    // (its input 0), else -1; which side of the pair it is; the weight it is
    // monitored at now and the weight it is heading for. Sized at
    // startMonitoring(), read and moved only by the callback.
    std::vector<int> monitorPairAnalyzer;
    std::vector<int> monitorPairSide;
    std::vector<float> monitorPairWeight;
    std::vector<float> monitorPairTarget;
    static constexpr float kUnsettledPairMonitorGain = 0.5f;
    static constexpr double kPairWeightRampSeconds = 0.010;
    float pairMonitorTarget (size_t channel) const noexcept;

    // Per-device pulled audio and the pointer table the writer wants, both
    // sized at startMonitoring(). The output callback fills these every block
    // and §11 forbids it allocating them there.
    std::vector<float> deviceScratch;      // channelCount * max (2 * bufferSize, 4096), contiguous per channel
    std::vector<const float*> devicePointers;

    // Written by the audio thread, read by the UI. Relaxed because a stale
    // reading for one frame is harmless and a lock here would not be (§11).
    std::atomic<double> callbackLoad { 0.0 };
    std::atomic<uint64_t> clockMaxWakeLateUs { 0 };
    std::atomic<uint64_t> clockCatchUpTicks { 0 };
    std::atomic<uint64_t> clockMaxPullUs { 0 };

    // §14.4, same ownership: written in the callback, read on the UI tick.
    //
    // The floor is a "nothing heard yet" value, not a loud one: a peak-hold
    // that starts at 0 dBFS spends its first seconds decaying down from a level
    // nothing produced, which would disarm the detector exactly when a take is
    // starting. Nothing can trigger from it regardless, because the correlation
    // beside it starts at zero.
    static constexpr float kPolarFloorDb = -200.0f;

    std::atomic<float> polarCorrelation { 0.0f };
    std::atomic<float> polarThirdPeakDb { kPolarFloorDb };

    // Peak-held on the audio thread so a loud moment on the third channel
    // survives until the UI next looks. The tick runs at 2 Hz and sees one
    // block in several hundred; without the hold, the one thing that should
    // break a false trigger is the thing most likely to be missed.
    float polarThirdPeakHeldDb = kPolarFloorDb;

    /// §14.4 measurement over one frame block, on the headphones' timing.
    /// Real-time safe: sums over the block, no allocation, no locking.
    void measurePolarPattern (const float* const* inputs, int channelCount, int numSamples) noexcept;

    void noteCallbackLoad (std::chrono::steady_clock::time_point start, int numSamples) noexcept;

    /// The reference channel's own correction against the output clock, which
    /// every §3.3 figure is quoted relative to. Zero when there is no master.
    double getMasterDriftPpm() const noexcept;

    /// Shared by both capture paths: sum, meter, record and publish one frame
    /// block, each channel as early as its device allows. The headphone mix
    /// is made from it as it stands; the writer is handed recordingOffsets
    /// (or nothing, where the OS has already lined the channels up) and
    /// lines the stems up itself. outputFrameOffset selects the destination
    /// range when a larger callback is processed in bounded slices.
    /// Real-time safe.
    void mixAndPublish (const float* const* inputs, int channelCount,
                        float* const* outputs, int numOutputs, int numSamples,
                        int outputFrameOffset, const int* recordingOffsetsForBlock,
                        const StemOffsetKind* recordingOffsetKindsForBlock) noexcept;
};

} // namespace mma
