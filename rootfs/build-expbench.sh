#!/bin/sh
# Build expbench inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-expbench.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/expbench.c -o /src/rootfs/expbench
ls -la /src/rootfs/expbench
