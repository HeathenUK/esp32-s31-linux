# 0065 step 5, arm 2: poll_ns 500000 / 1500000 / 3000000 at poll_bytes=4096,
# two runs each, A/B/C/B/A/C. Card p90 is 1.8-2.5 ms, so a 1.5 ms budget
# expires 10-25% of reads; this says whether the budget moves p90/p99 and
# what a longer spin buys.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/04c-b4-poll-ns-sweep.sh 150
br() {
	echo "BR $1 ctxt=$(awk '/^ctxt/{print $2}' /proc/stat) $(awk '/^poll /{printf "%s ", $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')"
}
echo "KERNEL $(uname -r) $(uname -v)"
echo 4096 > /sys/module/dw_mmc/parameters/poll_bytes
for ns in 500000 1500000 3000000 1500000 500000 3000000; do
	sync; sleep 1
	echo $ns > /sys/module/dw_mmc/parameters/poll_ns
	echo "ARM poll_ns=$(cat /sys/module/dw_mmc/parameters/poll_ns)"
	for r in 1 2; do
		br "pre  ns$ns r$r"
		/root/sdlat /dev/mmcblk0 4 300 rand
		br "post ns$ns r$r"
	done
done
echo 1500000 > /sys/module/dw_mmc/parameters/poll_ns
echo 0 > /sys/module/dw_mmc/parameters/poll_bytes
echo "DMESG_ERR $(dmesg | grep -c 'data error\|did not quiesce')"
echo B4C_DONE
