#!/bin/sh
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/fpsonly.c -o /src/rootfs/fpsonly.so -ldl
ls -la /src/rootfs/fpsonly.so
