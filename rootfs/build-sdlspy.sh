#!/bin/sh
# Build the SDL 1.2 LD_PRELOAD spy:
#   ./docker/build.sh 'cd /src && sh rootfs/build-sdlspy.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/sdlspy.c -o /src/rootfs/sdlspy.so -ldl
ls -la /src/rootfs/sdlspy.so
