#!/bin/bash
# icsweep.sh OUT LABEL NSEEDS [JOBS] - icsim.sh over seeds 1..NSEEDS with
# random code-page frames (pages=1,seed=S), JOBS at a time, then the
# contiguous run; prints refills per frame: contiguous, mean, median, p90,
# max over the seeds. QSR_ENV as for icsim.sh. Phase 6 tier 7. s31, MIT.
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1; L=$2; N=${3:-24}; J=${4:-8}
R=$OUT/qsr/icsweep-$L.txt; : > "$R"
run() { "$HERE/icsim.sh" "$OUT" "$L-s$1" pages=1,seed=$1 | sed "s/^/seed $1 /" >> "$R"; }
"$HERE/icsim.sh" "$OUT" "$L-c" | sed "s/^/seed 0 /" >> "$R" &
for s in $(seq 1 "$N"); do
	run "$s" &
	while [ "$(jobs -r | wc -l)" -ge "$J" ]; do sleep 1; done
done
wait
for s in $(seq 1 "$N"); do rm -rf "$OUT/qsr/ic-run-$L-s$s"; done
python3 - "$R" "$L" <<'PY'
import sys, re
c = None; v = []; ins = None
for l in open(sys.argv[1]):
    m = re.search(r'seed (\d+) .*: ([\d.]+) M insns, (\d+) distinct.*kB\), (\d+) refills', l)
    if not m: continue
    s, i, r = int(m.group(1)), float(m.group(2)), int(m.group(4))
    ins = i
    if s == 0: c = r
    else: v.append(r)
v.sort(); n = len(v)
p = lambda q: v[min(n - 1, int(q * n))]
print("icsweep %s: %.4f M insns, refills/frame contiguous %s, %d seeds mean %.0f median %d p90 %d max %d"
      % (sys.argv[2], ins, c, n, sum(v) / n, p(0.5), p(0.9), v[-1]))
PY
