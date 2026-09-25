# lvdesk-touch-test.sh - two-finger tap = right click, one-finger taps intact.
# Run ON the board:  BIN=/root/lvdesk.new sh lvdesk-touch-test.sh
# Uses uinject's synthetic touchscreen (tap / tap2 / taphold). xcalc runs with
# XLITE_TRACE_INPUT=1; "type 4" is ButtonPress, "type 5" ButtonRelease.
#  T1 tap on xcalc              -> 1 press, 1 release (a left click)
#  T2 taphold 300 ms on xcalc   -> 1 press, 1 release
#  T3 tap2 on xcalc             -> 1 press, 1 release, and it is Button3
#                                  (LVDESK_AIMDBG "btn3"), not a left press
#  T4 tap2 on the bare desktop  -> the app menu opens (right-click on desk)
# Prints one RESULT line. ~60 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; XL=/tmp/xcalc-touch.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
for p in $(pidof xcalc); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_AIMDBG=1 LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0
setsid sh -c "XLITE_TRACE_INPUT=1 exec xcalc >$XL 2>&1" </dev/null >/dev/null 2>&1 &
sleep 6
pr() { grep -ac 'queue event type 4$' $XL; }; rl() { grep -ac 'queue event type 5$' $XL; }
# a point on xcalc's display strip (inside the client, clear of its buttons)
X=260; Y=110
p0=$(pr); r0=$(rl); $U tap $X $Y >/dev/null 2>&1; sleep 1; p1=$(pr); r1=$(rl)
$U taphold $X $Y 300 >/dev/null 2>&1; sleep 1; p2=$(pr); r2=$(rl)
b3a=$(grep -ac "btn3 at" $LOG)
$U tap2 $X $Y >/dev/null 2>&1; sleep 1; p3=$(pr); r3=$(rl)
b3b=$(grep -ac "btn3 at" $LOG); tf=$(grep -ac "two-finger tap" $LOG)
for p in $(pidof xcalc); do kill -9 $p; done; sleep 1
$U tap2 600 250 >/dev/null 2>&1; sleep 1
python_menu=$(grep -ac "two-finger tap" $LOG)
echo "T1 tap press=$((p1-p0)) release=$((r1-r0))"
echo "T2 hold press=$((p2-p1)) release=$((r2-r1))"
echo "T3 tap2 press=$((p3-p2)) release=$((r3-r2)) btn3_dispatches=$((b3b-b3a)) gesture_logs=$tf"
echo "T4 desktop gestures=$python_menu"
ok=1
[ "$((p1-p0))$((r1-r0))" = 11 ] || ok=0
[ "$((p2-p1))$((r2-r1))" = 11 ] || ok=0
[ "$((p3-p2))$((r3-r2))" = 11 ] && [ $((b3b-b3a)) -ge 1 ] || ok=0
[ "$python_menu" -ge 2 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
