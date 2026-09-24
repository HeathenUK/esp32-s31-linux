# 0064 step 5, arm 2 (only if 04 shows a p99 tail > 1 ms above the 0 arm):
# the budget-expiry tail at poll_ns 500000 vs 1500000, poll_bytes=4096.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/04b-b4-poll-ns.sh 120
# Read sdprobe's poll expired/hit per arm: a shorter budget must move
# expired up and p99 down; if p99 does not move, the tail is the card, not
# the spin, and poll_ns stays at 1500000.
br() {
	echo "BR $1 ctxt=$(awk '/^ctxt/{print $2}' /proc/stat) $(awk '/^poll /{printf "%s ", $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')"
}
echo 0 > /sys/module/dw_mmc/parameters/auto_stop
echo 4096 > /sys/module/dw_mmc/parameters/poll_bytes
for ns in 500000 1500000 500000 1500000; do
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
echo B4B_DONE
