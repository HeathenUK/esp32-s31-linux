# 0065 step 5, arm 3: the min compared at a FIXED placement. Pre-flight
# (kernel #373) showed the unpinned baseline min 1.00 is the CPU0 placement
# and 1.17-1.19 is CPU1 (the cross-hart wake); the design's saving is the
# wake, so the 0 vs 4096 contrast is taken pinned to each CPU in turn.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/04d-b4-poll-pinned.sh 150
br() {
	echo "BR $1 ctxt=$(awk '/^ctxt/{print $2}' /proc/stat) irq=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts) $(awk '/^poll /{printf "%s ", $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')"
}
echo "KERNEL $(uname -r) $(uname -v)"
echo 1500000 > /sys/module/dw_mmc/parameters/poll_ns
for cpu in 0 1; do
	for arm in 0 4096 0 4096; do
		sync; sleep 1
		echo $arm > /sys/module/dw_mmc/parameters/poll_bytes
		echo "ARM cpu$cpu poll_bytes=$(cat /sys/module/dw_mmc/parameters/poll_bytes)"
		for r in 1 2; do
			br "pre  cpu$cpu p$arm r$r"
			/root/pin $cpu /root/sdlat /dev/mmcblk0 4 300 rand
			br "post cpu$cpu p$arm r$r"
		done
	done
done
echo 0 > /sys/module/dw_mmc/parameters/poll_bytes
echo "DMESG_ERR $(dmesg | grep -c 'data error\|did not quiesce')"
echo B4D_DONE
