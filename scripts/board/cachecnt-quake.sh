#!/bin/bash
# cachecnt-quake.sh <label> [window_s=10] - the L1 cache counters over a window
# in the middle of a fresh-boot Quake timedemo, plus the fps.
#
# What it answers: how much of CPU0's fetch time is STALL during a real game,
# I-side and D-side, and how many line refills per second - the numbers every
# .text..fast decision has been made without (perf-review-2026-09-23 T1.5).
# Semantics established 2026-09-23: hit+miss count ~one event per bus cycle,
# so miss/(hit+miss) is the stalled fraction; nxtlvl_rd is the refill count.
# The counters wrap in ~13 s at full rate, hence the 10 s window.
#
# Run it on several boots: a slow boot (the +-10% lottery) with a different
# stall fraction or refill rate at the same fps is the first physical
# signature that lottery has ever produced.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; W=${2:-10}
OUT=artifacts/perf-plan/ccq-$L-$(date +%H%M%S); mkdir -p "$OUT"
cat > "$OUT/launch.sh" <<'EOF'
cd /root/quake; rm -f /root/quake/id1/qconsole.log
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
setsid sh -c 'DISPLAY=:0 HOME=/root/quake AUDIODEV=s31route_11k exec ./tiopex-quake id1 -basedir /root/quake -mem 10 -width 320 -height 240 -fullscreen -condebug +timedemo demo1 >/root/quake/td.log 2>&1' </dev/null >/dev/null 2>&1 &
echo LAUNCHED
EOF
cat > "$OUT/finish.sh" <<'EOF'
i=0; while [ $i -lt 120 ]; do grep -aqE "[0-9]+ frames" /root/quake/id1/qconsole.log 2>/dev/null && break; sleep 1; i=$((i+1)); done
echo "RESULT $(grep -ahE '[0-9]+ frames' /root/quake/id1/qconsole.log 2>/dev/null | head -1)"
P=$(ps | awk '/[t]iopex/ {print $1}' | head -1); echo "QUAKE majflt=$(awk '{print $12}' /proc/$P/stat 2>/dev/null)"
for p in $(ps | awk '/[t]iopex/ {print $1}'); do kill -9 $p; done
amixer -q sset 'DACL' 178 2>/dev/null; amixer -q sset 'DACR' 178 2>/dev/null
echo FIN_DONE
EOF
python3 scripts/board/reset.py >/dev/null 2>&1
python3 scripts/board/runsh.py "$OUT/launch.sh" 120 100 2>&1 | tr -d '\r' | grep -aE "LAUNCHED|NO_SHELL"
sleep 25
bash scripts/board/cachecnt.sh "$L-mid" "sleep $W" 2>&1 | grep -aE "^(IBUS|DBUS|ELAPSED)|NO_SHELL" | tee "$OUT/counters.txt"
python3 scripts/board/runsh.py "$OUT/finish.sh" 150 130 2>&1 | tr -d '\r' | grep -aE "^(RESULT|QUAKE|FIN_DONE)|NO_SHELL|KILLED" | tee "$OUT/result.txt"
echo "out: $OUT"
