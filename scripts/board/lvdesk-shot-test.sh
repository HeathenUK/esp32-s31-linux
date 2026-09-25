# lvdesk-shot-test.sh - QoL D7 PrintScreen. ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-shot-test.sh [serve]
#  P1 Print (KEY_SYSRQ) on the desktop: a toast, and a 768,066-byte BMP
#     (800x480 RGB565 + 66-byte header) in /root/Pictures
#  P2 ten `ctl shot` in a row: MemAvailable and CmaFree back within 1 MB
#     once the files are deleted (nothing persistent is allocated)
#  P3 a shot during prboom -fullscreen: the client's own 320x200 (or 320x240)
#     buffer, no toast over the game, "1 screenshot saved" after it exits
# With "serve": leaves P1's file on port 8137 for one curl from the host.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new prboom 2>/dev/null; sleep 1; rm -rf /root/Pictures
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
mi() { awk -v k="$1:" '$1==k{print $2}' /proc/meminfo; }
$U key 99 >/dev/null 2>&1; sleep 1
f=$(ls /root/Pictures/*.bmp 2>/dev/null | head -n 1); sz=$(wc -c < "$f" 2>/dev/null)
p1="$(grep -a 'lvdesk: shot ' $LOG | tail -n 1) | toast: $(grep -ac 'toast.*Screenshot saved' $LOG) | size $sz"
sync; echo 1 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1; a0=$(mi MemAvailable); c0=$(mi CmaFree)
i=0; while [ $i -lt 10 ]; do echo "shot /tmp/shot$i.bmp" > /tmp/lvdesk.ctl; sleep 0.4; i=$((i+1)); done
sleep 1; ms=$(grep -a 'lvdesk: shot /tmp' $LOG | awk '{print $(NF-1)}' | tr '\n' ' ')
rm -f /tmp/shot*.bmp; sync; echo 1 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1; a1=$(mi MemAvailable); c1=$(mi CmaFree)
fs0=$(grep -ac 'toast.*Screenshot' $LOG)
cd /root/doom/wads && setsid sh -c 'exec env DISPLAY=:0 /root/doom/prboom -width 320 -height 200 -fullscreen >/tmp/prboom.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 14; echo "shot /tmp/fs.bmp" > /tmp/lvdesk.ctl; sleep 1
fsl=$(grep -a 'lvdesk: shot /tmp/fs.bmp' $LOG | tail -n 1); fst=$(grep -ac 'toast.*Screenshot' $LOG)
for p in $(pidof prboom); do kill -9 $p; done; sleep 3
rep=$(grep -a 'toast.*screenshot saved' $LOG | tail -n 1)
echo "P1 $p1"
echo "P2 ms: $ms | MemAvailable $a0 -> $a1 kB, CmaFree $c0 -> $c1 kB"
echo "P3 $fsl | toasts during fs: $((fst - fs0)) (want 0) | on leave: $rep"
ok=1; [ "$sz" = 768066 ] || ok=0; echo "$fsl" | grep -q saved || ok=0; [ -n "$rep" ] || ok=0; [ "$fst" = "$fs0" ] || ok=0
[ $((a0 - a1)) -lt 1024 ] || ok=0; [ $((c0 - c1)) -lt 1024 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"; rm -f /tmp/fs.bmp
if [ "${1:-}" = serve ]; then setsid /root/s31-serve "$f" 8137 >/dev/null 2>&1 </dev/null & sleep 0.2; echo "SERVING $(busybox ip -o -4 addr show wlan0 | busybox awk '{print $4}' | cut -d/ -f1)"; fi
