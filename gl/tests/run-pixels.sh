#!/bin/sh
# glx_pixels (gl/tests/glx_pixels.c) and glu_check (gl/tests/glu_check.c):
# plan F7's pixel paths, the remaining vertex state and the real Mesa GLU,
# every page under Mesa and ours in parallel, compared by tools/glref
# (images) and gl/tests/logcmp.py (the printed queries and readbacks).
#   gl/tests/run-pixels.sh [OUTDIR] [pages]     (from the Mac or in s31-glref)
# Pages p1..p4 are glx_pixels; g1 g2 are glu_check against Debian's
# libglu1-mesa (the static libGLU.a of the package: its shared library links
# glvnd's libOpenGL.so.0, which would bypass the implementation under test),
# and b1 b2 the same test against the board's GLU 9.0.3 built in the rig.
# Writes OUTDIR (default /src/artifacts/gl/f7/pixels): {mesa,ours}/*.f2.*,
# diff-*.png, sbs-*.png, logdiff-*.txt and summary.txt.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-pixels.sh "$@"
fi
OUT=${1:-/src/artifacts/gl/f7/pixels}
PAGES=${2:-"p1 p2 p3 p4 g1 g2 g3 b1 b2"}
P=/src/gl/ref-apps/prefix
B=/src/gl/out-host
LIBDIR=/usr/lib/$(gcc -dumpmachine)
mkdir -p "$OUT"
# -lGL resolves by LD_LIBRARY_PATH (run.sh), so one binary runs on both
gcc -O2 -Wall -I/src/gl/include /src/gl/tests/glx_pixels.c -o $B/glx_pixels \
	-L$B -lGL -lX11 -lm || exit 1
gcc -O2 -Wall -I/src/gl/include /src/gl/tests/glu_check.c -o $B/glu_check_deb \
	$LIBDIR/libGLU.a -L$B -lGL -lX11 -lstdc++ -lm || exit 1
gcc -O2 -Wall -I/src/gl/include -I$P/include /src/gl/tests/glu_check.c -o $B/glu_check_903 \
	-L$P/lib -Wl,-rpath,$P/lib -lGLU -L$B -lGL -lX11 -lm || exit 1
export GLREF_RUN_DIR=$OUT
bin() {
	case $1 in
	p*) echo "$B/glx_pixels ${1#p}" ;;
	g*) echo "$B/glu_check_deb ${1#g}" ;;
	b*) echo "$B/glu_check_903 ${1#b}" ;;
	esac
}
for p in $PAGES; do
	for impl in mesa ours; do
		echo "$impl $p $(bin $p)"
	done
done | xargs -P 10 -L 1 sh -c 'sh /src/tools/glref/run.sh $0 $1 2 $2 $3 >/dev/null 2>&1'
: > "$OUT/summary.txt"
for p in $PAGES; do
	python3 /src/tools/glref/compare.py "$OUT/mesa/$p.f2.png" "$OUT/ours/$p.f2.png" \
		--diff "$OUT/diff-$p.png" > "$OUT/cmp-$p.txt" 2>&1
	python3 /src/gl/tests/logcmp.py "$OUT/mesa/$p.f2.log" "$OUT/ours/$p.f2.log" \
		> "$OUT/logdiff-$p.txt" 2>&1
	echo "$p: $(tail -1 "$OUT/cmp-$p.txt") | log: $(tail -1 "$OUT/logdiff-$p.txt")" >> "$OUT/summary.txt"
	(cd "$OUT" && convert mesa/$p.f2.png ours/$p.f2.png diff-$p.png -scale 200% +append sbs-$p.png) 2>/dev/null
done
grep -h "libGL: unimplemented" "$OUT"/ours/*.log | sort | uniq -c > "$OUT/unimplemented.txt"
cat "$OUT/summary.txt"
