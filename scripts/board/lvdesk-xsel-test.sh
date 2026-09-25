# lvdesk-xsel-test.sh - QoL D4: X selections between clients and the console.
# ON the board:  BIN=/root/lvdesk.new XLIB=/root/xtest sh lvdesk-xsel-test.sh
# (XLIB empty = the shipped libX11). st A is placed at 150,60 (first window):
# its row 0 "XSELWORD" spans x 153-200 at y 90 (cells 6x12). st B runs
# `cat > /tmp/xselB`; each paste into it is followed by Enter.
#  S1 st->st PRIMARY: drag over A's word, middle click in B
#  S2 st->st Shift+Insert in B (PRIMARY again)
#  S3 st->console: console Shift+Insert pastes A's PRIMARY into the shell
#  S4 dead owner: A claims CLIPBOARD (Ctrl+Shift+C) and is killed; B's
#     Ctrl+Shift+V gets None and B still takes typing ("ALIVE")
#  S5 console->st: a console drag-select, then middle click in B
#  S6 a 20-line (1.6 kB, over the old 1 kB PROP_MAX) selection from a new
#     st A2, dragged corner to corner, arrives whole in B
BIN=${BIN:-/usr/bin/lvdesk}; XLIB=${XLIB-/root/xtest}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new st 2>/dev/null; sleep 1; rm -f /tmp/xselA /tmp/xselB /tmp/xselC
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_PTR_ACCEL=0 LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 7; echo "winmem off" > /tmp/lvdesk.ctl
[ -n "$XLIB" ] && export LD_LIBRARY_PATH=$XLIB; export DISPLAY=:0
setsid sh -c 'exec st -e sh -c "echo XSELWORD; exec cat > /tmp/xselA"' </dev/null >/tmp/stA.log 2>&1 &
sleep 4
setsid sh -c 'exec st -e sh -c "exec cat > /tmp/xselB"' </dev/null >/tmp/stB.log 2>&1 &
sleep 4
echo "move 1 300 130" > /tmp/lvdesk.ctl; sleep 1
BX=500; BY=300			# inside B, clear of A
raise() { echo "raise $1" > /tmp/lvdesk.ctl; sleep 0.5; }
# S1
raise 0; printf 'dragto 154 90 199 90\nsleep 500\n' | $U script >/dev/null 2>&1
raise 1; printf 'mclick %d %d\nsleep 800\ntype \\n\nsleep 500\n' $BX $BY | $U script >/dev/null 2>&1
s1=$(sed -n 1p /tmp/xselB 2>/dev/null)
# S2
printf 'chord 42 0 110\nsleep 800\ntype \\n\nsleep 500\n' | $U script >/dev/null 2>&1
s2=$(sed -n 2p /tmp/xselB 2>/dev/null)
# S3
printf 'chord 125 300 41\nsleep 700\ntype echo \nchord 42 0 110\nsleep 800\ntype  > /tmp/xselC\\n\nsleep 800\nchord 125 300 41\nsleep 500\n' | $U script >/dev/null 2>&1
s3=$(cat /tmp/xselC 2>/dev/null)
# S4
raise 0; printf 'chord 29 0 -42 46\nsleep 400\n' | $U script >/dev/null 2>&1
for p in $(pidof st); do grep -aq xselA /proc/$p/cmdline 2>/dev/null && kill -9 $p; done
sleep 1; raise 1
printf 'chord 29 0 -42 47\nsleep 800\ntype ALIVE\\n\nsleep 600\n' | $U script >/dev/null 2>&1
s4=$(tail -n 1 /tmp/xselB 2>/dev/null)
# S5: console rows are y = 4 + r*8, columns x = 4 + c*8
printf 'chord 125 300 41\nsleep 700\ntype clear; seq 1 30\\n\nsleep 1500\ndragto 7 68 15 68\nsleep 500\nchord 125 300 41\nsleep 500\n' | $U script >/dev/null 2>&1
n=$(wc -l < $LOG); echo clip > /tmp/lvdesk.ctl; sleep 1
w=$(tail -n +$((n+1)) $LOG | grep -a "lvdesk: clip" -A1 | sed -n 's/.*\[\([0-9]*\)\].*/\1/p' | head -n 1)
echo list > /tmp/lvdesk.ctl; sleep 0.5; bi=$(tail -n 20 $LOG | grep -a "lvdesk: win" | grep -av CON | tail -n 1 | awk '{print $3}')
raise ${bi:-0}; printf 'mclick %d %d\nsleep 800\ntype \\n\nsleep 500\n' $BX $BY | $U script >/dev/null 2>&1
s5=$(tail -n 1 /tmp/xselB 2>/dev/null)
# S6
setsid sh -c 'exec st -e sh -c "i=0; while [ \$i -lt 20 ]; do printf %02d%077d\\\\n \$i 0; i=\$((i+1)); done; exec sleep 99"' </dev/null >/tmp/stA2.log 2>&1 &
sleep 5; n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 0.5
ai=$(tail -n +$((n+1)) $LOG | grep -a "lvdesk: win.* FOCUS" | awk '{print $3}' | head -n 1)
echo "move ${ai:-0} 150 60" > /tmp/lvdesk.ctl; sleep 1; raise ${ai:-0}; sleep 0.5
b0=$(wc -l < /tmp/xselB)
printf 'dragto 154 90 626 372\nsleep 500\n' | $U script >/dev/null 2>&1
raise ${bi:-0}; printf 'mclick %d %d\nsleep 1500\ntype \\n\nsleep 500\n' $BX $BY | $U script >/dev/null 2>&1
s6=$(tail -n +$((b0+1)) /tmp/xselB | grep -c '^[0-2][0-9]0\{77\}$')
s6b=$(tail -n +$((b0+1)) /tmp/xselB | wc -c)
echo "S1 st->st middle click: '$s1'"
echo "S2 st->st Shift+Insert: '$s2'"
echo "S3 st->console: '$s3'"
echo "S4 dead CLIPBOARD owner, then typing: '$s4' (st alive: $(pidof st | wc -w))"
echo "S5 console->st: clip '$w' pasted '$s5'"
echo "S6 big selection: $s6 of 20 lines, $s6b bytes"
grep -a "xshim: UNIMPLEMENTED\|X error" $LOG /tmp/stA.log /tmp/stB.log | head -5
ok=1; [ "$s1" = XSELWORD ] || ok=0; [ "$s2" = XSELWORD ] || ok=0; [ "$s3" = XSELWORD ] || ok=0
[ "$s4" = ALIVE ] || ok=0; [ -n "$w" ] && [ "$s5" = "$w" ] || ok=0; [ "$s6" = 20 ] || ok=0
echo "RESULT bin=$BIN xlib=${XLIB:-shipped} ok=$ok"
for p in $(pidof st); do kill -9 $p; done
