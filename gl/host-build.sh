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
	exec docker run --rm -e S31GL_IN_RIG=1 -e S31GL_NO_GLX -e S31GL_HOST_DEFS -v "$REPO":/src -w /src \
		s31-glref:latest sh /src/gl/host-build.sh "$@"
fi

GL=/src/gl
CC=gcc
NM=nm
# S31GL_HOST_DEFS: extra -D flags for a diagnostic host build (phase 3a)
ARCHFLAGS="${S31GL_HOST_DEFS:-}"
XINC=""
XLIBS="-lXext -lX11"
OBJ=/tmp/s31gl-host
OUT=$GL/out-host/libGL.so.1
mkdir -p $GL/out-host
rm -f $GL/out-host/headless_gears $GL/out-host/core_test $GL/out-host/libGL.so \
	$GL/out-host/glx_prims $GL/out-host/raster_gate
export GL CC NM ARCHFLAGS XINC XLIBS OBJ OUT
sh $GL/api/build-lib.sh
ln -s libGL.so.1 $GL/out-host/libGL.so

echo "--- tests (headless_gears, core_test, raster_gate, glx_prims, glx_reopen), in parallel"
# each a background job; any failure fails the build (wait on every pid)
pids=""
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/headless_gears.c \
	-o $GL/out-host/headless_gears -L$GL/out-host -l:libGL.so.1 -lm \
	-Wl,-rpath,'$ORIGIN' & pids="$pids $!"
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/core_test.c \
	-o $GL/out-host/core_test -L$GL/out-host -l:libGL.so.1 -lm -ldl \
	-Wl,-rpath,'$ORIGIN' & pids="$pids $!"
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/raster_gate.c \
	-o $GL/out-host/raster_gate -L$GL/out-host -l:libGL.so.1 -lm \
	-Wl,-rpath,'$ORIGIN' & pids="$pids $!"
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/zepoch_test.c \
	-o $GL/out-host/zepoch_test -L$GL/out-host -l:libGL.so.1 -lm \
	-Wl,-rpath,'$ORIGIN' & pids="$pids $!"
# phase 4: texture filters and perspective colour
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/filt_test.c \
	-o $GL/out-host/filt_test -L$GL/out-host -l:libGL.so.1 -lm \
	-Wl,-rpath,'$ORIGIN' & pids="$pids $!"
# phase 5 O1: the fused fillers' bit-identity gate
$CC -O2 -Wall -I$GL/include -I$GL/api $GL/tests/fused_test.c \
	-o $GL/out-host/fused_test -L$GL/out-host -l:libGL.so.1 -lm \
	-Wl,-rpath,'$ORIGIN' & pids="$pids $!"
# no rpath: LD_LIBRARY_PATH picks the implementation (tools/glref/run.sh)
if [ -z "$S31GL_NO_GLX" ]; then
	$CC -O2 -Wall -I$GL/include $GL/tests/glx_prims.c \
		-o $GL/out-host/glx_prims -L$GL/out-host -lGL -lX11 & pids="$pids $!"
	$CC -O2 -Wall -I$GL/include $GL/tests/glx_reopen.c \
		-o $GL/out-host/glx_reopen -L$GL/out-host -lGL -lX11 & pids="$pids $!"
	# phase 4: stencil, blend equations, smooth lines and points; and the
	# app-only rand() preload of run-p4apps.sh (gl/tests/apprand.c)
	$CC -O2 -Wall -shared -fPIC $GL/tests/apprand.c -o $GL/out-host/apprand.so -ldl &
	pids="$pids $!"
	$CC -O2 -Wall -I$GL/include $GL/tests/glx_p4.c \
		-o $GL/out-host/glx_p4 -L$GL/out-host -lGL -lX11 -lm & pids="$pids $!"
	# phase 4 review: the receding floor (mipmap level per block)
	$CC -O2 -Wall -I$GL/include $GL/tests/glx_floor.c \
		-o $GL/out-host/glx_floor -L$GL/out-host -lGL -lX11 -lm & pids="$pids $!"
fi
for p in $pids; do wait $p; done
ls -l $OUT $GL/out-host/headless_gears $GL/out-host/core_test $GL/out-host/raster_gate
