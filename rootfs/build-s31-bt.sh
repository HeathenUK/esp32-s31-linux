#!/bin/sh
# Build s31-bt inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-s31-bt.sh'
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall -Wno-unused-result --sysroot=$SYSROOT \
    -I/src/shared -I$SYSROOT/usr/include/dbus-1.0 -I$SYSROOT/usr/lib/dbus-1.0/include \
    /src/rootfs/s31-bt.c -o /src/rootfs/s31-bt -ldbus-1 -lsbc
ls -la /src/rootfs/s31-bt
