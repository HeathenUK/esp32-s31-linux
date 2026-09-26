# regress-quick.sh - ON the board, after x11-compat-gate2 on the same fresh
# boot: windowed glxgears (gl-arm.sh, 5 x 10 s, the shipped /usr/bin/lvdesk)
# and one sdlquake timedemo demo1 (its menu invocation, fullscreen 320x240,
# sound on at DAC 110, restored to 143). Output /root/gq/regress-<label>.txt.
#   setsid sh regress-quick.sh <label> </dev/null >/dev/null 2>&1 &
L=$1; O=/root/gq/regress-$L.txt; mkdir -p /root/gq; exec > $O 2>&1
echo "REGRESS $L $(uname -v) up $(cut -d' ' -f1 /proc/uptime) lvdesk $(md5sum /usr/bin/lvdesk | cut -c1-8)"
sh /root/gl-arm.sh $L-win 1 2 win 5 /usr/bin/lvdesk
grep -a '^RUN' /root/glarm-$L-win.txt
for n in lvdesk glxgears; do killall $n 2>/dev/null; done; sleep 1
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid /usr/bin/lvdesk >/var/log/lvdesk.log 2>&1 </dev/null &
sleep 8
amixer -q sset DACL 110 2>/dev/null; amixer -q sset DACR 110 2>/dev/null
cd /root/quake
DISPLAY=:0 ./sdlquake -basedir /root/quake -winsize 320 240 -fullscreen +timedemo demo1 > /tmp/sq.out 2>&1 &
Q=$!; t=0
while [ $t -lt 200 ] && [ -d /proc/$Q ]; do sleep 5; t=$((t+5)); grep -aq 'frames' /tmp/sq.out && break; done
echo "SDLQUAKE $(grep -a 'frames' /tmp/sq.out | tail -1) after ${t}s"
kill $Q 2>/dev/null; sleep 1; kill -9 $Q 2>/dev/null
amixer -q sset DACL 143 2>/dev/null; amixer -q sset DACR 143 2>/dev/null
echo RQDONE
