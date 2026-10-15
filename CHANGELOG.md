# Changelog

## Unreleased

### Changed

- **A slow card no longer stops you recording.** With room on this computer
  for an hour of recording plus 2 GB, every take is recorded here and copied
  to the card after Stop, and the card's speed isn't checked at all.
  SobStage used to want room here for everything the card could hold -- about
  130 GB beside a 128 GB card, with four mics and a camera -- and short of
  that it sent the take to the card, said the card was too slow, and kept
  Record off. With less room than an hour, a card that passed its check, and
  has more room left than this computer, takes the take; one that is too
  slow, full, locked or FAT32 with a camera no longer stops you while this
  computer has room for 10 minutes: the take
  records here, and the line under the record button says how long you have
  ("This computer has room for about 25 minutes of recording."). The
  "Where does this recording go?" card says it too. A locked or full card is
  tried again within seconds of being unlocked or having space freed.
- **Cameras has its own settings page.** **Cameras** is always in the bar at
  the top of the window, between Help and Settings, and says how many are on.
  It opens a page beside the main screen, the way Settings does, so the
  pictures stay in view: for each camera its name, whether it records, the
  file it will write, what to do if it isn't working, its **Picture size**
  (was "Quality") and, on a Mac, what it is running at. It also says how much
  an hour of recording takes now.
- **"Also save video with the sound in one file" moved from Settings to the
  Cameras page.**
- **The "bigger, sharper preview" and "Full preview" boxes are gone.** The
  arrows on the main screen's camera row size the pictures, as before; what a
  camera records is its Picture size.

### Fixed

- **A take recorded on this computer now stops with 1 GB still free, not at
  nothing**, so the computer keeps working and the copy to the card can run.
  The ten- and two-minute warnings come in time for that, and say "on this
  computer".
- **On Windows, the note about combined video no longer gives Mac
  instructions.** It says where to put ffmpeg (`C:\Program Files\ffmpeg`,
  with `ffmpeg.exe` in its `bin` folder); so does the message after a take.
- **The Cameras button was hidden whenever a camera was on**, which is
  exactly when its settings matter. It is always in the top bar now.

## v1.13.28 -- 2026-10-14 (release candidate)

Takes for an SD card are recorded on this computer and copied to the card after Stop; a stricter power warning.

### Changed

