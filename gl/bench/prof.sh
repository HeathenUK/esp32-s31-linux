#!/bin/bash
# prof.sh OUT IMAGE... - flat per-function instruction profile of bench
# images (prof.py), e.g. prof.sh out q_gears_320 g_glxgears_300. Each
# image's profile goes to OUT/prof_IMAGE.txt. ~1 minute each (qemu logs
# every block it runs), all in parallel.
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$1" && pwd); shift
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
cd "$OUT"
for d in "$@"; do
	(
	f=$(mktemp -u "$OUT/prof.$d.XXXX"); mkfifo "$f"
	PROF_FN="$PROF_FN" python3 "$HERE/prof.py" $d.elf "$TC/bin/riscv32-esp-elf-nm" 10 < "$f" > prof_$d.txt &
	P=$!
	qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
		-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
		-icount shift=0 -d in_asm,exec,nochain -D "$f" -kernel $d.elf >/dev/null 2>&1 || true
	wait $P; rm -f "$f"
	) &
done
wait
for d in "$@"; do echo "== $d"; cat prof_$d.txt; done
