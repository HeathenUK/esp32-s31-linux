#!/bin/sh
# glref: run one stock GL app under Xvfb (800x480x16, like the panel) and
# capture its window after exactly <frame> glXSwapBuffers calls.
#
#   tools/glref/run.sh <mesa|ours> <name> <frame> <cmd...>
#
# Writes $GLREF_RUN_DIR/<impl>/<name>.f<frame>.{png,log,meta,status}
# (GLREF_RUN_DIR defaults to /src/artifacts/gl/adhoc). <frame> 0 means "run
# to exit, no capture" (glxinfo); the stall watchdog still captures an app
# that stops swapping.
#
# mesa: the container's Mesa (llvmpipe) through the system libGL.so.1
# ours: LD_LIBRARY_PATH=$GLREF_OURS (default /src/gl/out-host) so the same
#       binary binds our libGL.so.1
# Both run with LD_BIND_NOW=1, which makes glibc resolve every import at load
# the way musl does on the board: a missing GL symbol aborts the app at start
# ("symbol lookup error") instead of at first call.
#
# Other knobs: GLREF_TIMEOUT (s, default 60), GLREF_CAPTURE=screen,
# GLREF_STALL_MS, GLREF_FRAME_NS (see capture.c), GLREF_CWD (default: the
# mesa-demos SOURCE tree's src/demos, where ../data/ holds the demos' data).
# From the host it re-execs itself in the s31-glref container.
set -u

if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/tools/glref ]; then
    here=$(cd "$(dirname "$0")/../.." && pwd)
    exec docker run --rm -v "$here":/src -w /src \
        -e GLREF_RUN_DIR -e GLREF_TIMEOUT -e GLREF_CAPTURE -e GLREF_STALL_MS \
        -e GLREF_FRAME_NS -e GLREF_CWD -e GLREF_OURS -e GLREF_TRACE \
        s31-glref:latest sh /src/tools/glref/run.sh "$@"
fi

if [ $# -lt 4 ]; then
    echo "usage: $0 <mesa|ours> <name> <frame> <cmd...>" >&2
    exit 2
fi
IMPL=$1; NAME=$2; FRAME=$3; shift 3

R=/src/gl/ref-apps
P=$R/prefix
MD=$R/build/mesa-demos
OURS=${GLREF_OURS:-/src/gl/out-host}
RUN_DIR=${GLREF_RUN_DIR:-/src/artifacts/gl/adhoc}
OUT=$RUN_DIR/$IMPL
mkdir -p "$OUT"
STEM=$OUT/$NAME.f$FRAME
rm -f "$STEM.png" "$STEM.ppm" "$STEM.meta" "$STEM.status" "$STEM.log"

# --- the capture shim, rebuilt when capture.c changes -----------------------
SHIM_DIR=$R/build/glref
SHIM=$SHIM_DIR/capture.so
mkdir -p "$SHIM_DIR"
if [ ! -e "$SHIM" ] || [ /src/tools/glref/capture.c -nt "$SHIM" ]; then
    # build to a private name and rename: parallel runs never see half a .so
    tmp=$SHIM.$$
    if ! gcc -O2 -Wall -Wextra -fPIC -shared -o "$tmp" /src/tools/glref/capture.c -lX11 -ldl -lpthread; then
        echo "capture.c failed to build" >&2
        exit 2
    fi
    mv -f "$tmp" "$SHIM"
fi

case $IMPL in
mesa) LIBPATH=$P/lib ;;
ours)
    if [ ! -e "$OURS/libGL.so.1" ]; then
        echo "result=not-built" >"$STEM.status"
        echo "ours: $OURS/libGL.so.1 does not exist" | tee "$STEM.log" >&2
        exit 5
    fi
    LIBPATH=$OURS:$P/lib ;;
*) echo "impl must be mesa or ours" >&2; exit 2 ;;
esac

# --- a private Xvfb on the first free :90-:99 ---------------------------------
DPYN=""
for n in 90 91 92 93 94 95 96 97 98 99; do
    if mkdir "/tmp/glref-dpy-$n" 2>/dev/null; then DPYN=$n; break; fi
done
if [ -z "$DPYN" ]; then echo "no free display :90-:99" >&2; exit 2; fi
XPID=""
cleanup() {
    [ -n "$XPID" ] && kill "$XPID" 2>/dev/null && wait "$XPID" 2>/dev/null
    rm -rf "/tmp/glref-dpy-$DPYN" "/tmp/.X$DPYN-lock" "/tmp/.X11-unix/X$DPYN"
}
trap cleanup EXIT INT TERM
rm -f "/tmp/.X$DPYN-lock" "/tmp/.X11-unix/X$DPYN"
Xvfb ":$DPYN" -screen 0 800x480x16 -nolisten tcp -ac -br +extension GLX \
    >"$OUT/.xvfb-$DPYN.log" 2>&1 &