- **Takes going to an SD card or USB stick are now recorded on this
  computer and copied to the card when you stop, then checked.** The
  computer keeps its copy, in `RECORDINGS-MIRROR` in your home folder. Turn
  this off in Settings to record straight to the card. Nothing is written to
  the card while you record, so a slow or cheap card, or a card reader on the
  microphones' hub, can no longer cost you sound. The saved-take card shows
  how the copy is going, says where the take is safe if the card is out,
  full or locked, and has a **Copy again** button. A card that was out is
  copied as soon as it is back -- quit then, and SobStage asks to wait for
  that copy -- and a copy cut off by quitting finishes the next time you open
  SobStage with the card in. The combined video and podcast-ready copy follow
  once they are made. On a FAT32 card, a movie too big for it stays on this
  computer and everything else is copied; put in another card formatted as
  ExFAT and the whole take goes to that one, even after you quit and reopen
  SobStage. You are no longer told to reformat the card the take went to,
  which would erase the takes already on it; if you do reformat it while
  SobStage is open, the night's takes are copied to it again. If this
  computer couldn't hold as much as the card could, the take goes straight to
  the card as before, and you are told which folders in `RECORDINGS-MIRROR`
  to keep because they aren't all on the card yet. A take goes to its own
  card: if another card is in its place (cards often share a name, and
  Windows can give a card reader's letter to a USB stick), nothing is written
  to it and the take waits for its card; **Copy to this card**, on the
  saved-take card or under the record button for every take waiting this
  way, copies them to the card that is in. A take recorded with the card out
  is for the card last seen there; one that never knew its card says which
  card it went to, so you can check it. A save folder whose parent folder was
  deleted on the card is made again. Quitting while a take is still being
  copied says the take isn't on the card yet and where it stays -- or, when
  only its combined video or podcast copy is still going, that the take is on
  the card and only that file isn't. If you choose **Quit When Done** and the
  copy then fails -- even a retry failing just as the copy before it did --
  SobStage says so and where the take is safe instead of closing as though it
  were on the card. When the card's speed check is due while a take is being
  copied to it, the copy pauses for the check, so a fast card is not measured
  "too slow". If this computer's own drive stops answering at the end of a
  take recorded on it, SobStage says so instead of blaming the card.

### Fixed

- **"Your microphones need more power" appeared when the cause was something
  else**, such as a mic in use by another app or one mic dropping twice. It
  now names the mics ("Kitchen and Hall both lost their connection. They may
  need more power: use a USB hub with its own power adapter."), and one mic
  that keeps dropping gets "Try another cable or USB port." The activity list
  now gives every mic that would not open its own reason. A mic that drops
  off while it is being opened, as one short of power does, still counts, and
  so does a hub losing all its mics at once mid-take or refusing them all as
  they are opened, a mic that fails again after being plugged back in, and a
  second mic failing where the first has come back. Mics dropping as a
  Windows laptop wakes from sleep no longer count, nor do mics that Windows'
  privacy settings keep from SobStage: those say to turn on "Let desktop apps
  access your microphone" in Settings > Privacy & security > Microphone.

## v1.13.27 -- 2026-10-14 (release candidate)

Headphone sound fixes on Mac and Windows: clicks, warble and distortion.

### Fixed

The headphones sounded broken -- clicks, a warbly or robotic edge, and
distortion -- on Mac and Windows, with one microphone or several. This
release fixes the causes found by measuring the live headphone mix:

- **No more clicks and gaps.** Each microphone now keeps more audio in
  reserve for the moments a USB device or the computer delivers late
  (about 5 ms at the smallest buffer, up from 2.7 ms, which ordinary timing
  hiccups emptied). The
  headphone delay shown in Settings includes it: a few milliseconds more at
  the smallest buffer.
- **Microphones stay in time with the headphones.** A mic whose clock runs
  slightly fast or slow is caught within seconds instead of half a minute,
  and is no longer corrected by small gaps every few seconds. Each mic
  starts exactly on time, and one whose buffer size jumped and came back
  is brought back in line instead of staying 20 ms late (which sounded
  doubled or robotic).
- **No more distortion.** The headphone volume is now applied before the
  safety limit, so turning down really turns down; and when the mix is too
  loud it is turned down smoothly instead of being squared off. MIX.wav
  gets the same treatment.
- **A USB mic is no longer heard twice.** A new mic that shows up as two
  identical channels was added into the headphones (and MIX.wav) twice
  until SobStage had worked out it was mono -- for the whole take if you
  pressed Record first.
- **Smoother high frequencies.** The way each mic is kept in time no longer
  makes sibilance and cymbals swell and dip a few times a second.
- **The MIX meter shows when the headphones are being limited**, so a mix
  that is too hot is visible, not just audible.

## v1.13.26 -- 2026-10-12 (release candidate)

Mac fixes: headphone rate changes, sleep, combined video, quitting, cards and cameras.

### Fixed

- **A sample-rate change on the headphones no longer stops the take.**
  Bluetooth headphones switching to call mode, or another app setting the
  headphone interface to 44.1 kHz, used to end the recording; now the mics
  keep recording and the headphones come back after Stop. Between takes
  SobStage sets the output back to its own rate instead of blaming the CPU.
- **Mac sleep:** after the Mac wakes, the mics are no longer reported dead and
  reopened. A sleep during a take is noted in session.json.
- **Headphones on the combined device:** switched-off headphones stay off, and
  the right people keep the mix, when a mic drops out of the combined device
  or joins it late. The alarm and the start/stop chirps still play on a
  switched-off jack, so a fault is always heard.
- **Combined video waits for the next take** instead of competing with it for
  the card, and checks there is room first. Renaming the take folder in
  Finder while the combined video or podcast copy is being made no longer
  loses it.
- **Quitting:** after "Quit When Done", a second Cmd-Q asks again instead of
  cancelling the combined video, and a line says a quit is waiting. Quit
  questions bring SobStage to the front (the Dock icon bounces if macOS won't
  let it). "Stop and Quit" shows any problem with the take before quitting.
- **Cards:** a different card with the same name is speed- and format-checked
  again. A long take keeps recording past the 3.9 GB file split even if its
  folder was renamed. A take stored only in iCloud while you're offline is
  never overwritten.
- **Cameras:** two identical cameras no longer give a false "gone away" alert
  (or miss a real one); a capture dongle keeps its 30 fps mode when it
  reopens; "Recording to" names the file actually being written.
- **Less CPU in silence** on Intel Macs.
- **Disk image:** no icon covers the first-launch Terminal command any more.

## v1.13.25 -- 2026-10-09 (release candidate)

Headphones-first monitoring, Mac fixes and crash recovery.

### Fixed

- **Headphones come first.** The headphone mix is never held back to line
  the microphones up. Each mic reaches the headphones as soon as its own
  interface allows; the recorded files are lined up separately, after the
  sound has gone to the headphones. session.json records each interface's
  offset either way.
- **Mac: an interface that changes its buffer size mid-take** (another app,
  or macOS itself) no longer drops audio; going back and forth costs nothing.
- **Feedback detection is switched on.** The headphone mix is now watched
  for feedback while you record, as the guide always said; a howl cuts the
  headphones with the same message and Unmute button as before.
- **The headphone delay shown is accurate.** It now includes the small
  safety cushion it used to leave out, so the figure reads a little higher;
  the headphones are no slower. The buffer-change notice and the Settings
  figure now always agree.
- **The "slow microphone" warning** only names a mic that is really behind
  the others, not every mic once the buffer has been raised.
- **Meters, peak hold and tap-to-name keep real time** even when macOS slows
  the app down in the background.
- **Mute clears the runaway evidence,** so unmuting can't cut the headphones
  for a burst that happened before the mute.
- **Crash or power cut mid-take:** session.json is now kept up to date during
  the take, so the record a crash leaves is accurate; a card that mounts late
  after a crash is still checked; leftover half-written files are cleaned up;
  if the card copy can't be repaired, the backup copy is offered instead;
  "nothing playable survived" no longer repeats every launch; the recovered
  list mentions camera movies (including Windows .wmv).
- **Combined video:** a half-finished export is never left under the finished
  name; a repeat failure is reported every take; a combine is skipped (with a
  warning) when the card stopped answering; you're told when it's ready; on
  Mac it now ends where the sound ends.

## v1.13.24 -- 2026-10-07 (release candidate)

Mac fixes.

### Fixed

- **Mac: no more lost audio when an interface runs a bigger buffer than
  asked for.** Common on Macs; up to half a mic's audio could drop from the
  recording and the headphones.
- **Mac: mics on different interfaces line up exactly** in the recording.
  Each interface's own input delay is now measured and evened out, and the
  headphone delay shown includes it.
- **Mac: two identical cameras keep their own settings** (on/off, name,
  quality) across reboots and replugs, and unplugging one no longer hands its
  settings to the other.
- **A full disk can no longer wipe your settings** (mic names, trims, save
  folder), a take's session.json or a saved show.
- **Opening an older copy of SobStage no longer erases newer settings.**
- **No pointless backup on the same disk:** when recordings already go to
  the internal disk, the backup copy is skipped and the reason is shown.
- **Renaming the take folder after Stop** no longer sends the video, the
  saved-take file list or the combined video to the old place.
- **The start/stop chirp** can no longer be cut off by a race, and a take no
  longer loses a few samples at the very start.

## v1.13.23 -- 2026-10-06 (release candidate)

Mac fixes.

### Fixed

- **Mac: renaming or moving the take folder in Finder mid-take no longer
  ends the recording**, warns that the files stopped growing, or loses
  session.json and the activity log.
- **Mac: free space counts what macOS can free on demand** (APFS purgeable
  space, iCloud "Optimize Mac storage"), so a drive Finder shows as roomy is
  no longer called full.
- **Mac: Finder's own files** (.DS_Store, ._ files on ExFAT cards) are no
  longer counted as part of a take or recovered take.
- **Mac: a Windows-formatted (NTFS) drive** is explained as read-only on Mac,
  not as a card with its lock switch on.
- **Names keep accents and non-Latin letters** ("Müller" stays "Müller").
- **Space mutes again after typing a name** and pressing Return or clicking
  away; it used to type into the name.
- **The main screen can't be crushed** beside an open Settings or Help drawer,
  and pop-up cards can't be squashed by a short window.
- **Less battery drain:** the meters stop redrawing while the window is
  minimised, covered or on another Space. Alarms keep sounding.
- **VoiceOver** reads the mix meter's level and names each camera's switch.
- **Cmd-M minimises the window.** The window can now fill very wide displays.
- **Exporting diagnostics** falls back to your home folder when the Desktop
  is blocked, instead of failing.
- **Permission messages** no longer say to reopen the app (it picks up the
  change by itself), and say when an administrator has blocked access.
- **The log stays small** even if the app is left open for days.

## v1.13.22 -- 2026-10-06 (release candidate)

Mac fixes.

### Fixed

- **Mac (Intel): headphones plugged into the built-in jack become the
  monitor output straight away**, and unplugging them moves the mix back off
  the speakers. Before, nothing happened until some other device changed.
- **Mac: the combined SobStage device comes back by itself** after macOS's
  audio service restarts or the device is deleted outside the app. During a
  take whose headphone mix runs through it, it is re-created after Stop.

## v1.13.21 -- 2026-10-06 (release candidate)

Mac fixes.

### Fixed

- **Mac: cameras no longer leak memory each time they close.** Unplugging,
  switching a camera off and on, or changing its quality used to keep the
  whole old capture session (and its last frame) in memory.
- **Mac: a mic that blinks out and straight back** (a USB hub re-settling)
  now carries on recording instead of staying silent for the rest of the take.
- **Mac: a mic unplugged while its sample rate was changing** now says it
  disconnected, instead of "This interface is running at 0 kHz".
- **Mac: Cmd-, opens Settings**, like every other Mac app.
- **Quitting right after a take asks first** while the podcast copy or the
  combined video is still being made: Quit When Done, Quit Now or Don't Quit.
  It used to cancel them without a word.
- **The podcast copy no longer competes with the next take for the card**,
  and a copy cut short by a full card is never left looking finished.
- **Show names in any language** (Japanese, Greek, Cyrillic...) can be saved,
  and the show just saved is selected in the list straight away.

## v1.13.20 -- 2026-10-06 (release candidate)

Mac fixes.

### Fixed

- **Mac: closing the lid no longer stops a recording on the charger.** On
  battery, SobStage warns once per take to keep the lid open.
- **Mac: monitoring through a Yeti's own headphone jack no longer locks that
  Yeti** away from Zoom, OBS and other apps recording from the SobStage
  device.
- **Mac: headphones go to the right place straight away** after the
  combined SobStage device is created, instead of briefly to another output.
- **Mac cameras:** the 30 fps / 4K mode chosen for a camera now sticks once
  the camera starts; the live preview no longer risks going black when the
  screen updates (Record, Stop, a status change); and the combined video's
  picture lines up with the sound more closely.
- **Show names that look alike** ("Live stage", "Live-stage") are saved as
  separate shows instead of one replacing the other.
- **Long messages wrap** on the main screen (up to three lines) instead of
  being cut off.

## v1.13.19 -- 2026-10-05 (release candidate)

Four new features.

### Added

- **Podcast-ready copy after every take.** Pick where it's going (Spotify,
  Apple Podcasts, YouTube...) in Settings and each take also saves
  "MIX - <platform>.wav": the mix turned to that platform's loudness, with
  the loudest peaks gently limited so nothing clips. Your recording is never
  changed.
- **Shows.** Save your whole setup as a named show and load it with one
  click: mic names and trims, who hears the mix, cameras and their quality,
  where recordings go, the loudness target.
- **Setup guide.** New users get a short guide on first launch: mics, names,
  headphones, where recordings go, cameras, and a test take. Reopen it from
  Settings or Help.
- **Update check (off by default).** Turn it on in Settings and SobStage asks
  GitHub once a day whether a newer version is out, and offers the download
  page. It sends nothing about you; "Check now" works with it off.

## v1.13.18 -- 2026-10-05 (release candidate)

Better camera settings.

### Added

- **Camera settings, per camera.** Each camera in the Cameras panel now has a
  **Quality** choice: Best (up to 4K), 1080p, or 720p for smaller files. It is
  remembered, and changing it reopens the camera straight away (not during a
  take).
- **See what each camera is really doing.** Each camera row says what it is
  running at, for example "Running at 3840 x 2160, 30 fps" (Mac).
- The free-space and card-speed checks now budget each camera at its own
  quality, so a camera turned down to 720p no longer counts as a 4K stream.
- Clearer wording in the Cameras panel.

## v1.13.17 -- 2026-10-05 (release candidate)

4K camera recording on the Mac.

### Changed

- **Mac: cameras record in 4K.** SobStage used macOS's "High" camera setting,
  which tops out at 1080p, so a 4K camera had to be turned down or was
  recorded at a quarter of its resolution. A camera with a 4K mode that runs
  at 30 fps (Continuity Camera, a 4K webcam, a USB 3 capture card) now records
  in 4K. A USB 2 capture dongle still tops out at 1080p, whatever its HDMI
  input accepts.
- The free-space and card-speed checks now budget about 64 Mbit/s per camera
  (was 32), enough for a 4K stream.

## v1.13.16 -- 2026-10-04 (release candidate)

Polish: clearer messages, layout fixes and up-to-date docs.

### Changed

- **Clearer activity log.** A device's name is no longer said twice on a row
  ("Yeti -- Yeti is connected"), repeats read "(4 times)" instead of "(x4)",
  a microphone with a headphone jack is announced once instead of twice, and
  a long entry is shortened on screen instead of pushing older rows out of
  sight. The full text is still in the take's activity log.
- **Shorter messages.** The missing-camera message is one short sentence
  pair, "Two mics heard that" is said at most every ten minutes, and the
  camera panel and Settings use plain wording ("Seen by other apps", "Clock
  drift per mic", "Loudness for streaming").
- **Settings no longer hides its right edge under the scrollbar** on first
  open, and the save location shows your home folder as "~" so the take
  folder name isn't cut off.
- Docs: README, privacy notice, checklist and spec now match how cameras,
  headphones and the Mac combine actually work; test counts are current.

## v1.13.15 -- 2026-10-03 (release candidate)

One fix from a real sound check.

### Fixed

- **"Two mics heard that -- try tapping closer to one" no longer floods the
  log.** With two microphones in one room, any loud sound (talking, music)
  reached both and was reported on every screen update -- 325 times in one
  sound check. It is now reported once per sound, at most once a minute, and
  a clean tap on one microphone still names it.

## v1.13.14 -- 2026-10-03 (release candidate)

One Mac camera fix for HDMI capture dongles.

### Fixed

- **Mac: HDMI capture dongles run at 30 fps instead of about 5.** Many
  "USB2 Video" dongles offer 1080p at about 5 frames a second beside a 30 fps
  mode, and the Mac could pick the slow one (or one that streams black).
  When the chosen mode can't reach 30 fps, SobStage now switches to the
  largest mode up to 1080p that can. Webcams that already run smoothly are
  left alone.

## v1.13.13 -- 2026-10-03 (release candidate)

Two Mac camera fixes, aimed at a capture card showing a black preview.

### Fixed

- **Mac: a camera or capture card sending an all-black picture now says so.**
  A "USB2 Video" HDMI dongle with no signal, an unsupported input mode or an
  HDCP-protected source (Apple TV, iPad, many laptops and consoles) keeps
  sending black frames. SobStage used to show a silent black box; after about
  1.5 s of black the tile now says the picture is black and what to check, and
  the preview comes back as soon as there is a real picture.
- **Mac: cameras wait for camera permission instead of opening into black.**
  A camera opened while the macOS camera prompt was still up (or after a
  denial) could stream black until it was switched off and on. SobStage now
  asks for camera access first, says what to do while it waits, and starts
  the cameras as soon as you click Allow.

## v1.13.12 -- 2026-10-03 (release candidate)

Three more Mac fixes found reviewing the Mac audio and video code.

### Fixed

- **Mac: quitting while a combined video is being made can't leave a
  half-made file that looks finished.** SobStage now waits for the Mac to
  really stop before it removes the unfinished file.
- **Mac: monitor latency can't be under-reported** when an output device
  reports a nonsense delay.
- **Mac: a second copy of SobStage can't break the first one's combined
  device.** With two users logged in (Fast User Switching), each copy used
  to remove the other's SobStage device whenever it changed or quit.

## v1.13.11 -- 2026-10-03 (release candidate)

Four Mac fixes found reviewing the last three releases.

### Fixed

- **Mac: a mic pulled out mid-take is always noticed.** If it was pulled in
  the instant after its last sound, SobStage could miss the unplug entirely:
  no entry in the log and no warning. It now checks again a moment later.
- **Mac: headphones come back after Stop** if SobStage couldn't rebuild the
  combined device with a mic change made during the take. They used to stay
  silent until something else changed.
- **Mac: renaming the combined device can't leave the headphones silent**
  if macOS refuses the new name.
- **Mac: two identical mics keep the right switches** in Settings after
  they're unplugged and plugged back in a different order.

## v1.13.10 -- 2026-10-02 (release candidate)

The combined video works on every Mac with nothing to install, plus three
more Mac fixes.

### Fixed

- **Mac: "Also save video with the sound" no longer needs ffmpeg.** SobStage
  now makes the combined file with the Mac's own video tools. The picture is
  copied as recorded and the sound goes in at the depth it was recorded at,
  lined up with the picture. Nothing to install.
- **Mac: quitting during a take asks first.** Cmd+Q or the red close button
  mid-take now asks "Stop recording and quit?" instead of ending the take on
  the spot. Keep Recording is the default.
- **Mac: a hidden window can't slow the take down.** SobStage now tells macOS
  not to throttle it (App Nap) while a take records, so the watchdog, alarms
  and disk writer keep full speed behind other windows.
- **Mac: one glitch in the Mac's device list can't silence a working mic.**
  A mic that briefly went missing from macOS's list mid-take was written as
  silence until Stop, even though its sound was still arriving.

## v1.13.9 -- 2026-10-01 (release candidate)

Eleven more Mac fixes from the Mac-only bug hunt.

### Fixed

- **Mac: the window stays on its display.** Opening Settings, Help or a
  camera no longer moves a window on an external monitor back onto the
  laptop, and no longer slides the title bar under the menu bar.
- **Mac: arrow keys and trackpad scrolling no longer change settings.**
  Up/Down after clicking a Settings picker used to switch the headphone
  output, drive, rate or bit depth; a two-finger scroll over the volume or a
  trim slider changed the level.
- **Mac: two identical microphones switch correctly.** The on/off, input and
  headphone switches act on the one you clicked, not the first of the pair.
- **Mac: a card that stops answering can't freeze the window.** Choosing a
  new save location, or the check right after Stop, no longer hangs on a
  wedged card, and the folder chooser no longer opens on it.
- **Mac: a slow card still writing at Stop is waited for.** It used to be
  reported as having stopped answering: the final session.json was skipped,
  the take came back as "interrupted", and the combined video was cut short.
- **Mac: FAT32 (MS-DOS) cards are refused when a camera is on**, with how to
  fix it. The movie is one file and stopped at 4 GB, about 18 minutes in.
  Audio-only recording to FAT32 still works.
- **Mac: "SobStage isn't allowed to save here"** when macOS privacy settings
  block the folder, instead of telling you to re-seat a card that is fine.
  A locked or full card is named as such.
- **Mac: the drive check no longer leaves 200 MB files behind** after a quit
  or crash during it, and clears any left by earlier versions.
- **Mac: monitor latency shows the real figure**, including what the output
  device adds. Bluetooth headphones showed about 3 ms; they now show their
  real 150 ms or more.
- **Mac: a mixer that is also the headphone output keeps recording** when a
  second, separate mic fails to open, instead of closing the whole rig.
- **Mac: the combine ("Also save video with the sound") yields the card** to the
  next take recording on it, and an ffmpeg that can't run on this Mac (an
  Intel build without Rosetta) is skipped instead of failing every take.

## v1.13.8 -- 2026-10-01 (release candidate)

Everyone hears everyone, through their own microphone, plus two Mac fixes
for the combined "SobStage" device.

### Changed

- **Mac: the headphone mix now goes to SobStage -- every microphone's own
  headphone jack at once.** Plug headphones into each mic and everyone hears
  the whole room. This is the new default; your previous output choice is
  replaced by it. Picking another output in **Settings -> Monitoring and
  Output** switches it off and is remembered.
- **Mac: switch each person's headphones on or off.** Settings has a new list,
  "Who hears the mix in their headphones", with one tick box per microphone
  that has a jack. Off means that jack gets silence. It applies at once, even
  mid-take, and never changes the recording.

### Fixed

- **Mac: "Couldn't make the combined device other apps record from" at
  launch.** A SobStage device left behind by a crash or force-quit blocked the
  new one. It is now removed first, and other apps stop seeing last session's
  microphones.
- **Mac: no more reopening every microphone right after launch.** Creating the
  SobStage device made the app close and reopen every stream a moment after
  they first opened, which some USB mics stall on. An unchanged, healthy rig
  is now left alone.
- **Mac: other apps can still record from SobStage while it carries the
  headphone mix**, and plugging or unplugging a mic mid-take no longer cuts
  everyone's headphones -- the device is rebuilt at Stop.

## v1.13.7 -- 2026-10-01 (release candidate)

Urgent Mac fixes on v1.13.6: the microphone-permission lock and where the
headphone mix goes.

### Fixed

- **Mac: Record no longer stays locked while your microphones are working.**
  macOS can report that SobStage has not been asked about the microphone even
  while it is already feeding the app live sound (for example after updating
  to a new build). v1.13.6 then kept Record off with "macOS is asking whether
  SobStage may use your microphones" and no way past it. Live sound from any
  microphone now proves access is working, and Record turns on.
- **Mac: SobStage asks macOS for microphone access itself.** If macOS has
  not been asked yet, SobStage now asks at launch through the same system
  call it later checks, so the Allow prompt appears straight away and Record
  turns on as soon as you answer. If macOS already allows SobStage, Record
  is ready with no prompt at all.
- **Mac: headphones on a USB interface you picked in macOS get the monitor
  mix.** If the interface's inputs are switched off in SobStage and you chose
  it as the Mac's sound output, SobStage now uses it instead of the MacBook
  speakers, where live microphones could feed back. Headphones in the Mac's
  own jack still take priority, and a mic plugged in later can't take over.

## v1.13.6 -- 2026-09-30 (release candidate)

Seven more Mac fixes from the Mac-only bug hunt, each proven by a test that
failed first: where the headphone mix goes, camera previews over cards,
mics that drop out or are replugged mid-take, the camera crash-loop guard,
and cameras that fail to start recording.

### Fixed

- **Mac: the live mix now goes to your headphones, not the MacBook speakers.**
  If you have never picked an output in Settings, SobStage now picks
  headphones in the Mac's headphone jack, or whatever output macOS is playing
  through, instead of the built-in speakers. AirPods or a Bluetooth speaker
  that connects on its own, or the headphone jack of a mic you switched off,
  no longer takes over the monitor, though you can still choose any of them by
  hand. The output SobStage picks by itself also stays put when macOS moves
  its default output, for example when a USB mic is plugged in mid-show.
- **Mac: Camera previews no longer cover the recording cards.** The live
  camera picture used to sit on top of the save prompt, the Saved card, the
  red recording alert and the RECORDING banner, hiding their text and buttons.
  The previews now go blank while any of these is on screen and come back when
  it closes; the cameras keep running and recording throughout.
- **Mac: A mic unplugged and plugged back in during a take is no longer
  reported as recording while its track is silent.** The app now tells you it
  can't rejoin the take in progress and that pressing Stop, then Record brings
  it back. A mic plugged in for the first time mid-take is no longer described
  as being in the headphones.
- **Mac: A mic that stops sending audio for a few seconds mid-take and then
  starts again is recorded again.** Before, if a mic's audio paused for 5
  seconds or more (for example when the MacBook slept and woke), its track
  stayed silent for the rest of the take and the app wrongly warned that the
  computer was dropping sound. Now its track goes live again as soon as its
  audio comes back, and the activity log says so.
- **Mac: A camera that took the app down while starting is no longer restarted
  by opening the Cameras panel.** SobStage now leaves that camera off on every
  launch, not just the next one, until you switch it off and back on in the
  Cameras panel. Opening the panel, starting a take or switching on a
  different camera no longer starts it.
- **Mac: Record comes back on its own after you fix a mic that wouldn't
  open.** If no microphone could be opened (the interface was at the wrong
  rate, or another app was holding it), SobStage now tries again by itself
  after you fix the problem. Before, you had to replug the interface or
  restart the app.
- **Mac: A camera that can't start recording now says so straight away.** If a
  camera lost its connection just as you pressed Record (for example a
  Continuity iPhone being picked up), the app now reports "Couldn't start
  recording video from" that camera. Before, its tile showed STARTING for the
  whole take, no video was written, and Record stayed blocked for 15 seconds
  after Stop.

## v1.13.5 -- 2026-09-30 (release candidate)

Four Mac fixes on v1.13.4 that matter during a show, found by an exhaustive
Mac-only bug hunt. Each is proven by a test that failed first.

### Fixed

- **Mac: pressing Return can no longer stop a take by accident.** After you
  clicked Start recording, the record button kept the keyboard, so a stray
  Return stopped the take -- including behind the mid-take alert card, where
  Return means "keep recording". Return now only answers the card on screen.
- **Mac: camera gesture Reactions no longer end up in your recordings.** macOS
  draws fireworks, balloons and confetti into the camera picture when someone
  gives a thumbs-up or peace sign; SobStage now turns those effects off by
  default. If you switched them on for SobStage in the menu-bar Video menu,
  turn them off there.
- **Mac: Record waits until you answer the microphone permission message.**
  While macOS is still asking whether SobStage may use your microphones,
  Record stays off and tells you to click Allow; it turns on a moment after
  you do. Before, a take started then recorded silence.
- **Mac: the recorder protects the separate tracks and the backup even when
  other warnings keep appearing.** When the drive falls behind, the app
  switches to the mixed file in time, and it stops the backup copy before it
  fills your disk. A steady stream of glitch or overload warnings used to
  hold both back.

## v1.13.4 -- 2026-09-30 (release candidate)

An urgent Mac fix on v1.13.3: on macOS 27 the app quit about a second after
launch whenever a camera was connected.

### Fixed

- **Mac: the app no longer quits on launch on macOS 27 with a camera
  connected.** The camera was started a moment before its on-screen preview
  was attached. Attaching the preview changes the camera's setup, and on
  macOS 27 that change landing while the camera was still starting made
  macOS raise an error that closed the whole app. The preview is now attached
  first, nothing changes the camera's setup while it is starting, and a
  camera that fails to start this way is retried instead of closing the app.
  Pressing Record at the same moment can no longer close the app either.
- **Mac: a camera can never make the app quit on every launch.** If the app
  ever goes down while starting a camera, the next launch leaves that camera
  off, keeps the sound working and says so; turning the camera off and back
  on tries it again.

## v1.13.3 -- 2026-09-30 (release candidate)

A Mac fix release on v1.13.2: seven bugs found by a hunt aimed only at the
Mac. The most serious could leave nothing recording when the headphones had
to change sample rate. Physical-hardware validation and code signing remain
open in `RELEASE_CHECKLIST.md`.

### Fixed

- **Mac: headphones that switch between 44.1 and 48 kHz open again.** The
  Mac gave the headphone output less time to open than it allowed the
  output to change its sample rate, so an ordinary rate switch was reported
  as "macOS took too long to connect this interface ... unplug it"; outputs
  and microphones now get up to a second, which covers the switch.
- **Mac: one slow microphone no longer fails every other microphone.** When
  one microphone was slow to open, every microphone set up after it failed
  at once with "macOS is still finishing an earlier audio-rig operation";
  the others now wait briefly for it to finish and then open.
- **Mac: Record turns off when microphone access is refused.** The app
  checked microphone permission only when it opened, so clicking "Don't
  Allow" on the first-run prompt (or turning access off later in System
  Settings) left Record enabled over microphones macOS was feeding silence.
  It now re-checks every couple of seconds and on every plug or unplug: a
  refusal disables Record and says why, and allowing access reopens the
  microphones without restarting the app.
- **Mac: the computer no longer sleeps during a take.** The app said it kept
  the Mac awake while recording, but nothing did; a laptop left alone could
  dim and sleep mid-show. The display and the system are now kept awake from
  Record until Stop (or quit), and allowed to sleep again afterwards.
- **Mac: unplugging a camera mid-take no longer crashes the app.** A camera
  that was unplugged, or a Continuity iPhone that was picked up, while
  recording could make macOS raise an error on the camera's twice-a-second
  picture check, and the app quit on the spot, losing the take. The check now
  skips a camera that has stopped sending video and keeps watching it, so the
  recording carries on and the camera is reported as lost (or comes back by
  itself when its picture returns).
- **Mac: a camera unplugged right after Stop keeps its video file.** If a
  camera was unplugged in the moment after Stop, while macOS was still
  finishing its movie, the app waited 15 seconds and then said the movie had
  failed. It now lets that camera finish writing its file first.
- **Mac: an unsupported Mac is told so instead of failing to open.** The app
  did not declare its minimum macOS version (13 Ventura), so on an older Mac
  it simply failed to launch. It now declares it, and Finder explains that
  the Mac needs macOS 13 or later. The release check also confirms this and
  that the microphone and camera permission messages are still present.

## v1.13.2 -- 2026-09-29 (release candidate)

A bug-fix release on v1.13.1: 46 fixes found by eight rounds of bug hunting,
each proven by a test that failed before the fix and passes after it. Nothing
was added. Physical-hardware validation and code signing remain open in
`RELEASE_CHECKLIST.md`, as for v1.13.1.

### Fixed

- **Turning off the local backup mid-take stops it.** Unticking it only
  changed the setting: the copy kept being written, no longer stopped when
  the disk ran low, and the take's details said the backup was off; it now
  stops at once, and the take's details say where its copy ends.
