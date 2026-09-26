#!/bin/sh
# capture-qs.sh - record stock QuakeSpasm 0.96.3's GL stream during
# `timedemo demo1` (320x240 windowed, the glref virtual clock) into a
# gltrace file, against OUR host libGL. One command, from the Mac:
#
#   tools/glref/gltrace/capture-qs.sh [NAME]
#
# Knobs (environment):
#   QS_LIBGL    the real libGL.so.1 to record against (default: this tree's
#               /src/gl/out-host/libGL.so.1, i.e. what gl/host-build.sh made;
#               rebuild that first after a library change - the extension
#               string it advertises decides QuakeSpasm's paths)
#   QS_WINDOWS  counted frames (default "100-139,400-439"); frame N is the
#               one the Nth swap presents, as glref's f<N> captures
#   QS_WARM     full uncounted frames before each window (default 2)
#   QS_OUT      output directory (default /src/gl/bench/qstrace/work/NAME)
#   QS_ARGS     extra QuakeSpasm switches
#   QS_CFG      console commands run before the timedemo, ';'-separated (e.g.
#               "gl_texturemode GL_NEAREST_MIPMAP_NEAREST"): the game directory
#               is then a copy (pak0 linked) whose autoexec.cfg has them first
#   QS_GLENV    "S31GL_TEXFILTER=0 ..." environment for the app (our library's
#               runtime toggles), in the traced run and the QS_REF run
#   QS_REF=1    also capture the last frame the glref way (capture.c's own
#               XGetImage, a separate run without the tracer) as a check
#               that the tracer does not change what is drawn
# Output in QS_OUT:
#   qs.gltr          the trace; qs.gltr.names the GL entry points it uses
#   frames/f<N>.raw  the live window after each full frame (RGB565)
#   live.txt         frame, flags, live hash per swap (from the trace)
#   capture.log      QuakeSpasm's console + the tracer's summary
#   lib/libGL.so.1   the recording library (forwards to QS_LIBGL)
# Exit 0 only if the run captured the last frame and nothing was UNHANDLED.
# The launch is the board's: -heapsize 12288 -zone 384 at 320x240; the game
# directory is gl/ref-apps/quake (id1/pak0.pak, the shareware pak, and
# id1/autoexec.cfg running timedemo demo1; gitignored). -nosound: sound
# makes no GL call. s31, MIT.
set -eu
NAME=${1:-qs}
if [ ! -x /usr/bin/Xvfb ]; then
	R=$(cd "$(dirname "$0")/../../.." && pwd)
	exec docker run --rm -v "$R":/src -w /src -e QS_LIBGL -e QS_WINDOWS -e QS_WARM \
		-e QS_OUT -e QS_ARGS -e QS_REF -e QS_GLENV -e QS_CFG s31-glref:latest sh /src/tools/glref/gltrace/capture-qs.sh "$@"
fi
T=/src/tools/glref/gltrace
REAL=${QS_LIBGL:-/src/gl/out-host/libGL.so.1}
WINDOWS=${QS_WINDOWS:-100-139,400-439}
WARM=${QS_WARM:-2}
OUT=${QS_OUT:-/src/gl/bench/qstrace/work/$NAME}
QS=/src/artifacts/gl/glquake/host/quakespasm
BASE=/src/gl/ref-apps/quake
[ -f "$REAL" ] || { echo "capture-qs: no $REAL (run gl/host-build.sh)"; exit 2; }
[ -x "$QS" ] || { echo "capture-qs: no $QS (artifacts/gl/glquake/host/build-host.sh)"; exit 2; }
[ -f "$BASE/id1/pak0.pak" ] || { echo "capture-qs: no $BASE/id1/pak0.pak"; exit 2; }
[ -f "$BASE/id1/autoexec.cfg" ] || printf 'con_notifytime 0\ntimedemo demo1\n' > "$BASE/id1/autoexec.cfg"
if [ -n "${QS_CFG:-}" ]; then
	# the app's own settings, as a player would set them: a game directory
	# whose autoexec.cfg runs them before the timedemo
	NB=/tmp/qsbase-$$
	mkdir -p $NB/id1
	ln -s $BASE/id1/pak0.pak $NB/id1/pak0.pak
	{ echo "$QS_CFG" | tr ';' '\n'; cat $BASE/id1/autoexec.cfg; } > $NB/id1/autoexec.cfg
	BASE=$NB
