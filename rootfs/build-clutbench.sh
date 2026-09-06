#!/bin/sh
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT /src/rootfs/clutbench.c -o /src/rootfs/clutbench
ls -la /src/rootfs/clutbench
