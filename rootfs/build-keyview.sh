#!/bin/sh
# Build keyview inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-keyview.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT /src/rootfs/keyview.c \
    -o /src/rootfs/keyview -lX11
ls -la /src/rootfs/keyview
