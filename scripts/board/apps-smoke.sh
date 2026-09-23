#!/bin/bash
# apps-smoke.sh <label> - the "did the desktop apps regress" pass that has no
# timedemo: xcalc launch-to-window, then OpenTyrian's title animation for 20 s
# with lvdesk's frame-gap histogram (SIGUSR1 -> /var/log/lvdesk.log) and a
# screenshot of each. Numbers go next to the previous pass in
# docs/perf-plan-2026-09-23.md; a black panel or a missing window is the
# regression this exists to catch. Run on a booted, idle desktop. ~1.5 min.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}
OUT=artifacts/perf-plan/apps-$L-$(date +%H%M%S); mkdir -p "$OUT"
cat > "$OUT/xcalc.sh" <<'EOS'
L=$(ps | awk '/[l]vdesk/ {print $1}' | head -1)
T0=$(cut -d' ' -f1 /proc/uptime)
DISPLAY=:0 setsid xcalc </dev/null >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ]; do grep -aq "xcalc" /proc/*/comm 2>/dev/null && [ -n "$(ps | grep '[x]calc')" ] && break; sleep 0.25; i=$((i+1)); done
sleep 3
echo "XCALC pid=$(ps | awk '/[x]calc/ {print $1}' | head -1) launched_at=$T0 now=$(cut -d' ' -f1 /proc/uptime) mem=$(grep -a MemAvailable /proc/meminfo | tr -s ' ')"
echo XC_DONE
EOS
cat > "$OUT/tyrian.sh" <<'EOS'
for p in $(ps | awk '/[x]calc/ {print $1}'); do kill $p; done
L=$(ps | awk '/[l]vdesk/ {print $1}' | head -1)
# Do NOT truncate /var/log/lvdesk.log: the gate's smoke reads it for the
# vt_takeover line and the frame log, and an emptied log failed 3 of 24 checks
# on 2026-09-23. Take the histogram before and after and diff on the host.
kill -USR1 $L; sleep 1; grep -a "gaps" /var/log/lvdesk.log | tail -1 | sed 's/^/GAPS0 /'
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
cd /root/oty/usr/share/opentyrian/data && HOME=/root DISPLAY=:0 setsid /root/oty/usr/bin/opentyrian --no-joystick </dev/null >/root/oty.log 2>&1 &
sleep 22
kill -USR1 $L; sleep 1
echo "TYRIAN pid=$(ps | awk '/[o]pentyrian/ {print $1}' | head -1)"
grep -a "gaps" /var/log/lvdesk.log | tail -1 | sed 's/^/GAPS1 /'
echo TY_DONE
EOS
cat > "$OUT/clean.sh" <<'EOS'
for p in $(ps | awk '/[o]pentyrian|[x]calc/ {print $1}'); do kill -9 $p; done
amixer -q sset 'DACL' 178 2>/dev/null; amixer -q sset 'DACR' 178 2>/dev/null
echo CL_DONE
EOS
python3 scripts/board/runsh.py "$OUT/xcalc.sh" 40 20 2>&1 | tr -d '\r' | grep -aE "^XCALC|NO_SHELL"
python3 scripts/board/screenshot.py "$OUT/xcalc.png" >/dev/null 2>&1 && echo "  panel: $OUT/xcalc.png"
python3 scripts/board/runsh.py "$OUT/tyrian.sh" 60 40 2>&1 | tr -d '\r' | grep -aE "^(TYRIAN|GAPS)|NO_SHELL" | tee "$OUT/tyrian.txt"
python3 - "$OUT/tyrian.txt" <<'PY'
import re,sys
t=open(sys.argv[1]).read(); h={}
for tag in ('GAPS0','GAPS1'):
    m=re.search(tag+r'.*frames (\d+)\s+gaps <25:(\d+) <50:(\d+) <100:(\d+) <200:(\d+) <400:(\d+) >=400:(\d+)',t)
    if m: h[tag]=[int(x) for x in m.groups()]
if len(h)==2:
    d=[b-a for a,b in zip(h['GAPS0'],h['GAPS1'])]
    print("  tyrian frames %d  gaps <25:%d <50:%d <100:%d <200:%d <400:%d >=400:%d (delta over the run)" % tuple(d))
PY
python3 scripts/board/screenshot.py "$OUT/tyrian.png" >/dev/null 2>&1 && echo "  panel: $OUT/tyrian.png"
python3 scripts/board/runsh.py "$OUT/clean.sh" 20 20 2>&1 | tr -d '\r' | grep -aE "CL_DONE|NO_SHELL"
