#!/usr/bin/env bash
# The release checklist's hostile-timing gate, as far as a machine without a
# USB card can take it: the recording card stops answering at Record and at
# Stop, in the REAL app, and the window must keep answering.
#
# A card pulled or wedged mid-call does not fail -- the call never returns.
# Tools/fs_stall_shim.cpp is preloaded into the unmodified app and makes every
# filesystem call that touches the destination folder block for as long as a
# trigger file exists, the way a dead USB mass-storage request blocks.
# MessageThreadStallMeter (test builds only) measures how long the message
# thread -- the thread that draws the window and answers clicks -- went without
# running. That is measured, not inferred from a log line written afterwards.
#
#   Tools/e2e_card_stall.sh
#
# Each card step is allowed to wait out one five-second deadline on the message
# thread and must then give up and say so; the steps after it must not wait
# again. Stalls longer than MMA_MAX_STALL_MS (default 9000: one deadline plus
# scheduling slack on a shared runner) fail the gate. Before the deadlines this
# was a permanent freeze, and with deadlines but no memory of the dead card it
# was one deadline per step -- tens of seconds -- which this gate also catches.
#
# The last scenario holds one write instead of the whole drive: a mid-take
# refresh of the local backup's session.json, inside its rename until after
# Stop. The backup's record must still end on the stop-time one.
#
# Needs Xvfb, xdotool and a build with -DMMA_ALLOW_TEST_INPUTS=ON.
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
MAX_STALL_MS="${MMA_MAX_STALL_MS:-9000}"

newest() {
  local found=""
  for CANDIDATE in "$@"; do
    [ -e "$CANDIDATE" ] || continue
    if [ -z "$found" ] || [ "$CANDIDATE" -nt "$found" ]; then found="$CANDIDATE"; fi
  done
  echo "$found"
}

APP=$(newest build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
             build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage)
SHIM=$(newest build/libfs_stall_shim.so build-app/libfs_stall_shim.so)
test -n "$APP" || { echo "Build the app first"; exit 1; }
test -n "$SHIM" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first (no libfs_stall_shim.so)"; exit 1; }
echo "App: $APP"
echo "Shim: $SHIM"

bash Tools/setup_alsa_fixture.sh >/dev/null
REAL_HOME="$HOME"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mma-card-stall.XXXXXX")
APP_PID=""
FAILED=0

