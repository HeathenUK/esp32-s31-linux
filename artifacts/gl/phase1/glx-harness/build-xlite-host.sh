#!/bin/sh
# Build xlite (/src/xlite, unmodified) for the host rig as libX11.so.6, so
# gl/glx can be exercised against xlite's event queue and XShm code under
# Xvfb. Test harness only - xlite ships from xlite/build.sh.
O=/src/artifacts/gl/phase1/glx-harness/xlite-host
mkdir -p /tmp/xl $O
cp /src/xlite/*.c /src/xlite/*.h /src/xlite/*.txt /tmp/xl/
python3 /src/tools/mkxlitestubs.py /tmp/xl/symbols.txt /tmp/xl/stubs.c $(ls /tmp/xl/xlite*.c) >/dev/null
gcc -D_GNU_SOURCE -O2 -fPIC -shared -w -I/tmp/xl -Wl,-Bsymbolic-functions \
	-Wl,-soname,libX11.so.6 -o $O/libX11.so.6 /tmp/xl/*.c -lpthread -ldl
ls -l $O/libX11.so.6
