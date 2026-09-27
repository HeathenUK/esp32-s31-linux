#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/audiofp/build.sh'
# Instruments for artifacts/audio/first-principles-2026-09-27:
#   oplbench  - OpenTyrian's own opl.c + lds_play.c, extracted UNMODIFIED from
#               the Buildroot tarball and compiled with the flags its Makefile
#               uses, driven like its audio callback (see oplbench.c)
#   sdltone1  - sdltone.c against SDL 1.2;  sdltone2 - against SDL 2
set -e
D=/src/rootfs/audiofp
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=$(ls /src/build/buildroot/host/bin/*-linux-musl-gcc | head -1)
W=/tmp/audiofp-ot
rm -rf $W && mkdir -p $W
tar xzf /src/buildroot/dl/opentyrian/opentyrian-cf5dbeb69eebd9ef9afc4473088d9469b79589eb.tar.gz -C $W
# the same patch Buildroot applies before building the shipped binary
(cd $W/opentyrian-* && patch -p1 -s < /src/buildroot/package/opentyrian/0001-Move-definitions-that-don-t-need-to-be-exposed-from-opl-h-to-opl-c.patch)
cp $W/opentyrian-*/src/opl.c $W/opentyrian-*/src/opl.h $W/opentyrian-*/src/lds_play.c \
   $W/opentyrian-*/src/lds_play.h $W/opentyrian-*/src/loudness.h $W/opentyrian-*/src/opentyr.h \
   $W/opentyrian-*/src/file.h $W/
# OpenTyrian's Makefile (release): -std=c99 -I./src -DTARGET_UNIX -g0 -O2 -DNDEBUG
OTF="-std=c99 -DTARGET_UNIX -g0 -O2 -DNDEBUG -I$W -I$D/stub"
$CC $OTF -c -o $W/opl.o $W/opl.c
$CC $OTF -c -o $W/lds_play.o $W/lds_play.c
$CC $OTF -D_GNU_SOURCE -c -o $W/oplbench.o $D/oplbench.c
$CC -o $D/oplbench $W/oplbench.o $W/opl.o $W/lds_play.o -lm
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include/SDL -o $D/sdltone1 $D/sdltone.c -lSDL -lpthread -lm
$CC -O2 -Wall --sysroot=$SYSROOT -I$SYSROOT/usr/include/SDL2 -o $D/sdltone2 $D/sdltone.c -lSDL2 -lpthread -lm
ls -l $D/oplbench $D/sdltone1 $D/sdltone2
