#!/bin/sh
# Build the ALSA output-router plugin. alsa-lib dlopens it by name, so it must
# install as libasound_module_pcm_<type>.so and be listed explicitly anywhere a
# dependency walk decides what to ship - a DT_NEEDED scan will never find it.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
# -DPIC is not decoration: alsa's global.h selects between the shared and the
# STATIC form of its versioned-symbol macro on "#ifdef PIC", and libtool is
# what normally defines it. Without it the plugin emits the static form, which
# references snd_dlsym_start - a symbol the shared libasound does not export -
# and alsa-lib refuses to load the plugin with a relocation error.
$CC -O2 -Wall -fPIC -DPIC -shared --sysroot=$SYSROOT \
    /src/rootfs/s31route.c \
    -o /src/rootfs/libasound_module_pcm_s31route.so -lasound
ls -la /src/rootfs/libasound_module_pcm_s31route.so
