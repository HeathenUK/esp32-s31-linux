#!/bin/sh
set -eu
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -Wextra -fno-builtin -fno-tree-loop-distribute-patterns \
    -march=rv32imafc_zicsr_zifencei -mabi=ilp32 --sysroot="$SYSROOT" \
    /src/rootfs/pie-affinity-test.c /src/rootfs/pie-affinity-test.S \
    -o /src/rootfs/pie-affinity-test
