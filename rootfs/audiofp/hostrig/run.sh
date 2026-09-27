#!/bin/sh
# run.sh <rate> <ch> <samples> <secs> "<schedule>"
# schedule: space-separated t:action, action = sink=<name> | ready | unready | deadready
r=$1; c=$2; s=$3; secs=$4; sched=$5
echo hw:0,0 > /run/s31-sink; rm -f /run/s31-bt-sink
sleep 1000 & HOLD=$!
[ -n "$DBG" ] && export S31ROUTE_DEBUG=1
SDL_AUDIODRIVER=alsa sdltone $r $c $s $secs > /tmp/t.out 2> /tmp/t.err &
p=$!
prev=0
for ev in $sched; do
	t=${ev%%:*}; a=${ev#*:}
	sleep $(awk "BEGIN{print $t-$prev}"); prev=$t
	case $a in
	sink=*) echo ${a#sink=} > /run/s31-sink;;
	ready) echo $HOLD > /run/s31-bt-sink.new && mv /run/s31-bt-sink.new /run/s31-bt-sink;;
	unready) rm -f /run/s31-bt-sink;;
	deadready) echo 999999 > /run/s31-bt-sink;;
	esac
	echo "[t=$t $a]"
done
wait $p
kill $HOLD
cat /tmp/t.out
echo "--- underrun lines: $(grep -c -i underrun /tmp/t.err)"
grep -v -i underrun /tmp/t.err
