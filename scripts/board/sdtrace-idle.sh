#!/bin/sh
# sdtrace validation + idle hop table. BOARD-SIDE; ship it with
#
#     python3 scripts/board/runsh.py scripts/board/sdtrace-idle.sh 60 20
#
# Five arms on one boot, ~2.5 s of I/O in total: done_complete 2 / 0 / 2 at
# 4 KiB x 300 random O_DIRECT reads with sdlat pinned to CPU0, the same at
# mode 2 pinned to CPU1 (the cross-hart placement) (the A/B/A that validates the ring on a
# change whose size is already known - 0063 README: mode 0 -> 2 is ~0.15 ms
# and one context switch per request), then one 64 KiB x 100 arm to see the
# hops scale (c2d should grow ~1 ms, everything else stay flat). Each arm
# prints the summary the ring held BEFORE the reset, resets, runs sdlat with
# /proc/interrupts and /proc/stat ctxt bracketed, then the summary and all
# 64 ring lines. scripts/board/sdtrace-check.py parses the log and applies
# the pass/kill rules. done_complete is left at 2, the shipped default.
P=/sys/module/dw_mmc/parameters
D=/sys/module/dw_mmc_pltfm/parameters/done_complete
uname -a
if [ ! -r $P/sdtrace ]; then
	echo "SDTRACE_MISSING: this kernel has no sdtrace param - wrong kernel flashed? (uname above)"
	exit 1
fi
if [ ! -x /root/sdlat ] || [ ! -x /root/pin ]; then
	echo "SDLAT_MISSING: /root/sdlat or /root/pin not on the card (rootfs/, deploy.py them)"
	exit 1
fi
sync; sleep 1

irqs() { awk '/dw-mci/ { s = 0; for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) s += $i; print s }' /proc/interrupts; }
ctxt() { awk '/^ctxt/ { print $2 }' /proc/stat; }

# Every arm pins sdlat to a CPU (rootfs/pin: `pin <cpu> <cmd...>`). Unpinned,
# the scheduler's placement decided the arm: on kernel #370 the m2a/m2b
# repeats disagreed by 25% (indrv 399 vs 554 us) only because one landed
# on CPU1 and the other on CPU0, where the dw-mci hardirq is taken and the
# submitter's spin_lock_bh defers the BH. Two placements are two arms.
arm() { # label mode kb count cpu
	echo "$2" > $D; sync; sleep 1
	echo "ARM $1 mode=$(cat $D) kb=$3 n=$4 cpu=$5"
	echo "PRE_RESET $(head -1 $P/sdtrace)"
	echo 0 > $P/sdtrace
	I0=$(irqs); C0=$(ctxt)
	echo "SDLAT $(/root/pin "$5" /root/sdlat /dev/mmcblk0 "$3" "$4" rand)"
	I1=$(irqs); C1=$(ctxt)
	echo "DELTA irq=$((I1 - I0)) ctxt=$((C1 - C0)) n=$4 irq_x100_per_req=$(( (I1 - I0) * 100 / $4 )) ctxt_x100_per_req=$(( (C1 - C0) * 100 / $4 ))"
	echo "SDTRACE_BEGIN"
	cat $P/sdtrace
	echo "SDTRACE_END"
	echo "RING_BEGIN"
	echo 0 > $P/sdtrace_ring; cat $P/sdtrace_ring
	echo 32 > $P/sdtrace_ring; cat $P/sdtrace_ring
	echo "RING_END"
}

arm m2a 2 4 300 0
arm m0 0 4 300 0
arm m2b 2 4 300 0
arm m2c 2 4 300 1
arm k64 2 64 100 0
echo 2 > $D
echo 0 > $P/sdtrace_ring
echo "MODE_RESTORED $(cat $D)"
echo "RUN_DONE"
