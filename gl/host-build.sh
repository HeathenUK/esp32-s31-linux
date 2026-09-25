#!/bin/sh
# host-build.sh - build libGL.so.1 for the host GL rig (docker image
# s31-glref: Debian, Xvfb, Mesa), linked against the container's real
# libX11/libXext, plus the headless test. Run on the Mac:
#     gl/host-build.sh
# Output: gl/out-host/libGL.so.1 (+ libGL.so for -lGL), gl/out-host/headless_gears,
#         gl/out-host/core_test
set -e
if [ -z "$S31GL_IN_RIG" ]; then
	REPO=$(cd "$(dirname "$0")/.." && pwd)
	exec docker run --rm -e S31GL_IN_RIG=1 -e S31GL_NO_GLX -v "$REPO":/src -w /src \
		s31-glref:latest sh /src/gl/host-build.sh "$@"
fi

GL=/src/gl
CC=gcc
NM=nm
ARCHFLAGS=""
XINC=""
XLIBS="-lXext -lX11"
OBJ=/tmp/s31gl-host
OUT=$GL/out-host/libGL.so.1
mkdir -p $GL/out-host
rm -f $GL/out-host/headless_gears $GL/out-host/core_test $GL/out-host/libGL.so \
	$GL/out-host/glx_prims
export GL CC NM ARCHFLAGS XINC XLIBS OBJ OUT
sh $GL/api/build-lib.sh
ln -s libGL.so.1 $GL/out-host/libGL.so

echo "--- headless_gears (links the shipped libGL.so.1)"
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/headless_gears.c \
	-o $GL/out-host/headless_gears -L$GL/out-host -l:libGL.so.1 -lm \
	-Wl,-rpath,'$ORIGIN'
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/core_test.c \
	-o $GL/out-host/core_test -L$GL/out-host -l:libGL.so.1 -lm -ldl \
	-Wl,-rpath,'$ORIGIN'
# no rpath: LD_LIBRARY_PATH picks the implementation (tools/glref/run.sh)
[ -n "$S31GL_NO_GLX" ] || $CC -O2 -Wall -I$GL/include $GL/tests/glx_prims.c \
	-o $GL/out-host/glx_prims -L$GL/out-host -lGL -lX11
[ -n "$S31GL_NO_GLX" ] || $CC -O2 -Wall -I$GL/include $GL/tests/glx_reopen.c \
	-o $GL/out-host/glx_reopen -L$GL/out-host -lGL -lX11
ls -l $OUT $GL/out-host/headless_gears $GL/out-host/core_test
