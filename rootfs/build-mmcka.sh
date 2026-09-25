#!/bin/sh
# Build mmcka inside the build container (~5 s; writes only /src/rootfs, so
# it does not touch the kernel output volume - but do not run it while a
# `make linux` is in flight on the same container image either):
#   ./docker/build.sh 'cd /src && sh rootfs/build-mmcka.sh'
# Then, over Wi-Fi (the XIP /usr/sbin/mmcka from s31-tools stays as-is):
#   python3 scripts/board/deploy.py rootfs/mmcka /root/mmcka
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include \
    /src/rootfs/mmcka.c -o /src/rootfs/mmcka
ls -la /src/rootfs/mmcka
