#!/bin/sh
# Cross-compile xlite - our libX11 replacement - for the board.
#
# The output is named libX11.so.6.4.0 with soname libX11.so.6 on purpose:
# nothing above it is rebuilt. libXt, libXaw and the clients reference these
# symbols by name, so putting this first on the library path is the whole
# integration.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
OUT=/src/images/libX11.so.6.4.0

echo "--- generating stubs ---"
python3 /src/tools/mkxlitestubs.py /src/xlite/symbols.txt /src/xlite/stubs.c \
	/src/xlite/xlite.c /src/xlite/xlite_req.c /src/xlite/xlite_xrm.c \
	2>/dev/null || \
python3 /src/tools/mkxlitestubs.py /src/xlite/symbols.txt /src/xlite/stubs.c \
	/src/xlite/xlite.c

echo "--- compiling ---"
$CC -O2 -fPIC -shared -Wall -Wno-unused-parameter \
	-I"$SYSROOT/usr/include" -I/src/xlite \
	-Wl,-soname,libX11.so.6 \
	-o "$OUT" /src/xlite/*.c
ls -l "$OUT"
