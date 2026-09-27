# T2: stock OpenTyrian at its title screen (music playing), one arm per run.
# arm: stock | s31fp | s31fp-place | stock-place | s31fp-nice
#  s31fp   = LD_PRELOAD libs31fp.so for THIS measurement only (not shipped)
#  place   = audio thread -> CPU0 (hart 1) alone; every other thread of the
#            game and lvdesk -> CPU1 (the lent hart)
#  nice    = audio thread renice -10
# Detached; results in /root/afp/tyr-<arm>.txt after ~50 s. Volume untouched.
ARM=$1
cat > /root/afp/tyr-inner.sh <<'IN'
ARM=$1
OUT=/root/afp/tyr-$ARM.txt
LOG=/root/afp/oty-$ARM.log
for p in $(ps | awk '/[o]pentyrian/ {print $1}'); do kill -9 $p; done
sleep 1
cd /root/oty/usr/share/opentyrian/data
case $ARM in s31fp*) export LD_PRELOAD=/root/afp/libs31fp.so ;; esac
HOME=/root DISPLAY=:0 /root/oty/usr/bin/opentyrian --no-joystick </dev/null >$LOG 2>&1 &
unset LD_PRELOAD
sleep 25
PID=$(ps | awk '/[o]pentyrian/ {print $1}' | head -1)
L=$(ps | awk '/[l]vdesk/ {print $1}' | head -1)
{
echo "== arm $ARM pid $PID lvdesk $L uname $(uname -v)"
grep -a -E "requested|obtained" $LOG
# the audio thread: the busiest thread that is not the main one
AT=$(for t in /proc/$PID/task/*; do n=${t##*/}; [ "$n" = "$PID" ] && continue; set -- $(cat $t/stat); echo "$((${14}+${15})) $n"; done | sort -n | tail -1 | cut -d" " -f2)
echo "audio_tid $AT"
case $ARM in *place*)
  for t in /proc/$PID/task/* /proc/$L/task/*; do /root/afp/oncpu -s 2 ${t##*/}; done
  /root/afp/oncpu -s 1 $AT ;;
esac
case $ARM in *nice*) renice -n -10 -p $AT ;; esac
sleep 2
echo "bounces0 $(cat /sys/module/kernel/parameters/esp32s31_pie_bounces 2>/dev/null)"
echo "xruns0 $(grep -a -c -i -E "occurred|underrun" $LOG)"
echo "up0 $(cut -d" " -f1 /proc/uptime)"
for t in /proc/$PID/task/* /proc/$L/task/*; do echo "S0 $(cat $t/stat)"; done
sleep 15
echo "up1 $(cut -d" " -f1 /proc/uptime)"
for t in /proc/$PID/task/* /proc/$L/task/*; do echo "S1 $(cat $t/stat)"; done
echo "xruns1 $(grep -a -c -i -E "occurred|underrun" $LOG)"
echo "bounces1 $(cat /sys/module/kernel/parameters/esp32s31_pie_bounces 2>/dev/null)"
for t in /proc/$PID/task/*; do /root/afp/oncpu -p ${t##*/}; done
cat /proc/asound/card0/pcm0p/sub0/hw_params
grep -a -v -E "requested|obtained" $LOG | sort | uniq -c | sort -rn | head -5
kill -9 $PID
for t in /proc/$L/task/*; do /root/afp/oncpu -s 3 ${t##*/}; done
echo TYR_DONE
} > $OUT 2>&1
IN
setsid sh /root/afp/tyr-inner.sh $ARM </dev/null >/dev/null 2>&1 &
echo TYR_STARTED
