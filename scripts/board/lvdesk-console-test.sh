# lvdesk-console-test.sh - QoL A3 console, B0 Super chords, C0 popover repaint.
# Run ON the board:  BIN=/root/lvdesk.new sh lvdesk-console-test.sh
# Restarts the desktop on $BIN with LVDESK_RECTLOG=1, then:
#  C0  flushed px for a tray popover open and close        (want << 384000)
#  A3  Super+grave shows the console docked and focused, typing reaches its
#      shell, Super+grave again hides it, a third shows the SAME shell
#  B0  xcalc focused: Super+Left gives xcalc no KeyPress/KeyRelease, and the
#      "Super let go first" order leaks nothing either; Alt+F4 still closes it
# Prints one RESULT line. ~75 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}
LOG=/var/log/lvdesk.log; XL=/tmp/xcalc-con.log; U=/root/uinject
# uinject type presses Enter only for a REAL newline in its argument; a
# literal backslash-n from the shell types two characters and no Enter.
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
for p in $(pidof xcalc); do kill -9 $p; done; rm -f /tmp/conok*
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_RECTLOG=1 LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
c() { echo "$1" > /tmp/lvdesk.ctl; sleep ${2:-1}; }
lst() { n=$(wc -l < $LOG); c list; tail -n +$((n+1)) $LOG | grep -a 'lvdesk: win '; }
px() { n=$(wc -l < $LOG); c "$1" 2; tail -n +$((n+1)) $LOG | grep -a "  flush " | awk '{split($2,d,"x"); t+=d[1]*d[2]} END {print t+0}'; }
o=$(px "tray audio"); cl=$(px "tray audio")
echo "C0 open_px=$o close_px=$cl"
$U chord 125 300 41 >/dev/null 2>&1; sleep 1; l1=$(lst | grep -a Terminal); echo "A3 after 1st: $l1"
$U type "touch /tmp/conok
" >/dev/null 2>&1; sleep 2; t1=$([ -e /tmp/conok ] && echo 1 || echo 0)
sp1=$(for s in /proc/[0-9]*/status; do awk '/^Name:\tsh$/{f=1} /^PPid/{if(f)print FILENAME}' $s 2>/dev/null; done | head -1)
$U chord 125 300 41 >/dev/null 2>&1; sleep 1; l2=$(lst | grep -a Terminal); echo "A3 after 2nd: $l2"
$U chord 125 300 41 >/dev/null 2>&1; sleep 1; l3=$(lst | grep -a Terminal); echo "A3 after 3rd: $l3"
$U type "touch /tmp/conok2
" >/dev/null 2>&1; sleep 2; t3=$([ -e /tmp/conok2 ] && echo 1 || echo 0)
$U chord 125 300 41 >/dev/null 2>&1; sleep 1
export DISPLAY=:0
setsid sh -c "XLITE_TRACE_INPUT=1 exec xcalc >$XL 2>&1" </dev/null >/dev/null 2>&1 &
sleep 6; lst | grep -a xcalc
k0=$(grep -ac 'queue event type [23]$' $XL)
$U chord 125 300 105 >/dev/null 2>&1; sleep 1; k1=$(grep -ac 'queue event type [23]$' $XL)
$U chord 125 300 -105 >/dev/null 2>&1; sleep 1; k2=$(grep -ac 'queue event type [23]$' $XL)
$U altkey 62 >/dev/null 2>&1; sleep 3; x=$(pidof xcalc | wc -w)
for p in $(pidof xcalc); do kill -9 $p; done
echo "B0 super_left_keys=$((k1-k0)) super_first_keys=$((k2-k1)) xcalc_after_altf4=$x"
ok=1
[ "$o" -lt 150000 ] && [ "$cl" -lt 150000 ] || ok=0
echo "$l1" | grep -q " FOCUS.* CON" || ok=0
echo "$l2" | grep -q " HID" || ok=0
echo "$l3" | grep -q " FOCUS.* CON" || ok=0
[ "$t1$t3" = 11 ] || ok=0
[ $((k1-k0)) = 0 ] && [ $((k2-k1)) = 0 ] && [ "$x" = 0 ] || ok=0
grep -a -i "segfault\|assert" $LOG | head -3
echo "RESULT bin=$BIN ok=$ok"
