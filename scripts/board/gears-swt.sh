# gears-swt.sh - ON the board: per-frame client timing of stock glxgears
# fullscreen (render scale, 2 SHM buffers - the shipped defaults), with the
# desktop's own long-frame list for the same window.
#   setsid sh gears-swt.sh <label> [frames] [skip=300] [lvbin] </dev/null >/dev/null 2>&1 &
# rootfs/swapstamp.so (LD_PRELOAD, diagnostic only) stamps every
# glXSwapBuffers (the desktop's stage timers only with SWT_FSGSTAGE=1: their
# per-long-frame /proc/vmstat strstr was a PIE trap of its own): render gap, swap time, CPU time in each, glClear time,
# the CPU it ran on, context switches and faults per frame, and lvdesk's
# ticks/CPU/state per frame (SWT_PEER). Around the window: /proc/stat,
# /proc/interrupts and /proc/vmstat snapshots, per-task migrations
# (swm0/swm1, swt-migr.py) and, with SWT_CS=1, cpushare.sh (per-task CPU), and the kernel's PIE-bounce count per frame (SWT_CTR). Output /root/gq/swt-<label>.txt (frames) and /root/gq/swc-<label>.txt
# (context). $SWT_PRE is preloaded into glxgears ahead of swapstamp and
# gets a SIGUSR2 before the kill (rootfs/nopie.so dumps on it). $SWT_PIN / $SWT_LVPIN pin glxgears / lvdesk (s31pin mask).
L=$1; N=${2:-2000}; SK=${3:-300}; BIN=${4:-/usr/bin/lvdesk}
O=/root/gq/swc-$L.txt; mkdir -p /root/gq; exec > $O 2>&1
echo "SWT $L frames=$N skip=$SK bin=$BIN lvenv=${LVENV:-} glenv=${GLENV:-} $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
for n in lvdesk lvdesk.new glxgears; do killall $n 2>/dev/null; done; sleep 1
for n in lvdesk lvdesk.new glxgears; do killall -9 $n 2>/dev/null; done; sleep 1
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
[ -n "${LVENV:-}" ] && export $LVENV
LVDESK_CTL=1 ${SWT_FSGSTAGE:+LVDESK_FSGSTAGE=1} setsid $BIN >/var/log/lvdesk.log 2>&1 </dev/null &
sleep 8
LV=$(pidof $(basename $BIN) | awk '{print $1}')
echo "LV $LV $(pidof lvdesk lvdesk.new)"
amixer -q sset DACL 0 2>/dev/null; amixer -q sset DACR 0 2>/dev/null
[ -n "${SWT_LVPIN:-}" ] && /root/s31pin -p $SWT_LVPIN $LV
cd /root/gl2/bin
rm -f /root/gq/swt-$L.txt
setsid sh -c "exec env ${GLENV:-} LD_PRELOAD=\"${SWT_PRE:-} /root/swapstamp.so\" SWT_CTR=/sys/module/kernel/parameters/esp32s31_pie_bounces SWT_PEER=$LV SWT_N=$N SWT_SKIP=$SK SWT_OUT=/root/gq/swt-$L.txt LD_LIBRARY_PATH=/root/gl2/lib ./glxgears -fullscreen >/tmp/swt.out 2>&1" </dev/null >/dev/null 2>&1 &
sleep 3; C=$(pidof glxgears | awk '{print $1}')
[ -n "${SWT_PIN:-}" ] && /root/s31pin -p $SWT_PIN $C
echo fsgreset > /tmp/lvdesk.ctl
echo "--- before $(cut -d' ' -f1 /proc/uptime)"
grep '^cpu' /proc/stat; cat /proc/interrupts; grep -E 'pgscan|pgsteal|allocstall|pswp|pgmajfault|compact_stall|workingset_refault' /proc/vmstat
grep -H -e nr_migrations /proc/[0-9]*/task/[0-9]*/sched > /root/gq/swm0-$L.txt 2>/dev/null
for f in /proc/[0-9]*/task/[0-9]*/comm; do read c < $f; echo "$f $c"; done > /root/gq/swn-$L.txt 2>/dev/null
echo "PIEB0 $(cat /sys/module/kernel/parameters/esp32s31_pie_bounces)"
# QUIET during the window. busybox sh is a PIE user (musl strcmp): a shell
# loop, cpushare.sh's /proc walk or a runsh poll landing on the lent CPU
# bounces to CPU0 and makes the very clusters being measured (cpushare's sh
# alone made 259 of one window's 368 bounces). So the recorded frames start
# after the snapshots above (skip >= 300), the wait is ONE sleep, and
# cpushare.sh runs only with SWT_CS=1.
CS=""
[ -n "${SWT_CS:-}" ] && { sh /root/cpushare.sh 40 /root/gq/swcs-$L.txt & CS=$!; }
sleep $((N / 30 + SK / 30 + 4))
i=0; while [ ! -s /root/gq/swt-$L.txt ] && [ $i -lt 120 ]; do sleep 5; i=$((i+5)); done
echo "--- after $(cut -d' ' -f1 /proc/uptime) waited $i"
echo "PIEB1 $(cat /sys/module/kernel/parameters/esp32s31_pie_bounces)"
grep -H -e nr_migrations /proc/[0-9]*/task/[0-9]*/sched > /root/gq/swm1-$L.txt 2>/dev/null
grep '^cpu' /proc/stat; cat /proc/interrupts; grep -E 'pgscan|pgsteal|allocstall|pswp|pgmajfault|compact_stall|workingset_refault' /proc/vmstat
kill -USR1 $LV; sleep 1
grep -a "^lvdesk: frames\|^lvdesk: present us\|^lvdesk: dirty ms\|^lvdesk: long frames\|MIT-SHM ShmPutImage" /var/log/lvdesk.log | tail -n 5
[ -n "$CS" ] && { wait $CS; echo "--- cpushare"; cat /root/gq/swcs-$L.txt; }
[ -n "${SWT_PRE:-}" ] && { kill -USR2 $C; sleep 1; }
kill $C; sleep 1; kill -9 $C 2>/dev/null
tail -n 4 /tmp/swt.out
amixer -q sset DACL 143 2>/dev/null; amixer -q sset DACR 143 2>/dev/null
echo SWTDONE
