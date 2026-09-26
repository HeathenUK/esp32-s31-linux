#!/bin/sh
# mkbase.sh [REV] [DIR] - a library snapshot for the QuakeSpasm proxy: the gl/
# sources of git revision REV (default 246e832, the phase-4 final) in DIR
# (default gl/bench/base5, gitignored) and its host libGL.so.1 in
# DIR/out-host/ (built by DIR's own api/build-lib.sh in the s31-glref
# container, as gl/host-build.sh does, without the tests). Then:
#   QS_LIBGL=/src/gl/bench/base5/out-host/libGL.so.1 tools/glref/gltrace/capture-qs.sh p4final
#   gl/bench/qsreplay.sh gl/bench/base5 gl/bench/out-qsr-p4final gl/bench/qstrace/work/p4final/qs.gltr
# s31, MIT.
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd)
REV=${1:-246e832}
DIR=${2:-$R/gl/bench/base5}
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
case $DIR in "$R"/*) ;; *) echo "mkbase: $DIR must be inside the repo (docker mount)"; exit 2 ;; esac
( cd "$R" && git archive "$REV" gl/api gl/tinygl gl/include gl/build.sh gl/host-build.sh gl/glx gl/tests ) |
	tar -x -C "$DIR" --strip-components=1
REL=${DIR#"$R"/}
docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c "
	export GL=/src/$REL CC=gcc NM=nm ARCHFLAGS= XINC= XLIBS='-lXext -lX11' OBJ=/tmp/o OUT=/src/$REL/out-host/libGL.so.1
	mkdir -p /src/$REL/out-host && sh \$GL/api/build-lib.sh | tail -2"
ls -l "$DIR/out-host/libGL.so.1"
