#!/bin/sh
# p4-one.sh RUNDIR APP FRAME [VAR=VALUE ...] - phase 4: one suite app, one
# frame, ours with the given S31GL_* knobs, scored against the cached Mesa
# reference (artifacts/gl/ref-mesa). From the Mac (re-execs in s31-glref).
# Prints compare.py's line. s31, MIT.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/p4-one.sh "$@"
fi
RUN=$1; APP=$2; FR=$3; shift 3
for kv in "$@"; do export "$kv"; done
CMD=$(grep "^$APP " /src/tools/glref/apps.txt | awk -F'|' '{print $5}' | sed 's/^ *//')
XD=/src/gl/ref-apps/build/mesa-demos/src/xdemos
GD=/src/gl/ref-apps/build/mesa-demos/src/demos
SB=/src/gl/ref-apps/build
CMD=$(echo "$CMD" | sed "s|\$XD|$XD|; s|\$GD|$GD|; s|\$SB|$SB|")
export GLREF_RUN_DIR=/src/artifacts/gl/phase4/$RUN
sh /src/tools/glref/run.sh ours $APP $FR $CMD >/dev/null 2>&1
python3 /src/tools/glref/compare.py /src/artifacts/gl/ref-mesa/mesa/$APP.f$FR.png \
	$GLREF_RUN_DIR/ours/$APP.f$FR.png --diff $GLREF_RUN_DIR/$APP.f$FR.diff.png
