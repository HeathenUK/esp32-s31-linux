# lvdesk-term-hammer.sh - QoL A0 terminal close lifecycle, run ON the board.
#   BIN=/root/lvdesk.new sh lvdesk-term-hammer.sh
# Restarts the desktop on $BIN (default the shipped /usr/bin/lvdesk), then:
#  1 close the Terminal while `yes | head -c 2000000` streams into it
#  2 `run echo again` must reopen a Terminal, with exactly one sh child
#  3 ten close/reopen cycles, lvmem used printed each time (must stay flat)
#  4 `run xcalc &` then `run exit`: the Terminal closes, xcalc survives
# Prints one RESULT line. ~60 s.
BIN=${BIN:-/usr/bin/lvdesk}
LOG=/var/log/lvdesk.log
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
for p in $(pidof xcalc); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
L=$(pidof $(basename $BIN))
c() { echo "$1" > /tmp/lvdesk.ctl; sleep ${2:-1}; }
tid() { n=$(wc -l < $LOG); c list; tail -n +$((n+1)) $LOG | grep -a 'lvdesk: win ' | grep -a 'Terminal' | tail -1 | awk '{print $3}'; }
shkids() { k=0; for s in /proc/[0-9]*/status; do pp=$(awk '/^PPid/{print $2}' $s 2>/dev/null); nm=$(awk '/^Name/{print $2}' $s 2>/dev/null); [ "$pp" = "$L" ] && [ "$nm" = sh ] && k=$((k+1)); done; echo $k; }
used() { n=$(wc -l < $LOG); c lvmem; tail -n +$((n+1)) $LOG | grep -a 'lvmem total' | tail -1 | sed 's/.*max_used \([0-9]*\).*/\1/'; }
alive() { kill -0 $L 2>/dev/null && echo 1 || echo 0; }
c "run true" 2; t=$(tid); echo "open id=$t shkids=$(shkids)"
c "run yes | head -c 2000000" 0; sleep 1; c "close $t" 3
a1=$(alive); echo "after-stream-close alive=$a1 shkids=$(shkids)"
c "run echo again" 3; t=$(tid); k2=$(shkids); a2=$(alive); echo "reopen id=$t shkids=$k2 alive=$a2"
i=0; m=""; while [ $i -lt 10 ]; do t=$(tid); [ -n "$t" ] && c "close $t" 1; c "run true" 1; m="$m $(awk '/MemAvailable/{print $2}' /proc/meminfo)"; i=$((i+1)); done
a3=$(alive); echo "cycles alive=$a3 shkids=$(shkids) MemAvailable_kB:$m"
c "run xcalc &" 7; c "run exit" 3; t=$(tid); x=$(pidof xcalc | wc -w); a4=$(alive); echo "exit: terminal=${t:-gone} xcalc=$x alive=$a4"
for p in $(pidof xcalc); do kill -9 $p; done
ok=1; [ "$a1$a2$a3$a4" = 1111 ] || ok=0; [ "$k2" = 1 ] || ok=0; [ -z "$t" ] || ok=0; [ "$x" = 1 ] || ok=0
grep -a -i "segfault\|assert\|error" $LOG | head -3
echo "RESULT bin=$BIN ok=$ok"
