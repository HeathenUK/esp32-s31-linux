#!/bin/sh
set -eu
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
TOOLS=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
"$TOOLS-gcc" -O2 -Wall --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" \
	/src/rootfs/sdlkeys.c -o /src/rootfs/sdlkeys.bin -lSDL -lpthread
"$TOOLS-strip" /src/rootfs/sdlkeys.bin
ls -l /src/rootfs/sdlkeys.bin
