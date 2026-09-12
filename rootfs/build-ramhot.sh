#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/build-ramhot.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/ramhot.c -o /src/rootfs/ramhot.so -lpthread
ls -la /src/rootfs/ramhot.so
