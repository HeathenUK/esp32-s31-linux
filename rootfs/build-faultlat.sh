#!/bin/sh
# Build faultlat inside the build container (~5 s; writes only /src/rootfs, so
# it does not touch the kernel output volume - but do not run it while a
# `make linux` is in flight on the same container image either):
#   ./docker/build.sh 'cd /src && sh rootfs/build-faultlat.sh'
# Then, over Wi-Fi:
#   python3 scripts/board/deploy.py rootfs/faultlat /root/faultlat
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/faultlat.c -o /src/rootfs/faultlat
ls -la /src/rootfs/faultlat
