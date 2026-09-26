#!/bin/sh
# Host test of format-32 properties and the ICCCM hints (proptest.c, review
# X1): the same program under stock Xlib and under the host (LP64) xlite;
# the outputs must be identical. In the s31-glref rig (re-execs itself).
set -u
if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/xlite ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/xlite/test/run-props.sh "$@"
fi
T=/tmp/xlite-props
rm -rf $T; mkdir -p $T/src /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
cp /src/xlite/*.c /src/xlite/*.h /src/xlite/*.txt $T/src/
python3 /src/tools/mkxlitestubs.py $T/src/symbols.txt $T/src/stubs.c $(ls $T/src/xlite*.c) >/dev/null
( cd $T/src && ls *.c | xargs -P"$(nproc)" -I{} gcc -D_GNU_SOURCE -O2 -fPIC -w -I$T/src -c {} -o {}.o ) &&
gcc -shared -Wl,-Bsymbolic-functions -Wl,-soname,libX11.so.6 -o $T/libX11.so.6 $T/src/*.o -lpthread -ldl ||
	{ echo "xlite failed to build"; exit 2; }
gcc -O2 -Wall -o $T/proptest /src/xlite/test/proptest.c -lX11 || exit 2
Xvfb :9 -screen 0 800x480x16 -nolisten tcp >/dev/null 2>&1 &
XP=$!
sleep 1
export DISPLAY=:9
timeout 20 $T/proptest > $T/stock.txt 2>&1
LD_LIBRARY_PATH=$T timeout 20 $T/proptest > $T/xlite.txt 2>&1
kill $XP
cat $T/stock.txt
if diff $T/stock.txt $T/xlite.txt > $T/diff.txt; then
	echo "props: PASS (xlite output identical to stock Xlib)"
else
	echo "props: FAIL"; cat $T/diff.txt; exit 1
fi
