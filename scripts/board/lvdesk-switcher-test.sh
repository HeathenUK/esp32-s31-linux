# lvdesk-switcher-test.sh - QoL B4 Alt/Super+Tab switcher. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-switcher-test.sh [hold]
# st, xclock, xcalc; xclock minimised with Super+H. With "hold", leaves an
# Alt+Tab held open for 8 s in the background (for a host screenshot) and
# exits. Otherwise prints one RESULT line. ~60 s. Not on a board in use.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc) $(pidof st) $(pidof xclock); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0
for a in st xclock xcalc; do setsid sh -c "exec $a >/dev/null 2>&1" </dev/null >/dev/null 2>&1 & sleep 5; done
foc() { n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a " FOCUS" | awk '{print $NF}'; }
st_of() { n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a " $1\$" | sed 's/lvdesk: win [0-9]* //'; }
xi=$(n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a " xclock$" | awk '{print $3}')
echo "raise $xi" > /tmp/lvdesk.ctl; sleep 1; $U chord 125 0 35 >/dev/null 2>&1; sleep 1
if [ "${1:-}" = hold ]; then
	setsid sh -c "$U chord 56 8000 15 >/dev/null 2>&1" </dev/null >/dev/null 2>&1 &
	exit 0
fi
f0=$(foc); $U chord 56 0 15 1 >/dev/null 2>&1; sleep 1; f1=$(foc)
$U chord 56 0 15 108 28 >/dev/null 2>&1; sleep 1; f2=$(foc); x2=$(st_of xclock)
$U altkey 15 >/dev/null 2>&1; sleep 1; f3=$(foc)
for p in $(pidof xcalc) $(pidof st) $(pidof xclock); do kill -9 $p; done
echo "S1 focus before: $f0 | after Alt+Tab,Esc: $f1"
echo "S2 Alt+Tab,Down,Enter: focus $f2 | xclock: $x2"
echo "S3 plain Alt+Tab: focus $f3"
ok=1; [ "$f0" = "$f1" ] || ok=0; [ "$f2" = xclock ] || ok=0
echo "$x2" | grep -q "MIN\|HID" && ok=0; [ -n "$f3" ] && [ "$f3" != "$f2" ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
