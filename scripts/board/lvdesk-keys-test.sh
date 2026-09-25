# lvdesk-keys-test.sh - QoL B6 Super+N / launch keys and B7 shortcuts sheet.
# Run ON the board:  BIN=/root/lvdesk.new sh lvdesk-keys-test.sh [shot]
#  K1 Super+2 focuses the 2nd task button's window; again minimises it
#  K2 Super+E twice 250 ms apart starts exactly ONE xfiles (the 8 s guard)
#  K3 Super+/ opens the sheet, Space closes it, st receives no key
# With "shot": leaves the sheet open for a host screenshot and exits.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject; SL=/tmp/st-keys.log
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc) $(pidof st) $(pidof xfiles); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0
if [ "${1:-}" = shot ]; then printf 'chord 125 0 53\n' | $U script >/dev/null 2>&1; exit 0; fi
setsid sh -c "XLITE_TRACE_INPUT=1 exec st >$SL 2>&1" </dev/null >/dev/null 2>&1 &
sleep 6; setsid sh -c 'exec xcalc >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 5
L() { n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a " $1\$"; }
echo "raise 0" > /tmp/lvdesk.ctl; sleep 1
printf 'chord 125 0 3\n' | $U script >/dev/null 2>&1; a=$(L xcalc)
printf 'chord 125 0 3\n' | $U script >/dev/null 2>&1; b=$(L xcalc)
printf 'chord 125 0 18 18\n' | $U script >/dev/null 2>&1; sleep 9; x=$(pidof xfiles | wc -w)
for p in $(pidof xfiles); do kill -9 $p; done; sleep 1
echo "raise 0" > /tmp/lvdesk.ctl; sleep 1
k0=$(grep -ac 'queue event type 2$' $SL)
printf 'chord 125 0 53\nsleep 800\nkey 57\n' | $U script >/dev/null 2>&1
n=$(wc -l < $LOG); echo pop > /tmp/lvdesk.ctl; sleep 1; p=$(tail -n +$((n+1)) $LOG | grep -a "lvdesk: pop" | awk '{print $3}')
k1=$(grep -ac 'queue event type 2$' $SL)
for q in $(pidof xcalc) $(pidof st); do kill -9 $q; done
echo "K1 Super+2: $a | again: $b"
echo "K2 xfiles after Super+E, Super+E: $x"
echo "K3 sheet after Space: $p | st key presses: $((k1-k0))"
ok=1; echo "$a" | grep -q FOCUS || ok=0; echo "$b" | grep -q MIN || ok=0
[ "$x" = 1 ] || ok=0; [ "$p" = closed ] || ok=0; [ $((k1-k0)) = 0 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
