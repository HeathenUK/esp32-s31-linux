#!/bin/sh
# 0063/0064 write check: request_end's completion call was touched and
# writes share it. 24 MB O_DIRECT write, drop_caches, sha256 of the
# read-back must MATCH, sdprobe busy waited 0, no dmesg errors. ~30 s.
uname -a
cd /root || exit 1
rm -f wc.src wc.dst
dd if=/dev/urandom of=wc.src bs=1M count=24 2>&1 | tail -1
sync; echo 3 > /proc/sys/vm/drop_caches
echo 0 > /sys/module/dw_mmc/parameters/sdprobe
T0=$(cut -d' ' -f1 /proc/uptime)
if ! dd if=wc.src of=wc.dst bs=1M count=24 oflag=direct 2>&1 | tail -1; then
	echo "DD_DIRECT_UNSUPPORTED, falling back to conv=fsync"
	dd if=wc.src of=wc.dst bs=1M count=24 conv=fsync 2>&1 | tail -1
fi
T1=$(cut -d' ' -f1 /proc/uptime)
echo "WRITE_SECS $T0 -> $T1"
grep -a 'busy\|writes\|^req' /sys/module/dw_mmc/parameters/sdprobe | head -4
sync; echo 3 > /proc/sys/vm/drop_caches
A=$(sha256sum wc.src | cut -c1-16); B=$(sha256sum wc.dst | cut -c1-16)
echo "SHA src=$A dst=$B"
[ "$A" = "$B" ] && echo "SHA_MATCH" || echo "SHA_MISMATCH"
dmesg | grep -ai 'error\|timeout\|mmc.*fail' | tail -3
echo "DMESG_ERRORS $(dmesg | grep -aic 'error\|timeout\|mmc.*fail')"
rm -f wc.src wc.dst
echo WC_DONE
