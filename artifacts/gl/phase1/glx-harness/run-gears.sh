#!/bin/sh
# Run stock glxgears against a libGL dir under Xvfb depth 16 and grab two
# frames 0.5 s apart with XGetImage (xgrab.c). $1 = libGL dir (a missing dir
# means the rig's own Mesa llvmpipe, the reference), $2 = output PNG.
# $3 (optional) = extra LD_LIBRARY_PATH entry (e.g. xlite-host), $4 = preload.
LIB=${1:-/src/gl/out-host}
OUT=${2:-/src/artifacts/gl/phase1/glxgears_ours.png}
EXTRA=${3:+:$3}
gcc -O2 -o /tmp/xgrab /src/artifacts/gl/phase1/glx-harness/xgrab.c -lX11
gcc -O2 -o /tmp/xsendesc /src/artifacts/gl/phase1/glx-harness/xsendesc.c -lX11
[ -n "$4" ] && gcc -shared -fPIC -o /tmp/pre.so "$4"
Xvfb :99 -screen 0 800x480x16 -nolisten tcp >/tmp/xvfb.log 2>&1 &
XP=$!
sleep 1
export DISPLAY=:99
xdpyinfo | grep -E 'depth of root|default visual id'
LD_PRELOAD=${4:+/tmp/pre.so} LD_LIBRARY_PATH=$LIB$EXTRA S31GL_TRACE=1 \
	timeout 14 glxgears -info >/tmp/gears.log 2>&1 &
GP=$!
sleep 7
/tmp/xgrab > /tmp/f1.ppm
sleep 0.5
/tmp/xgrab > /tmp/f2.ppm
python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" /tmp/f1.ppm $OUT
python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" /tmp/f2.ppm /tmp/f2.png
echo "animating: $(compare -metric AE $OUT /tmp/f2.png null: 2>&1) pixels differ between frames 0.5 s apart"
[ -n "$3" ] && export XSENDESC_KEYCODE=4
/tmp/xsendesc glxgears
wait $GP
echo "glxgears exit $? (0 = clean exit on Escape; 124 = had to be killed)"
cat /tmp/gears.log
kill $XP