- **Settings' trim sliders follow renamed and swapped microphones.** A
  rename left the old name on its trim row, and after two microphones
  swapped places a slider could adjust the wrong one; the rows now follow
  each microphone and show its current trim.
- **A microphone that stops sending audio is reopened for the next take.**
  When a mic's audio stopped but it stayed plugged in, nothing reopened it,
  so every later take recorded it as silence while its strip looked live; it
  is now reopened, and one that stays dead is shown asleep and reported.
- **Clearing a microphone's or camera's name gives it back its own.**
  Erasing the name (or typing only symbols) renamed it "Session" -- on its
  strip, its sound files and its movie; it now goes back to the device's name.
- **Installing ffmpeg while SobStage is open takes effect.** After ffmpeg
  was missing once, SobStage never looked again until it was restarted, so
  the next take still wasn't combined as promised; it now checks again.
- **The take's details file counts the frames the drive dropped.** The
  session.json written at Stop always said no frames were lost to a slow
  drive, even when some were; it now records the take's real count.
- **The take's details file is timed correctly while a camera finishes.**
  When Stop had to wait for a camera's movie, session.json stamped every
  drift and dropout entry at 0 seconds and could report figures from after
  the take; it now records the take as it stood when Stop was pressed.
- **Short, loud peaks show on the meters.** At small buffer sizes a
  transient lasting only a few milliseconds could fall between meter updates
  and never reach the level, peak hold or setup advice; each update now
  shows the loudest moment since the last one.
- **The bottom row keeps its full height when Record is off.** With a reason
  shown under the record button, or no microphones plugged in, the mute
  button and volume slider were squashed to a sliver; the screen now makes
  room for them and scrolls if it has to.
- **The time left on the drive is readable in a narrow window.** Beside an
  open Settings drawer, or with the window dragged small, it was cut to
  "Room for 1..."; the volume slider now shortens first, and during a take
  the elapsed and remaining times share the space.
- **Record comes back once a card that couldn't be written is fixed.** After
  the drive check failed to write to a card, Record stayed off for the rest
  of the session -- even after freeing space or reinserting the card -- and
  said the card was too slow (0 MB/s). It now says the card couldn't be
  written, and checks again every few seconds until it can.
- **The drive warning no longer says nothing was lost after the tracks
  stopped.** Once the drive fell so far behind that only the mixed file was
  being kept, a later slowdown said "Nothing has been lost yet" and warned
  the separate tracks "will be dropped". It now says the separate tracks
  stopped earlier and the mixed file is still recording.
- **Pressing Quit again while the camera files finish no longer cuts the
  movie short.** A second quit forced SobStage closed in the middle of the
  camera writing its movie; it is now ignored, and SobStage closes by itself
  once the files are finished (at most about 20 seconds).
- **Opening SobStage again brings back its window.** Launching it while it
  was already running minimised did nothing, so it looked as if it had
  failed to start; the existing window is now restored and brought to the
  front.
- **Unplugging the headphones mid-take no longer shortens the recording.**
  The tenth of a second it took to notice the headphones had stopped was
  left out of every track and the mix, so the audio after it ran early
  against the camera. That span is now kept in the take -- as silence where
  the audio could not be held, and noted in the take's record -- so the
  tracks stay the length of the take and in sync.
- **A recovered take whose files the card wouldn't fix is no longer called
  playable.** The recovery card said every take had been repaired and was
  playable, even when the card refused the repair on some files. It now
  says which takes have files that couldn't be repaired, and to copy them
  off the card before playing them.
- **Recovered mono 24-bit takes are repaired into valid WAV files.** A take
  with an odd number of samples was left without the pad byte a WAV file
  needs after its audio, so stricter players and editors could reject it.
  Recovery now adds it, as a normal stop does.
- **Windows: audio a microphone's driver drops is now counted.** When the
  driver fell behind and lost a stretch of audio, no dropped audio was
  reported and the drift reading went wrong, as if the microphone's clock
  had slowed down. The loss is now counted and reported, and no longer
  skews drift.
- **Windows: mono and four-channel headphone outputs are no longer called
  incapable.** The check before monitoring asked only about stereo, so
  such outputs were refused with advice to change a Windows setting,
  although they would have opened fine.
- **macOS: unplugging one interface no longer locks out the rest until
  relaunch.** Closing a rig after an interface was pulled treated the
  vanished device as refusing to let go, and every later open said macOS
  was still releasing the previous rig. Only a device that is still
  present and refuses now holds that lock.
- **Trim on one input of an interface no longer moves the others.** Setting
  one person's trim on a multi-input interface showed it on every input and
  applied it to all of them after a relaunch. Each input keeps its own trim
  now, and session.json records it per input.
- **A settings file cut short no longer brings back half a value.** After a
  power cut mid-save, the recordings folder could come back as
  `/Users/me/RECO` and a microphone named "Lead Vocal" as "Lea", then be
  saved that way. Settings that finished writing are kept; the one that was
  cut off goes back to its default.
- **Clicking a microphone's strip no longer takes the space bar away from
  mute.** After a click to clear a clip or rename a microphone, the next
  Space reopened the rename dialog instead of muting the headphones.
- **Cards cover the whole window again after a resize with Settings or Help
  open.** They shrank to the main screen, leaving the drawer's controls
  usable beside a card that should have been in charge.
- **Linux: a multi-input interface records every input, not just one.**
  Opening the interface asked how many inputs it had by opening it a
  second time, which the hardware refuses while it is in use, so a
  four-input interface recorded a single track.
- **Linux: an interface being recorded from keeps all its strips when the
  device list changes.** Plugging or unplugging anything re-checked every
  device, the one in use could not be opened to ask, and its four strips
  collapsed to one. A device that cannot be asked now keeps what was known.
- **A long take's combined video keeps its sound to the end.** Once the
  mix passed 3.9 GB and carried on in `MIX_001.wav`, the video-with-sound
  file was cut off where `MIX.wav` ended. Every part of the mix is now
  joined, in order, under the picture.
- **Windows: recording into a folder with an accent in its name works.** A
  recordings folder under a path like `C:\Users\Zoë` failed every take before
  it started, and recovery could not open what was there.
- **A stalled drive raises the alarm once, not twice a second.** When the
  take's files stopped growing, the stall verdict repeated on every tick:
  each one added another alert row and brought the card and the siren back
  half a second after Keep recording. It is now reported once per stall,
  and again only if the files grow and then stall a second time.
- **Plugging in a second camera of the same model no longer stops the one
  recording.** A same-name group that only grew mid-take is still held out
  of the list, but its running recording is kept; nothing left. A group
  whose count drops is still treated as before.
- **Pulling that second camera out again no longer stops the one
  recording either.** The camera count going back down to where the take
  began was read as the recording camera leaving, and its movie was closed.
- **A stalled audio writer is caught while a camera is recording.** The
  check that the take's files are growing counted the camera's movie too,
  so a growing video hid audio that had stopped reaching the drive. Only the
  audio files count now.
- **Renaming a microphone during a take now shows on its strip at once.**
  The take's own files keep the name they started with, as before, but the
  strip kept the old name until the take ended. The UI walk renames a
  microphone mid-take and checks the strip.
- **WAV files with an odd-sized data chunk are padded to an even length**,
  as RIFF requires. A mono 24-bit take with an odd number of frames ended
  on an odd byte, which strict readers can reject. The pad is added when a
  file is finished; its size is not counted in the data chunk.
- **Windows: microphones open when the app's main thread is a
  single-threaded COM apartment**, which JUCE always makes it. The audio
  worker threads now join the multithreaded apartment themselves; before,
  COM calls from them could fail and every microphone read as "no longer
  connected". Reproduced in the Windows simulator, which now models COM
  apartments per thread.
- **Windows: a buffer smaller than the device's minimum period opens at the
  minimum** instead of failing with a message blaming another app; and a
  device that vanishes between a rejected period and the retry now says why
  the open failed instead of failing with no message. Other Initialize
  failures name the Windows error code.
- **Muting the headphones could come undone on its own.** When the app
  rebuilt its audio engine -- which it does itself when it raises the buffer
  size for a computer that can't keep up, or when the sample rate changes --
  the new engine started unmuted, so a room muted with the space bar or the
  Mute button was quietly un-muted, and a space press made during the
  rebuild was lost. The mute is now held by the app, like the volume, and
  carried into every rebuilt engine. The UI walk checks it: mute, force a
  rebuild, still muted; unmute, rebuild, still unmuted.
- **A take stopped while the last one was still being combined now gets
  its combined video too.** With "Also save video with the sound in one
  file" on, a take stopped while the previous take's ffmpeg was still
  running was turned away, and nothing asked again: its `_with-sound` file
  silently never appeared. It now waits its turn behind the first on the
  same worker -- still one ffmpeg at a time -- and the combine status
  covers both takes. Quitting still stops the run, queued takes included.
  The combiner simulator stops a second take mid-combine and checks both
  files arrive.
- **The fault siren is heard when the headphones die mid-take.** Once the
  monitor output stopped calling back, the app still counted it as the way
  to play its own sounds, so the siren for that very fault -- and every
  later one in the take -- was set on a tone only the dead output renders,
  and the computer's default output, there for exactly this, was held
  shut. While the output clock is lost the siren and the start and stop
  sounds now go through the default output, and move back to the
  headphones when they return. A unit test stops the output mid-siren and
  checks the rig stops offering it.
- **A buffer size or sample rate picked in Settings during a take now takes
  effect when the take stops.** The streams cannot be reopened mid-take, so
  the change is owed to the stop -- but nothing marked it owed: Settings
  showed the new size or rate while the streams stayed on the old one, take
  after take, until some unrelated event reopened them. A microphone or
  output plugged in mid-take was left out the same way, so the next take
  ran without the microphone it was promised. Any device change that
  arrives during a take is now applied at Stop. The UI walk records a
  second take that changes only the buffer size and checks the streams
  reopen at it once Stop is pressed.
- **A take's `session.json` describes the files it wrote, even when Settings
  changed mid-take.** A bit depth or sample rate picked during a take is
  for the next take -- this one's files are already open -- but the copy
  of `session.json` rewritten at Stop took whatever Settings said by then,
  so a 48 kHz 24-bit take could be recorded as 44.1 kHz 16-bit, and the
  combined video's sound was written at the new depth rather than the one
  recorded. Its microphone list was likewise whoever was ticked and plugged
  in at Stop. The take's rate, depth and microphones are now fixed when it
  starts, and both read those. The UI walk changes both mid-take and checks
  `session.json` against MIX.wav's own header.
- **A runaway-sound cut stays on until Unmute is pressed, even through an
  audio engine rebuild.** The cut was kept only in the engine, so when the
  app rebuilt it -- raising the buffer size for a computer that can't keep
  up, a hot-plug that moved the sample rate, or the restart a take put off
  until Stop -- the new engine started uncut. The headphones came back at
  full listening level with nobody pressing Unmute, and the "Sound was cut"
  message went away. If the feedback was still there, it howled for another
  half second before the new engine cut it again. The cut is now carried
  into the rebuilt engine, as the Mute is. The UI walk cuts the sound, forces
  a rebuild, and checks the sound is still cut. It then presses Unmute and
  checks the next rebuild does not bring the cut back.
- **Picking the headphone output no longer fails because the app's own
  chirp is holding it.** With no monitor output open, the start and stop
  chirps play through the computer's default output, and that output was
  only let go once monitoring already had its stream. On Linux, where the
  default output (dmix, PipeWire) sits on the same card, and on Windows with
  "Give exclusive mode applications priority" off, that card was then busy:
  picking it in Settings was refused as "in use", monitoring stayed off,
  and the default output was never closed again until the app restarted.
  It is now closed before the monitor output is opened, and reopened at
  once if monitoring still has no output. (PipeWire keeps an idle card held
  for a few seconds after its last client leaves, so there the first open
  can still be refused; the next one succeeds.) The UI walk picks the
  output again after a take and checks the default output was let go
  first.
- **A take the app stops for writing nothing keeps its red card on top.**
  When the files had not grown three seconds in, the app stopped the take
  and raised the "RECORDING FAILED" card, then opened the saved-take card,
  headed "Saved.", over it in the same moment. The siren went on sounding
  behind a card that did not mention it. The card that said why was out of
  sight, and only its OK, under the saved-take card, could stop the siren.
  The saved-take card now waits until the red card is closed, and OK brings
  it up. A new UI walk run (`MMA_UI_WALK_MODE=proof`, launch 5 of
  `Tools/e2e_ui_walk.sh`) records a take whose files never grow. It checks
  that the red card is the one on screen while the siren sounds, and that
  the saved-take card comes up only after OK.
- **A take's session.json lists only that take's buffer-size changes.**
  Every take carried every change since SobStage opened, including earlier
  takes' and ones made between takes, timed in seconds since the computer
  started. Each take now records its own, timed from its start.
- **Dismissing a recovered take also clears its local backup copy.** A take
  found both on the card and in `~/RECORDINGS-MIRROR` was shown once, but
  only the card copy was marked as dealt with, so the backup copy was
  offered as interrupted again at the next launch.

## v1.13.1 -- 2026-09-29 (release candidate)

A testing release on v1.13.0. The app itself behaves the same; what is new is
proof that the crying face on each microphone strip does what it claims, on
every platform and in the real app. Physical-hardware validation and code
signing remain open in `RELEASE_CHECKLIST.md`, as for v1.13.0.

### Added

- `sim_channel_meter`, a simulator for the crying face: a real `Metering`
  fed synthesized microphones through the strip's own 60 Hz timer, checked
  in every state -- asleep, frown, either side of the -18 dBFS tear line, a
  sob on clip, the clip latching and clearing, the tear drying as the level
  falls, a microphone lost while loud, the highlight ring, and every strip
  size -- both from what the strip says its face is and from the tear pixels
  it actually drew. Runs in CI on Linux, macOS and Windows.
- The UI walk now checks the faces in the real app on the virtual
  microphones: the two -8 dBFS tone mics each cry one tear, the silent input
  frowns, the pixels match every face, and the microphone killed mid-take
  closes its eyes.

## v1.13.0 -- 2026-09-29 (release candidate)

A feature release on v1.12.1: every camera records, faults during a take are
an alarm you cannot miss, starts and stops are announced, and each microphone
strip now wears the app's crying face. Physical-hardware validation and code
signing remain open in `RELEASE_CHECKLIST.md`, as for v1.12.1.

### Changed

- **Every camera plugged in records**, each to its own file for the whole
  take. A newly seen camera used to stay off until it was switched on in the
  Cameras panel; now it is in the take from the start, and the panel is where
  one is switched off. That choice is still remembered across an unplug and a
  relaunch. The first launch may raise the OS camera prompt before record is
  pressed, which is the price of never missing a camera from a take that
  cannot be redone.
- **The mid-take card is now an alarm.** When anything goes wrong during a
  take -- a microphone unplugged or gone silent, a camera lost or switched
  off, sound dropped, the drive falling behind, the headphone output gone --
  the whole window flashes red, a banner across the card says SOMETHING IS
  WRONG in letters that read across a room, and a two-tone siren sounds in
  the headphones, until Keep recording or Stop recording is pressed. The card
  used to dim the room politely and wait, which is what got missed on a loud
  stage.
- **Starts and stops are announced.** For three seconds the whole window
  flashes RECORDING in red, or RECORDING STOPPED in cyan, with the take's
  name, and the headphones play a rising chirp for a start and a falling one
  for a stop. Whatever started or stopped the take -- the button, the card,
  the proof, the drive -- the announcement is the same. The banner takes no
  clicks, so the saved-take card behind it works through it.
- The alarm and the banners honour the system's reduced-motion setting by
  pulsing once a second instead of flashing four times a second. The sounds
  go through the monitor output when one is open, and through the computer's
  own default output when none is, so a laptop with nothing plugged in still
  hears the siren.
- **Each microphone strip wears the app icon's crying face.** The level ring
  now has two eyes and a frown in it; it sheds one cyan tear once a microphone
  is loud (−18 dBFS and up) and two when it clips, and closes its eyes when
  there is no signal. The fill, the level track and the number still say
  everything; the face is for fun. The help text and prompts talk about the
  meter and the strip rather than a skull, and the macOS disk image says to
  drag the crying face onto Applications.

### Fixed

- A microphone that would not open was not mentioned when the headphone
  output had a problem of its own: the output's sentence replaced the
  microphone's, so a rig with a shared output and a dead cable was told about
  its headphones and nothing about the track that would be silent. Both are
  said now.
- On Linux test builds, ALSA's `default`, `sysdefault` and `dsnoop` aliases
  no longer appear in the microphone list as a fifth, silent microphone.

### Added

- `AlarmTone`, the app's own sounds, rendered into the headphone mix by the
  audio callback rather than played through the OS, which would fight the
  exclusive monitor stream for the device. Unit-tested for pattern, level and
  timing.
- The UI walk now checks the start and stop banners, that the headphones
  chirp for each, and in its dead-microphone run that the card flashes, the
  siren sounds and keeps sounding, and both stop the moment Keep recording is
  pressed.

## v1.12.1 -- 2026-09-28 (release candidate)

A fix release on v1.12.0. Two faults that could spoil a take without saying
so, both found by new whole-app simulators. Physical-hardware validation and
code signing remain open in `RELEASE_CHECKLIST.md`, as for v1.12.0.

### Fixed

