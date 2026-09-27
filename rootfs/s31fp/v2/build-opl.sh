#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/s31fp/v2/build-opl.sh'
# oplbench (rootfs/audiofp: OpenTyrian's own opl.c + lds_play.c, unmodified,
# its Makefile's flags) linked four ways - a TEST build of our benchmark, not
# the app:
#   oplbench-lg    libgcc's helpers (what the shipped binary has)
#   oplbench-old   the 2026-09-20 s31fp (mul/add/sub/floatsidf/floatunsidf/fixdfsi)
#   oplbench-v2    s31fp v2, all helpers, FENV=1 (bit-exact incl. fflags/frm)
#   oplbench-v2n   s31fp v2, FENV=0 (bit-exact results under RNE; no fflags/frm)
# The helpers replace libgcc's by --defsym, so opl.o is byte-identical in all
# four. *.q variants add plain memchr/strcmp/memcpy so QEMU can run them.
set -e
D=/src/rootfs/audiofp
V=/src/rootfs/s31fp/v2
CC=$(ls /src/build/buildroot/host/bin/*-linux-musl-gcc | head -1)
B=${B:-$V/out}
W=/tmp/v2-ot
rm -rf $W && mkdir -p $W $B
tar xzf /src/buildroot/dl/opentyrian/opentyrian-cf5dbeb69eebd9ef9afc4473088d9469b79589eb.tar.gz -C $W
(cd $W/opentyrian-* && patch -p1 -s < /src/buildroot/package/opentyrian/0001-Move-definitions-that-don-t-need-to-be-exposed-from-opl-h-to-opl-c.patch)
cp $W/opentyrian-*/src/opl.c $W/opentyrian-*/src/opl.h $W/opentyrian-*/src/lds_play.c \
   $W/opentyrian-*/src/lds_play.h $W/opentyrian-*/src/loudness.h $W/opentyrian-*/src/opentyr.h \
   $W/opentyrian-*/src/file.h $W/
OTF="-std=c99 -DTARGET_UNIX -g0 -O2 -DNDEBUG -I$W -I$D/stub"
$CC $OTF -c -o $W/opl.o $W/opl.c
$CC $OTF -c -o $W/lds_play.o $W/lds_play.c
$CC $OTF -D_GNU_SOURCE -c -o $W/oplbench.o $V/oplbench2.c
M="-mabi=ilp32 -march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs"
sed "s/\.Lm_/Lm_/g; s/\.La_/La_/g" $V/v2.S > $W/v2p.S  # labels kept as local symbols, for path profiles
$CC $M -O2 -DS31V2_FENV=1 -c -o $W/v2.o $W/v2p.S
$CC $M -O2 -DS31V2_FENV=1 -c -o $W/v2div.o $V/v2div.c
$CC $M -O2 -DS31V2_FENV=0 -c -o $W/v2n.o $V/v2.S
$CC $M -O2 -DS31V2_FENV=0 -c -o $W/v2ndiv.o $V/v2div.c
$CC $M -O2 -DS31V2_FENV=1 -DS31V2_FFLAGS=0 -c -o $W/v2f.o $V/v2.S
$CC $M -O2 -DS31V2_FENV=1 -DS31V2_FFLAGS=0 -c -o $W/v2fdiv.o $V/v2div.c
$CC $M -O2 -DS31FP_PRELOAD -DS31FP_ASM_MUL -DS31FP_ASM_ADD -c -o $W/oldc.o /src/rootfs/s31fp/s31fp.c
for f in muldf3 adddf3 conv; do $CC $M -c -o $W/old$f.o /src/rootfs/s31fp/$f.S; done
$CC $M -O2 -fno-builtin -c -o $W/noesp.o $V/noesp.c
cp $V/out/lgref.o $W/lgref.o 2>/dev/null || { echo "run build-v2.sh first"; exit 1; }
OLDSYM="-Wl,--defsym=__muldf3=s31fp_muldf3 -Wl,--defsym=__adddf3=s31fp_adddf3 -Wl,--defsym=__subdf3=s31fp_subdf3 -Wl,--defsym=__floatsidf=s31fp_floatsidf -Wl,--defsym=__floatunsidf=s31fp_floatunsidf -Wl,--defsym=__fixdfsi=s31fp_fixdfsi"
V2SYM=""
for s in muldf3 adddf3 subdf3 divdf3 gedf2 gtdf2 ledf2 ltdf2 eqdf2 nedf2 unorddf2 fixdfsi fixunsdfsi floatsidf floatunsidf extendsfdf2 truncdfsf2; do
	V2SYM="$V2SYM -Wl,--defsym=__$s=s31v2_$s"
done
OBJ="$W/oplbench.o $W/opl.o $W/lds_play.o"
for q in "" .q; do
	X=""; [ -n "$q" ] && X=$W/noesp.o
	$CC -static -o $B/oplbench-lg$q $OBJ $X -lm
	$CC -static -o $B/oplbench-old$q $OBJ $X $W/oldc.o $W/oldmuldf3.o $W/oldadddf3.o $W/oldconv.o $OLDSYM -lm
	$CC -static -o $B/oplbench-v2$q $OBJ $X $W/v2.o $W/v2div.o $W/lgref.o $V2SYM -lm
	$CC -static -o $B/oplbench-v2n$q $OBJ $X $W/v2n.o $W/v2ndiv.o $W/lgref.o $V2SYM -lm
	$CC -static -o $B/oplbench-v2f$q $OBJ $X $W/v2f.o $W/v2fdiv.o $W/lgref.o $V2SYM -lm
done
T=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
echo "esp.* instructions in opl.o: $($T-objdump -d $W/opl.o | grep -c '	esp\.')"
echo "operator_output bytes: $($T-nm -S $W/opl.o | grep operator_output)"
for v in lg old v2 v2n; do
	echo "$v: esp.* outside libc: $($T-objdump -d $B/oplbench-$v.q | awk '/^[0-9a-f]+ </{f=$2} /\tesp\./{print f}' | sort -u | tr '\n' ' ')"
	$T-nm $B/oplbench-$v | grep -E " __muldf3$| s31v2_muldf3$| s31fp_muldf3$| __gtdf2$" | tr '\n' ' '; echo
done
cp /src/build/buildroot/target/usr/share/opentyrian/data/music.mus $B/music.mus
ls -l $B/oplbench-*
# QEMU: static oplbench with libgcc's helpers AND preload3's constructor linked
# in - its own copies get patched at start exactly as the preload would
$CC $M -O2 -fno-builtin -c -o $W/p3s.o $V/preload3.c -I$V
$CC $M -O2 -c -o $W/p3sv2.o $V/v2.S
$CC $M -O2 -c -o $W/p3sdiv.o $V/v2div.c
$CC -static -o $B/oplbench-pre.q $OBJ $W/noesp.o $W/p3s.o $W/p3sv2.o $W/p3sdiv.o $W/lgref.o -lm
# dynamic, libgcc helpers - the shipped binary's shape; for the preload test
$CC -o $B/oplbench-dyn $OBJ -lm
ls -l $B/oplbench-dyn
