#!/usr/bin/env python3
"""radio-check.py <label> - are Wi-Fi and Bluetooth still reliable at this clock?

Run once at 320 and once at the overclock, same session (memory
overclock-verify-radios: on older ESP32s the PLLs had to stay at fixed rates
for the radio; on the S31 the CPU is on CPLL and the radios on BBPLL, which is
the hypothesis this tests). Reports, from the board:
  - iw wlan0 link (signal, bitrate)
  - a timed wget of a 3 MB file served from the host  -> KB/s
  - 30 pings to the host at 0.2 s                       -> loss %
  - a 10 s bluetoothctl scan                            -> devices seen
Uses deploy.py's helpers for the board/host addresses and the throwaway HTTP
server. Output goes to artifacts/perf-plan/radio-<label>-<time>/.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deploy  # noqa: E402

label = sys.argv[1] if len(sys.argv) > 1 else "run"
out = os.path.join(HERE, "..", "..", "artifacts", "perf-plan", "radio-%s-%s" % (label, time.strftime("%H%M%S")))
os.makedirs(out, exist_ok=True)
blob = os.path.join(out, "blob.bin")
with open(blob, "wb") as f:
    f.write(os.urandom(2 * 1024 * 1024))

ip = deploy.board_ip()
if not ip:
    sys.exit("radio-check: no board IP (is wlan0 up?)")
httpd, port = deploy.serve_dir(out)
host = deploy.host_ip_for(ip)
url = "http://%s:%d/blob.bin" % (host, port)
# Self-test from the host first: if the host cannot fetch its own server the
# board never could, and the failure is ours, not the radio's.
import urllib.request  # noqa: E402
try:
    n = len(urllib.request.urlopen(url, timeout=5).read())
    print("  host self-fetch: %d bytes from %s" % (n, url))
except Exception as e:  # noqa: BLE001
    print("  host self-fetch FAILED: %s (%s)" % (e, url))
script = os.path.join(out, "run.sh")
open(script, "w").write("""
echo "LINK $(iw wlan0 link 2>/dev/null | grep -aiE 'signal|bitrate' | tr -s ' \t' ' ' | tr '\\n' ' ')"
# /root, not /tmp: tmpfs on a 15 MB box cannot hold the blob. And wc, not
# stat: busybox here has no `stat -c`, which read every download as 0 bytes.
T0=$(cut -d' ' -f1 /proc/uptime); wget %s -O /root/blob.bin >/root/wget.err 2>&1; T1=$(cut -d' ' -f1 /proc/uptime)
S=$(wc -c < /root/blob.bin 2>/dev/null | tr -d " "); S=${S:-0}; [ "$S" = 0 ] && echo "WGET_ERR $(head -c 200 /root/wget.err)"; rm -f /root/blob.bin /root/wget.err
echo "WGET bytes=$S secs=$(echo "$T1 - $T0" | bc 2>/dev/null || awk -v a=$T0 -v b=$T1 'BEGIN{print b-a}') kBps=$(awk -v s=$S -v a=$T0 -v b=$T1 'BEGIN{ if (b>a) printf "%%.0f", s/1024/(b-a); else print 0 }')"
echo "PING $(ping -c 30 -i 0.2 -W 1 %s 2>/dev/null | grep -aE 'packet loss|round-trip|rtt' | tr '\\n' ' ')"
P0=$(bluetoothctl show 2>/dev/null | grep -a Powered | awk '{print $2}')
[ "$P0" = yes ] || bluetoothctl power on >/dev/null 2>&1; sleep 2
echo "BT $(timeout 16 bluetoothctl --timeout 10 scan on 2>/dev/null | grep -ac 'Device') devices in a 10 s scan; powered_before=$P0 powered_now=$(bluetoothctl show 2>/dev/null | grep -a Powered | awk '{print $2}')"
[ "$P0" = yes ] || bluetoothctl power off >/dev/null 2>&1
echo RC_DONE
""" % (url, host))
import runsh  # noqa: E402
# In-process, like deploy.py: a subprocess runsh collided with the port lock
# this process already used for board_ip() and reported SERIAL PORT BUSY.
try:
    txt = (runsh.run(script, timeout=120) or "").replace("\r", "")
except Exception as e:  # noqa: BLE001
    txt = "Error: %s" % e
finally:
    httpd.shutdown()
open(os.path.join(out, "run.log"), "w").write(txt)
for line in txt.split("\n"):
    if line.startswith(("LINK", "WGET", "PING", "BT ", "RC_DONE")) or "NO_SHELL" in line or "BUSY" in line or "Error" in line or "Traceback" in line:
        print("  " + line.strip())
print("log:", os.path.join(out, "run.log"))
