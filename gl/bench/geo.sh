#!/bin/bash
# geo.sh [OUT] - phase 3a geometry-side cases, from the objects
# gl/bench/build_q.sh left in OUT (run bench.sh first):
#   glxgears 300x300  glxgears_q.c: stock glxgears' gears, lists, 1 light
#   glxgears-2buf 300x300  the same into two colour buffers bound in turn
#                     (phase 3a pixel levers: GLX's two SHM segments)
#   geo1 320x240, 640x400  clear-heavy (geo.c GEOV=1)
#   geo3 320x240  indexed lit mesh, glDrawElements
#   geo4 320x240  512 glRotatef (256 arbitrary axes, 256 about z)
#   geo5 320x240  200 x (rotate, one lit triangle): inverse per glBegin
#   geo6 320x240  immediate lit mesh, specular, two lights incl. a spot
#   geo7 320x240  immediate lit mesh, glColor per vertex, GL_COLOR_MATERIAL
#   geo8 320x240  strip/loop/fan/quad-strip assembly probe (flat + smooth)
#   geo9 320x240, 640x400  full-coverage clear, far (review 3a m5)
#   geo10 320x240, 640x400 full-coverage clear, near
#   geo11 320x240 approaching camera, 60 frames (review 3a M4: demotions)
#   geo12 320x240 far-plane background, 60 frames (materialisations)
# Prints "name WxH: M insn/frame ..." to geo.txt and the soft-double
# breakdown to geo-dcalls.txt.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
B=$TC/bin; L=$TC/riscv32-esp-elf/lib; MULTI=rv32imac_zicsr_zifencei_zaamo_zalrsc/ilp32
BOARD="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base"
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
# S31_BENCH_NOWRAP=1 (review 3a m2): no soft-double counting wrappers (~8
# instructions a call inside the counted frames); dcalls then reads 0
[ -n "$S31_BENCH_NOWRAP" ] && WR=
INC="-I$HERE/../include -I$HERE/../api -I$HERE/../tinygl/examples"
cd "$OUT"
LM="-lm"; [ -f libmuslm.a ] && LM="$PWD/libmuslm.a -lm"
rm -f g_*.elf
link() { # demo.o ui.o out.elf
	$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
		-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
		"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" "$1" "$2" \
		demo/dwrap.o $(cat obj/core.list) $WR -Wl,--gc-sections $LM \
		-o "$3" 2>&1 | grep -v "RWX\|LOAD segment" || true
}
cc() { $B/riscv32-esp-elf-gcc $BOARD -O2 -w $INC "$@"; }
for s in "300 300" "320 240" "640 400"; do
	set -- $s
	cc -DQW=$1 -DQH=$2 $S31_BENCH_QDEFS -c "$HERE/q_ui.c" -o demo/q_ui_g$1.o &
done
cc -c "$HERE/glxgears_q.c" -o demo/glxgears_q.o &
# phase 3a: glxgears as GLX now runs a 300x300 window, two buffers in turn
cc -DQW=300 -DQH=300 -DQ_BUFS=2 -DQ_NAME_SUFFIX='"-2buf"' $S31_BENCH_QDEFS -c "$HERE/q_ui.c" -o demo/q_ui_g300b.o &
for v in 1 3 4 5 6 7 8 9 10 11 12; do cc -DGEOV=$v -c "$HERE/geo.c" -o demo/geo$v.o & done
# the tail cases count 60 frames (the approach swings over 20)
cc -DQW=320 -DQH=240 $S31_BENCH_QDEFS -DQN=60 -c "$HERE/q_ui.c" -o demo/q_ui_g320_60.o &
wait
link demo/glxgears_q.o demo/q_ui_g300.o g_glxgears_300.elf &
link demo/glxgears_q.o demo/q_ui_g300b.o g_glxgears2_300.elf &
link demo/geo1.o demo/q_ui_g640.o g_geo1_640.elf &
link demo/geo9.o demo/q_ui_g640.o g_geo9_640.elf &
link demo/geo10.o demo/q_ui_g640.o g_geo10_640.elf &
for v in 1 3 4 5 6 7 8 9 10; do link demo/geo$v.o demo/q_ui_g320.o g_geo${v}_320.elf & done
for v in 11 12; do link demo/geo$v.o demo/q_ui_g320_60.o g_geo${v}_320.elf & done
wait
ls g_*.elf | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -n 1 "$HERE/run_q.sh" 2>/dev/null > geo.all
grep Minsn geo.all | sort | tee geo.txt
grep Mframe geo.all | sort > geo-frames.txt || true
grep "dcalls/frame" geo.all | sort > geo-dcalls.txt || true
