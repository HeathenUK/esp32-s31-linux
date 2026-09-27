# Bounded by the CLOCK, not an iteration count: on a loaded board one ps|grep
# turn took >3.4 s, 40 turns outlived runsh's window and the run reported
# nothing at all (2026-09-21). A slow arm needs a larger DEMO_WAIT instead.
read u _ < /proc/uptime; end=$((${u%.*} + 100))
while ps | grep -q "[p]rboom"; do read u _ < /proc/uptime; [ ${u%.*} -ge $end ] && break; sleep 3; done
echo "TD_FPS $(grep -a 'frames per second' /root/doom/td.log | tail -1)"
echo "TD_STILL_RUNNING $(ps | grep -c '[p]rboom')"
echo "TD_ALARMS $(dmesg | grep -aicE 'oops|unhandled signal|rcu:.*(stall|starved)|vblank wait timed out|Out of memory|BUG:')"
echo "TD_PRE $(head -c 300 /root/doom/td.pre 2>/dev/null | tr '\n' ' ')"
echo "TD_PINNED $(dmesg | grep -ac 'pinned to CPU0')"
# The load an arm asked for is not evidence of the load it got (the load arm
# is bimodal, 2026-09-21): total bytes received since boot, and PIE bounces.
echo "TD_RX $(cat /sys/class/net/wlan0/statistics/rx_bytes 2>/dev/null)"
echo "TD_BOUNCES $(cat /sys/module/kernel/parameters/esp32s31_pie_bounces 2>/dev/null) sil_stuck=$(cat /sys/module/kernel/parameters/esp32s31_sil_stuck 2>/dev/null)"
amixer -q sset 'DACL' 143 2>/dev/null; amixer -q sset 'DACR' 143 2>/dev/null
echo TD_COLLECTED
