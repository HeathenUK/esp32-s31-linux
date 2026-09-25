#!/bin/sh
# Host rig: build glxtest and run it against a libGL dir under Xvfb depth 16,
# with MIT-SHM, with S31GL_NOSHM=1 (the XPutImage fallback) and with
# S31GL_SHMBUFS=2 (two ping-pong segments).
#   docker run --rm -v $REPO:/src -w /src s31-glref:latest sh gl/glx/test/run-host.sh [libdir]
LIB=${1:-/src/gl/out-host}
set -e
gcc -O2 -Wall -I/src/gl/include /src/gl/glx/test/glxtest.c -o /tmp/glxtest -L$LIB -lGL -lX11
Xvfb :98 -screen 0 800x480x16 -nolisten tcp >/tmp/xvfb98.log 2>&1 &
XP=$!
sleep 1
set +e
echo "=== MIT-SHM"
DISPLAY=:98 LD_LIBRARY_PATH=$LIB S31GL_TRACE=1 /tmp/glxtest; R1=$?
echo "=== S31GL_NOSHM=1"
DISPLAY=:98 LD_LIBRARY_PATH=$LIB S31GL_NOSHM=1 /tmp/glxtest; R2=$?
echo "=== S31GL_SHMBUFS=2"
DISPLAY=:98 LD_LIBRARY_PATH=$LIB S31GL_SHMBUFS=2 S31GL_TRACE=1 /tmp/glxtest; R3=$?
kill $XP
echo "exit shm=$R1 noshm=$R2 shmbufs2=$R3"
[ $R1 = 0 ] && [ $R2 = 0 ] && [ $R3 = 0 ]
