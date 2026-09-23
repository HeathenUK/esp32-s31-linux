#!/bin/bash
# prelock-aba.sh <label> "<sct addr size>" ["<sct addr size>"] - I-cache prelock
# A/B/A on a BOOTED board: 6 same-hart pingpongs, lock the given section(s),
# 6 more, release, 6 more. Reports the three medians; the A-to-A gap is the
# error bar and a B outside both A's by more than that gap is the effect.
# Same-boot arms are position-biased here (docs/perf-review dead-end #19),
# which is exactly why the third arm exists. ~80 s of board time.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; S0=${2:?"sct addr size"}; S1=${3:-}
OUT=artifacts/perf-plan/prelock-$L-$(date +%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<EOF
P=/sys/module/esp32s31_cache/parameters/icache_prelock
[ -w \$P ] || { echo "NO_PARAM"; exit 1; }
arm() { i=0; while [ \$i -lt 6 ]; do /root/pingpong 1 1 8000 | sed "s/^/\$1 /"; i=\$((i+1)); done; }
arm A1
echo "$S0" > \$P; [ -n "$S1" ] && echo "$S1" > \$P; cat \$P | tr '\n' ';'; echo
arm B
echo "0 0 0" > \$P; echo "1 0 0" > \$P
arm A2
echo PL_DONE
EOF
python3 scripts/board/runsh.py "$S" 150 140 2>/dev/null | tr -d '\r' > "$OUT/run.log"
grep -a "NO_PARAM\|sct0\|NO_SHELL\|BUSY\|TIMEKILL" "$OUT/run.log" | head -3
python3 - "$OUT/run.log" <<'PY'
import re, statistics, sys
arms = {}
for l in open(sys.argv[1]):
    m = re.search(r'^(A1|B|A2) pingpong 1<->1\s+([0-9.]+)', l)
    if m:
        arms.setdefault(m.group(1), []).append(float(m.group(2)))
med = {k: statistics.median(v) for k, v in arms.items() if v}
if len(med) == 3:
    gap = abs(med['A1'] - med['A2'])
    print("  A1 %.0f  B %.0f  A2 %.0f  us   A-A gap %.0f (%.1f%%)   B vs mean(A) %+.1f%%" % (
        med['A1'], med['B'], med['A2'], gap, 100 * gap / med['A1'],
        100 * (med['B'] / ((med['A1'] + med['A2']) / 2) - 1)))
else:
    print("  INCOMPLETE:", {k: len(v) for k, v in arms.items()})
PY
echo "  log: $OUT/run.log"
