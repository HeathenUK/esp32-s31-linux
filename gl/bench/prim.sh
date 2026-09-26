#!/bin/bash
# prim.sh [OUT] - per-primitive costs (prim.c, variants 0-8; review P2, P3)
# at 320x240, from the objects gl/bench/build_q.sh left in OUT
# (run bench.sh first). Prints "primN 320x240: M insn/frame ..."; every frame
# starts with a glClear, which variant 0 measures alone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
B=$TC/bin; L=$TC/riscv32-esp-elf/lib; MULTI=rv32imac_zicsr_zifencei_zaamo_zalrsc/ilp32
BOARD="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base"
# every soft-double libcall and double math function (wraps.txt, dwrap.c)
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
cd "$OUT"
for v in 0 1 2 3 4 5 6 7 8; do
	(
	$B/riscv32-esp-elf-gcc $BOARD -O2 -w -I$HERE/../include -I$HERE/../api -I$HERE/../tinygl/examples \
		-DPRIMV=$v -c "$HERE/prim.c" -o demo/prim$v.o
	$B/riscv32-esp-elf-gcc $BOARD -O2 -w -I$HERE/../include -I$HERE/../api -I$HERE/../tinygl/examples \
		-DQW=320 -DQH=240 -c "$HERE/q_ui.c" -o demo/q_ui_r$v.o
	$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
		-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
		"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" demo/prim$v.o demo/q_ui_r$v.o \
		demo/dwrap.o $(cat obj/core.list) $WR -Wl,--gc-sections -lm \
		-o p_prim_$v.elf 2>&1 | grep -v "RWX\|LOAD segment" || true
	) &
done
wait
ls p_prim_*.elf | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -n 1 "$HERE/run_q.sh" 2>/dev/null |
	grep Minsn | sort | tee prim.txt
