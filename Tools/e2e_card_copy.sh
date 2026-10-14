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
#   quit     the window is closed while a copy runs and "Quit When Done" is
#            chosen; the card is pulled before the copy finishes. The app
#            does not quit as though the take were on the card: it says the
#            copy did not finish and where the take is safe, and stays open
#            on "Don't Quit"
#   requit   as quit, for a retry: the copy fails with the card pulled, is
#            tried again once the card is back, and that retry -- the one
#            "Quit When Done" waits for -- fails just as the first did. Not
#            news to the activity log, but still asked about before quitting
#   cardback the card is out at Stop and put back in: the copy goes at once,
#            not at the next timed retry, and closing the window straight
#            after asks to wait for it; "Quit When Done" quits once the take
#            is on the card
#   othercard the card is out when Record is pressed, and another card -- the
#            same name, a different size, as a camera card is -- goes in
#            first: nothing is written to it, and the take waits for the card
#            last seen at the save location, which it then reaches
#   herestall this computer's drive stops answering as a take recorded here
#            is stopped: what is said names this computer's drive, never the
#            card the take never touched
#   copyhere as othercard, with the saved-take card closed: "Copy to this
#            card" under the record button -- the button the take's line
#            names -- copies it to the card that is in
#
# MMA_E2E_ONLY="quit requit" runs only the scenarios named.
#
# The scratch home and the "card" are on one disk, so MMA_TREAT_AS_CARD (test
# builds only) makes the card folder count as a card, and the shim has the
# card report 4 GB free: on one disk, a runner with less than four hours of
# takes free would otherwise send every take straight to the "card".
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
WINMSG=$(newest build/x11_window_message build-app/x11_window_message)
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

# MMA_E2E_ONLY="quit requit" runs only those scenarios; unset, all of them.
want() { [ -z "${MMA_E2E_ONLY:-}" ] || [[ " $MMA_E2E_ONLY " == *" $1 "* ]]; }

wait_for_file() {  # path, seconds
  for _ in $(seq 1 "$(( $2 * 2 ))"); do
    [ -e "$1" ] && return 0
    sleep 0.5
  done
  return 1
}

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
  # -noreset: the window-manager atoms made below must outlive their maker.
  Xvfb "$DISPLAY_NUM" -noreset -screen 0 1280x1200x24 >/dev/null 2>&1 &
  sleep 2
  [ -n "$WINMSG" ] && DISPLAY="$DISPLAY_NUM" "./$WINMSG" atoms

  env HOME="$SCENARIO_HOME" DISPLAY="$DISPLAY_NUM" \
  MMA_TREAT_AS_CARD="$CARD_ROOT" \
  MMA_STALL_PREFIX="$CARD" MMA_STALL_TRIGGER="$DEAD" MMA_STALL_METER_FILE="$METER" \
  MMA_STALL_CARD_FREE_MB=4096 \
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

if want copy; then
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
fi

if want stall; then
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
fi

if want second; then
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
fi

if want crash; then
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
fi

if want quit; then
# -------------------------------------------------------------------------
echo
echo "=== Quit When Done, and the copy it waits for fails ==="
# The copy is held at its last step, as in "crash"; the window is closed the
# way a window manager's close button does, and "Quit When Done" chosen. Then
# the card is pulled, so the copy the quit was waiting for fails.
BEFORE_FAIL=$FAILED
if [ -z "$WINMSG" ]; then
  fail "quit: no x11_window_message (needs X11 headers at build time)"
