#!/bin/sh
# Build sdl2keys inside the build container:
#   ./docker/build.sh 'cd /src && sh rootfs/build-sdl2keys.sh'
set -e
ST=$(ls -d /src/build/buildroot/host/riscv32-*-linux-musl/sysroot 2>/dev/null | head -1)
[ -n "$ST" ] || ST=/src/build/buildroot/staging
CC=$(ls /src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc)
$CC -Os -march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=ilp32 --sysroot=$ST \
    -I$ST/usr/include /src/rootfs/sdl2keys.c -o /src/rootfs/sdl2keys -lSDL2 -lpthread
ls -la /src/rootfs/sdl2keys
