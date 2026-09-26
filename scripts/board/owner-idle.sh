# owner-idle.sh - ON the board: is somebody using the desktop? Run it (60 s)
# before any arm, reset or reboot, and do host-side work instead if it says
# BUSY. Evidence, both read without touching the owner's session:
#  - lvdesk's own input-event count (its SIGUSR1 report prints "input lag
#    ... over N events" and restarts the count): the first read covers the
#    time since the previous report, the second the 60 s window;
#  - the USB host and hosted (Wi-Fi/BT) interrupt counts over the window.
# Prints IDLE or BUSY with the numbers.
LV=$(pidof lvdesk lvdesk.new | awk '{print $1}')
ev() { kill -USR1 $LV 2>/dev/null; sleep 1; grep -a 'input lag' /var/log/lvdesk.log | tail -n 1 | sed 's/.*over \([0-9]*\) events.*/\1/'; }
irq() { awk '/dwc2|usb/{u+=$2+$3} /wireless|hosted/{h+=$2+$3} END{print u+0, h+0}' /proc/interrupts; }
e0=$(ev); i0=$(irq); t0=$(cut -d' ' -f1 /proc/uptime)
sleep ${1:-60}
e1=$(ev); i1=$(irq)
set -- $i0 $i1
echo "since-last-report events ${e0:-?} | window events ${e1:-?} usb_irq +$(($3 - $1)) hosted_irq +$(($4 - $2)) | up $t0"
[ "${e0:-0}" = 0 ] && [ "${e1:-0}" = 0 ] && echo IDLE || echo BUSY
