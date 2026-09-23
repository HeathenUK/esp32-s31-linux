#!/bin/bash
# nosyncwait-aba.sh [runs=3] - bound the "last X round trip per frame" family
# (perf-review-2026-09-23 T2.4) in one boot: the sdl1 canary with XSync's
# reply wait deleted outright (XLITE_NOSYNCWAIT=1) against the same library
# without the knob, A/B/A. Nothing cheaper than deleting the wait can beat it,
# so B-A is the ceiling for C94 / X3-11 / X2-11.
#
# The test library goes to /root/xtest and is selected with LD_LIBRARY_PATH for
# the CANARY only - a benchmark of ours, not an application. /usr/lib is a
# read-only XIP overlay; nothing there changes. Same-boot arms: the A-to-A gap
# is the error bar (dead-end #19). B tears on the panel; that is expected.
set -u
cd "$(dirname "$0")/../.."
RUNS=${1:-3}
OUT=artifacts/perf-plan/nosyncwait-$(date +%H%M%S); mkdir -p "$OUT"
python3 scripts/board/deploy.py images/libX11.so.6.4.0 /root/xtest/libX11.so.6 2>&1 | tail -1
S=$OUT/run.sh
cat > "$S" <<EOS
cd /root; md5sum /root/xtest/libX11.so.6 | cut -c1-8
for arm in A B A2; do
	i=0; while [ \$i -lt $RUNS ]; do
		if [ \$arm = B ]; then K=1; else K=; fi
		echo "ARM \$arm \$(XLITE_NOSYNCWAIT=\$K LD_LIBRARY_PATH=/root/xtest DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '\"type\":\"measure\"')"
		i=\$((i+1))
	done
done
echo NSW_DONE
EOS
python3 scripts/board/runsh.py "$S" 200 170 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" <<'PY'
import json, re, statistics, sys
txt = open(sys.argv[1], errors='replace').read()
arms = {}
for m in re.finditer(r'^ARM (\S+) (\{.*\})$', txt, re.M):
    try:
        arms.setdefault(m.group(1), []).append(json.loads(m.group(2))["mean_ns"] / 1e6)
    except Exception:
        pass
for a in ('A', 'B', 'A2'):
    v = arms.get(a, [])
    print("  %-3s median %s ms  [%s]" % (a, ("%.3f" % statistics.median(v)) if v else "NONE", " ".join("%.2f" % x for x in v)))
if all(arms.get(a) for a in ('A', 'B', 'A2')):
    A = statistics.median(arms['A'] + arms['A2']); B = statistics.median(arms['B'])
    gap = abs(statistics.median(arms['A']) - statistics.median(arms['A2']))
    print("  B vs A: %+.1f%%  (A-to-A gap %.1f%% = the error bar)" % (100 * (B - A) / A, 100 * gap / A))
if 'NSW_DONE' not in txt: print("  RUN DID NOT FINISH - see", sys.argv[1])
PY
echo "log: $OUT/run.log"
