#!/bin/sh
# Build the toolkit replacements. Each keeps the soname it replaces, so the
# loader picks them up by library path alone and nothing above is rebuilt.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -fPIC -shared -Wall -Wno-unused-parameter \
	-I"$SYSROOT/usr/include" -I/src/xtlite \
	-Wl,-soname,libXt.so.6 -o /src/images/libXt.so.6.0.0 \
	/src/xtlite/xtlite.c /src/xtlite/xtclass.c

# libXaw7 and libXmu become EMPTY libraries. Their symbols are defined by our
# libXt above, and the dynamic loader resolves a symbol from whichever object
# defines it - these only have to exist so the DT_NEEDED entries in xcalc and
# each other are satisfied.
echo 'static const char xtlite_placeholder[] = "xtlite";' > /tmp/empty.c
$CC -O2 -fPIC -shared -Wl,-soname,libXaw7.so.7 -o /src/images/libXaw7.so.7.0.0 /tmp/empty.c
$CC -O2 -fPIC -shared -Wl,-soname,libXmu.so.6  -o /src/images/libXmu.so.6.2.0  /tmp/empty.c
for f in /src/images/libXt.so.6.0.0 /src/images/libXaw7.so.7.0.0 \
	 /src/images/libXmu.so.6.2.0; do ${CC%gcc}strip "$f"; done
ls -l /src/images/libXt.so.6.0.0 /src/images/libXaw7.so.7.0.0 /src/images/libXmu.so.6.2.0 | awk '{printf "  %-34s %7d\n", $9, $5}'

