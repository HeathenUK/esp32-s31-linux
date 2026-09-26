#!/bin/sh
# run-san.sh - libGL.so.1 built with ASan + UBSan (gl/out-san), then
# glx_raster pages 1-6, glx_pixels pages 1-4 and glu_check pages 1-3
# (plan F7), glx_prims, core_test, headless_gears and raster_gate against it
# under Xvfb. Prints every sanitizer report; "san: clean" when there is none.
# In s31-glref (re-execs itself there from the Mac).
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-san.sh "$@"
fi
set -e
GL=/src/gl
S=$GL/out-san
mkdir -p $S
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g"
GL=$GL CC=gcc NM=nm ARCHFLAGS="$SAN" XINC= XLIBS="-lXext -lX11" OBJ=/tmp/s31gl-san \
	OUT=$S/libGL.so.1 ZDEFS= LDFLAGS="$SAN" sh $GL/api/build-lib.sh > $S/build.log 2>&1 ||
	{ tail -20 $S/build.log; exit 1; }
P=$GL/ref-apps/prefix
gcc -O1 -g -I$GL/include -I$P/include $GL/tests/glx_raster.c -o $S/glx_raster -L$S -L$P/lib -lGLU -lGL -lX11
gcc -O1 -g -I$GL/include $GL/tests/glx_prims.c -o $S/glx_prims -L$S -lGL -lX11
gcc -O1 -g -I$GL/include $GL/tests/glx_pixels.c -o $S/glx_pixels -L$S -lGL -lX11 -lm
gcc -O1 -g -I$GL/include -I$P/include $GL/tests/glu_check.c -o $S/glu_check -L$S -L$P/lib -lGLU -lGL -lX11 -lm
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/core_test.c -o $S/core_test -L$S -l:libGL.so.1 -lm -ldl
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/headless_gears.c -o $S/headless_gears -L$S -l:libGL.so.1 -lm
gcc -O1 -g -I$GL/include -I$GL/api $GL/tests/raster_gate.c -o $S/raster_gate -L$S -l:libGL.so.1 -lm
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
$S/core_test >> $S/san.log 2>&1 || echo "core_test: rc $?" >> $S/san.log
$S/headless_gears 320 240 20 /tmp/hg.ppm >> $S/san.log 2>&1 || echo "headless_gears: rc $?" >> $S/san.log
$S/raster_gate >> $S/san.log 2>&1 || echo "raster_gate: rc $?" >> $S/san.log
kill $XP
if grep -q "runtime error\|ERROR: AddressSanitizer\|LeakSanitizer\|: rc " $S/san.log; then
	grep -A12 "runtime error\|ERROR: AddressSanitizer\|LeakSanitizer\|: rc " $S/san.log | head -80
else
	echo "san: clean ($(grep -c . $S/san.log) log lines; $(grep 'passed' $S/san.log | tr '\n' ' '))"
fi
