# fs-present-probe.sh - ON the board: what a fullscreen GL present costs the
# desktop, stage by stage, over one continuous window.
#   setsid sh fs-present-probe.sh <label> <client: gears|quake> [secs] [prof 0|1] [lvbin]
# Starts the desktop with LVDESK_FSGSTAGE=1 (per-stage present timers: expand,
# dirty = the DIRTYFB/PRESENT ioctl, present) plus $LVENV, starts the client
# fullscreen (gears: stock glxgears from /root/gl2, render scale on, 2 SHM
# buffers - the shipped defaults; quake: the tuned GLQuake timedemo), waits
# for it to settle, then takes SIGUSR1 reports before and after a continuous
# window of <secs> (default 20). The report carries the dirty-time histogram,
# the worst dirty and present, and the long-frame list (>= 30 ms, with the
# desktop's major faults in that frame). prof=1 also pins both processes to
# CPU0 for 8 s in the middle and takes 2 x 4000 h1s samples plus both maps.
# Output: /root/gq/fp-<label>.txt (+ .pcs .cmaps .lmaps).
L=$1; CL=$2; W=${3:-20}; PR=${4:-0}; BIN=${5:-/usr/bin/lvdesk}
O=/root/gq/fp-$L.txt; mkdir -p /root/gq; exec > $O 2>&1
echo "FP $L client=$CL secs=$W prof=$PR bin=$BIN lvenv=${LVENV:-} $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
killall lvdesk lvdesk.new glxgears quakespasm 2>/dev/null; sleep 1
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
[ -n "${LVENV:-}" ] && export $LVENV
LVDESK_CTL=1 LVDESK_FSGSTAGE=1 setsid $BIN >/var/log/lvdesk.log 2>&1 </dev/null &
sleep 8
LV=$(pidof $(basename $BIN))
amixer -q sset DACL 110 2>/dev/null; amixer -q sset DACR 110 2>/dev/null
if [ "$CL" = gears ]; then
	cd /root/gl2/bin; LD_LIBRARY_PATH=/root/gl2/lib setsid ./glxgears -fullscreen >/tmp/fp.out 2>&1 </dev/null &
	sleep 6; C=$(pidof glxgears)
else
	cd /root/quake/td; DISPLAY=:0 HOME=/root/quake setsid /root/quake/quakespasm -basedir /root/quake/td -condebug -mixspeed 11025 -heapsize 12288 -zone 384 -width 320 -height 240 -fullscreen >/tmp/fp.out 2>&1 </dev/null &
	sleep 60; C=$(pidof quakespasm)
fi
rep() { kill -USR1 $LV; sleep 1; grep -a "^lvdesk: frames\|^lvdesk: present us\|^lvdesk: dirty ms\|^lvdesk: alias\|^lvdesk: long frames\|MIT-SHM ShmPutImage" /var/log/lvdesk.log | tail -n 6; }
t() { awk '{print $12, $14+$15}' /proc/$1/stat; }
[ -n "${FP_PIN:-}" ] && /root/s31pin -p $FP_PIN $C	# whole-window client affinity (diagnosis)
[ -n "${FP_LVPIN:-}" ] && /root/s31pin -p $FP_LVPIN $LV
PS=/sys/module/esp32s31_lcd/parameters/present_stats; [ -w $PS ] && echo 1 > $PS
echo fsgreset > /tmp/lvdesk.ctl; sleep 1	# the window only (lvdesk ctl, 2026-09-26)
echo "--- before"; rep
grep riscv-timer /proc/interrupts; echo "LV0 $(t $LV) CL0 $(t $C) $(cut -d' ' -f1 /proc/uptime)"
if [ "$PR" = 1 ]; then
	sleep $((W / 2 - 4))
	/root/s31pin -p ${FP_CLMASK:-1} $C; /root/s31pin -p 1 $LV; sleep 1
	cat /proc/$C/maps > /root/gq/fp-$L.cmaps; cat /proc/$LV/maps > /root/gq/fp-$L.lmaps
	/root/h1s 0x2f030c1c 0x2f025860 4000 > /root/gq/fp-$L.pcs; /root/h1s 0x2f030c1c 0x2f025860 4000 >> /root/gq/fp-$L.pcs
	/root/s31pin -p 3 $C; /root/s31pin -p 3 $LV
	sleep $((W / 2 - 5))
else
	sleep $W
fi
echo "LV1 $(t $LV) CL1 $(t $C) $(cut -d' ' -f1 /proc/uptime)"; grep riscv-timer /proc/interrupts
echo "--- after"; rep
[ -r $PS ] && { echo "--- driver"; cat $PS; }
killall glxgears quakespasm 2>/dev/null
amixer -q sset DACL 143 2>/dev/null; amixer -q sset DACR 143 2>/dev/null
echo FPDONE
