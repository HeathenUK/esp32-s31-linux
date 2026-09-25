# lvdesk-route-test.sh - QoL A2 stacking-aware pointer routing, run ON the board.
#   BIN=/root/lvdesk.new sh lvdesk-route-test.sh
# xcalc runs with XLITE_TRACE_INPUT=1, so every event it receives is logged
# ("queue event type 4" = ButtonPress). Each check counts the presses xcalc got:
#  T1 right-click on the Terminal where it covers xcalc      -> want 0
#  T2 left click on the volume popover's label over xcalc   -> want 0
#  T3 wheel over an uncovered part of xcalc                 -> want >= 1
# Prints one RESULT line. ~70 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}
LOG=/var/log/lvdesk.log; XL=/tmp/xcalc-route.log
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
for p in $(pidof xcalc) $(pidof uinject); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 7
c() { echo "$1" > /tmp/lvdesk.ctl; sleep ${2:-1}; }
wid() { n=$(wc -l < $LOG); c list; tail -n +$((n+1)) $LOG | grep -a 'lvdesk: win ' | grep -a "$1" | tail -1 | awk '{print $3}'; }
bp() { grep -ac 'queue event type 4$' $XL; }
export DISPLAY=:0
setsid sh -c "XLITE_TRACE_INPUT=1 exec xcalc >$XL 2>&1" </dev/null >/dev/null 2>&1 &
sleep 6; x=$(wid xcalc); c "move $x 100 60" 1
c "run true" 2; t=$(wid Terminal); c "move $t 180 120" 2
n0=$(wc -l < $LOG); c list; tail -n +$((n0+1)) $LOG | grep -a 'lvdesk: win '
b0=$(bp); /root/uinject rclick 230 220 >/dev/null 2>&1; sleep 1; b1=$(bp)
c "close $t" 2
c "move $x 520 170" 2; c "tray audio" 2
/root/uinject click 575 390 >/dev/null 2>&1; sleep 1; b2=$(bp)
c "tray audio" 2
/root/uinject wheel 2 560 250 >/dev/null 2>&1; sleep 1; b3=$(bp)
for p in $(pidof xcalc); do kill -9 $p; done
t1=$((b1-b0)); t2=$((b2-b1)); t3=$((b3-b2))
ok=1; [ $t1 = 0 ] || ok=0; [ $t2 = 0 ] || ok=0; [ $t3 -ge 1 ] || ok=0
echo "RESULT bin=$BIN T1_rclick_on_terminal=$t1 T2_click_on_popover=$t2 T3_wheel_uncovered=$t3 ok=$ok"
