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
rm -f "$OUT"/q_*.elf "$OUT"/libmuslm.a

# S31_BENCH_LIBM=musl (phase 3a): link the board's libm - the math objects
# of the musl toolchain's libc.a (sin, cos, sqrt, pow, powf, expf, ...;
# compiled rv32imafc/ilp32: F instructions, soft double) - ahead of
# newlib's soft-float rv32imac libm, so a libm call costs what it costs on
# the board, and a float function that computes in double internally (musl
# sinf, cosf, powf, expf) shows up in the soft-double count. The other
# scripts (feat, pix, prim, geo) link $OUT/libmuslm.a when it exists.
if [ "$S31_BENCH_LIBM" = musl ]; then
	MUSLC=${S31_BENCH_MUSLC:-$HERE/../../toolchain/riscv32-esp-linux-musl/riscv32-esp-linux-musl/sysroot/usr/lib/libc.a}
	MM="__cos.o __cosdf.o __math_divzero.o __math_divzerof.o __math_invalid.o
		__math_invalidf.o __math_oflow.o __math_oflowf.o __math_uflow.o
		__math_uflowf.o __math_xflow.o __math_xflowf.o __rem_pio2.o
		__rem_pio2_large.o __rem_pio2f.o __sin.o __sindf.o ceilf.o cos.o cosf.o
		exp.o exp2f_data.o exp_data.o expf.o floor.o floorf.o fmodf.o log2f.o
		log2f_data.o logf.o logf_data.o pow.o pow_data.o powf.o powf_data.o
		fabs.o fabsf.o sqrt.o sqrt_data.o sqrtf.o sin.o sincos.o sincosf.o
		sinf.o scalbn.o"
	rm -rf "$OUT/muslm"; mkdir -p "$OUT/muslm"
	( cd "$OUT/muslm" && $B/riscv32-esp-elf-ar x "$MUSLC" $MM &&
	  $B/riscv32-esp-elf-ar rcs "$OUT/libmuslm.a" $MM )
	echo "--- board libm: $(echo $MM | wc -w) musl math objects from $MUSLC"
fi
LM="-lm"
[ -f "$OUT/libmuslm.a" ] && LM="$OUT/libmuslm.a -lm"

echo "--- library objects from $GLDIR (its own gl/api/build-lib.sh, S31GL_OBJONLY)"
# review 3a m3: each tree is compiled by ITS OWN build-lib.sh (its per-file
# flags - G14b's -fomit-frame-pointer for gl_attrib.c, the -Os list - are
# part of what is measured), as brsize.sh does. A tree whose build-lib.sh
# predates S31GL_OBJONLY (the f522dad baseline) is compiled by this one
# with that tree's flags: no -Os list
LIB=$GLDIR/api/build-lib.sh
if ! grep -q S31GL_OBJONLY "$LIB"; then
	LIB=$HERE/../api/build-lib.sh
	export S31GL_TGLCOLD=" "
	echo "    (pre-F3 tree: every TinyGL file -O2, as its own build-lib.sh did)"
fi
# S31_BENCH_DEFS (phase 3a pixel levers): extra -D flags for the library
# objects only, e.g. -DS31GL_ZTRICK_DEFAULT=1 to measure a lever off
GL=$GLDIR CC=$B/riscv32-esp-elf-gcc NM=$B/riscv32-esp-elf-nm ARCHFLAGS="$BOARD $S31_BENCH_DEFS" \
	XINC= XLIBS= OBJ=$OBJ OUT=$OUT/unused.so S31GL_OBJONLY=1 S31GL_NO_GLX=1 \
	S31GL_JOBS=$NPROC sh "$LIB" >"$OUT/build-lib.log" 2>&1 ||
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
cc ${S31_BENCH_NOWRAP:+-DDWRAP_NONE} -c "$HERE/dwrap.c" -o "$DEMO/dwrap.o" &
for s in "320 240" "640 400"; do
	set -- $s
	cc -DQW=$1 -DQH=$2 $S31_BENCH_QDEFS -c "$HERE/q_ui.c" -o "$DEMO/q_ui_$1.o" &
done
wait

# every soft-double libcall and double math function (wraps.txt, dwrap.c)
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
# S31_BENCH_NOWRAP=1 (review 3a m2): no soft-double counting wrappers (~8
# instructions a call inside the counted frames); dcalls then reads 0
[ -n "$S31_BENCH_NOWRAP" ] && WR=
for d in gears texobj teapotf; do
	for w in 320 640; do
		$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
			-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
			"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" "$DEMO/$d.o" "$DEMO/q_ui_$w.o" \
			"$DEMO/dwrap.o" $(cat "$OBJ/core.list") $WR -Wl,--gc-sections $LM \
			-o "$OUT/q_${d}_$w.elf" 2>&1 | grep -v "RWX\|has a LOAD segment" || true &
	done
done
wait
ls "$OUT"/q_*.elf >/dev/null
# .text of the library objects as linked into the gears image (gc'd)
$B/riscv32-esp-elf-size -A "$OUT/q_gears_320.elf" | awk '$1==".text"{print "q_gears_320.elf .text", $2}'
