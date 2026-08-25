#!/bin/sh
# Deterministic desktop interactivity harness.
#   $1 = clock  : "on" restores the taskbar clock, "off" removes it
# Window geometry is fixed so drag/raise always hit the same targets. Without
# this the pointer lands on whatever happens to be there and the numbers are
# measuring the layout, not the system.
CLOCK=${1:-on}
# Preflight. busybox has no pkill, and when a sweep called it the shell printed
# "pkill: not found" to stderr and carried on - jwm was never restarted, every
# arm ran the same config, and the numbers looked entirely plausible. A missing
# tool must abort the run, not quietly void it.
for t in killall pidof xterm xcalc jwm xsetroot; do
  command -v $t >/dev/null 2>&1 || { echo "PREFLIGHT FAIL: $t missing"; exit 1; }
done
[ -x /root/deskbench ] || { echo "PREFLIGHT FAIL: /root/deskbench missing"; exit 1; }
mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e /etc/system.jwmrc.bak ] && cp /etc/system.jwmrc.bak /etc/system.jwmrc
if [ "$CLOCK" = off ]; then
  sed -i '/<Swallow width="32" height="32" name="xclock">/d' /etc/system.jwmrc
  sed -i '/<Clock format=/d' /etc/system.jwmrc
fi
/etc/init.d/xorg start >/dev/null 2>&1
i=0; while [ $i -lt 40 ]; do [ -e /tmp/.X11-unix/X0 ] && break; sleep 1; i=$((i+1)); done
sleep 10
export DISPLAY=:0
xsetroot -cursor_name left_ptr
pidof jwm >/dev/null || { jwm >/dev/null 2>&1 & }
sleep 6
xterm -geometry 44x12+16+48 >/dev/null 2>&1 & sleep 7
xcalc -geometry +330+60      >/dev/null 2>&1 & sleep 6
xterm -geometry 40x8+40+250   >/dev/null 2>&1 & sleep 7
cj() { for p in $(pidof xterm xcalc jwm xclock); do awk '{print $14+$15}' /proc/$p/stat; done | awk '{s+=$1} END{print s+0}'; }
q=0
for w in 1 2 3 4 5 6 7 8; do A=$(cj); sleep 5; B=$(cj); D=$((B-A)); [ $D -lt 30 ] && q=$((q+1)) || q=0; [ $q -ge 2 ] && break; done
# Record the scanout mode with every run. Some earlier runs were in scaled
# 800x480 and others in unscaled 640x384 after a CMA allocation failure, which
# is different work per repaint and silently voids cross-run comparisons.
echo "SCANOUT $(dmesg | grep -a 'scanout started' | tail -1 | sed 's/.*: "//')"
dmesg | grep -aq 'scaling off' && echo "WARNING: scaling was dropped this boot - results not comparable"
echo "ARM clock=$CLOCK clients=$(pidof jwm xterm xcalc | wc -w) xclock=$(pidof xclock|wc -w) MemAvail=$(awk '/^MemAvailable/{print $2}' /proc/meminfo) settle_w=$w"
U=/sys/kernel/debug/esp32s31_lcd/updates
A=$(tr ' ' '\n' < $U | grep '^updates=' | cut -d= -f2); sleep 5
B=$(tr ' ' '\n' < $U | grep '^updates=' | cut -d= -f2)
echo "  idle_repaints_per_5s=$((B-A))"
for s in move click key drag raise menu; do
  case $s in raise) n=8;; menu) n=6;; *) n=10;; esac
  /root/deskbench $s $n 3 2>&1 | grep -E '^  (first|settle)' | sed "s/^/$s/"
done
/root/deskbench dragfps 40 3 2>&1 | grep -E 'fps|gap'
