export DISPLAY=:0 HOME=/root/quake; cd /root/quake; rm -f gate.log
setsid sh -c 'XLITE_TRACE_INPUT=1 exec ./tyr-quake-x11 -basedir /root/quake -mem 20 -sndspeed 11025 -width 320 -height 240 -fullscreen +map e1m1 >/root/quake/gate.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 48; a=$(pidof tyr-quake-x11 | wc -w); L=$(pidof lvdesk); [ -z "$L" ] && L=$(pidof lvdesk.new); kill -USR1 $L; sleep 1; n0=$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}')
setsid sh -c '/root/uinject park' </dev/null >/dev/null 2>&1 & sleep 5; m=$(grep -ac 'xlite: motion' /root/quake/gate.log); for p in $(pidof uinject); do kill $p; done
setsid sh -c 'i=0; while [ $i -lt 4 ]; do /root/uinject key 17; i=$((i+1)); done' </dev/null >/dev/null 2>&1 & sleep 14; k=$(grep -ac 'queue event type 2$' /root/quake/gate.log)
kill -USR1 $L; sleep 1; n1=$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}'); echo "quake alive=$a puts=$((n1-n0)) motion=$m keys=$k"; for p in $(pidof tyr-quake-x11); do kill -9 $p; done
