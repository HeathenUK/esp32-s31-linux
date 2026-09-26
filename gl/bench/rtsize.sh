#!/bin/sh
# rtsize.sh - (phase 4 L1) libGL.so.1 in Buildroot mode (brsize.sh's flags:
# the board TARGET_CFLAGS and TARGET_LDFLAGS, the musl toolchain, the
# staging sysroot read-only) for each name:dir tree in RTSIZE_TREES, with
# its own build-lib.sh: the size of every allocated section that changed,
# the stripped file, the LOAD segments, and ramtext.py's line.
# In the build container:
#   ./docker/build.sh 'RTSIZE_TREES="base:/src/gl/bench/p4l1base l1:/src/gl" sh /src/gl/bench/rtsize.sh'
# s31, MIT.
set -e
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
for t in ${RTSIZE_TREES:?name:dir ...}; do
  n=${t%%:*}; g=${t#*:}
  GL=$g CC=$CC NM=${CC%gcc}nm ARCHFLAGS="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base" \
    XINC="-isystem $SYSROOT/usr/include" XLIBS="-L$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/usr/lib -lXext -lX11" \
    LDFLAGS="-Wl,--as-needed -Wl,-Bsymbolic-functions" \
    ZDEFS="" OBJ=/tmp/rt-$n OUT=/tmp/rt-$n.so sh $g/api/build-lib.sh > /tmp/rt-$n.log 2>&1 || { tail /tmp/rt-$n.log; exit 1; }
  grep -iE "warning|^ramtext: .*bytes" /tmp/rt-$n.log | head -4
  ${CC%gcc}strip -o /tmp/rt-$n.s /tmp/rt-$n.so
  echo "$n: $(${CC%gcc}size -A /tmp/rt-$n.so | awk '$1 ~ /^\.(text|s31hot|rodata|data|bss|data\.rel\.ro|got)$/ {printf "%s %d  ", $1, $2}')stripped $(wc -c < /tmp/rt-$n.s) B"
  ${CC%gcc}readelf -lW /tmp/rt-$n.s | awk '$1 == "LOAD" {print "   LOAD off " $2 " vaddr " $3 " filesz " $5 " memsz " $6 " " $7}'
done
