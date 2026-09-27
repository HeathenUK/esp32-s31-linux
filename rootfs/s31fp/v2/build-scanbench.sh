#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/s31fp/v2/build-scanbench.sh'  scanbench: the one-pass scan's cost over a binary's text, without running it
set -e
T=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
CC=$(ls /src/build/buildroot/host/bin/*-linux-musl-gcc | head -1)
V=/src/rootfs/s31fp/v2
B=${B:-$V/out}
cd $V
python3 mksig2.py $T-objdump $T-objcopy $T-ar "$($T-gcc -print-libgcc-file-name)" > sigs2.h
M="-march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs"
$CC $M -O2 -Wall -o $B/scanbench scanbench.c
$CC $M -O2 -static -fno-builtin -c -o /tmp/noesp.o noesp.c
$CC $M -O2 -static -o $B/scanbench.q scanbench.c /tmp/noesp.o
