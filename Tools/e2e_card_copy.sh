#!/usr/bin/env bash
# Record on this computer, then copy to the card -- in the real app.
#
# A take meant for an SD card or USB stick is recorded into
# ~/RECORDINGS-MIRROR and copied to the card after Stop, read back and checked
# before any file on the card gets its real name. This gate drives that end to
# end: Xvfb, real clicks, the real ALSA fixture microphones, and the card
# stood in for by a folder the test shim (Tools/fs_stall_shim.cpp) can stop
# answering on demand, the way a dead USB card does.
#
#   copy     nothing touches the card while the take records; after Stop the
#            card's files match the computer's byte for byte, and the
#            computer keeps its copy
#   stall    the card stops answering during the copy: the window keeps
#            answering, the user is told the take is safe on this computer,
#            and the copy finishes by itself when the card comes back
#   second   a second take started while the first is copying: the copy steps
#            aside, nothing reaches the card until the second take stops, and
#            the second take records cleanly
#   crash    the app is killed mid-copy: the next launch finishes the copy
#
# The scratch home and the "card" are on one disk, so MMA_TREAT_AS_CARD (test
# builds only) makes the card folder count as a card.
#
#   Tools/e2e_card_copy.sh
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
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mma-card-copy.XXXXXX")
APP_PID=""
FAILED=0

