# SobStage — multi-microphone aggregator, recorder, and monitor
## Build Specification — v3, self-contained
Cross-platform desktop application (macOS + Windows). Records multiple external
microphones as separate channels directly to a selected destination and outputs
one shared live monitor mix to headphones. On macOS it also publishes a
transient combined input while the app runs; Windows v1 is standalone. Must be
operable by someone who has never configured audio hardware.
Reference hardware: Blue Yeti (standard model). Section 14 is written against its measured behavior and constrains decisions elsewhere in this document.
**Every value in this document is a specified default, not a placeholder.** Where a number appears, use it. Where a rule appears, follow it. Section 16 lists decisions that are closed and the reasoning behind them, so they are not reopened mid-build.
---
## 0. Non-negotiables
1. Audio is never lost. A dropped meter frame is acceptable. A dropped sample is not.
2. A novice reaches a finished recording without asking a question or reading anything.
3. Everyone hears the identical mix, containing every microphone including their own, from the moment the app opens.
4. The user never touches OS sound settings.
5. Nothing ships that requires disabling driver signature enforcement or running in test mode.
---
## 1. Scope and limits
| Property | Value |
|---|---|
| Minimum microphones | 1 |
| Maximum microphones | 8 |
| Behavior above 8 | Enumerate and display, but exclude from capture with a stated reason |
| Maximum session length | Unlimited; files split per §6.1 |
| Minimum macOS | 13.0 |
| Minimum Windows | Windows 10 22H2 |
| Minimum RAM | 4 GB |
| Minimum free internal disk | 2 GB, or the local mirror is disabled |
| Languages at v1 | English only. All user-facing strings externalized for later localization. |
**Single-microphone case.** With one mic the app is still a recorder and monitor.
That input is corrected onto the monitor/output callback's clock like every
other input; “master” is a reporting reference, not a resampling exemption.
Nothing in the UI changes shape.
**Zero-microphone case.** The app opens, shows an empty meter row, and displays one line: "Plug in a USB microphone or audio interface to get started." Monitoring and recording are unavailable but no error state is shown. This is a normal condition, not a failure.
---
## 2. Device aggregation
- Enumerate eligible external audio input devices at launch and after OS
  device-change notifications, never by polling on a timer.
- A recording input must report directly attached external hardware. Never list the computer's built-in microphone, a phone/Continuity source, Bluetooth/AirPlay, a network source, an aggregate/virtual device, an internal sound card, or an unknown transport as a microphone. On macOS the positive allowlist is USB, FireWire, and Thunderbolt; on Windows an eligible USB, IEEE 1394, PCI, or HDAUDIO branch must also have one Plug and Play node that reports both removable capability and an orderly/surprise-removal policy (the PCI/HDAUDIO path permits a removable Thunderbolt ancestor while rejecting fixed internal hardware); on Linux it is an ALSA hardware device whose sysfs ancestry proves an external bus. This restriction applies only to recording inputs; monitor outputs keep §5.3's full selection policy.
- **Identity limit:** reject every phone or wireless transport the OS identifies as such. A phone or receiver that deliberately exposes the same generic removable USB Audio Class identity as an audio interface provides no reliable form-factor signal to these APIs; the app cannot categorically distinguish it without a brittle name or VID/PID denylist that would also reject legitimate interfaces. This limitation must be stated in product and release documentation until the operating system exposes stronger identity evidence.
- Every detected microphone is included automatically. Inclusion is opt-out, not opt-in.
- Hot-plug without restart, without dialogs, without interrupting an active recording. See §6.5.
### 2.1 Channel layout normalization
Many USB mics present as stereo with one silent side or a duplicated mono source. On connection, compare both channels over the first 3 seconds during which either channel exceeds −50 dBFS.
Collapse to mono when **either** condition holds:
- One channel stays below −80 dBFS for the whole window, or
- Inter-channel correlation exceeds 0.99 **and** RMS difference is under 0.5 dB.
Otherwise record true stereo.
If the 3-second signal window is not reached within 60 seconds of connection, default to mono and re-evaluate if signal later appears. Never block on this and never ask a novice "mono or stereo?"
**In the headphones, while it is being decided.** Until there is a verdict, both inputs are kept as separate channels, and for a whole take if Record comes first (the layout is only rebuilt while idle). Summed at unity, a duplicated mono microphone put its one voice into the headphone mix twice: +6 dB, enough on its own to run ordinary speech into §5.5's limiter. So the headphone mix (never the files) plays each side of a pair still being analysed at half level, which is exactly the one voice for a duplicated microphone and only 6 dB low for a real two-input interface for those few seconds. The moment a verdict backed by the 3-second window is reached the headphones follow it, mid-take included and without waiting for the rebuild: Mono plays the chosen side alone, Stereo both sides at full level, the change made over 10 ms so it does not click. The provisional 60-second Mono keeps half each, since it is no evidence either way. Both sides are still written at unity, each to its own file, until the idle rebuild.
### 2.2 Sample rate negotiation
Query supported and current rates for every device. Automatic mode prefers the
rate the rig already shares, capped at 48 kHz, and resamples independent clocks
after opening. Do not force fixed-rate hardware to another rate. If the user
explicitly pins an incompatible rate, refuse the take and name both rates rather
than writing a file whose header does not describe the captured audio.
### 2.3 Bit depth
Follows device capability per channel. Do not upconvert — it adds file size and no information. Where a device supports multiple depths, choose the highest, capped at 24-bit.
### 2.4 Port memory
Identify each physical device by USB location ID plus serial number where present, falling back to location ID alone. When a previously seen device reconnects to the same port, restore its assigned name, trim, and channel-layout decision without re-running §2.1 or §14.6.
---
## 3. Clock drift compensation
Build first. Everything else is easier afterward.
Every USB mic runs an independent crystal. Uncorrected, channels desync and a long take is unusable.
### 3.1 Master selection
Default master: the device with the lowest measured drift after 60 seconds of running. Before that measurement exists, use enumeration order. Overridable behind Advanced.

