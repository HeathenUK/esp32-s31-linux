#!/bin/sh
# rapid.sh: A2DP "ready", then codec <-> loopback every 0.4 s (two switches
# inside most seconds) for 8 s, for three app formats.
S="0.3:ready"; t=0.5; i=0
while [ $i -lt 20 ]; do
	if [ $((i % 2)) = 0 ]; then S="$S $t:sink=hw:1,0"; else S="$S $t:sink=hw:0,0"; fi
	t=$(awk "BEGIN{print $t+0.4}"); i=$((i + 1))
done
for a in "44100 2 1024" "22050 1 512" "48000 2 2048"; do
	echo "== $a"
	DBG=1 /w/run.sh $a 10 "$S" > /tmp/o 2>&1
	grep -E 'maxgap|underrun lines' /tmp/o
	echo "sink opens: $(grep -c ' app ' /tmp/o) (1 + 20 switches expected)"
done