stop_app() {
  if [ -n "$APP_PID" ]; then kill -9 "$APP_PID" 2>/dev/null || true; APP_PID=""; fi
  pkill Xvfb 2>/dev/null || true
  sleep 1
}
cleanup() { rm -f "$WORK"/*.dead; stop_app; }
trap cleanup EXIT

fail() { echo "  FAIL  $*"; FAILED=$(( FAILED + 1 )); }
pass() { echo "  PASS  $*"; }

click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p')
  y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $(( x + $1 )) $(( y + $2 )) click 1
}

# Each scenario has its own home, with the card chosen and confirmed as the
# save location already, as it is for someone who has recorded to it before.
# `fresh` starts from an empty home; `again` relaunches the last one as it was
# left. Arguments after those two are extra environment for the app.
launch() {
  local name="$1" mode="$2"; shift 2
  SCENARIO_HOME="$WORK/$name"
  CARD_ROOT="$SCENARIO_HOME/CARD"
  CARD="$CARD_ROOT/RECORDINGS"
  STAGING="$SCENARIO_HOME/RECORDINGS-MIRROR"
  DEAD="$WORK/$name.dead"
  METER="$WORK/$name.stalls"
  LOG="$SCENARIO_HOME/.config/SobStage/log.txt"

  if [ "$mode" = fresh ]; then
    mkdir -p "$CARD" "$SCENARIO_HOME/.config/SobStage"
    cp "$REAL_HOME/.asoundrc" "$SCENARIO_HOME/"
    python3 - "$SCENARIO_HOME/.config/SobStage/settings.json" "$CARD" <<'PY'
import json, sys
json.dump({ "destinationFolder": sys.argv[2], "confirmedSaveLocation": sys.argv[2],
            "setupGuideDone": True }, open(sys.argv[1], "w"), indent=2)
PY
    rm -f "$DEAD" "$DEAD.caught" "$DEAD.released" "$METER" "$METER.now"
  fi

  pkill Xvfb 2>/dev/null || true; sleep 1
  Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
  sleep 2

  env HOME="$SCENARIO_HOME" DISPLAY="$DISPLAY_NUM" \
  MMA_TREAT_AS_CARD="$CARD_ROOT" \
  MMA_STALL_PREFIX="$CARD" MMA_STALL_TRIGGER="$DEAD" MMA_STALL_METER_FILE="$METER" \
  LD_PRELOAD="$PWD/$SHIM" \
    MMA_SKIP_SETUP_GUIDE=1 "$@" nohup "./$APP" >"$WORK/$name.$mode.stdout" 2>&1 &
  APP_PID=$!

  for _ in {1..60}; do
    if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then break; fi
    sleep 1
  done
  DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null \
    || { fail "$name: window never appeared"; return 1; }
  sleep 5  # devices enumerate; the launch checks finish
}

dismiss_cards() {
  for DONE_Y in 263 293 312 332; do click 417 "$DONE_Y"; done
  sleep 1
}

press_record() {
  # The record button; and the save prompt's Start button, in case one came
  # up. With no card up that second click lands on nothing.
  click 973 178
  sleep 1
  click 733 540
}

press_stop() {
  DISPLAY="$DISPLAY_NUM" xdotool key Escape
  sleep 1
  click 973 178
}

# The "Saved." card swallows clicks behind it by design; Escape is its Done.
dismiss_saved() {
  sleep 2
  DISPLAY="$DISPLAY_NUM" xdotool key Escape
  sleep 1
}

# The newest take folder in a root, by name (they start with the date and time).
newest_take() {
  find "$1" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort | tail -1
}

wait_for_new_take() {  # root, take to skip
  local take=""
  for _ in {1..15}; do
    sleep 1
    take=$(newest_take "$1")
    [ -n "$take" ] && [ "$take" != "${2:-}" ] && { echo "$take"; return 0; }
  done
  return 1
}

wait_for_log() {  # pattern, seconds
  for _ in $(seq 1 "$2"); do
    grep -q "$1" "$LOG" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

regular_files_under() {
  find "$1" -type f 2>/dev/null | wc -l
}

# Every file the copy carries, byte for byte, and nothing half-written.
check_copy_matches() {  # scenario, local take folder
  local name="$1" local_take="$2" card_take ok=1
  card_take="$CARD/$(basename "$local_take")"

  for f in "$local_take"/*; do
    [ -f "$f" ] || continue
    if ! cmp -s "$f" "$card_take/$(basename "$f")"; then
      fail "$name: $(basename "$f") on the card differs from the computer's (or is missing)"
      ok=0
    fi
  done
  [ "$ok" = 1 ] && pass "$name: every file on the card matches the computer's, byte for byte"

  if find "$card_take" -name '*.sobcopy' 2>/dev/null | grep -q .; then
    fail "$name: a half-written .sobcopy file was left on the card"
  else
    pass "$name: nothing half-written is left on the card"
  fi

  if [ -e "$local_take/.sobstage-card-copy.json" ]; then
    fail "$name: the computer's copy is still marked to be copied"
  else
    pass "$name: the computer's copy is no longer marked to be copied"
  fi

  if [ -f "$local_take/MIX.wav" ]; then
    pass "$name: the computer keeps its copy"
  else
    fail "$name: the computer's copy is gone"
  fi
  CARD_TAKE="$card_take"
}

worst_stall_since() {  # meter lines to skip
  local worst=0 now=0
  if [ -f "$METER" ]; then
    worst=$(tail -n +"$(( $1 + 1 ))" "$METER" | awk '/^STALL/ { if ($2 > w) w = $2 } END { print w + 0 }')
  fi
  [ -f "$METER.now" ] && now=$(cat "$METER.now" 2>/dev/null || echo 0)
  case "$now" in ''|*[!0-9]*) now=0 ;; esac
  [ "$now" -gt "$worst" ] && worst=$now
  echo "$worst"
}

meter_lines() { (grep -c '^STALL' "$METER" 2>/dev/null) || echo 0; }

verify() {  # scenario, take folder, seconds
  if python3 Tools/verify_take.py "$2" --seconds "$3" \
       --tone mma_mic1=440 --tone mma_mic2=1000 --silent-ok mma_out --no-mirror >"$WORK/$1.verify" 2>&1; then
    pass "$1: verify_take.py passes on $(basename "$(dirname "$2")")/$(basename "$2")"
  else
    fail "$1: verify_take.py failed on $2:"
    tail -15 "$WORK/$1.verify" | sed 's/^/        /'
  fi
}

# -------------------------------------------------------------------------
echo
echo "=== A take for the card is recorded on this computer and copied after Stop ==="
launch copy fresh
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "copy: no take started in $STAGING"
else
  pass "copy: take started on this computer in $(basename "$TAKE")"

  # Nothing touches the card while the take records: not a folder, not a file.
  TOUCHED=""
  for _ in $(seq 1 16); do
    sleep 0.5
    if find "$CARD" -mindepth 1 | grep -q .; then TOUCHED=$(find "$CARD" -mindepth 1 | head -3); break; fi
  done
  if [ -z "$TOUCHED" ]; then pass "copy: nothing was written to the card during the take"
  else fail "copy: the card was written to during the take: $TOUCHED"; fi

  press_stop
  if wait_for_log "Saved on this computer" 20; then pass "copy: Stop said the take is saved on this computer"
  else fail "copy: Stop did not say the take is saved on this computer"; fi

  if wait_for_log "Copied .* to the card" 60; then
    pass "copy: the copy to the card was reported done"
    check_copy_matches copy "$TAKE"
    verify copy "$CARD_TAKE" 5
  else
    fail "copy: no copy to the card was reported within 60 s"
  fi
fi
[ "$FAILED" = "0" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== The card stops answering during the copy ==="
BEFORE_FAIL=$FAILED
launch stall fresh
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "stall: no take started in $STAGING"
else
  pass "stall: take started on this computer in $(basename "$TAKE")"
  sleep 6

  STALLS_BEFORE=$(meter_lines)
  touch "$DEAD"
  press_stop

  if wait_for_log "stopped answering" 25 && grep -q "safe on this computer in" "$LOG"; then
    pass "stall: the user was told the card stopped answering and the take is safe on this computer"
  else
    fail "stall: no 'stopped answering ... safe on this computer' within 25 s"
  fi

  if grep -q "Copied .* to the card" "$LOG"; then
    fail "stall: a copy was reported done while the card was not answering"
  else
    pass "stall: nothing was called copied while the card was not answering"
  fi

  WORST=$(worst_stall_since "$STALLS_BEFORE")
  if [ "$WORST" -le "$MAX_STALL_MS" ]; then
    pass "stall: longest the window stopped answering: ${WORST} ms (limit ${MAX_STALL_MS})"
  else
    fail "stall: the window stopped answering for ${WORST} ms (limit ${MAX_STALL_MS})"
  fi

  verify stall "$TAKE" 3

  rm -f "$DEAD"
  if wait_for_log "Copied .* to the card" 60; then
    pass "stall: the card came back and the copy finished by itself"
    check_copy_matches stall "$TAKE"
  else
    fail "stall: the copy did not finish within 60 s of the card coming back"
  fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== A second take is started while the first is copying ==="
BEFORE_FAIL=$FAILED
launch second fresh
dismiss_cards
press_record

if ! TAKE1=$(wait_for_new_take "$STAGING"); then
  fail "second: the first take did not start"
else
  sleep 5
  touch "$DEAD"
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "second: the first take was not saved"
  dismiss_saved

  BEFORE_FILES=$(regular_files_under "$CARD")
  press_record
  if ! TAKE2=$(wait_for_new_take "$STAGING" "$TAKE1"); then
    fail "second: the second take did not start"
  else
    pass "second: the second take started in $(basename "$TAKE2") while the first was copying"

    # Halfway through, the card answers again. The copy must still wait.
    sleep 4
    rm -f "$DEAD"
    LEAKED=""
    for _ in $(seq 1 8); do
      sleep 0.5
      NOW_FILES=$(regular_files_under "$CARD")
      [ "$NOW_FILES" -gt "$BEFORE_FILES" ] && { LEAKED=$NOW_FILES; break; }
    done
    if [ -z "$LEAKED" ]; then pass "second: nothing was written to the card while the second take recorded"
    else fail "second: files appeared on the card during the second take ($BEFORE_FILES -> $LEAKED)"; fi

    press_stop
    wait_for_log "Saved on this computer in $(basename "$TAKE2")" 20 \
      || fail "second: the second take was not saved"

    if grep -q "falling behind\|Some sound was dropped" "$LOG"; then
      fail "second: the second take reported falling behind or dropped sound"
    else
      pass "second: the second take recorded without falling behind or dropping sound"
    fi
    verify second "$TAKE2" 5

    COPIED=""
    for _ in $(seq 1 60); do
      [ "$(grep -c 'Copied .* to the card' "$LOG" 2>/dev/null || echo 0)" -ge 2 ] && { COPIED=yes; break; }
      sleep 1
    done
    if [ -n "$COPIED" ]; then
      pass "second: both takes reached the card"
      for t in "$TAKE1" "$TAKE2"; do
        ok=1
        for f in "$t"/*; do
          [ -f "$f" ] || continue
          cmp -s "$f" "$CARD/$(basename "$t")/$(basename "$f")" || ok=0
        done
        [ "$ok" = 1 ] && pass "second: $(basename "$t") on the card matches the computer's" \
                      || fail "second: $(basename "$t") on the card differs from the computer's"
      done
    else
      fail "second: both takes were not copied within 60 s"
    fi
  fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== The app is killed in the middle of a copy ==="
# Only the last step of the copy is held -- session.json's rename to its real
# name on the card -- so the kill lands with every other file already on the
# card and session.json's working copy half-way there.
BEFORE_FAIL=$FAILED
launch crash fresh MMA_STALL_RENAME_ONTO=session.json
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "crash: no take started"
else
  sleep 6
  touch "$DEAD"
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "crash: the take was not saved"

  CAUGHT=""
  for _ in {1..30}; do
    [ -e "$DEAD.caught" ] && { CAUGHT=yes; break; }
    sleep 1
  done
  [ -n "$CAUGHT" ] && pass "crash: the copy is held at its last file"                    || fail "crash: the copy never reached session.json's rename"

  kill -9 "$APP_PID" 2>/dev/null || true
  APP_PID=""
  stop_app
  rm -f "$DEAD"

  if [ -e "$TAKE/.sobstage-card-copy.json" ]; then
    pass "crash: the take is still marked to be copied after the kill"
  else
    fail "crash: the take lost its mark when the app was killed"
  fi

  if [ -e "$CARD/$(basename "$TAKE")/session.json" ]; then
    fail "crash: session.json reached the card before the rest was checked"
  else
    pass "crash: the card's copy has no session.json, so it does not count as a take yet"
  fi

  LINES_BEFORE=$(wc -l < "$LOG")
  launch crash again
  dismiss_cards

  if wait_for_log "Copying 1 earlier take to the card" 40; then
    pass "crash: the next launch picked the copy up again"
  else
    fail "crash: the next launch did not say it was copying the earlier take"
  fi

  COPIED=""
  for _ in $(seq 1 60); do
    tail -n +"$(( LINES_BEFORE + 1 ))" "$LOG" | grep -q "Copied .* to the card" && { COPIED=yes; break; }
    sleep 1
  done
  if [ -n "$COPIED" ]; then
    pass "crash: the copy finished after the relaunch"
    check_copy_matches crash "$TAKE"
  else
    fail "crash: the copy did not finish within 60 s of the relaunch"
  fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

echo
if [ "$FAILED" = "0" ]; then
  echo "Card-copy gate passed."
else
  echo "Card-copy gate FAILED."
  exit 1
fi
