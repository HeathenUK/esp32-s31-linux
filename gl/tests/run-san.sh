#!/bin/sh
# run-san.sh - libGL.so.1 built with ASan + UBSan (gl/out-san), then
# glx_raster pages 1-6, glx_pixels pages 1-4 and glu_check pages 1-3
# (plan F7), glx_prims, glx_geo (phase 3a), core_test, headless_gears,
# raster_gate and zepoch_test (phase 3a: both ways, and S31GL_DIRTYBOX=2 at
# 97x71), filt_test (phase 4) and glx_p4 (phase 4 step 2: stencil, blend
# equations, smooth lines and points) against it, and (phase 4 L1) four of
# them again with the hot range copied to RAM (S31GL_RAMTEXT=1)
# under Xvfb. Prints every sanitizer report; "san: clean" when there is none.
# In s31-glref (re-execs itself there from the Mac).
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-san.sh "$@"
fi
set -e
GL=${RT_GL:-/src/gl}
S=$GL/out-san
mkdir -p $S
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g"
# phase 4 L1: the instrumented hot range has ~2,500 PC-relative sites
# (the sanitizer calls) and is ~3x larger: room for its RAM copy
RTDEFS="-DS31GL_RAMTEXT_FIXMAX=8192 -DS31GL_RAMTEXT_SLOT=131072"
GL=$GL CC=gcc NM=nm ARCHFLAGS="$SAN $RTDEFS" XINC= XLIBS="-lXext -lX11" OBJ=/tmp/s31gl-san \
	OUT=$S/libGL.so.1 ZDEFS= LDFLAGS="$SAN" sh $GL/api/build-lib.sh > $S/build.log 2>&1 ||
	{ tail -20 $S/build.log; exit 1; }
P=$GL/ref-apps/prefix
gcc -O1 -g -I$GL/include -I$P/include $GL/tests/glx_raster.c -o $S/glx_raster -L$S -L$P/lib -lGLU -lGL -lX11
gcc -O1 -g -I$GL/include $GL/tests/glx_prims.c -o $S/glx_prims -L$S -lGL -lX11
gcc -O1 -g -I$GL/include $GL/tests/glx_geo.c -o $S/glx_geo -L$S -lGL -lX11 -lm
gcc -O1 -g -I$GL/include $GL/tests/glx_pixels.c -o $S/glx_pixels -L$S -lGL -lX11 -lm
gcc -O1 -g -I$GL/include -I$P/include $GL/tests/glu_check.c -o $S/glu_check -L$S -L$P/lib -lGLU -lGL -lX11 -lm
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/core_test.c -o $S/core_test -L$S -l:libGL.so.1 -lm -ldl
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/headless_gears.c -o $S/headless_gears -L$S -l:libGL.so.1 -lm
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/raster_gate.c -o $S/raster_gate -L$S -l:libGL.so.1 -lm
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/zepoch_test.c -o $S/zepoch_test -L$S -l:libGL.so.1 -lm
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/filt_test.c -o $S/filt_test -L$S -l:libGL.so.1 -lm
gcc -O1 -g -I$GL/include $GL/tests/glx_p4.c -o $S/glx_p4 -L$S -lGL -lX11 -lm
Xvfb :77 -screen 0 800x480x16 -nolisten tcp >/dev/null 2>&1 &
XP=$!
sleep 1
export DISPLAY=:77 LD_LIBRARY_PATH=$S:$P/lib ASAN_OPTIONS=detect_leaks=1:halt_on_error=0
export LD_PRELOAD=$(gcc -print-file-name=libasan.so)
: > $S/san.log
for p in 1 2 3 4 5 6; do $S/glx_raster $p 3 >> $S/san.log 2>&1 || echo "glx_raster $p: rc $?" >> $S/san.log; done
for p in 1 2 3 4; do $S/glx_pixels $p 3 >> $S/san.log 2>&1 || echo "glx_pixels $p: rc $?" >> $S/san.log; done
for p in 1 2 3; do $S/glu_check $p 3 >> $S/san.log 2>&1 || echo "glu_check $p: rc $?" >> $S/san.log; done
$S/glx_prims 3 >> $S/san.log 2>&1 || echo "glx_prims: rc $?" >> $S/san.log
$S/glx_geo 3 >> $S/san.log 2>&1 || echo "glx_geo: rc $?" >> $S/san.log
$S/core_test >> $S/san.log 2>&1 || echo "core_test: rc $?" >> $S/san.log
$S/headless_gears 320 240 20 /tmp/hg.ppm >> $S/san.log 2>&1 || echo "headless_gears: rc $?" >> $S/san.log
$S/raster_gate >> $S/san.log 2>&1 || echo "raster_gate: rc $?" >> $S/san.log
# phase 3a: the depth epochs and dirty boxes, on and off
$S/zepoch_test 96 72 > /dev/null 2>> $S/san.log || echo "zepoch_test: rc $?" >> $S/san.log
S31GL_ZTRICK=0 S31GL_DIRTYBOX=0 $S/zepoch_test 96 72 > /dev/null 2>> $S/san.log || echo "zepoch_test off: rc $?" >> $S/san.log
# review 3a: the x-extent boxes, and an odd size (tail alignment, row packing)
S31GL_DIRTYBOX=2 $S/zepoch_test 97 71 > /dev/null 2>> $S/san.log || echo "zepoch_test x 97x71: rc $?" >> $S/san.log
# phase 4: texture filters and perspective colour (filt_test: bilinear,
# mipmaps, perspective colour, and a 400-batch fuzz of filtered primitives)
$S/filt_test >> $S/san.log 2>&1 || echo "filt_test: rc $?" >> $S/san.log
S31GL_TRILINEAR=0 $S/filt_test > /dev/null 2>> $S/san.log || true
# phase 4 (step 2): stencil, blend equations, smooth lines and points
$S/glx_p4 3 >> $S/san.log 2>&1 || echo "glx_p4: rc $?" >> $S/san.log
S31GL_STENCILRANGE=0 $S/glx_p4 2 >> $S/san.log 2>&1 || echo "glx_p4 norange: rc $?" >> $S/san.log
# phase 4 L1: the RAM copy of the hot range (S31GL_RAMTEXT=1)
S31GL_RAMTEXT=1 $S/core_test >> $S/san.log 2>&1 || echo "core_test ramtext: rc $?" >> $S/san.log
S31GL_RAMTEXT=1 $S/headless_gears 320 240 20 /tmp/hg.ppm >> $S/san.log 2>&1 || echo "headless_gears ramtext: rc $?" >> $S/san.log
S31GL_RAMTEXT=1 $S/zepoch_test 97 71 > /dev/null 2>> $S/san.log || echo "zepoch_test ramtext: rc $?" >> $S/san.log
S31GL_RAMTEXT=1 $S/glx_p4 3 >> $S/san.log 2>&1 || echo "glx_p4 ramtext: rc $?" >> $S/san.log
grep -q "libGL: ramtext on" $S/san.log || echo "ramtext: rc (the RAM copy did not engage)" >> $S/san.log
kill $XP
if grep -q "runtime error\|ERROR: AddressSanitizer\|LeakSanitizer\|: rc " $S/san.log; then
	grep -A12 "runtime error\|ERROR: AddressSanitizer\|LeakSanitizer\|: rc " $S/san.log | head -80
else
	echo "san: clean ($(grep -c . $S/san.log) log lines; $(grep 'passed' $S/san.log | tr '\n' ' '))"
fi
