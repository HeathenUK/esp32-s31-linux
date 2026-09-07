#!/bin/sh
# Build cluttest2 inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-cluttest2.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/cluttest2.c -o /src/rootfs/cluttest2
ls -la /src/rootfs/cluttest2
