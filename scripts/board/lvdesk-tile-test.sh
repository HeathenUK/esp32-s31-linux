# lvdesk-tile-test.sh - QoL B3 Super tiling. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-tile-test.sh
# st (resizable) and xcalc (which declares no fixed size - recorded only). Prints the `list` line after each
# chord and one RESULT line. ~70 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc) $(pidof st); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0
setsid sh -c 'exec xcalc >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 5; setsid sh -c 'exec st >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6
L() { n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a " $1\$" | sed 's/lvdesk: win [0-9]* //'; }
s0=$(L st); $U chord 125 0 105 >/dev/null 2>&1; s1=$(L st)
$U chord 125 0 103 >/dev/null 2>&1; s2=$(L st); $U chord 125 0 108 >/dev/null 2>&1; s3=$(L st)
x=$(L xcalc | awk '{print $1}'); xi=$(n=$(wc -l < $LOG); echo list > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a " xcalc$" | awk '{print $3}')
echo "raise $xi" > /tmp/lvdesk.ctl; sleep 1; $U chord 125 0 105 >/dev/null 2>&1; x1=$(L xcalc | awk '{print $1}')
$U chord 125 0 32 >/dev/null 2>&1; d1="$(L st) / $(L xcalc)"; $U chord 125 0 32 >/dev/null 2>&1; d2="$(L st) / $(L xcalc)"
for p in $(pidof st); do kill -9 $p; done; sleep 2
setsid sh -c 'exec st >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6; $U chord 125 0 16 >/dev/null 2>&1; sleep 4; q=$(pidof st | wc -w)
for p in $(pidof xcalc) $(pidof st); do kill -9 $p; done
echo "st start:    $s0"; echo "Super+Left:  $s1"; echo "Super+Up:    $s2"; echo "Super+Down:  $s3"
echo "xcalc before/after Super+Left: $x / $x1"
echo "Super+D:     $d1"; echo "Super+D:     $d2"; echo "Super+Q on a new st, running after: $q"
g0=$(echo "$s0" | awk '{print $1}')
ok=1
echo "$s1" | grep -q "^400x458+0+0 .*SNAP" || ok=0
echo "$s2" | grep -q "^800x458+0+0 MAX" || ok=0
[ "$(echo "$s3" | awk '{print $1}')" = "$g0" ] || ok=0
# xcalc is NOT asserted: it declares no fixed size (min != max), so it tiles,
# exactly as a drag to the edge already tiles it. Recorded, not a regression.
echo "$d1" | grep -q "HID.*/.*HID" || ok=0
echo "$d2" | grep -qv "HID" || ok=0
[ "$q" = 0 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
