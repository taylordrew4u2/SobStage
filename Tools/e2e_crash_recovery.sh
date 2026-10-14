#!/usr/bin/env bash
# What the next launch does after a crash, a force-quit or a power cut.
#
#   Tools/e2e_crash_recovery.sh
#
# Runs the REAL app, headless, in a throwaway HOME (its own settings folder,
# recordings and ~/.asoundrc), on its own Xvfb display, and checks:
#
#   1. Hidden temporary files a crash strands mid-replace of settings.json and
#      of a show template are removed at launch -- and the stranded show is not
#      offered as a show.
#   2. A save location that is NOT mounted at launch (macOS checks a card for
#      minutes after an unclean shutdown before it mounts) is checked for an
#      interrupted take once it appears: the take's WAV header is repaired, the
#      take's own stranded temp file is removed, and it is not yet marked as
#      dealt with (that is the card's Done).
#   3. An interrupted take with nothing playable in it (crash within a second
#      of Record) is marked as dealt with by the scan, so it is not announced
#      again at every launch.
#   4. SIGKILL mid-take on a real recording: the next launch repairs every
#      stem's header from the bytes on disk. A microphone dies part way
#      through first (Tools/alsa_readi_shim.cpp), and the take's session.json
#      -- the record the kill leaves behind -- has to say so: it used to be
#      written at Record and at Stop only, so it never did.
#   5. A locked card (a read-only mount): its copy of an interrupted take
#      cannot be repaired, while the local backup copy can. The backup copy is
#      the one offered, and the log says why. Needs mount (root); skipped
#      otherwise.
#
# Needs Xvfb, xdotool. MMA_DISPLAY picks the display (default :163); only the
# Xvfb this script started is killed. A build with -DMMA_ALLOW_TEST_INPUTS=ON
# supplies libalsa_readi_shim.so for the microphone in 4; without it that
# part is skipped.
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:163}"
APP="${MMA_APP:-}"
[ -z "$APP" ] && for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
echo "App: $APP"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/mma-crash-recovery.XXXXXX")"
export HOME="$WORK/home"
unset XDG_CONFIG_HOME XDG_DATA_HOME
mkdir -p "$HOME"
bash Tools/setup_alsa_fixture.sh "$WORK/fixture" >/dev/null

SUPPORT="$HOME/.config/SobStage"
CARD="$HOME/LATECARD"
DEST="$CARD/RECORDINGS"
LOG="$WORK/app.log"
FAILURES=0
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "ok:   $*"; }

XVFB_PID=""
APP_PID=""
LOCKED_MOUNT=""
cleanup() {
  [ -n "$APP_PID" ] && kill -9 "$APP_PID" 2>/dev/null || true
  [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
  [ -n "$LOCKED_MOUNT" ] && umount "$LOCKED_MOUNT" 2>/dev/null || true
}
trap cleanup EXIT

# Extra environment for the next launch only (a shim for scenario 4).
APP_ENV=()

Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 2

# The single-instance lock is machine-wide: another gate's app holding it makes
# this launch hand over and exit at once. Wait for it rather than fail.
launch() {
  for attempt in $(seq 1 30); do
    env DISPLAY="$DISPLAY_NUM" MMA_SKIP_SETUP_GUIDE=1 ${APP_ENV[@]+"${APP_ENV[@]}"} nohup "$APP" >>"$LOG" 2>&1 &
    APP_PID=$!
    sleep 3
    if kill -0 "$APP_PID" 2>/dev/null; then
      for _ in {1..60}; do
        DISPLAY="$DISPLAY_NUM" xdotool search --pid "$APP_PID" --name SobStage >/dev/null 2>&1 && return 0
        kill -0 "$APP_PID" 2>/dev/null || break
        sleep 1
      done
    fi
    kill -9 "$APP_PID" 2>/dev/null || true
    echo "launch $attempt: another SobStage holds the lock (or the window never came); retrying"
    sleep 10
  done
  echo "FAIL: the app never came up"; exit 1
}

crash() { kill -9 "$APP_PID" 2>/dev/null || true; wait "$APP_PID" 2>/dev/null || true; APP_PID=""; }

click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --pid "$APP_PID" --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p'); y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $(( x + $1 )) $(( y + $2 )) click 1
}

