#!/bin/bash
# Prove an SDL client got the shim's fast path, and that it actually drew.
#
# FOUR CLAIMS, EACH FROM EVIDENCE THE SHIM PRINTS UNCONDITIONALLY:
#
#   1. zero-copy      "xshim: ZEROCOPY window ..."   (a window, not a cursor)
#   2. hardware CLUT  "xshim: CLUT ... HARDWARE (PPA)"
#   3. it painted     several screenshots taken DURING the run
#   4. it was fast    prboom's fps, cross-checked against /proc/uptime
#
# Every one of those was previously answered by inference, and on 2026-09-06
# that produced four wrong conclusions in a row: a busy board read as dead, a
# `tail -2` that hid the very line being looked for, a trace mode that killed
# the client it was tracing, and an fps measured across an ntpd clock step.
#
# TIMEOUTS ARE SIZED TO THE WORK, NOT PADDED. Boot to lvdesk is ~40 s. Doom
# starts in seconds. A 5026-gametic timedemo at ~27 fps is ~190 s. Nothing here
# waits ten minutes for anything; if a step overruns its budget it FAILS and
# says what it last saw.
#
#   verify-sdl.sh <w> <h> [--timedemo]
set -u
cd "$(dirname "$0")/../.."

W=${1:-320}; H=${2:-200}; MODE=${3:-}
D=${CLAUDE_JOB_DIR:-/tmp}/tmp; mkdir -p "$D"
BOOT_BUDGET=75          # lvdesk is up ~40 s after reset
RUN_BUDGET=300          # the direct run measures 187 s; this is ample
say() { printf '%s\n' "$*"; }
R() { python3 scripts/board/runsh.py "$1" "${2:-30}" 2>&1 | grep '^ZZ '; }

cat > "$D/vs_ping.sh" <<'SH'
echo "ZZ ALIVE up=$(cut -d. -f1 /proc/uptime)s"
SH

# --- clean board -----------------------------------------------------------
# The predicate is runsh - "the board EXECUTED a command" - not alive.py's
# "something printed". When those two disagreed, the probe was wrong every
# time. Bounded by wall clock, and it prints every attempt so it is never
# silently stuck.
# Arm selection: XSHIM_PPA_MIN_PX in the environment is written to the board
# BEFORE the reset, so the arm is fixed for the whole boot and provable from
# the log afterwards.
if [ -n "${XSHIM_PPA_MIN_PX:-}" ]; then
	cat > "$D/vs_env.sh" <<SH
cat > /etc/lvdesk.env <<'EOF'
export XSHIM_PPA_MIN_PX=$XSHIM_PPA_MIN_PX
EOF
sync
sed 's/^/ZZ env /' /etc/lvdesk.env
SH
	E=$(R "$D/vs_env.sh" 40)
	echo "$E" | grep -q "XSHIM_PPA_MIN_PX=$XSHIM_PPA_MIN_PX" || {
		say "FAIL: env not applied"; exit 1; }
	say "arm: XSHIM_PPA_MIN_PX=$XSHIM_PPA_MIN_PX (confirmed on the board)"
else
	R "$D/vs_env.sh" 40 >/dev/null 2>&1 || true
fi
python3 scripts/board/reset.py >/dev/null 2>&1
DEADLINE=$(( $(date +%s) + BOOT_BUDGET ))
until R "$D/vs_ping.sh" 25 | grep -q "ZZ ALIVE"; do
	if [ "$(date +%s)" -ge "$DEADLINE" ]; then
		say "FAIL: no command executed within ${BOOT_BUDGET}s"
		say "      probe says: $(python3 scripts/board/alive.py 2>&1 | head -1)"
		exit 1
	fi
	say "  booting ($(( DEADLINE - $(date +%s) ))s left)"
	sleep 8
done
say "board: executing commands"

# --- the clock must be steady before any stopwatch starts ------------------
if [ -r "$D/clocksettle.sh" ]; then
	C=$(R "$D/clocksettle.sh" 120)
	echo "$C" | grep -q "clock settled" || { say "FAIL: clock never settled"; exit 1; }
	say "clock: settled"