**The master is a reference, not an exemption.** It names (a) the channel §3.3's drift figures are quoted against and (b) the clock source handed to the OS aggregate device. It does **not** mean "the one input that is never resampled". In the in-app capture path the streams are pulled by the output device's callback, so the clock every input is converted onto is the *output* device's, not any microphone's. Exempting the master from correction there does not make it the timebase — it leaves one channel uncorrected against a clock it has no relationship to, and that channel's ring then walks to one end of its travel and stays there, dropping arrivals when full or holding its last sample when dry. That is drift, on the one channel the whole rig is quoted against, and it is invisible to the underrun counters. Correct every channel, master included.
### 3.2 Correction
- **Every** input passes through an asynchronous sample rate converter locked to the clock that pulls it — the output device's callback in the in-app path (§5.2), the OS in the aggregate path. This includes the §3.1 master; see the note there for why exempting it corrupts the reference rather than protecting it.
- Because the pulling clock's own skew lands in every channel's ratio identically, it cancels in any channel-to-channel comparison. §3.3's per-device figure is therefore reported as this channel's correction *minus the master's*, which is what makes "runs fast relative to the master" true of the number shown.
- ASRC ratio is driven by a PI control loop on measured ring-buffer fill.
  - Starting gains: Kp = 5e-6, Ki = 1e-8, per sample of fill error. At 5 PPM per fill sample, the proportional path reaches the ±200 PPM safety clamp within 40 samples of phase debt — under 1 ms even at 44.1 kHz — while the separate slew limiter below still prevents an instantaneous ratio change.
  - **The fill the loop is driven by must move smoothly, not in whole blocks.** A device delivers whole blocks at its own clock's cadence, so the raw ring level read at a pull is a staircase: a device 150 PPM off moves it 7 samples a second, but the pull sees the same number for nine seconds and then a step of one block. With these gains one block is 320 PPM of request — past the clamp — and the slew then unwinds it for 40 s, during which the level overshoots by more than a two-block cushion. The loop limit-cycles (±58 PPM at 60 PPM of real drift, one dropped block every few seconds on a slow device, every microphone reported at the clamp). The level the loop steers is therefore the raw level with the device's progress since its last delivery folded in (from that delivery's timestamp and its block's duration), lightly smoothed. `Tools/sim_drift_loop.cpp` is the evidence and the test for this, against block-timed devices; `Tools/soak_drift.cpp` feeds drift a sample at a time and cannot see it.
  - **Audio that arrives late for a span already written as silence is skipped, not played late.** A device thread that stalls hands over everything the driver held when it wakes. The loop must not read that burst as fill (it would slam to the clamp and dry the ring again), and it must not be drained slowly either: for as long as the drain takes the channel sits a stall's length behind every other, which is inter-channel misalignment, and the monitor path is that much slower. The silence already in the file is the loss; the stream skips the late audio up to the silence it answers for, at the first pull that finds the ring above target, and the channel is back in step at once. Silence that nothing arrives to answer — a device that dropped the audio itself — is written off after a ring's worth of pulls, so it cannot eat a later block that merely runs a few samples ahead.
  - **§3.3's figure is a measurement, not the loop's state.** Each device's delivered-sample count and the consumer's pulled-sample count are each fitted against their own timestamps over the 60-second window; the ratio of the two rates is the device's drift against the pulling clock, and it is within about 2 PPM at the minute whatever the loop is doing. The loop's own PPM is a control state — slewing from zero, overshooting to the clamp, disturbed by every lost block — and reading it at sixty seconds reported +200 for a rig of perfect clocks.
  - Maximum ratio deviation: ±200 PPM. Clamp there.
  - Maximum ratio slew: 20 PPM per second. **Never correct instantaneously** — that produces audible pitch artifacts. 20 PPM is 0.035 cent, the whole ±200 PPM range 0.35 cent, and the ratio moves through it smoothly; at 5 PPM/s (the earlier figure) a microphone 160 PPM off the output took half a minute to catch and ran its ring 120 samples down on the way, a gap at the top of every take.
  - **Anti-windup pauses the integral only when the error would push it further the way the output is already straining** (rate- or clamp-limited upward with the level above target, or downward with it below). Pausing on every limited block froze the integral on real hardware: the level carries a sample or two of jitter, the proportional term turns that into several PPM of request, and the slew allows a few hundredths of a PPM a block, so the output was limited on every block. The loop then ran on its proportional term alone — a microphone 100 PPM off held 20 samples below target for the whole session — and periodic gaps did the rest of the correcting.
- **Drift slack is capped by the monitor latency budget, not chosen freely.** Drift safety and low latency pull against each other; latency wins wherever it can, and what it costs is in the headphone delay the app reports (§5.4), never hidden.
  - The slack is the ring's target fill, and it has two jobs: the drift between the device's clock and the output's (which the loop holds to a few samples once it has caught it), and a device handing a block over late. Lateness is time, not blocks — a CoreAudio IOProc a few milliseconds late on a busy Mac, a Windows DPC holding a 3 ms period back by 6 ms — and a delivery later than the slack writes silence into the channel: a click in the headphones and a gap in the stem. The ring therefore holds **one pull plus 6 ms of late delivery, never less than two pulls and never more than four**: 256 samples (5.3 ms) at 64/48k, 416 at 128, 432 at a 144-sample Windows period, 544 at 256, and two pulls — already more than 6 ms — at 480 and 512. Four pulls is the cap because at 64 samples it is what still fits the 10 ms ceiling with the input and output blocks; a full 6 ms there would not. Measured through the real coordinator with six simulated microphones on drifting clocks: two pulls (2.7 ms at 64) gave 30 clicks a minute at 64 samples with 0.5% of callbacks up to 2 ms late, 216 a minute at 3 ms, and 38 a minute at 144-sample periods with 0.2% up to 6 ms late; this cushion gives none in any of them. `DeviceInputStream::cushionSamples` is the one definition: the streams fill to it and `CaptureCoordinator::monitorPathMs` adds it to the headphone delay the app names, so the figure shown is the cushion held (with the resampler's two samples below).
  - What absorbs a machine whose scheduling jitter exceeds the cushion is §5.4's buffer ladder, which steps on the app's own ring losses (a dry pull or a full ring is the callback missing its deadline in every sense meant there), on every platform. The ring behind the cushion is sized separately from it: it holds the whole of the driver's own ring (eight periods on Linux, which is what stands between a late reader wake and an xrun in the kernel) on top of the target fill, because a reader that wakes late hands all of that over at once, and a ring that could not take it threw away audio the hardware had kept. Sixteen blocks; no latency, since the target fill is what the monitor path pays.
