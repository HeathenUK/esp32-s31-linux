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
	/src/xftlite/xftlite.c \
	-L/src/images -l:libXrender.so.1.3.0 -Wl,--no-as-needed
${CC%gcc}strip /src/images/libXft.so.2.3.9

cat > /tmp/ftstub.c <<'FTS'
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libfreetype, reduced to nothing at all - because that is exactly what is
 * needed.
 *
 * st links against libfreetype.so.6 (Xft's headers drag it into the link) but
 * calls NOT ONE FT_ function: `nm -D --undefined-only st | grep ^FT_` is
 * empty. Only the dynamic loader cares, and only that a library with this
 * soname exists. The real one is 660 kB.
 *
 * Shipping the real library to satisfy a dependency nobody calls would put a
 * full-fat font engine on a 15.4 MB board and break the standing rule that
 * nothing stock goes on the SD. This is the honest alternative: if some future
 * client genuinely calls into FreeType it will fail to resolve at load time
 * and say so, rather than silently working and costing 660 kB.
 */
FTS
$CC -shared -fPIC -O2 -nostdlib -Wl,-soname,libfreetype.so.6 \
    -o /src/images/libfreetype.so.6.20.6 /tmp/ftstub.c
${CC%gcc}strip /src/images/libfreetype.so.6.20.6

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
