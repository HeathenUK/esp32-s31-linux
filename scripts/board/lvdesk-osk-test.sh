# lvdesk-osk-test.sh - QoL D8 on-screen keyboard. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-osk-test.sh
# ONE uinject `script` run (one device settle) drives the whole sequence.
#  O1 tap3 shows it          O2 q, w on it -> xcalc gets 2 KeyPress (xcalc selects
#     no KeyRelease: real keys give it exactly the same 2 events)
#  O3 console: "touch oskok" + Enter typed on it -> /root/oskok exists
#  O4 tap3 again hides it
# Prints one RESULT line. ~40 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; XL=/tmp/xcalc-osk.log
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc); do kill -9 $p; done; rm -f /root/oskok
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0
setsid sh -c "XLITE_TRACE_INPUT=1 exec xcalc >$XL 2>&1" </dev/null >/dev/null 2>&1 &
sleep 6
osk() { n=$(wc -l < $LOG); echo osk > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | grep -a "lvdesk: osk" | tail -1 | awk '{print $3}'; }
k=$(grep -ac 'queue event type [23]$' $XL)
# key centres on the 800x150 keyboard at y 308 (see the C2 screenshot)
/root/uinject script >/dev/null 2>&1 <<'SCRIPT'
tap3 400 200
sleep 500
tap 107 326
sleep 300
tap 168 326
sleep 500
chord 125 300 41
sleep 800
tap 352 326
tap 598 326
tap 475 326
tap 299 402
tap 450 364
tap 400 438
tap 598 326
tap 209 364
tap 570 364
tap 598 326
tap 570 364
tap 730 364
sleep 1500
SCRIPT
o1=$(osk); k2=$(grep -ac 'queue event type [23]$' $XL)
f=$([ -e /root/oskok ] && echo 1 || echo 0)
/root/uinject tap3 400 250 >/dev/null 2>&1; sleep 1; o4=$(osk)
for p in $(pidof xcalc); do kill -9 $p; done; rm -f /root/oskok
echo "O1/O2 osk after tap3: $o1 | xcalc key events from q,w: $((k2-k))"
echo "O3 console file typed on the OSK: $f"
echo "O4 osk after second tap3: $o4"
ok=1; [ "$o1" = shown ] || ok=0; [ $((k2-k)) = 2 ] || ok=0; [ "$f" = 1 ] || ok=0; [ "$o4" = hidden ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
