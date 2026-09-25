# lvdesk-toast-vol-test.sh - QoL D1 toast and D2 volume/mute keys. ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-toast-vol-test.sh
# The volume ends EXACTLY where it started (keep it low: volume=40, DAC 143),
# and the script checks that. Screens are captured from the host.
#  V1 Up, Up    -> state volume +10 (40 -> 50 from the usual 40)
#  V2 Down, Down-> back to the start, DAC back to its start value
#  V3 Mute      -> state muted=1, volume unchanged, DAC 0; Mute again -> muted=0, DAC back
#  T1 20 notifies -> lvmem used_bytes back to baseline after they expire
# Prints one RESULT line. ~60 s. Never run while someone is using the board.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject; ST=/etc/lvdesk/state
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
sv() { sed -n 's/^volume=//p' $ST; }; sm() { sed -n 's/^muted=//p' $ST; }
dac() { amixer -c 0 sget DACL 2>/dev/null | sed -n 's/.*Playback \([0-9]*\) \[.*/\1/p' | tail -1; }
mem() { n=$(wc -l < $LOG); echo lvmem > /tmp/lvdesk.ctl; sleep 1; tail -n +$((n+1)) $LOG | sed -n 's/.*used_bytes \([0-9]*\).*/\1/p' | tail -1; }
v0=$(sv); d0=$(dac)
$U key 115 >/dev/null 2>&1; $U key 115 >/dev/null 2>&1; sleep 1; v1=$(sv); d1=$(dac)
$U key 114 >/dev/null 2>&1; $U key 114 >/dev/null 2>&1; sleep 1; v2=$(sv); d2=$(dac)
$U key 113 >/dev/null 2>&1; sleep 1; v3=$(sv); m3=$(sm); d3=$(dac)
$U key 113 >/dev/null 2>&1; sleep 1; m4=$(sm); d4=$(dac)
sleep 3; b0=$(mem)
i=0; while [ $i -lt 20 ]; do echo "notify toast number $i" > /tmp/lvdesk.ctl; i=$((i+1)); done
sleep 5; b1=$(mem)
echo "V1 up x2: volume $v0 -> $v1, DAC $d0 -> $d1"
echo "V2 down x2: volume -> $v2, DAC -> $d2"
echo "V3 mute: volume $v3 muted=$m3 DAC $d3 | unmute: muted=$m4 DAC $d4"
echo "T1 lvmem used_bytes before/after 20 notifies: $b0 / $b1"
ok=1
[ "$v1" = $((v0+10)) ] && [ "$v2" = "$v0" ] && [ "$d2" = "$d0" ] || ok=0
[ "$v3" = "$v0" ] && [ "$m3" = 1 ] && [ "$d3" = 0 ] && [ "$m4" = 0 ] && [ "$d4" = "$d0" ] || ok=0
[ "$b0" = "$b1" ] || ok=0
echo "RESULT bin=$BIN ok=$ok (final volume $(sv), DAC $(dac))"