# Presses the recovered card's Done until the take it names ($1, a
# session.json) is marked dealt with. The card appears only once both scans
# have finished, which can be after any fixed wait, and it does not always come
# up with the keyboard focus, so Escape alone was a coin toss. The card is
# centred in the window, which grows when the main screen has more to say, and
# Done sits 91px below the middle for a card of one take, whichever line count
# its detail wraps to.
dismiss_recovered_card() {
  local win h
  for _ in {1..15}; do
    win=$(DISPLAY="$DISPLAY_NUM" xdotool search --pid "$APP_PID" --name SobStage | head -1)
    h=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win" | sed -n 's/^HEIGHT=//p')
    click 417 $(( ${h:-420} / 2 + 91 ))
    sleep 1
    [ -n "$(stop_stamp "$1")" ] && return 0
  done
  return 1
}

# A mono 24-bit 48 kHz WAV of `seconds`, its RIFF and data sizes left at the
# placeholder zeros a crash in the first five seconds leaves.
make_stale_wav() {
  python3 - "$1" "$2" <<'PY'
import struct, sys
path, seconds = sys.argv[1], float(sys.argv[2])
frames = int(48000 * seconds)
audio = b"".join(struct.pack("<i", int(0.25 * 8388607 * (1 if (i // 50) % 2 else -1)))[:3] for i in range(frames))
with open(path, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 0) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 48000 * 3, 3, 24))
    f.write(b"data" + struct.pack("<I", 0))
    f.write(audio)
PY
}

# 0 when the header's data size describes every whole frame on disk.
header_matches_file() {
  python3 - "$1" <<'PY'
import os, struct, sys
path = sys.argv[1]
data = open(path, "rb").read()
at = data.find(b"data", 12)
declared = struct.unpack("<I", data[at + 4:at + 8])[0]
fmt = data.find(b"fmt ", 12)
channels, = struct.unpack("<H", data[fmt + 10:fmt + 12])
bits, = struct.unpack("<H", data[fmt + 22:fmt + 24])
align = channels * bits // 8
actual = (len(data) - (at + 8)) // align * align
riff, = struct.unpack("<I", data[4:8])
sys.exit(0 if declared == actual and declared > 0 and riff >= declared else 1)
PY
}

stop_stamp() {
  python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('stopTimestamp',''))" "$1"
}

# 0 when session.json records `device` dropping out part way through the take
# (not at its start, which is the "could not be opened" entry).
records_mid_take_dropout() {
  python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    meta = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(1)
hits = [d for d in meta.get("dropouts") or []
        if sys.argv[2] in (d.get("deviceUsbId") or "") and float(d.get("timestampSeconds") or 0) > 0]
sys.exit(0 if hits else 1)
PY
}

dropouts_of() {
  python3 -c "import json,sys; print(json.dumps(json.load(open(sys.argv[1])).get('dropouts'), indent=1))" "$1" 2>&1 || true
}

# ---- setup: last launch crashed in every way at once -----------------------
mkdir -p "$SUPPORT/Templates"
cat > "$SUPPORT/settings.json" <<JSON
{"destinationFolder":"$DEST","confirmedSaveLocation":"$DEST","setupGuideDone":true,"mirrorEnabled":false,"checkForUpdates":false}
JSON
cp "$SUPPORT/settings.json" "$SUPPORT/.settings_temp1a2b3c4d.json"
echo '{"name":"Friday gig","version":1}' > "$SUPPORT/Templates/.Friday gig_temp0c1d.json"
printf 'cam-1\n' > "$SUPPORT/.camera-starting_temp77.txt"

# The card is not mounted yet: a FILE where its folder should be, so nothing
# (not even the drive check) can create the destination.
echo "not mounted" > "$CARD"

launch
echo "launched (pid $APP_PID); card not mounted"

# ---- 1. stranded temporaries -------------------------------------------------
sleep 2
if [ -e "$SUPPORT/.settings_temp1a2b3c4d.json" ]; then fail "stranded .settings_temp file was left in place"
else pass "stranded .settings_temp file removed at launch"; fi
if [ -e "$SUPPORT/Templates/.Friday gig_temp0c1d.json" ]; then fail "stranded show temp file was left in place"
else pass "stranded show temp file removed at launch"; fi
if [ -e "$SUPPORT/.camera-starting_temp77.txt" ]; then fail "stranded camera guard temp file was left in place"
else pass "stranded camera guard temp file removed at launch"; fi

# ---- 2/3. the card mounts late ---------------------------------------------
# Built beside the mount point and moved into place in one rename, as a card
# appears when it mounts: everything on it at once. Made in place, the app
# could see the save location appear -- and check it -- before the last of
# its files had been written.
rm -f "$CARD"
FINAL_DEST="$DEST"
DEST="$HOME/.latecard-staging/RECORDINGS"
TAKE="$DEST/2026-10-07_2100_Interrupted"
STUB="$DEST/2026-10-07_2000_Stub"
mkdir -p "$TAKE" "$STUB"
echo '{"startTimestamp":"2026-10-07T21:00:00.000Z","sampleRate":48000,"bitDepth":24}' > "$TAKE/session.json"
echo '{"startTimestamp":"2026-10-07T20:00:00.000Z","sampleRate":48000,"bitDepth":24}' > "$STUB/session.json"
make_stale_wav "$TAKE/MIX.wav" 3
make_stale_wav "$TAKE/01_Alice.wav" 3
echo '{"half":' > "$TAKE/.session_temp9e8d.json"
make_stale_wav "$STUB/MIX.wav" 0.2
# A finished take whose combined video and podcast copy were cut off by the
# crash: their working files, beside a finished combined video that must stay.
DONE="$DEST/2026-10-07_1900_Finished"
mkdir -p "$DONE"
echo '{"startTimestamp":"2026-10-07T19:00:00.000Z","stopTimestamp":"2026-10-07T19:10:00.000Z"}' > "$DONE/session.json"
make_stale_wav "$DONE/MIX.wav" 2
echo partial > "$DONE/.V01_Cam_with-sound.mov"
echo partial > "$DONE/MIX - Apple Podcasts.wav.part"
echo finished > "$DONE/V02_Desk_with-sound.mov"
touch -d '2026-10-07 20:00' "$STUB"
mv "$HOME/.latecard-staging" "$CARD"
DEST="$FINAL_DEST"
TAKE="$DEST/2026-10-07_2100_Interrupted"
STUB="$DEST/2026-10-07_2000_Stub"
DONE="$DEST/2026-10-07_1900_Finished"
echo "card mounted with an interrupted take on it"

# The drive check retries a location it could not write every 10 s while idle,
# then writes 200 MB; the recovery scan follows it.
REPAIRED=""
for _ in {1..90}; do
  if header_matches_file "$TAKE/MIX.wav" && header_matches_file "$TAKE/01_Alice.wav"; then REPAIRED=yes; break; fi
  sleep 1
done
if [ -n "$REPAIRED" ]; then pass "interrupted take on the late card was repaired"
else fail "interrupted take on the late card was never repaired (headers still stale)"; fi

if [ -e "$TAKE/.session_temp9e8d.json" ]; then fail "the take's stranded .session_temp file was left in place"
else pass "the take's stranded .session_temp file removed"; fi

if [ -n "$(stop_stamp "$TAKE/session.json")" ]; then fail "the interrupted take was marked dealt with before the user saw it"
else pass "the interrupted take waits for the card's Done"; fi

if [ -e "$DONE/.V01_Cam_with-sound.mov" ] || [ -e "$DONE/MIX - Apple Podcasts.wav.part" ]; then
  fail "an unfinished combined video / podcast copy was left in a take folder"
else pass "unfinished combined video and podcast copy removed"; fi
if [ -e "$DONE/V02_Desk_with-sound.mov" ] && [ -e "$DONE/MIX.wav" ]; then pass "finished files beside them untouched"
else fail "a finished file was removed"; fi

if [ -n "$(stop_stamp "$STUB/session.json")" ]; then pass "the nothing-playable take was marked dealt with"
else fail "the nothing-playable take was left to be announced again next launch"; fi

DISPLAY="$DISPLAY_NUM" import -window root "$WORK/late-card.png" 2>/dev/null || true
crash

# ---- 4. SIGKILL mid-take -------------------------------------------------------
# A healthy card from the start, and the recovered card from above dismissed by
# its Done once it is up. mma_mic2 is made to die mid-take on cue.
SHIM=""
for CANDIDATE in "$(dirname "$APP")/../../libalsa_readi_shim.so" build/libalsa_readi_shim.so build-app/libalsa_readi_shim.so; do
  [ -e "$CANDIDATE" ] || continue
  if [ -z "$SHIM" ] || [ "$CANDIDATE" -nt "$SHIM" ]; then SHIM="$CANDIDATE"; fi
done
UNPLUG="$WORK/unplug-mma_mic2"
if [ -n "$SHIM" ]; then
  SHIM="$(cd "$(dirname "$SHIM")" && pwd)/$(basename "$SHIM")"
  APP_ENV=(LD_PRELOAD="$SHIM" MMA_SHIM_MODE=dead MMA_SHIM_DEVICE=mma_mic2 MMA_SHIM_FAIL_WHEN_FILE="$UNPLUG")
  echo "shim: $SHIM"
fi
launch
APP_ENV=()
if dismiss_recovered_card "$TAKE/session.json"; then pass "Done on the card marked the recovered take dealt with"
else fail "Done on the card did not mark the recovered take"; fi

newest() {
  find "$DEST" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null \
    | { grep -v -e '_Interrupted$' -e '_Stub$' -e '_Finished$' || true; } | LC_ALL=C sort | tail -1
}

LIVE=""
for attempt in 1 2 3; do
  click 973 178; sleep 1; click 733 540
  for _ in {1..10}; do
    sleep 1
    LIVE=$(newest)
    [ -n "$LIVE" ] && break
  done
  [ -n "$LIVE" ] && break
done
if [ -z "$LIVE" ]; then
  DISPLAY="$DISPLAY_NUM" import -window root "$WORK/no-start.png" 2>/dev/null || true
  fail "no take started for the SIGKILL check (screen: $WORK/no-start.png)"
else
  if [ -n "$SHIM" ]; then
    sleep 3
    touch "$UNPLUG"
    # Said in the record within moments of happening -- not at Stop, which a
    # crash never reaches, and not at the next thirty-second refresh either.
    SAID=""
    for _ in {1..20}; do
      if records_mid_take_dropout "$DEST/$LIVE/session.json" mma_mic2; then SAID=yes; break; fi
      sleep 1
    done
    if [ -n "$SAID" ]; then pass "the microphone that died mid-take is in session.json while the take runs"
    else fail "session.json said nothing about the microphone that died mid-take:"; dropouts_of "$DEST/$LIVE/session.json"; fi
  else
    echo "skip: no libalsa_readi_shim.so, so no microphone dies mid-take"
  fi

  # Seven seconds: past one five-second header rewrite, not at the next.
  sleep 7
  crash
  echo "killed mid-take: $LIVE"

  if [ -n "$SHIM" ]; then
    if records_mid_take_dropout "$DEST/$LIVE/session.json" mma_mic2 \
       && [ -z "$(stop_stamp "$DEST/$LIVE/session.json")" ]; then
      pass "the killed take's session.json still records the mid-take dropout"
    else
      fail "the killed take's session.json lost the mid-take dropout:"; dropouts_of "$DEST/$LIVE/session.json"
    fi
  fi
  rm -f "$UNPLUG"
  STALE=0
  for w in "$DEST/$LIVE"/*.wav; do header_matches_file "$w" || STALE=$((STALE + 1)); done
  echo "stems with a stale header after the kill: $STALE"

  launch
  REPAIRED=""
  for _ in {1..30}; do
    ALL=yes
    for w in "$DEST/$LIVE"/*.wav; do header_matches_file "$w" || ALL=""; done
    [ -n "$ALL" ] && { REPAIRED=yes; break; }
    sleep 1
  done
  if [ -n "$REPAIRED" ]; then pass "every stem of the killed take was repaired on relaunch"
  else fail "the killed take's stems were not all repaired on relaunch"; fi
  if [ -z "$(stop_stamp "$DEST/$LIVE/session.json")" ]; then pass "the killed take is offered, not silently marked"
  else fail "the killed take was marked dealt with before anyone saw it"; fi
  DISPLAY="$DISPLAY_NUM" import -window root "$WORK/after-kill.png" 2>/dev/null || true
  crash
fi

# ---- 5. a locked card: the backup copy is offered ------------------------------
# A read-only mount is a locked SD card: nothing on it can be repaired, not even
# opened for the repair. The same take's backup copy -- in a folder that took a
# "_2", tied to the card copy only by session.json's mirrorPath -- repairs.
LOCKED="$HOME/LOCKEDCARD"
mkdir -p "$LOCKED"
if mount -t tmpfs -o size=16m tmpfs "$LOCKED" 2>/dev/null; then
  LOCKED_MOUNT="$LOCKED"
  LDEST="$LOCKED/RECORDINGS"
  LTAKE="$LDEST/2026-10-08_2200_Locked"
  BACKUP="$HOME/RECORDINGS-MIRROR/2026-10-08_2200_Locked_2"
  mkdir -p "$LTAKE" "$BACKUP"
  for F in "$LTAKE" "$BACKUP"; do
    echo "{\"startTimestamp\":\"2026-10-08T22:00:00.000Z\",\"sampleRate\":48000,\"bitDepth\":24,\"mirrorEnabled\":true,\"mirrorActive\":true,\"mirrorPath\":\"$BACKUP\"}" > "$F/session.json"
    make_stale_wav "$F/MIX.wav" 3
    make_stale_wav "$F/01_Alice.wav" 3
  done
  echo "movie" > "$LTAKE/V01_Cam.mov"
  mount -o remount,ro "$LOCKED"

  cat > "$SUPPORT/settings.json" <<JSON
{"destinationFolder":"$LDEST","confirmedSaveLocation":"$LDEST","setupGuideDone":true,"mirrorEnabled":true,"checkForUpdates":false}
JSON
  APP_LOG="$SUPPORT/log.txt"
  launch

  REPAIRED=""
  for _ in {1..60}; do
    if header_matches_file "$BACKUP/MIX.wav" && header_matches_file "$BACKUP/01_Alice.wav"; then REPAIRED=yes; break; fi
    sleep 1
  done
  if [ -n "$REPAIRED" ]; then pass "the backup copy of the locked card's take was repaired"
  else fail "the backup copy of the locked card's take was never repaired"; fi
  if header_matches_file "$LTAKE/MIX.wav"; then fail "the locked card's copy was somehow repaired (is the mount writable?)"
  else pass "the locked card's copy could not be repaired"; fi

  OFFERED=""
  for _ in {1..20}; do
    grep -q "the card's copy couldn't be repaired, so the local backup copy is the one offered" "$APP_LOG" 2>/dev/null \
      && { OFFERED=yes; break; }
    sleep 1
  done
  if [ -n "$OFFERED" ] && grep "the local backup copy is the one offered" "$APP_LOG" | grep -qF "$BACKUP"; then
    pass "the backup copy is offered in the card copy's place, and the log says why and where"
  else
    fail "the log never said the backup copy was offered for the locked card's take"
    grep "Interrupted take" "$APP_LOG" 2>/dev/null | tail -8 | sed 's/^/        /'
  fi
  DISPLAY="$DISPLAY_NUM" import -window root "$WORK/locked-card.png" 2>/dev/null || true

  # Done: the backup is marked dealt with; the locked card cannot be.
  if dismiss_recovered_card "$BACKUP/session.json"; then pass "Done marked the backup copy dealt with"
  else fail "Done did not mark the backup copy"; fi
  crash
  umount "$LOCKED" 2>/dev/null || true
  LOCKED_MOUNT=""
else
  echo "skip: can't mount a read-only card here (needs root), so the locked-card check did not run"
fi

echo "screens and log: $WORK"
if [ "$FAILURES" -gt 0 ]; then echo "$FAILURES check(s) FAILED"; exit 1; fi
echo "all crash-recovery checks passed"
rm -rf "$WORK/home/RECORDINGS-MIRROR" 2>/dev/null || true
