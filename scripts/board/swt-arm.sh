#!/bin/bash
# swt-arm.sh <label> <runs> [lvdesk-binary] [LVENV] : one FRESH-BOOT arm of
# the glxgears fullscreen per-frame capture (gears-swt.sh on the board,
# rootfs/swapstamp.so in the client), then the windowed fast-present check
# (gl-arm.sh: glxgears 300x300, 5 x 10 s, fps and lvdesk CPU) on the same
# boot. LVENV (e.g. LVDESK_LENTCPU=scalar) goes to the desktop in both. reset.py, wait for 60 s of uptime,
# fire <runs> gears-swt.sh windows back to back (setsid), poll until done,
# then fetch every file over Wi-Fi (/root/s31-serve, the screenshot-hw.py
# path - not base64 over the console, which hart0 interleaves) into
# artifacts/gl/dips/arms/<label>-<stamp>/ and print swt-analyse.py's summary.
# Budget ~85 s boot + ~100 s per run: keep <runs> at 2 (the 10-minute rule).
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; N=${2:?runs}; BIN=${3:-/usr/bin/lvdesk}; LVE=${4:-}
OUT=artifacts/gl/dips/arms/$L-$(date +%m%d-%H%M%S); mkdir -p "$OUT"
T=$(mktemp -d)
python3 scripts/board/reset.py > "$OUT/reset.log" 2>&1
cat > $T/fire.sh <<X
cat > /root/gq/swarm-$L.sh <<'Y'
export LVENV="$LVE"
i=1; while [ \$i -le $N ]; do sh /root/gears-swt.sh $L-r\$i 1500 300 $BIN; i=\$((i+1)); done
sh /root/gl-arm.sh $L-win 1 2 win 5 $BIN; cp /root/glarm-$L-win.txt /root/gq/swglarm-$L-r0.txt
echo ARMDONE > /root/gq/swarm-$L.done
Y
mkdir -p /root/gq; rm -f /root/gq/swarm-$L.done
setsid sh /root/gq/swarm-$L.sh </dev/null >/dev/null 2>&1 &
echo FIRED \$(uname -v) up \$(cut -d' ' -f1 /proc/uptime)
X
python3 scripts/board/runsh.py $T/fire.sh 60 60 | grep -a FIRED | tee "$OUT/fired.txt"
cat > $T/wait.sh <<X
sleep $((N * 80 + 90)); i=0; while [ \$i -lt 36 ]; do [ -f /root/gq/swarm-$L.done ] && break; sleep 10; i=\$((i+1)); done
[ -f /root/gq/swarm-$L.done ] && echo ARM_FINISHED || echo ARM_RUNNING
X
while :; do
	r=$(python3 scripts/board/runsh.py $T/wait.sh 400 | grep -a "ARM_FINISHED\|ARM_RUNNING" | tail -1 | tr -d "\r")
	echo "poll: $r"; [ "$r" = ARM_FINISHED ] && break
	[ -z "$r" ] && { echo "poll returned nothing - check alive.py"; exit 1; }
done
cat > $T/serve.sh <<X
cd /root/gq && tar cf - sw[tcmn]*-$L-r*.txt swglarm-$L-r0.txt 2>/dev/null | gzip -1 > /tmp/_swarm.tgz
echo "NETIP \$(busybox ip -o -4 addr show wlan0 | busybox awk '{print \$4}' | cut -d/ -f1)"
setsid /root/s31-serve /tmp/_swarm.tgz 8139 >/dev/null 2>&1 </dev/null &
sleep 0.2; echo SERVING
X
IP=$(python3 scripts/board/runsh.py $T/serve.sh 30 | tr -d "\r" | awk '/^NETIP [0-9]/{print $2}' | tail -1)
[ -n "$IP" ] || { echo "no board address"; exit 1; }
curl --fail -s --connect-timeout 3 --max-time 30 "http://$IP:8139/x" -o "$T/a.tgz" && tar xzf "$T/a.tgz" -C "$OUT"
i=1; while [ $i -le $N ]; do
	echo "== $L r$i"; python3 scripts/board/swt-analyse.py "$OUT/swt-$L-r$i.txt" | sed -n 1,4p
	python3 scripts/board/swt-migr.py "$OUT" "$L-r$i" | sed -n "1,4p;/PIE/p"
	i=$((i+1))
done
grep -a "^RUN\|^ARM" "$OUT/swglarm-$L-r0.txt"
echo "$OUT"
