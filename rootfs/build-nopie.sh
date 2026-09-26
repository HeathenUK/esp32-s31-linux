#!/bin/sh
# Build the PIE-caller counter: ./docker/build.sh 'cd /src && sh rootfs/build-nopie.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT /src/rootfs/nopie.c -o /src/rootfs/nopie.so
ls -la /src/rootfs/nopie.so