else
launch quit fresh MMA_STALL_RENAME_ONTO=session.json
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "quit: no take started"
else
  sleep 5
  touch "$DEAD"
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "quit: the take was not saved"
  dismiss_saved

  CAUGHT=""
  for _ in {1..30}; do
    [ -e "$DEAD.caught" ] && { CAUGHT=yes; break; }
    sleep 1
  done
  [ -n "$CAUGHT" ] && pass "quit: the copy is under way, held at its last file" \
                   || fail "quit: the copy never reached session.json's rename"

  MAIN_WIN=$(DISPLAY="$DISPLAY_NUM" xdotool search --name '^SobStage$' | head -1 || true)
  DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$MAIN_WIN" close

  ASK=""
  for _ in {1..20}; do
    ASK=$(DISPLAY="$DISPLAY_NUM" xdotool search --name 'Still saving the copy to the card' 2>/dev/null | head -1 || true)
    [ -n "$ASK" ] && break
    sleep 0.5
  done

  if [ -z "$ASK" ]; then
    fail "quit: closing the window did not ask about the copy still being saved"
  else
    pass "quit: closing the window asked about the copy to the card still being saved"
    DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$ASK" focus
    sleep 0.5
    DISPLAY="$DISPLAY_NUM" xdotool key Return   # Quit When Done
    sleep 2

    if kill -0 "$APP_PID" 2>/dev/null; then
      pass "quit: Quit When Done waits for the copy"
    else
      fail "quit: the app quit at once instead of waiting for the copy"
    fi

    # The card is pulled while the copy finishes: its last rename fails.
    rm -rf "$CARD_ROOT"
    rm -f "$DEAD"

    PROBLEM=""
    for _ in {1..20}; do
      kill -0 "$APP_PID" 2>/dev/null || break
      PROBLEM=$(DISPLAY="$DISPLAY_NUM" xdotool search --name 'problem with the take' 2>/dev/null | head -1 || true)
      [ -n "$PROBLEM" ] && break
      sleep 0.5
    done

    if ! kill -0 "$APP_PID" 2>/dev/null; then
      fail "quit: the app quit as though the take were on the card, after its copy failed"
      APP_PID=""
    elif [ -z "$PROBLEM" ]; then
      fail "quit: the failed copy was not asked about"
    else
      pass "quit: the app asked before quitting, because the copy to the card failed"

      if wait_for_log "The card isn't plugged in" 5; then
        pass "quit: the failure, and where the take is safe, is in the activity log"
      else
        fail "quit: the failed copy is not in the activity log"
      fi

      DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$PROBLEM" focus
      sleep 0.5
      DISPLAY="$DISPLAY_NUM" xdotool key Escape   # Don't Quit
      sleep 3
      if kill -0 "$APP_PID" 2>/dev/null; then
        pass "quit: Don't Quit keeps SobStage open"
      else
        fail "quit: SobStage quit after Don't Quit"
        APP_PID=""
      fi
    fi

    if [ -e "$TAKE/.sobstage-card-copy.json" ] && [ -f "$TAKE/MIX.wav" ]; then
      pass "quit: the take is safe on this computer, still marked to be copied"
    else
      fail "quit: the take on this computer lost its mark or its sound"
    fi
  fi
fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app
fi

if want requit; then
# -------------------------------------------------------------------------
echo
echo "=== Quit When Done, and the retry it waits for fails as the first copy did ==="
# The card is pulled as the take's copy finishes, so the copy fails. The card
# goes back in and the copy is tried again by itself; the window is closed
# while that retry is held at its last step, and "Quit When Done" chosen.
# The reader drops out again, and the retry fails exactly as the first
# attempt did. A failure like the last one is not news to the activity log,
# but it is to someone about to leave with the card: the app still asks.
BEFORE_FAIL=$FAILED
if [ -z "$WINMSG" ]; then
  fail "requit: no x11_window_message (needs X11 headers at build time)"
