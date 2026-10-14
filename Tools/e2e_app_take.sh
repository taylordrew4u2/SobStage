#!/usr/bin/env bash
# The end-to-end gate: the REAL app, headless, records a real take through the
# real ALSA backend against the virtual microphones from setup_alsa_fixture.sh,
# and the files it leaves are checked by verify_take.py.
#
#   Tools/e2e_app_take.sh [seconds-to-record] [extra verify_take.py args]
#
# Needs Xvfb, xdotool, ImageMagick. Every step waits for evidence of the state
# it expects (window up, take folder created, stop timestamp written) rather
# than a fixed sleep, so a slow start-up cannot turn a stop click into a start.
# Exit status is verify_take.py's.
set -euo pipefail

SECONDS_TO_RECORD="${1:-15}"
DISPLAY_NUM="${MMA_DISPLAY:-:99}"
APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
echo "App: $APP (built $(date -r "$APP" '+%Y-%m-%d %H:%M:%S'))"

bash Tools/setup_alsa_fixture.sh >/dev/null
RECORDINGS="$HOME/RECORDINGS"
mkdir -p "$RECORDINGS"
newest_recording() {
  find "$RECORDINGS" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; 2>/dev/null \
    | LC_ALL=C sort | tail -1
}
BEFORE=$(newest_recording)

cleanup() { kill "${APP_PID:-0}" 2>/dev/null || true; sleep 1; pkill Xvfb 2>/dev/null || true; }
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
# The fixture's `file` microphones sit on the clockless `null` slave: left
# alone they deliver as fast as the file reads, flood the ring, and each stem
# becomes a patchwork of whatever fitted -- which says nothing about the app
# and lets the tone vote land anywhere from 40% to 100% depending on how fast
# the runner happens to be. Tools/alsa_readi_shim.cpp paces each microphone to
# its own clock instead, the way hardware does. A caller that brings its own
# shim (e2e_realtime_mics.sh) keeps it and its clocks.
if [ -z "${MMA_APP_LD_PRELOAD:-}" ]; then
  for CANDIDATE in build/libalsa_readi_shim.so build-app/libalsa_readi_shim.so; do
    [ -e "$CANDIDATE" ] || continue
    if [ -z "${MMA_APP_LD_PRELOAD:-}" ] || [ "$CANDIDATE" -nt "$MMA_APP_LD_PRELOAD" ]; then
      MMA_APP_LD_PRELOAD="$PWD/$CANDIDATE"
    fi
  done
  if [ -n "${MMA_APP_LD_PRELOAD:-}" ]; then
    export MMA_SIM_REALTIME="${MMA_SIM_REALTIME:-1}"
    export MMA_SIM_PPM="${MMA_SIM_PPM:-mma_mic1=150,mma_mic2=-150,mma_out=0}"
    echo "Microphone clocks: $MMA_SIM_PPM"
  fi
fi
# MMA_APP_LD_PRELOAD reaches the app and nothing else, so a shim meant for it
# (the real-time microphone clocks, for one) is not loaded into Xvfb, xdotool
# and every other helper this script runs.
env ${MMA_APP_LD_PRELOAD:+LD_PRELOAD="$MMA_APP_LD_PRELOAD"} \
  DISPLAY="$DISPLAY_NUM" MMA_SKIP_SETUP_GUIDE=1 nohup "./$APP" >/tmp/mma-e2e-app.log 2>&1 &
APP_PID=$!

# 1. The window is up.
for _ in {1..60}; do
  if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then break; fi
  sleep 1
done
DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null || { echo "FAIL: window never appeared"; exit 1; }
# Devices enumerate and streams open after the window shows. A gate that wants
# the app to have settled its buffer ladder first (§5.4 steps on loss events
# inside a 30-second window) asks for longer.
sleep "${MMA_SETTLE_SECONDS:-4}"

# Clicks are given relative to the app window's top-left corner, because the
# window manager-less Xvfb root places the window wherever it likes: hard-coded
# screen coordinates missed the record button entirely on a taller screen and
# the gate then reported "no take folder" for a perfectly healthy app.
click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p')
  y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $(( x + $1 )) $(( y + $2 )) click 1
}

# A launch after an interrupted take shows the Recovered card first, and a
# card swallows every click behind it by design. Its Done button sits at
# (417,312) relative to the window on this display; with no card up the click lands on an inert
# label. Either way the main screen is reachable afterwards.
# The card's height depends on how many takes it lists, so its Done button
# does not sit at one fixed offset. Each candidate that misses lands on an
# inert label, so trying all of them is safe and one of them lands.
for DONE_Y in 263 293 312 332; do click 417 "$DONE_Y"; done
sleep 1

# 2. Record. The button is at (973,178) relative to the window. A take is proven started by its folder appearing.
TAKE=""
for attempt in 1 2 3; do
  click 973 178
  # The first record press on a machine that has never recorded raises the
  # "Where does this recording go?" card instead of starting, and that card
  # swallows every click behind it -- so without this the gate fails on any
  # fresh profile. Its Start recording button sits at (733,540) relative to the window; with no card up the point is below the window and the click
  # lands on the root window, which does nothing.
  sleep 1
  click 733 540
  for _ in {1..10}; do
    sleep 1
    CUR=$(newest_recording)
    if [ -n "$CUR" ] && [ "$CUR" != "$BEFORE" ]; then TAKE="$CUR"; break; fi
  done
  [ -n "$TAKE" ] && break
  echo "record press $attempt did not start a take; trying again"
done
[ -n "$TAKE" ] || {
  # What the screen looked like when it would not start, so the next person
  # does not have to relaunch the app by hand to find out.
  DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-no-start.png 2>/dev/null || true
  echo "FAIL: no take folder appeared in $RECORDINGS (screen: /tmp/mma-e2e-no-start.png)"
  exit 1
}
echo "take started: $TAKE"

sleep "$SECONDS_TO_RECORD"
DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-recording.png

# 3. Stop. A mid-take card, if one is up, swallows clicks by design: Escape is
#    its "keep recording", and the Keep button sits where the card puts it.
DISPLAY="$DISPLAY_NUM" xdotool key Escape
sleep 1
click 973 178
STOPPED=""
for _ in {1..20}; do
  sleep 1
  if python3 -c "import json,sys; sys.exit(0 if json.load(open('$RECORDINGS/$TAKE/session.json')).get('stopTimestamp') else 1)" 2>/dev/null; then
    STOPPED=yes; break
  fi
done
DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-saved.png
[ -n "$STOPPED" ] || echo "WARN: no stop timestamp after 20 s (the verifier will fail on it)"

cleanup
trap - EXIT

# Two seconds of slack for start-up and finalisation.
python3 Tools/verify_take.py "$RECORDINGS/$TAKE" --seconds "$((SECONDS_TO_RECORD - 3))" \
  --tone mma_mic1=440 --tone mma_mic2=1000 --silent-ok mma_out "${@:2}"
