# SobStage support

SobStage v1.13.27 is a release candidate. Before reporting a problem, check the
in-app **Help** screen and the known release gates in
[`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md).

## Ask for help or report a bug

Open a GitHub issue:

<https://github.com/taylordrew4u2/usbmic/issues/new>

GitHub issues are public. Include:

- the SobStage version and operating-system version;
- the microphone or interface, output device and removable drive models;
- what you did, what you expected and what happened instead;
- whether the problem occurs before recording, during a take or after stopping;
- the exact warning shown by SobStage.

Do not post credentials, personal information or recordings you do not have
permission to share. No private support channel or guaranteed response time is
currently offered.

## Export diagnostics

Choose **Settings → Export diagnostics** (or the same button in Help). SobStage
creates `SobStage-diagnostics*.zip` on the Desktop, or in your home folder if
it may not write to the Desktop (on a Mac, if you answered **Don't Allow** when
macOS asked about the Desktop folder). The message after the export shows the
exact path. The zip can contain:

- the application log;
- up to five recent `session.json` files from the selected destination;
- the app version, audio backend, sample rate, bit depth and buffer size;
- device names, stable USB identifiers, inclusion state, exclusion reasons and
  measured drift;
- the selected output, output/monitor errors and local destination path.

The export never contains recorded audio or video. It can still reveal device
names, identifiers, session names and local folder paths. Open and review the
zip before attaching it to a public issue; omit or redact it if needed.

## Recording-input policy

SobStage intentionally lists only directly attached external microphone
hardware. macOS admits USB, FireWire and Thunderbolt transports; Windows admits
eligible USB, FireWire and Thunderbolt Plug and Play branches only when Windows
also reports positive removable-device evidence; Linux admits kernel ALSA cards
only when their sysfs ancestry reports removable hardware.
The computer's built-in microphone, known phone or Continuity transports,
Bluetooth, AirPlay, network, aggregate, virtual, internal and unknown inputs are
excluded. A phone or wireless receiver that exposes itself as generic removable
USB Audio Class hardware is technically indistinguishable from an interface and
may appear; SobStage deliberately does not guess from a product name or vendor
ID. This policy does not change output-device choices or the separate camera
list.

If a fixed-rate interface reports a rate mismatch, set SobStage to the rate the
device is already using. For the PUPGSIS T12S seen during development, that rate
was 44.1 kHz.

Physical-hardware compatibility is not yet claimed beyond the completed checks
in the release checklist. If plugged-in hardware is missing, report its exact
model and connection type rather than weakening the fail-closed policy.
