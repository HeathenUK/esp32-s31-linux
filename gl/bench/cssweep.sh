#!/bin/bash
# cssweep.sh OUT LABEL NSEEDS [JOBS] - the D-cache model (cachesim.sh) of
# one qsr image over seeds 1..NSEEDS of random page frames (pages=1,seed=S,
# every page, colour buffers too) plus the contiguous run; prints PSRAM kB
# per frame: contiguous, mean, p90, max. QSR_ENV as for cachesim.sh.
# Phase 6 tier 7. s31, MIT.
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$1" && pwd); L=$2; N=${3:-16}; J=${4:-8}
LIST=$OUT/qsr/cslist-$L.txt
{ echo "$L-c|$QSR_ENV|"; for s in $(seq 1 "$N"); do echo "$L-s$s|$QSR_ENV|pages=1,seed=$s"; done; } > "$LIST"
"$HERE/ccsweep.sh" "$OUT" "$LIST" "$J" > "$OUT/qsr/cssweep-$L.txt"
python3 - "$OUT/qsr/cssweep-$L.txt" "$L" <<'PY'
import sys, re
c = None; v = []
for l in open(sys.argv[1]):
    m = re.search(r'cachesim (\S+): .*PSRAM ([\d.]+) kB', l)
    if not m: continue
    if m.group(1).endswith('-c'): c = float(m.group(2))
    else: v.append(float(m.group(2)))
v.sort(); n = len(v)
print("cssweep %s: PSRAM kB/frame contiguous %.1f, %d seeds mean %.1f p90 %.1f max %.1f"
      % (sys.argv[2], c, n, sum(v) / n, v[min(n - 1, int(0.9 * n))], v[-1]))
PY
