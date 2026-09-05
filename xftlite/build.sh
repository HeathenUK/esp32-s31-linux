#!/bin/sh
# Build xftlite - our libXft replacement - plus a libxkbfile stub.
#
# Both keep the soname of the library they replace, so putting them on the
# library path is the whole integration and nothing above them is rebuilt.
#
# libxkbfile is here because it is the ONLY thing on this board that needs
# libxcb, and it drags libXau and libXdmcp behind it - 68 kB resident for the
# one function xclock calls (XkbStdBell, which rings a bell there is no
# hardware for).
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
# Bind internal calls at link time - see xstubs/build.sh for the reasoning.
# These libraries are built outside Buildroot, so BR2_TARGET_LDFLAGS misses them.
LDHARD="-Wl,-Bsymbolic-functions"

rm -f /src/images/libXft.so.2.3.9	# a failed build must leave nothing to ship
$CC -O2 -fPIC -shared -Wall -Wno-unused-parameter \
	-I"$SYSROOT/usr/include" -I"$SYSROOT/usr/include/freetype2" \
	$LDHARD \
	-Wl,-soname,libXft.so.2 -o /src/images/libXft.so.2.3.9 \
	/src/xftlite/xftlite.c
${CC%gcc}strip /src/images/libXft.so.2.3.9

cat > /tmp/xkbstub.c <<'XKB'
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libxkbfile, reduced to the one call anything here makes.
 *
 * It is the only library on this board that needs libxcb, which brings libXau
 * and libXdmcp with it: 68 kB resident so that xclock can ring a bell on
 * hardware that has no bell.
 */
#include <stdio.h>
int XkbStdBell(void *dpy, unsigned long win, int percent, unsigned int name)
{
	(void)dpy; (void)win; (void)percent; (void)name;
	return 1;			/* there is no bell on this board */
}
XKB
$CC -O2 -fPIC -shared -Wall $LDHARD -Wl,-soname,libxkbfile.so.1 \
	-o /src/images/libxkbfile.so.1.0.2 /tmp/xkbstub.c
${CC%gcc}strip /src/images/libxkbfile.so.1.0.2

ls -l /src/images/libXft.so.2.3.9 /src/images/libxkbfile.so.1.0.2 |
	awk '{printf "  %-36s %7d bytes\n", $9, $5}'
