#!/bin/sh
# Build sockbench inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-sockbench.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/sockbench.c -o /src/rootfs/sockbench
ls -la /src/rootfs/sockbench
