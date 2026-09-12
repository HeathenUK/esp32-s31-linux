#!/bin/sh
# Build ramtext inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-ramtext.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -mno-relax -Wl,--no-relax --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/ramtext.c -o /src/rootfs/ramtext
ls -la /src/rootfs/ramtext
