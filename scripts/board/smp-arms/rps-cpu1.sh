f=/sys/class/net/wlan0/queues/rx-0/rps_cpus
[ -w $f ] && echo 2 > $f
echo "rps_cpus=$(cat $f 2>/dev/null || echo NO_RPS_IN_THIS_KERNEL)"
