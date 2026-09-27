# one fresh-boot arm: OpenTyrian title (ARM=stock|v2), then on the same boot the
# regression set with the matching preload setting (plain|v2) when REG=1.
# OpenTyrian window: 60 s from uptime ~100; regression starts at uptime 180.
ARM=${ARM:-stock}; TAG=${TAG:-1}; REG=${REG:-0}
# STOCK OpenTyrian (/root/oty/usr/bin/opentyrian, what the lvdesk menu starts) at its title screen, music playing,
# on a FRESH boot; arm = stock | v2 (LD_PRELOAD=libs31fp.so v2 candidate for
# THIS launch only - not installed). Audio thread CPU and xruns/s over a
# 20 s window, with nothing touching the board during it. Volume: the
# shipped level is left alone (the title music plays at DAC 143).
# Launch: detached; /root/afp2/tyr-<arm>-<tag>.txt after ~60 s.
cat > /root/afp2/tyr-inner.sh <<'IN'
ARM=$1; TAG=$2
OUT=/root/afp2/tyr-$ARM-$TAG.txt; LOG=/root/afp2/oty-$ARM-$TAG.log
read u _ < /proc/uptime; u=${u%.*}; [ $u -lt 75 ] && sleep $((75 - u))
for p in $(pidof opentyrian); do kill -9 $p; done; sleep 1
D=/root/oty/usr/share/opentyrian/data
cd $D
PRE=; [ $ARM = v2 ] && PRE="LD_PRELOAD=/root/afp2/libs31fp.so S31FP_CACHE=/root/afp2/cache5 S31FP_DEBUG=1"
env $PRE HOME=/root DISPLAY=:0 /root/oty/usr/bin/opentyrian --no-joystick </dev/null >$LOG 2>&1 &
sleep 25
PID=$(pidof opentyrian | cut -d' ' -f1)
{
echo "== arm $ARM tag $TAG pid $PID uname $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
grep -a "s31fp" $LOG
AT=$(for t in /proc/$PID/task/*; do n=${t##*/}; [ "$n" = "$PID" ] && continue; set -- $(cat $t/stat); echo "$((${14}+${15})) $n"; done | sort -n | tail -1 | cut -d" " -f2)
echo "audio_tid $AT"
x0=$(grep -a -c -i -E "occurred|underrun" $LOG); u0=$(cut -d" " -f1 /proc/uptime)
set -- $(cat /proc/$PID/task/$AT/stat); a0=$((${14}+${15}))
set -- $(cat /proc/$PID/stat); p0=$((${14}+${15}))
# 60 s window, xruns counted per 10 s (does it HOLD, not just the mean)
xp=$x0; B=""
for k in 1 2 3 4 5 6; do sleep 10; xk=$(grep -a -c -i -E "occurred|underrun" $LOG); B="$B $((xk - xp))"; xp=$xk; done
echo "xruns_per_10s$B  alive $([ -d /proc/$PID ] && echo yes || echo NO)"
x1=$(grep -a -c -i -E "occurred|underrun" $LOG); u1=$(cut -d" " -f1 /proc/uptime)
set -- $(cat /proc/$PID/task/$AT/stat); a1=$((${14}+${15}))
set -- $(cat /proc/$PID/stat); p1=$((${14}+${15}))
echo "$x0 $x1 $u0 $u1 $a0 $a1 $p0 $p1" | awk '{d=$4-$3; printf "RESULT xruns/s %.2f  audio_thread_cpu %.1f%%  process_cpu %.1f%%  window %.1fs\n", ($2-$1)/d, ($6-$5)/d, ($8-$7)/d, d}'
grep -E "VmRSS|RssAnon|RssFile" /proc/$PID/status | tr '\n' ' '; echo
awk '/^Private_Dirty/{d+=$2} /^Private_Clean/{c+=$2} END{print "MEM private_dirty_kB", d, "private_clean_kB", c}' /proc/$PID/smaps
grep -c libs31fp /proc/$PID/maps | sed 's/^/maps_libs31fp /'
grep -a -v -E "requested|obtained" $LOG | sort | uniq -c | sort -rn | head -4
kill -9 $PID
echo TYR_DONE
} > $OUT 2>&1
IN
setsid sh /root/afp2/tyr-inner.sh $ARM $TAG </dev/null >/dev/null 2>&1 &
echo TYR_STARTED
if [ "$REG" = 1 ]; then
if [ "$ARM" = v2 ]; then ARM=v2; else ARM=plain; fi
# Regression, one FRESH boot per arm: glxgears (windowed, 3 x 5 s reports),
# prboom timedemo demo1 (-window), sdlquake timedemo demo1 (its menu line,
# 320x240 fullscreen). arm = plain | v2 (LD_PRELOAD candidate, these launches
# only). Quiet: DAC at the floor (110) for the run, restored to 143; nothing
# polls during a measured window. /root/afp2/reg-<arm>.txt after ~4 min.
cat > /root/afp2/reg-inner.sh <<'IN'
ARM=$1; O=/root/afp2/reg-$ARM.txt; exec > $O 2>&1
# after the OpenTyrian arm on the same boot (done by ~125 s): ONE sleep, no polling
read u _ < /proc/uptime; u=${u%.*}; [ $u -lt ${REG_AT:-180} ] && sleep $((${REG_AT:-180} - u))
PRE=; [ $ARM = v2 ] && PRE="LD_PRELOAD=/root/afp2/libs31fp.so S31FP_CACHE=/root/afp2/cache5"
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
fi
