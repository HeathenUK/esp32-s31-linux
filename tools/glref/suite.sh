#!/bin/sh
# glref suite: every target in tools/glref/apps.txt, at 2-3 frame numbers,
# under one implementation, compared with cached Mesa references.
#
#   tools/glref/suite.sh [--impl ours|mesa] [--run NAME] [--apps "a b ..."]
#                        [--frames "3 20 60"] [--jobs N] [--refresh-ref]
#
#   --impl ours   (default) our libGL from $GLREF_OURS (/src/gl/out-host)
#   --impl mesa   Mesa again: the harness self-test; every frame must be EXACT
#   --run NAME    output under artifacts/gl/NAME (default <impl>-<timestamp>)
#
# Writes artifacts/gl/<run>/report.md (+ report.json, <impl>/ captures and
# logs, diff/ images; for ours also xlite-load.md, appended to the report:
# tools/glref/xlite-load.sh). Mesa references are generated once into
# artifacts/gl/ref-mesa/ and reused while their stamp (capture.c, run.sh,
# apps.txt, the app binaries, the Mesa package version) is unchanged.
# From the host it re-execs itself in the s31-glref container.
set -u

if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/tools/glref ]; then
    here=$(cd "$(dirname "$0")/../.." && pwd)
    exec docker run --rm -v "$here":/src -w /src -e GLREF_OURS -e GLREF_TIMEOUT \
        s31-glref:latest sh /src/tools/glref/suite.sh "$@"
fi

IMPL=ours RUN="" APPS="" FRAMES="3 20 60" JOBS=4 REFRESH=0
while [ $# -gt 0 ]; do
    case $1 in
    --impl) IMPL=$2; shift ;;
    --run) RUN=$2; shift ;;
    --apps) APPS=$2; shift ;;
    --frames) FRAMES=$2; shift ;;
    --jobs) JOBS=$2; shift ;;
    --refresh-ref) REFRESH=1 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$RUN" ] || RUN=$IMPL-$(date +%Y%m%d-%H%M%S)

G=/src/tools/glref
A=/src/artifacts/gl
RUNDIR=$A/$RUN
REFDIR=$A/ref-mesa
MD=/src/gl/ref-apps/build/mesa-demos
XD=$MD/src/xdemos
GD=$MD/src/demos

sh $G/build-apps.sh >/dev/null || { echo "reference apps failed to build" >&2; exit 2; }

if [ "$IMPL" = ours ] && [ ! -e "${GLREF_OURS:-/src/gl/out-host}/libGL.so.1" ]; then
    echo "our libGL is not built yet: ${GLREF_OURS:-/src/gl/out-host}/libGL.so.1 missing" >&2
    exit 5
fi

# --- the reference stamp -------------------------------------------------------
stamp() {
    {
        dpkg-query -W -f '${Version}\n' libgl1-mesa-dri 2>/dev/null
        md5sum $G/capture.c $G/apps.txt $G/run.sh
        for f in $XD/* $GD/*; do [ -f "$f" ] && [ -x "$f" ] && md5sum "$f"; done
    } | md5sum | cut -d' ' -f1
}
STAMP=$(stamp)
mkdir -p "$REFDIR/mesa"
if [ "$REFRESH" = 1 ] || [ "$(cat "$REFDIR/STAMP" 2>/dev/null)" != "$STAMP" ]; then
    echo "mesa references: regenerating (stamp changed or --refresh-ref)"
    rm -f "$REFDIR"/mesa/*
fi

# --- job list ---------------------------------------------------------------------
JOBDIR=$(mktemp -d)
trap 'rm -rf "$JOBDIR"' EXIT
nj=0
addjob() { # impl rundir name frame capture cmd
    nj=$((nj + 1))
    f=$(printf '%s/%04d.sh' "$JOBDIR" $nj)
    printf 'GLREF_RUN_DIR=%s GLREF_CAPTURE=%s exec sh %s/run.sh %s %s %s %s\n' \
        "$2" "$5" "$G" "$1" "$3" "$4" "$6" >"$f"
}
SEL=""
while IFS='|' read -r name kind cap frames cmd; do
    name=$(echo "$name" | tr -d ' ')
    case $name in ''|'#'*) continue ;; esac
    kind=$(echo "$kind" | tr -d ' ')
    cap=$(echo "$cap" | tr -d ' ')
    frames=$(echo "$frames" | sed 's/^ *//; s/ *$//')
    cmd=$(echo "$cmd" | sed 's/^ *//; s/ *$//' | sed "s|\$XD|$XD|g; s|\$GD|$GD|g")
    if [ -n "$APPS" ]; then
        case " $APPS " in *" $name "*) ;; *) continue ;; esac
    fi
    SEL="$SEL $name"
    [ -n "$frames" ] || frames=$FRAMES
    [ "$kind" = exit ] && frames=0
    for fr in $frames; do
        [ -f "$REFDIR/mesa/$name.f$fr.status" ] || addjob mesa "$REFDIR" "$name" "$fr" "$cap" "$cmd"
        addjob "$IMPL" "$RUNDIR" "$name" "$fr" "$cap" "$cmd"
    done
done <$G/apps.txt

mkdir -p "$RUNDIR"
echo "suite: $RUN, impl=$IMPL, $nj runs, $JOBS at a time"
# shellcheck disable=SC2012
ls "$JOBDIR"/*.sh | xargs -P "$JOBS" -n 1 sh 2>&1 | sed 's/^/  /'
echo "$STAMP" >"$REFDIR/STAMP"

python3 $G/report.py --run-dir "$RUNDIR" --ref-dir "$REFDIR" --impl "$IMPL" \
    --apps-file $G/apps.txt --frames "$FRAMES" --apps "$SEL" --run-name "$RUN"
RC=$?

# The runs above use the host's stock libX11. The board's is xlite, and musl
# binds eagerly: add the load-time arm (every import against xlite + xstubs)
# so a PASS above is never read as "loads on the board".
if [ "$IMPL" = ours ]; then
    sh $G/xlite-load.sh "$RUNDIR/xlite-load.md" | sed 's/^/  /'
    { echo; sed 's/^# xlite load arm/## xlite load arm (board X stack, load time only)/' "$RUNDIR/xlite-load.md"; } >>"$RUNDIR/report.md"
fi
exit $RC
