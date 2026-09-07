#!/bin/bash
# Deliberately try to wedge the board, and prove it survives every time.
#
# WHY THIS EXISTS. On 2026-09-07 roughly six separate events were reported as
# "the board wedged". Most were not: two were scripts that outran their runsh
# window and left the login shell occupied, one was a probe run against a board
# that a background job already owned. Every one of them presents as NO_SHELL,
# which is exactly what a dead board looks like - so each cost a reset, a
# re-diagnosis, and in one case an hour chasing a hardware fault that was not
# there.
#
# The fixes are a port flock (console.open_port) and a board-side watchdog in
# runsh. This file is the proof they work, and the regression test for the next
# time someone edits either. It found THREE bugs in the hardening itself on its
# first run - a watchdog that fired after runsh had stopped listening, a race
# that swallowed the marker, and a false positive on every normal run - none of
# which were visible by reading the code.
#
#   scripts/board/wedge-test.sh            run everything
#
# Exit 0 only if every case passed AND the board is healthy at the end.
set -u
cd "$(dirname "$0")/../.."
D=${CLAUDE_JOB_DIR:-/tmp}/tmp; mkdir -p "$D"
PASS=0; FAIL=0
ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
chk()  { if eval "$2" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

# Board must execute a command. This is the ONLY definition of alive used here:
# not "something printed", which a booting board also does.
alive() { python3 scripts/board/runsh.py "$D/wt_ping.sh" 30 2>&1 | grep -q WT_ALIVE; }

cat > "$D/wt_ping.sh" <<'SH'
echo WT_ALIVE
SH

echo "=== precondition: board answers ==="
if alive; then ok "board alive before testing"; else
	bad "board not alive before testing - nothing below is meaningful"; exit 1; fi

echo
echo "=== 1. script far outlives its window (the 'find /' and big-copy case) ==="
cat > "$D/wt_slow.sh" <<'SH'
echo WT_STARTED
sleep 300
echo WT_SHOULD_NOT_APPEAR
SH
OUT=$(python3 scripts/board/runsh.py "$D/wt_slow.sh" 20 2>&1)
echo "$OUT" | grep -q 'BOARD KILLED THIS SCRIPT' && ok "overrun is announced, not silent" \
	|| bad "overrun was silent - the caller cannot tell this from a dead board"
echo "$OUT" | grep -q 'WT_SHOULD_NOT_APPEAR' && bad "script kept running past the kill" \
	|| ok "script really was killed"
chk "board usable IMMEDIATELY after an overrun" alive

echo
echo "=== 2. a normal script must not be flagged (no false positives) ==="
OUT=$(python3 scripts/board/runsh.py "$D/wt_ping.sh" 30 2>&1)
echo "$OUT" | grep -q 'BOARD KILLED' && bad "false timekill on a normal run" \
	|| ok "quiet on a normal run"
echo "$OUT" | grep -q 'WT_ALIVE' && ok "normal output intact" || bad "output lost"

echo
echo "=== 3. two tools at once must report contention, never NO_SHELL ==="
python3 scripts/board/runsh.py "$D/wt_slow.sh" 30 >/dev/null 2>&1 &
BG=$!
sleep 5
OUT=$(python3 scripts/board/runsh.py "$D/wt_ping.sh" 15 2>&1)
echo "$OUT" | grep -q 'SERIAL PORT BUSY' && ok "contention named the holder" \
	|| bad "contention not reported"
echo "$OUT" | grep -q 'NOT a dead board' && ok "says it is not a dead board" \
	|| bad "did not say it is not a dead board"
echo "$OUT" | grep -q 'NO_SHELL' && bad "still reported NO_SHELL (the old lie)" \
	|| ok "did NOT report NO_SHELL"
wait $BG 2>/dev/null
chk "board usable after contention" alive

echo
echo "=== 4. a reset must NOT be able to land inside someone else's run ==="
python3 - <<'PY' > "$D/wt_reset.out" 2>&1
import subprocess, sys
sys.path.insert(0, 'scripts/board')
import console
console.take_port_lock(what='wedge-test: pretend measurement')
r = subprocess.run([sys.executable, 'scripts/board/withlock.py',
                    '--', 'echo', 'SHOULD_NOT_RUN'],
                   capture_output=True, text=True, timeout=60)
print('rc=%d' % r.returncode)
print(r.stdout + r.stderr)
PY
grep -q 'SHOULD_NOT_RUN' "$D/wt_reset.out" && bad "flash/reset ran during a measurement" \
	|| ok "flash/reset refused while the board was in use"
grep -q 'rc=3' "$D/wt_reset.out" && ok "refusal has a distinct exit status" \
	|| bad "no distinct exit status"

echo
echo "=== 5. console flood must not wedge anything ==="
cat > "$D/wt_flood.sh" <<'SH'
i=0
while [ $i -lt 400 ]; do
  echo "WT_FLOOD $i ------------------------------------------------------"
  i=$((i+1))
done
echo WT_FLOOD_DONE
SH
OUT=$(python3 scripts/board/runsh.py "$D/wt_flood.sh" 60 2>&1)
echo "$OUT" | grep -q 'WT_FLOOD_DONE' && ok "survived a 400-line console flood" \
	|| bad "flood lost its terminator"
chk "board usable after a flood" alive

echo
echo "=== 6. back-to-back calls (no leaked lock, no stale state) ==="
R=0
for i in 1 2 3 4 5; do alive || R=1; done
[ "$R" = 0 ] && ok "five sequential calls all answered" || bad "a sequential call failed"

echo
echo "=== 7. a killed tool must not leave the lock held ==="
python3 - <<'PY' >/dev/null 2>&1
import os, sys, signal, subprocess
sys.path.insert(0, 'scripts/board')
p = subprocess.Popen([sys.executable, '-c',
    "import sys; sys.path.insert(0,'scripts/board'); import console, time;"
    "console.take_port_lock(what='about to be killed'); time.sleep(30)"])
import time; time.sleep(3)
p.kill(); p.wait()
PY
chk "lock released after its holder was killed" alive

echo
echo "=== final: board health ==="
if ./scripts/board/board-ok.sh 110 >/dev/null 2>&1; then
	ok "board healthy with a painting screen"
else
	bad "board not healthy at the end"
fi

echo
echo "==================== RESULT: pass=$PASS fail=$FAIL ===================="
[ "$FAIL" = 0 ] || exit 1
