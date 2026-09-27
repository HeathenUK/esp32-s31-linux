#!/bin/sh
# ./docker/build.sh 'cd /src && sh rootfs/s31fp/v2/build-v2.sh'
# Host/QEMU test build of s31fp v2 (nothing here is installed anywhere).
#   lgref.o    libgcc's OWN double routines, renamed __x -> s31lg_x: the
#              fallback for every non-fast case and the test reference
#   v2test     bit-exactness + bench harness (QEMU: needs the D oracle)
set -e
T=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl
CC=$T-gcc
D=/src/rootfs/s31fp/v2
O=/src/rootfs/s31fp
B=${B:-/tmp/s31v2}
mkdir -p $B && cd $B
# no xesp* vendor extensions: QEMU cannot execute them, and v2 uses none
MARCH=${MARCH:--march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs}
LG=$($CC -print-libgcc-file-name)
M="adddf3.o subdf3.o muldf3.o divdf3.o eqdf2.o gedf2.o ledf2.o unorddf2.o fixdfsi.o fixunsdfsi.o floatsidf.o floatunsidf.o extendsfdf2.o truncdfsf2.o"
rm -rf lg && mkdir lg && (cd lg && $T-ar x $LG $M)
R=""
for s in adddf3 subdf3 muldf3 divdf3 eqdf2 nedf2 gedf2 gtdf2 ledf2 ltdf2 unorddf2 fixdfsi fixunsdfsi floatsidf floatunsidf extendsfdf2 truncdfsf2; do
	R="$R --redefine-sym __$s=s31lg_$s"
done
$T-ld -r -o lgref.o $(for m in $M; do echo lg/$m; done)
$T-objcopy $R lgref.o
$T-nm lgref.o | grep -c " T s31lg_"
FENV=${FENV:-1}; FFL=${FFL:-$FENV}
$CC $MARCH -mabi=ilp32 -O2 -DS31V2_FENV=$FENV -DS31V2_FFLAGS=$FFL -c -o v2.o $D/v2.S
$CC $MARCH -mabi=ilp32 -O2 -DS31V2_FENV=$FENV -DS31V2_FFLAGS=$FFL -c -o v2div.o $D/v2div.c
# old s31fp (2026-09-20) for instruction-count comparison
$CC $MARCH -mabi=ilp32 -O2 -DS31FP_PRELOAD -DS31FP_ASM_MUL -DS31FP_ASM_ADD -c -o oldc.o $O/s31fp.c
$CC $MARCH -mabi=ilp32 -c -o oldmul.o $O/muldf3.S
$CC $MARCH -mabi=ilp32 -c -o oldadd.o $O/adddf3.S
$CC $MARCH -mabi=ilp32 -c -o oldconv.o $O/conv.S
$CC -march=rv32imafdc_zicsr_zifencei_zba_zbb_zbs -mabi=ilp32 -O2 -c -o hw.o $D/hw.c
$CC $MARCH -mabi=ilp32 -O2 -Wall -c -o v2test.o $D/v2test.c
$CC $MARCH -mabi=ilp32 -O2 -fno-builtin -c -o noesp.o $D/noesp.c
OBJS="v2test.o noesp.o v2.o v2div.o lgref.o hw.o oldc.o oldmul.o oldadd.o oldconv.o"
$CC $MARCH -mabi=ilp32 -O2 -c -o wrap.o $D/wrap.c
WR=$(for s in muldf3 adddf3 subdf3 divdf3 gedf2 ledf2 eqdf2 unorddf2 fixdfsi fixunsdfsi extendsfdf2 truncdfsf2 floatsidf floatunsidf; do printf -- "-Wl,--wrap=s31lg_$s "; done)
# v2check: fall-backs counted through --wrap; v2test: clean, for instruction counts
$CC $MARCH -mabi=ilp32 -static -o v2check$FENV $OBJS wrap.o $WR
printf 'volatile unsigned long v2_refcalls;\n' > nowrap.c && $CC $MARCH -mabi=ilp32 -c -o nowrap.o nowrap.c
$CC $MARCH -mabi=ilp32 -static -o v2test$FENV $OBJS nowrap.o
ls -l $B/v2test$FENV $B/v2check$FENV
# board build: no D oracle (the board has no D), libgcc copies are the reference
if [ "$FENV" = 1 ]; then
	$CC $MARCH -mabi=ilp32 -O2 -Wall -DNO_HW -c -o v2testb.o $D/v2test.c
	cat > hwstub.c <<'HW'
#include <stdint.h>
#define S(n) uint64_t hw_##n() { return 0; }
S(mul) S(add) S(sub) S(div) S(gt) S(ge) S(lt) S(le) S(eq) S(unord) S(floatsidf) S(floatunsidf) S(fixdfsi) S(fixunsdfsi) S(extendsfdf2) S(truncdfsf2)
HW
	$CC $MARCH -mabi=ilp32 -O2 -c -o hwstub.o hwstub.c
	BO="v2testb.o v2.o v2div.o lgref.o hwstub.o oldc.o oldmul.o oldadd.o oldconv.o"
	$CC $MARCH -mabi=ilp32 -static -o v2check-board $BO wrap.o $WR
	$CC $MARCH -mabi=ilp32 -static -o v2bench-board $BO nowrap.o
	ls -l $B/v2check-board $B/v2bench-board
fi