- A microphone whose audio stream died mid-take while still plugged in is now
  treated exactly like one that was unplugged. Before, the failure only reached
  the activity log: its meter stayed lit, the mid-take alert card never came
  up, and the silence written to its track was reported as the computer being
  too slow. Found by the new UI walk's dead-microphone run.
- A NaN or infinity from a microphone driver is written as silence. Infinity
  used to become a full-scale click in that microphone's file, a click at the
  ceiling in `MIX.wav` and a click at the monitor ceiling in the headphones. One
  NaN wiped out the take's loudness figure for good and was sent to the
  headphone output as NaN. Found by the new mix-bus simulator.

### Added

- `sim_mix_bus`: the recording-mix and headphone limiters driven with eight
  microphones at full scale, trims moved mid-take, full-scale clicks, and
  drivers handing over NaN and infinity, at 16, 24 and 32 bits, with every file
  read back from disk. Runs in `ctest` on all three platforms and under
  AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.
- `Tools/e2e_ui_walk.sh` and `Source/UI/UiWalker.h` (test builds only): the
  real app walks every screen, control and card from inside the process, then
  is killed mid-take to prove the next launch offers the take back, then loses
  a microphone mid-take to prove the alert card, Keep recording and the saved
  take all work. Runs in CI with the other end-to-end gates.
- The macOS and Windows capture simulators each write to a folder of their
  own. They shared file names in the temp directory, so under `ctest -j` one
  deleted the other's recording mid-check and the Windows headphone-unplug
  scenario failed intermittently.
- `MMA_SHIM_FAIL_WHEN_FILE` in `Tools/alsa_readi_shim.cpp`, so a test can kill
  a device at the moment it chooses rather than a fixed time after it opened.
- Unit tests for the ffmpeg search order.

## v1.12.0 -- 2026-09-12 (release candidate)

This candidate concentrates on making failures visible, keeping the record of a
take durable, tightening device discovery and making the release artifacts
auditable. It is not a general-release claim: code signing/notarization and the
physical-hardware matrix remain open in `RELEASE_CHECKLIST.md`.

### Changed

- Recording inputs are restricted to directly attached external hardware and
  fail closed when identity is unknown. macOS admits USB, FireWire and
  Thunderbolt transports; Windows follows an endpoint into its Plug and Play
  tree and requires both an eligible wired branch and positive removable-device
  evidence on one node; Linux requires a kernel ALSA card whose sysfs ancestry
  says it is removable. Built-in computer, known phone/Continuity transports,
  Bluetooth/AirPlay, network, aggregate, virtual, internal and unknown inputs
  are omitted. Generic removable USB Audio Class hardware is admitted without
  name or vendor-ID guesses, so a phone or wireless receiver presenting exactly
  like an interface cannot be categorically excluded. Monitor-output choices
  are unaffected. Every backend now repeats the eligibility check immediately
  before opening an input stream, and Linux's virtual fixture is enabled only
  in a separately compiled test binary.
- Cameras now start off. Merely launching SobStage, opening the Cameras panel or
  connecting a camera does not enable it; the user must switch it on.
  That explicit choice is remembered, so previously enabled cameras reopen on
  later launches until they are switched off.
- Linux now asks an ALSA device how many channels it can actually open, then
  falls back to mono with an explicit warning instead of advertising silent
  phantom stems. The end-to-end verifier requires signal on every expected stem.
- Mid-take alerts distinguish urgent audio loss, warnings, recovery and ordinary
  news by severity; overflow is counted instead of silently dropping a fifth
  event.
- The initial recording destination is immediately usable at `~/RECORDINGS`.
  Removable-volume discovery continues on a detached worker and publishes card
  choices when it finishes, so a stale mount under `/Volumes` cannot keep the
  SobStage window from appearing or make shutdown wait for that scan.
- Destination status polling, write-speed preflight and interrupted-take
  recovery now use detached one-flight workers too. They never join a blocked
  filesystem call during launch, a location change or shutdown, and results
  from a destination that has since changed are discarded.
- Choosing or deferring a save location and previewing the next folder no
  longer stats a removable path on the interface thread. The selected path is
  validated by the detached recovery and speed gates, and revisiting the same
  mount name rechecks the physical medium now occupying it.
- Recording fails closed while the selected destination or enabled local backup
  is still being checked for an interrupted take, or when a required recovery
  scan or preflight cannot run. The disabled record control states what is being
  checked and what the operator can change or restart; a worker failure cannot
  silently become permission to record.
- Loudness advice now says “turn down” when true-peak headroom makes the computed
  gain negative and explains when a steadier next take is needed.

### Added

- `ActivityJournal` records starts, stops, failures and recoveries without
  letting repeated noise evict an unseen failure. The history appears in
  Advanced, `activity.log` in both copies of a take, and the application log.
- Microphone, output and camera arrivals and departures are announced by name,
  including swaps where the total device count does not change.
- Backend health reaches the UI and logs: failed hot-plug registration, stalled
  or unrecoverable streams, output loss, dropped backend frames, open failures,
  mirror failures and post-take combine failures are reported.
- The camera simulator now executes enumeration, stable identity,
  arrival/removal, open failure/retry, a device-list reorder during open, and
  mid-take unplug/replug continuity rather than only type-checking the camera
  source. A camera whose movie was interrupted stays absent for that take and
  its remembered preview returns only for the next one.
- Release packaging validates version/tag identity against the exact tip of
  `main`, pins third-party actions and JUCE by commit, and builds a universal
  macOS 13+ app. A tagged release signs and notarizes when Developer ID/notary
  and Authenticode credentials exist and otherwise ships ad-hoc/unsigned with
  the release notes saying so (the fail-closed gate added in the candidate cut
  blocked every release, since no certificate has ever been on file); when
  signing, it signs the outer DMG as well as the app, preserves
  hardened-runtime microphone/camera entitlements, and launches every opaque
  downloaded container again on a second runner before promotion.
- The portable Windows executable links its Visual C++ runtime statically, and
  the release gate inspects its PE imports so a clean machine is not surprised
  by an unbundled MSVC runtime DLL.
- Publication is staged as a private draft, downloads and verifies the remote
  checksums, and refuses to overwrite an existing release. Each candidate also
  carries a versioned, offline-buildable corresponding-source bundle containing
  the exact SobStage and JUCE revisions.
- Public support, privacy and release-checklist documents state what diagnostic
  exports contain, what is still unverified and how to roll back safely.

### Fixed

- Camera screens now share one native preview for each open device instead of
  recreating an AVFoundation preview layer on an already-running session.
  The macOS native view is now a real AppKit layer-hosting view, created in
  Apple's required layer-then-enable order, and the selected OS index is pinned
  to that camera's unique identifier so same-name capture cards do not all open
  the first device. Capture-session start/stop work runs on a serial background
  queue so a stuck card driver cannot hold the interface or app shutdown.
  Same-device retries replace stale placeholders, reported runtime capture
  errors replace the affected tile with an explanation, and an enabled HDMI
  capture card that disappears from the operating-system camera list remains
  visible as unavailable with reconnection guidance. This does not claim that
  every black frame can be diagnosed: the physical HDMI preview-and-recording
  gate remains open in `RELEASE_CHECKLIST.md`.
- Camera tiles and take arming now require a real image callback rather than a
  non-null native view. A bounded first-frame timeout and throttled heartbeat
  expose missing or lost HDMI signal, reject stale callbacks after reconnect,
  and allow a late live signal to recover without being mistaken for a new
  device.
- REC and movie metadata now follow backend evidence. macOS AVFoundation and
  Windows DirectShow report real writer start and finish results; a failed
  DirectShow graph never enters REC, and Stop waits asynchronously for movie
  finalization before metadata or ffmpeg combining can claim the file. A
  fifteen-second timeout quarantines a writer that never finishes, rapid retake
  is blocked while finalizing, and ordinary quit drains callbacks without
  joining a stuck camera driver.
- A missing capture card remains in the frozen watchdog roster without being
  promised as a file or written into `session.json` as a movie that never
  started. A connected busy/privacy-blocked camera retains that precise error,
  and a card which reconnects during a take is restored synchronously for an
  immediate next take.
- Camera discovery owns detached mailbox state, coalesces repeated refreshes
  and never joins AVFoundation or DirectShow enumeration at shutdown. A stale
  or late discovery result cannot reach a destroyed/replacement controller.
  SobStage also never attempts a fresh synchronous camera open after the audio
  writer has started, so a faulty capture-card open cannot strand a live take
  behind a frozen interface.
- Dismissing repaired takes no longer reads or rewrites their removable-card
  metadata on the interface thread. A detached acknowledgement worker owns the
  paths; an unavailable/read-only card leaves the metadata untouched so the
  take can be offered again on a later launch.
- `session.json` now records whether the mirror actually ran, so the end-to-end
  gate really compares every mirrored file. First-run save-location handling in
  that gate is also exercised.
- Durable text writes are verified after the write, preserve line endings and
  report failure. This covers settings, session metadata, activity logs,
  diagnostics inventory and recovered-take metadata, including a drive that
  fills during a take.
- Preflight throughput windows flush through the operating-system cache before
  their rates are accepted, and the final partial window participates, so page
  cache speed cannot masquerade as card speed.
- `activity.log` receives the recording stop and is updated during the take, so
  a crash or card loss does not leave a blank history. Global logs receive the
  same distinct events without repeat floods, in chronological order.
- Diagnostics and session metadata carry the real application version instead
  of the literal token `JUCE_APP_VERSION`.
- Corrupt or truncated settings and session metadata are detected and reported;
  mirrored interrupted takes are deduplicated in recovery.
- A full or unreadable destination is no longer silently collapsed into an
  “unknown” state. A running take keeps its Stop control available, and its
  destination cannot be changed underneath it.
- CoreAudio rate changes wait up to a bounded 500 ms for the asynchronous HAL
  update instead of rejecting a change on the first stale read-back.
- A saved explicit sample rate remains visible in Settings while every eligible
  external microphone is unplugged; the picker no longer turns into a blank
  control between rigs.
- A macOS input open now has a five-second UI deadline. If a USB driver's HAL
  start call wedges, its worker retains ownership and performs cleanup detached;
  already-open streams are quarantined behind it rather than making launch or
  quit wait for the driver's minutes-long timeout. One callback lifetime gate
  covers both the audio IOProc and nominal-rate, device-alive and overload
  property listeners, and drains admitted callbacks before their owner can be
  released. A driver that refuses to release callback client data stays safely
  quarantined for the rest of that process instead of being retried unsafely.
- Launch no longer closes and immediately reopens the audio rig after the first
  device-list callback has already started monitoring. That redundant reopen
  could send a fragile USB driver straight into a teardown/recreate stall before
  the first window appeared.
- Monitor selection carries the backend's built-in and sample-rate capability
  facts. A fixed-48 kHz HDMI capture-card output can no longer displace a
  compatible Mac output at launch or on hot-plug during a 44.1 kHz take. If a
  driver advertises compatibility but refuses the real output open, SobStage
  reopens the microphones input-only, keeps recording available, and shows
  that live headphone monitoring is off.
- Open CoreAudio devices now watch nominal-rate, alive and processor-overload
  events individually. Listener callbacks remain real-time-safe; the message
  thread reports typed failures, omits an immediately dead device, and
  automatically finalizes a recording if its live sample rate changes rather
  than continuing under an untrue WAV format.
- Windows COM teardown now occurs only when SobStage owns the initialization,
  and an installed ASIO driver no longer bypasses WASAPI exclusive-mode checks
  when no ASIO capture path exists.
- WASAPI enumeration reports real channel/rate capabilities and backend tests
  cover PCM formats, hot-plug and external-input policy.
- Windows packages now instantiate the device-property keys used by the
  removable-hardware proof, fixing a real-app link failure that the simulated
  backend could not expose.
- A take channel reads only its explicitly selected physical interface input;
  an adjacent disabled socket can no longer be substituted when the selected
  socket is silent. A known stereo-presenting USB microphone may inspect both
  sides only after a persisted analyzer verdict identifies it as one source.
- Oversized audio callbacks are processed in bounded, allocation-free slices
  instead of being silently dropped when they exceed the coordinator's scratch
  headroom. Any impossible scratch-layout failure is counted and silenced, and
  an oversized CoreAudio buffer keeps its physical channel slots so a later
  input can never be recorded under the wrong microphone's name.
- Fresh two-channel devices are now analyzed while both physical sockets remain
  independently routed. Mono/stereo verdicts and the live side of a collapsed
  microphone persist per port, so an immediate take cannot fall back to a
  silent side; a disabled socket never participates in the decision. The
  classifier uses the full three-second signal window rather than its final
  callback, and the 60-second all-silent fallback remains provisional so a
  quiet interface can still prove that it has two independent inputs later.
- Monitor controls and drift reporting no longer race the audio callback.
  Manual limiter resets cannot erase a newer feedback trip, and reconnecting a
  device cannot inherit sustained-drift time from its previous connection.
- WAV recovery bounds corrupt chunk sizes and handles RIFF padding. Periodic WAV
  header rewrites are flushed through the operating-system cache to storage,
  and split BWF files carry their sample offset from the session origin.
- Modal cards form a keyboard-focus boundary, preventing Tab or Shift-Tab from
  reaching recording controls behind a dialog.
- A writer ring reaching 90% now sheds both card and mirror stems together
  while preserving both MIX files; the mirror can no longer suppress the
  safety transition. A starved input span becomes counted silence instead of a
  held last sample that invented a DC signal.
- Linux hot-plug shutdown uses an explicit `eventfd` wake-up instead of relying
  on another thread's `close()` to interrupt `read()`. Repeated filesystem
  probes and camera enumeration now run on workers, and a failed camera open is
  retried only by an explicit user action.
- Output arrivals retain their true connection order, explicit output choices
  persist by stable ID, duplicate display names remain selectable, and the
  Settings picker no longer blanks during its refresh. Meter bindings rebuild
  only when capture changes instead of 60 times per second.
- Microphone meters now expose names, levels, clipping and actions to assistive
  technology, show visible keyboard focus, and open rename from the keyboard.
  The rename callback keeps the physical port/input target even if the rig
  changes while the dialog is open.
- macOS serious/critical thermal pressure now reaches the existing performance
  warning and activity journal; other systems remain explicitly unknown rather
  than guessed.
- Post-take ffmpeg combining owns detached state and polls a child process in
  bounded steps. Quit requests cancellation without joining a wedged muxer,
  kills its child and removes an incomplete combined file; the original video
  and WAV files remain untouched.

### Fixed after the 2026-09-12 candidate cut

- The §5.4 buffer ladder had never counted a single overrun: it is now fed from
  the backend's processor-overload signal, and only resets to its lowest rung
  when the device signature actually changes.
- macOS, Windows and Linux now ask the operating system for the microphone and
  camera privacy decision and report exactly what it answers, instead of
  assuming permission was granted.
- Linux asks ALSA whether the card can run at the take's rate before promising
  monitoring, and reports the monitoring latency and buffer size the device
  granted rather than the one that was requested.
- Several messages the user could never actually see are now delivered: the
  camera that failed to start is named, a movie that did not finish is called
  out, an ambiguous tap-to-name result is surfaced, a mirror-copy write failure
  is routed to the user, and the card's account of why it stopped is captured
  before the writers are destroyed instead of dying with the take.
- Interrupted-take recovery counts only files that are actually playable,
  excluding empty and unrepairable ones, so the headline no longer overstates
  what was salvaged.
- A/V synchronisation drift, a false camera alert, and loudness advice that was
  discarded at Stop are fixed; three controls now show the value that was saved
  rather than a constant.
- The CoreAudio and WASAPI fakes now refuse what the real APIs refuse: distinct
  endpoint and physical-device identities, unplugged and disabled endpoints,
  scope- and element-matched property-listener delivery, and real `Start()`
  refusals. The one-IOProc-per-device check now checks something.
- The unwired clock-master chain and its dead accessors were removed rather
  than left looking implemented.

- A removable volume pulled or wedged at the wrong moment can no longer
  freeze the window during diagnostics export (now a detached worker, one at a
  time), take-folder creation at Record start or the file listing at Stop
  (both bounded to five seconds, with a plain message naming the card that
  stopped answering). A take whose folder could not be listed is no longer
  called empty; the saved-take card reuses the Stop listing instead of
  reading the card a second time.
- Opening the take's files at Record start, the writer's final drain at Stop,
  and the `session.json` and activity-log writes are bounded the same way. A
  card that stops answering there stops the take from starting, or is
  reported as a Stop whose end may not have reached the card, instead of
  freezing the window. The abandoned write finishes on its own worker.
- Once a card step times out, the rest of that take skip the card instead of
  each waiting out its own deadline, so a card that dies at Stop costs one
  five-second pause rather than half a minute. A take whose card stopped
  answering is no longer reported as "Saved to" or "the audio itself is
  saved"; the saved-take card says the end may be missing and points at the
  backup. A refused start gives one reason instead of two.
- `Tools/e2e_card_stall.sh` makes the recording folder stop answering inside
  the real app (via `Tools/fs_stall_shim.cpp`) at Record and at Stop, and
  measures how long the window stopped responding with a test-build-only
  message-thread meter. It runs in CI and in the release workflow.