XPID=$!
export DISPLAY=":$DPYN"
i=0
until xdpyinfo >/dev/null 2>&1; do
    i=$((i + 1))
    if [ $i -gt 100 ] || ! kill -0 "$XPID" 2>/dev/null; then
        echo "Xvfb :$DPYN did not start" >&2
        cat "$OUT/.xvfb-$DPYN.log" >&2
        exit 2
    fi
    sleep 0.05
done
rm -f "$OUT/.xvfb-$DPYN.log"

# --- run --------------------------------------------------------------------
cd "${GLREF_CWD:-$R/mesa-demos-9.0.0/src/demos}" || exit 2
BIN=$(command -v "$1" 2>/dev/null || echo "$1")
{
    echo "# glref run: impl=$IMPL name=$NAME frame=$FRAME display=:$DPYN"
    echo "# cmd: $*"
    echo "# libGL.so.1 resolves to:"
    LD_LIBRARY_PATH=$LIBPATH ldd "$BIN" 2>&1 | grep -E 'libGL|libGLU|libglut|not found' | sed 's/^/#   /'
} >"$STEM.log"

# For ours: every gl*/glX* import of the app and of the GL-using libraries it
# loads (libglut, libGLU) that our libGL does not export. LD_BIND_NOW reports
# only the first; this is the whole list.
UNRESOLVED=""
if [ "$IMPL" = ours ]; then
    libs=$(LD_LIBRARY_PATH=$LIBPATH ldd "$BIN" 2>/dev/null | sed -n 's/.*\(libglut\|libGLU\)[^ ]* => \([^ ]*\).*/\2/p')
    nm -D --defined-only "$OURS/libGL.so.1" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//' | sort -u >"$STEM.have"
    for f in "$BIN" $libs; do nm -D --undefined-only "$f" 2>/dev/null; done |
        awk '{print $NF}' | sed 's/@.*//' | grep -E '^gl[A-Z]|^glX' | sort -u >"$STEM.need"
    UNRESOLVED=$(comm -23 "$STEM.need" "$STEM.have" | tr '\n' ' ')
    rm -f "$STEM.have" "$STEM.need"
fi

# the preload goes on the app only, not on timeout(1).
# LP_NUM_THREADS=0: llvmpipe's rasteriser threads made multi-context apps
# non-deterministic (manywin: 5 of 30 Mesa runs differed, 0 of 30 without them)
timeout -k 5 "${GLREF_TIMEOUT:-60}" env \
    LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=0 \
    LD_LIBRARY_PATH="$LIBPATH" LD_BIND_NOW=1 LD_PRELOAD="$SHIM" \
    GLREF_OUT="$STEM" GLREF_FRAME="$FRAME" \
    "$@" >>"$STEM.log" 2>&1 </dev/null
RC=$?

# --- classify -------------------------------------------------------------------
UNIMPL=$(grep -c 'libGL: unimplemented' "$STEM.log")
MISSING=$(grep -E 'symbol lookup error|undefined symbol|Error relocating|cannot open shared object' "$STEM.log" |
    sed -n 's/.*undefined symbol: \([A-Za-z0-9_]*\).*/\1/p; s/.*symbol not found.*: \([A-Za-z0-9_]*\).*/\1/p' |
    sort -u | tr '\n' ' ')
LOADFAIL=$(grep -cE 'symbol lookup error|undefined symbol|Error relocating|cannot open shared object' "$STEM.log")
if [ -f "$STEM.ppm" ]; then
    python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' \
        "$STEM.ppm" "$STEM.png" && rm -f "$STEM.ppm"
fi
if [ "$LOADFAIL" -gt 0 ]; then RES=missing-symbol
elif [ $RC -eq 124 ] || [ $RC -eq 137 ]; then RES=timeout
elif [ $RC -gt 128 ] && [ $RC -le 192 ]; then RES=crash # timeout(1) reports a signal as 128+N
elif [ $RC -eq 4 ]; then RES=capture-failed
elif [ $RC -eq 3 ] && [ -f "$STEM.png" ]; then RES=stalled
elif [ $RC -eq 0 ] && [ -f "$STEM.png" ]; then RES=ok
elif [ $RC -eq 0 ]; then RES=exit
else RES=error
fi
SIG=""
[ $RES = crash ] && SIG=$((RC - 128))
{
    echo "result=$RES"
    echo "rc=$RC"
    [ -n "$SIG" ] && echo "signal=$SIG"
    echo "unimplemented=$UNIMPL"
    echo "missing=$MISSING"
    echo "unresolved=$UNRESOLVED"
    [ -f "$STEM.meta" ] && sed 's/^/meta_/' "$STEM.meta"
} >"$STEM.status"
rm -f "$STEM.meta"
echo "$IMPL $NAME f$FRAME: $RES (rc=$RC${SIG:+ sig=$SIG}${MISSING:+ missing: $MISSING}) -> $STEM.png"
case $RES in ok|stalled|exit) exit 0 ;; *) exit 1 ;; esac
