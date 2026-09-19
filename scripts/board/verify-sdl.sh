#!/bin/bash
# Prove an SDL client got the shim's fast path, and that it actually drew.
#
# FOUR CLAIMS, EACH FROM EVIDENCE THE SHIM PRINTS UNCONDITIONALLY:
#
#   1. zero-copy      "xshim: ZEROCOPY window ..."   (a window, not a cursor)
#   2. CPU LUT        proven BY CONSTRUCTION, not by a log line - see below
#   3. it painted     several screenshots taken DURING the run
#   4. it was fast    prboom's fps, cross-checked against /proc/uptime
#
# CLAIM 2 CHANGED ON 2026-09-07, AND THIS IS THE POINT OF THIS EDIT.
# This script used to REQUIRE "xshim: CLUT ... HARDWARE (PPA)" to pass. That
# string was deleted from xshim.c by the 2026-09-06 rollback, along with every
# other PPA CLUT line - `grep -i ppa lvdesk/xshim.c` now finds only comments.
# So the harness demanded evidence the agreed baseline CANNOT produce, and
# scored a correct board FAIL. A gate that cannot pass is not a gate.
#
# The baseline is zero-copy + a CPU LUT. The CPU LUT needs no log line because
# xshim_window_pixels() contains exactly one depth-8 path - `pal = pal8;` and a
# scalar loop - and no alternative for the binary to have taken. Construction
# is stronger evidence than a printf, so this now VERIFIES THE SOURCE instead
# of grepping for a string, and fails if a PPA path ever reappears unannounced.
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
#   env: VS_MODE=window|fullscreen (default fullscreen), VS_SOUND=1,
#        VS_ENV="VAR=val ..." (the arm, written to /etc/lvdesk.env)
set -u
cd "$(dirname "$0")/../.."

W=${1:-320}; H=${2:-200}; MODE=${3:-}
D=${CLAUDE_JOB_DIR:-/tmp}/tmp; mkdir -p "$D"
BOOT_BUDGET=75          # lvdesk is up ~40 s after reset
RUN_BUDGET=440          # a painting run is ~270-400 s at 19-21 fps; 360 cut one off mid-demo
say() { printf '%s\n' "$*"; }

# --- claim 2, checked before a single byte is flashed ----------------------
CLUT_PATH=unknown
if grep -q 'pal = pal8;' lvdesk/xshim.c; then
	if grep -inE '^[^*/]*\bppa_[a-z_]+\(' lvdesk/xshim.c | grep -q .; then
		CLUT_PATH="MIXED - a PPA call is back in xshim.c"
	else
		CLUT_PATH="CPU (scalar pal8 loop, no PPA path in the source)"
	fi
fi
R() { python3 scripts/board/runsh.py "$1" "${2:-30}" 2>&1 | grep '^ZZ '; }

cat > "$D/vs_showenv.sh" <<'SH'
# READ ONLY. Never writes /etc/lvdesk.env - see the else branch below.
echo "ZZ env: $(tr '\n' ' ' < /etc/lvdesk.env 2>/dev/null || echo '(none)')"
SH

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
# VS_ENV generalises that: a space-separated "VAR=val VAR2=val" list written
# to /etc/lvdesk.env verbatim (as export lines), so ANY runtime switch can be
# an arm - LVDESK_DIRECTEXP, a module parameter echoed by lvdesk.env, or the
# stock arm as VS_ENV="" (the empty string, distinct from unset, EMPTIES the
# file so the stock arm is provable too). XSHIM_PPA_MIN_PX keeps working.
[ -n "${XSHIM_PPA_MIN_PX:-}" ] && VS_ENV="${VS_ENV:+$VS_ENV }XSHIM_PPA_MIN_PX=$XSHIM_PPA_MIN_PX"
if [ -n "${VS_ENV+set}" ]; then
	{
		echo ": > /etc/lvdesk.env"
		for kv in $VS_ENV; do echo "echo 'export $kv' >> /etc/lvdesk.env"; done
		echo "sync"
		echo "echo \"ZZ env \$(tr '\\n' ' ' < /etc/lvdesk.env)\""
	} > "$D/vs_env.sh"
	E=$(R "$D/vs_env.sh" 40)
	for kv in $VS_ENV; do
		echo "$E" | grep -q "export $kv" || { say "FAIL: env not applied ($kv)"; exit 1; }
	done
	say "arm: ${VS_ENV:-(stock, lvdesk.env emptied)} (confirmed on the board)"
