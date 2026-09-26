# pielog-run.sh - ON the board (kernel with esp32s31_pie_log, the #394
# diagnostic): which calls put tasks through the lent-CPU PIE trap while
# stock glxgears runs fullscreen. Arms the kernel log for <n> traps, runs
# one quiet gears-swt.sh window, then saves the s31pie lines and every
# trapping process's maps (to resolve the pcs) to /root/gq/pielog-<label>.*
#   setsid sh pielog-run.sh <label> [n=150] [lvbin] </dev/null >/dev/null 2>&1 &
L=$1; N=${2:-150}; BIN=${3:-/usr/bin/lvdesk}
P=/sys/module/kernel/parameters/esp32s31_pie_log
[ -w $P ] || { echo "no $P - not the diagnostic kernel" > /root/gq/pielog-$L.txt; exit 1; }
dmesg -c >/dev/null
( sleep 14; echo $N > $P; g=$(pidof glxgears | awk '{print $1}'); echo "=== $g glxgears" > /root/gq/pielog-$L.gmaps; grep r-xp /proc/$g/maps >> /root/gq/pielog-$L.gmaps ) &
sh /root/gears-swt.sh $L 1500 300 $BIN
echo 0 > $P
dmesg | grep s31pie > /root/gq/pielog-$L.txt
for pid in $(sed -n 's/.*\[\([0-9]*\)\] pc.*/\1/p' /root/gq/pielog-$L.txt | sort -u); do
	[ -r /proc/$pid/maps ] && { echo "=== $pid $(cat /proc/$pid/comm)"; grep r-xp /proc/$pid/maps; }
done > /root/gq/pielog-$L.maps
echo PLDONE >> /root/gq/pielog-$L.maps
