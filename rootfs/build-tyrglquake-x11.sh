#!/bin/sh
# Cross-build TyrQuake 0.71 GLQuake (bin/tyr-glquake, GL plan stage 6 - the
# owner's target) with the same stock-source recipe as build-tyrquake-x11.sh:
# its Makefile's DEFAULT Linux targets - X11 video
# and input (the SDL ones are marked experimental), SDL2 sound (the Linux
# default is pulseaudio, absent here). Stock source, its own build system. For
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
OUT=/src/rootfs/tyr-glquake-x11
CC=/src/build/buildroot/host/bin/riscv32-esp-linux-musl-gcc
STAGING=/src/build/buildroot/staging
cd "$SRC"
make -j4 BUILD_DIR=/tmp/tqgx TARGET_OS=UNIX TARGET_UNIX=linux USE_SDL=N VID_TARGET=x11 IN_TARGET=x11 SND_TARGET=sdl USE_X86_ASM=N \
	CC="$CC" STRIP=true \
	CD_TARGET=null LOCALBASE= X11BASE= \
	CFLAGS="-std=gnu11 -O2 -ffast-math -fsingle-precision-constant -Wno-error -w" \
	SDL_CFLAGS="-I$STAGING/usr/include/SDL2 -D_REENTRANT" \
	SDL_LFLAGS="-L$STAGING/usr/lib -lSDL2 -lm" \
	bin/tyr-glquake 2>&1 | grep -vE "^\s*$" | tail -15
cp bin/tyr-glquake "$OUT.dbg"
cp bin/tyr-glquake "$OUT"
/src/build/buildroot/host/bin/riscv32-esp-linux-musl-strip "$OUT"
ls -la "$OUT"
