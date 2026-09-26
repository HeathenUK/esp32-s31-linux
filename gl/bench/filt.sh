#!/bin/bash
# filt.sh [OUT] - phase 4 cases (filt.c FILTV 1-11: texture filters and
# perspective colour), from the objects gl/bench/build_q.sh left in OUT
# (run bench.sh first). 1-4 and 11 at 320x240 and 640x400, the rest at
# 320x240.
# Prints "filtN WxH: M insn/frame ..." to filt.txt. s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
B=$TC/bin; L=$TC/riscv32-esp-elf/lib; MULTI=rv32imac_zicsr_zifencei_zaamo_zalrsc/ilp32
BOARD="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base"
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
[ -n "$S31_BENCH_NOWRAP" ] && WR=
INC="-I$HERE/../include -I$HERE/../api -I$HERE/../tinygl/examples"
cd "$OUT"
LM="-lm"; [ -f libmuslm.a ] && LM="$PWD/libmuslm.a -lm"
rm -f t_*.elf
link() {
	$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
		-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
		"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" "$1" "$2" \
		demo/dwrap.o $(cat obj/core.list) $WR -Wl,--gc-sections $LM \
		-o "$3" 2>&1 | grep -v "RWX\|LOAD segment" || true
}
cc() { $B/riscv32-esp-elf-gcc $BOARD -O2 -w $INC "$@"; }
for s in "320 240" "640 400"; do
	set -- $s
	cc -DQW=$1 -DQH=$2 $S31_BENCH_QDEFS -c "$HERE/q_ui.c" -o demo/q_ui_t$1.o &
done
for v in 1 2 3 4 5 6 7 8 9 10 11; do cc -DFILTV=$v -c "$HERE/filt.c" -o demo/filt$v.o & done
wait
for v in 1 2 3 4 5 6 7 8 9 10 11; do link demo/filt$v.o demo/q_ui_t320.o t_filt${v}_320.elf & done
for v in 1 2 3 4 11; do link demo/filt$v.o demo/q_ui_t640.o t_filt${v}_640.elf & done
wait
ls t_*.elf | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -n 1 "$HERE/run_q.sh" 2>/dev/null > filt.all
grep Minsn filt.all | sort | tee filt.txt
grep Mframe filt.all | sort > filt-frames.txt || true
