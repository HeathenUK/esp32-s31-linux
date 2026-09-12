#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/build-s31_freertos_mem.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include -I/src/shared \
    -I/src/linux-71-port/include/uapi -I/src/bootloader/main \
    /src/rootfs/s31_freertos_mem.c -o /src/rootfs/s31_freertos_mem
ls -la /src/rootfs/s31_freertos_mem