else
# The timed retry far off: the retry after the card is back is the one its
# being seen back starts, so the first copy's failure is the last before it.
launch requit fresh MMA_STALL_RENAME_ONTO=session.json MMA_CARD_RETRY_SECONDS=600
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "requit: no take started"
else
  sleep 5
  touch "$DEAD"
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "requit: the take was not saved"
  dismiss_saved

  wait_for_file "$DEAD.caught" 30 || fail "requit: the copy never reached session.json's rename"

  # Pulled as the copy finishes: its last rename fails, the card gone. Put
  # back once that failure is in, with the next rename onto session.json
  # held again.
  rm -rf "$CARD_ROOT"
  rm -f "$DEAD"
  wait_for_file "$DEAD.released" 20 || fail "requit: the held rename never finished"
  if wait_for_log "The card isn't plugged in" 10; then
    pass "requit: the first copy failed with the card pulled"
  else
    fail "requit: the first copy's failure was not reported"
  fi
  FIRST_FAILURES=$(grep -c "The card isn't plugged in" "$LOG" || true)

  rm -f "$DEAD.caught" "$DEAD.released"
  touch "$DEAD"
  mkdir -p "$CARD"

  if ! wait_for_file "$DEAD.caught" 40; then
    fail "requit: the copy was not tried again, once the card was back, up to its last file"
  else
    pass "requit: the card back, the copy is tried again by itself and held at its last file"

    MAIN_WIN=$(DISPLAY="$DISPLAY_NUM" xdotool search --name '^SobStage$' | head -1 || true)
    DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$MAIN_WIN" close

    ASK=""
    for _ in {1..20}; do
      ASK=$(DISPLAY="$DISPLAY_NUM" xdotool search --name 'Still saving the copy to the card' 2>/dev/null | head -1 || true)
      [ -n "$ASK" ] && break
      sleep 0.5
    done

    if [ -z "$ASK" ]; then
      fail "requit: closing the window did not ask about the copy still being saved"
    else
      pass "requit: closing the window asked about the copy to the card still being saved"
      DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$ASK" focus
      sleep 0.5
      DISPLAY="$DISPLAY_NUM" xdotool key Return   # Quit When Done
      sleep 2
      kill -0 "$APP_PID" 2>/dev/null || fail "requit: the app quit at once instead of waiting for the copy"

      # The reader drops out again: the retry fails just as the first copy did.
      rm -rf "$CARD_ROOT"
      rm -f "$DEAD"

      PROBLEM=""
      for _ in {1..20}; do
        kill -0 "$APP_PID" 2>/dev/null || break
        PROBLEM=$(DISPLAY="$DISPLAY_NUM" xdotool search --name 'problem with the take' 2>/dev/null | head -1 || true)
        [ -n "$PROBLEM" ] && break
        sleep 0.5
      done

      AGAIN=$(grep -c "The card isn't plugged in" "$LOG" || true)
      [ "$AGAIN" = "$FIRST_FAILURES" ] \
        && echo "  (the retry's failure was a repeat of the first: not written to the log again)"

      if ! kill -0 "$APP_PID" 2>/dev/null; then
        fail "requit: the app quit as though the take were on the card, after the retry it waited for failed"
        APP_PID=""
      elif [ -z "$PROBLEM" ]; then
        fail "requit: the failed retry was not asked about"
      else
        pass "requit: the app asked before quitting, because the retry it waited for failed"
        DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$PROBLEM" focus
        sleep 0.5
        DISPLAY="$DISPLAY_NUM" xdotool key Escape   # Don't Quit
        sleep 3
        if kill -0 "$APP_PID" 2>/dev/null; then
          pass "requit: Don't Quit keeps SobStage open"
        else
          fail "requit: SobStage quit after Don't Quit"
          APP_PID=""
        fi
      fi

      if [ -e "$TAKE/.sobstage-card-copy.json" ] && [ -f "$TAKE/MIX.wav" ]; then
        pass "requit: the take is safe on this computer, still marked to be copied"
      else
        fail "requit: the take on this computer lost its mark or its sound"
      fi
    fi
  fi
fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app
fi

if want cardback; then
# -------------------------------------------------------------------------
echo
echo "=== The card is out at Stop, and put back in ==="
# The line under Record says the take will be copied when the card is back.
# Put back, it goes at once -- the timed retry is set far off here, so only
# the card being seen can start it -- and a quit straight after waits for it.
# The copy is held at its last step, so the question is there to be found.
BEFORE_FAIL=$FAILED
if [ -z "$WINMSG" ]; then
  fail "cardback: no x11_window_message (needs X11 headers at build time)"
