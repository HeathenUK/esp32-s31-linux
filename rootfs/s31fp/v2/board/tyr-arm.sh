# STOCK OpenTyrian (/root/oty/usr/bin/opentyrian, what the lvdesk menu starts) at its title screen, music playing,
# on a FRESH boot; arm = stock | v2 (LD_PRELOAD=libs31fp.so v2 candidate for
# THIS launch only - not installed). Audio thread CPU and xruns/s over a
# 20 s window, with nothing touching the board during it. Volume: the
# shipped level is left alone (the title music plays at DAC 143).
# Launch: detached; /root/afp2/tyr-<arm>-<tag>.txt after ~60 s.
ARM=${ARM:-stock}; TAG=${TAG:-1}
cat > /root/afp2/tyr-inner.sh <<'IN'
ARM=$1; TAG=$2
OUT=/root/afp2/tyr-$ARM-$TAG.txt; LOG=/root/afp2/oty-$ARM-$TAG.log
read u _ < /proc/uptime; u=${u%.*}; [ $u -lt 75 ] && sleep $((75 - u))
for p in $(pidof opentyrian); do kill -9 $p; done; sleep 1
D=/root/oty/usr/share/opentyrian/data
cd $D
PRE=; [ $ARM = v2 ] && PRE="LD_PRELOAD=/root/afp2/libs31fp.so S31FP_CACHE=/root/afp2/cache S31FP_DEBUG=1"
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
sleep 20
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
