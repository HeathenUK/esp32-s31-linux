#!/bin/sh
# ./docker/build.sh '$S31_MAKE s31fp-v3'
# libs31fp.so v3 CANDIDATE = v2 (soft-double, unchanged sources in ../v2)
# + A1 s31str.c + A2 s31clk.c; and its QEMU/board test programs.
set -e
T=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
CC=$(ls /src/build/buildroot/host/bin/*-linux-musl-gcc | head -1)
V2=/src/rootfs/s31fp/v2
V=/src/rootfs/s31fp/v3
B=${B:-$V/out}
V2_BUILD=${V2_BUILD:-/src/build/s31fp-v2}
W=$B/obj
mkdir -p $W $B
M="-march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=ilp32"
# v2, exactly as v2/build-preload3.sh compiles it (sigs3.h as committed)
$CC $M -O2 -fPIC -fno-builtin -ffreestanding -fno-stack-protector -Wall -fvisibility=hidden -c -o $W/p3.o $V2/preload3.c
$CC $M -O2 -fPIC -DS31V2_FENV=1 -c -o $W/p3v2.o $V2/v2.S
$CC $M -O2 -fPIC -fno-builtin -fno-stack-protector -fvisibility=hidden -DS31V2_FENV=1 -c -o $W/p3div.o $V2/v2div.c
# v3
F="$M -O2 -fPIC -fno-builtin -ffreestanding -fno-stack-protector -fno-strict-aliasing -Wall -Wextra -fvisibility=hidden"
$CC $F -c -o $W/s31str.o $V/s31str.c
$CC $F -c -o $W/s31clk.o $V/s31clk.c
$CC $M -shared -nostdlib -Wl,-z,now -Wl,--hash-style=gnu -o $B/libs31fp.so \
	$W/p3.o $W/p3v2.o $W/p3div.o $V2_BUILD/lgref.o $W/s31str.o $W/s31clk.o
$T-strip $B/libs31fp.so
echo "imports:"; $T-nm -D -u $B/libs31fp.so | tr '\n' ' '; echo
echo "exports:"; $T-nm -D --defined-only $B/libs31fp.so | awk '{print $3}' | tr '\n' ' '; echo
$T-readelf -lW $B/libs31fp.so | grep -E "TLS|LOAD" || true
# no TLS segment: one changes musl's thread stack alignment (s31str.c)
! $T-readelf -lW $B/libs31fp.so | grep -q " TLS " || { echo "libs31fp.so has a TLS segment"; exit 1; }
# nothing from libgcc may be needed (the library links no libgcc)
U=$($T-nm -D -u $B/libs31fp.so | awk '{print $2}' | grep -v -E '^(environ|dlsym|malloc|free|_pthread_cleanup_push|_pthread_cleanup_pop|pthread_key_create|pthread_getspecific|pthread_setspecific)$' || true)
[ -z "$U" ] || { echo "UNEXPECTED IMPORTS: $U"; exit 1; }
ls -l $B/libs31fp.so; md5sum $B/libs31fp.so
# tests (dynamic, against the target musl)
$CC $M -O2 -fno-builtin -Wall -o $B/strtest $V/strtest.c -lpthread
$CC $M -O2 -Wall -o $B/clktest $V/clktest.c -lpthread
$CC $M -O2 -Wall -o $B/v3spawn $V/v3spawn.c
$CC $M -O2 -o $B/v3nop $V/v3nop.c
$CC $M -O2 -Wall -o $B/v3work $V/v3work.c
# the QEMU run needs the target's dynamic loader and libc
S=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
mkdir -p $B/qroot/lib
cp -L $S/lib/ld-musl-riscv32-sf.so.1 $B/qroot/lib/
ls -l $B/strtest $B/clktest $B/v3spawn $B/v3nop $B/v3work
