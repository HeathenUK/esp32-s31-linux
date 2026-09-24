#!/bin/bash
# churn.sh <label> [--current] [--no-demo] - WHERE does the kernel's time go
# under the X11 TyrQuake timedemo, in numbers per second and in correctly
# resolved sampler categories? No build, one ~5 min result.
#
#   scripts/board/churn.sh base            # fresh boot (reset.py), then the window
#   scripts/board/churn.sh armA --current  # no reset: the board as it is now
#                                          # (same-boot A/B/A: run it three times)
#   scripts/board/churn.sh idle --no-demo  # the idle desktop, no Quake
#
# It runs on the HOST and ships ONE board script through runsh.py. Board side,
# in order: uname + the sil_stuck counter + the live sysctl_sched_features
# word; $PRE; launch exactly the harness invocation of the 10.9-12.6 fps series
# (id1 -mem 10 -sndspeed 11025 fullscreen +timedemo demo1; -sndspeed is the
# game's own option, so it is the rate the game ASKS for); one 15 s sleep;
# snapshot A to /tmp (tmpfs: /proc/interrupts, /proc/stat, /proc/softirqs,
# grep'd /proc/timer_list, vmstat, SD stat, the game's status); 8 back-to-back
# h1s captures of 4000 samples = 32 s (the sampler is capped at 4000 =
# 4 s per capture: rootfs/h1s.c, bootloader/main/s31_vcpu.c "H1 PC SAMPLER");
# snapshot B + the game's maps; pin the game to CPU0 and take 2 more captures
# (reported SEPARATELY - pinning changes the workload); then one long sleep and
# a slow poll for the demo's RESULT line. 8 forks in the 32 s window, no
# polling: the fork storm of the old harness cost 17% of fps
# (quake-timedemo.sh:52-58).
#
# Host side: churn-cat.py rates = per-source IRQ/s per CPU (IPI rows included:
# 'Function call interrupts' is the TTWU_QUEUE wakelist IPI, 'Rescheduling'
# the resched IPI, arch/riscv/kernel/smp.c:43-47), ctxt/s, softirq/s per
# type, hrtimer nr_events/s per CPU, SD reads/s, swap-ins/s; then
# h1s-report.py for the standard view with user symbols, and churn-cat.py cat
# for the categorised kernel shares (idle/irq-entry/softirq/timers/scheduler/
# locks/memory/storage/syscall-fs/drivers/M-mode/other, RAM vs flash, top 25).
#
# THREE SELF-CHECKS ABORT THE REPORT (they are the point of this script):
#   1. the board's `uname -a` #N must equal the build stamp inside
#      images/xipImage - otherwise images/System.map is for another kernel;
#   2. the most-hit kernel PC must resolve inside arch_cpu_idle (the parked
#      WFI). The 2026-09-24 X11 profile's top PC 0xc032b1b4 x106 resolves to
#      show_pwq against #369's map (arch_cpu_idle is 0x888 higher), so every
#      symbol in that report is a neighbour of the real function. MAP MISMATCH
#      here means: rates are printed, symbols are refused;
#   3. esp32s31_sil_stuck must be 0 (a stuck-interrupt-level count is a
#      board whose IRQ accounting cannot be trusted).
# Plus: every capture must return 4000 samples, and the RESULT fps is printed
# so the window can be checked against the 10.9-12.6 band.
#
# WHAT IT NEVER DOES: no build, no flash, no input injection, no write to the
# card during the window (snapshots in tmpfs; the prof dumps go to /root only
# as each capture ends, one write each), no second board tool while its runsh
# window is open (port flock + board-side watchdog at timeout-3 s), no
# `echo 0 > cpu1/online` (the hart0 monitor refuses the HSM stop and the CPU
# BUG()s - use `make linux SMP_ONE=1` for that question).
#
# Judge on RATES and CATEGORY SHARES. fps is a +-10% per-boot lottery; rates
# are per-boot stable. Same-boot A/B/A must report the A-to-A gap as its
# error bar (dead-end #19); category shares need a second boot before being
# believed to better than +-3 points.
#
# Knobs (environment):
#   PRE="board cmd"      run on the board before Quake starts (a devmem write,
#                        a module param); its exit status is printed
#   QUAKE_BIN=./tyr-quake-x11   QUAKE_ARGS="-mem 10 -sndspeed 11025"
#   CAPS=8 PINNED=2 SETTLE=15   captures (4 s each) / pinned captures / settle s
#   H1S_CTRL=0x2f031968 H1S_BUF=0x2f026630   sampler addresses; they move
#                        with every loader build: check
#                        `nm hello_world.elf | grep s31_h1s_` after a loader flash
#   SYMDIR=images        <basename>.nm tables for user symbols; EXTRA_NM_DIR
#                        (default artifacts/perf-plan/x11-quake-profile-20260924)
#                        adds tyr-quake-x11.nm / libX11.so.6.nm
#   TIMEOUT=300          runsh window (hard; the board kills the script at
#                        TIMEOUT-3 s and prints RS_TIMEKILL)
# Output: artifacts/perf-plan/churn-<label>-<HHMMSS>/ with run.log, snapA/,
# snapB/, prof.txt, prof-pinned.txt, maps.txt, rates.txt, report.txt,
# cat.txt, cat-pinned.txt and summary.txt.
set -u
cd "$(dirname "$0")/../.."
HERE=$(cd "$(dirname "$0")" && pwd)
CAT=$HERE/churn-cat.py
[ -x "$CAT" ] || CAT=scripts/board/churn-cat.py