fi

# --- launch ----------------------------------------------------------------
TD=""; [ "$MODE" = "--timedemo" ] && TD="-timedemo demo1"
cat > "$D/vs_fire.sh" <<SH
export DISPLAY=:0
export LD_LIBRARY_PATH=/root/doom/lib
export DOOMWADDIR=/root/doom/wads
for p in \$(ps | awk '/prboom/ && !/awk/ {print \$1}'); do kill -9 \$p 2>/dev/null; done
sleep 1
rm -f /root/doom/vs.log
cd /root/doom/wads
setsid /root/doom/prboom -width $W -height $H -nosound $TD >/root/doom/vs.log 2>&1 </dev/null &
echo "ZZ u0=\$(cut -d. -f1 /proc/uptime)"
SH
U0=$(R "$D/vs_fire.sh" 40 | sed -n 's/^ZZ u0=//p')
[ -n "$U0" ] || { say "FAIL: could not start the client"; exit 1; }
say "launched ${W}x${H}${TD:+ timedemo}"

cat > "$D/vs_probe.sh" <<'SH'
{ grep "ZEROCOPY window" /var/log/lvdesk.log
  grep "xshim: CLUT " /var/log/lvdesk.log
  echo "doom=$(ps | grep -c '[p]rboom')"
  grep -h "frames per second" /root/doom/vs.log 2>/dev/null
  echo "u1=$(cut -d. -f1 /proc/uptime)"
} > /tmp/vs 2>&1
sed 's/^/ZZ /' /tmp/vs
SH

# --- watch it run, screenshotting as it goes -------------------------------
SHOTS=0; GOOD=0; BLANK=0; ZC_SEEN=0; HW_SEEN=0; CPU_SEEN=0; LASTGOOD=""; FPSLINE=""; U1=""; GONE=0
RUN_END=$(( $(date +%s) + RUN_BUDGET ))
i=0
while [ "$(date +%s)" -lt "$RUN_END" ]; do
	i=$((i + 1))
	sleep 25
	S="$D/verify_${W}x${H}_$i.jpg"; rm -f "$S"
	python3 scripts/board/screenshot-hw.py "$S" >/dev/null 2>&1
	if [ -f "$S" ]; then
		SZ=$(wc -c < "$S" | tr -d ' ')
		SHOTS=$((SHOTS + 1))
		# CALIBRATED ON MEASURED FRAMES, not a round number:
		#   ~15.8 kB  bare desktop, no client
		#   ~19.2 kB  client window present but BLACK
		#   ~36.1 kB  timedemo gameplay (darker than the menus)
		#   ~47.4 kB  Doom title screen
		# 40000 was set from the title screen alone and aborted a run that
		# was rendering gameplay perfectly. 28000 separates black from
		# drawing with margin on both sides.
		if [ "$SZ" -ge 28000 ]; then
			GOOD=$((GOOD + 1)); BLANK=0
			say "  shot $i: ${SZ} B  PAINTING"
		else
			BLANK=$((BLANK + 1))
			say "  shot $i: ${SZ} B  blank ($BLANK in a row)"
			# BAIL. Timing a window that is drawing nothing produces
			# an fps number that means nothing, and waiting out a
			# five-minute timedemo to discover it is exactly the
			# waste this harness exists to prevent.
			if [ "$BLANK" -ge 2 ] && [ "$GOOD" = 0 ]; then
				say ""
				say "=== ABORT ${W}x${H}: window is BLACK after"
				say "    $((i * 25))s and $BLANK consecutive shots."
				say "    Not timing it. Last shim state:"
				R "$D/vs_probe.sh" 40 | sed 's/^ZZ /      /'
				exit 1
			fi
		fi
	else
		say "  shot $i: capture failed"
	fi
	OUT=$(R "$D/vs_probe.sh" 40)
	# STICKY EVIDENCE. A share is logged ONCE, early; a probe late in the
	# run will not see it again, and an empty probe (busy board, console
	# contention) must not be read as "no". Once seen, it stays seen -
	# treating a failed probe as a negative is what produced a bogus
	# "zero-copy: NO" on a run that had four shares in its own log.
	echo "$OUT" | grep -q "ZEROCOPY window" && ZC_SEEN=1
	echo "$OUT" | grep -q "HARDWARE (PPA)" && HW_SEEN=1
	echo "$OUT" | grep -q "CPU (software loop)" && CPU_SEEN=1
	[ -n "$OUT" ] && LASTGOOD="$OUT"
	F=$(echo "$OUT" | sed -n 's/^ZZ \(Timed .*\)$/\1/p'); [ -n "$F" ] && FPSLINE="$F"
	V=$(echo "$OUT" | sed -n 's/^ZZ u1=//p'); [ -n "$V" ] && U1="$V"
	echo "$OUT" | grep -q "ZZ doom=0" && GONE=1
	[ -n "$FPSLINE" ] && break
	[ "$GONE" = 1 ] && { say "  client exited"; break; }
	# An interactive run needs only enough shots to prove it paints.
	[ -z "$TD" ] && [ "$i" -ge 3 ] && break
