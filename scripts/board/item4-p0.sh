#!/bin/sh
# Paging plan item 4, first measurement (no build): the fault-to-run split.
# BOARD-SIDE; launches itself under setsid with output on the card:
#
#     python3 scripts/board/runsh.py scripts/board/item4-p0.sh 30 5
#     ... then collect /root/item4-p0.log (cat, it is small) when RUN_DONE
#
# Per arm: rootfs/faultlat pinned to CPU0 (where the game faults) pushes
# 3 MB of anonymous memory to swap with MADV_PAGEOUT, resets the sdtrace
# ring, then takes 60 major faults in random page order back to back (the
# card stays awake). The ring then holds exactly those reads: faultlat's
# per-fault time minus the ring's per-request total is the software above
# the driver, the ceiling of the SWP_SYNCHRONOUS_IO path. page-cluster 2
# (shipped) and 0 (the I/O shape of the synchronous path, which reads no
# neighbours) alternate, three of each. sdlat 4 KiB rand on CPU0 is the
# same-boot reference.
if [ "$1" != "--child" ]; then
	# runsh ships every script as /tmp/r.sh and the next runsh call
	# overwrites it while this child is still reading it (sh reads a
	# script incrementally: "unterminated quoted string" at line 47).
	cp "$0" /root/item4-p0.run.sh
	rm -f /root/item4-p0.log
	setsid sh /root/item4-p0.run.sh --child >/root/item4-p0.log 2>&1 </dev/null &
	echo LAUNCHED
	exit 0
fi
P=/sys/module/dw_mmc/parameters
uname -a
grep -E 'MemAvailable|SwapFree|SwapTotal' /proc/meminfo
echo "page-cluster $(cat /proc/sys/vm/page-cluster) vma_ra $(cat /sys/kernel/mm/swap/vma_ra_enabled) poll_bytes $(cat $P/poll_bytes 2>/dev/null) sched $(cat /sys/block/mmcblk0/queue/scheduler)"
vm() { awk '/^(pswpin|pswpout|swap_ra|swap_ra_hit|pgmajfault) / { printf "%s=%s ", $1, $2 }' /proc/vmstat; echo; }
echo "SDLAT_REF $(/root/pin 0 /root/sdlat /dev/mmcblk0 4 200 rand)"
PC0=$(cat /proc/sys/vm/page-cluster)
for r in 1 2 3; do
	for pc in 2 0; do
		echo $pc > /proc/sys/vm/page-cluster
		sync; sleep 1
		echo "ARM pc$pc run $r"
		echo "VM0 $(vm)"
		/root/pin 0 /root/faultlat 3 60 $P/sdtrace 0
		echo "VM1 $(vm)"
		echo "SDTRACE_BEGIN"; cat $P/sdtrace; echo "SDTRACE_END"
		echo "RING_BEGIN"
		echo 0 > $P/sdtrace_ring; cat $P/sdtrace_ring
		echo 32 > $P/sdtrace_ring; cat $P/sdtrace_ring
		echo 0 > $P/sdtrace_ring
		echo "RING_END"
	done
done
echo $PC0 > /proc/sys/vm/page-cluster
echo "RESTORED page-cluster $(cat /proc/sys/vm/page-cluster)"
echo RUN_DONE
