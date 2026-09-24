# 0065 diagnosis: which hop holds the time when a polled read lands at
# budget + ~1 ms. poll_bytes=4096, poll_ns=3000000, sdlat pinned to each CPU,
# sdtrace reset before each run, summary + newest 32 ring rows after.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/04e-b4-poll-ring.sh 90
echo "KERNEL $(uname -r) $(uname -v)"
echo 4096 > /sys/module/dw_mmc/parameters/poll_bytes
echo 3000000 > /sys/module/dw_mmc/parameters/poll_ns
for cpu in 0 1; do
	sync; sleep 1
	echo 0 > /sys/module/dw_mmc/parameters/sdtrace
	p0=$(awk '/^poll /{print $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')
	echo "RUN cpu$cpu $(/root/pin $cpu /root/sdlat /dev/mmcblk0 4 300 rand)"
	echo "POLL cpu$cpu pre: $p0 | post: $(awk '/^poll /{print $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')"
	echo "SDTRACE cpu$cpu"
	cat /sys/module/dw_mmc/parameters/sdtrace
	echo "RING cpu$cpu"
	cat /sys/module/dw_mmc/parameters/sdtrace_ring
done
echo 1500000 > /sys/module/dw_mmc/parameters/poll_ns
echo 0 > /sys/module/dw_mmc/parameters/poll_bytes
echo "DMESG_ERR $(dmesg | grep -c 'data error\|did not quiesce')"
echo B4E_DONE
