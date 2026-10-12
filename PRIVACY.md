# SobStage privacy notice

Effective 4 October 2026. Applies to the SobStage v1.13.26 release candidate.

SobStage is a local-first desktop recorder. It has no account system and this
candidate does not automatically upload recordings, diagnostics, analytics or
crash reports. It also has no automatic updater. Those are deliberate v1 scope
decisions: diagnostics are created only when you ask, and updates are explicit
downloads whose published checksum can be verified.
If you switch on **Check for updates** in Settings (off by default), SobStage
asks GitHub's public releases API for the latest version number at most once a
day, and **Check now** does the same once per press; the request
carries no identifier, settings, logs or recordings, though GitHub, like any
website, sees your IP address.

## Information the app accesses

With operating-system permission, SobStage accesses eligible audio inputs and
the output you select. Recording inputs are limited to hardware the operating
system identifies as directly attached and external: USB, FireWire or
Thunderbolt on macOS; an eligible wired Plug and Play branch with positive
removable-device evidence on Windows; and a removable kernel ALSA card on
Linux. Built-in computer, known
phone/Continuity transports, Bluetooth/AirPlay, network, aggregate, virtual,
internal and unknown inputs are intentionally excluded. A phone or wireless
receiver presenting as generic removable USB Audio Class hardware is
indistinguishable from an interface and may be admitted; SobStage does not apply
product-name or vendor-ID guesses. Every connected camera is opened and
recorded by default, so the first launch may show the operating system's camera
prompt; switch a camera off in the Cameras panel and that choice is remembered.
SobStage also accesses the
recording destination and optional mirror location you choose.

## Information stored on your computer

SobStage can write:

- audio stems, a mixed WAV, optional camera video and combined video files in
  each take folder;
- `session.json` and `activity.log` beside a take;
- a mirror copy when the backup option is enabled;
- `settings.json` and `log.txt` in the SobStage application-data folder.

Settings can include device names and stable identifiers, input choices,
assigned names, camera choices, audio settings and destination paths. Session
metadata and logs can include the app version, device/session names, local paths,
timing, format, health, dropout and error information.

## Diagnostics and sharing

Diagnostics are created only when you press **Export diagnostics**. The zip
contains the application log, a device inventory and up to five recent
`session.json` files. It never contains recorded audio or video. It can contain
device names and identifiers, session names and local folder paths, so review or
redact it before sharing.

Nothing is uploaded by SobStage. If you attach a diagnostic zip or other file to
the project's GitHub issue tracker, you are choosing to send it to GitHub and
the issue is public. See [`SUPPORT.md`](SUPPORT.md).

## Retention and deletion

SobStage keeps these files until you delete them. Removing the app does not
remove recordings, mirrors, settings or logs. Delete the recording folders and
the SobStage application-data folder (`~/Library` on macOS, `%APPDATA%` on
Windows, or `~/.config` on Linux) if you want to remove them.

Questions about this notice can be filed at
<https://github.com/taylordrew4u2/usbmic/issues/new>.
