# sinkswitch-test.sh - s31route never blocks an app on a sink nobody drains.
# Run ON the board, detached (it takes ~75 s, longer than a runsh window):
#     setsid sh /root/afp/sinkswitch-test.sh </dev/null >/dev/null 2>&1 &
# and collect /root/afp/sinkswitch.txt afterwards. Nothing here raises the
# volume: the DAC is only ever LOWERED to 143, and AVRCP volume is untouched.
#
# Each arm plays /root/afp/sdltone1 (SDL 1.2, 44100 Hz stereo, samples=512:
# the 2048/512 ring the original report used) for 14 s and moves
# /run/s31-sink mid-stream: 3 s -> hw:1,0 (Bluetooth), 7 s -> hw:0,0,
# 9 s -> hw:1,0, 12 s -> hw:0,0. Pass = calls within 3% of expected, maxgap
# under 400 ms, no underrun storm (< 5 lines). At 5 s and 10 s it records
# which device is actually RUNNING (codec card0 vs loopback card1).
#
#   A1 route off         - s31-bt not routing: no reader (the original report)
#   A2 route on, no sink - headphones disconnected
#   B  route on + sink   - headphones connected, /run/s31-bt-sink present;
#                          SKIPPED (not failed) if they never become ready
#   C  forced loopback   - a fake /run/s31-bt-sink with nobody reading:
#                          does the board's loopback really stall? Either it
#                          drains silently, or s31route reports "took nothing
#                          for N ms" and falls back to the codec. Both pass;
#                          the log says which, which is the open question.
O=/root/afp/sinkswitch.txt
exec > $O 2>&1
T=/root/afp/sdltone1
BT="/usr/bin/s31-bt cmd"
echo "SINKSWITCH $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
md5sum /usr/lib/alsa-lib/libasound_module_pcm_s31route.so /usr/bin/s31-bt
for c in DACL DACR; do
	v=$(amixer -c 0 sget $c 2>/dev/null | awk -F'[][ :]+' '/Mono|Front/ {for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) {print $i; exit}}')
	[ -n "$v" ] && [ "$v" -gt 143 ] && amixer -q -c 0 sset $c 143
done
echo "DAC $(amixer -c 0 sget DACL 2>/dev/null | grep -o '[0-9]* \[' | head -1)"
SINK0=$(cat /run/s31-sink 2>/dev/null || echo hw:0,0)
ADDR=$($BT list 2>/dev/null | awk '/^DEV / && /kind=audio/ && /paired=1/ {print $2; exit}')
echo "sink was $SINK0, audio device ${ADDR:-none}"
$BT status

where() {
	c0=$(head -1 /proc/asound/card0/pcm0p/sub0/status 2>/dev/null)
	c1=$(head -1 /proc/asound/card1/pcm0p/sub0/status 2>/dev/null)
	echo "  t=$1 codec[$c0] loopback[$c1] ready[$(cat /run/s31-bt-sink 2>/dev/null)]"
}

arm() {
	echo "=== ARM $1"
	echo hw:0,0 > /run/s31-sink
	$T 44100 2 512 14 > /tmp/ss.out 2> /tmp/ss.err &
	p=$!
	sleep 3; echo hw:1,0 > /run/s31-sink
	sleep 2; where 5
	sleep 2; echo hw:0,0 > /run/s31-sink
	sleep 2; echo hw:1,0 > /run/s31-sink
	sleep 1; where 10
	sleep 2; echo hw:0,0 > /run/s31-sink
	wait $p
	cat /tmp/ss.out
	u=$(grep -c -i underrun /tmp/ss.err)
	grep -v -i underrun /tmp/ss.err | head -12
	calls=$(sed -n 's/.* calls \([0-9]*\) (expect \([0-9]*\)).*/\1 \2/p' /tmp/ss.out)
	gap=$(sed -n 's/.*maxgap \([0-9]*\)\..*/\1/p' /tmp/ss.out)
	ok=$(echo "$calls $gap $u" | awk '{print ($1 >= $2*0.97 && $1 <= $2*1.10 && $3 < 400 && $4 < 5) ? "PASS" : "FAIL"}')
	echo "RESULT $1 calls/expect=$calls maxgap_ms=$gap underrun_lines=$u $ok"
}

$BT route off; sleep 1
arm A1-route-off

$BT route on
[ -n "$ADDR" ] && $BT disconnect $ADDR
sleep 3
arm A2-route-on-no-sink

if [ -n "$ADDR" ]; then
	$BT connect $ADDR
	i=0; while [ $i -lt 12 ] && [ ! -e /run/s31-bt-sink ]; do sleep 1; i=$((i+1)); done
fi
if [ -e /run/s31-bt-sink ]; then
	arm B-route-on-sink
	grep -a 'ROUTE\|STOPPED\|PLAY' /var/log/s31-bt.log 2>/dev/null | tail -8
else
	echo "RESULT B-route-on-sink SKIP (no /run/s31-bt-sink within 12 s - headphones off?)"
fi

$BT route off; sleep 1
sleep 600 & FAKE=$!
echo $FAKE > /run/s31-bt-sink
arm C-forced-loopback-no-reader
kill $FAKE; rm -f /run/s31-bt-sink

# leave it as the owner had it
echo "$SINK0" > /run/s31-sink
case $SINK0 in hw:1*) $BT route on;; esac
echo "sink restored to $(cat /run/s31-sink)"
echo SINKSWITCH_DONE
