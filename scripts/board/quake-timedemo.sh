#!/bin/bash
# quake-timedemo.sh <label> [quake args...] - one TyrQuake timedemo on a FRESH
# BOOT, reporting fps plus the paging numbers that decide it.
#
#   scripts/board/quake-timedemo.sh mem12 -mem 12
#   scripts/board/quake-timedemo.sh mem10 -mem 10
#
# Why fresh boot: on a long-running board the same invocation fell from
# 9.5-11 fps to 5.8 fps (2026-09-21) because swap was already full of earlier
# runs. Why these numbers: TyrQuake here is PAGING-bound - its heap sits in SD
# swap and ~74% of CPU0 goes to the kernel faulting it back - so fps without
# majflt and VmSwap does not say why a run was fast or slow.
#
# Invocation facts (tiopex-quake = TyrQuake 0.62): the FIRST argument is
# swallowed as a game directory, so -basedir goes first; -condebug logs to
# /root/-basedir/qconsole.log; the default heap is 128 MB and cannot start.
# Everything after <label> is passed to Quake before the fixed resolution.
#
# ~1 min boot + ~100-170 s demo. Checks the panel is not black afterwards.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; shift
ARGS="$*"
OUT=artifacts/quake/td-$L-$(date +%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<EOF
cd /root/quake
rm -f /root/-basedir/qconsole.log
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
setsid sh -c 'DISPLAY=:0 exec ./tiopex-quake -basedir /root/quake $ARGS -width 320 -height 240 -fullscreen -condebug +timedemo demo1 >/root/quake/td.log 2>&1' </dev/null >/dev/null 2>&1 &
i=0; while [ \$i -lt 200 ]; do grep -aqE "[0-9]+ frames" /root/-basedir/qconsole.log 2>/dev/null && break; grep -aq "^Error:" /root/quake/td.log 2>/dev/null && break; sleep 1; i=\$((i+1)); done
P=\$(ps | awk '/[t]iopex/ {print \$1}' | head -1)
echo "RESULT \$(grep -aE '[0-9]+ frames' /root/-basedir/qconsole.log 2>/dev/null | head -1)"
echo "ERROR \$(grep -a '^Error:' /root/quake/td.log 2>/dev/null | head -1)"
echo "QUAKE majflt=\$(awk '{print \$12}' /proc/\$P/stat 2>/dev/null) \$(grep -aE 'VmRSS|VmSwap' /proc/\$P/status 2>/dev/null | tr -s ' ' | tr '\n' ' ')"
echo "MEM \$(grep -aE 'MemAvailable|SwapFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')"
echo "SCANOUT_FAIL \$(dmesg | grep -ac 'failed to start scanout')"
EOF
cat > "$OUT/clean.sh" <<'EOF'
for p in $(ps | awk '/[t]iopex|[s]dlquake/ {print $1}'); do kill -9 $p 2>/dev/null; done
amixer -q sset 'DACL' 178 2>/dev/null; amixer -q sset 'DACR' 178 2>/dev/null
echo CLEAN
EOF

python3 scripts/board/reset.py >/dev/null 2>&1
python3 scripts/board/runsh.py "$S" 240 170 > "$OUT/run.log" 2>&1
python3 scripts/board/screenshot.py "$OUT/panel.png" > /dev/null 2>&1
python3 scripts/board/runsh.py "$OUT/clean.sh" 20 20 > /dev/null 2>&1
fps=$(sed -n 's/.* \([0-9.]*\) fps.*/\1/p' "$OUT/run.log" | head -1)
echo "[$L] fps=${fps:-NONE}  $(grep -a '^RESULT' "$OUT/run.log" | cut -c8-)"
grep -a '^ERROR [^ ]' "$OUT/run.log" | sed 's/^/     /'
grep -aE '^(QUAKE|MEM|SCANOUT_FAIL)' "$OUT/run.log" | sed 's/^/     /'
echo "     panel: $OUT/panel.png"
