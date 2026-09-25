# lvdesk-search-test.sh - QoL B5 type-to-search in the app menu. ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-search-test.sh [shot]
#  S1 Super tap, "clock": query row + hits, first hit highlighted
#  S2 Enter launches it (xclock appears)
#  S3 "zzz": no matches (rows=1), Enter launches nothing
#  S4 Esc clears back to the menu (cur=-1), Esc again closes
# With "shot": leaves a "quake" search open for a host screenshot and exits.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xclock); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
pop() { n=$(wc -l < $LOG); echo pop > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: pop" | tail -1 | sed 's/lvdesk: pop //'; }
if [ "${1:-}" = shot ]; then
	printf 'hold 125 150\nsleep 300\ntype quake\n' | $U script >/dev/null 2>&1; exit 0
fi
printf 'hold 125 150\nsleep 300\ntype clock\n' | $U script >/dev/null 2>&1; a=$(pop)
$U key 28 >/dev/null 2>&1; sleep 4; x=$(pidof xclock | wc -w)
for p in $(pidof xclock); do kill -9 $p; done; sleep 1
printf 'hold 125 150\nsleep 300\ntype zzz\nkey 28\n' | $U script >/dev/null 2>&1; b=$(pop); x2=$(pidof xclock | wc -w)
$U key 1 >/dev/null 2>&1; c=$(pop); $U key 1 >/dev/null 2>&1; d=$(pop)
echo "S1 after 'clock': $a"
echo "S2 xclock running after Enter: $x"
echo "S3 after 'zzz'+Enter: $b (xclock: $x2)"
echo "S4 Esc: $c | Esc again: $d"
ok=1
echo "$a" | grep -q "^open rows=[2-9].* sel=1" || ok=0
[ "$x" = 1 ] || ok=0
echo "$b" | grep -q "^open rows=1 " || ok=0; [ "$x2" = 0 ] || ok=0
echo "$c" | grep -q "^open rows=3 .*cur=-1" || ok=0; echo "$d" | grep -q "^closed" || ok=0
echo "RESULT bin=$BIN ok=$ok"
