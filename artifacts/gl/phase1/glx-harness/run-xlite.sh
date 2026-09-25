#!/bin/sh
# Host rig, xlite as libX11 (built for the host from /src/xlite unmodified),
# talking to Xvfb depth 16. Exercises GLX against xlite's event queue and
# XShm client code. $1 = libGL dir.
LIB=$1
XL=/src/artifacts/gl/phase1/glx-harness/xlite-host
gcc -O2 -I/src/gl/include /src/gl/glx/test/glxtest.c -o /tmp/glxtest -L$LIB -lGL -lX11
Xvfb :97 -screen 0 800x480x16 -nolisten tcp >/tmp/xvfb97.log 2>&1 &
XP=$!
sleep 1
DISPLAY=:97 LD_LIBRARY_PATH=$LIB:$XL GLXTEST_XLITE=1 S31GL_TRACE=1 timeout 60 /tmp/glxtest; echo "exit $?"
echo "=== xlite + S31GL_SHMBUFS=2"
DISPLAY=:97 LD_LIBRARY_PATH=$LIB:$XL GLXTEST_XLITE=1 S31GL_SHMBUFS=2 S31GL_TRACE=1 timeout 60 /tmp/glxtest; echo "exit $?"
echo "--- ldd"
LD_LIBRARY_PATH=$LIB:$XL ldd /tmp/glxtest | grep -E 'X11|GL|Xext'
kill $XP
