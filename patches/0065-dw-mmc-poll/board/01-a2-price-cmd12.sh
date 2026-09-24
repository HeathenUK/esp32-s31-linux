# 0064 step 2 (A2 + A1): price the software-CMD12 leg by contrast, no build.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/01-a2-price-cmd12.sh 150
# Board idle, done_complete left at 2, knobs (if the 0064 kernel is on) at 0.
# Every sdlat run is bracketed by BR lines: ctxt from /proc/stat, the dw-mci
# IRQ count summed over both CPU columns, and sdprobe's cmds_all n /
# req_total n / issue2cmd / cmd2data / last_opcode. Per run derive
#   irq/req  = d(irq)/300      cmds/req = d(cmds_all n)/300
#   ctxt/req = d(ctxt)/300     last_opcode after the run
# Expected: 512b -> ~2 IRQs, 1 cmd, last_opcode 17;
#           1024b -> ~3 IRQs, 2 cmds, last_opcode 12 (software CMD12 proven).
# stop_leg = min(1024b) - min(512b) - 0.013 ms.
# Decision: >= 0.15 ms -> B3 worth its arm; 0.10-0.15 -> keep the knob,
# expect a tie; < 0.10 or last_opcode != 12 -> kill B3, record the numbers.
# A1 kill for the whole hop family: issue2cmd + cmd2data >= 1.0 ms.
br() {
	echo "BR $1 ctxt=$(awk '/^ctxt/{print $2}' /proc/stat) irq=$(awk '/dw-mci/{s=0; for(i=2;i<=3;i++) s+=$i; print s}' /proc/interrupts) $(awk '/^cmds_all|^req_total|^issue2cmd|^cmd2data/{printf "%s ", $0}' /sys/module/dw_mmc/parameters/sdprobe | tr -s ' ')"
}
echo "KERNEL $(uname -r) $(uname -v)"
echo "SCHED $(cat /sys/block/mmcblk0/queue/scheduler) done_complete=$(cat /sys/module/dw_mmc_pltfm/parameters/done_complete) poll_bytes=$(cat /sys/module/dw_mmc/parameters/poll_bytes 2>/dev/null) auto_stop=$(cat /sys/module/dw_mmc/parameters/auto_stop 2>/dev/null)"
sync; sleep 1
for round in 1 2 3; do
	for sz in 512b 1024b 4; do
		br "pre  r$round $sz"
		/root/sdlat /dev/mmcblk0 $sz 300 rand
		br "post r$round $sz"
	done
done
echo "DMESG_ERR $(dmesg | grep -c 'data error\|did not quiesce')"
echo A2_DONE