- **The §3.2 drift loop was unstable against real devices, and is fixed.** A
  device delivers whole blocks on its own clock, so the ring level a pull sees
  moves in steps of a block; one step, at the spec's gains, was a request past
  the ±200 PPM clamp, and the 5 PPM/s slew then unwound it for forty seconds
  while the level overshot the cushion. Every dry pull refunded a block, which
  the loop read as a fast device. Reproduced with zero jitter in a new
  deterministic simulator: a device 60 PPM slow never converged (the loop swung
  between 0 and −116 PPM), a device 150 PPM slow lost a block of audio every
  few seconds, and every microphone in the rig was reported at +200 PPM. The
  loop is now driven by the level with the device's progress since its last
  delivery folded in, lightly smoothed. Gains, clamp and slew are the spec's.
  In simulation all five test clocks (±150, ±60, 0 PPM) now lock within 1 PPM
  with no loss after the first minute.
- **Audio that arrives late, after silence has stood in for it, is skipped
  rather than played late.** A device thread that stalls hands over everything
  the driver held when it wakes; the span that burst covers is already in the
  file as silence. Playing it too left that channel a stall's length behind
  every other for as long as the loop took to drain it — minutes at 200 PPM,
  with the monitor path that much slower and the ring that much closer to
  full. The stream now skips it, up to the silence it answers for, at the
  first pull that finds the ring above target, and the loss is counted once.
  Silence that nothing arrives to answer, from a device that dropped the audio
  itself, is written off after a ring's worth of pulls.
- **The app's ring now holds a whole driver burst.** Linux asks the driver
  for eight periods of ring so a late reader finds its audio kept; the app's
  own ring was eight blocks with two of them the target fill, so the burst
  that ride-out produced overflowed it by two blocks — audio the hardware had
  kept, thrown away one layer up. The ring is sixteen blocks, the driver's
  stays eight, and a static assertion holds the two apart. No latency cost:
  the target fill is unchanged.
- When the software clock wakes from a stall the whole process shared, it
  gives the device threads a millisecond to land their bursts before it pulls
  the missed ticks; pulled first, they found the rings dry and wrote silence
  for audio that arrived a moment later.
- **A dry ring is now in the take's record.** A pull that came before the
  microphone's block wrote silence into the stem, and nothing ever said so:
  `session.json` carried the writer's drops, the ring overflow, the layout
  misses and the driver's drops, and not this one. It is reported now, per
  take, like the others.
- **Audio the driver lost no longer reads as a slow clock.** The drift
  measurement counts the samples a device delivered against its timestamps;
  a driver ring that overflowed while the reader thread was not running took
  a stall's worth out of that count in one step, which the fit read as the
  clock running slow for the minute the step sat in its window -- a
  microphone at +150 PPM reported at -330 after one xrun. The platform layer
  now tells the stream what the driver lost, and the measurement counts it
  as produced.
- **Reported drift is now measured, not read off the loop.** Each device's
  delivered samples and the consumer's pulled samples are fitted against their
  own timestamps over the 60-second window; the ratio is the device's clock
  against the pulling clock, within about 2 PPM at the minute. In the real app
  on simulated microphones set to +150 / −150 / 0 PPM it reported +149.8 /
  −149.5 / +0.1. The §3.3 "unreliable" flag and clock-master selection use the
  measurement; the loop's own figure remains available to the harnesses.
- **§5.4's buffer ladder now hears the app's own ring losses**, on every
  platform. It was fed only by CoreAudio's processor-overload property, which
  the other two backends never raise, so a machine whose scheduling jitter was
  wider than the two-block cushion sat at 64 samples losing audio with the
  ladder never stepping. A dry pull or a full ring now counts as a callback
  overrun; three inside thirty seconds steps up, and the streams reopen at the
  new size. On a virtual machine with 5–9 ms stalls the app stepped 64 → 128 →
  256 while monitoring and the take that followed lost nothing.
- The software clock (used when no monitor output is running, and when one is
  lost mid-take) no longer throws away ticks after a late wake, runs at the
  same real-time priority as the device threads, and begins a takeover on a
  fresh deadline rather than a burst of stale ones.
- Linux capture asks the driver for a period of one block and a buffer of
  eight, so a worker thread that wakes late finds its audio still in the
  driver's ring instead of dropped there; monitor latency is unchanged. An xrun
  is charged with what the gap actually cost rather than a flat period.
- **The Linux test fixture fed the app corrupt audio after thirty seconds.**
  ALSA's `file` plugin starts an input file over when it reaches the end, and
  from then on every block it delivers has a phase jump in it, for as long as
  the stream stays open; the fixture's tone files were thirty seconds long.
  It hid because the buffer ladder reopens the streams, which starts the
  files over, so a take usually began inside a fresh thirty seconds -- and
  showed as a stem whose tone the per-block vote could not recognise, on a
  take the app had recorded faithfully. The files are four minutes now, and
  `Tools/tone_timeline.py` prints where in a stem the tone is and is not.
- A microphone simulator: `Tools/alsa_readi_shim.cpp` can pace every ALSA
  device to its own clock (`MMA_SIM_REALTIME`, `MMA_SIM_PPM`) and emulate the
  driver's ring (`MMA_SIM_BUFFER_FRAMES`), so virtual microphones behave like
  independent USB crystals on real hardware instead of delivering audio as fast
  as a file can be read. `Tools/e2e_realtime_mics.sh` records a take on them
  with the real app and checks the tracks, the loss, the ladder and the app's
  drift measurement against the clocks it was given; it runs in CI and the
  release workflow. `Tools/sim_drift_loop.cpp` is the deterministic
  block-timed simulation of the loop, a test at every ladder rung with the
  jitter that rung is expected to absorb. `Tools/clock_trace.cpp` prints the
  loop as a time series for reading.

### Verification baseline

The candidate contains 576 unit tests, 201 CoreAudio simulator checks, 108
WASAPI simulator checks, 262 camera simulator checks, a seven-check synchronous
camera-lifecycle probe and an 11-check take-combiner probe, plus the capture,
refusal and end-to-end harnesses. These numbers describe automated coverage,
not physical-hardware certification.

CoreAudio input and output opens and closes now use bounded ownership paths;
the simulator covers stalled HAL calls, late callbacks, cleanup failure and
hot-plug listener lifetime. This is not evidence for the hardware matrix.
Developer ID/notarization, Windows Authenticode, physical audio takes and
visibly non-black HDMI capture-card preview and recording remain GA gates in
`RELEASE_CHECKLIST.md`.

## v1.11.0 -- 2026-09-08

### Added -- the take must prove itself on disk, or it stops

A day-long recording was lost because the app kept the button red and the
clock running over files that never grew. That cannot happen now:

- Three seconds after record is pressed, the files on the drive must have
  grown past their headers. If they have not, the take stops itself and a
  red card says "Recording failed. Nothing was recorded."
- If the files stop growing for six seconds mid-take, a red card says the
  drive has stopped taking audio and what to do, while the take carries on.
- If no sound above -60 dBFS reaches the app for twenty seconds, the card
  says the files hold silence and names the mixer checks.
- The record button is disabled, with the reason, whenever the microphones
  are not actually open -- an output that refused low-latency mode, a
  microphone held by another app.

The rules live in Core (RecordingProof) under seven tests. A second
end-to-end gate, `Tools/e2e_refusal.sh`, runs the real app with
microphones that cannot open and requires that no take of empty files is
left behind; it passes alongside `Tools/e2e_app_take.sh`.

## v1.10.0 -- 2026-09-08

### Fixed -- the audit's silent-loss findings, proven end to end

- **Recording no longer depends on the headphone output.** The rig was
  pulled onto the output device's callback, so with no output selected
  nothing recorded and nothing said so, and an output unplugged mid-take
  froze the take silently. A software clock now pulls whenever the output
  is absent or stops calling back; the mid-take card says "The headphone
  output has stopped" and the take carries on. Reproduced for real: a
  headless take on this rig used to leave seven 0-byte files, and now
  leaves seven files of audio.
- **32-bit takes are written correctly.** The header said 32-bit over
  24-bit samples; every file was unreadable. The packer now writes 32-bit
  integer PCM, and a depth it cannot pack is refused.
- **Dropped audio is always counted.** A full input ring discarded samples
  without counting them; they now reach the mid-take card, said once and
  then at most once a minute with the running total.
- **Unplugging an interface silences every socket it carries**, not only
  the first, and every socket comes back together.
- **The first take of every launch gets its backup copy.** The mirror
  decision was read before it was made. It is now made at arm, before the
  folders exist, and a mirror folder never overwrites an earlier one with
  the same name.
- **Changes refused mid-take are applied when the take ends** (a mic
  plugged in, an unplug, a rename, a rate change), so the next take's plan
  and its streams agree.
- **Trim sliders act on the right device** on an interface with several
  sockets; unticking a mic in Settings mid-take no longer reads as an
  unplug; a mic switched off stays off across an unplug.
- **"The drive is full. Recording has stopped" now stops**, and unknown
  free space is no longer reported as a full drive.
- **Recovered takes are offered once.** After the Recovered card, each take
  is marked finished, so it does not come back at every launch.
- A lock-free hand-off now uses sequentially consistent ordering (a
  nanosecond crash window on Intel and Windows); a 24-bit decode no longer
  left-shifts a negative value; a Windows notification class is final.

### Added -- an end-to-end gate

`Tools/e2e_app_take.sh` starts the real app headless, records a real take
through the real ALSA backend against the virtual microphones, stops it,
and `Tools/verify_take.py` checks every file: bit depth and block align as
configured, length, signal, the right tone on the right stem, a stop
timestamp, and the mirror copy's sizes when the mirror ran. No release
ships without it passing.

## v1.9.0 -- 2026-09-06

### Added -- a pop-up the moment something goes wrong mid-take

If a microphone stops sending sound, a camera goes away, sound is dropped,
the drive falls behind or drops to mix-only, the monitor path breaks, or
the drive is down to ten or two minutes of room, a card comes up over the
main screen the moment it happens and says so in plain words, with how far
into the take it was. The take carries on behind it: Keep recording (or
Escape) dismisses the card, Stop recording is the same press as the record
button. Good news -- a mic or camera coming back -- joins the card without
raising it. Each change is said once; a mic that was already dead when
record was pressed was on screen already and is not an alert. Cameras are
re-listed every two seconds during a take, because the system does not
announce one being unplugged, it just stops listing it.

The rules live in Core (TakeWatchdog) under twelve tests; the card is one
more ModalCard beside the save prompt and the saved-take card.

## v1.8.0 -- 2026-09-06

### Changed -- Settings and Help are a drawer beside the main screen

Settings used to replace the whole window, so every trip to a picker was a
trip away from the meters, the pictures and the record button. It now opens
as a drawer down the right-hand side, scrolling on its own, with the main
screen still live on the left: the skulls keep filling, the camera tiles
keep running, and recording can be started without closing anything. Help
opens the same way. The Settings or Help button stays lit while its drawer
is open and closes it again; so does Close at the top of the drawer, and
Escape. The window widens (never narrows) so both halves fit, and the
footer keeps to one line at the narrower width. The camera panel still
takes the whole window, because it owns the live viewers while it is up.

## v1.7.1 -- 2026-09-06

### Changed -- controls that look like controls, and are easier to hit

Buttons were filled in the same tone as the panel behind them with a hairline
nobody could see, so a screen of buttons read as a screen of labels, and the
smallest of them were 24 px tall. Every button, picker and tick box is now
one step lighter than its panel with a visible edge, brightens plainly under
the pointer, and the pointer becomes a hand over anything that can be
pressed, including a strip's name. Sizes went up across the app: header
buttons 36 px, Settings rows 32 px, tick boxes 20 px with the whole row as
the target, menu rows 32 px, card buttons 38 px, and the footer 40 px.
Button and menu text is 14 px everywhere instead of scaling down with the
control. Escape closes Settings, Cameras or Help.

## v1.7.0 -- 2026-09-05

### Added -- a Help screen

A third door beside Settings, and a Help button on the Settings screen too.
It answers "why is it silent?" in the app, in the order the causes actually
turn up: the checklist for a mixer or interface (the box ticked in Settings,
microphone permission, the channel unmuted and its faders up, the USB send --
LOOPBACK on a PUPGSIS T12S -- switched on, the gain up), whether the mic is
dynamic or a condenser needing 48 V the mixer may not have, what the amber
line under the strips means and what to do about each cause it names, what
to choose for sample rate, bit depth and buffer size and why, how an
interface with several sockets is shown and named, where the files are, and
what to send when none of that applied. Open Settings and Export diagnostics
are buttons under the last topic.

The words live in Core rather than in the UI, so a test holds them to
account: the mixer checklist stays first and keeps naming the causes this
app has actually shipped fixes for.

## v1.6.0 -- 2026-09-05

### Added -- choose which sockets of an interface to record

An interface is one row in Settings, with a tick box per physical input
indented under it. An eight-input box with two people on it used to record
eight files, six of them silence, and report room for a fraction of the take
it could actually hold. Untick the unused sockets and they are not recorded:
no strip, no file, no share of the disk estimate. The sockets left keep their
physical numbers, so "input 3" is still the one labelled 3 on the box.

### Added -- name each socket of an interface separately

Clicking a strip's name used to name the whole box, so both people on a
two-input interface became "Kitchen 1" and "Kitchen 2". On an interface each
socket is a person, and naming a strip now names that socket: "Alex" and
"Sam", used on the strips and in the stem filenames, with no socket number
after a name that already says who it is. A single-input microphone still
takes the name as before.

Both are port memory (§2.4): they follow the interface across a replug and a
relaunch, verified by a round-trip test.

## v1.5.1 -- 2026-09-05

### Changed -- the clock master is this computer, always; the picker is gone

Every microphone was already corrected onto the output clock -- the machine's
own. Naming one microphone as "master" changed nothing about that path; it
only moved which crystal the drift figures were quoted against, and handed the
user a picker for a choice with no audible consequence. (The picker had also
been showing nothing selected since v1.2.1, because it was filled with
per-input strip names while its selection was a device name.)

Drift is now measured against this computer, so every microphone's figure
means the same thing, no master can be unplugged mid-take, and Settings shows
"This computer" where the picker was.

### Documentation

The Settings screenshot in the README was two versions stale: it showed sample
rate, bit depth and buffer size as read-only text, and the microphone rows
without their input counts. Retaken from the current build.

## v1.5.0 -- 2026-09-05

### Added -- sample rate, bit depth and buffer size are controls, like Audio MIDI Setup

All three were read-only lines in Settings, and the app once told a user to
change one of them there. They are pickers now:

- **Sample rate** offers every rate any recorded microphone reports, plus the
  rate each is running at. A pinned rate is honoured, full stop. The earlier
  rule quietly dropped a pin the rig "could not reach" and fell back to
  automatic, which from the user's side is a control that does nothing. If the
  hardware refuses, the main screen names the device and both rates.
