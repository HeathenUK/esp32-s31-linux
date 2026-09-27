#!/bin/bash
# icsim.sh OUT LABEL [plugin args] - the QuakeSpasm proxy's counted frames
# through the I-cache model (qemu/icsim.c: hart 1's measured geometry, 32 kB
# 2-way 64 B lines, physically indexed). Runs OUT/qsr/qsr.elf (qsreplay.sh's
# image, QSR_ENV as there); prints per counted frame: instructions, distinct
# lines executed, modelled refills. Extra args go to the plugin (lo=,hi=,
# pages=1,seed=S, dump=FILE, kb=, ways=). Phase 6 tier 6. s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$1" && pwd); LABEL=${2:-ic}; shift 2
Q=$OUT/qsr
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
PL=$HERE/qemu/icsim.dylib
if [ ! -f "$PL" ] || [ "$HERE/qemu/icsim.c" -nt "$PL" ]; then
	cc -O2 -shared -fPIC -undefined dynamic_lookup $(pkg-config --cflags glib-2.0) \
		-I"$(brew --prefix qemu)/include" "$HERE/qemu/icsim.c" -o "$PL"
fi
a() { $TC/bin/riscv32-esp-elf-nm "$Q/qsr.elf" | awk -v s="$1" '$3==s{print "0x"$1}'; }
ARGS="on=$(a q_on),off=$(a q_off)"; for x in "$@"; do ARGS="$ARGS,$x"; done
D=$Q/ic-run-$LABEL; rm -rf "$D"; mkdir -p "$D"
ln -s "$(cat "$Q/trace.path")" "$D/qs.gltr"
for kv in $QSR_ENV; do echo "$kv"; done > "$D/qr.env"
( cd "$D" && qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
	-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
	-plugin "$PL,$ARGS" -d plugin -D ic.log -kernel "$Q/qsr.elf" > out.txt 2>&1 || true )
rm -f "$D"/qsr_f*.raw
awk -v l="$LABEL" '/^icsim/{n=$9; printf "icsim %s: %d frames, per frame: %.3f M insns, %.0f distinct lines (%.1f kB), %.0f refills (%.1f kB)\n", l, n, $11/n/1e6, $17/n, $17/n*64/1024, $15/n, $15/n*64/1024}' "$D/ic.log"
