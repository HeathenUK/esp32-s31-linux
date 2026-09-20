#!/bin/sh
# Build sdlkeys inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-sdlkeys.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/sdlkeys.c -o /src/rootfs/sdlkeys -lSDL -lpthread
ls -la /src/rootfs/sdlkeys