else
	# DO NOT touch /etc/lvdesk.env here.
	#
	# This used to re-run $D/vs_env.sh - a leftover script from a PREVIOUS
	# invocation whose entire job is to overwrite that file. So a run with
	# no XSHIM_PPA_MIN_PX silently clobbered whatever arm the caller had
	# set up, and then measured something else. On 2026-09-07 that wiped
	# LVDESK_DIRECTEXP=1 and produced a 22.3 fps "direct expansion" result
	# that was really the shadow path - caught only because the EXPAND
	# counter showed 25 lines when the direct path emits none.
	#
	# An arm the harness did not set is an arm the harness must not
	# destroy. Report what is there instead.
	E=$(R "$D/vs_showenv.sh" 30)
	[ -n "$E" ] && say "arm: ${E#ZZ }"
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
# Stage the clock gate. It used to be tested for and never shipped, so the
# `if [ -r ... ]` below silently skipped it on every run - and a timedemo fired
# before NTP steps the clock reports 0.0 fps, which reads as a total
# regression rather than as a harness fault.
cp "$(dirname "$0")/clocksettle.sh" "$D/clocksettle.sh" 2>/dev/null || true
if [ -r "$D/clocksettle.sh" ]; then
	C=$(R "$D/clocksettle.sh" 120)
	echo "$C" | grep -q "clock settled" || { say "FAIL: clock never settled"; exit 1; }
	say "clock: settled"
fi

# --- launch ----------------------------------------------------------------
if [ "${VS_CPUSHARE:-0}" = 1 ]; then
	{ echo "cat > /root/cpushare.sh <<'CPUSHARE_EOF'"; cat "$(dirname "$0")/cpushare.sh"; echo "CPUSHARE_EOF"; echo 'rm -f /root/cpushare.txt; echo "ZZ cpushare shipped"'; } > "$D/vs_cps.sh"
	R "$D/vs_cps.sh" 40 | grep -q "cpushare shipped" || { say "FAIL: could not ship cpushare.sh"; exit 1; }
fi
TD=""; [ "$MODE" = "--timedemo" ] && TD="-timedemo demo1"
# prboom DEFAULTS TO FULLSCREEN (use_fullscreen=1 in the binary), and once the
# shim grew VidMode a "windowed" run without -window silently became a
# fullscreen one - a whole day of numbers were mislabelled that way. So the
# mode is always passed explicitly and printed. VS_MODE=window|fullscreen,
# default fullscreen (what every historical verify-sdl number actually was).
VS_MODE=${VS_MODE:-fullscreen}
case "$VS_MODE" in
	window) VS_WINFLAG="-window" ;;
	fullscreen) VS_WINFLAG="-fullscreen" ;;
	*) say "FAIL: VS_MODE must be window or fullscreen"; exit 1 ;;
esac
# VS_SOUND=1 runs with sound (the real-world case); default is -nosound, which
# is what the historical fps figures were taken with.
VS_SNDFLAG="-nosound"; [ "${VS_SOUND:-0}" = 1 ] && VS_SNDFLAG=""
cat > "$D/vs_fire.sh" <<SH
export DISPLAY=:0
export LD_LIBRARY_PATH=/root/doom/lib
export DOOMWADDIR=/root/doom/wads
for p in \$(ps | awk '/prboom/ && !/awk/ {print \$1}'); do kill -9 \$p 2>/dev/null; done
sleep 1
rm -f /root/doom/vs.log
cd /root/doom/wads
setsid /root/doom/prboom -width $W -height $H $VS_WINFLAG $VS_SNDFLAG $TD >/root/doom/vs.log 2>&1 </dev/null &
# VS_CPUSHARE=1: 30 s into the run, attribute 60 s of CPU per task
# (scripts/board/cpushare.sh, shipped below). Fork-free, so it does not
# disturb what it measures; the file is read back in the verdict.
if [ "${VS_SOUND:-0}" = 1 ] || [ "${VS_CPUSHARE:-0}" = 1 ]; then :; fi
[ "${VS_CPUSHARE:-0}" = 1 ] && [ -r /root/cpushare.sh ] && \
	setsid sh -c 'sleep 30; sh /root/cpushare.sh 60 /root/cpushare.txt' </dev/null >/dev/null 2>&1 &
