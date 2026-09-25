# lvdesk-launch-test.sh - QoL D3 launch feedback. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-launch-test.sh
# Toasts are also logged ("lvdesk: toast ..."), which is what this reads.
#  L1 launch nosuchcmd        -> "Starting", then "... failed: ...not found"
#  L2 launch sh -c 'exit 0'   -> "Starting" only
#  L3 launch sh -c 'kill -9 $$' -> "... was killed (signal 9)" (not "out of memory": oom_kill did not move)
#  L4 launch xcalc            -> "Starting" only, xcalc maps
# ~40 s. Prints one RESULT line.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log
killall lvdesk lvdesk.new 2>/dev/null; sleep 1; for p in $(pidof xcalc); do kill -9 $p; done
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8
t() { n=$(wc -l < $LOG); echo "launch $1" > /tmp/lvdesk.ctl; sleep ${2:-3}; tail -n +$((n+1)) $LOG | grep -a "lvdesk: toast" | sed 's/lvdesk: toast //' | tr '\n' '|'; }
a=$(t nosuchcmd); b=$(t "sh -c 'exit 0'"); c=$(t "sh -c 'kill -9 \$\$'"); d=$(t xcalc 5); x=$(pidof xcalc | wc -w)
for p in $(pidof xcalc); do kill -9 $p; done
echo "L1 $a"; echo "L2 $b"; echo "L3 $c"; echo "L4 $d (xcalc $x)"
ok=1
echo "$a" | grep -q "failed:.*not found" || ok=0
echo "$b" | grep -q "failed\|killed" && ok=0
echo "$c" | grep -q "killed (signal 9)" || ok=0
echo "$d" | grep -q "failed\|killed" && ok=0; [ "$x" = 1 ] || ok=0
echo "RESULT bin=$BIN ok=$ok"
