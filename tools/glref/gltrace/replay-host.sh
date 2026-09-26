#!/bin/sh
# replay-host.sh - replay a gltrace on the host rig (Xvfb 800x480x16, the
# s31-glref container) against Mesa or our libGL, and compare every full
# frame with the live capture's hash in the trace.
#
#   tools/glref/gltrace/replay-host.sh <ours|mesa> TRACE OUTDIR [OURS_DIR]
#
# TRACE and OUTDIR are container paths (/src/...). OURS_DIR holds the
# libGL.so.1 for "ours" (default /src/gl/out-host). Writes OUTDIR/hashes.txt,
# OUTDIR/f<N>.raw for every full frame, OUTDIR/replay.log, and with
# QS_PNG=1 PNGs of the counted frames' first and last. Against the library
# the trace was recorded with, every hash must match (exit 0). s31, MIT.
set -eu
if [ ! -x /usr/bin/Xvfb ]; then
	R=$(cd "$(dirname "$0")/../../.." && pwd)
	exec docker run --rm -v "$R":/src -w /src -e QS_PNG -e QS_GLENV s31-glref:latest sh /src/tools/glref/gltrace/replay-host.sh "$@"
fi
IMPL=$1; TRACE=$2; OUT=$3; OURS=${4:-/src/gl/out-host}
T=/src/tools/glref/gltrace
# QS_GLENV: the library's runtime toggles, as for capture-qs.sh
for kv in ${QS_GLENV:-}; do export "$kv"; done
mkdir -p "$OUT/bin"
rm -f "$OUT"/f*.raw "$OUT/hashes.txt"
[ -f "$TRACE.names" ] || python3 $T/trace.py names "$TRACE" > "$TRACE.names"
python3 $T/gen.py --inc /src/gl/include/GL dispatch "$TRACE.names" "$OUT/bin/dispatch.c" --mode table 2>/dev/null
gcc -O2 -Wall -Wno-unused-variable -I$T -I/src/gl/include $T/replay_host.c $T/replay.c "$OUT/bin/dispatch.c" \
	-o "$OUT/bin/qsreplay_host" -lGL -lX11 -ldl
GLREF_RUN_DIR=$OUT/run GLREF_OURS=$OURS GLREF_TIMEOUT=${GLREF_TIMEOUT:-600} GLREF_STALL_MS=0 GLREF_CWD=$OUT \
	sh /src/tools/glref/run.sh "$IMPL" replay 0 "$OUT/bin/qsreplay_host" "$TRACE" "$OUT" --frames >/dev/null 2>&1 || true
cp "$OUT/run/$IMPL/replay.f0.log" "$OUT/replay.log"
grep "qsreplay_host" "$OUT/replay.log" | tail -3
if [ -n "${QS_PNG:-}" ]; then
	for f in $(awk '$5=="count"{print $1}' "$OUT/hashes.txt"); do
		python3 $T/trace.py raw2png "$OUT/f$f.raw" "$OUT/f$f.png" 320 240
	done
fi
grep -q " 0 differ" "$OUT/replay.log"
