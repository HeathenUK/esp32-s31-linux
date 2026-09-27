# sinkear.sh - listen to a stream moving between the speaker and the
# Bluetooth headphones mid-stream. Run it from a board shell with the
# headphones on and connected:
#     sh /root/afp/sinkear.sh
# A quiet 440 Hz tone (sdltone1, SDL 1.2, 44100 Hz stereo) plays for 24 s:
#     0-6 s speaker, 6-12 headphones, 12-18 speaker, 18-24 headphones.
# Listen for: the tone never stopping or stuttering on either side of a
# switch (a skip of up to ~50 ms at the switch itself is the old sink's
# queue being dropped), and on the headphones arriving ~0.5-1 s after the
# switch (s31-bt acquiring A2DP). If the headphones are NOT ready, the
# "headphones" phases stay on the speaker - that is the fix, not a fault.
# The DAC is only ever lowered to 143, never raised; AVRCP volume untouched.
for c in DACL DACR; do
	v=$(amixer -c 0 sget $c 2>/dev/null | awk -F'[][ :]+' '/Mono|Front/ {for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) {print $i; exit}}')
	[ -n "$v" ] && [ "$v" -gt 143 ] && amixer -q -c 0 sset $c 143
done
S0=$(cat /run/s31-sink 2>/dev/null || echo hw:0,0)
/usr/bin/s31-bt cmd route on >/dev/null; sleep 1
[ -e /run/s31-bt-sink ] && echo "headphones: ready" || echo "headphones: NOT ready (the headphone phases will stay on the speaker)"
echo hw:0,0 > /run/s31-sink
/root/afp/sdltone1 44100 2 512 24 >/tmp/sinkear.out 2>/tmp/sinkear.err &
p=$!
for step in "6 hw:1,0 headphones" "6 hw:0,0 speaker" "6 hw:1,0 headphones"; do
	set -- $step
	echo "now: $([ "$(cat /run/s31-sink)" = hw:0,0 ] && echo speaker || echo headphones)"
	sleep $1
	echo $2 > /run/s31-sink
done
echo "now: headphones"
wait $p
echo "$S0" > /run/s31-sink
case $S0 in hw:1*) ;; *) /usr/bin/s31-bt cmd route off >/dev/null;; esac
grep -a 'calls' /tmp/sinkear.out
echo "underrun lines: $(grep -c -i underrun /tmp/sinkear.err)"
grep -a 's31route' /tmp/sinkear.err
echo "output restored to $S0"
