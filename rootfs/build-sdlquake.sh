#!/bin/sh
# Cross-build sdlquake 1.0.9 (id Software's Quake, Sam Lantinga's SDL 1.2
# port, GPL) for the board, unmodified. Its autoconf predates RISC-V and
# cannot be told about the cross toolchain, so this compiles the source list
# straight out of Makefile.am with the Buildroot toolchain instead.
#
#   sdlquake/           the unpacked sdlquake-1.0.9 tarball (gitignored)
#   rootfs/sdlquake     the output binary
#
# Flags, and why:
#   -std=gnu89                  GCC 14 defaults to C23, where false/true are keywords
#   -fcommon                    1999 code defines globals in headers
#   -Wno-error=...              GCC 14 makes 1990s C errors; keep them warnings
#   -Did386=0                   no x86 asm; the .S files are skipped
#   -DSDL -DELF                 what configure.in adds
#   -ffast-math                 what id's own CFLAGS used
#   -fsingle-precision-constant this board has F but no D: a double literal
#                               drags every expression into soft-float
set -e
SRC=/src/sdlquake
OUT=/src/rootfs/sdlquake
CC=/src/build/buildroot/host/bin/riscv32-esp-linux-musl-gcc
SDLCFG=/src/build/buildroot/staging/usr/bin/sdl-config
CFLAGS="-std=gnu89 -g -O2 -ffast-math -fsingle-precision-constant -fcommon -Did386=0 -DSDL -DELF \
 -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
 -Wno-error=int-conversion -Wno-error=return-mismatch -w $($SDLCFG --cflags)"
cd "$SRC"
# .c files named in sdlquake_SOURCES plus NONX86_SRCS; never the .S/.h/.bat
LIST=$(awk '/^sdlquake_SOURCES/,/^$/' Makefile.am | tr -d '\\' | tr ' \t' '\n\n' | grep '\.c$')
NONX86=$(awk '/^NONX86_SRCS/,/^$/' Makefile.am | tr -d '\\' | tr ' \t' '\n\n' | grep '\.c$')
OBJS=""
mkdir -p /tmp/sq
for f in $LIST $NONX86; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
	o=/tmp/sq/${f%.c}.o
	$CC $CFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done
$CC -O2 -o "$OUT" $OBJS $($SDLCFG --libs) -lm
cp "$OUT" "$OUT.dbg"		# unstripped, for resolving a kernel fault PC
/src/build/buildroot/host/bin/riscv32-esp-linux-musl-strip "$OUT"
ls -la "$OUT"
