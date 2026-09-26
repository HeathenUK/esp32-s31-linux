#!/bin/bash
# qsprof.sh OUT [LABEL] - per-function instruction profile of the QuakeSpasm
# proxy's counted frames: runs OUT/qsr/qsr.elf (built by qsreplay.sh; same
# trace, same QSR_ENV) under the pcprof plugin (gl/bench/qemu/pcprof.c) and
# maps the blocks to functions and library objects (pcprof.py). The
# instruction total equals qsreplay.sh's counted-frame sum minus the swap
# handling between frames (q_off .. q_on). ~1-2 minutes for the full trace.
# The trace is the one qsreplay.sh last used (OUT/qsr/trace.path; QSP_TRACE
# overrides). Writes OUT/qsr/prof-LABEL.txt. s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$1" && pwd)
LABEL=${2:-lib}
Q=$OUT/qsr
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
NM=$TC/bin/riscv32-esp-elf-nm
PL=$HERE/qemu/pcprof.dylib
if [ ! -f "$PL" ] || [ "$HERE/qemu/pcprof.c" -nt "$PL" ]; then
	cc -O2 -shared -fPIC -undefined dynamic_lookup $(pkg-config --cflags glib-2.0) \
		-I"$(brew --prefix qemu)/include" "$HERE/qemu/pcprof.c" -o "$PL"
fi
a() { $NM "$Q/qsr.elf" | awk -v s="$1" '$3==s{print "0x"$1}'; }
ON=$(a q_on); OFF=$(a q_off)
TRACE=${QSP_TRACE:-$(cat "$Q/trace.path")}
D=$Q/prof-run-$LABEL
rm -rf "$D"; mkdir -p "$D"
ln -s "$TRACE" "$D/qs.gltr"
for kv in $QSR_ENV; do echo "$kv"; done > "$D/qr.env"
( cd "$D" && qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
	-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
	-plugin "$PL,on=$ON,off=$OFF" -d plugin -D pcprof.log -kernel "$Q/qsr.elf" > out.txt 2>&1 || true )
N=$(grep -c "^qsrf .* count" "$D/out.txt")
python3 "$HERE/pcprof.py" "$Q/qsr.elf" "$NM" "$N" "$D/pcprof.log" "$OUT/obj/core.list" | tee "$Q/prof-$LABEL.txt"
