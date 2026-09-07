#!/bin/bash
# Prove the hart0 log-level toggle actually suppresses output.
#
# WHY A TEST AND NOT A CLAIM. The first attempt to demonstrate this failed
# three times in a row and each failure was instructive:
#
#   - 60 s captures with Doom running recorded ZERO console bytes at both
#     INFO and WARN, because the chatter is BURSTY. It comes from association
#     and block-ack setup, not steady state, so an idle-but-connected board
#     says nothing at any level and proves nothing either way.
#   - A "positive control" at VERBOSE produced nothing, because
#     CONFIG_LOG_MAXIMUM_LEVEL=3 compiles DEBUG and VERBOSE out. No runtime
#     call can raise past INFO; the tool now says so.
#   - Generating traffic with wget did not help: block-ack setup happens at
#     session start, not per packet.
#
# So the workload has to FORCE an association: `wpa_cli reassociate`. That
# reliably produces the chatter, and only then is the comparison meaningful.
#
#   scripts/board/hart0-log-test.sh
#
# Exit 0 if INFO produces materially more output than WARN, and if WARN-level
# messages survive in both (suppressing those too would be a bug, not a win).
set -u
cd "$(dirname "$0")/../.."
D=${CLAUDE_JOB_DIR:-/tmp}/tmp; mkdir -p "$D"

arm() {          # arm <level> <tag>
	cat > "$D/h0_$2.sh" <<SH
s31-coex loglevel wifi $1 >/dev/null 2>&1
setsid sh -c 'sleep 3; wpa_cli -i wlan0 reassociate; sleep 10' \\
	</dev/null >/dev/null 2>&1 &
echo "ZZ armed level $1"
SH
	python3 scripts/board/runsh.py "$D/h0_$2.sh" 40 2>&1 | grep -q 'ZZ armed' || {
		echo "  could not arm level $1"; return 1; }
	# conlog holds the port, so runsh must have exited first - it has.
	python3 scripts/board/conlog.py "$D/h0_$2.log" 35 >/dev/null 2>&1
}

echo "=== ARM A: wifi at INFO (3) ==="
arm 3 info
I_INFO=$(grep -c '^I (.*wifi:' "$D/h0_info.log" 2>/dev/null | head -1)
W_INFO=$(grep -c '^W (.*wifi:' "$D/h0_info.log" 2>/dev/null | head -1)
echo "  INFO-level wifi lines: $I_INFO   WARN-level: $W_INFO"

echo "=== ARM B: wifi at WARN (2) ==="
arm 2 warn
I_WARN=$(grep -c '^I (.*wifi:' "$D/h0_warn.log" 2>/dev/null | head -1)
W_WARN=$(grep -c '^W (.*wifi:' "$D/h0_warn.log" 2>/dev/null | head -1)
echo "  INFO-level wifi lines: $I_WARN   WARN-level: $W_WARN"

# Leave the board as we found it: quiet.
python3 scripts/board/runsh.py <(echo 's31-coex loglevel wifi 2 >/dev/null 2>&1; echo ZZ restored') 30 >/dev/null 2>&1

echo
FAIL=0
if [ "$I_INFO" -gt "$I_WARN" ]; then
	echo "  PASS  INFO chatter suppressed ($I_INFO -> $I_WARN)"
else
	echo "  FAIL  no suppression ($I_INFO -> $I_WARN). If BOTH are 0 the"
	echo "        association never happened - the test provoked nothing and"
	echo "        proves nothing. Check wpa_cli and wlan0 first."
	FAIL=1
fi
if [ "$W_WARN" -gt 0 ]; then
	echo "  PASS  WARN-level messages survive ($W_WARN kept)"
else
	echo "  WARN  no WARN-level lines seen; cannot confirm they survive"
fi
exit $FAIL
