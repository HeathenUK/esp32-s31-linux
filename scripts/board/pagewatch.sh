#!/bin/sh
# pagewatch.sh <pid> <out.txt> - board-side, fork-free paging sampler.
#
# One line per second while <pid> lives:
#   uptime  pid_majflt  lvdesk_majflt  sd_read_sectors  SwapFree_kB
# Every value is a counter the kernel keeps anyway (/proc/<pid>/stat field
# 12, /sys/block/mmcblk0/stat, /proc/meminfo), read with shell builtins, so
# the sampler costs nothing the workload can feel - unlike grep/awk pipelines,
# each of which is a fork. Run it under setsid from the arming script and
# read the file afterwards; diff the columns for faults/s, KB/s and swap.
P=$1; OUT=$2
L=$(for p in /proc/[0-9]*; do read c < $p/comm; [ "$c" = lvdesk ] && echo ${p#/proc/} && break; done)
: > "$OUT"
while [ -d /proc/$P ]; do
	read up rest < /proc/uptime
	read a b c d e f g h i j k mf rest2 < /proc/$P/stat
	lf=0; [ -n "$L" ] && read a2 b2 c2 d2 e2 f2 g2 h2 i2 j2 k2 lf rest3 < /proc/$L/stat
	read rd rm rs rest4 < /sys/block/mmcblk0/stat
	while read key v u; do [ "$key" = SwapFree: ] && sf=$v; done < /proc/meminfo
	echo "$up $mf $lf $rs $sf" >> "$OUT"
	sleep 1
done
