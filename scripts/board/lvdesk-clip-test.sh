# lvdesk-clip-test.sh - QoL A6 copy and paste in the console. ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-clip-test.sh [shot]
# The console is docked at 0,0: content padding 4, cells 8x8.
#  P1 drag across two rows -> ctl clip holds two lines
#  P2 drag within one row -> one number; `echo <Shift+Insert> > /tmp/clipok`
#     writes that number (paste reached the shell)
#  P3 middle click pastes too
#  P4 select, Ctrl+C: copied and the highlight cleared; Ctrl+V pastes it
#  P5 no selection: Ctrl+C interrupts `sleep 30` (the next command runs)
# With "shot": leaves a two-row selection up for a host screenshot.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; rm -f /tmp/clipok /tmp/clipok2 /tmp/clipok3
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_PTR_ACCEL=0 LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
clip() { n=$(wc -l < $LOG); echo clip > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: clip" -A3 | tr '\n' '|'; }
# rows are y = 4 + r*8 (+4 to hit the middle); columns x = 4 + c*8 (+3)
printf 'chord 125 300 41\nsleep 600\ntype clear; seq 1 30\\n\nsleep 1500\ndragto 7 52 15 60\nsleep 500\n' | $U script >/dev/null 2>&1
a=$(clip)
[ "${1:-}" = shot ] && exit 0
printf 'dragto 7 68 15 68\nsleep 500\ntype echo \nchord 42 0 110\nsleep 600\ntype  > /tmp/clipok\\n\nsleep 800\n' | $U script >/dev/null 2>&1
b=$(clip); f=$(cat /tmp/clipok 2>/dev/null)
printf 'type echo \nmoveto 200 100\nsleep 300\nmclick 200 100\nsleep 600\ntype  > /tmp/clipok2\\n\nsleep 800\n' | $U script >/dev/null 2>&1
g=$(cat /tmp/clipok2 2>/dev/null)
printf 'dragto 7 76 15 76\nsleep 400\nchord 29 0 46\nsleep 500\n' | $U script >/dev/null 2>&1
h=$(clip)
printf 'type echo \nchord 29 0 47\nsleep 600\ntype  > /tmp/clipok3\\n\nsleep 800\n' | $U script >/dev/null 2>&1
i=$(cat /tmp/clipok3 2>/dev/null)
rm -f /tmp/intok; printf 'type sleep 30\\n\nsleep 1000\nchord 29 0 46\nsleep 800\ntype touch /tmp/intok\\n\nsleep 1500\n' | $U script >/dev/null 2>&1
j=$([ -e /tmp/intok ] && echo 1 || echo 0)
echo "P1 two-row drag: $a"
echo "P2 one-row drag: $b | shell wrote: '$f'"
echo "P3 middle-click paste wrote: '$g'"
echo "P4 after select+Ctrl+C: $h | Ctrl+V wrote: '$i'"
echo "P5 Ctrl+C with no selection interrupted sleep: $j"
w=$(echo "$b" | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
w4=$(echo "$h" | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
ok=1; echo "$a" | grep -q "sel=1" || ok=0; [ -n "$w" ] && [ "$f" = "$w" ] || ok=0
echo "$h" | grep -q "sel=0" || ok=0; [ -n "$w4" ] && [ "$i" = "$w4" ] || ok=0; [ "$j" = 1 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
