#!/bin/bash
# foot.sh [OUT] - executed-code footprint of gears and texobj at 320x240
# (foot.py; review P4), from the images bench.sh left in OUT. Blocks run at
# least 10 times in the 10 counted frames (phase 3a: only between q_ui.c's
# q_mark() calls) count as hot. Reported, not gated. ~1 minute: qemu logs
# every block it runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
OUT=$(cd "$OUT" && pwd)
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
cd "$OUT"
# FOOT_IMAGES (phase 3a): the images to measure, default gears and texobj
# at 320x240 as before; e.g. "q_gears_320 g_glxgears_300 q_teapot_320"
IMGS=${FOOT_IMAGES:-q_gears_320 q_texobj_320}
for d in $IMGS; do
	(
	f=$(mktemp -u "$OUT/foot.$d.XXXX"); mkfifo "$f"
	python3 "$HERE/foot.py" $d.elf obj "$TC/bin/riscv32-esp-elf-nm" 10 < "$f" > foot_$d.txt &
	P=$!
	qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
		-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
		-icount shift=0 -d in_asm,exec,nochain -D "$f" -kernel $d.elf >/dev/null 2>&1 || true
	wait $P; rm -f "$f"
	) &
done
wait
for d in $IMGS; do echo "== $d"; cat foot_$d.txt; done | tee foot.txt
