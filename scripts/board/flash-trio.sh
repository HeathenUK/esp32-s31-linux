#!/bin/bash
# flash-trio.sh <dir> - flash a loader + OpenSBI + kernel trio from <dir> with
# the RAW esptool command (make flash-* runs sync-images, which replaces
# images/ with the volume's current builds - memory s31-clock-arm-traps),
# reset, and prove the clock with the host/board ratio test.
set -u
cd "$(dirname "$0")/../.."
D=${1:?dir with hello_world.bin fw_payload.bin xipImage}
ESPTOOL=/Users/gadyke/.espressif/python_env/idf6.0_py3.12_env/bin/esptool
for f in hello_world.bin fw_payload.bin xipImage; do [ -r "$D/$f" ] || { echo "missing $D/$f"; exit 2; }; done
for pair in "0x20000 hello_world.bin" "0x380000 fw_payload.bin" "0x400000 xipImage"; do
	set -- $pair
	echo "--- $2 -> $1"
	python3 scripts/board/withlock.py --wait 900 -- $ESPTOOL -p /dev/cu.usbserial-130 -b 2000000 write-flash $1 "$D/$2" 2>&1 | grep -aE "verified|rror" | head -1
done
python3 scripts/board/alive.py --reset --timeout 120 2>&1 | grep -aE "^STAGE|DOES NOT BOOT" | tail -1
python3 - <<'PY'
import subprocess, time, re
def bt():
    s='/tmp/s31-bt.sh'; open(s,'w').write('uname -v; echo "BT $(cut -d\\" \\" -f1 /proc/uptime)"; echo BT_DONE\n')
    h0=time.time(); out=subprocess.run(['python3','scripts/board/runsh.py',s,'30','20'],capture_output=True,text=True).stdout; h1=time.time()
    m=re.search(r'BT ([\d.]+)',out); u=re.search(r'#\d+ SMP',out)
    return (h0+h1)/2, float(m.group(1)) if m else None, u.group(0) if u else '?'
h0,b0,u=bt(); time.sleep(40); h1,b1,_=bt()
r=(b1-b0)/(h1-h0)
print("UNAME %s  CLOCK ratio board/host = %.3f  %s" % (u, r, "TRUE" if abs(r-1)<0.02 else "MISMATCH - a timebase does not match the real clock"))
PY
