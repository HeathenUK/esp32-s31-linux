#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/s31fp/build.sh'
set -e
T=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
CC=$(ls /src/build/buildroot/host/bin/*-gcc | head -1)
cd /src/rootfs/s31fp
python3 mksig.py $T-objdump $T-objcopy $T-ar "$($T-gcc -print-libgcc-file-name)" > sigs.h
$CC -O2 -fPIC -shared -Wall -DS31FP_PRELOAD $S31FP_EXTRA -DS31FP_ASM_MUL -fvisibility=hidden -o libs31fp.so preload.c s31fp.c muldf3.S
$T-strip libs31fp.so
ls -l libs31fp.so
# board-side verifier: asm/C routines against libgcc's own, random + edge operands
$CC -O2 -DS31FP_PRELOAD -DS31FP_ASM_MUL -o s31fp-verify verify.c s31fp.c muldf3.S
ls -l s31fp-verify
