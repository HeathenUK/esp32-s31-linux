# 0064 step 3: confirm the issuing context and the dw-mci IRQ's CPU, no build.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/02-preflight.sh 60
# Pass: scheduler is [none] and the dw-mci IRQ is pinned to one CPU (record
# which). CPU1 arm expected +25-45 us on min and possibly +1 ctxt/req (the
# cross-hart IPI). Kill for the DESIGN: scheduler not none -> restore it
# (echo none > /sys/block/mmcblk0/queue/scheduler) before any poll arm;
# polling inside queue_rq is pointless when kblockd is the issuer.
IRQ=$(awk '/dw-mci/{sub(":","",$1); print $1}' /proc/interrupts)
echo "IRQ dw-mci=$IRQ smp_affinity=$(cat /proc/irq/$IRQ/smp_affinity) effective=$(cat /proc/irq/$IRQ/effective_affinity 2>/dev/null)"
echo "IRQLINE $(grep dw-mci /proc/interrupts | tr -s ' ')"
echo "SCHED $(cat /sys/block/mmcblk0/queue/scheduler) rq_affinity=$(cat /sys/block/mmcblk0/queue/rq_affinity) nomerges=$(cat /sys/block/mmcblk0/queue/nomerges)"
echo "ONLINE $(cat /sys/devices/system/cpu/online)"
sync; sleep 1
for cpu in 0 1 0 1; do
	c0=$(awk '/^ctxt/{print $2}' /proc/stat)
	i0=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts)
	echo "PIN cpu$cpu $(/root/pin $cpu /root/sdlat /dev/mmcblk0 4 300 rand)"
	c1=$(awk '/^ctxt/{print $2}' /proc/stat)
	i1=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts)
	echo "PIN cpu$cpu ctxt/req=$(( (c1 - c0) )) irq/req=$(( (i1 - i0) ))  (divide both by 300)"
done
echo PREFLIGHT_DONE
