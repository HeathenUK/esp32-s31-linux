#!/bin/bash
# Doom's own timedemo, the project's headline number, with nothing in the way.
#
#   timedemo.sh <name> <repeats> [fullscreen|window] [prelaunch-script]
#
# WHY THIS EXISTS BESIDE verify-sdl.sh / ab.sh. Those prove four claims per run
# (zero-copy, LUT, painted, fast) by probing the board and taking screenshots
# WHILE the game runs. On 2026-09-21 their probes stopped being answered under
# the current console tooling: every run ended "the board never answered a
# probe", no fps, RESULT FAIL - and killing the harness left its script holding
# the login shell, which looked exactly like a dead board. (Every historical
# ab.sh run in artifacts/ab/ says RESULT FAIL too; it was only ever good for its
# fps line.) This script measures ONE thing and touches the board twice:
#
#   1. reset, wait for a shell, then start the run DETACHED on the board
#      (setsid, output to a file on the card) and LEAVE - runsh exits;
#   2. nothing at all for the length of the demo - no probe, no screenshot, no
#      poll: a harness that is not there cannot cost the game anything;
#   3. collect: prboom's own "Timed N gametics in M realtics = X fps" line,
#      kernel faults from dmesg, and whether the game is still running.
#
# The measurement rules, built in:
#   - fresh boot per run (performance decays run-over-run as memory fills);
#   - the board SETTLES first: the run starts when uptime reaches SETTLE_S
#     (default 75 s - Wi-Fi associates at ~41 s and rcS ends at ~32 s);
#   - sound ON, volume at the floor (DACL/DACR 110), restored (178) at collect;
#   - the spread is printed with the median; overlapping ranges = no difference.
#
# [prelaunch-script]: a file of shell commands run on the board after the
# settle and before the game - how an arm applies its knobs (sysfs affinity,
# RPS, a background load). It is part of the arm; keep it short and bounded.
#
# Budget per run: ~40 s boot + settle + ~190 s demo + collect = ~5.5 min.
set -u
cd "$(dirname "$0")/../.."
NAME=${1:?name}; REP=${2:?repeats}; MODE=${3:-fullscreen}; PRE=${4:-}
SETTLE_S=${SETTLE_S:-75}; DEMO_WAIT=${DEMO_WAIT:-215}
W=${TD_W:-320}; H=${TD_H:-200}
WINFLAG=""; [ "$MODE" = window ] && WINFLAG="-window"
OUT=artifacts/timedemo/$NAME-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
[ -n "$PRE" ] && { [ -r "$PRE" ] || { echo "no such prelaunch script: $PRE"; exit 2; }; cp "$PRE" "$OUT/prelaunch.sh"; }
echo "timedemo: $NAME x$REP ${W}x${H} $MODE settle=${SETTLE_S}s ${PRE:+prelaunch=$PRE} -> $OUT"

