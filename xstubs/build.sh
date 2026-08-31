#!/bin/sh
# Build the four stub libraries. Each keeps the soname of the library it
# replaces, so the loader picks it up by library path alone - the same
# integration as xlite, and nothing above them is rebuilt.
set -e
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
OUT=/src/images
S=/src/xstubs/xstubs.c
build() {			# soname, output name, -D flag
	$CC -O2 -fPIC -shared -Wall -I"$SYSROOT/usr/include" -D"$3" \
	rm -f "$OUT/$2"	# a failed build must leave nothing to ship
		-Wl,-soname,"$1" -o "$OUT/$2" "$S"
	${CC%gcc}strip "$OUT/$2"
	ls -l "$OUT/$2" | awk '{printf "  %-24s %7d bytes\n", $9, $5}'
}
build libICE.so.6  libICE.so.6.3.0    STUB_ICE
build libSM.so.6   libSM.so.6.0.1     STUB_SM
build libXext.so.6 libXext.so.6.4.0   STUB_XEXT
build libXpm.so.4  libXpm.so.4.11.0   STUB_XPM
# Not X libraries, but the same argument: xfiles references 13 fontconfig
# symbols and ONE Xcursor symbol, and pays 80 kB of RSS for them - fontconfig
# alone drags in freetype, expat and zlib. See the notes in xstubs.c.
build libfontconfig.so.1 libfontconfig.so.1.16.0 STUB_FONTCONFIG
build libXcursor.so.1    libXcursor.so.1.0.2     STUB_XCURSOR