stop_app() {
  if [ -n "$APP_PID" ]; then kill -9 "$APP_PID" 2>/dev/null || true; APP_PID=""; fi
  pkill Xvfb 2>/dev/null || true
  sleep 1
}
cleanup() { rm -f "$WORK"/*.dead; stop_app; }
trap cleanup EXIT

fail() { echo "  FAIL  $*"; FAILED=1; }
pass() { echo "  PASS  $*"; }

click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p')
  y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $(( x + $1 )) $(( y + $2 )) click 1
}

# Each scenario gets its own profile, so a remembered destination, a recovery
# card or a first-run prompt from one cannot change what the next one sees.
# Arguments after the name are extra environment for the app (VAR=value),
# applied after the defaults so they can replace them.
launch() {
  local name="$1"; shift
  SCENARIO_HOME="$WORK/$name"
  CARD="$SCENARIO_HOME/RECORDINGS"
  DEAD="$WORK/$name.dead"
  METER="$WORK/$name.stalls"
  LOG="$SCENARIO_HOME/.config/SobStage/log.txt"
  mkdir -p "$CARD"
  cp "$REAL_HOME/.asoundrc" "$SCENARIO_HOME/"
  rm -f "$DEAD" "$DEAD.caught" "$DEAD.released" "$METER" "$METER.now"

  pkill Xvfb 2>/dev/null || true; sleep 1
  Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
  sleep 2

  env HOME="$SCENARIO_HOME" DISPLAY="$DISPLAY_NUM" \
  MMA_STALL_PREFIX="$CARD" MMA_STALL_TRIGGER="$DEAD" MMA_STALL_METER_FILE="$METER" \
  LD_PRELOAD="$PWD/$SHIM" \
    MMA_SKIP_SETUP_GUIDE=1 "$@" nohup "./$APP" >"$WORK/$name.stdout" 2>&1 &
  APP_PID=$!

  for _ in {1..60}; do
    if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then break; fi
    sleep 1
  done
  DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null \
    || { fail "$name: window never appeared"; return 1; }
  sleep 5  # devices enumerate; the destination's detached checks finish
}

# The card goes away. Stalls counted from here on are the ones this gate is
# about; whatever start-up cost before it is some other gate's business.
kill_card() {
  STALLS_BEFORE=$( (grep -c '^STALL' "$METER" 2>/dev/null) || true)
  STALLS_BEFORE=${STALLS_BEFORE:-0}
  touch "$DEAD"
}

worst_stall_since_card_died() {
  local worst=0 now=0
  if [ -f "$METER" ]; then
    worst=$(tail -n +"$(( STALLS_BEFORE + 1 ))" "$METER" | awk '/^STALL/ { if ($2 > w) w = $2 } END { print w + 0 }')
  fi
  # A window that froze and has not come back has no finished STALL line yet.
  [ -f "$METER.now" ] && now=$(cat "$METER.now" 2>/dev/null || echo 0)
  now=${now:-0}
  [ "$now" -gt "$worst" ] && worst=$now
  echo "$worst"
}

# A session.json's stopTimestamp, or nothing (no file, unreadable, unfinished).
stop_stamp() {
  python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('stopTimestamp',''))" "$1" 2>/dev/null || true
}

wait_for_log() {  # pattern, seconds
  for _ in $(seq 1 "$2"); do
    grep -q "$1" "$LOG" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

# The meter rewrites "<file>.now" by truncating and then writing, so a read
# that lands between the two sees an empty file, which `[ -lt ]` cannot
# compare: a CI run failed on exactly that instant. Read until a number is
# there; a window that has stopped answering leaves the last number in place
# rather than nothing, so waiting cannot turn a frozen window into a pass.
read_now() {
  local value=""
  for _ in $(seq 1 40); do
    value=$(cat "$METER.now" 2>/dev/null || true)
    case "$value" in
      ''|*[!0-9]*) sleep 0.05 ;;
      *) echo "$value"; return ;;
    esac
  done
  echo 99999
}

check_responsive() {  # scenario name
  local worst
  worst=$(worst_stall_since_card_died)
  if [ "$worst" -le "$MAX_STALL_MS" ]; then
    pass "$1: longest the window stopped answering after the card died: ${worst} ms (limit ${MAX_STALL_MS})"
  else
    fail "$1: the window stopped answering for ${worst} ms after the card died (limit ${MAX_STALL_MS})"
  fi
}

press_record() {
  # The first press on a fresh profile raises "Where does this recording go?";
  # its Start button is at (733,540). With no card up that click lands on
  # nothing. Coordinates as in e2e_app_take.sh.
  click 973 178
  sleep 1
  click 733 540
}

dismiss_cards() {
  for DONE_Y in 263 293 312 332; do click 417 "$DONE_Y"; done
  sleep 1
}

# -------------------------------------------------------------------------
echo
echo "=== The card stops answering during a take, and then Stop is pressed ==="
launch stop
dismiss_cards
press_record

TAKE=""
for _ in {1..15}; do
  sleep 1
  TAKE=$(find "$CARD" -mindepth 1 -maxdepth 1 -type d | head -1)
  [ -n "$TAKE" ] && break
done
if [ -z "$TAKE" ]; then
  fail "stop: no take started, so the Stop path was never reached"
else
  pass "stop: take started in $(basename "$TAKE")"
  sleep 3

  kill_card
  sleep 1
  DISPLAY="$DISPLAY_NUM" xdotool key Escape
  click 973 178

  if wait_for_log "stopped answering" 40; then
    pass "stop: the app said the card stopped answering"
  else
    fail "stop: nothing in the log about the card after 40 s"
  fi

  # What the user is told has to be true. After a card that stopped
  # answering, nothing can be called saved -- these lines used to appear
  # straight after the card's own failure message.
  AFTER_DEATH=$(awk '/stopped answering/ { seen = 1 } seen' "$LOG")
  if echo "$AFTER_DEATH" | grep -q "Saved to\|audio itself is saved"; then
    fail "stop: claimed the take was saved after the card stopped answering:"
    echo "$AFTER_DEATH" | grep "Saved to\|audio itself is saved" | sed 's/^/        /'
  else
    pass "stop: nothing was called saved after the card stopped answering"
  fi

  # Still dead. The window has to be answering NOW, not merely have answered
  # once on the way out.
  sleep 3
  check_responsive stop
  if [ "$(read_now)" -lt 1000 ]; then
    pass "stop: window answering while the card is still dead"
  else
    fail "stop: window not answering while the card is still dead"
  fi

  # The card comes back. The abandoned writer finishes on its own worker and
  # the app carries on.
  rm -f "$DEAD"
  sleep 5
  if kill -0 "$APP_PID" 2>/dev/null; then
    pass "stop: app still running after the card came back"
  else
    fail "stop: app exited after the card came back"
  fi
fi
[ "$FAILED" = "0" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== The card stops answering just as Record is pressed ==="
BEFORE_FAIL=$FAILED
launch start
dismiss_cards
kill_card
press_record

if wait_for_log "stopped answering" 30; then
  pass "start: the app said the card stopped answering"
else
  fail "start: nothing in the log about the card after 30 s"
fi

sleep 2
check_responsive start

# One failure, one reason. A vaguer second message used to follow the first
# and became the last word on screen.
if [ "$(grep -c '\[failed\] Recording:' "$LOG" 2>/dev/null || echo 0)" = "1" ]; then
  pass "start: exactly one reason given"
else
  fail "start: more than one failure message for one refused take:"
  grep '\[failed\] Recording:' "$LOG" | sed 's/^/        /'
fi

if grep -q "Recording started" "$LOG" 2>/dev/null; then
  fail "start: a take was reported as started on a card that never answered"
else
  pass "start: no take was claimed"
fi

rm -f "$DEAD"
sleep 3
if kill -0 "$APP_PID" 2>/dev/null; then
  pass "start: app still running after the card came back"
else
  fail "start: app exited after the card came back"
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== The backup drive holds a mid-take refresh of session.json past Stop ==="
# The local backup's session.json is refreshed mid-take on a worker. A slow
# backup drive still inside one of those renames when Stop is pressed must not
# get it put in place after the stop-time record, or the backup's record
# describes an unfinished take. Only that one rename is held (the shim's
# MMA_STALL_RENAME_ONTO); every other call, the stop-time record's own write
# included, goes through. It is let go only after Stop has finished with the
# backup. MMA_KEEP_SAME_DISK_BACKUP: the backup shares the scratch home's disk
# with the take, and test builds keep it anyway.
BEFORE_FAIL=$FAILED
BACKUP_ROOT="$WORK/backup/RECORDINGS-MIRROR"
launch backup MMA_STALL_PREFIX="$BACKUP_ROOT" MMA_STALL_RENAME_ONTO=session.json \
              MMA_KEEP_SAME_DISK_BACKUP=1
dismiss_cards
press_record

TAKE=""
BACKUP_TAKE=""
for _ in {1..15}; do
  sleep 1
  TAKE=$(find "$CARD" -mindepth 1 -maxdepth 1 -type d | head -1)
  BACKUP_TAKE=$(find "$BACKUP_ROOT" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1 || true)
  [ -n "$TAKE" ] && [ -n "$BACKUP_TAKE" ] && [ -f "$BACKUP_TAKE/session.json" ] && break
done
if [ -z "$TAKE" ] || [ -z "$BACKUP_TAKE" ] || [ ! -f "$BACKUP_TAKE/session.json" ]; then
  fail "backup: no take with a backup copy started, so there was nothing to hold"
else
  pass "backup: take started in $(basename "$TAKE"), backed up to $(basename "$BACKUP_TAKE")"
  sleep 2

  # From here the next rename onto the backup's session.json is held: the
  # thirty-second refresh, or an earlier one if something worth recording
  # happens first.
  kill_card
  CAUGHT=""
  for _ in {1..45}; do
    [ -e "$DEAD.caught" ] && { CAUGHT=yes; break; }
    sleep 1
  done

  if [ -z "$CAUGHT" ]; then
    fail "backup: no refresh of the backup's session.json reached its rename in 45 s, so nothing was held"
  else
    pass "backup: a mid-take refresh is held inside its rename on the backup drive"

    DISPLAY="$DISPLAY_NUM" xdotool key Escape
    sleep 1
    click 973 178

    STOPPED=""
    for _ in {1..30}; do
      [ -n "$(stop_stamp "$TAKE/session.json")" ] && { STOPPED=yes; break; }
      sleep 1
    done
    if [ -n "$STOPPED" ]; then pass "backup: the card's copy has its stop-time record"
    else fail "backup: the card's session.json has no stop time 30 s after Stop"; fi

    # Stop waits at most one deadline for the backup's record; by now it has
    # finished with the backup, whatever it decided. Then the drive lets the
    # held refresh go.
    sleep 8
    rm -f "$DEAD"
    RELEASED=""
    for _ in {1..20}; do
      [ -e "$DEAD.released" ] && { RELEASED=yes; break; }
      sleep 1
    done
    [ -n "$RELEASED" ] || fail "backup: the held refresh never finished its rename"
    # Anything ordered behind the refresh lands straight after it.
    sleep 3

    if [ -n "$(stop_stamp "$BACKUP_TAKE/session.json")" ]; then
      pass "backup: the backup's session.json still has its stop time after the held refresh landed"
    else
      fail "backup: the held refresh was put in place over the stop-time record; the backup's session.json describes an unfinished take"
    fi
  fi

  check_responsive backup
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== This computer's backup drive stops answering, and then Stop is pressed ==="
# The take goes straight to the card; its backup copy is on this computer, and
# that drive stops answering. Each backup step at Stop is bounded, and once
# one has timed out the ones after it must not wait again: each waiting its
# own deadline was 40 s of frozen window. (The take's writer feeds the card
# and the backup on one thread, so its stop timing out cannot say which drive
# stopped, and the card's record is skipped as for a stuck card. That is the
# writer's to tell apart; this scenario holds the window to account.)
BEFORE_FAIL=$FAILED
BACKUP_ROOT="$WORK/backupdead/RECORDINGS-MIRROR"
launch backupdead MMA_STALL_PREFIX="$BACKUP_ROOT" MMA_KEEP_SAME_DISK_BACKUP=1
dismiss_cards
press_record

TAKE=""
BACKUP_TAKE=""
for _ in {1..15}; do
  sleep 1
  TAKE=$(find "$CARD" -mindepth 1 -maxdepth 1 -type d | head -1)
  BACKUP_TAKE=$(find "$BACKUP_ROOT" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1 || true)
  [ -n "$TAKE" ] && [ -n "$BACKUP_TAKE" ] && break
done
if [ -z "$TAKE" ] || [ -z "$BACKUP_TAKE" ]; then
  fail "backupdead: no take with a backup copy started"
else
  pass "backupdead: take started in $(basename "$TAKE"), backed up to $(basename "$BACKUP_TAKE")"
  sleep 3

  kill_card
  sleep 1
  DISPLAY="$DISPLAY_NUM" xdotool key Escape
  click 973 178

  if wait_for_log "Recording stopped\|Recording was stopped" 45; then
    pass "backupdead: the stop finished"
  else
    fail "backupdead: the stop was never logged within 45 s"
  fi

  sleep 3
  check_responsive backupdead
  rm -f "$DEAD"
  sleep 3
  if kill -0 "$APP_PID" 2>/dev/null; then
    pass "backupdead: app still running after the backup drive came back"
  else
    fail "backupdead: app exited after the backup drive came back"
  fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

echo
if [ "$FAILED" = "0" ]; then
  echo "Card-stall gate passed."
else
  echo "Card-stall gate FAILED."
  exit 1
fi
