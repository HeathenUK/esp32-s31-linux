#!/bin/bash
. /home/user/esp32-s31-linux/tools/cloud/env.sh
P=/home/user/esp32-s31-linux/build/cloud/probes; E=$P/evolution-evidence
L=/home/user/esp32-s31-linux/toolchain/riscv32-esp-linux-musl/riscv32-esp-linux-musl/sysroot/usr/lib/libc.so
rec(){ name=$1; shift; out=$E/$name.txt
  { echo "# probe: $name"; echo "# date: $(date -u +%FT%TZ)"; echo "# cwd: $PWD"; echo "# cmd: $*";
    echo "# qemu: $(qemu-riscv32 --version 2>/dev/null | head -1)"; for f in $INPUTS; do echo "# md5 $(md5sum $f)"; done; echo "# ---- stdout/stderr ----";
    t0=$SECONDS; "$@" 2>&1; echo "# rc=$? elapsed=$((SECONDS-t0))s"; } > $out 2>&1; }
cd $P/coverage/v
for fo in sin:3c55e cos:2c296 sincos:3c638 exp:2d96c log:36dae pow:39532 atan:291a0 atan2:294e6 sinf:3cd00 cosf:2c348 sincosf:3c7a2 powf:39eca; do
  f=${fo%%:*}; o=${fo##*:}; INPUTS="mt.c mt $L" rec libm_rebuild_vs_libc_$f s31-qemu ./mt $L $f $o 500000; done
INPUTS="mt.c mt $L" rec libm_negctl_floor_vs_ceil s31-qemu ./mt $L floor 2be6c 100000
cd $P/safety-arith; INPUTS="rm.c rm $L" rec libc_helpers_vs_libgcc_allmodes s31-qemu ./rm $L 200000