for ((r = 1; r <= REP; r++)); do
	python3 scripts/board/reset.py >/dev/null 2>&1
	{
		echo 'uname -v; cat /sys/devices/system/cpu/online 2>/dev/null'
		echo 'for p in $(ps | awk "/prboom/ && !/awk/ {print \$1}"); do kill -9 $p 2>/dev/null; done'
		echo 'rm -f /root/doom/td.log /root/doom/td.pre'
		echo "amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null"
		if [ -n "$PRE" ]; then
			echo "cat > /root/doom/td-pre.sh <<'TDPRE_EOF'"; cat "$PRE"; echo 'TDPRE_EOF'
		else
			echo ': > /root/doom/td-pre.sh'
		fi
		# Everything slow happens detached, on the board, with no console.
		echo "setsid sh -c 'while read u _ < /proc/uptime; [ \${u%.*} -lt $SETTLE_S ]; do sleep 1; done; sh /root/doom/td-pre.sh > /root/doom/td.pre 2>&1; cd /root/doom; DISPLAY=:0 exec /root/doom/prboom -width $W -height $H $WINFLAG -timedemo demo1 > /root/doom/td.log 2>&1' </dev/null >/dev/null 2>&1 &"
		echo 'echo TD_LAUNCHED'
	} > "$OUT/launch-$r.sh"
	python3 scripts/board/runsh.py "$OUT/launch-$r.sh" 25 90 > "$OUT/launch-$r.log" 2>&1
	if ! grep -q "^TD_LAUNCHED" "$OUT/launch-$r.log"; then
		# A boot that never gave a shell must leave EVIDENCE, not just "NA":
		# 2026-09-21, one reset in ~20 on #301 produced "not one byte in 25s"
		# and nothing was recorded. alive.py --reset watches the whole boot at
		# both bauds (the loader's black box speaks at 115200) and keeps every
		# byte. Then one retry, on that fresh boot.
		echo "    run $r: no shell after reset - recording a boot: $OUT/noshell-$r.*"
		python3 scripts/board/alive.py --reset --timeout 60 --log "$OUT/noshell-$r.raw" > "$OUT/noshell-$r.txt" 2>&1
		python3 scripts/board/runsh.py "$OUT/launch-$r.sh" 25 30 > "$OUT/launch-$r.log" 2>&1
		if ! grep -q "^TD_LAUNCHED" "$OUT/launch-$r.log"; then
			echo "    run $r: could not launch even after a recorded reboot"; echo "$r|NA|NOLAUNCH" >> "$OUT/results.psv"; continue
		fi
	fi
	kver=$(grep -m1 -o "#[0-9]* [A-Z].*" "$OUT/launch-$r.log" | cut -c1-40)
	# Hands off. The settle and the whole demo happen with nobody watching.
	sleep $((SETTLE_S + DEMO_WAIT - 40))
	cat > "$OUT/collect.sh" <<'E'
# Bounded by the CLOCK, not an iteration count: on a loaded board one ps|grep
# turn took >3.4 s, 40 turns outlived runsh's window and the run reported
# nothing at all (2026-09-21). A slow arm needs a larger DEMO_WAIT instead.
read u _ < /proc/uptime; end=$((${u%.*} + 100))
while ps | grep -q "[p]rboom"; do read u _ < /proc/uptime; [ ${u%.*} -ge $end ] && break; sleep 3; done
echo "TD_FPS $(grep -a 'frames per second' /root/doom/td.log | tail -1)"
echo "TD_STILL_RUNNING $(ps | grep -c '[p]rboom')"
echo "TD_FAULTS $(dmesg | grep -aicE 'oops|unhandled signal|rcu:.*(stall|starved)|vblank wait timed out|Out of memory|BUG:')"
echo "TD_PRE $(head -c 300 /root/doom/td.pre 2>/dev/null | tr '\n' ' ')"
echo "TD_PINNED $(dmesg | grep -ac 'pinned to CPU0')"
amixer -q sset 'DACL' 178 2>/dev/null; amixer -q sset 'DACR' 178 2>/dev/null
echo TD_COLLECTED
E
	python3 scripts/board/runsh.py "$OUT/collect.sh" 140 10 > "$OUT/collect-$r.log" 2>&1
	fps=$(sed -n 's/^TD_FPS .*= \([0-9.]*\) frames per second.*/\1/p' "$OUT/collect-$r.log" | tail -1)
	faults=$(sed -n 's/^TD_FAULTS \([0-9]*\).*/\1/p' "$OUT/collect-$r.log" | tail -1)
	still=$(sed -n 's/^TD_STILL_RUNNING \([0-9]*\).*/\1/p' "$OUT/collect-$r.log" | tail -1)
	echo "$r|${fps:-NA}|faults=${faults:-?} still_running=${still:-?}|$kver" >> "$OUT/results.psv"
	echo "    run $r: ${fps:-NA} fps  faults=${faults:-?} still_running=${still:-?}  [$kver]"
done

echo; echo "=== $NAME: ${W}x${H} $MODE, fresh boot per run ==="
python3 - "$OUT/results.psv" <<'PY'
import statistics, sys
rows=[l.rstrip('\n').split('|') for l in open(sys.argv[1])]
good=[float(f) for _,f,*_ in rows if f not in ('NA','')]
bad=len(rows)-len(good)
if good:
    print(f"  median {statistics.median(good):.2f} fps  range {min(good):.2f}-{max(good):.2f}  n={len(good)}" + (f"  ({bad} without a result)" if bad else ""))
else:
    print(f"  no results ({bad} runs)")
PY
echo "logs: $OUT"