L=${1:?usage: churn.sh <label> [--current] [--no-demo]}; shift
CURRENT=0; DEMO=1
for a in "$@"; do
	case "$a" in
	--current) CURRENT=1 ;;
	--no-demo) DEMO=0 ;;
	*) echo "churn.sh: unknown option $a" >&2; exit 2 ;;
	esac
done
PRE=${PRE:-:}
QUAKE_BIN=${QUAKE_BIN:-./tyr-quake-x11}
QUAKE_ARGS=${QUAKE_ARGS:--mem 10 -sndspeed 11025}
CAPS=${CAPS:-8}; PINNED=${PINNED:-2}; SETTLE=${SETTLE:-15}
CTRL=${H1S_CTRL:-0x2f031968}; BUF=${H1S_BUF:-0x2f026630}
SYMDIR=${SYMDIR:-images}
EXTRA_NM_DIR=${EXTRA_NM_DIR:-artifacts/perf-plan/x11-quake-profile-20260924}
TIMEOUT=${TIMEOUT:-300}
# After reset.py a prompt is ~85 s away and X starts on the way; 170 s of
# boot_wait is what quake-timedemo.sh uses and it has never raced the boot.
BOOT_WAIT=170; [ "$CURRENT" = 1 ] && BOOT_WAIT=0

