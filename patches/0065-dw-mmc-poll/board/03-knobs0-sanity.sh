# 0064 step 4 (after the flash): the new kernel with both knobs at 0 must be
# a no-op against #369 before any arm is believed.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/03-knobs0-sanity.sh 60
# Pass: uname shows the new #N (>= 370); poll_bytes/auto_stop/poll_ns read
# (0065 ships poll_bytes=16384 by default - echo 0 first for a no-op arm)
# 0/0/1500000; both sdlat mins within 0.03 ms of #369 (min 1.17, p50 1.28,
# ctxt/req 2); DMESG_ERR 0; the dw-mci IRQ rate at idle is ~0 (no ACD
# flood). Kill: any 'data error' / 'did not quiesce' line at knobs 0 means
# the ACD/RINTSTS edit changed the idle path - revert that hunk first.
echo "KERNEL $(uname -a)"
echo "KNOBS poll_bytes=$(cat /sys/module/dw_mmc/parameters/poll_bytes) auto_stop=$(cat /sys/module/dw_mmc/parameters/auto_stop) poll_ns=$(cat /sys/module/dw_mmc/parameters/poll_ns) done_complete=$(cat /sys/module/dw_mmc_pltfm/parameters/done_complete)"
echo "SCHED $(cat /sys/block/mmcblk0/queue/scheduler)"
i0=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts); sleep 3
i1=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts)
echo "IDLE_IRQ_3S $((i1 - i0))"
sync; sleep 1
for r in 1 2; do
	c0=$(awk '/^ctxt/{print $2}' /proc/stat)
	/root/sdlat /dev/mmcblk0 4 300 rand
	c1=$(awk '/^ctxt/{print $2}' /proc/stat)
	echo "CTXT run$r $((c1 - c0)) /300"
done
grep '^poll\|^autostop\|^gap\|^cmds_all\|^req_total' /sys/module/dw_mmc/parameters/sdprobe
echo "DMESG_ERR $(dmesg | grep -c 'data error\|did not quiesce')"
dmesg | grep 'data error\|did not quiesce' | tail -3
echo SANITY_DONE
