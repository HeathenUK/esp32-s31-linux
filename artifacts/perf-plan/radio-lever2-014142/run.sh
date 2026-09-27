
echo "LINK $(iw wlan0 link 2>/dev/null | grep -aiE 'signal|bitrate' | tr -s ' 	' ' ' | tr '\n' ' ')"
# /root, not /tmp: tmpfs on a 15 MB box cannot hold the blob. And wc, not
# stat: busybox here has no `stat -c`, which read every download as 0 bytes.
T0=$(cut -d' ' -f1 /proc/uptime); wget http://192.168.1.226:52439/blob.bin -O /root/blob.bin >/root/wget.err 2>&1; T1=$(cut -d' ' -f1 /proc/uptime)
S=$(wc -c < /root/blob.bin 2>/dev/null | tr -d " "); S=${S:-0}; [ "$S" = 0 ] && echo "WGET_ERR $(head -c 200 /root/wget.err)"; rm -f /root/blob.bin /root/wget.err
echo "WGET bytes=$S secs=$(echo "$T1 - $T0" | bc 2>/dev/null || awk -v a=$T0 -v b=$T1 'BEGIN{print b-a}') kBps=$(awk -v s=$S -v a=$T0 -v b=$T1 'BEGIN{ if (b>a) printf "%.0f", s/1024/(b-a); else print 0 }')"
echo "PING $(ping -c 30 -i 0.2 -W 1 192.168.1.226 2>/dev/null | grep -aE 'packet loss|round-trip|rtt' | tr '\n' ' ')"
P0=$(bluetoothctl show 2>/dev/null | grep -a Powered | awk '{print $2}')
[ "$P0" = yes ] || bluetoothctl power on >/dev/null 2>&1; sleep 2
echo "BT $(timeout 16 bluetoothctl --timeout 10 scan on 2>/dev/null | grep -ac 'Device') devices in a 10 s scan; powered_before=$P0 powered_now=$(bluetoothctl show 2>/dev/null | grep -a Powered | awk '{print $2}')"
[ "$P0" = yes ] || bluetoothctl power off >/dev/null 2>&1
echo RC_DONE
