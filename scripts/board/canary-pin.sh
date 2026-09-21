#!/bin/bash
# canary-pin.sh [reps] - does a PINNED lvdesk explain the canary's per-boot
# bimodality? A/B/A on ONE boot, so nothing depends on a lucky reset.
#
# Background: the same SMP kernel has read 12.69, 13.97, 14.02, 14.77 and
# 16.80 ms on the windowed canary across five boots - whole distributions
# apart, ~1% repeatable within a session. `esp32s31_pie_bounce=N` PERMANENTLY
# pins any task that executes PIE more than N times in a second, and whether
# that catches lvdesk while the desktop starts is luck of the boot. A pinned
# lvdesk cannot migrate, and forced placement measured ~15% slower than
# letting the scheduler choose (canary-placement.sh, 2026-09-21).
#
#   free    lvdesk mask 3  - what an unlucky boot does NOT get
#   pinned  lvdesk mask 1  - what the PIE pin leaves behind
set -u
cd "$(dirname "$0")/../.."
REP=${1:-3}
OUT=artifacts/smp-finish/canarypin-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/arm.sh
echo "canary-pin: $REP runs per arm -> $OUT"

for arm in free pinned free2 pinned2; do
	case $arm in free|free2) M=3 ;; *) M=1 ;; esac
	cat > "$S" <<EOF
L=\$(ps | awk '/[l]vdesk/ {print \$1}' | head -1)
for t in \$(ls /proc/\$L/task 2>/dev/null); do /root/oncpu -s $M \$t 2>/dev/null; done
echo "ARM $arm lvdesk=\$L mask=\$(/root/oncpu -p \$L | awk '{print \$2}')"
cd /root
i=0; while [ \$i -lt $REP ]; do
  DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
  i=\$((i+1))
done
echo ARM_DONE
EOF
	python3 scripts/board/runsh.py "$S" 150 15 > "$OUT/$arm.log" 2>&1
	echo "  $(grep -a '^ARM ' "$OUT/$arm.log" | head -1)"
	python3 - "$OUT/$arm.log" "$arm" <<'PY'
import json, re, statistics, sys
means = []
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'\{.*"type":\s*"measure".*\}', l)
    if m:
        try:
            means.append(json.loads(m.group(0))["mean_ns"] / 1e6)
        except Exception:
            pass
print("    %-7s median %s ms  n=%d  [%s]" % (
    sys.argv[2],
    ("%.3f" % statistics.median(means)) if means else "NONE",
    len(means), " ".join("%.2f" % v for v in means)))
PY
done
echo "logs: $OUT"
