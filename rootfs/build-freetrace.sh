#!/bin/sh
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/freetrace.c -o /src/rootfs/freetrace.so -ldl
ls -la /src/rootfs/freetrace.so
