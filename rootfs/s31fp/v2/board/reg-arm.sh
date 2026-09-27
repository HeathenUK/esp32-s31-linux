# Regression, one FRESH boot per arm: glxgears (windowed, 3 x 5 s reports),
# prboom timedemo demo1 (-window), sdlquake timedemo demo1 (its menu line,
# 320x240 fullscreen). arm = plain | v2 (LD_PRELOAD candidate, these launches
# only). Quiet: DAC at the floor (110) for the run, restored to 143; nothing
# polls during a measured window. /root/afp2/reg-<arm>.txt after ~4 min.
ARM=${ARM:-plain}
cat > /root/afp2/reg-inner.sh <<'IN'
ARM=$1; O=/root/afp2/reg-$ARM.txt; exec > $O 2>&1
# after the OpenTyrian arm on the same boot (done by ~125 s): ONE sleep, no polling
read u _ < /proc/uptime; u=${u%.*}; [ $u -lt ${REG_AT:-140} ] && sleep $((${REG_AT:-140} - u))
PRE=; [ $ARM = v2 ] && PRE="LD_PRELOAD=/root/afp2/libs31fp.so S31FP_CACHE=/root/afp2/cache"
echo "== arm $ARM uname $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
amixer -q sset DACL 110 2>/dev/null; amixer -q sset DACR 110 2>/dev/null
G=/root/gl2/bin/glxgears	# stock glxgears; the shipped libGL from /usr/lib
echo "glxgears $G"
cd /root/gl2/bin && env $PRE DISPLAY=:0 $G > /tmp/gg.out 2>&1 &
GP=$!; sleep 17; kill $GP 2>/dev/null; sleep 1; kill -9 $GP 2>/dev/null
grep -a FPS /tmp/gg.out | sed 's/^/GLXGEARS /'
cd /root/doom && env $PRE DISPLAY=:0 ./prboom -width 320 -height 200 -window -timedemo demo1 > /tmp/td.log 2>&1 &
PB=$!; t=0; while [ -d /proc/$PB ] && [ $t -lt 150 ]; do sleep 5; t=$((t+5)); done
kill -9 $PB 2>/dev/null
echo "PRBOOM after ${t}s $(grep -a -i 'timed' /tmp/td.log | tail -1)"
cd /root/quake && env $PRE DISPLAY=:0 ./sdlquake -basedir /root/quake -winsize 320 240 -fullscreen +timedemo demo1 > /tmp/sq.out 2>&1 &
Q=$!; sleep 100
grep -aq frames /tmp/sq.out || sleep 60
echo "SDLQUAKE $(grep -a 'frames' /tmp/sq.out | tail -1)"
kill $Q 2>/dev/null; sleep 1; kill -9 $Q 2>/dev/null
amixer -q sset DACL 143 2>/dev/null; amixer -q sset DACR 143 2>/dev/null
dmesg | grep -i -E "segfault|unhandled|oops|BUG" | tail -3
echo REG_DONE
IN
setsid sh /root/afp2/reg-inner.sh $ARM </dev/null >/dev/null 2>&1 &
echo REG_STARTED
