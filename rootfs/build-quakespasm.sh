#!/bin/sh
# Cross-build QuakeSpasm 0.96.3 - GLQuake on SDL 1.2 (the owner's target,
# docs/gl-plan-2026-09-25.md) - stock source, its own Makefile, for the board.
#
#   tyrquake/quakespasm-quakespasm-0.96.3/   the unpacked GitHub tag tarball
#                                             (gitignored, like tyrquake-0.71)
#   rootfs/quakespasm                         the output binary (+ .dbg)
#
# Build inputs only: USE_SDL2=0 is the Makefile's own default (SDL 1.2, now
# built with GL: BR2_PACKAGE_S31_LIBGL_SDL_OPENGL); SDL_CONFIG points at the
# board's sdl-config; the MP3 and Vorbis music codecs are its own switches and
# are off (no libmad/libvorbis on the board - WAV stays); CC is the cross
# compiler; -fsingle-precision-constant as for sdlquake and TyrQuake (F without
# D). Memory is a RUNTIME option (-heapsize, in kB), not a build change.
set -e
SRC=/src/tyrquake/quakespasm-quakespasm-0.96.3/Quake
OUT=/src/rootfs/quakespasm
CC=/src/build/buildroot/host/bin/riscv32-esp-linux-musl-gcc
STAGING=/src/build/buildroot/staging
cd "$SRC"
make clean >/dev/null 2>&1 || true
make -j$(nproc) CC="$CC" SDL_CONFIG="$STAGING/usr/bin/sdl-config" \
	USE_SDL2=0 USE_CODEC_MP3=0 USE_CODEC_VORBIS=0 USE_CODEC_WAVE=1 \
	CPUFLAGS="-fsingle-precision-constant" STRIP=true 2>&1 | grep -iE "error|warning: implicit|LINK|quakespasm" | tail -15
cp quakespasm "$OUT.dbg"
cp quakespasm "$OUT"
/src/build/buildroot/host/bin/riscv32-esp-linux-musl-strip "$OUT"
ls -la "$OUT"
