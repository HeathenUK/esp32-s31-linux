# 0064 step 5 (B4): polled completion, same-boot A/B/A on sdlat.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/04-b4-poll-aba.sh 150
# Board idle, done_complete=2, auto_stop=0. Arms 4096 0 4096 16384 0.
# Per arm: 3 x 4k/300 rand and 1 x 16k/200 rand, each bracketed (BR lines:
# ctxt, dw-mci IRQ sum, sdprobe req_total/issue2cmd/poll).
# Expected at poll_bytes=4096: min 0.85-0.95, p50 1.00-1.15, ctxt/req ~0,
# IRQs/req unchanged (arm 1 still takes the hardirqs; they find MINTSTS
# clear), sdprobe poll hit ~= n, expired ~1-2%. 16384 arm: the 16k run
# gets the same absolute saving (~0.3 ms).
# Pass: min improves >= 0.15 ms AND ctxt/req <= 1 in both 4096 arms with
# the 0 arms back at ~1.17/2. Kill: min moves < 0.10 ms or ctxt does not
# fall (completion not landing inline - check done_complete=2 and that the
# request had the cap), or any p99 above the 0 arm's by > 1 ms (budget
# expiry double-paying - then run 04b with poll_ns 500000).
br() {
	echo "BR $1 ctxt=$(awk '/^ctxt/{print $2}' /proc/stat) irq=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts) $(awk '/^req_total|^issue2cmd|^poll /{printf "%s ", $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')"
}
echo "KERNEL $(uname -r) $(uname -v)"
echo 0 > /sys/module/dw_mmc/parameters/auto_stop
echo "SETUP done_complete=$(cat /sys/module/dw_mmc_pltfm/parameters/done_complete) auto_stop=$(cat /sys/module/dw_mmc/parameters/auto_stop) poll_ns=$(cat /sys/module/dw_mmc/parameters/poll_ns) sched=$(cat /sys/block/mmcblk0/queue/scheduler)"
for arm in 4096 0 4096 16384 0; do
	sync; sleep 1
	echo $arm > /sys/module/dw_mmc/parameters/poll_bytes
	echo "ARM poll_bytes=$(cat /sys/module/dw_mmc/parameters/poll_bytes)"
	for r in 1 2 3; do
		br "pre  arm$arm 4k r$r"
		/root/sdlat /dev/mmcblk0 4 300 rand
		br "post arm$arm 4k r$r"
	done
	br "pre  arm$arm 16k"
	/root/sdlat /dev/mmcblk0 16 200 rand
	br "post arm$arm 16k"
done
echo 0 > /sys/module/dw_mmc/parameters/poll_bytes
echo "DMESG_ERR $(dmesg | grep -c 'data error\|did not quiesce')"
echo B4_DONE
