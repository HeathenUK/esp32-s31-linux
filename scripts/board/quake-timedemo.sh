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
# taken as the writable game dir $HOME/<argv1>, so "id1" + HOME=/root/quake puts
# saves and qconsole.log in /root/quake/id1; the default 128 MB heap cannot start.
# Everything after <label> is passed to Quake before the fixed resolution.
# QUAKE_BIN picks the binary: ./name (in /root/quake, i.e. on SD) or an absolute
# path such as /usr/bin/tyrquake (in XIP flash). Default ./tiopex-quake.
#
# SOUND RATE IS THE BIGGEST CONFOUND. The menu launches Quake with
# AUDIODEV=s31route_11k (mixes at its native 11025 Hz); through "default" it
# is granted 48 kHz, mixes 4.4x the samples and pages a 4.4x sound cache.
# Same kernel, same day (2026-09-23, #353): 19.8 fps at 11 kHz, 14.2 at
# 48 kHz, 20.6 with -nosound. Runs made before this default were passed
# PRE='export AUDIODEV=s31route_11k'; after a context reset that was dropped
# and 14 fps was chased as a kernel regression for an hour. So the harness
# now sets it itself and prints AUDIO=<device>. QUAKE_AUDIODEV=default
# measures the 48 kHz arm on purpose.
#
# ~1 min boot + ~100-170 s demo. Checks the panel is not black afterwards.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; shift
ARGS="$*"
PRE=${PRE:-:}	# a board-side command run before Quake starts, e.g. a sysctl
OUT=artifacts/quake/td-$L-$(date +%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
AUDIODEV_ON_BOARD=${QUAKE_AUDIODEV:-s31route_11k}
cat > "$S" <<EOF
cd /root/quake
export AUDIODEV=$AUDIODEV_ON_BOARD; echo "AUDIO=\$AUDIODEV"
$PRE
echo "PRE_APPLIED watermark_scale_factor=\$(cat /proc/sys/vm/watermark_scale_factor) page-cluster=\$(cat /proc/sys/vm/page-cluster)"
rm -f /root/-basedir/qconsole.log /root/quake/id1/qconsole.log
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
setsid sh -c 'DISPLAY=:0 HOME=/root/quake exec ${QUAKE_BIN:-./tiopex-quake} id1 -basedir /root/quake $ARGS -width 320 -height 240 -fullscreen -condebug +timedemo demo1 >/root/quake/td.log 2>&1' </dev/null >/dev/null 2>&1 &
# QUIET WAIT. This loop used to poll every second - three busybox forks a
# second for the whole demo, on a board at 300-700 kB MemAvailable where a
# fork can evict a Quake page. Measured 2026-09-23: the same kernel read
# 15.9 fps through this harness and 18.5-18.9 with the demo left alone
# (cachecnt-quake.sh polls only after 40 s). The harness was the lottery.
# So: one long sleep that covers the load and most of the demo, then a slow
# poll. Error exit stays quick because it is checked once before the sleep.
sleep 8; grep -aq "^Error:" /root/quake/td.log 2>/dev/null || sleep 42
i=0; while [ \$i -lt 60 ]; do grep -aqE "[0-9]+ frames" /root/quake/id1/qconsole.log 2>/dev/null && break; grep -aq "^Error:" /root/quake/td.log 2>/dev/null && break; sleep 3; i=\$((i+1)); done
P=\$(ps | awk '/[t]iopex|[t]yr-quake|[t]yrquake/ {print \$1}' | head -1)
echo "RESULT \$(grep -ahE '[0-9]+ frames' /root/-basedir/qconsole.log /root/quake/id1/qconsole.log 2>/dev/null | head -1)"
echo "ERROR \$(grep -a '^Error:' /root/quake/td.log 2>/dev/null | head -1)"
echo "QUAKE majflt=\$(awk '{print \$12}' /proc/\$P/stat 2>/dev/null) \$(grep -aE 'VmRSS|VmSwap' /proc/\$P/status 2>/dev/null | tr -s ' ' | tr '\n' ' ')"
echo "MEM \$(grep -aE 'MemAvailable|SwapFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')"
echo "SCANOUT_FAIL \$(dmesg | grep -ac 'failed to start scanout')"
echo "SDSTAT \$(cat /sys/block/mmcblk0/stat | tr -s ' ')"
echo "KNOBS vma_ra=\$(cat /sys/kernel/mm/swap/vma_ra_enabled 2>/dev/null) page-cluster=\$(cat /proc/sys/vm/page-cluster)"
EOF
cat > "$OUT/clean.sh" <<'EOF'
for p in $(ps | awk '/[t]iopex|[s]dlquake|[t]yr-quake|[t]yrquake/ {print $1}'); do kill -9 $p 2>/dev/null; done
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
grep -aE '^(AUDIO=|PRE_APPLIED|QUAKE|MEM|SCANOUT_FAIL|SDSTAT|KNOBS)' "$OUT/run.log" | sed 's/^/     /'
echo "     panel: $OUT/panel.png"
