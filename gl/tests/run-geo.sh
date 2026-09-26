#!/bin/sh
# glx_geo (gl/tests/glx_geo.c, phase 3a): the geometry path - w != 1,
# rotations, lighting, colour material, strips in lists - under Mesa and
# ours, compared by tools/glref.
#   gl/tests/run-geo.sh [OUTDIR] [LIBDIR]   (from the Mac or in s31-glref)
# LIBDIR: our libGL.so.1 to test (default /src/gl/out-host; e.g. a build of
# the phase 3a base tree). Writes OUTDIR (default
# /src/artifacts/gl/phase3a/geo): {mesa,ours}/geo.f2.*, diff.png, sbs.png,
# summary.txt. s31, MIT.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-geo.sh "$@"
fi
OUT=${1:-/src/artifacts/gl/phase3a/geo}
LIB=${2:-/src/gl/out-host}
mkdir -p "$OUT"
BIN=/tmp/glx_geo
gcc -O2 -Wall -I/src/gl/include /src/gl/tests/glx_geo.c -o $BIN -L/src/gl/out-host -lGL -lX11 -lm || exit 1
export GLREF_RUN_DIR=$OUT
sh /src/tools/glref/run.sh mesa geo 2 $BIN >/dev/null 2>&1 &
GLREF_OURS=$LIB sh /src/tools/glref/run.sh ours geo 2 $BIN >/dev/null 2>&1 &
wait
python3 /src/tools/glref/compare.py "$OUT/mesa/geo.f2.png" "$OUT/ours/geo.f2.png" --diff "$OUT/diff.png" | tee "$OUT/summary.txt"
convert "$OUT/mesa/geo.f2.png" "$OUT/ours/geo.f2.png" "$OUT/diff.png" -scale 200% +append "$OUT/sbs.png" 2>/dev/null || true