OUT=artifacts/perf-plan/churn-$L-$(date +%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh

# ---- host-side prerequisites, checked before the board is touched
STAMP=$(grep -a -m1 -o 'Linux version 7[^#]*#[0-9]*' images/xipImage 2>/dev/null | grep -o '#[0-9]*$')
[ -n "$STAMP" ] || { echo "churn.sh: no build stamp in images/xipImage - run make sync-images"; exit 2; }
grep -q ' T arch_cpu_idle$' images/System.map || { echo "churn.sh: images/System.map has no arch_cpu_idle"; exit 2; }
# sysctl_sched_features: VA in the map -> PSRAM PA (VA - 0x70800000). Read
# only, printed in the header so an arm's devmem write can be verified.
SF_VA=$(grep ' D sysctl_sched_features$' images/System.map | cut -c1-8)
SF_PA=""
[ -n "$SF_VA" ] && SF_PA=$(printf '0x%08x' $((0x$SF_VA - 0x70800000)))
echo "churn.sh [$L]: images/xipImage is $STAMP, sysctl_sched_features VA 0x$SF_VA PA ${SF_PA:-?}, -> $OUT"

# ---- the board script
QLAUNCH=""
if [ "$DEMO" = 1 ]; then
	QLAUNCH="setsid sh -c 'DISPLAY=:0 HOME=/root/quake exec $QUAKE_BIN id1 -basedir /root/quake $QUAKE_ARGS -width 320 -height 240 -fullscreen -condebug +timedemo demo1 >/root/quake/td.log 2>&1' </dev/null >/dev/null 2>&1 &"
fi
cat > "$S" <<EOF
# churn window, generated by scripts/board/churn.sh - do not edit by hand
echo "UNAME \$(uname -a)"
echo "SIL_STUCK \$(cat /sys/module/kernel/parameters/esp32s31_sil_stuck 2>/dev/null || echo MISSING)"
echo "SCHED_FEATURES \$( [ -n "$SF_PA" ] && devmem $SF_PA 32 2>/dev/null || echo MISSING) pa=$SF_PA"
echo "H1S \$(ls -l /root/h1s /root/pin 2>&1 | tr '\n' ' ')"
chmod +x /root/h1s /root/pin 2>/dev/null
for p in \$(ps | awk '/[t]iopex|[t]yr-quake|[t]yrquake/ {print \$1}'); do kill -9 \$p 2>/dev/null; done
rm -f /root/prof.txt /root/prof-pinned.txt /root/maps.txt
rm -rf /tmp/churn-A /tmp/churn-B; mkdir -p /tmp/churn-A /tmp/churn-B
cd /root/quake 2>/dev/null || echo "NO_QUAKE_DIR"
$PRE
echo "PRE_STATUS \$? sched_features=\$( [ -n "$SF_PA" ] && devmem $SF_PA 32 2>/dev/null)"
rm -f /root/-basedir/qconsole.log /root/quake/id1/qconsole.log /root/quake/.tyrquake/id1/qconsole.log
# Benchmarks run quiet: DAC to the floor, sound stays on, restored at the end.
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
$QLAUNCH
sleep $SETTLE
P=\$(ps | awk '/[t]iopex|[t]yr-quake|[t]yrquake/ {print \$1}' | head -1)
echo "QPID \${P:-NONE} \$(grep -a '^Error:' /root/quake/td.log 2>/dev/null | head -1)"
snap() {
	d=/tmp/churn-\$1
	cat /proc/uptime > \$d/uptime
	cat /proc/interrupts > \$d/interrupts
	cat /proc/stat > \$d/stat
	cat /proc/softirqs > \$d/softirqs
	grep -aE 'cpu:|nr_events|nr_retries|nr_hangs|hrtick|tick_nohz|dl_task_timer|vblank|hrtimer_wakeup|sched_clock|watchdog|it_real' /proc/timer_list > \$d/timer_list 2>/dev/null
	grep -aE '^(pswpin|pswpout|pgmajfault|pgfault) ' /proc/vmstat > \$d/vmstat
	cat /sys/block/mmcblk0/stat > \$d/sdstat
	[ -n "\$P" ] && grep -aE 'ctxt_switches|^State|^Cpus_allowed_list|^VmRSS|^VmSwap' /proc/\$P/status > \$d/qstatus 2>/dev/null
	[ -n "\$P" ] && awk '{print \$14, \$15, \$12}' /proc/\$P/stat > \$d/qstat 2>/dev/null
}
snap A
i=0; while [ \$i -lt $CAPS ]; do /root/h1s $CTRL $BUF 4000 >> /root/prof.txt 2>/root/prof.err; i=\$((i+1)); done
snap B
[ -n "\$P" ] && cat /proc/\$P/maps > /root/maps.txt 2>/dev/null
if [ -n "\$P" ] && [ $PINNED -gt 0 ]; then
	/root/pin -p 0 \$P; echo "PINNED \$? \$(grep Cpus_allowed_list /proc/\$P/status)"
	i=0; while [ \$i -lt $PINNED ]; do /root/h1s $CTRL $BUF 4000 >> /root/prof-pinned.txt 2>>/root/prof.err; i=\$((i+1)); done
fi
echo "CAPS \$(grep -ac . /root/prof.txt 2>/dev/null) pinned \$(grep -ac . /root/prof-pinned.txt 2>/dev/null) err \$(head -c 200 /root/prof.err 2>/dev/null | tr '\n' ' ')"
if [ -n "\$P" ]; then
	# The demo is ~100-170 s at 11 fps; we are ~65 s in. One long sleep, then a slow poll.
	sleep 45
	i=0; while [ \$i -lt 24 ]; do grep -aqE "[0-9]+ frames" /root/quake/id1/qconsole.log /root/quake/.tyrquake/id1/qconsole.log /root/-basedir/qconsole.log 2>/dev/null && break; grep -aq "^Error:" /root/quake/td.log 2>/dev/null && break; sleep 5; i=\$((i+1)); done
	echo "RESULT \$(grep -ahE '[0-9]+ frames' /root/-basedir/qconsole.log /root/quake/id1/qconsole.log /root/quake/.tyrquake/id1/qconsole.log 2>/dev/null | head -1)"
	echo "QUAKE majflt=\$(awk '{print \$12}' /proc/\$P/stat 2>/dev/null) \$(grep -aE 'VmRSS|VmSwap' /proc/\$P/status 2>/dev/null | tr -s ' ' | tr '\n' ' ')"
	kill -9 \$P 2>/dev/null
fi
echo "MEM \$(grep -aE 'MemAvailable|SwapFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')"
echo "SCANOUT_FAIL \$(dmesg | grep -ac 'failed to start scanout')"
amixer -q sset 'DACL' 178 2>/dev/null; amixer -q sset 'DACR' 178 2>/dev/null
for s in A B; do echo "SNAP_BEGIN \$s"; for f in /tmp/churn-\$s/*; do echo "==> \${f##*/} <=="; cat \$f; done; echo "SNAP_END \$s"; done
echo PROF_BEGIN; cat /root/prof.txt 2>/dev/null; echo PROF_END
echo PROFPIN_BEGIN; cat /root/prof-pinned.txt 2>/dev/null; echo PROFPIN_END
echo MAPS_BEGIN; cat /root/maps.txt 2>/dev/null; echo MAPS_END
rm -rf /tmp/churn-A /tmp/churn-B
echo CHURN_DONE
EOF

