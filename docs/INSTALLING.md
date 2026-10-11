# Installing SobStage

Downloads, checksum verification, per-platform installation, what each platform has and has not been validated against, and where SobStage keeps its files. Back to the [README](../README.md).

## Download

**Latest release: [the Releases page](https://github.com/taylordrew4u2/usbmic/releases/latest).**
[`CHANGELOG.md`](../CHANGELOG.md) lists what changed in each one and what is still
missing.

The source currently describes the **v1.13.25 release candidate**. It is not a
general-release claim: signing, artifact inspection and physical-hardware gates
are tracked in [`RELEASE_CHECKLIST.md`](../RELEASE_CHECKLIST.md). For help, see
[`SUPPORT.md`](../SUPPORT.md); data handling is documented in
[`PRIVACY.md`](../PRIVACY.md).

Builds for macOS, Windows and Linux are produced by the
[Release workflow](../.github/workflows/release.yml):

- **macOS** — `SobStage-macOS.dmg`, a normal drag-to-install disk
  image: mount it and drag the app onto the Applications alias beside it.
  `SobStage-macOS.zip` carries the same `.app` for anyone who would
  rather not mount an image. Needs macOS 13 (Ventura) or later; one universal
  app runs natively on Apple silicon and Intel Macs.
- **Windows and Linux** — `SobStage-Windows.zip` and
  `-Linux.zip`.
- **Tagged releases** — all of the above are attached to the
  [Releases page](https://github.com/taylordrew4u2/usbmic/releases). Start here; this is the supported download.
- **Any commit** — run the Release workflow from the Actions tab
  (`workflow_dispatch`) and download the artifacts it uploads. It uses the same
  source revision and package layout, but an untagged rehearsal may be
  ad-hoc/unsigned. Artifacts are wrapped in an extra `.zip` by GitHub and expire
  after 90 days, so prefer a release unless you specifically need an untagged
  commit.

Each archive and the disk image contain the application, this README, `LICENSE`,
`LICENSING.md`, `SUPPORT.md`, `PRIVACY.md` and the release checklist.
Tagged releases also carry `SobStage-<version>-source.zip`, containing the exact
SobStage and pinned JUCE source revisions, and `SHA256SUMS` covering every
download.

To check one downloaded file before opening it, keep it beside `SHA256SUMS` and
run the command for your system (replace the filename when checking another
asset):

```sh
# macOS
grep ' SobStage-macOS.dmg$' SHA256SUMS | shasum -a 256 -c -

# Linux
grep ' SobStage-Linux.zip$' SHA256SUMS | sha256sum -c -
```

On Windows, PowerShell can compare the published and measured values directly:

```powershell
$expected = ((Select-String ' SobStage-Windows.zip$' SHA256SUMS).Line -split '\s+')[0]
$actual = (Get-FileHash SobStage-Windows.zip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw 'SobStage-Windows.zip checksum does not match' }
```

The workflow signs and notarizes the macOS app and Authenticode-signs the
Windows executable when the repository carries those credentials. Without them
the macOS app is ad-hoc signed and the Windows executable unsigned, and the
workflow log labels the build that way. Current releases are unsigned.
[Installing → macOS](#macos) explains the one extra step an ad-hoc build needs;
a notarized release does not need it.

The Blue Yeti in the spec is reference hardware only. Recording-input discovery
uses a positive external-hardware rule and fails closed rather than guessing:

- macOS admits directly attached USB, FireWire and Thunderbolt transports;
- Windows admits USB, FireWire and Thunderbolt device-tree branches only when
  Windows also marks the hardware or one of its ancestors removable;
- Linux admits kernel ALSA cards whose sysfs ancestry says they are removable.

The computer's own microphone, known phone/Continuity transports,
Bluetooth/AirPlay, network, aggregate, virtual, internal and unknown inputs are
deliberately omitted. There is one hard identity limit: a phone or wireless
receiver that presents itself to the OS as generic removable USB Audio Class
hardware is indistinguishable from a USB interface and may be admitted. The app
does not guess from product names or vendor IDs. A categorical “never a phone”
guarantee therefore remains unmet. Output choices are unaffected. The policy is
covered by automated checks, but the physical-hardware matrix below is still
required on every platform.

§1 names macOS and Windows as the shipping targets. Linux now has a real ALSA
backend too, so the Linux build finds and records from microphones rather than
being a development shell — what it lacks is the §7 combined-device support,
which needs a driver on every platform but macOS.

Step-by-step setup is in [Installing](#installing) below.


## What to expect on your platform

This is the **v1.13.25 release candidate**. The recording engine is covered by
878 unit tests plus capture and platform harnesses. What differs by platform is
how much of the *device* layer has been run against a live audio system and
physical hardware.

| Platform | Status | What this means for you |
|---|---|---|
| **Linux** | External-only policy and real ALSA API exercised; physical hardware unverified | The production build lists kernel ALSA cards only when sysfs proves they are removable. A separately compiled test binary admits file-backed virtual microphones so capture and hot-plug can run through ALSA in CI. Multi-input hardware, driver timing and real USB devices still require bench validation. Linux is an early-use build, not a v1 production target. |
| **macOS** | App launched on hardware; CoreAudio simulated; completed physical take outstanding | A PUPGSIS T12S was detected on a real Mac at 44.1 kHz and exposed the fixed-rate negotiation failure. An input HAL open now stops holding the UI after five seconds and quarantines late cleanup so a wedged USB interface cannot freeze launch or quit; the simulator also covers buffer layouts, rate ranges, hog-mode refusal, hot-plug and the external-only input policy. A successful physical-microphone take, latency loopback and hostile-event matrix are still owed. An older v1.11.0 build opened a USB HDMI capture device and AVFoundation logged a first-frame enqueue, but no visible non-black preview or completed camera recording has been verified for the v1.13.25 candidate. |
| **Windows** | WASAPI and external-only policy simulated; physical hardware unverified | Enumeration follows each endpoint into the Plug and Play device tree, requires an eligible wired branch plus positive removable capability and removal-policy evidence on the same node, and fails closed otherwise. Fixed/internal USB, known phone, Bluetooth, software and unknown sources are omitted in simulation. Exclusive-mode format negotiation, 16/24/32-bit conversion and the worker-thread handshake execute in CI. A real microphone, output device, driver timing and camera capture have not completed the hardware matrix. |

The automated environment can exercise ALSA through virtual PCMs and the other
backends through simulators. It cannot substitute for a physical interface,
headphone path, removable card or real camera.

What the simulation cannot reproduce is a real driver's timing, firmware quirks
and scheduling. Those are release unknowns until the hardware checklist passes.

**If something misbehaves**, use *Settings → Export diagnostics*. It creates a
zip containing logs, recent session metadata, device names and stable IDs, and
local destination paths — never audio. Review it before sharing, especially on
a public issue, then follow [`SUPPORT.md`](../SUPPORT.md).

## Installing

No separate installer is used. The macOS app and Windows portable ZIP carry
their application runtime; the Linux ZIP expects the system audio/desktop
libraries listed below. Current releases are ad-hoc signed on macOS and
unsigned on Windows; the workflow signs and notarizes once the credentials in
[`RELEASE_CHECKLIST.md`](../RELEASE_CHECKLIST.md) exist.

### macOS

1. Double-click `SobStage-macOS.dmg`. A window opens showing the app
   and an arrow pointing at your **Applications** folder.
2. Drag the crying face onto **Applications**. That is the install.
3. Open **Terminal** and run this once before first launch:

   ```sh
   xattr -dr com.apple.quarantine "/Applications/SobStage.app"
   ```

   Then open the app normally. This step is needed because current releases
   are ad-hoc signed rather than notarized with a Developer ID;
   without clearing that flag you get *"SobStage is damaged and
   can't be opened"*. Nothing is wrong with the download — see
   [Troubleshooting](#troubleshooting-macos) below for the full explanation.

   Control-click → **Open** is the usual advice for an unsigned app and it does
   *not* work here: it gets past an app with no signature, not a quarantined one
   whose ad-hoc signature Gatekeeper will not accept.
4. macOS will ask for **microphone permission** — allow it, or every meter
   stays silent. If you declined by accident: System Settings → Privacy &
   Security → Microphone → enable SobStage.
5. Plug in your USB, FireWire or Thunderbolt microphones, and plug headphones
   into each microphone's headphone jack. SobStage plays the same mix to all of
   them through its combined **SobStage** device; switch a person's headphones
   on or off in **Settings → Who hears the mix in their headphones**. If you use
   cameras, macOS also asks for **camera permission**; SobStage waits for your
   answer and starts the cameras when you click Allow. The Mac's microphone,
   iPhone/Continuity, Bluetooth/AirPlay and software inputs are intentionally
   left out. Monitoring is live from launch; there is nothing to arm. Each
   microphone reaches the headphones as soon as its own device allows: an
   interface that is slower than the rest (or that macOS runs at a larger
   buffer because another app asked for one) delays only itself, never
   everyone's headphones. The recording is lined up as it is written, so the
   tracks still match — see *Headphones first* in the
   [user guide](USER-GUIDE.md).

#### Troubleshooting (macOS)

**macOS says "SobStage is damaged and can't be opened."**

Nothing is damaged and your download is fine — this is what Gatekeeper says
when a quarantined app's signature does not satisfy it. Control-click → Open
does *not* clear it. Run this once, in Terminal:

```sh
xattr -dr com.apple.quarantine "/Applications/SobStage.app"
```

Then open the app normally. If you put the app somewhere other than
Applications, point the command at wherever it actually is.

This workaround is for an ad-hoc signed build, which is what every release so
far is. Once the workflow has Developer ID credentials it signs, notarizes and
staples the app and this step goes away.

**macOS asks for microphone or camera permission again after an update.**

Expected with the current ad-hoc signed builds: macOS ties the permission to
the exact signed copy of the app, and every update is a new one. Click
**Allow** again. If no prompt appears and SobStage says it isn't allowed, open
System Settings → Privacy & Security → Microphone (or Camera) and switch
SobStage off and back on; SobStage picks up the change by itself. A Developer
ID signed build keeps the permission across updates.

### Windows

1. Unzip `SobStage-Windows.zip` anywhere (e.g. a folder in
   `Program Files` or your Desktop).
2. Run `bin\SobStage.exe` from the unzipped folder. Current releases are not
   Authenticode-signed, so SmartScreen shows "unknown publisher" on first run:
   choose **More info → Run anyway**. Check the download against `SHA256SUMS`
   first. A signed build shows its verified publisher instead, though a new
   publisher may still get a reputation warning until it has established trust.
3. If no microphones appear: Settings → Privacy & security → Microphone →
   make sure **Let desktop apps access your microphone** is on.
4. Plug in mics and headphones; monitoring is live from launch.

### Linux

1. On Debian or Ubuntu, install the runtime providers used by the verified build:

   ```sh
   sudo apt-get install libasound2-dev libx11-dev libxext-dev libxinerama-dev \
     libxrandr-dev libxcursor-dev libxcomposite-dev libfreetype6-dev \
     libfontconfig1-dev libgl1-mesa-dev
   ```

2. Unzip `SobStage-Linux.zip`.
3. From the unzipped folder, run it:
   ```sh
   "bin/SobStage"
   ```
4. The production build lists only ALSA hardware whose Linux device ancestry
   identifies it as removable. Built-in cards, PipeWire/PulseAudio aliases and
   virtual PCMs are intentionally omitted. If a plugged-in interface does not
   appear, check that your user can access ALSA devices (often through the
   `audio` group) and report the hardware details. Developers can compile a
   separate test-only build that admits the virtual fixture; release packages
   never enable that option.


## First run — where things go, on every platform

- **Recordings** start at `~/RECORDINGS` immediately while removable-volume
  discovery runs away from the window thread. Connected card choices appear in
  **Settings → Save recordings to** after that scan finishes; a stale mount
  under `/Volumes` cannot hold the app closed at launch or quit. Free-space and
  destination-status polling use the same detached, one-at-a-time pattern, and
  a result for a destination you have since changed is ignored. The app
  benchmarks a new destination away from the window thread before enabling the
  record button (§6.4). The benchmark measures the card once (another card put
  in at the same `/Volumes` name is a different card, and is measured afresh);
  whether that is fast enough is decided fresh each time you reach for record,
  so switching a
  camera on can block arming a card that was fine for the microphones alone —
  and the message says the cameras are what did it. A benchmark that cannot be
  started or completed blocks recording with an explanation; it cannot silently
  become a pass or make launch or quit wait for the worker.
- **Video goes in the same folder** as the audio for that take, one file per
  camera, named `V01_<camera name>`. The remaining-time figure on the main
  screen accounts for it, so "Room for 2h 10m" stays true once a camera is
  running.
- **A local backup copy** of each take is kept by default in
  `RECORDINGS-MIRROR` in your home directory, so a card failure is an
  inconvenience rather than data loss. Toggle it in the Settings panel. The
  destination and enabled-backup interrupted-take scans also run away from the
  window thread. Recording stays disabled, with the reason shown, until both
  required scans succeed; a scan that cannot run fails closed. Recovered takes
  are shown after both scans settle, while stale work from an old destination is
  abandoned without delaying launch, a location change or shutdown.
- **Your settings** live beside the log, at
  `SobStage/settings.json`. Delete it to start over from defaults;
  a corrupt or unreadable one is ignored rather than fatal.
- **The log** lives at `SobStage/log.txt` under your user
  application-data directory (`~/Library` on macOS, `%APPDATA%` on Windows,
  `~/.config` on Linux). **Export diagnostics** in the Settings panel bundles
  it with recent session metadata, device identifiers and local destination
  paths — never audio. Read [`PRIVACY.md`](../PRIVACY.md) before sharing the zip.

## Uninstalling

Delete the app. The only things it leaves behind are your recordings
(`RECORDINGS`, `RECORDINGS-MIRROR`) and the log-and-settings folder above (which
also holds your saved shows) — remove those too if you want nothing left.

On a Mac, SobStage removes its combined **SobStage** input device when it
quits and on its next launch. If it was force-quit or the Mac lost power and
you then deleted the app, that device can still be listed: remove it in
**Audio MIDI Setup** (select it and press the minus button).