fi
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/frames"
# the last frame: the capture shim ends the app right after its swap
LAST=$(echo "$WINDOWS" | tr ',' '\n' | sed 's/.*-//' | sort -n | tail -1)

# --- the recording libGL.so.1, exporting exactly what $REAL exports
nm -D --defined-only "$REAL" > "$OUT/lib/exports.txt"
python3 $T/gen.py --inc /src/gl/include/GL wrap "$OUT/lib/exports.txt" "$OUT/lib/wrap.c" 2> "$OUT/lib/gen.log"
gcc -O1 -g -fPIC -shared -fvisibility=hidden -Wall -Wno-unused-variable -Wno-array-parameter -I$T -I/src/gl/include \
	$T/gltrace_rt.c "$OUT/lib/wrap.c" -o "$OUT/lib/libGL.so.1" -Wl,-soname,libGL.so.1 \
	-ldl -lX11 -lpthread
# identical export surface (names only)
nm -D --defined-only "$OUT/lib/libGL.so.1" | awk '{print $3}' | sort > "$OUT/lib/have.txt"
awk '{print $3}' "$OUT/lib/exports.txt" | sort > "$OUT/lib/want.txt"
if ! cmp -s "$OUT/lib/have.txt" "$OUT/lib/want.txt"; then
	echo "capture-qs: export surface differs from $REAL:"; diff "$OUT/lib/want.txt" "$OUT/lib/have.txt" | head
	exit 2
fi

for kv in ${QS_GLENV:-}; do export "$kv"; done
# --- the run (tools/glref/run.sh: Xvfb 800x480x16, capture.c's virtual clock)
export GLTRACE_REAL="$REAL" GLTRACE_OUT="$OUT/qs.gltr" GLTRACE_WINDOWS="$WINDOWS" \
	GLTRACE_WARM="$WARM" GLTRACE_FRAMES="$OUT/frames"
GLREF_RUN_DIR=$OUT/run GLREF_OURS=$OUT/lib GLREF_CWD=$BASE GLREF_TIMEOUT=${GLREF_TIMEOUT:-600} \
	GLREF_STALL_MS=20000 \
	sh /src/tools/glref/run.sh ours qs "$LAST" $QS -basedir $BASE -width 320 -height 240 -window \
	-nosound -nocdaudio -heapsize 12288 -zone 384 ${QS_ARGS:-} > "$OUT/run.txt" 2>&1 || true
cp "$OUT/run/ours/qs.f$LAST.log" "$OUT/capture.log" 2>/dev/null || true
cat "$OUT/run.txt"
grep "^gltrace:" "$OUT/capture.log" | grep -v "context .* current" | tail -8
python3 $T/trace.py summary "$OUT/qs.gltr" > "$OUT/live.txt"
tail -3 "$OUT/live.txt"
if [ -n "${QS_REF:-}" ]; then
	unset GLTRACE_REAL GLTRACE_OUT GLTRACE_WINDOWS GLTRACE_WARM GLTRACE_FRAMES
	RD=$(dirname "$REAL")
	GLREF_RUN_DIR=$OUT/ref GLREF_OURS=$RD GLREF_CWD=$BASE GLREF_TIMEOUT=600 \
		sh /src/tools/glref/run.sh ours qs "$LAST" $QS -basedir $BASE -width 320 -height 240 -window \
		-nosound -nocdaudio -heapsize 12288 -zone 384 ${QS_ARGS:-} || true
	python3 $T/trace.py pngcmp "$OUT/frames/f$LAST.raw" "$OUT/ref/ours/qs.f$LAST.png"
fi
grep -q "glref: ok at swap $LAST" "$OUT/capture.log" || { echo "capture-qs: the run did not reach frame $LAST"; exit 1; }
grep -q " 0 UNHANDLED" "$OUT/capture.log" || { echo "capture-qs: UNHANDLED calls (see capture.log)"; exit 1; }
echo "capture-qs: $OUT/qs.gltr ($(du -h "$OUT/qs.gltr" | cut -f1))"
