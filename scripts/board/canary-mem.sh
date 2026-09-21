#!/bin/bash
# canary-mem.sh <boots> - is the boot lottery MEMORY ITSELF?
#
# BUDGET: ~90 s per boot, so keep <boots> at 4 or fewer - no run of this may
# exceed ~10 minutes (user, 2026-09-21: "Never ever run a test that takes more
# than 10 minutes per result"). Sampling is sized to the EFFECT: a 17% split
# against a 0.5% within-boot spread needs one canary run per boot, not three.
#
# By 2026-09-21 everything in software was eliminated as the cause of the
# ~1-in-4 boot that runs the windowed desktop ~17% slower for its whole life:
# not task placement (a runtime placement reset does not recover it), not
# client memory (three fresh client processes per boot agree to 0.5%), not
# lvdesk's allocations (a restart does not recover it), not the scanout
# buffer, CMA or the present path. What is left is below Linux - and the
# loader spends ~148 ms of every boot in "MSPI Timing: Enter psram timing
# tuning", choosing delay lines that hold until the next reset.
#
# So measure memory directly, per boot, beside the canary. PSRAM bandwidth is
# this board's shared ceiling, so if the calibration lands differently the
# whole machine moves with it.
#
#   slow boots also measure slower memory -> the tuning is the cause, and the
#       fix is to pin or re-run the calibration rather than accept it
#   memory identical, canary still split  -> not memory; look above PSRAM
set -u
cd "$(dirname "$0")/../.."
N=${1:?boots}
OUT=artifacts/smp-finish/canarymem-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<'EOF'
cd /root
chmod +x /root/membw 2>/dev/null
# CANARY FIRST, memory second. membw allocates 4 MB on a 15.4 MB machine, so
# running it first perturbs exactly the state the canary then measures: with
# membw leading, three boots gave 14.77-16.39 ms when fast boots have always
# been 13.6-14.0. Measuring the harness into the result (2026-09-21).
#
# ONE canary run, not three. The effect being classified is 17% and the
# within-boot spread is 0.5%, so a single run separates fast from slow with
# enormous margin - three runs bought nothing and tripled the wall clock.
DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
/root/membw 2
echo RUN_DONE
EOF

echo "canary-mem: $N fresh boots -> $OUT"
for ((b = 1; b <= N; b++)); do
	python3 scripts/board/reset.py >/dev/null 2>&1
	python3 scripts/board/runsh.py "$S" 200 170 > "$OUT/boot-$b.log" 2>&1
	python3 - "$OUT/boot-$b.log" "$b" <<'PY'
import json, re, statistics, sys
txt = open(sys.argv[1], errors='replace').read()
def last(metric):
    v = re.findall(r'membw %s ([0-9.]+)' % metric, txt)
    return float(v[-1]) if v else float('nan')
means = [json.loads(m.group(0))["mean_ns"] / 1e6
         for m in re.finditer(r'\{.*?"type":\s*"measure".*?\}', txt)]
print("  boot %s: chase %6.2f ns  copy %6.2f  fill %6.2f  read %6.2f  |  canary %s ms" % (
    sys.argv[2], last('chase_ns'), last('copy_MBs'), last('fill_MBs'), last('read_MBs'),
    ("%.3f" % statistics.median(means)) if means else "NONE"))
PY
done
echo "logs: $OUT"
