# /root/gq/pre.sh <label> : apply one paging arm's runtime knobs, then (in the
# background) snapshot memory once mid-timedemo. Knobs from env, unset = shipped:
#   SWP=swappiness PC=page-cluster MINFREE=min_free_kbytes WSF=watermark_scale_factor
#   ZMB=zram MB (lzo-rle, priority 100 ahead of the SD file) ZCAP=zram mem_limit kB
#   ZALG=lzo|lzo-rle  CENSUS=1 full census at 75 s (else light snapshot at 120 s)
L=$1; O=/root/gq/$L.pre
exec >$O 2>&1
[ -n "$SWP" ] && sysctl -w vm.swappiness=$SWP
[ -n "$PC" ] && sysctl -w vm.page-cluster=$PC
[ -n "$MINFREE" ] && sysctl -w vm.min_free_kbytes=$MINFREE
[ -n "$WSF" ] && sysctl -w vm.watermark_scale_factor=$WSF
if [ -n "$ZMB" ]; then
	echo ${ZALG:-lzo-rle} > /sys/block/zram0/comp_algorithm
	echo $((ZMB * 1048576)) > /sys/block/zram0/disksize
	[ -n "$ZCAP" ] && echo $((ZCAP * 1024)) > /sys/block/zram0/mem_limit
	mkswap /dev/zram0 >/dev/null && s31swapon /dev/zram0 100
fi
cat /proc/swaps; sysctl vm.swappiness vm.page-cluster vm.min_free_kbytes vm.watermark_scale_factor
setsid sh /root/gq/snap.sh $L </dev/null >/root/gq/$L.snap 2>&1 &
