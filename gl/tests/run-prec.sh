#!/bin/sh
# run-prec.sh [OUTDIR] [OURS_DIR] - gl/tests/glx_prec.c under Mesa and ours
# (the precision probe of phase 5), scored by gl/tests/precscore.py. From
# the Mac or in s31-glref. OURS_DIR (default /src/gl/out-host) holds the
# libGL.so.1 measured. Writes OUTDIR (default /src/artifacts/gl/phase5/prec):
# {mesa,ours}/prec.f2.*, score.txt. s31, MIT.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-prec.sh "$@"
fi
OUT=${1:-/src/artifacts/gl/phase5/prec}
export GLREF_OURS=${2:-/src/gl/out-host}
mkdir -p "$OUT"
gcc -O2 -Wall -I/src/gl/include /src/gl/tests/glx_prec.c -o /tmp/glx_prec \
	-L/src/gl/out-host -lGL -lX11 -lm || exit 1
export GLREF_RUN_DIR=$OUT
for impl in mesa ours; do
	echo "$impl"
done | xargs -P 2 -L 1 sh -c 'sh /src/tools/glref/run.sh $0 prec 2 /tmp/glx_prec >/dev/null 2>&1'
python3 /src/gl/tests/precscore.py "$OUT/mesa/prec.f2.png" "$OUT/ours/prec.f2.png" > "$OUT/score.txt"
cat "$OUT/score.txt"
