#!/bin/bash
# canary-boots.sh <boots> [runs] - the windowed canary across FRESH BOOTS, with
# the two numbers that decide which regime each boot landed in.
#
# The per-boot bimodality chased on 2026-09-21 was not noise: any task the PIE
# trap policy pinned to CPU0 for good could not migrate afterwards, and a
# pinned lvdesk measured 15.3% slower (canary-pin.sh). Whether it caught lvdesk
# while the desktop started was luck of the boot, so the same kernel read
# 12.69-16.80 ms. This reports `pinned` and the number of processes left with
# mask 1 beside every canary number, so a slow boot names its own reason
# instead of looking like a flaky board.
set -u
cd "$(dirname "$0")/../.."
N=${1:?boots}; REP=${2:-3}
OUT=artifacts/smp-finish/canaryboots-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<'EOF'
uname -v | cut -c1-20
L=$(ps | awk '/[l]vdesk/ {print $1}' | head -1)
n=0; for p in $(ps | awk '!/\[|awk|ps$/ {print $1}' | grep -v PID); do
	[ "$(/root/oncpu -p $p 2>/dev/null | awk '{print $2}')" = "1" ] && n=$((n+1))
done
echo "STATE lvdesk_mask=$(/root/oncpu -p $L | awk '{print $2}') pinned=$(dmesg | grep -ac 'pinned to CPU0') stuck=$n"
# How much CPU1 does Linux actually GET? It is a FreeRTOS task on hart0 sharing
# that hart with the radios and the hosted transport, so a boot where Wi-Fi or
# Bluetooth is busier hands Linux less of it - a per-boot, whole-run difference.
grep -E "^cpu[01] " /proc/stat | sed 's/^/STAT0 /'
echo "WIFI $(cat /sys/class/net/wlan0/operstate 2>/dev/null) $(cat /sys/class/net/wlan0/statistics/rx_packets 2>/dev/null)"
cd /root; i=0
while [ $i -lt REPS ]; do
	DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
	i=$((i+1))
done
grep -E "^cpu[01] " /proc/stat | sed 's/^/STAT1 /'
echo "WIFI2 $(cat /sys/class/net/wlan0/statistics/rx_packets 2>/dev/null)"
echo RUN_DONE
EOF
sed -i '' "s/REPS/$REP/" "$S" 2>/dev/null || sed -i "s/REPS/$REP/" "$S"

echo "canary-boots: $N fresh boots x $REP runs -> $OUT"
for ((b = 1; b <= N; b++)); do
	python3 scripts/board/reset.py >/dev/null 2>&1
	python3 scripts/board/runsh.py "$S" 150 110 > "$OUT/boot-$b.log" 2>&1
	python3 - "$OUT/boot-$b.log" "$b" <<'PY'
import json, re, statistics, sys
txt = open(sys.argv[1], errors='replace').read()
st = re.search(r'STATE \S+ \S+ \S+', txt)
def cpu(tag, which):
    m = re.search(r'%s cpu%d ((?:\d+ ){8,})' % (tag, which), txt)
    return [int(x) for x in m.group(1).split()] if m else None
busy = ""
a0, b0, a1, b1 = cpu('STAT0', 0), cpu('STAT1', 0), cpu('STAT0', 1), cpu('STAT1', 1)
if a0 and b0 and a1 and b1:
    d0 = [y - x for x, y in zip(a0, b0)]
    d1 = [y - x for x, y in zip(a1, b1)]
    t0, t1 = sum(d0), sum(d1)
    busy = " cpu0_busy=%d%% cpu1_busy=%d%% cpu1_ticks=%d" % (
        100 * (t0 - d0[3]) / t0 if t0 else 0,
        100 * (t1 - d1[3]) / t1 if t1 else 0, t1)
rx = re.findall(r'WIFI2? \S*\s*(\d+)', txt)
if len(rx) >= 2:
    busy += " wifi_rx=%d" % (int(rx[1]) - int(rx[0]))
means = [json.loads(m.group(0))["mean_ns"] / 1e6
         for m in re.finditer(r'\{.*?"type":\s*"measure".*?\}', txt)]
print("  boot %s: %s%s  canary %s ms  [%s]" % (
    sys.argv[2], st.group(0) if st else "STATE ?", busy,
    ("%.3f" % statistics.median(means)) if means else "NONE",
    " ".join("%.2f" % v for v in means)))
PY
done
echo "logs: $OUT"
