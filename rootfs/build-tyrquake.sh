#!/bin/sh
# Cross-build TyrQuake 0.71 (software renderer, SDL2 video/input/sound) for
# the board, unmodified, through its own Makefile.
#
#   tyrquake/tyrquake-0.71/   the unpacked tarball (gitignored)
#   rootfs/tyr-quake          the output binary (+ .dbg unstripped)
#
# The Makefile takes CC and the SDL flags as variables, so no autoconf fight:
# TARGET_OS=UNIX TARGET_UNIX=linux picks the unix sys/cd files, USE_SDL=Y the
# SDL targets, USE_X86_ASM=N the C paths. -fsingle-precision-constant for the
# same reason as sdlquake: F without D.
set -e
SRC=/src/tyrquake/tyrquake-0.71
OUT=/src/rootfs/tyr-quake
CC=/src/build/buildroot/host/bin/riscv32-esp-linux-musl-gcc
STAGING=/src/build/buildroot/staging
cd "$SRC"
make -j4 BUILD_DIR=/tmp/tq TARGET_OS=UNIX TARGET_UNIX=linux USE_SDL=Y USE_X86_ASM=N \
	CC="$CC" STRIP=true \
	CD_TARGET=null LOCALBASE= X11BASE= \
	CFLAGS="-std=gnu11 -O2 -ffast-math -fsingle-precision-constant -Wno-error -w" \
	SDL_CFLAGS="-I$STAGING/usr/include/SDL2 -D_REENTRANT" \
	SDL_LFLAGS="-L$STAGING/usr/lib -lSDL2 -lm" \
	bin/tyr-quake 2>&1 | grep -vE "^\s*$" | tail -15
cp bin/tyr-quake "$OUT.dbg"
cp bin/tyr-quake "$OUT"
/src/build/buildroot/host/bin/riscv32-esp-linux-musl-strip "$OUT"
ls -la "$OUT"