- **Bit depth**: 16, 24 or 32. Applies to the next take; no stream is reopened.
- **Buffer size**: Automatic (§5.4's ladder) or a fixed 64 to 1024 samples.

All three persist across relaunch, verified by a round-trip test.

## v1.4.2 -- 2026-09-05

### Verified -- the backend actually reports the rate a device is running at

v1.4.1's rule depends on one number: the rate the interface is on right now.
Nothing checked that CoreAudioBackend reports it. A field left at 0 would have
silently reverted the whole rule to highest-common -- the same way v1.4.0
shipped unable to fire. `sim_coreaudio` now drives the real backend against a
fake interface sitting at 44.1 kHz and asserts it says 44100.

### Fixed -- a key mismatch can no longer silently reselect 48 kHz

If the microphones in the take fail to match what the OS listed, the negotiator
would have been handed an empty list and returned its 48 kHz default -- the
exact failure this rule exists to end, through a side door. Every enumerated
device votes instead, and a debug build asserts.

## v1.4.1 -- 2026-09-04

### Fixed -- a microphone nobody is recording no longer decides the sample rate

v1.4.0 was supposed to keep the recording on whatever rate the interface was
already using. On the rig that reported it, nothing changed: the app still
demanded 48 kHz from hardware locked at 44.1, and still would not open.

The vote was taken over every microphone the OS lists, not the ones being
recorded. A MacBook's built-in microphone sitting at 48 kHz -- switched off, no
strip on screen, no file in the take -- outvoted the interface the recording
actually runs on. The rig "disagreed", so v1.4.0's rule fell through to
highest-common, which is exactly where it came in.

A microphone nobody is recording cannot resample, cannot drift, and cannot be
harmed by the choice. Letting it constrain the rate only ever costs the
microphones that are. Only included devices vote now.

### Fixed -- the rate a device is running at counts as a rate it supports

An interface advertising only 48 kHz while sitting at 44.1 is doing 44.1. Some
report only the rate they would prefer, and taking that list as the whole truth
ruled out the one rate guaranteed to work -- the one already running.

### The rule now lives where the tests can reach it

`SampleRateNegotiator::votingDevices()` decides who votes, in Core, tested. It
was app code, which needs JUCE and no headless test can reach -- which is how
v1.4.0 shipped a fix that could not fire on the hardware that reported the bug.
The same mistake as v1.1.0.

## v1.4.0 -- 2026-09-04

### Fixed -- the app stays on the rate your interface is already using

A PUPGSIS mixer running at 44.1 kHz, which advertises 48 kHz and then refuses
to switch to it, could not record at all:

    PUPGSIS-T12S 1 couldn't be opened for recording. This interface is running
    at 44.1 kHz and won't change to the 48 kHz this recording uses.

§2.2 chose the highest rate common to every device, which is right on paper and
worth nothing when the hardware will not move. An interface clock-locked to
44.1, or one another process has a claim on, lists 48 kHz among its
capabilities and refuses the write -- and the take never starts.

The rule now has one exception: when every device is ALREADY running at one
common rate, that rate is chosen even if a higher one is also available.
Switching is the step that fails; staying put cannot. The difference between
44.1 and 48 kHz is inaudible next to a recording that did not happen. A rig
whose devices disagree still gets highest-common, and §3 resamples whichever
cannot follow.

### Added -- Sample rate is a control in Settings, not a read-only line

The message above told people to "set the recording to 44.1 kHz in Settings".
Settings displayed the sample rate and offered no way to change it, so the one
instruction on screen named a control that did not exist.

It is now a picker, defaulting to **Automatic**, listing the rates every
microphone in the rig can actually reach. The choice is remembered, and is
ignored if the rig later cannot reach it -- a rate pinned for last week's
interface must not silently break this week's.

## v1.3.0 -- 2026-09-04

### Fixed -- a mixer that is also your headphones is opened once, not twice

A small livestream mixer presents its microphone inputs and its monitor output
as a SINGLE duplex device. SobStage opened it for output, took hog mode on it --
§5.4 requires the monitor path be exclusive -- and then opened it a second time
for input. That asks macOS for another claim on a device this process has just
taken exclusively, and the refusal arrives as:

    <mic name> couldn't be opened for recording.

against a microphone that is plugged in and working. Because every take channel
on that rig comes off the one device, the whole capture failed, which is a
silent recording from hardware with nothing wrong with it.

The device is now opened once and that single stream carries both halves of the
cycle: the microphones are read from the same callback that fills the
headphones, which is also how CoreAudio means a duplex device to be driven. A
rig where the microphones and the output are different boxes is untouched.

### Fixed -- an oversized callback no longer drops the whole block in silence

CoreAudio is allowed to hand a callback more frames than the buffer size that
was asked for, and `CoreAudioBackend` sizes its own scratch for exactly that.
The coordinator did not: one oversized slice made the entire pull return early,
so no audio, no meters and no error -- the silent failure §0.1 forbids. Its
scratch now carries the same headroom the backend gives its own.

### Documentation

A diagram of how a rig becomes tracks, covering the single-input microphone, the
multi-input interface, and the duplex mixer -- the mapping that was got wrong
repeatedly and is impossible to see from the UI alone.

## v1.2.2 -- 2026-09-04

### Fixed -- when a microphone won't open, the app says why

"PUPGSIS-T12S 1 couldn't be opened for recording." was the whole message, and
it named a microphone rather than a cause. Four different faults produced that
one sentence, and they have four different fixes:

- the interface is running at a sample rate the recording isn't using
- macOS has not granted SobStage microphone access
- another app is holding the interface
- the microphone was unplugged between being listed and being opened

The backend knew which of those it was at the moment it failed and threw the
answer away. §0.1 is about audio, but the principle is the same: the app knew
and said nothing, and the person was left to guess. The monitor-output path had
always reported its cause; the microphone path now does too, naming the rates
involved where a rate is the problem, and where to change them.

### Fixed -- a reason that doesn't fit on one line is no longer clipped

The problem line was a fixed 20px band, so a message long enough to explain
anything was cut off after a few words -- which left exactly the dead end the
message was written to end. It now grows with its text, capped at four lines so
a runaway message can never push the record button off the bottom of the
window, and `layout_probe` checks the screen still fits.

## v1.2.1 -- 2026-09-04

### Fixed -- the screen agreed with the files about how many microphones there are

v1.2.0 made a two-input interface record two tracks. It did not make the app
*look* like it had. Everything the user sees before pressing record was still
counted per device, while the recording itself was counted per input, and the
two answers disagreed:

- The main screen showed **one** meter strip for a two-input interface, and
  only grew to two at the moment recording started. Before that, the app
  looked exactly like one that could not see the second microphone -- which is
  how it was reported.
- The Settings list showed one row for the interface with nothing to say it
  carried two microphones, so the row read as the app refusing the second mic.
- §6.4's remaining-time figure was computed for the device count, so it
  promised twice the recording time a two-input interface would actually fit,
  and four times on a four-input one. The disk cannot keep that promise.

### Fixed -- an interface's microphones are told apart on screen, not just on disk

Asked for the name of a channel, the app answered per device. On a four-input
interface that gave four identically-named strips over four correctly-named
files: the meters were right, the files were right, and there was no way to
tell which meter belonged to which person.

### How this is prevented from coming back

The rule was written down twice -- once in the capture builder, per input, and
once in the screen's accessors, per device -- and the copies drifted. There is
now one description of what a rig records, `planChannels()` in Core, and every
caller resolves through it. It is Core rather than app code for the reason
`takeChannelsForDevice` was moved there: the app's builder needs JUCE and the
headless tests cannot reach it, so a rule that lives only there is a rule
nothing checks.

## v1.2.0 — 2026-09-04

### Fixed -- two microphones on a two-input interface are both recorded

A two-input interface with two people plugged into it is the commonest
small multi-mic rig there is, and this app collapsed it to a single track.

Two channels were assumed to be a stereo USB microphone presenting the same
voice on both sides, so §2.1 picked one side and discarded the other. One of
the two people was thrown away without a word -- and if the discarded side
was the one carrying the microphone that mattered, the whole take came back
silent from a rig that was working perfectly.

v1.1.0 addressed this for interfaces with more than two inputs and left
two-input devices collapsing exactly as before, which is why it changed
nothing for the rig that reported it.

§0.1 settles which way to guess when the app cannot tell the two apart.
Keeping both sides of a duplicated mono microphone costs a redundant file,
which is untidy. Collapsing two microphones into one loses somebody's audio
entirely, with nothing said. Those are not comparable, so both sides are now
kept until §2.1's analyzer has actually examined the audio and decided they
carry the same source -- a verdict §2.4 already remembers per port, so a
stereo USB mic still collapses correctly from its second take onward.

Covered in Tools/sim_capture_mac by the failure as reported: a two-input
interface where only the second person is speaking, checked to confirm both
files exist at full length and the talking one carries signal.

## v1.1.1 — 2026-09-04

### Fixed -- the interface fix in v1.1.0 never actually took effect

v1.1.0 made a device contribute one take channel per input it presents.
For anyone whose interface was already in the device list, it changed
nothing at all, and the report was exactly that: absolutely nothing
changed.

How many inputs a device has is the OS's to say, and syncToEnumeration
refreshes what the OS owns each time the device list changes -- the display
name, whether it is built in. The input count was not in that list. So a
device that arrived by any other route, or was present before the first
enumeration, kept the default of one input for the rest of the session. The
app went on believing a four-microphone interface had one microphone on it,
and the v1.1.0 rule never fired.

The count is now refreshed like every other fact the OS owns.

### Added -- the rule that decides this is testable now

The decision lived inside the app's channel builder, which needs JUCE and so
cannot be reached by the Core tests at all. That is how it shipped untested
and how the stale count went unnoticed: nothing anywhere could have caught
either.

takeChannelsForDevice() is a plain Core function now, with the reconcile
covered by a test that fails against the old code.

## v1.1.0 — 2026-09-04

### Fixed -- an audio interface's microphones are all recorded, not just one

Reported from a real rig: "it's not letting me add the mics I have
connected to my interface."

One device is not one microphone. An interface with four microphones
plugged into it is a single device presenting four inputs, and this app
took exactly one channel from any device it opened. §2.1's stereo collapse
-- written for a USB mic that presents the same voice on both sides -- was
being applied to interfaces, where the two sides are two different people.
It picked one and discarded the rest without a word. Inputs three and four
were never even read.

Anyone recording several people through one interface got one of them. If
their own microphone happened to be on a discarded input, they got a
silent recording and nothing on screen explaining why.

§2.1 had already said what should happen: collapse to mono when a side is
silent or duplicated, "otherwise record true stereo". The otherwise was
never implemented.

A device now contributes one take channel per input it presents, each with
its own strip, its own name and its own file. Two-input devices are
unchanged -- that is the USB-mic case §2.1 was written for, and the
analyzer still decides. Above two, a device is an interface and every input
is a microphone.

The streams are also grouped by device now. Opening one stream per channel
would have asked the OS for the same exclusive device once per microphone,
and on macOS the second request is refused -- the take would have died
naming a microphone that was plugged in and working.

Covered by Tools/sim_capture_mac: a four-input interface carrying a
different tone on each input, recorded through the real coordinator, with
every tone checked into its own file.

## v1.0.3 — 2026-09-04

### Added -- the macOS recording path is tested end to end

sim_coreaudio proved the backend hands over the right samples. live_capture
proved the coordinator and writer turn samples into files. Nothing joined
them, and the join is where macOS actually lives -- so three things reached
users untested:

- live_capture's fixture microphones are mono, so the stereo path, which is
  what a USB mixer or any stereo interface takes through §2.1's
  channel-layout analysis, had never reached a file in any test.
- every harness and every unit test recorded at 16 bits. The app ships 24.
- the backend had only ever been asked for audio in isolation, never while
  a take was running.

Tools/sim_capture_mac closes all three: a stereo interleaved device, at 24
bits, recorded through the real coordinator into real files, with the bytes
checked rather than the file size. It runs in CI on every platform.

### Added -- a silent take now says whose fault it is

When a take comes back with no sound, the single most useful thing to know
is whether any audio arrived. If none did, the rig is worth checking: a
mute switch, a cable, the wrong input selected. If audio DID arrive and
none of it reached the files, nothing about the rig explains it -- the
samples were here and this program lost them.

The app could not tell those apart, so it said "silent" for both, and the
only available next step was to go and check hardware that may have been
working perfectly the whole time.

The capture path now records the loudest sample that ARRIVED, alongside the
loudest the writer managed to WRITE. When audio arrived and nothing was
written, the take is reported as a fault in SobStage, in those words,
rather than as a silent microphone.

This is the §0.1 rule taken one step further. Reporting that audio was lost
is the requirement; saying which half of the system lost it is what makes
the report worth reading.

## v1.0.2 — 2026-09-04

### Fixed -- a take could record silence while the meters showed signal

Reported from a real Mac: the on-screen meters moved while the user
talked, and the files came out silent.

The capture path hands the writer whatever channels the device delivered,
clamped to the take's channel list. The writer then required that count to
match the take's exactly, and rejected the whole block when it did not --
every channel of it, including the ones that had arrived intact. If a
device simply reports fewer inputs than the take was built for, that is not
an occasional dropped block: it is the entire recording.

It was invisible on screen because metering happens after, reads the same
buffer, and has no equivalent check. So the app displayed live level for
audio it was dropping on the floor, which is §0.1's unacceptable failure at
its largest -- everything lost, nothing said.

The earlier audit found this code and fixed the wrong half of it. It saw
that a rejected block left framesDropped at zero and made it count instead,
which made the loss visible without stopping it. The remedy was wrong: §6.5
already settles what to do when a channel is not there, for the case of a
mic unplugged mid-take -- the file layout holds and that channel writes
silence. A short block is the same problem and now gets the same answer, so
every channel that arrives is written.

A block carrying MORE channels than the take is the genuinely lossy case,
since those samples have nowhere to go. Those are still counted, and the
channels that fit are still written rather than the whole block going in
the bin.

## v1.0.1 — 2026-09-04

### Fixed -- a take that records silence is no longer called saved

Reported from a real Mac: the files came out with no sound in them, and the
app said nothing about it.

The rule that decides whether a take holds audio judged it by file size
alone -- under a kilobyte per file meant nothing was written. That catches
a card pulled before any audio landed, and misses the failure people
actually hit. A device that is present and streaming digital silence -- a
microphone muted at its own switch, a dead channel on an interface, a USB
board whose audio never carried signal -- fills every stem for the full
length of the take. The files are megabytes. The rule weighed the bytes,
found plenty, and the app said "Saved." over a folder holding nothing.

That is §0.1's one unacceptable failure: audio lost without a word.

The writer now tracks the loudest sample it writes, and the verdict uses
that alongside the file sizes. Three outcomes, and the two failures are
told apart because they send you to look at different things:

- nothing arrived -- "the files are empty, no audio reached the drive"
- a stream ran and was flat -- "the files are silent, the microphones were
  connected but sent no sound"
- a real take -- "Saved to ..."

The silence bar is -90 dBFS. A 24-bit LSB sits near -138 dBFS and preamp
noise in a quiet room is far above -90, so a whisper is still a recording;
only a genuinely dead stream falls under it.

The saved-take panel is now told the verdict rather than working it out.
It can see file sizes and nothing else, so from there a silent take and a
good one are the same list of megabyte files -- and a panel reaching its
own verdict is how it once came to warn that files were empty while the
status line beside it said "Saved to ...".

## v1.0.0 — 2026-09-03

The first stable release. No code changes from v0.9.4.

> **Correction, 12 September 2026:** the original note said the camera work had
> been confirmed on real hardware. Later log review confirms something narrower:
> the older v1.11.0 app opened a physical USB HDMI capture device reported as
> `USB2 Video`, and AVFoundation enqueued a first frame. It does not confirm that
> SobStage displayed a visibly non-black preview or completed a camera recording.
> Those hardware gates remain outstanding for the v1.12.0 candidate.

### What 1.0 claims

The feature set is complete and the interface has been used on a Mac. Eight
microphones to separate tracks, a live mix bus, camera preview and capture,
combined video-and-audio output, streaming-loudness targets, and session
recovery are all implemented and covered by 375 tests plus two capture
harnesses that run on every commit.

### What 1.0 does not claim

Four things remain unproven against real hardware, and 1.0 does not pretend
otherwise -- see the platform table in README.md:

- **No recording has been made from a physical microphone.** The CoreAudio
  and WASAPI device layers are exercised every commit against simulated
  hosts that reproduce the awkward shapes real devices take, but a real
  driver's timing and firmware quirks are not something a simulation
  reproduces. Linux exercises the real ALSA API with virtual PCMs, not a
  physical sound card.
- **No real camera has completed preview and recording.** Camera discovery and
  selection now run in simulation, but open, preview and capture still require
  the macOS/Windows hardware matrix.
- **The ffmpeg muxing has never run.** Combining picture and sound is built
  and tested as a command, not as an execution. It needs ffmpeg installed
  (on a Mac: brew install ffmpeg).
- **The loudness meter has only met synthetic signals.** It matches the
  BS.1770-4 reference tones to within 0.02 LU, which is the right check, but
  not the same as a real take.

A first stable release is a statement that the software is finished enough
to depend on, not that every path has been walked. These are the paths that
have not.

## v0.9.4 — 2026-09-03

### Fixed -- the camera picture fits the window it opens in

The picture was measured against the wrong height, and three faults came
out of that one cause.

MainScreen gives the picture whatever height is left over, and took that
from its own getHeight(). But the owner sizes MainScreen to
max(viewport, requiredHeight) -- so a taller picture grew the canvas,
which offered more spare height, which grew the picture again. It settled
at the fraction cap: 1007px of content inside the 560px window the app
opens at. The picture overflowed the window, and the record button went
below the fold.

It is now measured against the viewport -- what the user can actually see
-- which is an input from the owner rather than something derived from the
content, so the loop cannot form.

### Fixed -- switching a camera on makes room for it

That change alone left the picture small at launch (352px wide), because
the window opens sized for an audio-only rig and never grew when a camera
arrived. Switching one on now grows the window to fit it, bounded by the
display, and only ever upward: a window sized by hand is never shrunk
behind the user's back.

At the size the app opens at, the picture is now 1146x644 -- the full
width of the window.

### Fixed -- no more empty band above the footer

With the window free to grow, the opening height no longer has to reserve
space for cameras nobody switched on, so it drops from 560 to the window's
own 420 minimum. An audio-only rig had been opening with roughly 250px of
empty background between the last status line and the footer.

### Added -- the layout check now tests the size the window actually opens at

Every case in Tools/layout_probe had assumed a window someone had already
dragged bigger, which is exactly how a picture that was small on launch
went out reported as large. It now covers the real opening size and prints
OVERFLOWS WINDOW when content does not fit, so this class of fault fails
loudly instead of needing to be noticed.

## v0.9.3 — 2026-09-03

### Fixed -- the app now reports the version it actually is

`getApplicationVersion()` returned a hardcoded "0.1.0". The window title
had therefore misreported the version on every build from v0.1.0 to
v0.9.2 -- nine releases -- and the one place a user could read it was
wrong the whole time.

It now comes from the version in CMakeLists.txt, which is the single
place the number is written down, so it cannot go stale again.

### Added -- the version is on screen

Beside the tagline, at the top of the main window. "Which build am I
running" is the first question asked when a change appears not to have
arrived, and an answer that takes hunting through menus is an answer
nobody checks.

This is not hypothetical. A report that the enlarged camera picture had
not changed turned out to be an older build still installed, and nothing
in the app could have shown that.


## v0.9.2 — 2026-09-03

### Changed -- the camera picture is as big as the window will allow

The live picture topped out at 456 pixels wide in a 760-pixel window. That
was enough to tell someone was in shot and not enough to judge focus or
framing on, which is the entire reason the picture is on the main screen
rather than behind a door.

It is now sized as a share of the window rather than as a fixed number of
pixels, defaulting to the whole of the width available. So making the window
bigger makes the picture bigger -- which is what dragging a window larger was
asking for, and what the old fixed sizes answered with more empty background.

It grows in both directions. The picture is given the height left after
everything else on the screen has been laid out, so a taller window is a
taller picture, and a picture can never grow far enough to push the record
button off the bottom.

On an ordinary 1680x1050 display that is a 1223x687 picture where the old
maximum was 456x256 -- about seven times the area -- and on a larger display
it simply keeps going.

The arrows still work exactly as before, and now choose how much of the
window to spend on the shot rather than picking from a list of fixed sizes.
A size chosen before this release is kept; the untouched default is lifted,
since it was never really a choice.

### Fixed -- the window could open larger than the screen

Sizing the window to its content and centring it did not clamp to the
display, so a window taller than the screen hung off both ends of it and took
the record button with it. Harmless while the pictures were thumbnails, and
not once they were worth looking at. The window is now bounded by the display
it opens on, and the screen scrolls if its content is taller.

## v0.9.1 — 2026-09-03

### Changed -- the combined file no longer re-encodes the sound

The picture was always copied bit for bit. The sound was not: it was going out
as 256 kbps AAC, which is transparent enough for most listening and still
throws the take away. The stems are 24-bit PCM, and a lossy codec is a one-way
door -- nobody records at 24 bits in order to deliver a generation-loss copy of
it, and a combined file worse than the parts it was made from is not worth
making.

The audio is now kept as it was recorded: 24-bit PCM in a `.mov`, or FLAC in
the Matroska case, which is also lossless. Neither stream is re-encoded any
more, so the combined file is exactly as good as the stems and the video beside
it.

That changed the container on macOS from `.mp4` to `.mov`. mp4's PCM support is
an afterthought, so audio in an mp4 is lossy in practice; `.mov` carries PCM
natively, is what the camera already writes, and opens in every editor.

The audio is also written at the depth the take was actually recorded at rather
than a fixed one, so a 24-bit take does not quietly lose its bottom eight bits
on the way out.

## v0.9.0 — 2026-09-03

### Added -- aim the loudness at where the take is actually going

Every streaming service turns everything it plays to the same loudness. So
how loud a take is decides what a listener hears, and the peak meters this app
already had say nothing about it: two takes peaking at the same number can be
six decibels apart to the ear, and it is the louder one the platform turns
down.

Pick where the take is going in Settings and the app measures the mix the way
the platforms do -- ITU-R BS.1770, K-weighted and gated, the same standard they
all normalise against -- then says which way to move and by how much. Nothing
is changed for you. The stems stay at unity, as they always have.

The gating matters more than it sounds. Ungated, a recording of someone talking
with pauses measures quieter than the same voice without them, so the advice
would be "turn it up" for nothing more than leaving space to breathe. The two
gates in the standard are what stop that.

Two things the app now knows that are easy to get wrong:

**Mono needs a different number.** Every file this app writes is mono, and a
mono file played through both speakers is the same signal twice -- which
measures 3 LU louder than the single channel. Delivered at Spotify's published
-14, a mono take plays back at -11: three decibels hotter than everything
around it, every time. So the aim is -17 mono for Spotify, -19 for Apple
Podcasts, and the advice says so rather than quietly applying it.

**It will not tell you to clip.** If a take is under the target but its peaks
are already near the platform's ceiling, the suggested gain is cut to what the
ceiling allows and the app says why. Meeting a loudness figure by clipping
trades a number the platform would have fixed anyway for distortion it cannot.

True peak is measured rather than assumed from the sample peak, because a
waveform can pass between two samples higher than either -- a file that looks
like it sits at -1 dBFS can still clip a platform's decoder.

Targets are the platforms' own published figures: Spotify, YouTube, Amazon and
Tidal at -14; Apple Music and Apple Podcasts at -16; EBU R128 broadcast at -23.
Off by default -- someone recording a rehearsal is not delivering anywhere, and
a number telling them they are 8 dB under Spotify is noise.

### Changed -- the app is called SobStage

The name changes everywhere it is visible: the window, the .app bundle, the
Windows executable, the DMG, and the virtual device other apps see in their
input list. Anyone who had renamed that device keeps their own name; anyone who
had not will find "SobStage" where "Multi-Mic Aggregator" used to be, and will
need to pick it again once in Zoom, OBS or whatever else was pointed at it.

Everything the app remembers about a rig moves with it. Every microphone's name
and trim, which ones are switched off, the destination folder, the backup
setting and the camera choices all live in a folder named after the app, and a
rename that simply looked somewhere new would have presented as all of it being
forgotten -- which is exactly the thing the settings file exists to prevent. The
old folder is moved across the first time the new name is used.

The internal namespace is still `mma`. It is not visible anywhere a user can
look, and renaming it would have touched a hundred files without changing
anything.

### Added -- save the video with the sound in one file

The picture and the sound have always been written separately, for a reason
worth keeping: the platform camera capture is video-only, the microphones are
the sound, and one clean track per person is the point of the rig. But separate
files mean opening an editor before anyone can watch, send or upload a take.

Switching this on writes one more file per camera with both in it, beside the
originals rather than instead of them. The stems, the mix and the silent video
all stay exactly where they were, so a combined file that fails to appear costs
nothing that was not already saved. It is off by default: it costs disk and
minutes of processing after every take, and nobody who does not want it pays.

The sound is lined up rather than assumed to match. The stems start before any
camera is asked to record, so a camera's file begins a fraction of a second into
the take -- small enough to look like nothing, large enough to look wrong.
How far in is measured for each camera as it starts, and that much is trimmed
off the front of the audio.

The picture is copied rather than re-encoded, so the file appears in minutes
rather than hours and loses nothing. That means the container has to be one that
can carry what the camera already wrote: mp4 on macOS, Matroska on Windows,
whose format mp4 cannot legally hold.

This needs ffmpeg, which the app cannot install for anyone. If it is missing,
the toggle says so and says how to get it -- before a take rather than after
one, since finding out afterwards is too late to do anything about.

## v0.8.0 — 2026-09-01

### Added -- the camera pictures can be resized from the main screen

A fixed 176px tile was a guess, and the wrong one for half the rigs it will
meet. One camera across a table wants a picture you can judge focus on; four in
a row want to fit. Which of those someone is doing is not something the app can
work out for them, so it is a control rather than a better guess.

Two arrows above the pictures, and the up and down keys, step through five 16:9
sizes from 120 to 456 wide. The default is the old 176, so nothing moves for
anyone who does not touch it.

The arrows wrap rather than shrink: ask for a bigger picture with four cameras
and you get one, on two rows. Squeezing them all onto a single row would make
the arrows do nothing on exactly the rig that wants them. The height the screen
declares follows the wrap, so the record button stays on screen rather than
being pushed under the fold by pictures that grew without saying so.

The buttons disable at the ends of the range, and the keys work only while the
main screen is the thing on screen -- behind Settings or Cameras they would
resize something the user cannot see. A focused slider or text box consumes its
own arrows first, so this cannot steal them from the volume.

The chosen size is remembered across launches. A settings file written before
this existed has no such key and loads as the default rather than as zero, which
would have silently shrunk the pictures of everyone who upgraded.

Not visually verified: JUCE implements CameraDevice on macOS and Windows only,
so the tiles cannot be seen from the Linux build this was written on. The five
sizes are a considered first guess, not a measured one.

## v0.7.0 — 2026-08-31

### Changed -- the cameras are on the main screen now

The picture and the levels were on two different screens, so you could watch one
or the other and never both. That is the wrong way round for the moment that
matters: a microphone goes quiet while the shot still looks fine, and the only
place that shows is the meters you left behind to look at the camera.

Every camera switched on for the take now has a live tile on the main screen,
above the channel strips, captioned with its name -- four identical webcams
being the same problem §14.6 solves for microphones. A rig with no camera
enabled is laid out exactly as before and costs an audio-only user no space.

The Cameras door keeps doing what it did: switching cameras on and off, naming
them, choosing preview quality. That is settings. What moved is the watching.

One viewer per camera, enforced by ownership rather than hoped for: whichever
screen is visible owns the viewers, and the main screen releases its tiles
before either panel opens. Opening a camera the user already switched on does
not move the macOS privacy prompt -- the first grant is still spent behind the
camera door, with the reason on screen -- and a rig with nothing enabled opens
nothing and prompts for nothing.

Not visually verified: JUCE implements CameraDevice on macOS and Windows only,
so the tiles cannot be seen from the Linux build this was written on.

## v0.6.0 — 2026-08-31

### Fixed -- "Saved to ..." for a take that saved nothing

The saved-take card has always been able to tell that a finished take holds
nothing but WAV headers, and it warns when it does. The status line beside it
said `Saved to <folder>` regardless, because `SavedTakePanel::takeIsEmpty()` --
whose own comment reads *"The owner uses this to say so rather than calling it
saved"* -- had no callers. So the two disagreed on screen at the same moment,
and the status line is the one a user reads on their way out of the room.

The rule moves to `Core/TakeCompleteness` so both places reach one verdict
instead of keeping two copies that can drift, and the §10.6 notice now says the
files are empty when they are. §0.1 is about never silently dropping audio;
telling someone it was saved when it was not is the same failure wearing a
better word.

### Fixed -- two meter-strip setters that nothing ever called, and a screenshot tool that hid it

- **Every strip painted an empty row.** `SkullMeterComponent` reserves an 11px
  line under the bold microphone name and draws `deviceName` into it --
  and `setDeviceName()` had no callers anywhere, so that line was blank on every
  channel for the life of the app. It now carries the hardware's own product
  string, so a port the user named "Kitchen" shows `Kitchen` with `Blue Yeti`
  faint beneath it. Left deliberately empty when the two would be the same
  words, so an unnamed microphone does not print its product string twice.
- **§9.3's `prefers-reduced-motion` was never read.** The gate has always been
  there -- the clip-eye glow draws only when `reducedMotion` is false -- and
  `setReducedMotion()` had no callers, so the flag sat at its default and the
  glow drew for everyone regardless of the setting. The preference is now read
  from the OS once at startup (macOS `com.apple.universalaccess`, Windows
  `SPI_GETCLIENTAREAANIMATION`) and applied to every strip. Where no single
  setting exists to read, "no preference" is reported rather than guessed.
- **`Tools/screenshot_app.sh` silently photographed stale binaries.** The README
  builds into `build`; the script only ever looked in `build-app`, so it either
  refused to run or -- worse -- rendered whatever old artefact happened to be
  sitting in the other directory. It now looks in both, prefers the **newest**
  binary, and prints which one it is capturing and when that was built. This is
  not hypothetical: it produced a screenshot offered as evidence for a change
  the binary did not contain.

### Fixed -- a failed backup drive stopped the mirror in silence

§6.3 has two ways a mirror can stop mid-take, and only one of them was ever
reported.

- **A failed mirror write said nothing at all.** `WritePipeline` has always
  detected it, stopped mirroring, and correctly left the card write running --
  §6.3's "the mirror must never take the recording down with it". But
  `hasMirrorWriteFailed()` had no callers outside its own tests, so nothing
  ever *noticed*. Pulling the backup drive mid-take produced no message and no
  line in `session.json`, because only the low-space stop was recorded. The
  user was left with a truncated backup copy that looked exactly like a
  complete one. `MirrorState` gains `StoppedWriteFailed` so the two reasons
  stay distinguishable -- "your disk is filling up" and "your backup drive is
  gone" need different words -- and both now reach the user and the record.
- **The emergency stop sat below two mirror checks.** The §6.5 card-removal
  branch is commented "checked before anything else because the take has to
  stop now", and it wasn't: the mirror checks returned ahead of it, so a mirror
  message could delay the stop by a poll. It is now where its comment says.

### Fixed -- a mid-take unplug pointed the clock master at silence

The take's channel list and the device list are two different index spaces.
Outside a recording they agree, which is what made conflating them so easy to
miss. During a take they do not: §6.5 freezes the channel list, so an unplugged
microphone keeps its slot and writes silence, while `DeviceManager` tracks what
the OS reports right now and drops anything unplugged. Five places paired an
index from one space with a lookup in the other.

- **The clock master locked onto the unplugged channel.** `applyClockMaster()`
  found its index by counting included devices, which is the right number only
  until a microphone leaves mid-take. Recording A, B and C and unplugging B
  made the device list `A, C`; asked to lock to C, the old arithmetic returned
  index 1 -- channel B, the one writing silence. Every other microphone was
  then drift-corrected against a channel carrying no audio. Resolution now
  goes by device id against the take's frozen list, and skips any candidate
  that is dead or is not in this take at all.
- **§3.3 master failover never ran.** `selectFailoverMaster` had no production
  callers -- the fifth unwired Core API in this series. Losing the master left
  the rig on whatever channel the shifted index happened to name. The
  switchover is now performed and, as §3.3 requires, logged with its timestamp
  in `session.json`.
- **Drift was attributed to the wrong microphone.** The reporting loop walked
  the device list while reading `getChannelDriftPpm(index)` from capture, so
  after an unplug the Advanced panel showed one microphone's PPM under
  another's name, and §3.3's 100 PPM "unreliable" flag could land on a device
  keeping perfect time.
- **Names, meters and the dead-channel indicator drifted apart.** The skull
  strips take their name from the device list and their level, liveness and
  dashed outline from capture. After a mid-take unplug the name and the meter
  on a strip belonged to different people, and the shorter device count meant
  the last channel's meter was never polled at all. `getIncludedMicCount()` and
  `getMicDisplayName()` now answer in the take's space while one is running,
  and an unplugged microphone's strip keeps the name it was opened with rather
  than going blank.
- **§10.5 advice addressed the wrong person.** `SetupAdvisor` is fed one peak
  per channel but was given names from the device list, so unplugging a
  microphone renamed everyone after it in every piece of advice.

### Fixed -- every microphone looked dead, and §6.5's hot-plug row said nothing

Found by sweeping `Source/Core` for public API with no production callers,
after two bugs of exactly that shape (#34, #35).

- **Every meter read `--.-` forever.** `SkullMeterComponent::setNoSignal` had no
  callers anywhere, and the flag it sets defaults to true -- and it gates both
  the dashed skull outline *and* both numeric readouts. So every microphone was
  drawn permanently dashed with a hardcoded `--.-` and `pk --.-`, whatever it
  was actually doing. §8.1 puts the meters live from launch and §9.2 asks for
  numeric readouts in a monospace face "so digits do not jitter as values
  change"; the digits never changed at all. Now dashed means what §6.5 says it
  means -- the channel is not delivering audio, either unbound or writing
  silence because its microphone was unplugged mid-take -- and a connected
  microphone in a quiet room shows its real level, which is the difference
  between "nobody is talking" and "this is not working".
- **A microphone plugged in mid-take said nothing.** §6.5 requires one line:
  "Mic added to monitoring. It'll be recorded starting with your next take."
  `RecordingEngine` has contained that sentence, verbatim, the whole time --
  nothing had ever asked it for one. So plugging a microphone in during a
  recording was completely silent, leaving the user to assume it was being
  captured.
- **An unplug mid-take was never logged.** §6.5 says "log the dropout";
  `RecordingEngine::onMicUnplugged` existed to record exactly that and was never
  called, so a microphone could fall out of a four-hour take and leave no trace
  in `session.json`. Reconnections were equally unrecorded. Both are now logged
  against the device they happened to.

The audio itself was never at risk here: an unplugged channel has always kept
its slot and written silence (§6.5), which is the part that protects the take.
What was missing was every way the user or the record would have known.


### Fixed -- §6.5's back-pressure row was policy nothing ever called

- **The ring-buffer warnings never fired.** `CapacityMonitor::evaluateFill` has
  always held §6.5's thresholds -- warn at 50% fill, fall back to mix-only at
  90% when there is no mirror -- and has always had tests. Nothing in the app
  ever called it. Neither did anything call `noteDegradationAt`, and
  `CaptureCoordinator::getRingFillFraction` was exposed and read by nobody. So
  a drive falling behind produced no warning at all, and §0.1's "never silently
  drop" described precisely what happened.
- **Now the fill is polled during a take** and the warning is shown, ranked
  above the remaining-time warnings: a ring filling up is audio about to be
  lost now, where running low on room is audio that will stop being recorded
  later.
- **At 90% with no mirror, the stems stop and the mix keeps going.** A complete
  mix is worth more than eight stems with the same hole in them, and the ring
  drains at a fraction of the byte rate while it recovers. The mix is still
  summed from every channel while degraded -- the point is to shed write
  bandwidth, not to drop anyone out of the recording that survives.
- **It never un-degrades within a take**, for the same reason the mirror never
  restarts (§6.3): stems that resume mid-file are worse than stems that
  stopped, because the gap is invisible in the waveform.
- **§6.5: "log the exact sample position of degradation."** It goes into
  `session.json` as a dropout entry, which is what it is -- the moment the
  stems stopped receiving audio they should have had.


### Fixed -- a pulled card no longer fails silently

- **Every failed write was being thrown away.** `SessionWriter::writeInterleaved`
  returns false on an unrecoverable write and its own header says the caller
  handles that per §6.5 "target card removed". `WritePipeline` was that caller,
  and it discarded all four of those return values -- the stems, the mix, and
  both mirror copies. So pulling the card mid-take wrote into the void: no stop,
  no finalize, no alert, the elapsed time still climbing and the screen still
  claiming to be recording. This is the data-loss case in the §12 hostile-event
  matrix and it was unhandled.
- **Now the take stops, finalizes and says so.** §6.5 in full: stop immediately,
  close every open file, alert loudly, and -- when the mirror was still running
  -- state that a complete copy survives and give its path. The panel that
  appears at the end of a take carries the alert and lists what actually
  survived, pointed at the mirror rather than the folder that is no longer
  there.
- **A mirror that fails never takes the recording with it.** §6.3 makes the
  mirror a safety net, so a failed mirror write stops the mirror for the rest of
  the take, exactly as running out of internal room does, and the card write
  continues untouched. The mirror exists to turn a card failure into an
  inconvenience; it must not become one.
- **A mirror with a hole in it is never offered as a copy.** If the mirror had
  already stopped earlier in the take, the alert does not name it -- a user told
  a complete copy survives will trust it.
- **Proven against a real failing write.** The process's maximum file size is
  capped so the writer's own file grows into a hard EFBIG, which is what a
  departed card looks like from inside `write()`, with SIGXFSZ ignored so the
  failure arrives as the return value the pipeline has to notice. Paired with a
  healthy take that must keep the flag down, so the test cannot pass by always
  reporting failure.


### Fixed -- the card-speed gate now counts the video, and is answered fresh

- **A camera could push a card past what it can sustain, and nothing checked.**
  §6.4 blocks arming unless the destination sustains twice what the take needs,
  but that figure counted the audio only. Eight microphones at 24-bit/48k need
  about 4.6 MB/s and one camera in requested high-quality mode can ask for as
  much again,
  so a card could pass the gate and then fail once a camera started -- the exact
  mid-take degradation §6.4 exists to refuse in advance. The video is now part
  of the required rate, added once rather than doubled, since the x2 covers the
  stems plus the mix and there is only ever one copy of the video.
- **The verdict is no longer frozen at benchmark time.** How fast the card is
  belongs to the card; how fast it needs to be belongs to the take, and the take
  changes whenever a microphone or a camera is switched on. The 200 MB
  measurement is still cached per volume and never re-run needlessly, but the
  gate is applied against the rig as it stands at the moment someone reaches for
  record. This also closes the same staleness for microphone count, which was
  there before cameras existed: the stored pass/fail was computed with whatever
  channel count happened to be live when the benchmark last ran.
- **§10.6: the way out is named.** When the cameras are what pushed a card over,
  the message says so -- and when the card was too slow for the audio alone, it
  does not, because turning the cameras off would not save it.


### Added -- the app remembers your rig

- **Setting up happens once.** Microphone names and trims, which microphones are
  switched off, where recordings go, the backup setting, the combined-device
  name, the cameras and their names, and the answer to the "where does this go"
  card are all still there next launch. §2.4 already carried a name and a trim
  across a replug, and PortIdentityStore said in as many words that writing them
  to disk was the App layer's job -- which the App layer had never done, so
  every one of them was carried faithfully across an unplug and lost completely
  on quit.
- **Keyed by port, not by name.** Four identical microphones share a product
  string (§14.6); switching one off by name would have switched off its three
  siblings on the next launch.
- **A settings file is never worth failing to launch over.** One that is
  corrupt, truncated by a power cut, or written by a newer version loads as
  defaults, and a file from an older version keeps every key it does recognise
  rather than being dropped whole.
- **A remembered destination has to still be there.** A card unplugged since
  last time falls back to the default rather than leaving the app pointed at a
  path that no longer exists.

### Added -- §6.6 crash recovery

- **A take the app was killed in the middle of is handed back.** On launch the
  destination and the mirror are checked for sessions with no stop timestamp;
  their file headers are repaired from the audio actually on disk, and what was
  found is presented before the main screen with a button that opens the folder.
- **This collects a guarantee that was already being paid for.** SessionWriter
  has always rewritten each file's RIFF and data sizes every five seconds
  precisely so an interrupted file stays playable -- and nothing had ever gone
  looking for those files afterwards. The audio was on the card, under a header
  describing a file up to five seconds shorter than it really was, and the app
  came up as though nothing had happened.
- **Under a second is reported, not offered.** §6.6 calls such a file an
  unplayable stub. It is excluded from what is presented, named in the count so
  nobody is left wondering where a file went, and left on disk rather than
  deleted -- see *Judgment calls*.
- **A take where nothing survived is not mentioned at all**, and neither is a
  clean shutdown, which is nearly every launch.

### Changed -- cameras say what they will write

- Each camera row now shows the file it will produce (`Writes
  V01_Kitchen-Cam.mov`), updating as it is renamed. Renaming is the moment
  someone wants to see what the name does.
- The Cameras panel's way back moved to the top left, where the one in Settings
  already was. Two doors off the main screen that close in different corners is
  two things to learn instead of one.


### Added -- you are told where the files go before there are any, and shown them after

- **A card before your first take asks where it's going.** Two questions --
  what to call this recording, and where to put it -- with the exact folder
  that will be created shown underneath, updating as you type the name. It
  lists what will be in the folder, says where the backup copy goes, and
  offers a button to pick somewhere else. §6.2 calls a novice losing track of
  their recording a total product failure, and the cheapest place to prevent
  that is before there is anything to lose.
- **It is asked once.** The answer is remembered against the folder it was
  given about, so every later press of record starts immediately, exactly as
  §10.4 requires. It is asked again only when the destination moves somewhere
  the user has not agreed to, or when they tick *Ask me this before every
  recording*.
- **The files can be watched appearing.** While a take runs, the main screen
  shows that take's own folder rather than its parent, and a live count and
  total size read off the disk -- not inferred from the channel count, so what
  is on screen is what is on the card.
- **Stopping produces the files, not a sentence.** §6.2 has always asked for
  "show the location and offer to open the containing folder"; until now that
  was a status line that said "Saved to ..." for ten seconds and offered
  nothing. It is now a panel naming every file that was written with its size,
  the backup copy's location, and an **Open the folder** button. A take whose
  files came out empty says so, and says to check the mute switches -- §10.5's
  single most common failure.

### Added -- cameras

- **Any camera the OS lists can be recorded**, from a new **Cameras** door on
  the main screen: USB webcam, built-in camera, a capture card presenting an
  HDMI feed. The app does not vet where the picture comes from, the same way
  §2 does not vet a microphone's.
- **Live from the moment the panel opens.** Framing is something you fix before
  a take, and a picture you cannot see until you press record is a picture you
  aim afterwards.
- **Recording requests JUCE's high-quality mode, and nothing on screen lowers
  that request.** The operating system and driver select the actual format. The
  preview toggle decides how large the picture is *drawn*, so the live view can
  be cheap without deliberately asking for a lower recording format -- §6.6
  warns about spending CPU before it starts dropping audio, and a 4K frame
  redrawn to check someone is in shot is exactly that spend.
- **Picture and sound are separate files.** Each camera writes one file into the
  same session folder as the audio, under §6.2's naming rules, with no sound
  track of its own -- the sound is the microphone tracks beside it. `session.json`
  records the pairing and states that the video carries no audio, so an editor
  reading the folder later is told rather than having to find out.
- **The remaining-time figure counts the video.** "Room for 8h" with two cameras
  running would have been out by an order of magnitude, and §6.5's whole point
  is that a novice cannot act on a surprise part-way through a take.
- **New cameras stay closed until asked for.** The user must explicitly switch
  a camera on the first time. That choice is remembered, so an enabled camera
  reopens on later launches until the user switches it off.
- **The camera path compiles everywhere.** JUCE implements camera capture on
  macOS and Windows only, so on any other machine it would have been unverified
  by construction -- which is how CoreAudioBackend and WasapiAsioBackend came to
  be carrying five defects each. `Simulation/Camera` supplies a stand-in
  `juce_video`, and `sim_camera` compiles the controller unmodified against it,
  so a wrong signature fails the build on Linux too.

### Fixed

- **The elapsed time was set on a label with no bounds.** The main screen splits
  its status row in two while recording and leaves it single when not, but the
  flag that chose between the two layouts was never assigned, and changing state
  never re-laid the screen out -- so "Recording for 4m 12s" was written every
  tick to a label nobody could see.


### Changed — a cooler palette, and one place it lives

- **The colours are cool rather than warm.** A slate ground with cyan doing the
  acting, in place of the sepia and amber, which read as a leather-bound thing
  rather than as an instrument. The meters keep green through yellow to red:
  that is the convention every level meter uses, and the palette is not worth
  relearning a meter for.
- **A warning colour distinct from the accent.** Amber for anything that needs
  attention -- a monitor problem, the reason a record button will not go.
  Painted in the accent, a problem said "press me" in the colour of the thing
  that had just gone wrong.
- **The palette lives in one place.** The meters, the mix bar and the main
  screen each carried their own copy of the same hex values, so a recolour
  meant editing four files and finding out from a screenshot which one had
  been missed.

### Changed — the icon and the disk image follow the app

- **The app icon is recoloured**, and now carries the accent the way the meters
  do: bone skull on slate, filled from the jaw up to a level. A skull is a
  skull; a skull holding a level is a level meter, and that is what tells you
  at 32px what the app is for.
- **The disk-image window** matches, and its drag arrow is the accent rather
  than the faintest mark in the window -- it is the entire instruction that
  window exists to give.

### Fixed — the first-launch instruction, in the two places a Mac user reads it

The disk-image panel and the README's install steps both led with
*"Control-click the app, then choose Open."* That is the workaround for an
app with **no** signature. This build is ad-hoc signed, and Control-click →
Open does not clear the quarantine flag on a build with no Developer ID --
which is exactly the state someone seeing *"is damaged"* is in. So the first
thing a Mac user read was the one instruction that could not work, while the
Troubleshooting section further down said so. Both now lead with the
`xattr -dr com.apple.quarantine` command, which is what actually works.

### Changed — polish on both screens

- **Channel strips are rounded cards with air around them.** Each strip painted
  itself to its own edges, so a row of them shared hard seams and read as one
  striped slab rather than as separate channels. The skull outline is lighter,
  and the peak-hold bar is inset to the skull's width instead of running edge
  to edge, where it read as a rule drawn across the card.
- **The mix bar** puts its name on the left and its number on the right, both
  inside the track's padding. Justified into the corner, the label sat on the
  fill and became unreadable the moment the mix got loud.
- **Settings has four sections** -- where recordings go, recording format,
  microphones, monitoring and output -- each a heading over a hairline. It was
  one flat list of fifteen rows. Labels and values are two tones rather than
  one, and the local-backup toggle moves up beside the other storage choices.
- **The reason a record button is disabled only takes a row when there is one.**
  Reserved unconditionally it left a hole under the button whenever recording
  was possible, which is almost always.

## v0.5.0 — 2026-08-29

### Changed — one visual language, and controls where they are looked for

- **The app has a look of its own.** Every control was previously a JUCE
  default in JUCE blue. A single `AppLookAndFeel` now carries the palette and
  draws the buttons, sliders, scrollbars and tick boxes: flat, rounded, hairline
  outlines on a dark ground. The record button is the one saturated element on
  the screen and turns red while recording, so the state is never carried by
  colour alone.
- **The window opens at the height the screen needs.** It opened at 480 pixels
  while the main screen needed 528, which put the monitor volume, the mute
  button and the only way into Settings below the fold behind a scrollbar. Both
  screens still scroll on a display too short for them.
- **"Advanced" is now "Settings".** It is the one door out of the main screen
  and it holds ordinary choices — which microphones to record, where the files
  go. "Advanced" read as a warning to stay out of it.
- **Where recordings go is the first thing in Settings**, directly under Done.
  Picking an SD card before a take is what most people open the panel to do,
  and it sat at the bottom, below four read-only format rows, the microphone
  list and the drift report.
- **The monitor slider says what it is.** Its value box read a bare `70`; it
  now reads `Monitor 70`, naming the control in the space it already had.
- `Tools/screenshot_app.sh` renders the app headless and captures it, so a
  layout change can be looked at rather than reasoned about. `MMA_CLICK=x,y`
  clicks before capturing, which is how the Settings screen gets captured.

### Added — from the first working session

- **Choose which microphones to record.** Settings now lists every microphone
  the OS reports, with a checkbox each. Previously everything enumerated was
  recorded, which on a Mac meant the machine's own microphone was always a
  track. A deselected mic also gives its slot back, so turning off one you are
  not using makes room for a ninth you are.
- **The clock master is explained**, in the terms that matter to someone
  recording: each USB mic runs on its own crystal, no two tick at quite the same
  rate, one is the reference and the rest are nudged to match it, and that is
  what keeps a long take aligned.
- **Channel-strip meters.** The meters were short wide boxes stretched to fill
  the window, so two mics rendered as two enormous panels and eight as slivers.
  They are now fixed-width vertical strips laid side by side like a mixing desk,
  with the level, peak and name stacked tight beneath each meter.

### Fixed — four bugs from the first session with real microphones

The first time this app met physical hardware it produced four faults. None had
been caught by any test or by the CoreAudio simulation, because all of them
depend on how a real OS enumerates real devices.

- **Each microphone appeared several times** — one Yeti was listed five times.
  The device-change handler called `addDevice` for every enumerated device on
  every notification, and `addDevice` appended with no duplicate check. macOS
  fires that listener several times while a USB microphone initialises, so each
  firing re-added everything already present. Enumeration is now reconciled:
  new devices added, departed ones dropped, and devices that never left keep
  their enumeration order, drift history and inclusion untouched.
- **The clock master was always the computer.** Before any drift measurement
  exists, master selection fell back to enumeration order — and CoreAudio
  enumerates the machine's built-in microphone first, so it won on every Mac
  however many USB mics were attached. §3.1 now prefers a device the user
  plugged in, falling back to the built-in only when it is the only one.
- **Microphones were not recognised promptly.** The same defect as the first:
  the notification did arrive, and each arrival grew the list rather than
  reflecting it.
- **Settings had no way back.** Opening the Advanced panel hides the main
  screen, and the button that opens it lives on that screen, so nothing on
  display could return. The panel now has a Done button, laid out first so a
  long device list cannot push it out of view.

Deduplication keys on device identity, never on name: two Yetis report the same
product name and must remain two devices.

## v0.4.0 — 2026-08-28

### Fixed — five defects on the audio thread

None were caught by the previous 246-test suite. Each fix ships with a
regression test confirmed to fail against the old code.

- **Underrun counts were inflated roughly 30x.** When a device's ring buffer ran
  dry mid-block, `DeviceInputStream::pull` left the resampler's inner loop but
  let the outer loop continue, re-entering the failure path once per remaining
  sample. A single starved 64-sample block was reported as ~2,000 lost samples.
  §0.1 makes any non-zero underrun the one failure the user is shown, so this
  was a false alarm about the app's central promise.
- **The monitor's runaway cut fired on unrelated bursts.** `kLimiterReleaseSeconds`
  was declared and never used, so an engagement took as long to unwind as it
  took to build and carried its credit into the next one. Two 300 ms bursts
  10 ms apart — neither close to the 500 ms threshold — combined past it and cut
  the monitor mid-take.
- **Use-after-free between the audio callback and `stopRecording`.** The callback
  read a `std::unique_ptr` while another thread moved and then destroyed it. The
  audio thread now reads an atomic pointer published with release, and teardown
  waits for in-flight callbacks.
- **Three `std::pow` calls per sample in the callback**, from operands that never
  change. All are now computed once.
- **The ring buffer copied one sample at a time** — 384,000 atomic release-stores
  a second at 8 microphones. Both read and write now move data in two `memcpy`
  chunks.

### Added — installable rather than merely built

- **macOS `.dmg`**, ad-hoc signed and drag-to-install. The first attempt shipped a
  bundle Gatekeeper refused as *"is damaged"*: `install(DIRECTORY)` and `cp -R`
  both discard the ad-hoc signature Apple Silicon requires. Packaging now
  re-signs after the install, stages with `ditto`, and verifies the signature
  inside the mounted image.
- **An application icon**, generated by `Tools/make_icon.py` from the app's own
  §9.2 palette. CI fails the build if `Icon.icns` is absent from the bundle.
- **A laid-out disk-image window** — the app, an arrow, and the Applications
  folder, with the first-launch step written on the background art.

## v0.3.0 and earlier

`v0.1.0`, `v0.2.0`, `v0.2.1` and `v0.3.0` were cut before this changelog
existed. See the [Releases page](../../releases) for what each contained.

## Known limitations

- Untagged macOS rehearsal artifacts use an ad-hoc signature and may require the
  quarantine-removal step in the README. Tagged public releases now fail closed
  unless the app and disk image are Developer ID signed, notarized and stapled;
  the required owner credentials are still an open v1.12.0 release gate.
- The §12 hardware validation matrix is still outstanding. v0.5.0 fixes the
  first four faults a real session found, but the matrix itself has not been
  worked through.
