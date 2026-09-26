#!/bin/sh
# run-mtex.sh [OUTDIR] [pages] - phase 5 O1: gl/tests/glx_mtex.c (two
# texture units, the combiner, GL_ADD) under Mesa and ours, every page in
# parallel, compared by tools/glref/compare.py (images) and
# gl/tests/logcmp.py (the printed queries and errors). From the Mac or in
# s31-glref. Writes OUTDIR (default /src/artifacts/gl/phase5/mtex):
# {mesa,ours}/*.f2.*, diff-*.png, sbs-*.png, logdiff-*.txt, summary.txt.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-mtex.sh "$@"
fi
OUT=${1:-/src/artifacts/gl/phase5/mtex}
PAGES=${2:-"p1 p2 p3 p4 p5"}
B=/src/gl/out-host
mkdir -p "$OUT"
gcc -O2 -Wall -I/src/gl/include /src/gl/tests/glx_mtex.c -o $B/glx_mtex \
	-L$B -lGL -lX11 -lm || exit 1
export GLREF_RUN_DIR=$OUT
for p in $PAGES; do
	for impl in mesa ours; do
		echo "$impl $p $B/glx_mtex ${p#p}"
	done
done | xargs -P 8 -L 1 sh -c 'sh /src/tools/glref/run.sh $0 $1 2 $2 $3 >/dev/null 2>&1'
: > "$OUT/summary.txt"
for p in $PAGES; do
	python3 /src/tools/glref/compare.py "$OUT/mesa/$p.f2.png" "$OUT/ours/$p.f2.png" \
		--diff "$OUT/diff-$p.png" > "$OUT/cmp-$p.txt" 2>&1
	python3 /src/gl/tests/logcmp.py "$OUT/mesa/$p.f2.log" "$OUT/ours/$p.f2.log" \
		> "$OUT/logdiff-$p.txt" 2>&1
	echo "$p: $(tail -1 "$OUT/cmp-$p.txt") | log: $(tail -1 "$OUT/logdiff-$p.txt")" >> "$OUT/summary.txt"
	(cd "$OUT" && convert mesa/$p.f2.png ours/$p.f2.png diff-$p.png -scale 200% +append sbs-$p.png) 2>/dev/null
done
grep -h "libGL: unimplemented\|libGL: approximated" "$OUT"/ours/*.log | sort | uniq -c > "$OUT/unimplemented.txt"
cat "$OUT/summary.txt"
