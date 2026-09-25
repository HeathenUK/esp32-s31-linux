# lvdesk-winmem-test.sh - QoL D5 window geometry memory. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-winmem-test.sh
#  W1 drag xcalc by its title bar, close -> an xcalc line in /etc/lvdesk/winpos
#  W2 relaunch xcalc -> opens where it was dragged (a place fully on the
#     panel: one partly off it is clamped back on, as the cascade is)
#  W3 xclock moved only through ctl, closed -> NO xclock line
#  W4 `winmem off`, relaunch xcalc -> the cascade (150,60)
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject; F=/etc/lvdesk/winpos
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc) $(pidof xclock); do kill -9 $p; done; rm -f $F
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_PTR_ACCEL=0 LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0
geo() { n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a " $1\$" | awk '{print $4}'; }
idx() { n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a " $1\$" | awk '{print $3}'; }
setsid sh -c 'exec xcalc >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6; g0=$(geo xcalc)
printf 'dragto 250 70 450 70\nsleep 600\n' | $U script >/dev/null 2>&1; g1=$(geo xcalc)
echo "close $(idx xcalc)" > /tmp/lvdesk.ctl; sleep 7; w1=$(grep -a "^xcalc " $F)
setsid sh -c 'exec xcalc >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6; g2=$(geo xcalc); for p in $(pidof xcalc); do kill -9 $p; done; sleep 2
setsid sh -c 'exec xclock >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 5; echo "move $(idx xclock) 300 300" > /tmp/lvdesk.ctl; sleep 1; echo "close $(idx xclock)" > /tmp/lvdesk.ctl; sleep 7
w3=$(grep -a "^xclock " $F | wc -l)
echo "winmem off" > /tmp/lvdesk.ctl; sleep 1
setsid sh -c 'exec xcalc >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6; g4=$(geo xcalc); for p in $(pidof xcalc) $(pidof xclock); do kill -9 $p; done
echo "W1 xcalc opened $g0, dragged to $g1, closed -> winpos: '$w1'"
echo "W2 relaunched at $g2"
echo "W3 xclock lines after a ctl-only move: $w3"
echo "W4 winmem off: $g4"
ok=1; [ -n "$w1" ] || ok=0; [ "$g2" = "$g1" ] || ok=0; [ "$w3" = 0 ] || ok=0; [ "$g4" = "$g0" ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
