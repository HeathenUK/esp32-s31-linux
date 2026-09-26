#!/bin/bash
# build_q.sh - build the qemu instruction-count bench images from OUR libGL
# sources (gl/tinygl + gl/api: the core of libGL.so.1, no GLX), compiled by
# gl/api/build-lib.sh itself with the board's Buildroot flags, so the objects
# measured are the ones the library is linked from (-fPIC, -O2 rasteriser,
# -Os ABI layer, --gc-sections at the link).
#
#   gl/bench/build_q.sh [GLDIR] [OUT]
#     GLDIR  the gl/ tree to measure (default: this repo's gl/); a copy of an
#            older tree works, which is how BASELINE.md was made
#     OUT    output directory (default gl/bench/out)
# Makes $OUT/q_<demo>_<W>.elf for demo in gears texobj teapotf and W in
# 320 (x240) and 640 (x400). Runs on the Mac with the ESP-IDF bare-metal
# toolchain (riscv32-esp-elf 15.2.0, the same GCC as the board's musl one);
# run them with gl/bench/run_q.sh or all at once with gl/bench/bench.sh.
#
# Demos: gl/tinygl/examples/gears.c and texobj.c unchanged, and teapotf.c,
# which is examples/teapot.c with its 1e-6 constants made float (the
# survey's variant: the stock one is 57% the demo's own soft double).
# Caveat: there is no rv32imafc/ilp32 newlib multilib, so libc/libm/libgcc
# come from rv32imac/ilp32 (soft-float libm). Our objects use the F
# instructions as on the board; only libm calls differ, and none is on a
# per-pixel path.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
GLDIR=$(cd "${1:-$HERE/..}" && pwd)
OUT=${2:-$HERE/out}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
B=$TC/bin
L=$TC/riscv32-esp-elf/lib
MULTI=rv32imac_zicsr_zifencei_zaamo_zalrsc/ilp32
# the board's TARGET_CFLAGS (Buildroot BR2_TARGET_OPTIMIZATION); build-lib.sh
# appends its own -O2/-Os after them, as s31-libgl.mk does
BOARD="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base"
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
OBJ=$OUT/obj
rm -f "$OUT"/q_*.elf

echo "--- library objects from $GLDIR (gl/api/build-lib.sh, S31GL_OBJONLY)"
# a tree whose own build-lib.sh predates S31GL_OBJONLY (the f522dad
# baseline) is compiled by this one with that tree's flags: no -Os list
if ! grep -q S31GL_OBJONLY "$GLDIR/api/build-lib.sh"; then
	export S31GL_TGLCOLD=" "
	echo "    (pre-F3 tree: every TinyGL file -O2, as its own build-lib.sh did)"
fi
GL=$GLDIR CC=$B/riscv32-esp-elf-gcc NM=$B/riscv32-esp-elf-nm ARCHFLAGS="$BOARD" \
	XINC= XLIBS= OBJ=$OBJ OUT=$OUT/unused.so S31GL_OBJONLY=1 S31GL_NO_GLX=1 \
	S31GL_JOBS=$NPROC sh "$HERE/../api/build-lib.sh" >"$OUT/build-lib.log" 2>&1 ||
	{ cat "$OUT/build-lib.log"; exit 1; }
grep -i "warning" "$OUT/build-lib.log" || true

EX=$GLDIR/tinygl/examples
DEMO=$OUT/demo
mkdir -p "$DEMO"
sed 's/1e-6 \*/1e-6f */g' "$EX/teapot.c" > "$DEMO/teapotf.c"
cp "$EX/teapot.h" "$DEMO/"
CF="$BOARD -O2 -w"
INC="-I$HERE/../include -I$HERE/../api -I$EX"
cc() { $B/riscv32-esp-elf-gcc $CF $INC "$@"; }

cc -c "$EX/gears.c" -o "$DEMO/gears.o" &
cc -c "$EX/texobj.c" -o "$DEMO/texobj.o" &
cc -I"$DEMO" -c "$DEMO/teapotf.c" -o "$DEMO/teapotf.o" &
cc -c "$HERE/dwrap.c" -o "$DEMO/dwrap.o" &
for s in "320 240" "640 400"; do
	set -- $s
	cc -DQW=$1 -DQH=$2 -c "$HERE/q_ui.c" -o "$DEMO/q_ui_$1.o" &
done
wait

# every soft-double libcall and double math function (wraps.txt, dwrap.c)
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
for d in gears texobj teapotf; do
	for w in 320 640; do
		$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
			-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
			"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" "$DEMO/$d.o" "$DEMO/q_ui_$w.o" \
			"$DEMO/dwrap.o" $(cat "$OBJ/core.list") $WR -Wl,--gc-sections -lm \
			-o "$OUT/q_${d}_$w.elf" 2>&1 | grep -v "RWX\|has a LOAD segment" || true &
	done
done
wait
ls "$OUT"/q_*.elf >/dev/null
# .text of the library objects as linked into the gears image (gc'd)
$B/riscv32-esp-elf-size -A "$OUT/q_gears_320.elf" | awk '$1==".text"{print "q_gears_320.elf .text", $2}'
