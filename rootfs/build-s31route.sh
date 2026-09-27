#!/bin/sh
# Build the ALSA output-router plugin. alsa-lib dlopens it by name, so it must
# install as libasound_module_pcm_<type>.so and be listed explicitly anywhere a
# dependency walk decides what to ship - a DT_NEEDED scan will never find it.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
NM=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-nm
# -DPIC is not decoration: alsa's global.h selects between the shared and the
# STATIC form of its versioned-symbol macro on "#ifdef PIC", and libtool is
# what normally defines it. Without it the plugin emits the static form, which
# references snd_dlsym_start - a symbol the shared libasound does not export -
# and alsa-lib refuses to load the plugin with a relocation error.
#
# The resampler (s31resample.h) is single precision by design: this core has
# F and no D. -fsingle-precision-constant and -Wdouble-promotion keep a stray
# double literal from sneaking in, and the nm check below fails the build if
# any soft-double helper (__adddf3, __muldf3, ...) is referenced anyway.
$CC -O2 -Wall -Wdouble-promotion -fsingle-precision-constant -fPIC -DPIC \
    -shared --sysroot=$SYSROOT -I/src/rootfs \
    /src/rootfs/s31route.c \
    -o /src/rootfs/libasound_module_pcm_s31route.so -lasound
if $NM -u /src/rootfs/libasound_module_pcm_s31route.so | grep -E '__[a-z]+df[0-9]|__extendsfdf|__truncdfsf'; then
	echo "ERROR: s31route references soft-double helpers" >&2
	exit 1
fi
ls -la /src/rootfs/libasound_module_pcm_s31route.so
