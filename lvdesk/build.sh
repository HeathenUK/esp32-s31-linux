#!/bin/sh
# Cross-compile an LVGL app for the board.
# Dynamic, not static: the DRM backend needs libdrm and buildroot ships only a
# shared one. The board already has the matching musl and libdrm.so.2.
set -e
SYSROOT=$(echo /src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot)
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
APP=${1:-lvdesk}
OUT=/src/lvdesk/$APP.bin
CFLAGS="-O2 -I/src/lvgl -I/src/lvdesk -DLV_CONF_INCLUDE_SIMPLE -I$SYSROOT/usr/include -I$SYSROOT/usr/include/libdrm"
rm -rf /tmp/lvo /tmp/lvfail; mkdir -p /tmp/lvo; : > /tmp/lverr
find /src/lvgl/src -name '*.c' > /tmp/srcs
echo "compiling $(wc -l < /tmp/srcs) LVGL sources on $(nproc) cores..."
cat /tmp/srcs | xargs -P "$(nproc)" -I{} sh -c \
  'o=/tmp/lvo/$(echo "{}" | md5sum | cut -c1-16).o; '"$CC $CFLAGS"' -c "{}" -o "$o" 2>>/tmp/lverr || echo "FAILED {}" >> /tmp/lvfail'
[ -s /tmp/lvfail ] && { echo "--- failures:"; head -3 /tmp/lvfail; grep -m5 'error:' /tmp/lverr; exit 1; }
echo "linking $APP"
$CC $CFLAGS -o $OUT /src/lvdesk/$APP.c /tmp/lvo/*.o -L$SYSROOT/usr/lib -ldrm -lm
ls -la $OUT
