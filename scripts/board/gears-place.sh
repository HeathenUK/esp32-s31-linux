# gears-place.sh - ON the board: the fullscreen glxgears PLACEMENT lottery,
# sampled cheaply. K independent draws, each a fresh desktop (/usr/bin/lvdesk)
# and a fresh stock glxgears -fullscreen (shipped defaults); at 16 s it prints
# the CPU each is on (/proc/pid/stat field 39) and glxgears' own last two
# 5-second FPS lines, then kills both. Quiet during each draw: one sleep.
#   setsid sh gears-place.sh <label> [K=8] </dev/null >/dev/null 2>&1 &
# Output /root/gq/place-<label>.txt, ~27 s per draw. Fresh boot is the caller's.
L=$1; K=${2:-8}; mkdir -p /root/gq; exec > /root/gq/place-$L.txt 2>&1
echo "PLACE $L K=$K $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
i=1
while [ $i -le $K ]; do
	killall lvdesk glxgears 2>/dev/null; sleep 1; killall -9 lvdesk glxgears 2>/dev/null
	LVDESK_CTL=1 setsid /usr/bin/lvdesk >/var/log/lvdesk.log 2>&1 </dev/null &
	sleep 8
	LV=$(pidof lvdesk | awk '{print $1}')
	cd /root/gl2/bin
	setsid sh -c "exec env LD_LIBRARY_PATH=/root/gl2/lib ./glxgears -fullscreen >/tmp/place.out 2>&1" </dev/null >/dev/null 2>&1 &
	sleep 16
	C=$(pidof glxgears | awk '{print $1}')
	echo "D $i gears_cpu $(awk '{print $39}' /proc/$C/stat) lvdesk_cpu $(awk '{print $39}' /proc/$LV/stat) | $(grep -a FPS /tmp/place.out | tail -n 2 | tr '\n' ' ')"
	kill $C; sleep 1
	i=$((i+1))
done
echo "PIEB $(cat /sys/module/kernel/parameters/esp32s31_pie_bounces)"
echo PLDONE
