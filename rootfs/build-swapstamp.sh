#!/bin/sh
# Build the per-swap client timer: ./docker/build.sh 'cd /src && sh rootfs/build-swapstamp.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT /src/rootfs/swapstamp.c -o /src/rootfs/swapstamp.so -ldl
ls -la /src/rootfs/swapstamp.so
