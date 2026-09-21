#!/bin/bash
# canary-reset.sh <boots> - is the per-boot slow regime a PLACEMENT ATTRACTOR
# the scheduler fell into, or something fixed at boot (memory layout, CMA,
# cache aliasing)?
#
# Decisive because the two answers predict opposite things about a RUNTIME
# reset. Per boot: measure, then squeeze every task through CPU0 and release
# it again (affall 1; affall 3) - which destroys whatever placement the
# scheduler settled into without changing one byte of memory layout - then
# measure again.
#
#   attractor      a slow boot becomes fast after the squeeze
#   fixed at boot  it stays slow
#
# The squeeze itself is not a proposed fix - permanent forced placement
# measured ~15% WORSE than free (canary-placement.sh). It is only a way to
# make the scheduler re-decide.
set -u
cd "$(dirname "$0")/../.."
N=${1:?boots}
OUT=artifacts/smp-finish/canaryreset-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<'EOF'
cd /root
echo PHASE_BEFORE
i=0; while [ $i -lt 2 ]; do
	DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
	i=$((i+1))
done
grep -E "^cpu[01] " /proc/stat | sed 's/^/BSTAT /'
/root/affall 1 >/dev/null 2>&1
sleep 1
/root/affall 3 >/dev/null 2>&1
sleep 1
echo PHASE_AFTER
i=0; while [ $i -lt 2 ]; do
	DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
	i=$((i+1))
done
grep -E "^cpu[01] " /proc/stat | sed 's/^/ASTAT /'
echo RUN_DONE
EOF

echo "canary-reset: $N fresh boots -> $OUT"
for ((b = 1; b <= N; b++)); do
	python3 scripts/board/reset.py >/dev/null 2>&1
	python3 scripts/board/runsh.py "$S" 200 170 > "$OUT/boot-$b.log" 2>&1
	python3 - "$OUT/boot-$b.log" "$b" <<'PY'
import json, re, statistics, sys
txt = open(sys.argv[1], errors='replace').read()
def means(seg):
    return [json.loads(m.group(0))["mean_ns"] / 1e6
            for m in re.finditer(r'\{.*?"type":\s*"measure".*?\}', seg)]
before = means(txt.split('PHASE_AFTER')[0])
after = means(txt.split('PHASE_AFTER')[-1]) if 'PHASE_AFTER' in txt else []
def fmt(v):
    return ("%.3f" % statistics.median(v)) if v else "NONE"
verdict = ""
if before and after:
    b, a = statistics.median(before), statistics.median(after)
    verdict = "  %+.1f%%%s" % (100 * (a / b - 1),
                               "  <-- SLOW BOOT WENT FAST" if b > 15 and a < 15 else
                               ("  <-- stayed slow" if b > 15 else ""))
print("  boot %s: before %s  after %s%s   [%s | %s]" % (
    sys.argv[2], fmt(before), fmt(after), verdict,
    " ".join("%.2f" % v for v in before), " ".join("%.2f" % v for v in after)))
PY
done
echo "logs: $OUT"
