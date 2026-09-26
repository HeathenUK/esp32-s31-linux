#!/bin/sh
# brsize.sh - (phase 3a) libGL.so.1 in Buildroot mode (the board TARGET_CFLAGS, the
# musl toolchain, the staging sysroot read-only) for the base3a snapshot and
# this tree, each with its own build-lib.sh: .text, .rodata, stripped size.
# In the build container: ./docker/build.sh "sh /src/gl/bench/brsize.sh". s31, MIT.
set -e
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
# BRSIZE_TREES (phase 3a pixel levers): the name:dir pairs to build
for t in ${BRSIZE_TREES:-base3a:/src/gl/bench/base3a final:/src/gl}; do
  n=${t%%:*}; g=${t#*:}
  GL=$g CC=$CC NM=${CC%gcc}nm ARCHFLAGS="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base" \
    XINC="-isystem $SYSROOT/usr/include" XLIBS="-L$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/usr/lib -lXext -lX11" \
    ZDEFS="" OBJ=/tmp/br-$n OUT=/tmp/br-$n.so sh $g/api/build-lib.sh > /tmp/br-$n.log 2>&1 || { tail /tmp/br-$n.log; exit 1; }
  grep -i "warning" /tmp/br-$n.log | head -3
  echo "$n: $(${CC%gcc}size -A /tmp/br-$n.so | awk '$1==".text"{print ".text", $2} $1==".rodata"{print ".rodata", $2}' | tr '\n' ' ') stripped $(${CC%gcc}strip -o /tmp/br-$n.s /tmp/br-$n.so && wc -c < /tmp/br-$n.s) B"
done
