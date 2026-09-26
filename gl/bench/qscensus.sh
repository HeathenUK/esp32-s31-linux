#!/bin/bash
# qscensus.sh GLDIR OUT TRACE - which general-path stage lists draw the
# QuakeSpasm proxy's counted frames: builds GLDIR's library objects with
# -DS31GL_CENSUS (gl/tinygl/source/s31_census.c; a diagnostic build, its
# instruction counts are not the library's), replays TRACE (QSR_ENV as
# qsreplay.sh) and prints per stage list the chunks, pixels and pixels alive
# after the depth test, with the stages' names, largest first.
# Writes OUT/qsr/census.txt. s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
GLDIR=${1:?gldir}; OUT=${2:?out}; TRACE=${3:?trace}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
S31_BENCH_DEFS="-DS31GL_CENSUS $S31_BENCH_DEFS" QSR_NULL=0 QSR_LABEL=census \
	"$HERE/qsreplay.sh" "$GLDIR" "$OUT" "$TRACE" > "$OUT.census.log" 2>&1 || { cat "$OUT.census.log"; exit 1; }
OUT=$(cd "$OUT" && pwd)
python3 "$HERE/census.py" "$TC/bin/riscv32-esp-elf-nm" "$OUT/qsr/qsr.elf" "$OUT/qsr/run-census-lib/out.txt" | tee "$OUT/qsr/census.txt"
