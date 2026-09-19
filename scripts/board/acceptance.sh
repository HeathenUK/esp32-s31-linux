#!/bin/bash
# The full acceptance run. Slower than gate.py and meant for after a flash of
# anything that touches the display, the shim or the kernel - ~8 minutes.
#
#   scripts/board/acceptance.sh            reset, gate, handover, Doom timedemos
#   ACCEPT_NO_RESET=1 acceptance.sh        skip the reset (board already fresh)
#
# Stages, each reusing an existing tool:
#   1. reset + board-ok.sh          the board boots to a painted desktop
#   2. gate.py                      contract, smoke, sdlbench canary vs baseline
#   3. handover, both ways, twice   S40lvdesk stop -> fbcon owns the panel
#                                   (driver logs "console client restored",
#                                   the panel shows console text); start ->
#                                   the desktop is back and captured the VT.
#                                   The permanent-scanout work MUST keep this
#                                   working (user requirement, 2026-09-19).
#   4. verify-sdl.sh timedemo       real Doom fps, fullscreen then windowed,
#                                   each with its own fresh boot and the 26 fps
#                                   floor verify-sdl already enforces.
#
# FAIL CLOSED: every stage's verdict is parsed from what it printed; a stage
# that prints nothing is a failure, never a pass.
set -u
cd "$(dirname "$0")/../.."
OUT=artifacts/acceptance/$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
say() { printf '%s\n' "$*" | tee -a "$OUT/summary.txt"; }
BAD=0
T0=$(date +%s)

if [ "${ACCEPT_NO_RESET:-0}" != 1 ]; then
	python3 scripts/board/reset.py >/dev/null 2>&1
	R=$(bash scripts/board/board-ok.sh 120 2>&1 | tail -1)
	say "boot      : $R"
	case "$R" in "BOARD OK"*) ;; *) BAD=$((BAD + 1)) ;; esac
fi

python3 scripts/board/gate.py > "$OUT/gate.log" 2>&1
G=$(grep -E "^GATE " "$OUT/gate.log" | tail -1)
say "gate      : ${G:-NO VERDICT}"
case "$G" in "GATE PASS"*) ;; *) BAD=$((BAD + 1)) ;; esac

# --- handover ----------------------------------------------------------------
cat > "$OUT/handover.sh" <<'SH'
M0=$(dmesg | grep -c "console client restored")
for round in 1 2; do
	/etc/init.d/S40lvdesk stop >/dev/null 2>&1
	n=0; while [ $n -lt 15 ] && [ -n "$(pidof lvdesk)" ]; do sleep 1; n=$((n+1)); done
	sleep 2
	M1=$(dmesg | grep -c "console client restored")
	# fbcon owns the panel again: the driver re-created its client
	echo "ZZ round $round stop: lvdesk=$(pidof lvdesk) restored=$((M1 - M0)) fbcon_bound=$(cat /sys/class/vtconsole/vtcon1/bind 2>/dev/null)"
	# write something the console will paint, so a screenshot has content
	printf '\n\nHANDOVER TEST round %s\n' "$round" > /dev/tty0 2>/dev/null
	sleep 1
	/etc/init.d/S40lvdesk start >/dev/null 2>&1
	n=0; while [ $n -lt 20 ] && ! grep -q "console keyboard off" /var/log/lvdesk.log 2>/dev/null; do sleep 1; n=$((n+1)); done
	sleep 3
	echo "ZZ round $round start: lvdesk=$(pidof lvdesk) vt=$(grep -c 'console keyboard off' /var/log/lvdesk.log) after=${n}s"
	M0=$(dmesg | grep -c "console client restored")
done
echo "ZZ HANDOVER_END"
SH
H=$(python3 scripts/board/runsh.py "$OUT/handover.sh" 120 2>&1 | grep '^ZZ ')
printf '%s\n' "$H" > "$OUT/handover.log"
if echo "$H" | grep -q HANDOVER_END &&
   [ "$(echo "$H" | grep -c 'stop: lvdesk= restored=[1-9]')" = 2 ] &&
   [ "$(echo "$H" | grep -cE 'start: lvdesk=[0-9]+ vt=[1-9]')" = 2 ]; then
	say "handover  : PASS (fbcon<->lvdesk, twice each way)"
else
	say "handover  : FAIL"; printf '%s\n' "$H" | sed 's/^/            /' | tee -a "$OUT/summary.txt"
	BAD=$((BAD + 1))
fi
python3 scripts/board/screenshot-hw.py "$OUT/after-handover.jpg" >/dev/null 2>&1

# --- real-world: Doom timedemo, each on a fresh boot --------------------------
for mode in fullscreen window; do
	VS_MODE=$mode bash scripts/board/verify-sdl.sh 320 200 --timedemo > "$OUT/doom-$mode.log" 2>&1
	F=$(sed -n 's/.*= \([0-9.]*\) frames per second.*/\1/p' "$OUT/doom-$mode.log" | tail -1)
	V=$(grep -o "RESULT *: [A-Z]*" "$OUT/doom-$mode.log" | tail -1)
	say "doom $mode: ${F:-NA} fps  ${V:-NO VERDICT}"
	case "$V" in *PASS) ;; *) BAD=$((BAD + 1)) ;; esac
done

say "ACCEPTANCE $([ $BAD = 0 ] && echo PASS || echo "FAIL ($BAD stage(s))") in $(( $(date +%s) - T0 ))s  ($OUT)"
exit $((BAD > 0))