echo "ZZ u0=\$(cut -d. -f1 /proc/uptime)"
SH
U0=$(R "$D/vs_fire.sh" 40 | sed -n 's/^ZZ u0=//p')
[ -n "$U0" ] || { say "FAIL: could not start the client"; exit 1; }
say "launched ${W}x${H} ${VS_MODE}${VS_SNDFLAG:+ nosound}${TD:+ timedemo}"

cat > "$D/vs_probe.sh" <<'SH'
{ grep "ZEROCOPY window" /var/log/lvdesk.log
  grep "xshim: CLUT " /var/log/lvdesk.log
  echo "doom=$(ps | grep -c '[p]rboom')"
  grep -h "frames per second" /root/doom/vs.log 2>/dev/null
  echo "u1=$(cut -d. -f1 /proc/uptime)"
} > /tmp/vs 2>&1
sed 's/^/ZZ /' /tmp/vs
SH

cat > "$D/vs_quiet.sh" <<'SH'
# Deliberately minimal: NO grep over the growing lvdesk log, NO screenshot.
# Every byte here lands on the machine being timed.
{ echo "doom=$(ps | grep -c '[p]rboom')"
  grep -h "frames per second" /root/doom/vs.log 2>/dev/null
  echo "u1=$(cut -d. -f1 /proc/uptime)"
} > /tmp/vq 2>&1
sed 's/^/ZZ /' /tmp/vq
SH

# --- watch it run, screenshotting as it goes -------------------------------
SHOTS_NEEDED=3          # enough to prove painting at three points in the demo
QUIET=0
SHOTS=0; GOOD=0; BLANK=0; ZC_SEEN=0; HW_SEEN=0; CPU_SEEN=0; LASTGOOD=""; FPSLINE=""; U1=""; GONE=0
RUN_END=$(( $(date +%s) + RUN_BUDGET ))
i=0
while [ "$(date +%s)" -lt "$RUN_END" ]; do
	i=$((i + 1))
	# Once painting is PROVEN, stop touching the board: long sleeps, no
	# screenshots, and the cheapest possible liveness poll.
	if [ "$GOOD" -ge "$SHOTS_NEEDED" ]; then
		[ "$QUIET" = 0 ] && { say "  --- $GOOD good shots: going QUIET so the fps is not ours ---"; QUIET=1; }
		sleep 45
	else
		sleep 25
	fi
	S="$D/verify_${W}x${H}_$i.jpg"; rm -f "$S"
	[ "$QUIET" = 0 ] && python3 scripts/board/screenshot-hw.py "$S" >/dev/null 2>&1
	if [ "$QUIET" = 0 ] && [ -f "$S" ]; then
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
	elif [ "$QUIET" = 0 ]; then
		say "  shot $i: capture failed"
	fi
	if [ "$QUIET" = 1 ]; then
		OUT=$(R "$D/vs_quiet.sh" 40)
	else
		OUT=$(R "$D/vs_probe.sh" 40)
	fi
	# STICKY EVIDENCE. A share is logged ONCE, early; a probe late in the
	# run will not see it again, and an empty probe (busy board, console
	# contention) must not be read as "no". Once seen, it stays seen -
	# treating a failed probe as a negative is what produced a bogus
	# "zero-copy: NO" on a run that had four shares in its own log.
	echo "$OUT" | grep -q "ZEROCOPY window" && ZC_SEEN=1
	echo "$OUT" | grep -q "HARDWARE (PPA)" && HW_SEEN=1
	echo "$OUT" | grep -q "CPU (software loop)" && CPU_SEEN=1
	[ -n "$OUT" ] && LASTGOOD="$OUT"
	F=$(echo "$OUT" | sed -n 's/^ZZ \(Timed .*\)$/\1/p' | tail -1); [ -n "$F" ] && FPSLINE="$F"
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
fi
say "  CLUT expansion path    : $CLUT_PATH"
[ "$HW" -gt 0 ] && say "  !! a HARDWARE (PPA) line appeared - the source and the board disagree"
say "  screenshots painting   : $GOOD of $SHOTS"
PASS=1
[ "$ZC" -gt 0 ] || PASS=0
case "$CLUT_PATH" in "CPU "*) ;; *) PASS=0 ;; esac
[ "$GOOD" -ge 2 ] || PASS=0
# FAIL CLOSED. Twice now this block has printed PASS for a run that missed
# the threshold, because an arithmetic error on an empty or multi-line value
# aborted the command that was supposed to set PASS=0 and execution simply
# carried on. A gate whose failure mode is "pass" is worse than no gate.
#
# So: every value is squeezed to ONE integer (or empty) before it is allowed
# near $(( )), and the fps threshold is evaluated with plain string-free
# integer tests whose outcome cannot depend on a parse succeeding.
num() { printf '%s' "$1" | tr -dc '0-9\n' | awk 'NF{print; exit}'; }

