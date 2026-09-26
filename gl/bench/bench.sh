#!/bin/bash
# bench.sh [GLDIR] [OUT] - build (build_q.sh) and run every image in
# parallel; prints "demo WxH Minsn/frame dcalls/frame fbhash" sorted, and
# writes it to $OUT/results.txt. Frames land in $OUT/*.raw.
# Then (unless BENCH_GUARD_ONLY=1) feat.sh, pix.sh, prim.sh, geo.sh and (phase 4)
# filt.sh and p4.sh (stencil, blend equations, smooth lines/points) on the same
# objects, and every line is checked against limits.txt: the plan's +1%
# guard for gears and texobj, tripwires for the general path, the pixel
# paths and the per-primitive costs (review P5d). Exit 1 on any excess.
# BENCH_NO_LIMITS=1 skips the check (for measuring another tree, such as
# gl/bench/base).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-$HERE/out}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
"$HERE/build_q.sh" "${1:-$HERE/..}" "$OUT"
cd "$OUT"
ls q_*.elf | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -n 1 "$HERE/run_q.sh" 2>/dev/null > results.all
grep Minsn results.all > results.raw
grep Mframe results.all | sort > frames.txt || true
grep "dcalls/frame" results.all | sort > dcalls.txt || true
sort results.raw | tee results.txt
n=$(wc -l < results.txt)
[ "$n" -eq 6 ] || { echo "bench: $n of 6 images reported (timeout or crash)" >&2; exit 1; }
[ -n "$BENCH_GUARD_ONLY" ] && exit 0
"$HERE/feat.sh" "$OUT" >/dev/null
"$HERE/pix.sh" "$OUT" >/dev/null
"$HERE/prim.sh" "$OUT" >/dev/null
"$HERE/geo.sh" "$OUT" >/dev/null
"$HERE/filt.sh" "$OUT" >/dev/null
"$HERE/p4.sh" "$OUT" >/dev/null
cat feat.txt pix.txt prim.txt geo.txt filt.txt p4.txt
[ -n "$BENCH_NO_LIMITS" ] && exit 0
python3 - "$HERE/limits.txt" results.txt feat.txt pix.txt prim.txt geo.txt filt.txt p4.txt <<'PY'
import re, sys
lim = {}
for l in open(sys.argv[1]):
    if l.startswith('#') or not l.strip():
        continue
    f = [x.strip() for x in l.split('|')]
    lim[f[0]] = (float(f[1]), f[2])
got = {}
for p in sys.argv[2:]:
    for l in open(p):
        m = re.match(r'(.+?): ([\d.]+) Minsn', l)
        if m:
            got[m.group(1)] = float(m.group(2))
bad = 0
for k, (v, kind) in lim.items():
    if k not in got:
        print("bench: %s did not report" % k); bad += 1
    elif got[k] > v:
        print("bench: %s %.4f > %s limit %.4f" % (k, got[k], kind, v)); bad += 1
print("bench: %d lines within limits.txt, %d over" % (len(lim) - bad, bad))
sys.exit(1 if bad else 0)
PY
