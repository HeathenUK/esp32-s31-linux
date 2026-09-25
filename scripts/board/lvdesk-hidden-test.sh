# lvdesk-hidden-test.sh - QoL A4: a hidden terminal renders nothing, and its
# scrollback pages exist only once used. Run ON the board:
#   BIN=/root/lvdesk.new sh lvdesk-hidden-test.sh
#  H1 RssAnon: desktop up / terminal opened / after `seq 1 400` (scrollback used)
#  H2 minimise, `seq 1 300` while hidden, restore: the last row reads 300
#     (checked through the flushed-area log: rows repainted after the restore)
#  H3 px flushed while hidden output streams (want 0)
# ~60 s. Prints one RESULT line. Not on a board in use.
BIN=${BIN:-/usr/bin/lvdesk}; LOG=/var/log/lvdesk.log; U=/root/uinject
killall lvdesk lvdesk.new 2>/dev/null; sleep 1
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
LVDESK_RECTLOG=1 LVDESK_CTL=1 setsid $BIN >$LOG 2>&1 </dev/null &
sleep 8; L=$(pidof $(basename $BIN))
ra() { awk '/^RssAnon/{print $2}' /proc/$L/status; }
c() { echo "$1" > /tmp/lvdesk.ctl; sleep ${2:-1}; }
r0=$(ra); c "run true" 3; r1=$(ra); c "run seq 1 400" 4; r2=$(ra)
tid=$(n=$(wc -l < $LOG); c list; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a Terminal | awk '{print $3}')
c "run clear" 2
# output that starts only AFTER the terminal is hidden: `run` raises the
# terminal, so the command is queued first and minimised straight after
c "run (sleep 4; seq 1 300) &" 1
c "raise $tid" 0; $U chord 125 0 35 >/dev/null 2>&1
hid=$(n=$(wc -l < $LOG); c list; tail -n +$((n+1)) $LOG | grep -a "lvdesk: win" | grep -a Terminal)
n=$(wc -l < $LOG); sleep 6
px=$(tail -n +$((n+1)) $LOG | grep -a "  flush " | awk '{split($2,d,"x"); t+=d[1]*d[2]} END {print t+0}')
still=$(n2=$(wc -l < $LOG); c list; tail -n +$((n2+1)) $LOG | grep -a "lvdesk: win" | grep -a Terminal)
echo "H3 px flushed while hidden output streamed: $px"
echo "   before: $hid"
echo "   after:  $still"
echo "H1 RssAnon kB: up $r0 / terminal open $r1 / after seq 400 $r2"
ok=1; echo "$hid" | grep -q HID || ok=0; echo "$still" | grep -q HID || ok=0
[ "$px" -lt 20000 ] || ok=0; [ $((r2-r1)) -ge 30 ] || ok=0
echo "RESULT bin=$BIN ok=$ok (restore it and look: the last row must read 300)"
