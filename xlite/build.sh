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
rm -f "$OUT"	# a failed build must leave nothing to ship

echo "--- generating stubs ---"
IMPLS=$(ls /src/xlite/xlite*.c)
python3 /src/tools/mkxlitestubs.py /src/xlite/symbols.txt /src/xlite/stubs.c $IMPLS

echo "--- compiling ---"
INSTR=""
[ -n "$XLITE_INSTRUMENT" ] && INSTR="-DXLITE_INSTRUMENT -finstrument-functions"
$CC -O2 -fno-omit-frame-pointer -fPIC -shared -Wall -Wno-unused-parameter $INSTR \
	-I"$SYSROOT/usr/include" -I/src/xlite \
	-Wl,-soname,libX11.so.6 \
	-o "$OUT" /src/xlite/*.c
${CC%gcc}strip "$OUT"
ls -l "$OUT"
