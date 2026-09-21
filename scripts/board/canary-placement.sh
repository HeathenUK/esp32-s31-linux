#!/bin/bash
# canary-placement.sh [repeats] - is the SDL canary's per-boot bimodality TASK
# PLACEMENT? Measured deterministically, on ONE boot, instead of waiting for a
# lucky one.
#
# Why this exists: the same SMP kernel gave 12.55-12.84 ms on one boot and
# 14.43-15.39 on another - whole distributions, not overlapping, ~17% apart
# (2026-09-21). A windowed frame is client -> lvdesk -> client, so if the two
# land on different harts every frame pays a cross-CPU wake (~1.4 ms round
# trip measured); if they land together it pays a same-CPU switch. Which one
# happens is decided at boot - partly by luck, partly by whether lvdesk
# tripped the PIE bounce limit and got pinned to CPU0 for good.
#
# Three arms on one boot, A/B/A/B so drift is visible:
#   co   lvdesk and client both on CPU0   (co-resident, no cross-CPU wake)
#   split lvdesk on CPU0, client on CPU1  (a wake every frame)
#   free  both {0,1}                      (what a boot actually gives you)
#
# Needs rootfs/oncpu (there is no taskset on the board).
set -u
cd "$(dirname "$0")/../.."
REP=${1:-3}
OUT=artifacts/smp-finish/placement-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/arm.sh

echo "placement: $REP repeats per arm -> $OUT"
python3 scripts/board/reset.py >/dev/null 2>&1

# One board-side script per arm: set lvdesk's mask, run the canary under
# oncpu, print the mean the same way sdlbench does.
for arm in co split free co2 split2; do
	case $arm in
	co|co2)       LV=1; CL=1 ;;
	split|split2) LV=1; CL=2 ;;
	free)         LV=3; CL=3 ;;
	esac
	cat > "$S" <<EOF
L=\$(ps | awk '/[l]vdesk/ {print \$1}' | head -1)
for t in \$(ls /proc/\$L/task 2>/dev/null); do /root/oncpu -s $LV \$t 2>/dev/null; done
echo "ARM $arm lvdesk=\$L mask=\$(/root/oncpu -p \$L | awk '{print \$2}') client=$CL"
echo "PINNED \$(dmesg | grep -ac 'pinned to CPU0')"
cd /root
DISPLAY=:0 /root/oncpu $CL /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
echo ARM_DONE
EOF
	python3 scripts/board/runsh.py "$S" 90 15 > "$OUT/$arm.log" 2>&1
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
if means:
    print("    %-6s mean %.3f ms  (n=%d, %s)" % (
        sys.argv[2], statistics.median(means), len(means),
        " ".join("%.2f" % v for v in means)))
else:
    print("    %-6s NO RESULT - see the log" % sys.argv[2])
PY
done
echo "logs: $OUT"
