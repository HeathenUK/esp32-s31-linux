#!/bin/sh
# Board-side eligibility gate. All work happens BEFORE benchmark timing.
# Three consecutive 2-second windows: associated, same IPv4, >=85% CPU idle.
# /proc/uptime supplies the deadline; wall-clock/NTP adjustments cannot extend it.
set -eu
budget=${1:-60}
start=$(cut -d. -f1 /proc/uptime)
stable=0
last_ip=
while :; do
    now=$(cut -d. -f1 /proc/uptime)
    [ "$((now-start))" -lt "$budget" ] || { echo 'SETTLE_FAIL deadline'; exit 1; }
    state=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')
    address=$(ip -o -4 addr show wlan0 2>/dev/null | awk '{print $4}')
    read -r _ u n s idle wait irq soft steal rest < /proc/stat
    total=$((u+n+s+idle+wait+irq+soft+steal))
    sleep 2
    read -r _ u2 n2 s2 idle2 wait2 irq2 soft2 steal2 rest < /proc/stat
    total2=$((u2+n2+s2+idle2+wait2+irq2+soft2+steal2))
    delta=$((total2-total))
    idle_pc=0
    [ "$delta" -le 0 ] || idle_pc=$(((idle2-idle)*100/delta))
    state2=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')
    address2=$(ip -o -4 addr show wlan0 2>/dev/null | awk '{print $4}')
    if [ "$state" = COMPLETED ] && [ "$state2" = COMPLETED ] &&
       [ -n "$address" ] && [ "$address" = "$address2" ] &&
       [ "$idle_pc" -ge 85 ]; then
        if [ "$last_ip" = "$address" ]; then stable=$((stable+1)); else stable=1; fi
    else
        stable=0
    fi
    last_ip=$address2
    echo "SETTLE_SAMPLE state=$state2 ip=$address2 idle_percent=$idle_pc stable=$stable uptime=$now"
    [ "$stable" -lt 3 ] || { echo SETTLE_OK; exit 0; }
done