done

FINAL=$(R "$D/vs_probe.sh" 40)
[ -n "$FINAL" ] && LASTGOOD="$FINAL"
echo "$FINAL" | grep -q "ZEROCOPY window" && ZC_SEEN=1
echo "$FINAL" | grep -q "HARDWARE (PPA)" && HW_SEEN=1
echo "$FINAL" | grep -q "CPU (software loop)" && CPU_SEEN=1
ZC=$ZC_SEEN; HW=$HW_SEEN; CPU=$CPU_SEEN
PROBED=0; [ -n "$LASTGOOD" ] && PROBED=1

say ""
say "================= VERDICT ${W}x${H} ================="
echo "$LASTGOOD" | grep -E "ZEROCOPY window|xshim: CLUT " | sed 's/^ZZ /  /'
if [ "$PROBED" = 0 ]; then
	say "  zero-copy window share : UNKNOWN - the board never answered a probe"
	say "  hardware CLUT (PPA)    : UNKNOWN - the board never answered a probe"
else
	say "  zero-copy window share : $([ "$ZC" -gt 0 ] && echo YES || echo NO)"
	say "  hardware CLUT (PPA)    : $([ "$HW" -gt 0 ] && echo YES || echo "NO (CPU LUT: $([ "$CPU" -gt 0 ] && echo yes || echo unseen))")"
fi
say "  screenshots painting   : $GOOD of $SHOTS"
PASS=1
[ "$ZC" -gt 0 ] || PASS=0
[ "$HW" -gt 0 ] || PASS=0
[ "$GOOD" -ge 2 ] || PASS=0
if [ -n "$FPSLINE" ]; then
	say "  timedemo               : $FPSLINE"
	RT=$(echo "$FPSLINE" | sed -n 's/.*in \([0-9]*\) realtics.*/\1/p')
	FPS=$(echo "$FPSLINE" | sed -n 's/.*= \([0-9]*\)\..*/\1/p')
	if [ -n "$RT" ] && [ -n "$U1" ]; then
		TICK=$((RT / 35)); WALL=$((U1 - U0))
		say "  clock cross-check      : realtics=${TICK}s vs monotonic=${WALL}s"
		DIFF=$((TICK - WALL)); [ $DIFF -lt 0 ] && DIFF=$((-DIFF))
		if [ "$WALL" -gt 0 ] && [ $DIFF -gt $((WALL / 4 + 20)) ]; then
			say "  !! REJECTED: the clock moved during the run"
			PASS=0
		fi
	fi
	[ -n "$FPS" ] && [ "$FPS" -lt 26 ] && { say "  !! fps below the 26 threshold"; PASS=0; }
elif [ -n "$TD" ]; then
	say "  timedemo               : DID NOT FINISH in ${RUN_BUDGET}s"
	PASS=0
fi
say "  RESULT                 : $([ $PASS = 1 ] && echo PASS || echo FAIL)"
exit $((1 - PASS))
