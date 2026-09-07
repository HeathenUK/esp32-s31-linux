#!/bin/bash
# One line of truth about the board. Run it after EVERY flash or reset.
#
# WHY THIS EXISTS
# ===============
# esptool resets the board when it finishes writing, and boot to lvdesk takes
# ~40 s. Probing inside that window gets no answer, and "no answer" was being
# reported as a dead board - repeatedly, on 2026-09-06, to a user who was
# looking at a screen that was black for the same reason: it is black between
# reset and lvdesk starting. Neither observation meant anything was wrong.
#
# So: WAIT for the board to execute a command, THEN look at the screen, THEN
# say one line. Never report board state without having done all three.
#
#   board-ok.sh [budget_seconds]     default 75
#
# Exit 0 = executing commands AND the screen has content.
# Exit 1 = executing, but the screen looks blank.
# Exit 2 = never executed a command inside the budget.
set -u
cd "$(dirname "$0")/../.."
BUDGET=${1:-75}
D=${CLAUDE_JOB_DIR:-/tmp}/tmp; mkdir -p "$D"

cat > "$D/bok_ping.sh" <<'SH'
echo "ZZ up=$(cut -d. -f1 /proc/uptime) lvdesk=$(ps | grep -c '[l]vdesk') mem=$(awk '/MemAvailable/{print $2}' /proc/meminfo)"
SH

DEADLINE=$(( $(date +%s) + BUDGET ))
INFO=""
while :; do
	INFO=$(python3 scripts/board/runsh.py "$D/bok_ping.sh" 25 2>&1 | grep '^ZZ ')
	[ -n "$INFO" ] && break
	if [ "$(date +%s)" -ge "$DEADLINE" ]; then
		echo "BOARD DEAD: no command executed in ${BUDGET}s (this is NOT the boot window - that is ~40s)"
		exit 2
	fi
	sleep 8
done

S="$D/bok.jpg"; rm -f "$S"
python3 scripts/board/screenshot-hw.py "$S" >/dev/null 2>&1
if [ -f "$S" ]; then
	SZ=$(wc -c < "$S" | tr -d ' ')
else
	SZ=0
fi
# A bare desktop is ~15 kB, a desktop with a painting client ~47-105 kB, and a
# black screen ~19 kB with a window on it. Below 10 kB there is nothing at all.
if [ "$SZ" -lt 10000 ]; then
	echo "BOARD UP but SCREEN EMPTY (${SZ} B) | ${INFO#ZZ }"
	exit 1
fi
echo "BOARD OK (screen ${SZ} B) | ${INFO#ZZ }"
exit 0
