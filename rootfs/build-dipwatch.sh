#!/bin/sh
# Build the C dip sampler: ./docker/build.sh 'cd /src && sh rootfs/build-dipwatch.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT /src/rootfs/dipwatch.c -o /src/rootfs/dipwatch
ls -la /src/rootfs/dipwatch
