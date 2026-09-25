#!/bin/sh
# Paging plan item 4, the build's same-boot arms (kernel with 0066 sync_swap).
# BOARD-SIDE; launches itself under setsid with output on the card:
#
#     python3 scripts/board/runsh.py scripts/board/item4-arms.sh 30 5
#     ... collect /root/item4-arms.log when it ends in RUN_DONE
#     python3 scripts/board/item4-p0.py <log>
#
# Alternates mmcblk.sync_swap 0 / 1 (swapoff + swapon between arms, since
# swapon reads BLK_FEAT_SYNCHRONOUS once), three of each, and runs the same
# faultlat probe as item4-p0.sh (pinned CPU0, 3 MB paged out, 60 majors
# back to back, the sdtrace ring reset just before the touch phase).
if [ "$1" != "--child" ]; then
	cp "$0" /root/item4-arms.run.sh	# runsh reuses /tmp/r.sh
	rm -f /root/item4-arms.log
	setsid sh /root/item4-arms.run.sh --child >/root/item4-arms.log 2>&1 </dev/null &
	echo LAUNCHED
	exit 0
fi
P=/sys/module/dw_mmc/parameters
Q=/sys/module/mmcblk/parameters/sync_swap
uname -a
grep -E 'MemAvailable|SwapFree' /proc/meminfo
echo "page-cluster $(cat /proc/sys/vm/page-cluster) poll_bytes $(cat $P/poll_bytes) sched $(cat /sys/block/mmcblk0/queue/scheduler)"
vm() { awk '/^(pswpin|pswpout|swap_ra|swap_ra_hit|pgmajfault|allocstall_normal) / { printf "%s=%s ", $1, $2 }' /proc/vmstat; echo; }
for r in 1 2 3; do
	for s in 0 1; do
		echo $s > $Q
		swapoff /swapfile && swapon /swapfile
		dmesg | grep 'mmcblk: sync_swap' | tail -1
		sync; sleep 1
		echo "ARM s$s run $r"
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
echo 0 > $Q
swapoff /swapfile && swapon /swapfile
echo "RESTORED sync_swap $(cat $Q)"; dmesg | grep 'mmcblk: sync_swap' | tail -1
echo RUN_DONE
