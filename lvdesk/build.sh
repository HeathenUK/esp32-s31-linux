#!/bin/sh
# Cross-compile an LVGL app for the board.
# Dynamic against the board's musl. libdrm is deliberately NOT linked: it came
# in as an Xorg dependency and leaves with X, and the fbdev backend does not
# need it. Re-enable LV_USE_LINUX_DRM and -ldrm if the DRM path is revisited.
set -e
SYSROOT=$(echo /src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot)
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
APP=${1:-lvdesk}
OUT=/src/lvdesk/$APP.bin
CFLAGS="-O2 -I/src/lvgl -I/src/lvdesk -DLV_CONF_INCLUDE_SIMPLE "
rm -rf /tmp/lvo /tmp/lvfail; mkdir -p /tmp/lvo; : > /tmp/lverr
find /src/lvgl/src -name '*.c' > /tmp/srcs
echo "compiling $(wc -l < /tmp/srcs) LVGL sources on $(nproc) cores..."
cat /tmp/srcs | xargs -P "$(nproc)" -I{} sh -c \
  'o=/tmp/lvo/$(echo "{}" | md5sum | cut -c1-16).o; '"$CC $CFLAGS"' -c "{}" -o "$o" 2>>/tmp/lverr || echo "FAILED {}" >> /tmp/lvfail'
[ -s /tmp/lvfail ] && { echo "--- failures:"; head -3 /tmp/lvfail; grep -m5 'error:' /tmp/lverr; exit 1; }
echo "linking $APP"
$CC $CFLAGS -o $OUT /src/lvdesk/$APP.c /tmp/lvo/*.o -lm
ls -la $OUT
