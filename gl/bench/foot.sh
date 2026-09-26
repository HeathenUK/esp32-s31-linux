#!/bin/bash
# foot.sh [OUT] - executed-code footprint of gears and texobj at 320x240
# (foot.py; review P4), from the images bench.sh left in OUT. Blocks run at
# least 13 times (once a frame over the 13 frames) count as hot. Reported,
# not gated. ~1 minute: qemu logs every block it runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
OUT=$(cd "$OUT" && pwd)
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
cd "$OUT"
for d in gears texobj; do
	(
	f=$(mktemp -u "$OUT/foot.$d.XXXX"); mkfifo "$f"
	python3 "$HERE/foot.py" q_${d}_320.elf obj "$TC/bin/riscv32-esp-elf-nm" 13 < "$f" > foot_$d.txt &
	P=$!
	qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
		-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
		-icount shift=0 -d in_asm,exec,nochain -D "$f" -kernel q_${d}_320.elf >/dev/null 2>&1 || true
	wait $P; rm -f "$f"
	) &
done
wait
for d in gears texobj; do echo "== $d 320x240"; cat foot_$d.txt; done | tee foot.txt
