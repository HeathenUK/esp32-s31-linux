# /root/gq/stream.sh <label> : the 99-s31-memory.conf min_free_kbytes measure.
# CoreMark idle, then CoreMark while dd streams a 32 MB uncached file 3x
# (both pinned to CPU0, the one-CPU contention the 2026-08-18 number had).
L=$1; O=/root/gq/stream-$L.txt; exec >$O 2>&1
[ -n "$MINFREE" ] && sysctl -w vm.min_free_kbytes=$MINFREE
echo "STREAM $L $(uname -v) up $(cut -d' ' -f1 /proc/uptime) $(sysctl -n vm.min_free_kbytes) $(sysctl -n vm.swappiness)"
while [ $(cut -d. -f1 /proc/uptime) -lt 75 ]; do sleep 5; done
echo "IDLE $(/root/s31pin 1 coremark 2>&1 | grep -a 'Iterations/Sec')"
F=/root/gq/stream32.bin
( i=1; while [ $i -le 3 ]; do t0=$(cut -d' ' -f1 /proc/uptime); /root/s31pin 1 dd if=$F of=/dev/null bs=64k 2>/dev/null; t1=$(cut -d' ' -f1 /proc/uptime); echo "DD $i $t0 $t1" | awk '{printf "DD %d %.2f s %.2f MB/s\n",$2,$4-$3,32/($4-$3)}'; i=$((i+1)); done ) > /root/gq/stream-$L.dd &
D=$!
echo "LOAD $(/root/s31pin 1 coremark 2>&1 | grep -a 'Iterations/Sec')"
wait $D; cat /root/gq/stream-$L.dd
grep -E 'allocstall_normal|pgsteal_direct|pgsteal_kswapd' /proc/vmstat | tr '\n' ' '; echo
dmesg | grep -ci 'page allocation failure' | sed 's/^/ALLOCFAIL /'
echo STREAMDONE
