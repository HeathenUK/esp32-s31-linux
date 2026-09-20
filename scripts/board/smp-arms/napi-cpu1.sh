f=/sys/class/net/wlan0/threaded
[ -w $f ] && echo 1 > $f
sleep 1
for p in $(ps | awk '/napi\/wlan0/ && !/awk/ {print $1}'); do taskset -p 2 $p >/dev/null 2>&1; echo "napi_pid=$p affinity=$(taskset -p $p 2>/dev/null | sed 's/.*: //')"; done
echo "threaded=$(cat $f 2>/dev/null)"
