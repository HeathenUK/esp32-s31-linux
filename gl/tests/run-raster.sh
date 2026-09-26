#!/bin/sh
# glx_raster (gl/tests/glx_raster.c): the plan F3-F6 rasteriser features,
# every page under Mesa and ours in parallel, compared by tools/glref.
#   gl/tests/run-raster.sh [OUTDIR] [pages]     (from the Mac or in s31-glref)
# Writes OUTDIR (default /src/artifacts/gl/f3f6/raster): {mesa,ours}/rN.f2.*,
# diff-N.png, sbs-N.png (Mesa | ours | diff at 2x), log-diff-6.txt (page 6
# prints its glGet answers; the two lists must agree) and summary.txt.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-raster.sh "$@"
fi
OUT=${1:-/src/artifacts/gl/f3f6/raster}
PAGES=${2:-"1 2 3 4 5 6"}
P=/src/gl/ref-apps/prefix
BIN=/src/gl/out-host/glx_raster
mkdir -p "$OUT"
# built here against the Khronos headers and the rig's GLU; -lGL resolves by
# LD_LIBRARY_PATH (run.sh), so the same binary runs on both
gcc -O2 -Wall -I/src/gl/include -I$P/include /src/gl/tests/glx_raster.c -o $BIN \
	-L/src/gl/out-host -L$P/lib -lGLU -lGL -lX11 || exit 1
export GLREF_RUN_DIR=$OUT
# at most 10 at once: run.sh has ten private displays (:90-:99)
for p in $PAGES; do
	for impl in mesa ours; do
		echo "$impl $p"
	done
done | xargs -P 10 -n 2 sh -c 'sh /src/tools/glref/run.sh $0 r$1 2 '$BIN' $1 >/dev/null 2>&1'
: > "$OUT/summary.txt"
for p in $PAGES; do
	python3 /src/tools/glref/compare.py "$OUT/mesa/r$p.f2.png" "$OUT/ours/r$p.f2.png" \
		--diff "$OUT/diff-$p.png" > "$OUT/cmp-$p.txt" 2>&1
	echo "page $p: $(tail -1 "$OUT/cmp-$p.txt")" >> "$OUT/summary.txt"
	(cd "$OUT" && convert mesa/r$p.f2.png ours/r$p.f2.png diff-$p.png -scale 200% +append sbs-$p.png) 2>/dev/null
done
grep -h "^query\|^error\|^glu" "$OUT/mesa/r6.f2.log" > "$OUT/log-mesa-6.txt" 2>/dev/null
grep -h "^query\|^error\|^glu" "$OUT/ours/r6.f2.log" > "$OUT/log-ours-6.txt" 2>/dev/null
diff "$OUT/log-mesa-6.txt" "$OUT/log-ours-6.txt" > "$OUT/log-diff-6.txt" && echo "page 6 glGet answers: identical" >> "$OUT/summary.txt" || echo "page 6 glGet answers: DIFFER (log-diff-6.txt)" >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
