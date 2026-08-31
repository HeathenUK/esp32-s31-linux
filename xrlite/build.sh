#!/bin/sh
# Build xrlite - our libXrender replacement.
#
# Keeps the soname libXrender.so.1, so putting it on the library path is the
# whole integration. The stock library stays on the card: swapping the path
# back is an instant A/B if the tessellation ever looks wrong.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc

rm -f /src/images/libXrender.so.1.3.0	# a failed build must leave nothing to ship
$CC -O2 -fPIC -shared -Wall -Wno-unused-parameter \
	-I"$SYSROOT/usr/include" \
	-Wl,-soname,libXrender.so.1 -o /src/images/libXrender.so.1.3.0 \
	/src/xrlite/xrlite.c
${CC%gcc}strip /src/images/libXrender.so.1.3.0
ls -l /src/images/libXrender.so.1.3.0 |
	awk '{printf "  %-38s %7d bytes\n", $9, $5}'
