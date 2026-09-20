#!/bin/sh
set -eu
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include /src/rootfs/ringhammer.c -o /src/rootfs/ringhammer -lX11
ls -l /src/rootfs/ringhammer
