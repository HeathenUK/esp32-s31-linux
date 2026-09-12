#!/bin/sh
# Build s31swapon inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-s31swapon.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/s31swapon.c -o /src/rootfs/s31swapon
ls -la /src/rootfs/s31swapon