if [ -n "$FPSLINE" ]; then
	FPSLINE=$(printf '%s\n' "$FPSLINE" | tail -1)
	say "  timedemo               : $FPSLINE"
	RT=$(num "$(printf '%s' "$FPSLINE" | sed -n 's/.*in \([0-9]*\) realtics.*/\1/p')")
	FPS=$(num "$(printf '%s' "$FPSLINE" | sed -n 's/.*= \([0-9]*\)\..*/\1/p')")
	A=$(num "$U0"); B=$(num "$U1")
	if [ -n "$RT" ] && [ -n "$A" ] && [ -n "$B" ] && [ "$B" -gt "$A" ]; then
		TICK=$(( RT / 35 ))
		WALL=$(( B - A ))
		say "  clock cross-check      : realtics=${TICK}s vs monotonic=${WALL}s"
		DIFF=$(( TICK - WALL )); [ "$DIFF" -lt 0 ] && DIFF=$(( -DIFF ))
		if [ "$DIFF" -gt $(( WALL / 4 + 20 )) ]; then
			say "  !! REJECTED: the clock moved during the run"
			PASS=0
		fi
	else
		say "  clock cross-check      : SKIPPED (u0=${U0:-?} u1=${U1:-?})"
	fi
	if [ -z "$FPS" ]; then
		say "  !! no fps could be parsed from that line - FAILING"
		PASS=0
	elif [ "$FPS" -lt 26 ]; then
		say "  !! ${FPS} fps is BELOW the 26 threshold - REGRESSION"
		PASS=0
	else
		say "  fps gate               : ${FPS} >= 26 OK"
	fi
elif [ -n "$TD" ]; then
	say "  timedemo               : DID NOT FINISH in ${RUN_BUDGET}s"
	PASS=0
fi
if [ "${VS_CPUSHARE:-0}" = 1 ]; then
	cat > "$D/vs_cpsget.sh" <<'SH'
[ -r /root/cpushare.txt ] && sed 's/^/ZZ cps /' /root/cpushare.txt | head -25 || echo "ZZ cps MISSING"
SH
	say "  cpu share (60 s mid-run, ticks; game vs everything else):"
	R "$D/vs_cpsget.sh" 40 | sed 's/^ZZ cps /      /' | tee -a "$D/cpushare-${W}x${H}.txt"
fi
say "  RESULT                 : $([ $PASS = 1 ] && echo PASS || echo FAIL)"
exit $((1 - PASS))