# ---- run it (every step bounded: reset.py by esptool, runsh by TIMEOUT + the board watchdog)
if [ "$CURRENT" = 0 ]; then
	echo "churn.sh: reset.py (fresh boot), then runsh $TIMEOUT s after a $BOOT_WAIT s boot wait"
	python3 scripts/board/reset.py >"$OUT/reset.log" 2>&1 || { echo "churn.sh: reset.py failed:"; cat "$OUT/reset.log"; exit 3; }
else
	echo "churn.sh: --current, no reset; runsh $TIMEOUT s"
fi
python3 scripts/board/runsh.py "$S" "$TIMEOUT" "$BOOT_WAIT" 2>"$OUT/runsh.err" | tr -d '\r' > "$OUT/run.log"
rc=${PIPESTATUS[0]}
if [ "$rc" = 3 ] || grep -q 'SERIAL PORT BUSY' "$OUT/runsh.err" "$OUT/run.log" 2>/dev/null; then
	echo "churn.sh: SERIAL PORT BUSY - another tool holds the console. This is NOT a dead board; stop the holder."; cat "$OUT/runsh.err"; exit 3
fi
if grep -q '^NO_SHELL' "$OUT/run.log"; then
	echo "churn.sh: $(grep -m1 '^NO_SHELL' "$OUT/run.log") - ask scripts/board/alive.py, do not reset blind"; exit 3
fi
if grep -q 'RS_TIMEKILL' "$OUT/run.log"; then
	echo "churn.sh: THE BOARD KILLED THE SCRIPT (outran TIMEOUT=$TIMEOUT). Board is fine; result is HALF-APPLIED. Nothing below is a result."
fi
grep -q '^CHURN_DONE' "$OUT/run.log" || echo "churn.sh: WARNING no CHURN_DONE marker - partial output"

# ---- unpack
for s in A B; do
	mkdir -p "$OUT/snap$s"
	sed -n "/^SNAP_BEGIN $s/,/^SNAP_END $s/p" "$OUT/run.log" | awk -v d="$OUT/snap$s" '
		/^==> .* <==$/ { f = d "/" $2; next }
		/^SNAP_(BEGIN|END)/ { next }
		f { print > f }'
done
sed -n '/^PROF_BEGIN/,/^PROF_END/p' "$OUT/run.log" | grep -aE '^[0-9a-f]{8}$' > "$OUT/prof.txt"
sed -n '/^PROFPIN_BEGIN/,/^PROFPIN_END/p' "$OUT/run.log" | grep -aE '^[0-9a-f]{8}$' > "$OUT/prof-pinned.txt"
sed -n '/^MAPS_BEGIN/,/^MAPS_END/p' "$OUT/run.log" | grep -av '^MAPS_' > "$OUT/maps.txt"

# ---- self-checks
UN=$(grep -a -m1 '^UNAME ' "$OUT/run.log")
BOARDN=$(echo "$UN" | grep -o '#[0-9]*' | head -1)
SIL=$(grep -a -m1 '^SIL_STUCK ' "$OUT/run.log" | awk '{print $2}')
{
	echo "== churn [$L] $(date '+%Y-%m-%d %H:%M:%S')  $OUT"
	echo "$UN"
	grep -aE '^(SIL_STUCK|SCHED_FEATURES|PRE_STATUS|QPID|PINNED|CAPS|RESULT|QUAKE|MEM|SCANOUT_FAIL|H1S)' "$OUT/run.log"
	echo "host: images/xipImage build $STAMP; board runs ${BOARDN:-?}"
} | tee "$OUT/summary.txt"
FAIL=0
if [ -z "$BOARDN" ] || [ "$BOARDN" != "$STAMP" ]; then
	echo "SELF-CHECK 1 FAILED: board kernel ${BOARDN:-unknown} != images/xipImage $STAMP -> System.map is for another kernel; symbols REFUSED" | tee -a "$OUT/summary.txt"; FAIL=1