else
launch cardback fresh MMA_STALL_RENAME_ONTO=session.json MMA_CARD_RETRY_SECONDS=600
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "cardback: no take started"
else
  sleep 4
  mv "$CARD_ROOT" "$CARD_ROOT.out"
  sleep 1
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "cardback: the take was not saved"
  dismiss_saved

  if wait_for_log "The card isn't plugged in" 20; then
    pass "cardback: with the card out, the take is safe on this computer"
  else
    fail "cardback: the copy with the card out was not reported"
  fi
  sleep 3   # seen out between takes

  touch "$DEAD"
  mv "$CARD_ROOT.out" "$CARD_ROOT"

  if wait_for_file "$DEAD.caught" 20; then
    pass "cardback: the card back in, the copy goes at once"
  else
    fail "cardback: the card back in, nothing was copied within 20 s"
  fi

  MAIN_WIN=$(DISPLAY="$DISPLAY_NUM" xdotool search --name '^SobStage$' | head -1 || true)
  DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$MAIN_WIN" close

  ASK=""
  for _ in {1..20}; do
    kill -0 "$APP_PID" 2>/dev/null || break
    ASK=$(DISPLAY="$DISPLAY_NUM" xdotool search --name 'Still saving the copy to the card' 2>/dev/null | head -1 || true)
    [ -n "$ASK" ] && break
    sleep 0.5
  done

  if ! kill -0 "$APP_PID" 2>/dev/null; then
    fail "cardback: closing the window quit at once, with the card in and its take not on it"
    APP_PID=""
  elif [ -z "$ASK" ]; then
    fail "cardback: closing the window did not ask about the copy to the card"
  else
    pass "cardback: closing the window asked to wait for the copy to the card"
    DISPLAY="$DISPLAY_NUM" "./$WINMSG" "$ASK" focus
    sleep 0.5
    DISPLAY="$DISPLAY_NUM" xdotool key Return   # Quit When Done
    sleep 2
    kill -0 "$APP_PID" 2>/dev/null || fail "cardback: the app quit at once instead of waiting for the copy"
    rm -f "$DEAD"

    GONE=""
    for _ in {1..40}; do
      kill -0 "$APP_PID" 2>/dev/null || { GONE=yes; break; }
      sleep 0.5
    done
    if [ -n "$GONE" ]; then
      APP_PID=""
      pass "cardback: once the copy finished, the app quit by itself"
    else
      fail "cardback: the app did not quit within 20 s of the copy being let go"
    fi
    check_copy_matches cardback "$TAKE"
  fi
fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app
fi

if want othercard; then
# -------------------------------------------------------------------------
echo
echo "=== The card is out at Record, and another card goes in first ==="
# The shim has the card report itself a FAT card of the size in $FATMB: one
# card is told from another by that, as a camera card called "NO NAME" is
# told from the user's own. The user's card is seen at the save location at
# launch, then taken out; the take is recorded with it out; another card of
# another size goes in at the same place.
BEFORE_FAIL=$FAILED
FATMB="$WORK/othercard.fatmb"
echo 7600 > "$FATMB"
launch othercard fresh MMA_STALL_CARD_FAT_MB_FILE="$FATMB"
dismiss_cards
sleep 2   # the card seen, and remembered, at its save location

mv "$CARD_ROOT" "$CARD_ROOT.users"
sleep 2
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "othercard: no take started with the card out"
else
  pass "othercard: with the card out, the take started on this computer in $(basename "$TAKE")"
  sleep 4
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "othercard: the take was not saved"
  dismiss_saved
  wait_for_log "The card isn't plugged in" 20 || fail "othercard: the copy with the card out was not reported"

  # Another card at the same place: the same path, another size.
  echo 3800 > "$FATMB"
  mkdir -p "$CARD"

  if wait_for_log "isn't the one this take is for" 30; then
    pass "othercard: the card in now is said not to be the take's"
  else
    fail "othercard: the other card was not refused within 30 s"
  fi
  if grep -q "Copied .* to the card" "$LOG"; then
    fail "othercard: the take was reported copied to another card"
  else
    pass "othercard: nothing was called copied"
  fi
  if [ -e "$CARD/$(basename "$TAKE")" ]; then
    fail "othercard: the take was written to another card"
  else
    pass "othercard: nothing of the take was written to the other card"
  fi

  # The user's own card back.
  LINES_BEFORE=$(wc -l < "$LOG")
  rm -rf "$CARD_ROOT"
  mv "$CARD_ROOT.users" "$CARD_ROOT"
  echo 7600 > "$FATMB"
  COPIED=""
  for _ in $(seq 1 40); do
    tail -n +"$(( LINES_BEFORE + 1 ))" "$LOG" | grep -q "Copied .* to the card" && { COPIED=yes; break; }
    sleep 1
  done
  if [ -n "$COPIED" ]; then
    pass "othercard: its own card back, the take is copied to it"
    check_copy_matches othercard "$TAKE"
  else
    fail "othercard: the take did not reach its own card within 40 s"
  fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app
