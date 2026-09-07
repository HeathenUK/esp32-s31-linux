#!/bin/sh
# Build s31-coex inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-s31-coex.sh'
#
# It includes shared/s31_hosted_sram.h, the ONE definition of the control
# protocol that hart0 and Linux both compile against - so an op added on one
# side and not the other cannot happen silently.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
$CC -O2 -Wall --sysroot=$SYSROOT -I/src/shared /src/rootfs/s31-coex.c \
    -o /src/rootfs/s31-coex
ls -la /src/rootfs/s31-coex