fi
if [ "${SIL:-x}" != "0" ]; then
	echo "SELF-CHECK 3 FAILED: esp32s31_sil_stuck=${SIL:-missing} (must be 0) -> interrupt accounting untrusted; report REFUSED" | tee -a "$OUT/summary.txt"; FAIL=1
fi
NP=$(grep -ac . "$OUT/prof.txt"); NPP=$(grep -ac . "$OUT/prof-pinned.txt")
[ "$NP" = $((CAPS * 4000)) ] || echo "WARNING: $NP samples, expected $((CAPS * 4000)) (a short capture = the sampler never finished: wrong H1S addresses for this loader?)" | tee -a "$OUT/summary.txt"
fps=$(sed -n 's/.* \([0-9.]*\) fps.*/\1/p' "$OUT/run.log" | head -1)
if [ "$DEMO" = 1 ]; then
	if [ -n "$fps" ]; then
		awk -v f="$fps" 'BEGIN { if (f < 10.9 || f > 12.6) print "NOTE: fps " f " is OUTSIDE the 10.9-12.6 series band - the window is not comparable to the series"; else print "fps " f " inside the 10.9-12.6 band" }' | tee -a "$OUT/summary.txt"
	else
		echo "WARNING: no RESULT line (demo did not finish inside the window, or Quake failed: see QPID)" | tee -a "$OUT/summary.txt"
	fi
fi

# ---- rates (always: they need no symbols)
echo; echo "== rates: snapshot A -> B"
python3 "$CAT" rates "$OUT/snapA" "$OUT/snapB" | tee "$OUT/rates.txt"

# ---- symbols (refused on a failed self-check)
if [ "$FAIL" = 1 ]; then
	echo; echo "MAP MISMATCH / untrusted board: symbol resolution refused. Rates above stand on their own."; exit 4
fi
if [ "$NP" -lt 1000 ]; then
	echo; echo "too few samples ($NP) for a category report"; exit 4
fi
SYMARGS=(--symdir "$SYMDIR")
if [ -d "$EXTRA_NM_DIR" ]; then
	for f in "$EXTRA_NM_DIR"/*.nm; do
		[ -e "$f" ] || continue
		b=$(basename "$f" .nm)
		[ -e "$SYMDIR/$b.nm" ] || SYMARGS+=(--sym "$b=$f")
	done
fi
echo; echo "== h1s-report (unpinned, $NP samples; user symbols via maps)"
python3 scripts/board/h1s-report.py images/System.map "$OUT/prof.txt" 60 --maps "$OUT/maps.txt" "${SYMARGS[@]}" > "$OUT/report.txt" 2>&1
sed -n '1,45p' "$OUT/report.txt"
echo; echo "== categories (unpinned)"
python3 "$CAT" cat images/System.map "$OUT/prof.txt" --top 25 --label "$L-unpinned" | tee "$OUT/cat.txt"
crc=${PIPESTATUS[0]}
if [ "$crc" = 2 ]; then
	echo "SELF-CHECK 2 FAILED (MAP MISMATCH) - see above" | tee -a "$OUT/summary.txt"; exit 4
fi
if [ "$NPP" -ge 1000 ]; then
	echo; echo "== categories (PINNED to CPU0, $NPP samples - a different workload; never sum with the unpinned window)"
	python3 "$CAT" cat images/System.map "$OUT/prof-pinned.txt" --top 15 --label "$L-pinned" | tee "$OUT/cat-pinned.txt"
	python3 scripts/board/h1s-report.py images/System.map "$OUT/prof-pinned.txt" 40 --maps "$OUT/maps.txt" "${SYMARGS[@]}" > "$OUT/report-pinned.txt" 2>&1
fi
{
	echo "-- key rates"
	grep -aE '^(window|  ctxt|  IPI-IRQ:|  DEV-IRQ:|  TIMER-IRQ:|  Function-call|  arm |  VERDICT|  RCU:|  SCHED:)' "$OUT/rates.txt"
	grep -aE '^  (riscv-timer|IPI[0-9]+|[0-9]+:).*' "$OUT/rates.txt" | head -12
	echo "-- key categories (unpinned)"
	grep -aE '^  (idle|irq-entry|softirq|timers|scheduler|locks|memory|storage|syscall-fs|drivers|M-mode|other|past-_etext|SUM|non-idle|irq-entry\+|VERDICT)' "$OUT/cat.txt"
	grep -aE '^flash residue|^  arm E' "$OUT/cat.txt"
} | tee -a "$OUT/summary.txt"
echo; echo "churn.sh [$L]: done -> $OUT/summary.txt (rates.txt, cat.txt, cat-pinned.txt, report.txt)"
