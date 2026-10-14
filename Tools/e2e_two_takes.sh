#!/usr/bin/env bash
# A gig is not one take. Every other gate here starts the app, records once and
# exits, so nothing exercised the second take -- and the second take is where
# this codebase has historically gone wrong: CaptureCoordinator carries comments
# about take three's record carrying take one's losses, and about overrunSamples
# being "the one counter here that was not" reset with its siblings.
#
# Two takes in ONE app session, and then:
#
#   * both takes must be complete and correct on their own
#   * they must be separate folders, not one overwritten by the other
#   * take two's reported loss must be TAKE TWO'S, not both takes summed
#
# The third is the interesting one. The counters are per-take by design, so a
# regression that stops resetting them makes take two's figure roughly the sum
# of both -- about double. The bound below is deliberately loose (1.6x against
# an expected ~1.0x) so ordinary run-to-run variation cannot trip it while a
# doubling cannot hide.
#
#   Tools/e2e_two_takes.sh [seconds-per-take]
set -euo pipefail

SECONDS_PER_TAKE="${1:-6}"
DISPLAY_NUM="${MMA_DISPLAY:-:99}"

APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
echo "App: $APP"

bash Tools/setup_alsa_fixture.sh >/dev/null
RECORDINGS="$HOME/RECORDINGS"
mkdir -p "$RECORDINGS"
newest() { find "$RECORDINGS" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; 2>/dev/null | LC_ALL=C sort | tail -1; }

cleanup() { kill "${APP_PID:-0}" 2>/dev/null || true; sleep 1; pkill Xvfb 2>/dev/null || true; }
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
# The fixture's `file` microphones are clockless: unpaced they flood the ring
# and each stem is a patchwork of whatever fitted, so the tone check below
# measured the runner's speed, not the app. The shim gives each microphone its
# own clock (see e2e_app_take.sh). mma_out runs twice as fast as it should on
# purpose: its overflow is what the per-take reset check at the end measures.
SHIM=""
for CANDIDATE in build/libalsa_readi_shim.so build-app/libalsa_readi_shim.so; do
  [ -e "$CANDIDATE" ] || continue
  if [ -z "$SHIM" ] || [ "$CANDIDATE" -nt "$SHIM" ]; then SHIM="$CANDIDATE"; fi
done
test -n "$SHIM" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first (no libalsa_readi_shim.so)"; exit 1; }
env MMA_SIM_REALTIME=1 MMA_SIM_PPM="${MMA_SIM_PPM:-mma_mic1=150,mma_mic2=-150,mma_out=1000000}" \
  LD_PRELOAD="$PWD/$SHIM" \
  DISPLAY="$DISPLAY_NUM" MMA_SKIP_SETUP_GUIDE=1 nohup "./$APP" >/tmp/mma-e2e-two-takes.log 2>&1 &
APP_PID=$!

for _ in {1..60}; do
  DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1 && break
  sleep 1
done
DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null || { echo "FAIL: window never appeared"; exit 1; }
sleep 4

click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p'); y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $((x + $1)) $((y + $2)) click 1
}

# The Recovered card, if a previous run left one. Same candidates the main gate
# uses; each miss lands on an inert label.
for DONE_Y in 263 293 312 332; do click 417 "$DONE_Y"; done
sleep 1

TAKES=()
for n in 1 2; do
  BEFORE=$(newest)
  TAKE=""
  for attempt in 1 2 3; do
    click 973 178
    sleep 1
    click 733 540   # "Where does this recording go?" on a fresh profile
    for _ in {1..10}; do
      sleep 1
      CUR=$(newest)
      if [ -n "$CUR" ] && [ "$CUR" != "$BEFORE" ]; then TAKE="$CUR"; break; fi
    done
    [ -n "$TAKE" ] && break
    echo "take $n: record press $attempt did not start it; trying again"
  done
  if [ -z "$TAKE" ]; then
    DISPLAY="$DISPLAY_NUM" import -window root "/tmp/mma-e2e-two-takes-$n.png" 2>/dev/null || true
    echo "FAIL: take $n never started (screen: /tmp/mma-e2e-two-takes-$n.png)"
    exit 1
  fi
  echo "take $n started: $TAKE"

  sleep "$SECONDS_PER_TAKE"
  DISPLAY="$DISPLAY_NUM" xdotool key Escape
  sleep 1
  click 973 178
  for _ in {1..20}; do
    sleep 1
    python3 -c "import json,sys; sys.exit(0 if json.load(open('$RECORDINGS/$TAKE/session.json')).get('stopTimestamp') else 1)" 2>/dev/null && break
  done

  # The "Saved." card goes up after every take and swallows clicks behind it by
  # design -- which is exactly why take two never started until this gate
  # dismissed it. Its Done button sits at (417,347) relative to the window.
  sleep 1
  click 417 347
  sleep 1

  TAKES+=("$TAKE")
done

[ "${TAKES[0]}" != "${TAKES[1]}" ] || { echo "FAIL: both takes landed in one folder (${TAKES[0]})"; exit 1; }
echo "PASS: two takes, two folders"

FAILED=0
for t in "${TAKES[@]}"; do
  echo "--- verifying $t"
  python3 Tools/verify_take.py "$RECORDINGS/$t" --seconds "$((SECONDS_PER_TAKE - 3))" \
    --tone mma_mic1=440 --tone mma_mic2=1000 --silent-ok mma_out || FAILED=1
done
[ "$FAILED" -eq 0 ] || { echo "FAIL: a take in a two-take session did not verify"; exit 1; }

python3 - "$RECORDINGS/${TAKES[0]}/session.json" "$RECORDINGS/${TAKES[1]}/session.json" <<'PY'
import json, re, sys

def overrun(path):
    for d in json.load(open(path)).get('dropouts') or []:
        m = re.search(r'Dropped (\d+) samples: audio arrived faster', d.get('description', ''))
        if m:
            return int(m.group(1))
    return 0

first, second = overrun(sys.argv[1]), overrun(sys.argv[2])
if first == 0:
    # Nothing overflowed at all, so there is no accumulation to detect. Say so
    # rather than passing silently on a check that did not run.
    print('  NOTE  neither take reported an overrun; the reset check had nothing to measure')
    sys.exit(0)

ratio = second / first
if ratio > 1.6:
    print('  FAIL  take 2 reports %d samples against take 1\'s %d (%.2fx).' % (second, first, ratio))
    print('        A per-take counter is not being reset between takes, so take 2\'s')
    print('        record is carrying take 1\'s losses as well as its own.')
    sys.exit(1)
print('  PASS  take 2\'s loss is its own, not both takes summed (%.2fx)' % ratio)
PY

echo "ALL CHECKS PASSED"
