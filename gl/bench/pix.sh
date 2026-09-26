#!/bin/bash
# pix.sh [OUT] - instruction counts of the plan F7 pixel paths (pix.c,
# variants 1-6) at 320x240, from the objects gl/bench/build_q.sh left in OUT
# (run bench.sh first). Prints "pixN 320x240: M insn/frame ..."; every frame
# starts with a glClear, which variant 0 measures alone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
B=$TC/bin; L=$TC/riscv32-esp-elf/lib; MULTI=rv32imac_zicsr_zifencei_zaamo_zalrsc/ilp32
BOARD="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base"
# every soft-double libcall and double math function (wraps.txt, dwrap.c)
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
# S31_BENCH_NOWRAP=1 (review 3a m2): no soft-double counting wrappers (~8
# instructions a call inside the counted frames); dcalls then reads 0
[ -n "$S31_BENCH_NOWRAP" ] && WR=
cd "$OUT"
LM="-lm"; [ -f libmuslm.a ] && LM="$PWD/libmuslm.a -lm"
for v in 0 1 2 3 4 5 6 7 8; do
	(
	$B/riscv32-esp-elf-gcc $BOARD -O2 -w -I$HERE/../include -I$HERE/../api -I$HERE/../tinygl/examples \
		-DPIXV=$v -c "$HERE/pix.c" -o demo/pix$v.o
	$B/riscv32-esp-elf-gcc $BOARD -O2 -w -I$HERE/../include -I$HERE/../api -I$HERE/../tinygl/examples \
		-DQW=320 -DQH=240 $S31_BENCH_QDEFS -c "$HERE/q_ui.c" -o demo/q_ui_p$v.o
	$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
		-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
		"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" demo/pix$v.o demo/q_ui_p$v.o \
		demo/dwrap.o $(cat obj/core.list) $WR -Wl,--gc-sections $LM \
		-o p_pix_$v.elf 2>&1 | grep -v "RWX\|LOAD segment" || true
	) &
done
wait
ls p_pix_*.elf | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -n 1 "$HERE/run_q.sh" 2>/dev/null |
	grep Minsn | sort | tee pix.txt
