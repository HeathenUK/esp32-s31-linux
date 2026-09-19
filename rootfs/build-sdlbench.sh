#!/bin/sh
# ./docker/build.sh 'sh rootfs/build-sdlbench.sh'
set -eu
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
TOOLS=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
for version in 1 2; do
    lib=SDL; inc=usr/include
    if [ "$version" = 2 ]; then lib=SDL2; inc=usr/include/SDL2; fi
    source_id=$(sha256sum /src/rootfs/sdlbench$version.c /src/rootfs/sdlbench-common.h /src/rootfs/sdlbench-runtime.h | sha256sum | cut -d' ' -f1)
    "$TOOLS-gcc" -DSDLBENCH_SOURCE_ID=\"$source_id\" -O2 -g -Wall -Wextra --sysroot="$SYSROOT" -I"$SYSROOT/$inc" \
        /src/rootfs/sdlbench$version.c -o /src/rootfs/sdlbench$version.bin -l"$lib" -lpthread
    "$TOOLS-strip" /src/rootfs/sdlbench$version.bin
    visinc=usr/include/SDL
    if [ "$version" = 2 ]; then visinc=usr/include/SDL2; fi
    "$TOOLS-gcc" -O2 -Wall -Wextra --sysroot="$SYSROOT" -I"$SYSROOT/$visinc" \
        /src/rootfs/sdlbench-visual.c -o /src/rootfs/sdlbench-visual$version.bin -l"$lib" -lpthread
    "$TOOLS-strip" /src/rootfs/sdlbench-visual$version.bin
    "$TOOLS-readelf" -d /src/rootfs/sdlbench$version.bin | grep NEEDED
done
