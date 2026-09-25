# lvdesk-altf4-test.sh - Alt+F4 closes every kind of window. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-altf4-test.sh
# fullscreen prboom, windowed prboom (grabs input), xcalc: each must be gone
# within 8 s of one Alt+F4 (prboom ignores WM_DELETE_WINDOW with its quit
# prompt, so it is the 3 s drop that ends it). ~90 s. Not on a board in use.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
for p in $(pidof prboom) $(pidof xcalc); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; export DISPLAY=:0; cd /root/doom/wads
setsid sh -c 'exec /root/doom/prboom -width 320 -height 240 >/tmp/pb.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 20; a=$(grep -ac "fullscreen on\|kms_fs_enter\|fullscreen:" $LOG); $U altkey 62 >/dev/null 2>&1; sleep 8
fs=$(pidof prboom | wc -w); echo "fullscreen prboom after=$fs"; grep -a "Alt+F4\|fullscreen off" $LOG | tail -2
for p in $(pidof prboom); do kill -9 $p; done; sleep 2
setsid sh -c 'exec /root/doom/prboom -width 320 -height 240 -window >/tmp/pb.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 20; $U altkey 62 >/dev/null 2>&1; sleep 8; ww=$(pidof prboom | wc -w); echo "windowed prboom after=$ww"
for p in $(pidof prboom); do kill -9 $p; done; sleep 1
setsid sh -c 'exec xcalc >/dev/null 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6; $U altkey 62 >/dev/null 2>&1; sleep 4; xc=$(pidof xcalc | wc -w); echo "xcalc after=$xc"
for p in $(pidof xcalc); do kill -9 $p; done
[ "$fs$ww$xc" = 000 ] && ok=1 || ok=0; echo "RESULT bin=$BIN ok=$ok"
