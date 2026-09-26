uname -v; cat /sys/devices/system/cpu/online 2>/dev/null
for p in $(ps | awk "/prboom/ && !/awk/ {print \$1}"); do kill -9 $p 2>/dev/null; done
rm -f /root/doom/td.log /root/doom/td.pre
dmesg -n 4
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
: > /root/doom/td-pre.sh
setsid sh -c 'while read u _ < /proc/uptime; [ ${u%.*} -lt 75 ]; do sleep 1; done; sh /root/doom/td-pre.sh > /root/doom/td.pre 2>&1; cd /root/doom; DISPLAY=:0 exec /root/doom/prboom -width 320 -height 200 -window -timedemo demo1 > /root/doom/td.log 2>&1' </dev/null >/dev/null 2>&1 &
echo TD_LAUNCHED