- **Short-kernel ASRC on the monitor path** — 16-tap sinc or equivalent. Long-kernel high-quality resamplers add delay and are disqualified live. If quality suffers, use the short kernel live and a higher-quality offline pass for recorded stems.
  - What runs is a **four-point cubic (Catmull-Rom)** per channel: each output lands between the middle two of the last four samples taken from the ring, the outer two shaping the curve between them. It passes every sample through exactly and follows a straight line or a parabola exactly, and it costs at most two samples of delay (0.04 ms at 48 kHz), which the headphone delay the app reports includes (§5.4). The four samples carry across blocks, across a gap (the output after it picks up from the samples and the fraction of a sample where it stopped), and across room given back for a larger block (all four move by exactly the room); a fresh start, a skip past late audio and `prepare()` begin from the first sample alone.
  - It replaced a straight line between two samples. Every channel's ratio sits a little off 1.0 — the loop tracking its device's clock — so where an output lands between two samples sweeps through a whole sample over and over, once every 1/PPM samples: five times a second at 100 PPM, once a second at 20. Half way between two samples a straight line is a two-sample average, which takes 3 dB off 12 kHz and 6 dB off 16 kHz; on a sample it takes nothing. So the top of every voice rose and fell at that rate, in the headphones and in the stems (§3.2's correction applies to every input, so the stems are written from the same corrected audio): a warble on sibilance and breath, a swirl on cymbals. Measured through the stream and through the coordinator's headphone output, a tone at 8, 12 and 16 kHz swung 1.2, 3.0 and 6.0 dB at 100 PPM and at 20; with the cubic it swings 0.2, 1.1 and 3.3 dB. A 16-tap kernel would also flatten 16 kHz, at eight samples of delay. Nothing else moves: across the simulated rigs, with and without stalls, every gap, held sample, skip and loop correction is the same as with the straight line.
### 3.3 Reporting and failover
- Track per-device drift in PPM, displayed behind Advanced, updated every 10 seconds.
- Flag any device exceeding **100 PPM sustained** as unreliable. Flag, do not exclude.
- **Master failover.** If the master is unplugged, promote the remaining device with the lowest measured drift; tiebreak by enumeration order. Re-lock without stopping the recording. Log the switchover timestamp in `session.json`. A bounded transient is acceptable; a stopped recording is not.
  - Per §3.1 this moves a reference, not a correction: no channel's resampling changes, so mid-take there is no transient to bound and the take's channel list and file layout stay fixed (§6.5). What it does prevent is quoting every surviving microphone against a master that has stopped updating.
### 3.4 Validation gate
A 4-hour take with four dissimilar USB microphones finishes with **under 1 ms inter-channel drift**, measured by cross-correlating a shared transient at the start and end of the take. This passes before UI work is considered complete.

The master's own crystal is part of what this measures. A gate that gives the master a zero offset — a crystal identical to the pulling clock's — assumes away exactly the case §3.1's note describes, and will pass over a master that is drifting. `Tools/soak_drift.cpp` gives it a non-zero offset for that reason.
---
## 4. Gain, trim, and the recorded signal
- Per-microphone trim exists as a single shared-bus trim. It does not create a second mix.
- Range: −20 dB to +20 dB. Default 0 dB. Step 0.5 dB.
- **Trim affects the monitor mix and the summed mix file. Trim does not affect the discrete per-microphone files.** Stems are always written at unity gain, unprocessed.
- Rationale: the novice gets a mix matching what they heard; the editor gets clean stems that a bad trim decision cannot destroy.
- `session.json` records trim values so the mix is reproducible from stems.
- **On hardware with an analog gain knob, software trim cannot recover a clipped input** — the knob sits ahead of the converter. On persistent clipping, direct the user to the physical knob on the named microphone. Never present software trim as the fix.
---
## 5. Monitoring — locked topology
### 5.1 The mix
- One mix. Identical for every listener. Contains every microphone, including each listener's own.
- No mix-minus, no per-listener variation, no solo, no cue routing, no exceptions.
- Live from launch, independent of record state. No arm step, no enable toggle. Hearing the room is the app's proof of life.
- Summing is unity, no per-channel attenuation with channel count. Level is managed by trim and the master control, not by automatic gain. (The one exception is §2.1's pair still being analysed, heard at half each so one voice is not heard twice.)
- Master monitor volume: 0–100, default 70, mapped logarithmically. Recorded files are unaffected by it. It is applied **before** §5.5's safety limiter, so the limiter judges what actually reaches the ears and turning the headphones down takes the mix out of it. Applied after the limiter, as it was, the −3 dBFS ceiling acted on the full-level bus: measured with four microphones in one room peaking at −6 dBFS, the same peaks were squared off at every volume (the worst 50 ms of a minute's conversation 23.5 dB above its distortion at volume 100, 70 and 40 alike), and the volume control only made the distortion quieter.
### 5.2 Physical topology
```
Computer -> combined device (SobStage) -> every microphone's own headphone jack
```
- **Default (macOS): everyone hears everything through their own microphone.** The monitor output is the combined device of §7 — the same "SobStage" other apps record from — so the one mix plays out of every microphone's headphone jack at once. Each person's jack can be switched off in Settings ("Who hears the mix in their headphones"); a switched-off jack gets no mix, never a different one. It still carries the app's own sounds (the start and stop chirps and the fault siren), so a rig with every jack switched off still hears an alarm.
- The combined device is never hog-moded: other apps must still be able to record from it.
- Picking any other output in Settings turns this off and remembers that choice; the priority order of §5.3 then applies as before. Where no combined device exists, or no microphone in it has a headphone jack, §5.3 applies.
- During a take the headphone stream stays where it was opened until Stop, whatever output is picked meanwhile. While that stream is on the combined device, the combined device is not re-made until Stop either (a microphone plugged in or pulled, a rename, or macOS having removed it): re-making it under the take's mix would silence every jack for the rest of the take. This goes by the stream that is open, never by the output picked, and so do the per-jack gains, which follow every change in the combined device's layout.
- Hardware with non-defeatable analog direct monitoring (§14.5) means a listener hears their own voice twice (0 ms analog + the mix). Accepted: the user asked for every mic's jack to carry the whole room.

### 5.3 Output device selection
Automatic, in this priority order:
1. A device the user explicitly chose in a previous session that is currently present.
2. A newly connected output device (a device appearing after launch is assumed to be the one the user just plugged in).
3. A device exposing a physical headphone jack.
4. System default output.
Never a microphone's own playback endpoint on its own, at any priority (the combined device of §5.2 is the one way a mic jack is used).
Never an output the backend positively reports cannot run at the recording
sample rate; continue the priority order among compatible outputs. An output
with unknown rate capability remains eligible so an actual open refusal is
reported rather than guessed.
If the selected output still refuses its real low-latency open, show the reason
and reopen the microphones input-only under the software clock. Headphone
monitoring is off, but recording remains available; an output failure must not
become a recording-input failure.
The same holds when another process changes the open output's nominal sample
rate (an output with inputs of its own is not hogged, so any app can). Its
callback is the clock every input is pulled on (§3.2), so it must not go on
pulling at the new rate -- not even until the app's next look at its streams:
it stops pulling on the platform's own notification of the change, before
the new rate is read or anything decided, and the software clock takes over
from the last block it pulled, so the take keeps wall-clock length and no
ring backs up behind it. A notification that changed nothing costs the
headphones only until the rate is read back. Between takes, reopen the rig at
its own rate, which sets the output back; if it changes again within 30 s of
that, stop using it as the clock (headphones silent, the software clock
pulls) until it returns to the rig's rate, rather than fight over it.
Mid-take, stop using it as the
clock and carry the take on under the software clock with the headphones
silent; Stop reopens it. Only an output that itself carries take audio -- the
combined device, whose rate is its microphones', or a duplex mixer -- stops
the take, as a microphone's rate change does (§2.2). Reports that arrive
together (one change on the combined device is its output's and each
microphone's) are all read before that stop, so the take stops once and the
rig reopens once; none of them is applied to the streams Stop has just
reopened, and an output switched with a microphone is put back by that
reopen rather than set aside. Ring losses from the mismatch -- at the
smallest buffers the rings can overflow in the moment before the software
clock takes over -- are not counted toward §5.4's buffer ladder for 2 s from
when the app first sees the output held, and a take reports them as the
output's switch, not as the computer failing to keep up.
### 5.4 Latency budget — hard constraint
Zero latency is not achievable in a software monitor path. The requirement is a budget.
- **Target: under 6 ms** mic diaphragm to headphone, **measured by loopback at startup, not estimated.**
- **Ceiling: 10 ms.** Above this the listener hears a distinct echo of themselves against their own bone conduction and the product has failed.
| Stage | Budget |
|---|---|
| USB transport + ADC/DAC | ~2–4 ms, fixed, not reducible |
| Input buffer, 32–64 samples @ 48 k | 0.7–1.3 ms |
| Drift and late-delivery cushion (§3.2) | 5.3 ms at 64 samples (four pulls); one pull and 6 ms at 128–256; two pulls from 480 |
| Drift resampler (§3.2, four-point cubic) | 2 samples, 0.04 ms |
| Sum + limiter | < 0.1 ms |
| Output buffer, 32–64 samples | 0.7–1.3 ms |
Requirements this imposes:
- **Exclusive-mode audio only on the monitor path.** CoreAudio direct on macOS;
  WASAPI exclusive on Windows v1. **Shared-mode WASAPI is disqualified** — it
  delivers 40–100 ms, which makes the product unusable. ASIO remains post-v1
  and becomes a supported alternative only when a real capture/monitor path,
  packaging and hardware validation ship together.
- If neither exclusive path is available, say so and name the cause. Never ship a 40 ms mix silently.
- **Nothing on the monitor bus but summing, trim, the master volume and the safety limiter.** No EQ, no filtering, no lookahead, no processing. A lookahead limiter is disqualified: the limiter has zero lookahead and adds no delay. It turns the mix down on the very sample that would go over (instant attack), holds that gain 10 ms, and lets it back over 50 ms, so an overload is turned down rather than squared off; only the rise of the first crest of one, which nothing without lookahead can see coming, can still meet the ceiling flat. The headphone delay the app reports is unchanged by it.
- **The monitor path never waits to line microphones up.** Devices hand their audio over at different times: each has its own input latency (on macOS, the device latency, safety offset and stream latency it reports) and its own IO block, and CoreAudio leaves a device at its own size — 1156 frames, say — when it refuses the 64 asked for. Each microphone reaches the headphones as early as its own device allows. Holding the quicker ones back to the slowest would put one slow interface's 23 ms in everyone's ears, far past the ceiling; audio monitoring comes first, and lining the tracks up is a recording concern, done on the writer thread (§6.1).
  - A device that grows its IO block mid-session moves its own channel later by the growth, once (a counted gap where its ring ran dry, a shift where its cushion covered it), and keeps the room for that block afterwards, so going straight back to it costs nothing. No other channel moves with it.
  - That room is given back once the device has run at a block no more than half as large for 2 s. Kept for good, one excursion to 1024 frames and back left that microphone 20 ms behind the others in the headphones for the rest of the session: its talker heard themselves late, and their voice reached the mix twice, 20 ms apart, through the other microphones — a slapback, heard as robotic. Exactly the room (the difference between the two blocks) is dropped from the ring, at once, but only at a quiet moment: below −40 dBFS for the 5 ms before the jump, across everything it passes over and on what it lands on. The jump is then quiet to quiet in the headphones; until such a moment comes nothing moves. −40 dBFS is far below speech and above ordinary room noise between words: measured, background noise up to −50 dBFS RMS still lets the room go within a fraction of a second, where a −50 dBFS threshold was never met by noise at −58 dBFS RMS. Exactly the room, not whatever the level reads above the smaller target at that moment: the level carries a delivery's lateness (a millisecond late moved the channel 49 samples short of the 1092 its place in time said, and its stem 47 samples out of step with the others), and the stems are lined up by place in time (§6.1). The headphone delay the app reports follows the held block, so it falls with it.
  - A device that goes back to a block whose room was given back pays the growth's gap again, so each time it does, its room is kept twice as long before it goes (up to 64 s). Without that, a device another app switched between 1156 and 64 every three seconds paid a gap every six (ten in a minute, enough to step the buffer ladder); with it, it pays two and then none.
  - The headphone delay the app reports — the Advanced panel, the buffer-ladder notice, `measuredLatencyMs` in `session.json` — is the quickest microphone's own path: the output buffers, the cushion its ring holds ahead of the output against drift and late delivery (§3.2: one pull of the output block plus 6 ms, between two and four pulls — 5.3 ms at 64 samples), the drift resampler's two samples (§3.2), what the output device adds after the buffers, and its device's input latency and the IO block it runs at where that is larger than the one asked for. On a rig of like devices that is everyone's. All of these figures come from one sum, so the buffer-ladder notice announces exactly the figure the Advanced panel shows once the rig has been rebuilt at the new size; with no headphones open the notice names no delay. Beside it the app reports the slowest open microphone's own path the same way — the Advanced panel shows it where it differs, the buffer-ladder notice adds it, `session.json` records it as `slowestMicLatencyMs` — and when that microphone is well behind the rest — past the 10 ms ceiling while they are within it, or past it and at least 5 ms behind them — the activity log names it once (and says when that clears). A buffer step that takes the whole rig past the ceiling names no microphone for being a block behind the others: the buffer-ladder notice has said what the rig costs, the slowest microphone included. A slower device is late in its own channel only, but it is never left unsaid. Its other figures are recorded per device in `session.json`.
**Buffer ladder.** Start at 64 samples. Step up through 128, 256, 512 on trigger. Trigger: 3 or more callback overruns within any 30-second window. Never step down automatically during a recording; re-evaluate only on next launch or on device change. Log every step in `session.json`.
Note: everyone is in the same room and already hears each other acoustically at 0 ms. The headphone mix's job is confidence and level-checking, not intelligibility. That lowers the stakes on 8 ms considerably. It does not lower the stakes on 40 ms.
### 5.5 Safety
- **Monitor bus safety limiter, mandatory.** Ceiling −3 dBFS on what reaches the ears, after the master volume. Zero lookahead, instant attack, 10 ms hold, 50 ms release, and a hard clip at the ceiling as the final backstop (it only ever catches rounding). A loose connector or device fault can put full-scale noise into headphones on someone's head. This is a hearing-safety requirement, not a mix decision. It used to be a per-sample hard clip at the ceiling, before the volume: every loud overlap of a few voices was squared off, whatever the volume. Measured at volume 100 with six microphones peaking at −6 dBFS all talking at once, 1937 squared-off samples a minute became 414; a sustained vowel heard by six microphones, which the hard clip held flat a third of the time (distortion 15.1 dB under the sound throughout), now meets the ceiling flat for 12 samples in a minute and is otherwise clean. The price is that at full volume a loud overlap is turned down by a few dB for a fraction of a second. At the default volume none of these reach the ceiling at all.
- **Runaway cut.** If the signal at the ears is over the ceiling — the limiter having to act — for more than 500 ms with no break longer than 1 ms, mute the monitor bus, show the reason, and require a manual unmute. It is timed on what reaches the ears, after the volume, as the limiter is: an overload the volume keeps under the ceiling is not a runaway in anyone's headphones (and feedback building is still caught by the band detector below, at any volume).
- **Feedback protection.** Detect narrowband energy growth: any 1/3-octave band rising more than 10 dB over 500 ms while remaining within 6 dB of the broadband peak. On detection, mute the monitor bus with a visible reason and a one-tap unmute.
- **Global monitor mute** on the spacebar, instantly reversible, with a visible muted state.
- At startup, refuse to route monitor output to a single device that is also a selected input, and explain why. The combined device (§5.2) is the exception.
- Comb filtering from self-monitoring is inherent. Mitigate with low latency; never attempt to correct it with processing.
---
## 6. Recording
### 6.1 What is written
- Discrete per-microphone files at unity gain, plus one summed mix file.
- The mix file is trim + sum + a **mix-bus limiter at −1 dBFS**. This is a separate instance from the monitor safety limiter in §5.5 and is not affected by the runaway cut — a monitor mute must never silence the recording. It is the same kind of limiter (zero lookahead, instant attack, 10 ms hold, 50 ms release, the ceiling guaranteed): a loud overlap is turned down in `MIX.wav` rather than squared off. It used to be a per-sample hard clip, which held four in-phase microphones 2.6 dB over the ceiling flat for 22000 samples a second; now only the first crest's rise, 25 samples, once. Stems never pass through it.
- The mix file is not affected by master monitor volume.
- WAV/BWF. Bit depth per §2.3. Sample rate per §2.2.
- BWF timestamp and a shared session origin in every file, so a DAW aligns stems on import without manual nudging.
- **Stems are lined up on the writer thread, never on the monitor path (§5.4).** The samples the writer receives are each as early as its device allowed, so a clap two microphones heard at once arrives that many samples apart. The writer delays each channel by the slowest device's input latency plus IO block, less its own, through a delay line per channel: the clap lands on the same frame in every stem, and `MIX.wav` is summed from the lined-up stems so it carries the clap once.
  - The leading channels start with that much silence. What a faster channel still holds when the take stops is audio from after the slowest device's last sample, past the aligned end of the take, and is not written: every file holds exactly the frames the take accepted.
  - A device whose IO block grows mid-take moves the alignment once its new size has settled (two deliveries at it: one large delivery can be a driver handing over a backlog, which moves nothing), from the block in which that is seen. When it becomes the slowest, the writer writes silence into every other channel's stems (nothing they handed over is lost, it comes out later); when it is not, the writer takes its own move — the silence its stream put in — back out of its stem. Between its stream moving and its size settling, about one of its blocks, that channel is out of step with the rest by the growth. The writer takes that move out as soon as the stream makes it, while the silence is still at the newest end of the line; if the device's next delivery says the large one was a backlog after all, the writer puts the silence back exactly where it took it from, so a backlog moves nothing in any stem, and none of it is recorded as alignment.
  - A device that gives back room it held for a larger block (§5.4) drops exactly that much from its stream at a quiet moment, from the newest end of what it had buffered, and the writer puts the same amount of silence into its stem exactly there: behind that channel's samples still waiting in its delay line, which come out on time. What was quiet room sound there becomes digital silence, counted in `alignmentSilenceFrames`, and every stem stays in step. Written at the front of the line instead, as any other longer offset is, the silence landed as many samples before the quiet moment as the channel was being held back — inside whatever was being said — and the samples that were waiting came out that much late against every other stem: measured, 1092 samples (23 ms) of digital silence inside a tone in the quicker microphone's track, and the tone ending 1092 samples after the other microphone's. Another device's growth belongs at the front: its stream moved at the moment the oldest waiting sample stands for. When both come in the same block, each part goes where it belongs. The alignment never comes forward during a take for it: had the slowest device's return brought every other stem forward, the writer would have taken that out of their newest audio — cut from every other microphone's track at that moment. So the stems stay lined up to the largest delay the take has seen, and the next take starts from the rig as it stands.
  - Memory is bounded (about half a second a channel) and allocated before the take; nothing is added to the audio thread but handing over each channel's offset when it changes. An offset past the bound is clamped, and the take's record says the stems are not exactly aligned. So does a device whose driver reports an input latency past belief (over 24000 frames, or below zero): the stems are lined up by the bound, which cannot be right either, and the record keeps the driver's own figure beside it.
- Auto-split at 3.9 GB, or write RF64. Split files are numbered `_001`, `_002`, and carry continuous BWF timestamps.
### 6.2 Session structure
A novice losing track of their recording is a total product failure even when the software worked perfectly.
```
/RECORDINGS/2026-08-26_1432_Session/
    session.json
    MIX.wav
    01_Yeti-Kitchen.wav
    02_Yeti-Couch.wav
    03_Yeti-Corner.wav
```
- Folder name: `YYYY-MM-DD_HHMM_<name>`. Default `<name>` is `Session`. User-editable before or after recording.
- File names carry channel number and the user-assigned mic name, never a bare index.
- Sanitize names to `[A-Za-z0-9 _-]`, collapse whitespace to single hyphens, truncate at 40 characters.
- On folder name collision, append `_2`, `_3`, and so on. Never overwrite, never prompt.
- `session.json` contains: app version, start and stop timestamps, sample rate, bit depth, buffer size, measured latency, device list with names and USB IDs, trim values, per-device drift log, buffer-size changes, dropout events, master failover events, and mirror status.
  - And how the stems were lined up (§6.1): per device its input latency (`inputLatencyFrames`, with the driver's own figure as `reportedInputLatencyFrames` where it was past belief and bounded), the IO block it ran at (`ioBlockFrames`), how far its stems sit later than its audio arrived when the take began (`alignmentStartFrames`, the silence each of them opens with) and when it ended (`alignmentDelayFrames`; the two differ only when an IO block grew mid-take; in the record kept current while the take runs, the one a crash leaves, all of these stand as they did when it was last written, the starting offset already the one the stems open with), the silence written into them (`alignmentSilenceFrames`) or samples taken out of them (`alignmentDroppedFrames`) mid-take to keep them in step, and the silence its own stream left in them when its IO block grew by less than its ring's cushion and the writer could not take it back out because the device was, or became, the slowest (`ioShiftFrames`: no audio lost, so not a dropout, but a gap at the same frame as the silence the other stems were given for it); at the top level the slowest input latency (`alignedInputLatencyFrames`) and whether the writer lined everything up exactly (`stemsAligned`). Where it did not, each device's offsets are what to slide its stems by in an editor.
- On stop, show the location and offer to open the containing folder.
### 6.3 Write pipeline
- Lock-free ring buffer in RAM, dedicated writer thread. **No allocation, locking, logging, or file I/O inside the audio callback. Ever.**
- RAM buffer sized for 30 seconds at current channel count and rate, minimum 64 MB.
- **Redundant local mirror**, default on. If internal free space exceeds 2 GB plus the projected session size, write a second copy to the user's home directory in parallel. Converts most card failures from data loss into inconvenience.
- If internal space drops below 1 GB during recording, stop the mirror, keep the card write going, and note it in `session.json`. Do not interrupt the recording and do not show a modal.
### 6.4 Pre-flight
Run on volume selection and again on arming; cache per volume ID with a 30-day expiry.
- A result belongs to the volume it measured, not the path: a different card
  can mount at the same `/Volumes/<name>`. Record the measured volume's identity
  (mount point, filesystem type, device, volume UUID, size) with the result.
  Between takes, the status worker reads the identity at the destination; when
  a look begun after the result shows a different volume, or none, discard the
  result and run the check and the interrupted-take scan again for whatever is
  there. At arming, read the identity once more under the card deadline and
  refuse if it differs.
- Size tells volumes apart only for FAT and ExFAT, whose size is fixed when
  formatted. A ZFS dataset, a network share or a FUSE mount reports a size that
  moves while it stays mounted (ZFS counts the pool's free space), so for those
  a changed size alone never makes it another volume; a different mount point,
  type, device or UUID still does.
- Run the filesystem work away from the window thread. Only one check for the
  current destination may publish; changing destinations abandons the old
  result, and launch, destination changes and shutdown never join the worker.
- Keep recording disabled with a plain-language reason while the current check
  is pending. Failure to start or complete the check is a refusal, never an
  implicit pass.
- Write a 200 MB test file. Measure **sustained minimum throughput over rolling 1-second windows**, not average. Delete the test file.
- Required rate = channels × sample rate × bytes per sample × 2 (card plus mix file overhead).
- **Block arming if sustained minimum is under 2× required.** Refuse before recording starts; **never degrade mid-take.** A novice cannot act on a mid-take warning, and a corrupted three-hour recording is worse than a delayed start.
- Report free space as remaining recording time in hours and minutes, not bytes.
- Detect FAT32, or any filesystem the OS cannot write reliably. Offer reformat to exFAT, state clearly what is erased, require typed confirmation.
### 6.5 Mid-recording events
| Event | Behavior |
|---|---|
| Microphone unplugged | Continue writing silence to that channel. Never change channel count or file layout mid-file. Log the dropout. Meter goes hollow. |
| Microphone reconnected mid-take | Streams are not reopened mid-take, so a mic that left the device list keeps writing silence to its existing channel until the take ends; never report it as recording again. Log the return and state in one line that it rejoins when the user presses Stop, then Record. Restore name and trim per §2.4. |
| New microphone plugged in mid-take | Do **not** add to the in-progress recording, and nothing is opened mid-take, so it is not in the monitor mix yet either. State in one line: "Mic plugged in. It isn't in this take or the headphones yet. It'll be recorded starting with your next take." |
| Clock master unplugged | Failover per §3.3. Recording continues. |
| **Target card removed** | Stop immediately, finalize every open file, alert loudly. If the mirror is running, state that a complete copy survives locally and give its path. |
| Card full | Stop at the last complete buffer block, finalize, report that available time was exhausted. Warn at 10 minutes and 2 minutes remaining. |
| Sustained buffer overrun | Warn visually at 50% ring fill. Never silently drop. At 90% fill, fall back to writing the mix file only on both destinations and log the exact sample position of degradation. A mirror shares the same upstream ring and cannot make overflowing stems safe. |
| Output device disappears | Monitoring stops, recording continues uninterrupted. Re-select per §5.3 and resume monitoring automatically. |
### 6.6 Crash and power loss
- Update WAV headers every 5 seconds so an interrupted file remains playable to within 5 seconds of the cut.
- On launch, scan the last-used card and the enabled mirror directory for
  sessions lacking a stop timestamp. A session record that could not be read
  (it would not open, or delivered fewer bytes than its size, as a cloud-only
  placeholder does offline) is not a missing stop timestamp: leave that folder
  untouched and report it. One that was read and is empty or cut short is
  treated as interrupted. Run both scans away from the window thread;
  they must never delay the first window, a destination change or shutdown.
  Discard an obsolete result if its destination changes while it is running.
- Block recording until every required recovery scan succeeds. Failure to start
  or complete a scan fails closed with a plain-language next step. Once both
  current scans settle, repair headers and present any recovered recordings
  before recording can begin.
- Discard any recovered file containing under 1 second of audio; report it as empty rather than presenting an unplayable stub.
- **Inhibit system sleep, screensaver, and display sleep for the duration of a recording.** A laptop sleeping mid-take is a common and total failure.
- A Mac that sleeps anyway (its lid closed between takes, or on battery mid-take, which no assertion prevents) must not wake to a rig of "unplugged" devices. CoreAudio says nothing when a device stops calling its IOProc, so the backend calls a stream dead after 5 s without a callback; that watchdog runs on the host clock, which stops during sleep. Sleep itself is noticed by CLOCK_MONOTONIC_RAW (which counts through sleep and, unlike the time of day, is never stepped by a time sync) gaining on the host clock between polls, and the watchdog then waits until audio runs on any open device (the Mac fully awake, audio back on: not a dark wake with the lid shut) before giving every device its 5 s again. If nothing runs within a minute it resumes anyway, so a rig that never comes back is still reported. That minute counts only while the Mac says its audio is on (IOKit's system capabilities): a dark wake (Power Nap, a backup, iCloud, with the lid shut) can last minutes with the app polling and CoreAudio stopped, and none of it counts, however long it lasts. The watchdog is held the same way when it is about to call a device dead and the Mac says its audio is off with no sleep seen yet, which is the Mac on its way to sleep. The power manager is asked only then, never on an ordinary poll. Holding the watchdog is not silence about the sleep: once per sleep with the rig open the backend reports it, against no device, and a take running across it gets "The Mac went to sleep: this take has a gap here." in its session.json dropouts at that moment and a warning in activity.log, since its stems join the audio from before the sleep to the audio from after it. No microphone is marked dead and nothing is reopened for it; between takes nothing is said.
- Monitor for thermal throttling and sustained CPU pressure above 80% for 30 seconds; warn before it causes dropouts.
---
## 7. App-facing device strategy

**v1 scope is closed:** macOS publishes a transient aggregate input through
CoreAudio's public aggregate-device API while SobStage is running. Nothing is
installed into the HAL. Windows v1 is the standalone recorder and monitor; it
does not claim to create a system-visible input for another application.

An installed macOS `AudioServerPlugIn` and every Windows virtual endpoint are
post-v1 work. Windows kernel-mode WDM/KS is the native route to a system-visible
audio endpoint and brings separate signing, installer and support obligations,
so it is deliberately outside the recording release's critical path.

Keep future app-facing outputs behind a swappable interface:

| Backend | Reach | Cost |
|---|---|---|
| **A — none.** Standalone recorder + monitor. | n/a | **Windows v1.** Complete without a driver. |
| **B — ASIO output.** User-mode DLL, COM registered. | DAWs and production tools, not ordinary conferencing/browser inputs. | Post-v1 product work. |
| **C — licensed signed virtual cable.** | System-wide endpoint, subject to the licensed product. | Post-v1 commercial dependency. |
| **D — own signed WDM driver.** | System-wide endpoint. | Post-v1 legal, signing, installer and support program. |
- The UI always shows which backend is active and names, in plain language, which applications can and cannot see the aggregate device.
- Do not imply that an unavailable backend is installed, signed or working.
- macOS differentiation is automatic setup, direct-to-card recording,
  always-on shared monitoring and drift telemetry, not an installed driver.
---
## 8. Metering
### 8.1 Behavior
- One meter per microphone, plus one for the shared mix. The mix meter shows the summed headphone bus **before** the master volume, so it reads the same at any listening level; its clip latch counts each time §5.5's safety limiter starts turning the headphones down, not the −0.1 dBFS rule below (the bus can run past full scale harmlessly when the volume is below 100). Reading the signal after the volume, as it did, a bus being squared off at −3 dBFS showed as about −21 dBFS at the default volume, and its CLIP could never light.
- **Meters run from launch, not from record.** No visual mode change when recording starts.
- Each shows instantaneous level, peak hold, clip state, and an always-visible numeric dBFS readout.
- Scale −60 to 0 dBFS, logarithmic.
- Ballistics: 10 ms attack, 1.5 s decay, peak hold 2 s then decay at 20 dB/s. Raw sample values strobe and are unreadable.
- Clip latches with a running count until tapped. Threshold: 3 consecutive samples at or above −0.1 dBFS.
- **Dead-channel detection:** a channel below −60 dBFS for 20 continuous seconds while at least one other channel has exceeded −40 dBFS during the same window. Flag it and name the likely cause.
### 8.2 Implementation
- Audio thread computes only max-abs and RMS per block into a per-channel atomic. Nothing else.
- UI thread polls at 60 Hz. Meter rendering never blocks and is never blocked by the audio callback.
- Metering is lossless with respect to recording: dropping a meter frame is fine, dropping a sample is not.
---
## 9. Visual treatment — channel meters
The signature element. Each microphone is a channel strip whose badge is the app icon's crying face; the badge **is** the meter, not decoration beside one.
### 9.1 Behavior
- Level fills the badge ring from the bottom upward, and the level track beside it from the left. Empty = −60 dBFS, full = 0 dBFS, mapped on the §8.1 scale.
- Peak hold is a 2 px bone-white tick across the level track.
- Clip turns the ring and the readout clip-yellow and the readout says `CLIP n`, latched until tapped.
- The face cries along: one cyan tear at −18 dBFS and above, two on clip. Decoration only — the fill, track and number carry the meaning.
- No signal renders as a hollow outline with closed eyes, the readout `--.-`.
- Mic name and numeric dBFS on each strip.
- The shared mix uses a horizontal bar, not a badge — visually distinct so the bus is never confused with a channel.
### 9.2 Palette
Source of truth: `palette` in `Source/UI/AppLookAndFeel.h`.
| Role | Hex |
|---|---|
| Background | `#0A0E13` |
| Surface (panels, strips) | `#121A23` |
| Surface high (controls, empty well) | `#1C2733` |
| Bone (primary text, ring, peak) | `#E3EAF2` |
| Secondary text | `#8496A8` |
| Tertiary text | `#566372` |
| Accent (record button, focus, tears) | `#22D3EE` |
| Danger (recording, clipping) | `#FF4D5E` |
| Warning | `#FBBF24` |
| Hairline outline | `#27333F` |
| Fill, below −18 dBFS | `#2DD4A7` |
| Fill, −18 to −3 dBFS | `#FACC15` |
| Fill, above −3 dBFS | `#FF4D5E` |
| Clip | `#FDE047` |
| Dimmed / no signal outline | `#4A5B6D` |
### 9.3 Constraints
- **Color never carries meaning alone.** Every colored state also changes shape, text, or number. Clip indication cannot depend on hue.
- Badge legible at 26 px. Test small before committing to detail.
- Respect `prefers-reduced-motion`: flashing and pulse softened or off, fill and numbers still live.
- Numeric readouts in a monospace face so digits do not jitter as values change.
---
## 10. User experience — zero-knowledge setup
Target user has never configured an audio device, does not know what a sample rate or buffer is, and will not read documentation.
### 10.1 First run
- Launches to a working state. No wizard, no account, no device picker.
- Every connected microphone detected and included automatically.
- Rate, depth, buffer, clock master, output device, and backend chosen automatically. The user is never asked.
- Monitoring live the instant the window opens.
- Destination starts at `~/RECORDINGS`; removable-volume discovery runs
  detached and adds connected card choices afterward, stated in one line.
  Free-space and destination-status probes use the same detached, one-flight
  model. A stale mount or filesystem call must never hold the first window,
  destination changes or application shutdown, and obsolete results must not
  replace current state.
- **Permissions are the first real obstacle.** macOS microphone permission, macOS removable-volume access, Windows microphone privacy setting. If any is denied, show what to enable and why, inside the app, without sending the user hunting through system settings unaided.
### 10.2 Main screen
One window. One primary control: a large record button. Everything else is status.
Visible by default:
- Channel-strip row with names and levels
- Record button
- Elapsed time
- Remaining recording time
- Where it saves
- Master headphone volume
- Mute state
**No audio jargon in the default view.** Not "aggregate device," "ASIO," "buffer," "sample rate," "gain staging," "monitor bus." Say microphones, headphones, where it saves, how long you can record.
### 10.3 Advanced panel — exhaustive contents
One door, one click, nothing behind it required for correct operation:
sample rate · bit depth · buffer size · measured latency · clock master selection · per-device drift in PPM · per-microphone trim · output device selection · active app-facing backend and its limitations · local mirror on/off · destination folder · diagnostics export.
### 10.4 Record button behavior
- Idle → press → recording starts immediately, no countdown, no confirmation.
- Recording → press → recording stops immediately, files finalize, location is shown. No confirmation dialog. Stopping is not destructive.
- Disabled when zero microphones are connected, a required recovery scan is
  pending or failed, or pre-flight (§6.4) is pending or failed; the reason and
  applicable next step are shown next to the button.
### 10.5 Physical setup guidance
Novices fail on hardware, not software. Detect and explain, with the fix stated:
- Bus power exhausted → a hub **with its own wall adapter** is needed, and why.
- Card filesystem wrong → offer reformat to exFAT, state what is erased.
- Card too slow → state measured speed, required speed, and the two ways out.
- Headphones plugged into a microphone with no combined device available (Windows, Linux) → redirect to the headphone amp (§5.3).
- Only one headphone output for several listeners → a splitter or amp is required.
- **Microphone muted at the hardware switch** → the single most common failure. Name it explicitly whenever a channel reads silence.
- **Non-cardioid polar pattern** → see §14.4.
### 10.6 Language
- Errors name what happened, then what to do, in one sentence. No codes, no apologies, no vagueness.
  - Good: "Mic 4 isn't sending sound. Check the mute button on the mic."
  - Bad: "Input stream error (0x8007001F)."
- Buttons say what happens. "Start recording" → "Recording." "Stop recording" → a file, and where it went.
- Technical detail is never hidden from someone who wants it, and never shown to someone who doesn't. That is what the Advanced panel is for.
### 10.7 Acceptance test
Hand the app and a bag of microphones to someone who has never recorded audio. They must reach a finished recording on a card, with everyone hearing everyone, without asking a question or reading anything. **Every question they ask is a bug in the interface, not a gap in the documentation.**
---
## 11. Engineering constraints
- Suggested stack: C++ with JUCE, or PortAudio/miniaudio. CoreAudio on macOS;
  WASAPI exclusive on Windows v1. Keep ASIO isolated as post-v1 work.
- Real-time thread does no allocation, locking, logging, file I/O, or drawing. Ever.
- A macOS input HAL open may hold the UI for at most five seconds. After that
  deadline, the worker retains and cleans up its stream without being joined by
  launch or shutdown. Close callback admission and drain all admitted IOProc and
  property-listener callbacks before releasing their owner.
- Removable-volume discovery, filesystem status polling, destination preflight
  and interrupted-take recovery must not retain an application/UI owner and are
  never joined by launch, destination changes or shutdown. Publish only results
  that still belong to the current destination, and fail the recording gate
  closed when a required check cannot produce a result.
- Signed and notarized macOS build. Authenticode-signed Windows portable ZIP.
- **Diagnostics export**: one button writes a zip containing app logs, the last five `session.json` files, and a device inventory. Never includes audio. This is how support requests are handled without screen-sharing.
- **No automatic crash upload in v1.** Recovery, local logs and a user-initiated
  diagnostics export are the supported failure path. Nothing leaves the
  computer unless the user reviews and shares it.
- **No automatic updater in v1.** Tagged releases and checksums are the
  supported update channel. A user downloads and installs an update explicitly;
  SobStage never changes itself during a recording.
---
## 12. Validation matrix
Test before release, not after.
- **Microphones:** at least six models across price tiers, including one that presents as stereo-with-silent-right and one with a hardware mute switch.
- **Counts:** 1, 2, 4, and 8 simultaneous microphones, plus a 9-mic case to confirm §1 behavior.
- **Duration:** one 4-hour continuous take at maximum channel count, passing §3.4.
- **Cards:** one fast card and one deliberately slow card that fails pre-flight.
- **Latency:** measured loopback figure on macOS CoreAudio and Windows WASAPI
  exclusive. Confirm the 10 ms ceiling holds on the slowest supported
  configuration; add an ASIO result only if a real ASIO path later ships.
- **Hostile events:** unplug a mic mid-take · reconnect it mid-take · unplug the clock master mid-take · pull the card mid-take · force-quit mid-take · cut power mid-take · exhaust bus power mid-take · disconnect the output device mid-take · fill the card mid-take.
- **OS:** current and previous major versions of macOS and Windows 11, plus the §1 minimums.
---
## 13. Build order
1. Multi-device capture with drift compensation. Gated on §3.4.
2. Direct-to-card write pipeline with throughput benchmarking and the §12 hostile-event matrix.
3. Shared monitor bus with limiter, mute, and feedback protection. Gated on the §5.4 latency ceiling.
4. Metering.
5. Transient macOS aggregate input; keep post-v1 virtual backends isolated.
6. UI and the zero-knowledge setup flow.
---
## 14. Hardware profile — Blue Yeti
Verified: 16-bit / 48 kHz, 5 V / 150 mA, four polar patterns, 3.5 mm headphone jack with analog direct monitoring, hardware mute button, analog gain knob. Confirm on a bench unit; Yeti X and Yeti Nano differ from the standard Yeti.
### 14.1 Format
- **Fixed at 16-bit / 48 kHz.** A 24-bit default is unreachable with this hardware. Bit depth follows §2.3.
- An all-Yeti rig lands on 48 kHz with no inter-device resampling needed, which relaxes the drift budget. Drift compensation is still required — separate crystals, separate clocks — but the ASRC ratio stays near unity.
### 14.2 Power and USB topology
Yeti draws 150 mA. USB 2.0 port budget is 500 mA; USB 3.0 is 900 mA.
**Acceptable:** one Yeti per dedicated computer port, up to the machine's port count · any number of Yetis on a hub with its own AC adapter.
**Not acceptable:** three or more Yetis behind a bus-powered hub · four Yetis sharing one USB 2.0 port by any means.
**"Powered hub" means a hub with a wall adapter.** A hub that only plugs into the computer adds ports, not power. The two products look identical on a shelf and the distinction is invisible to a novice — first-run guidance must state it in exactly those words. Recommend a powered USB 3.0 hub rated at least 2 A total.
**Detection:** bus power exhaustion does not produce a clean error. It produces failed enumeration, intermittent dropouts, and devices disappearing mid-session. On two or more enumeration failures or device drops within 5 minutes with three or more mics attached, state the cause directly: "Your microphones need more power than this computer's USB port can supply. Use a USB hub with its own power adapter."
### 14.3 Controller contention — separate issue from power
Most laptops route all USB ports to one controller. Sustained SD card writes on the same controller as four isochronous audio streams cause dropouts that present as an application fault.
- Prefer a built-in SD slot over a USB card reader.
- If a USB reader is used, place it on a different controller from the microphones where the OS exposes controller topology.
- Detect the co-located case and warn **before arming**, not during: "Your card reader and microphones share one USB connection. Use the built-in card slot if you have one."
### 14.4 Polar pattern — the sleeper failure
- The pattern knob is physical with no software readback. Omni, stereo, and bidirectional capture the whole room.
- Multiple Yetis in omni or stereo in one room produce heavy bleed, unusable stems, inter-channel comb filtering, and elevated feedback risk.
- **Every microphone must be set to cardioid for multi-person recording.**
- **Detection:** correlation above 0.6 between two channels sustained for 10 seconds, while a third channel is below −45 dBFS, indicates at least one non-cardioid mic. Name the mic, name the knob, state the setting. Do not say "bleed" — the word means nothing to the target user. Say: "Mic 2 is picking up the whole room. Turn its pattern knob to the single-heart setting."
### 14.5 Headphone jack
- The standard Yeti's analog direct monitoring **cannot be disabled** — there is no blend control, and the direct signal is always present at the jack.
- §5.2 accepts this: on the default macOS topology a Yeti listener hears their own voice twice (0 ms analog plus the mix). Their jack can be switched off in Settings.
### 14.6 Identical device names — first-run blocker
Four Yetis enumerate with the same product string. A novice cannot tell which strip is which person.
- Disambiguate internally per §2.4.
- **Tap-to-name flow:** prompt the user to tap or speak into one microphone. Detect which channel exceeds −25 dBFS while all others stay below −45 dBFS for 300 ms, highlight that strip, and let them type a name. Repeat per mic. If two channels trigger together, say "Two mics heard that — try tapping closer to one" and retry.
- Takes seconds, requires no technical understanding, and works without headphones plugged into the mics.
- Names persist per §2.4 so the second session does not repeat setup.
### 14.7 Room reality
Yetis are sensitive condensers. Several open in one untreated room will each capture every voice. Stems will not be clean isolation. This is a hardware limit, not a software defect — state it in product documentation rather than letting users conclude the app is broken.
---
## 15. Out of scope for v1
Stated so they are not assumed:
- Timecode, word clock, or sync to external video.
- Networked or remote microphones.
- Post-production editing, effects, or noise reduction.
- Mobile.
- Localization beyond English.
- Automatic crash uploads and automatic updating. v1 is local-first and uses
  reviewed diagnostics plus explicit, checksum-verified downloads.
- An installed macOS HAL plugin or a Windows system-visible virtual input.
- **More than one monitor mix.** Deliberate product decision, not a limitation to be fixed later.
- **Zero-latency monitoring.** Physically unavailable given a single shared mix from USB microphones. The 10 ms ceiling in §5.4 is the commitment.
---
## 16. Closed decisions
These were resolved deliberately. They look like oversights and are not. Do not reopen them without raising the trade-off explicitly.
| Decision | Why |
|---|---|
| Trim affects the mix file but not the stems | A novice's bad gain decision must not be permanent. The editor gets clean raw material regardless. |
| No mix-minus, no per-listener feeds | Everyone hears the identical mix. This was chosen over zero-latency self-monitoring, knowingly. |
| Microphone headphone jacks carry the shared mix (macOS default) | Chosen knowingly despite the doubled voice from non-defeatable direct monitoring (§14.5); each jack can be switched off, never given a different mix. |
| Shared-mode WASAPI disqualified | 40–100 ms makes own-voice monitoring an audible echo. The product fails at that latency. |
| Pre-flight blocks rather than degrading mid-take | A novice cannot act on a mid-take warning. A delayed start beats a corrupted three-hour recording. |
| New mics join monitoring but not the in-progress take | Channel count cannot change inside a WAV file. |
| Unplugged mics write silence rather than shrinking the file | Same reason. File layout is fixed at record start. |
| Local mirror on by default | For this user, silent data loss is worse than a full internal drive. |
| Bit depth follows the device | Upconverting 16-bit Yeti audio to 24-bit adds size and no information. |
| Monitor mute never silences the recording | Separate limiter instances exist specifically to guarantee this. |
| 8-microphone ceiling | Above that, bus power, controller bandwidth, and room bleed all fail before the software does. |
