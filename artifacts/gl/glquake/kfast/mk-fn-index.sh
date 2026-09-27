#!/bin/sh
# mk-fn-index.sh <out> : function -> object index of the built kernel tree
# (fastfn-pick.py's <fn-index>: "<fn> <obj> <size hex>"), run IN the build
# container from /src/build/linux. Built-in objects only (modules and the
# link intermediates excluded), text symbols with a size.
cd /src/build/linux || exit 1
NM=/home/builder/.espressif/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf/bin/riscv32-esp-elf-nm
[ -x "$NM" ] || NM=$(command -v riscv32-esp-linux-gnu-nm || command -v riscv32-linux-nm)
find . -name '*.o' ! -name '*.mod.o' ! -name 'vmlinux*.o' ! -name '.tmp*' ! -path './tools/*' ! -path './scripts/*' |
while read o; do
	[ -f "${o%.o}.ko" ] && continue
	# leaf objects only: a composite (foo-y) repeats its members' symbols
	[ -f "/src/linux-71-port/${o%.o}.c" ] || continue
	"$NM" -S --defined-only "$o" 2>/dev/null | awk -v o="${o#./}" 'NF==4 && $3 ~ /^[tT]$/ {print $4, o, $2}'
done > "$1"
wc -l "$1"
