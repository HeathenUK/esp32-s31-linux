# lvdesk-kbdmenu-test.sh - QoL B1 Super tap and B2 keyboard-driven menus.
# Run ON the board:  BIN=/root/lvdesk.new sh lvdesk-kbdmenu-test.sh
#  K1 Super tap (150 ms) opens the app menu, row 0 highlighted; a 2 s hold does not
#  K2 Down/Up move the highlight; Right enters System; Left goes Back to the root
#  K3 System > Console by Down, Down, Enter, Down, Down, Enter: the console appears
#  K4 Esc closes an open menu
#  K5 typing with the menu open reaches no client (xcalc KeyPress count)
# Prints one RESULT line. ~80 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject; XL=/tmp/xcalc-km.log
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
pop() { n=$(wc -l < $LOG); echo pop > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: pop" | tail -1 | sed 's/lvdesk: pop //'; }
k() { for c in "$@"; do $U key $c >/dev/null 2>&1; done; }
$U hold 125 150 >/dev/null 2>&1; sleep 1; a=$(pop)
k 1; sleep 1; $U hold 125 2000 >/dev/null 2>&1; sleep 1; b=$(pop)
echo "K1 tap: $a | long hold: $b"
$U hold 125 150 >/dev/null 2>&1; k 108; c1=$(pop); k 108; c2=$(pop); k 103; c3=$(pop)
k 108 106; c4=$(pop); k 105; c5=$(pop)
echo "K2 down: $c1 | down: $c2 | up: $c3 | right into System: $c4 | left back: $c5"
k 1; sleep 1; $U hold 125 150 >/dev/null 2>&1; k 108 108 28; sleep 1; k 108 108 28; sleep 2
n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; con=$(tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a Terminal)
echo "K3 console: $con"
echo "console hide" > /tmp/lvdesk.ctl; sleep 1
$U hold 125 150 >/dev/null 2>&1; k 1; d=$(pop); echo "K4 esc: $d"
export DISPLAY=:0; setsid sh -c "XLITE_TRACE_INPUT=1 exec xcalc >$XL 2>&1" </dev/null >/dev/null 2>&1 &
sleep 6; e0=$(grep -ac 'queue event type 2$' $XL); $U hold 125 150 >/dev/null 2>&1; k 2 3 4; e1=$(grep -ac 'queue event type 2$' $XL); k 1; sleep 1
k 2; e2=$(grep -ac 'queue event type 2$' $XL)
for p in $(pidof xcalc); do kill -9 $p; done
echo "K5 keys to xcalc with menu open: $((e1-e0)) | after it closed: $((e2-e1))"
ok=1
echo "$a" | grep -q "^open rows=3 sel=0 cur=-1 menu=1" || ok=0
echo "$b" | grep -q "^closed" || ok=0
echo "$c1" | grep -q "sel=1" && echo "$c2" | grep -q "sel=2" && echo "$c3" | grep -q "sel=1" || ok=0
echo "$c4" | grep -q "open.*sel=0 cur=[0-9]" || ok=0
echo "$c5" | grep -q "open.*sel=0 cur=-1" || ok=0
echo "$con" | grep -q "FOCUS.* CON" || ok=0
echo "$d" | grep -q "^closed" || ok=0
[ $((e1-e0)) = 0 ] && [ $((e2-e1)) -ge 1 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
