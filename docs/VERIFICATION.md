# Verification status

Exactly what has been verified and how: compiled, executed against simulation, executed against a real OS audio API, or run on physical hardware. Also the design judgment calls and build-order status against the specification. Back to the [README](../README.md).

## Honest limits

This page enumerates what has been verified and what has not, at the
granularity of "compiled" vs "executed" vs "run against real hardware". A
PUPGSIS T12S has been detected on a real Mac at its fixed 44.1 kHz rate, but a
completed take from physical microphones has not yet passed the release matrix
on any platform. That is stated here, in the release checklist, and in the
[platform table](INSTALLING.md#what-to-expect-on-your-platform).

> **Read this page before running SobStage on anything you care about.** The
> engine is extensively tested, and the macOS and Windows audio backends
> execute against simulated CoreAudio and WASAPI device layers. Real-driver
> timing, multi-device hardware behavior and full-take validation remain
> release gates. See [`RELEASE_CHECKLIST.md`](../RELEASE_CHECKLIST.md).

## Implemented and verified

All of `Source/Core` plus the platform-neutral Linux input policy, covered by
950 unit tests passing in CI on Linux, macOS
and Windows. The table below lists the largest areas rather than every file:

| Area | Spec | Tests |
|---|---|---|
| `MonitorBus` — sum, trim, master volume ahead of the −3 dBFS safety limiter, runaway cut, feedback protection | §5 | 26 |
| `PeakLimiter` / `MixBusLimiter` — zero-lookahead gain-riding limiter (instant attack, 20 ms hold, 50 ms release) for the headphones and `MIX.wav` | §5.5, §6.1 | 10 |
| `RecordingEngine` — mid-take unplug/reconnect/new-mic events | §6.5 | 15 |
| `PreflightThroughputTest` — rolling-minimum throughput, 2x gate, FAT32 | §6.4 | 22 |
| `SessionFolderNaming` — sanitization, truncation, collision suffixes | §6.2 | 11 |
| `DriftCompensator` — PI loop, ±200 PPM clamp, 20 PPM/s slew | §3.2 | 13 |
| `DeviceInputStream` — per-device ring, drift loop, resampler onto the pulling clock | §3.2, §3.3 | 29 |
| `AlsaBackend` — real Linux audio: enumeration, exclusive-mode gate, capture, inotify hotplug | §2, §5.4, §11 | `live_capture` |
| `AlsaInputPolicy` — fail-closed removable-hardware selection | §2 | 13 |
| `DeviceManager` — 8-mic cap, 9th exclusion, master selection and failover | §1, §3.1, §3.3 | 18 |
| `RingBuffer` — lock-free SPSC, 30s / 64 MB minimum sizing | §6.3 | 9 |
| `Metering` — ballistics, peak hold, clip latch, the mix bar's level at the headphone volume and limiter-driven CLIP | §8.1 | 15 |
| `SessionWriter` — RIFF/WAVE headers, auto-split, periodic header rewrite | §6.1, §6.6 | 16 |
| `SampleRateNegotiator` — highest common rate capped at 48 kHz | §2.2 | 14 |
| `PolarPatternDetector` — non-cardioid detection | §14.4 | 5 |
| `ChannelLayoutAnalyzer` — mono collapse rules, 60 s timeout | §2.1 | 12 |
| `DeadChannelDetector` — silence against an active reference channel | §8.1 | 5 |
| `SessionMetadata` + JSON | §6.2 | 9 |

## Executed, not just compiled

`Tools/e2e_capture` drives the real capture path with synthetic audio — two
mics on mismatched clocks, recorded to disk, then the WAVs are decoded and
checked by Goertzel that each stem holds its own microphone's tone and the mix
holds both. `Tools/soak_drift` is the §3.4 gate above. Neither is a unit test:
they take minutes, and they answer questions unit tests cannot.

Both found real bugs that the unit-test suite did not:

- **Integral windup.** The PI loop's integral saturated at the ±200 PPM clamp
  long before the deliberately slow 5 PPM/s slew could deliver it, so every
  crossing had to unwind from saturation. Over hours the loop swung between
  +140 and −45 PPM instead of settling, starved rings, and failed §3.4 at
  3.04 ms. Integration then paused whenever the output was rate- or
  clamp-limited -- which, with a level carrying a sample of jitter, was every
  block, so the integral never moved and the loop ran proportional-only.
  It now pauses only when the error would push the integral further the way
  the output is already limited.
- **Pre-roll sized from ring capacity.** Playout started at half the ring, which
  meant 1024 samples — **21 ms of monitor latency**, on its own more than twice
  the entire §5.4 budget. Pre-roll is now a fixed two blocks, and the monitor
  path measures 5.33 ms end to end against the 10 ms ceiling.

A third, smaller one: the output clock starts before any device has delivered,
so the first pull of every take underran. That is normal startup rather than
lost audio, and counting it made the §0.1 metric untrustworthy.

The fourth was the one the soak could not see. `Tools/soak_drift` feeds each
microphone's drift as an extra sample slipped into a block, so the ring level
moves one sample at a time; a real device delivers whole blocks on its own
clock, so the level a pull sees moves in steps of a block, and against that
the §3.2 loop limit-cycled — a device 60 PPM slow never converged, a device
150 PPM slow dropped a block of audio every few seconds, and every microphone
in the rig was reported at +200 PPM. `Tools/sim_drift_loop` models delivery
the real way, deterministically, and is now the test: five clocks at ±150,
±60 and 0 PPM lock with no loss, at every rung of the buffer ladder with the
jitter that rung is meant to absorb. Without jitter the loop sits within
1 PPM RMS of each clock over the last minute; with it, it is held to 20 PPM
RMS over the last minute rather than at the last instant, since under a
rung's stalls its figure moves with each one and a single reading of it is
luck. `Tools/e2e_realtime_mics.sh`
then does the same through the real app, with the virtual microphones paced
to real, independent clocks: the take is checked, the ladder is checked, and
the drift the app reports is checked against the clocks it was given.

## Exercised against a real OS audio API

`Source/Platform/AlsaBackend.cpp` is a real Linux backend on ALSA, and
`Tools/live_capture` drives it: ALSA opens the devices, libasound delivers the
audio on threads the backend creates, and the harness checks what comes out.
`Tools/setup_alsa_fixture.sh` builds file-backed virtual microphones carrying
known tones, so this runs on a machine with no sound hardware. The fixture is
available only in a separately compiled test build; the production binary's
positive hardware policy rejects virtual inputs.

Measured, five runs identical: each device delivers its own tone at 0.2000
magnitude with 0.0001 leakage of the other — a 2000:1 separation — and §5.4
correctly refuses a shared (`default`) output by name.

This does **not** make CoreAudio or WASAPI verified; those are different APIs.
What it retires is the broader claim that `IAudioBackend`'s contract had never
met a real audio system: it has, and it holds. Two honest limits of the fixture:
ALSA's `file` plugin delivers as fast as it is read rather than at 48 kHz, so
the timing is not real-time and the capture ring floods (which is why the
full-stack layer asserts frame-locked files and signal, not per-stem tone
coherence); and it cannot refuse a sample format, so the fixture must be
written in whatever format the backend negotiates.

## Executed against simulated CoreAudio and WASAPI

`CoreAudioBackend.cpp` and `WasapiAsioBackend.cpp` can only be compiled on their
own OS, so on every other machine they were unverified by construction — which
is how five user-facing defects lived in them undetected, including a stereo USB
microphone recording silence on macOS and a 16- or 24-bit microphone refusing to
open at all on Windows.

`Simulation/` closes that gap. It supplies stand-in OS headers — the ~10
CoreAudio calls and ~30 WASAPI symbols these two files actually use — behind a
configurable virtual device layer. `Tools/sim_coreaudio` and `Tools/sim_wasapi`
then compile **the backend sources unmodified** against those headers and drive
them. The code under test is the code that ships; only the operating system
underneath it is fake.

The devices are configured to be awkward on purpose, because the ideal case was
never what failed:

| Simulated | CoreAudio | WASAPI |
|---|---|---|
| Buffer shapes | interleaved and one-channel-per-buffer, input and output | interleaved, in every accepted wire format |
| Formats | continuous and discrete sample-rate ranges | float32, 32-, 24- and 16-bit PCM; devices accepting only one |
| Exclusivity | hog mode granted, denied, and held by another process | exclusive-only; a shared-mode request fails the simulation outright |
| Refusals | a rate the device cannot reach; a rate it already holds | a rejected period the device renames; a device that accepts nothing |
| Hotplug | `kAudioHardwarePropertyDevices` listener | registered `IMMNotificationClient` |
| Scale | eight interleaved stereo mics at the §1 ceiling | eight mics at once in four different wire formats |

The current baseline is 239 CoreAudio checks and 131 WASAPI checks, run by `ctest`
on Linux, macOS and Windows alike. The WASAPI backend's worker thread is a real
thread doing a real event handshake, so that path is exercised rather than
reasoned about. Both simulators run under AddressSanitizer,
UndefinedBehaviorSanitizer and ThreadSanitizer.
The release checklist requires recording the final counts if the candidate gains
another check during hardening.

The CoreAudio simulator includes a start call that returns only after the UI
deadline, including a callback delivered before that late return. Input open
stops waiting after five seconds in production; the detached worker retains the
stream, serializes its cleanup, and prevents duplicate opens from stacking on
the same device. A single admission-and-lease gate covers the IOProc and all
property listeners, so teardown can close admission and drain every callback
before application-owned state is released. The same harness retains listener
client data deliberately and verifies that SobStage keeps the inert stream
quarantined instead of creating a use-after-free or unsafe retry.

Input and output HAL opens and closes use bounded ownership paths. The simulator
stalls rate, buffer, hog-mode, IOProc create/start/stop/destroy and listener
operations, including late callbacks and failed cleanup-worker construction.
That liveness evidence is still not a substitute for the physical audio matrix.

**Whether the simulation is worth anything was checked by breaking things.** Each
of the five shipped defects was re-introduced, plus four more (a dropped
`AUDCLNT_BUFFERFLAGS_SILENT` check, PCM writes that wrap instead of clip, a
removed buffer-alignment retry, a removed hotplug registration). All nine turned
the harnesses red:

| Re-introduced defect | Checks failed |
|---|---|
| CoreAudio IOProc skips non-mono input buffers | 6 |
| CoreAudio reads only the maximum of a rate range | 1 |
| CoreAudio proceeds when hog mode is denied | 3 |
| CoreAudio treats an already-correct rate as fatal | 2 |
| WASAPI offers float32 only | 28 |
| WASAPI ignores the SILENT flag | 1 |
| WASAPI PCM writes wrap instead of clipping | 1 |
| WASAPI drops the buffer-alignment retry | 2 |
| WASAPI hotplug registration removed | 2 |

Writing the simulation also found a bug in the simulation itself, which is worth
recording because it is the failure mode this whole approach risks: `HRESULT`
was first typed as `long`, which is 64-bit on Linux, so every `0x8889xxxx` error
code came out positive and `FAILED()` read every WASAPI failure as success. The
harness caught it as fifteen red checks rather than passing silently.

**What this does not establish** is behaviour against a real driver — its
timing, firmware quirks or scheduling. Simulation checks the backend against the
API shapes represented by the fake; it cannot certify either an unmodelled OS
behavior or a particular piece of hardware. Real-driver and device behavior on
all three platforms remains in the physical matrix. See [What to expect on your
platform](INSTALLING.md#what-to-expect-on-your-platform).

## Compiled and rendered

The full application builds and links in CI on Linux, macOS and Windows, so
`Source/UI` is not unverified code either:

- `ChannelMeterComponent`, `MixBarComponent`, `MainScreen`, `AdvancedPanel`,
  `CameraPanel`, `ModalCard`, `SaveLocationPrompt`, `SavedTakePanel`,
  `MainComponent`, `Main.cpp` — JUCE components using the §9.2 palette.
- `CameraController` compiles twice: once as it ships (camera path compiled out
  on Linux) and once with `JUCE_USE_CAMERA=1` against `Simulation/Camera`'s
  stand-in `juce_video`, via the `sim_camera` target. That simulator executes
  340 checks covering enumeration, selection, arrival/removal, open
  failure/retry, a list reorder during open, actual-frame gating and loss,
  native-viewer lifetime and reparenting, runtime-error recovery, an enabled
  capture card missing from the OS list, recording-start truth and asynchronous
  movie finalization and start refusal. A separate seven-check synchronous
  lifecycle probe covers DirectShow start and finish callback ordering. It
  verifies that an interrupted camera
  stays out for the rest of that take and its remembered preview reopens only
  afterwards; final-candidate AVFoundation/DirectShow open, visibly non-black
  preview and recording still need macOS or Windows hardware.
- `RecoveredTakesPanel` has been rendered against a real interrupted take:
  a session folder with no stop timestamp and four WAVs whose size fields were
  zeroed, as a SIGKILL leaves them. The app found it at launch, repaired all
  four headers, and offered the three that held real audio while reporting the
  0.4-second one as empty. The repaired files were then confirmed playable by
  a decoder outside this project.

The app has also been driven headless under Xvfb against the virtual ALSA
microphones, through a whole take: press record, answer the save-location card,
watch the file count and total climb on the main screen, stop, and read the
saved-take panel listing every file that was written with its size. Pressing
record a second time started immediately with no card, into a `_2` folder — so
"asked once, then never again" is a checked claim rather than an intended one.

What that does **not** cover is a successful physical-camera workflow. An older
v1.11.0 build opened a USB HDMI capture device and AVFoundation logged a
first-frame enqueue, but that run did not verify a visibly non-black SobStage
preview or a completed recording. The simulator drives the
`CameraDevice::openDevice` boundary, including failures and a hot-plug reorder;
the v1.13.26 AVFoundation/DirectShow viewer and `startRecordingToFile`
paths still need the physical macOS and Windows matrix. See *Not yet validated
against hardware*.

## Hardware signals and safeguards

The app treats unavailable platform evidence as unknown rather than inventing a
warning. CPU pressure is measured from the audio callback's own deadline usage;
platform thermal and USB-controller evidence is reported only where the OS can
provide it. The release matrix still verifies those paths on the supported
hardware rather than treating a simulator as proof.

- **Thermal throttling** (§6.6). On macOS, `SystemThermalState` reads
  `NSProcessInfo.thermalState`; serious and critical states feed the warning.
  Windows and Linux currently report unknown and retain the callback-deadline
  pressure check rather than guessing from an unsupported platform signal.
- **USB host-controller topology** (§14.3). `ControllerContentionDetector`
  treats unknown topology as unjudgeable and stays silent. No current backend
  populates a dependable controller ID; implementing that requires a separate
  OS-registry mapping and physical validation.

## Deliberately stubbed

Virtual device backends per §7 — with one carve-out that ships: on macOS the
combined device needs no driver at all, because CoreAudio's public
`AudioHardwareCreateAggregateDevice` API publishes a system-wide aggregate
(`Source/Platform/MacSystemAggregateDevice.cpp`). Other apps see one named
multi-channel input containing the eligible external inputs, with per-sub-device drift
compensation handled by the HAL. What remains stubbed is Windows, and the
§7 "virtual cable carrying the summed mix" use case; each is gated on
something that cannot be obtained from source code:

| Backend | State | Blocked on |
|---|---|---|
| A — none (standalone recorder + monitor) | Implemented (`NullBackend`) | nothing |
| B — ASIO output DLL | Interface + stub | ASIO SDK, COM registration |
| C — licensed signed virtual cable | Interface + stub | commercial per-seat license (VB-Audio / VAC / Thesycon) |
| D — own attestation-signed WDM driver | Interface + stub | registered legal entity, EV certificate, Partner Center |

## Release-owner gates

These release gates require credentials, hardware or distribution operations
outside the source tree:

- Add the Apple Developer ID/notarization and Windows Authenticode credentials
  named in `RELEASE_CHECKLIST.md`; the release workflow ships ad-hoc/unsigned
  builds without them and verifies signatures again after packaging once they
  exist.
- Complete owner/legal/privacy/licensing sign-off and the physical matrix.

An installed macOS HAL plugin, Windows virtual input, automatic crash upload
and automatic updating are explicitly outside v1. The shipped macOS combined
input is transient and uses CoreAudio's public aggregate-device API; Windows v1
is the standalone recorder and monitor.

## Not yet validated against hardware

The entire §12 validation matrix is outstanding. A PUPGSIS T12S has enumerated
on a real Mac at 44.1 kHz, but no completed physical-microphone take has passed
the matrix. In particular:

- **§3.4 passes against simulated clocks, and only those.** `Tools/soak_drift`
  runs four dissimilar clocks (+40 / +100 / −80 / +45 PPM) for four hours at
  both 44.1 and 48 kHz. The current results are **1 sample = 0.023 ms** at
  44.1 kHz and **2 samples = 0.042 ms** at 48 kHz against the 1 ms ceiling,
  with zero underruns. Simulated offsets are steady, though; real crystals
  wander with temperature and load, so the hardware run is still owed. What this does
  retire is the question of whether the *software* holds alignment — it does,
  and it did not before the two bugs above were found.
- **§5.4 latency ceiling** — the 10 ms ceiling must be confirmed by loopback on
  macOS CoreAudio and Windows WASAPI exclusive. Add ASIO only if a real ASIO
  path later ships.
- Hostile-event matrix, card throughput on real slow media, bus-power
  exhaustion, and the §10.7 novice acceptance test.
- **The card-removal path is proven at the pipeline, not in the running app.**
  `WritePipeline` noticing a failed write is tested against a real failing
  write — the process's maximum file size is capped so the write returns EFBIG,
  which is what a departed card looks like from inside `write()`. What has not
  been exercised is the whole path in the app: the virtual-microphone rig here
  produces no audio, so no bytes are written and no write can fail. Stopping
  the take, finalizing, and showing the alert are wired to that flag and each
  tested or exercised separately, but the four together need a real card to
  pull out.
- **No physical camera has completed the candidate workflow.** `sim_camera`
  executes discovery, selection, arrival/removal, open failure/retry, native
  viewer lifetime/reparenting, runtime-error recovery, a topology reorder
  during open, and the no-false-recovery policy for a mid-take unplug/replug.
  Separately, an older v1.11.0 build opened `USB2 Video` and AVFoundation logged
  a first-frame enqueue; that did not verify a visible non-black preview or a
  completed recording, and the v1.13.26 artifacts remain untested with a
  physical capture card.
  Outstanding on real hardware:
  what resolution `openDevice` actually settles on, what the recorded file
  costs per second against the estimate the remaining-time figure uses
  (`CameraSelection::kEstimatedVideoBytesPerSecond`, deliberately pessimistic
  at ~64 Mbit/s, sized for 4K), whether two cameras can be held open at once on a given
  machine, and whether recording video alongside eight microphones stays
  within the §6.6 CPU budget.

## Judgment calls

- **No JUCE in `Source/Core`.** The spec does not require this, but without it
  nothing could be tested in an environment without JUCE, and §13 orders drift
  compensation first — before any UI exists to host it.
- **Hand-rolled test framework** (`Tests/TestFramework.h`) instead of Catch2,
  and a small hand-rolled JSON writer/parser (`Source/Core/Json.h`) instead of
  nlohmann. Both avoid a network fetch in the build. Either can be swapped for
  the mainstream library later; the JSON one is used for `session.json`, `settings.json` and the activity journal.
- **`MMA_BUILD_APP` defaults to `OFF`** so that `cmake -B build && cmake --build
  build` succeeds on any machine. Turn it on for real platform builds.
- Backends B/C/D return an explicit unavailable status rather than pretending to
  work, so §7's requirement that the UI names which applications can and cannot
  see the aggregate device stays truthful.
- **One question before the first take, and none after that.** §10.4 says a
  record press starts immediately with no confirmation, and it is right: a
  dialog on every press is friction on the one control that matters. But §6.2
  says a novice losing track of their recording is a total product failure, and
  the app was relying on a 12px grey line to prevent it. The reading taken here
  is that §10.4 forbids *confirming the act of recording*, not *telling someone
  where their files will be* — so the card is shown at most once per
  destination, before the first take against it, and the answer is remembered
  against the folder it was given about. Every press after that goes straight
  to recording. A user who wants it every time can ask for that on the card.
- **A recovered stub is reported, not deleted.** §6.6 says to "discard any
  recovered file containing under 1 second of audio; report it as empty rather
  than presenting an unplayable stub." The reporting half is taken literally;
  the discarding half is not. Silently removing a file from someone's card at
  launch, before they have seen it or asked for anything, is a worse mistake
  than listing a short file — so a stub is excluded from what is offered and
  left exactly where it is.
- **The listening level is the one setting not written when it changes.**
  Everything else that is remembered goes to disk the moment it changes, so a
  crash cannot cost it. Master volume is written only at shutdown: it is the
  one control that moves continuously while someone listens, and it is comfort
  rather than setup — losing it costs a second to reset, where losing a trim
  costs the ear-work that found it.
- **Cameras are an addition, not a spec item.** `docs/SPEC.md` is about
  microphones and says nothing about video, so everything in the Cameras panel
  is a judgment call against the spec's own principles rather than a
  requirement being met: every plugged-in camera recording by default because
  a take with a camera missing from it cannot be redone, while §6.5's
  card-full failure is guarded by the remaining-time figure counting every
  recording camera; capture
  requests JUCE's high-quality capture mode while only the *drawing* is made
  cheap (the OS/driver still chooses the actual format), because §6.6 is
  about not spending CPU where it costs audio; picture and sound as separate
  files, because §6.1's whole premise is one clean track per person and a
  camera's own microphone would put a room mic into that. Nothing about the
  camera path can affect the audio path — it is opened, recorded and closed
  entirely outside the audio callback.


## Build order

Per §13, and where this repository sits against it:

1. Multi-device capture with drift compensation — **engine written, gated on the
   §3.4 hardware measurement**
2. Direct-to-card write pipeline with throughput benchmarking — **written and
   unit-tested, hostile-event matrix outstanding**
3. Shared monitor bus with limiter, mute, feedback protection — **written and
   unit-tested, gated on the §5.4 latency measurement**
4. Metering — **written and unit-tested**
5. Virtual device backends — **A implemented and compiled on all three platforms, B/C/D stubbed per above**
6. UI and zero-knowledge setup flow — **builds in CI on all three platforms and runs headless; T12S enumeration confirmed, completed hardware workflow outstanding**
