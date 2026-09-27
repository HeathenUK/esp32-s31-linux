#!/bin/sh
. /home/user/esp32-s31-linux/tools/cloud/env.sh
w=$1; shift
for v in "$@"; do s31-qemu ./mix s16 $v -32768 32768 | tail -1; done > s16_w$w.txt 2>&1
echo DONE >> s16_w$w.txt
