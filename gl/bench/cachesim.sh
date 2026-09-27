#!/bin/bash
# cachesim.sh OUT [LABEL] - PSRAM line traffic of the QuakeSpasm proxy's
# counted frames through a model of the S31 D-cache (qemu/cachesim.c:
# 64 kB, 64 B lines, 2-way, write-back, write-allocate assumed): runs
# OUT/qsr/qsr.elf (qsreplay.sh's image, QSR_ENV as there) and prints
# refills and write-backs per counted frame. CS_KB / CS_WAYS change the
# geometry. CS_ARGS adds plugin arguments (tier 4: pages=1,seed=S,col=A:N -
# the board's physical page placement, see cachesim.c). The run's frame
# dumps are deleted (a sweep would otherwise keep 12 MB a point).
# Phase 6 tier 3. s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$1" && pwd)
LABEL=${2:-lib}
Q=$OUT/qsr
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
NM=$TC/bin/riscv32-esp-elf-nm
PL=$HERE/qemu/cachesim.dylib
if [ ! -f "$PL" ] || [ "$HERE/qemu/cachesim.c" -nt "$PL" ]; then
	cc -O2 -shared -fPIC -undefined dynamic_lookup $(pkg-config --cflags glib-2.0) \
		-I"$(brew --prefix qemu)/include" "$HERE/qemu/cachesim.c" -o "$PL"
fi
a() { $NM "$Q/qsr.elf" | awk -v s="$1" '$3==s{print "0x"$1}'; }
ON=$(a q_on); OFF=$(a q_off)
TRACE=${QSP_TRACE:-$(cat "$Q/trace.path")}
D=$Q/cs-run-$LABEL
rm -rf "$D"; mkdir -p "$D"
ln -s "$TRACE" "$D/qs.gltr"
for kv in $QSR_ENV; do echo "$kv"; done > "$D/qr.env"
( cd "$D" && qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
	-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
	-plugin "$PL,on=$ON,off=$OFF,kb=${CS_KB:-64},ways=${CS_WAYS:-2}${CS_ARGS:+,$CS_ARGS}" -d plugin -D cs.log -kernel "$Q/qsr.elf" > out.txt 2>&1 || true )
rm -f "$D"/qsr_f*.raw
N=$(grep -c "^qsrf .* count" "$D/out.txt")
awk -v n="$N" -v l="$LABEL" '/^cachesim sets/{printf "cachesim %s: %d counted frames, per frame: loads %.0f stores %.0f refills %.0f (%.1f kB) writebacks %.0f (%.1f kB), PSRAM %.1f kB\n", l, n, $7/n, $9/n, $11/n, $11*64/1024/n, $13/n, $13*64/1024/n, ($11+$13)*64/1024/n}' "$D/cs.log" | tee "$Q/cs-$LABEL.txt"
