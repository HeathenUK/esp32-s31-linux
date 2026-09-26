#!/bin/sh
# run-p4apps.sh RUN [NAME...] - phase 4: the stock apps and tests that
# exercise the phase 4 features (stencil, blend equations, smooth lines and
# points) but are not in tools/glref/apps.txt, each under Mesa and ours at
# the frames below, scored by tools/glref/compare.py. Output in
# artifacts/gl/phase4/RUN/ (summary.txt: one line per app and frame).
# From the Mac: re-execs itself in s31-glref. s31, MIT.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-p4apps.sh "$@"
fi
RUN=${1:?run}; shift
D=/src/gl/ref-apps/build/mesa-demos/src
export GLREF_RUN_DIR=/src/artifacts/gl/phase4/$RUN
mkdir -p $GLREF_RUN_DIR
# name | frames | command (stock binaries, stock switches)
# floor-*: the receding floor of review 4 R1 (gl/tests/glx_floor.c), per
# filter; floor8: the same cut into 8x8 quads.
# (reflect is not here: it asks GLUT_ALPHA, which neither GL has at 16 bpp)
LIST="dinoshade|3 20 60|$D/demos/dinoshade -geometry 320x240+0+0
dissolve|3 20|env PRE=/src/gl/out-host/apprand.so sh /src/gl/tests/apprand.sh $D/demos/dissolve -geometry 320x240+0+0
tri-stencil|3|$D/trivial/tri-stencil -geometry 320x240+0+0
stencilwrap|3|$D/tests/stencilwrap -geometry 320x240+0+0
p4|2|/src/gl/out-host/glx_p4
floor-lml|2|/src/gl/out-host/glx_floor 3 0 1
floor-lmn|2|/src/gl/out-host/glx_floor 3 1 1
floor-nmn|2|/src/gl/out-host/glx_floor 3 2 1
floor-nml|2|/src/gl/out-host/glx_floor 3 5 1
floor-lin|2|/src/gl/out-host/glx_floor 3 3 1
floor-near|2|/src/gl/out-host/glx_floor 3 4 1
floor8-lml|2|/src/gl/out-host/glx_floor 3 0 8"
[ $# -gt 0 ] && LIST=$(echo "$LIST" | grep -E "^($(echo "$@" | tr ' ' '|'))\|")
: > $GLREF_RUN_DIR/summary.txt
echo "$LIST" | while IFS='|' read -r name frames cmd; do
	[ -n "$name" ] || continue
	for f in $frames; do
		( sh /src/tools/glref/run.sh mesa $name $f $cmd >/dev/null 2>&1
		  sh /src/tools/glref/run.sh ours $name $f $cmd >/dev/null 2>&1
		  r=$(python3 /src/tools/glref/compare.py $GLREF_RUN_DIR/mesa/$name.f$f.png \
			$GLREF_RUN_DIR/ours/$name.f$f.png --diff $GLREF_RUN_DIR/$name.f$f.diff.png 2>&1 | tail -1)
		  un=$(grep -c "libGL: unimplemented" $GLREF_RUN_DIR/ours/$name.f$f.log 2>/dev/null)
		  echo "$name f$f: $r unimpl=$un" >> $GLREF_RUN_DIR/summary.txt ) &
	done
	wait
done
sort $GLREF_RUN_DIR/summary.txt
