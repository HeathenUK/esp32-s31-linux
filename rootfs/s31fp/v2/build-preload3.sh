#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/s31fp/v2/build-preload3.sh'
# libs31fp.so v2 CANDIDATE (not installed) + its QEMU and board tests.
set -e
T=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
CC=$(ls /src/build/buildroot/host/bin/*-linux-musl-gcc | head -1)
V=/src/rootfs/s31fp/v2
B=${B:-$V/out}
cd $V
python3 mksig3.py $T-objdump $T-objcopy $T-ar "$($T-gcc -print-libgcc-file-name)" > sigs3.h
M="-march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=ilp32"
# no libc: raw syscalls, one import (environ)
$CC $M -O2 -fPIC -fno-builtin -ffreestanding -fno-stack-protector -Wall -fvisibility=hidden -c -o /tmp/p3.o preload3.c
$CC $M -O2 -fPIC -DS31V2_FENV=1 -c -o /tmp/p3v2.o v2.S
$CC $M -O2 -fPIC -fno-builtin -fno-stack-protector -fvisibility=hidden -DS31V2_FENV=1 -c -o /tmp/p3div.o v2div.c
$CC $M -shared -nostdlib -Wl,-z,now -Wl,--hash-style=gnu -o $B/libs31fp.so /tmp/p3.o /tmp/p3v2.o /tmp/p3div.o $B/lgref.o
$T-strip $B/libs31fp.so
echo "dynamic symbols (exports/imports):"; $T-nm -D $B/libs31fp.so | grep -v " [tTrRdDbB] s31\| A " | head
echo "relocations: $($T-readelf -r $B/libs31fp.so | grep -c R_RISCV)"
ls -l $B/libs31fp.so
# static QEMU test: the constructor patches THIS program's own libgcc copies,
# then the checker compares those (patched) entry points with the references
$CC $M -O2 -fno-builtin -Wall -c -o /tmp/p3s.o preload3.c
$CC $M -O2 -Wall -DPATCHED -c -o /tmp/pt.o v2test.c
$CC $M -O2 -c -o /tmp/p3sv2.o v2.S
$CC $M -O2 -c -o /tmp/p3sdiv.o v2div.c
printf 'volatile unsigned long v2_refcalls;\n' > /tmp/nw.c && $CC $M -c -o /tmp/nw.o /tmp/nw.c
$CC $M -O2 -fno-builtin -c -o /tmp/noesp.o noesp.c
$CC $M -O2 -c -o /tmp/hw.o -march=rv32imafdc_zicsr_zifencei_zba_zbb_zbs hw.c
$CC $M -O2 -c -o /tmp/hwstub.o $B/hwstub.c
$CC $M -static -o $B/ptest.q /tmp/pt.o /tmp/p3s.o /tmp/p3sv2.o /tmp/p3sdiv.o $B/lgref.o /tmp/nw.o /tmp/noesp.o /tmp/hw.o $B/oldc.o $B/oldmul.o $B/oldadd.o $B/oldconv.o
# the same, dynamic, for the board: the REAL preload patches it at load
$CC $M -O2 -Wall -DPATCHED -DNO_HW -c -o /tmp/ptb.o v2test.c
O=/src/rootfs/s31fp
$CC $M -O2 -fPIC -DS31FP_PRELOAD -DS31FP_ASM_MUL -DS31FP_ASM_ADD -c -o /tmp/oc.o $O/s31fp.c
for f in muldf3 adddf3 conv; do $CC $M -fPIC -c -o /tmp/o$f.o $O/$f.S; done
$CC $M -fPIC -c -o /tmp/hwstubp.o $B/hwstub.c
$CC $M -o $B/ptest-dyn /tmp/ptb.o /tmp/nw.o $B/lgref.o /tmp/hwstubp.o /tmp/oc.o /tmp/omuldf3.o /tmp/oadddf3.o /tmp/oconv.o
file $B/ptest-dyn 2>/dev/null | cut -c1-120 || true
ls -l $B/ptest.q $B/ptest-dyn
