#!/bin/sh
# Cross-compile an LVGL app for the board.
# Dynamic against the board's musl. libdrm is deliberately NOT linked and is
# not needed: lvdesk/kms.c issues the KMS ioctls directly against the uapi
# headers in the toolchain sysroot, which is all libdrm would have wrapped.
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
# lvdesk drives KMS itself; the other tools here are single-file.
EXTRA=""
LIBS=""
if [ "$APP" = lvdesk ]; then
  EXTRA=/src/lvdesk/kms.c
  # ALSA's mixer API, not a fork to amixer - see the audio popover.
  # ALSA lives in the buildroot sysroot, which SYSROOT has always pointed at
  # and nothing used. Headers to compile against, the .so to link against; the
  # board carries the runtime copy in /usr/lib.
  LIBS="-I$SYSROOT/usr/include -L$SYSROOT/usr/lib -lasound"
fi
echo "linking $APP"
$CC $CFLAGS -o $OUT /src/lvdesk/$APP.c $EXTRA /tmp/lvo/*.o -lm $LIBS
ls -la $OUT
