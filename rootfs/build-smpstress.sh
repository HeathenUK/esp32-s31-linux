#!/bin/sh
set -eu
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -Wextra -march=rv32imafc_zicsr_zifencei -mabi=ilp32 \
    --sysroot="$SYSROOT" /src/rootfs/smpstress.c -o /src/rootfs/smpstress -pthread
