#!/bin/sh
# run-stencil-pick.sh [LIBDIR] - phase 4 F8: gl/tests/stencil_pick.c under
# SDL2, SDL 1.2 (sdl12-compat) and freeglut against our libGL (default
# gl/out-host) on Xvfb depth 16, with and without a stencil request. From the
# Mac it re-execs in s31-glref. Exit 1 unless every toolkit gets 0 stencil
# bits when it asks for none and 8 when it asks for stencil. s31, MIT.
if [ ! -x /usr/bin/Xvfb ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/gl/tests/run-stencil-pick.sh "$@"
fi
LIB=${1:-/src/gl/out-host}
P=/src/gl/ref-apps/prefix
T=/tmp/pick; mkdir -p $T
gcc -O2 -DPICK_SDL2 $(sdl2-config --cflags) /src/gl/tests/stencil_pick.c -o $T/sdl2 -L$LIB -lGL $(sdl2-config --libs) || exit 2
gcc -O2 -DPICK_SDL12 $(sdl-config --cflags) /src/gl/tests/stencil_pick.c -o $T/sdl12 -L$LIB -lGL $(sdl-config --libs) || exit 2
gcc -O2 -I$P/include /src/gl/tests/stencil_pick.c -o $T/glut -L$LIB -lGL -L$P/lib -lglut || exit 2
Xvfb :97 -screen 0 800x480x16 -nolisten tcp >/dev/null 2>&1 &
XP=$!
sleep 1
bad=0
for t in sdl2 sdl12 glut; do
	for a in 0 8; do
		out=$(DISPLAY=:97 SDL12COMPAT_OPENGL_SCALING=0 LD_LIBRARY_PATH=$LIB:$P/lib timeout 20 $T/$t $a 2>/dev/null | grep PICK)
		echo "${out:-PICK $t asked=$a NO OUTPUT}"
		echo "$out" | grep -q "GL_STENCIL_BITS=$a " || bad=1
	done
done
kill $XP
[ $bad = 0 ] && echo "stencil-pick: PASS" || echo "stencil-pick: FAIL"
exit $bad
