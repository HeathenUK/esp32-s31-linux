#!/bin/sh
# One QuakeSpasm arm, one frame, via tools/glref/run.sh (Xvfb 800x480x16,
# capture shim, virtual clock). Game data in ./id1 (pak0.pak, gitignored);
# ./id1/autoexec.cfg runs "timedemo demo1" (shareware ignores +commands).
#   run-arm.sh <mesa|ours> <name> <frame> [extra quakespasm switches...]
# Output: artifacts/gl/glquake/dark/<impl>/<name>.f<frame>.{png,log,status}
# "ours" binds ./ours/gl/out-host/libGL.so.1 (a frozen copy of
# gl/out-host/libGL.so.1; the path keeps capture.c's "/gl/out-host/"
# real-time rule). BD=<basedir> overrides the game dir (host/plain: no
# autoexec.cfg, the board's stock start - demo loop in real time).
IMPL=$1; NAME=$2; FRAME=$3; shift 3
REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
H=/src/artifacts/gl/glquake/host
BD=${BD:-$H}
GLREF_RUN_DIR=${GLREF_RUN_DIR:-/src/artifacts/gl/glquake/dark} \
GLREF_OURS=${GLREF_OURS:-$H/ours/gl/out-host} GLREF_CWD=$H GLREF_TIMEOUT=${GLREF_TIMEOUT:-120} \
    exec "$REPO/tools/glref/run.sh" "$IMPL" "$NAME" "$FRAME" \
    $H/quakespasm -basedir $BD -width 320 -height 240 -window -nosound -nocdaudio "$@"
