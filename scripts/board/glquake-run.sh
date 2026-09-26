# glquake-run.sh - one QuakeSpasm (GLQuake, SDL 1.2) run ON the board:
#   setsid sh glquake-run.sh <label> <max_secs> [quakespasm args...] &
# Runs /root/quake/quakespasm from SD (-basedir /root/quake -condebug plus the
# given args), then waits QUIETLY: one sleep per 10 s, one awk per sample, no
# fork storm (memory s31-harness-lies: a poll loop is a memory-pressure
# workload of its own on this board). Every 10 s it records the game's
# VmRSS/VmSwap/majflt and the system's pswpin/pswpout/pgmajfault, so paging
# during the timedemo is visible as a series, not an end total.
# GQ_BASE picks the basedir (default /root/quake). QuakeSpasm IGNORES
# "+command" arguments with the shareware pak (the cmdline cvar is set only
# when gfx/pop.lmp exists: COM_CheckRegistered, common.c), so a timedemo
# runs from /root/quake/td, whose id1/autoexec.cfg says "timedemo demo1"
# (quake.rc execs autoexec.cfg before startdemos) beside a pak0.pak symlink.
# GQ_WRAP is prefixed to the command (e.g. "/root/s31pin 1" to pin).
# Ends when qconsole.log has the timedemo "N frames" line, on "Error"/exit,
# or at max_secs. Output: /root/gq/<label>.txt (collect it afterwards).
# Sound stays on at the codec's quiet level (DAC 110, restored to 143).
L=$1; MAX=$2; shift 2
mkdir -p /root/gq; O=/root/gq/$L.txt
exec >$O 2>&1
echo "GQ $L args: $* | $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
echo "MEM0 $(grep -E 'MemAvailable|MemFree|SwapFree|CmaFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')"
vm() { awk '/^(pswpin|pswpout|pgmajfault) /{printf "%s=%s ",$1,$2}' /proc/vmstat; }
echo "VM0 $(vm)"
B=${GQ_BASE:-/root/quake}
rm -f $B/qconsole.log
# the desktop's own counters (SIGUSR1 report): present gaps (fullscreen) and
# the MIT-SHM put count, before and after
lvrep() { kill -USR1 $(pidof lvdesk lvdesk.new) 2>/dev/null; sleep 1; grep -a 'lvdesk: frames\|MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 2 | tr '\n' ' '; }
echo "LV0 $(lvrep)"
amixer -q sset DACL 110 2>/dev/null; amixer -q sset DACR 110 2>/dev/null
cd $B	# qconsole.log is written to the working directory
T0=$(cut -d' ' -f1 /proc/uptime)
DISPLAY=:0 HOME=/root/quake setsid $GQ_WRAP /root/quake/quakespasm -basedir $B -condebug "$@" >/root/gq/$L.out 2>&1 </dev/null &
P=$!
t=0
while [ $t -lt $MAX ]; do
	sleep 10; t=$((t+10))
	[ -d /proc/$P ] || { echo "EXITED at ${t}s"; break; }
	echo "S $t $(awk '/^(VmRSS|VmSwap)/{printf "%s%s ",$1,$2}' /proc/$P/status) majflt=$(awk '{print $12}' /proc/$P/stat) sdrd=$(awk '{print $3}' /sys/block/mmcblk0/stat) $(vm)"
	# once, mid-run: every mapping with more than 256 kB resident or swapped
	[ $t -eq ${GQ_SMAPS:-60} ] && awk '/^[0-9a-f]+-/{if(n>=256)print "M",sz,r,sw,n,nm; nm=$6; r=0;sw=0;n=0} /^Size:/{sz=$2} /^Rss:/{r=$2;n+=$2} /^Swap:/{sw=$2;n+=$2} END{if(n>=256)print "M",sz,r,sw,n,nm}' /proc/$P/smaps
	# GQ_PROF=<t>: at t s pin the game and the desktop to CPU0 (the hart0 PC
	# sampler sees only hart 1 = CPU0), take GQ_PROFN x 4000 h1s samples
	# (4 s each) and both processes' maps, for h1s-report.py
	if [ -n "$GQ_PROF" ] && [ $t -eq $GQ_PROF ]; then
		LV=$(pidof lvdesk lvdesk.new); /root/s31pin -p 1 $P; /root/s31pin -p 1 $LV; sleep 1
		cat /proc/$P/maps > /root/gq/$L.qmaps; cat /proc/$LV/maps > /root/gq/$L.lmaps
		cat /proc/interrupts > /root/gq/$L.irq0; grep -E '^(ctxt|intr|cpu)' /proc/stat > /root/gq/$L.stat0
		for th in /proc/$P/task/*; do echo "$(cat $th/comm) $(awk '{print $14,$15,$39}' $th/stat) $(grep ctxt $th/status | tr -s ' \t' ' ' | tr '\n' ' ')"; done > /root/gq/$L.thr0
		q0=$(awk '{print $14" "$15}' /proc/$P/stat); l0=$(awk '{print $14" "$15}' /proc/$LV/stat)
		i=0; : > /root/gq/$L.pcs
		while [ $i -lt ${GQ_PROFN:-2} ]; do /root/h1s ${H1S_CTRL:-0x2f030c1c} ${H1S_BUF:-0x2f025860} 4000 >> /root/gq/$L.pcs; i=$((i+1)); done
		cat /proc/interrupts > /root/gq/$L.irq1; grep -E '^(ctxt|intr|cpu)' /proc/stat > /root/gq/$L.stat1
		for th in /proc/$P/task/*; do echo "$(cat $th/comm) $(awk '{print $14,$15,$39}' $th/stat) $(grep ctxt $th/status | tr -s ' \t' ' ' | tr '\n' ' ')"; done > /root/gq/$L.thr1
		echo "PROF quake utime/stime $q0 -> $(awk '{print $14" "$15}' /proc/$P/stat) lvdesk $l0 -> $(awk '{print $14" "$15}' /proc/$LV/stat) samples $(wc -l < /root/gq/$L.pcs)"
		/root/s31pin -p 3 $LV	# the desktop's own affinity back (0-1)
		t=$((t + 4 * ${GQ_PROFN:-2}))
	fi
	grep -aqE '^ *[0-9]+ frames' $B/qconsole.log 2>/dev/null && break
done
T1=$(cut -d' ' -f1 /proc/uptime)
echo "WALL $T0 $T1"
echo "RESULT $(grep -aE '^ *[0-9]+ frames' $B/qconsole.log | head -1)"
echo "VM1 $(vm)"
echo "LV1 $(lvrep)"
echo "MEM1 $(grep -E 'MemAvailable|MemFree|SwapFree|CmaFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')"
[ -d /proc/$P ] && echo "FINAL $(awk '/^(VmRSS|VmSwap|VmData|VmHWM)/{printf "%s%s ",$1,$2}' /proc/$P/status) majflt=$(awk '{print $12}' /proc/$P/stat) cpu=$(awk '{print $14+$15}' /proc/$P/stat)"
[ -n "$GQ_HOLD" ] && sleep $GQ_HOLD
kill -9 $P 2>/dev/null
amixer -q sset DACL 143 2>/dev/null; amixer -q sset DACR 143 2>/dev/null
echo "--- log"; grep -aiE 'error|fail|hunk|megabyte|FOUND|not supported|disabled|gamma|sound|Video mode|GL_|frames|speed|warning' $B/qconsole.log | head -60
echo "--- out"; tail -n 15 /root/gq/$L.out
echo GQDONE
