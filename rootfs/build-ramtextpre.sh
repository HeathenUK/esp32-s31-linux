#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/build-ramtextpre.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -fPIC -shared --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/ramtextpre.c -o /src/rootfs/ramtext.so
ls -la /src/rootfs/ramtext.so