fi

if want herestall; then
# -------------------------------------------------------------------------
echo
echo "=== This computer's drive stops answering at Stop, for a take recorded here ==="
# The shim's prefix is the take's folder on this computer, not the card: the
# take's last writes, and its finishing, wait on a drive that has stopped.
BEFORE_FAIL=$FAILED
launch herestall fresh MMA_STALL_PREFIX="$WORK/herestall/RECORDINGS-MIRROR" \
  MMA_STALL_CARD_PREFIX="$WORK/herestall/CARD/RECORDINGS"
dismiss_cards
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "herestall: no take started in $STAGING"
else
  pass "herestall: take started on this computer in $(basename "$TAKE")"
  sleep 5

  touch "$DEAD"
  press_stop

  if wait_for_log "This computer's drive stopped answering while the take was being finished" 45; then
    pass "herestall: the take's finishing is said to have stopped on this computer's drive"
  else
    fail "herestall: no 'This computer's drive stopped answering while the take was being finished' within 45 s"
  fi

  # The stop's own line comes once the take is finished: everything said by
  # then is looked at.
  wait_for_log "Recording was stopped\|Recording stopped" 60 || fail "herestall: the stop was never logged"
  sleep 2
  if grep -q "The card stopped answering\|the card stopped answering\|the card stopped accepting" "$LOG"; then
    fail "herestall: the card was blamed for a take that never touched it:"
    grep "card stopped" "$LOG" | tail -3 | sed 's/^/        /'
  else
    pass "herestall: nothing blames the card"
  fi
  rm -f "$DEAD"
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app
fi

if want copyhere; then
# -------------------------------------------------------------------------
echo
echo "=== A take waiting for its own card, copied to the one in from under Record ==="
# The take's line says "To copy it to the card that's in now, choose Copy to
# this card." That button was only ever on the saved-take card of the take
# just stopped: closed, or for any earlier take, it was nowhere.
BEFORE_FAIL=$FAILED
FATMB="$WORK/copyhere.fatmb"
echo 7600 > "$FATMB"
launch copyhere fresh MMA_STALL_CARD_FAT_MB_FILE="$FATMB"
dismiss_cards
sleep 2   # the card seen, and remembered, at its save location

mv "$CARD_ROOT" "$CARD_ROOT.users"
sleep 2
press_record

if ! TAKE=$(wait_for_new_take "$STAGING"); then
  fail "copyhere: no take started with the card out"
else
  sleep 4
  press_stop
  wait_for_log "Saved on this computer" 20 || fail "copyhere: the take was not saved"
  dismiss_saved
  wait_for_log "The card isn't plugged in" 20 || fail "copyhere: the copy with the card out was not reported"

  echo 3800 > "$FATMB"
  mkdir -p "$CARD"

  if wait_for_log "choose Copy to this card" 30; then
    pass "copyhere: the take waits for its own card, and its line names Copy to this card"
  else
    fail "copyhere: the take's line does not name Copy to this card"
  fi

  # Under the record button, right of the line under it: the record button's
  # right edge, 28 px below its row.
  sleep 3
  click 1083 232
  if wait_for_log "Copying 1 take to the card that's in now" 10; then
    pass "copyhere: Copy to this card is under the record button, with the saved-take card closed"
  else
    fail "copyhere: no Copy to this card under the record button"
    command -v import >/dev/null && DISPLAY="$DISPLAY_NUM" import -window root "$WORK/copyhere.png" 2>/dev/null \
      && echo "        (screen: $WORK/copyhere.png)"
  fi

  if wait_for_log "Copied .* to the card" 40; then
    pass "copyhere: the take is copied to the card that is in"
    check_copy_matches copyhere "$TAKE"
  else
    fail "copyhere: the take was not copied to the card that is in within 40 s"
  fi
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app
fi

echo
if [ "$FAILED" = "0" ]; then
  echo "Card-copy gate passed."
else
  echo "Card-copy gate FAILED."
  exit 1
fi
