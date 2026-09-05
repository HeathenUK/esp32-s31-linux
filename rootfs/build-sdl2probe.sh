#!/bin/sh
# Build sdl2probe inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-sdl2probe.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include/SDL2 -D_REENTRANT \
    /src/rootfs/sdl2probe.c -o /src/rootfs/sdl2probe -lSDL2
ls -la /src/rootfs/sdl2probe
