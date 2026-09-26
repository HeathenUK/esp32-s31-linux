#!/bin/sh
# run-3a.sh LABEL - every correctness check phase 3a holds a lever to, from
# the Mac (host only: nothing touches the board). Output in
# artifacts/gl/phase3a/LABEL/:
#   host.log      gl/host-build.sh (host libGL + tests), core_test, raster_gate,
#                 filt_test (phase 4), fmath_test and f2d_test (host)
#   suite/        tools/glref/suite.sh (22 apps), and suitecmp.txt against
#                 artifacts/gl/phase3a/suite-base (= phase2 SUITE.md 10.2)
#   pixels/       gl/tests/run-pixels.sh (glx_pixels p1-p4, GLU g1-g3, b1-b2)
#   raster/       gl/tests/run-raster.sh (glx_raster pages 1-6)
#   prims.log     gl/tests/run-prims.sh
#   mtex/         gl/tests/run-mtex.sh (phase 5: glx_mtex pages 1-5 vs Mesa)
#   prec/         gl/tests/run-prec.sh (phase 5 P: glx_prec vs Mesa, >= 99.3%
#                 of the values exact)
#   qsr-fused.log gl/tests/run-qsr-fused.sh (phase 5 O2: every QuakeSpasm
#                 trace, fused fillers vs S31GL_FUSED=0, frame hashes)
#   bench.log     gl/bench/bench.sh against limits.txt (phase 5 review P2)
#   rv32.log      gl/build.sh (RV32, library to gl/out-rv32/, NOT /src/images),
#                 then core_test, raster_gate, d2f_test and headless_gears under
#                 qemu-user (s31-glref-qemu)
# Prints one summary line per check. s31, MIT.
set -u
L=${1:?label}
R=$(cd "$(dirname "$0")/../.." && pwd)
A=$R/artifacts/gl/phase3a/$L
mkdir -p "$A"
cd "$R"
F=/private/tmp/s31-3a-$$
mkdir -p $F
{
	gl/host-build.sh
	echo "--- core_test"; docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'cd /src/gl/out-host && ./core_test 2>&1 | tail -3'
	echo "--- raster_gate"; docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'cd /src/gl/out-host && ./raster_gate 2>&1 | tail -3'
	echo "--- filt_test (phase 4)"; docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'cd /src/gl/out-host && ./filt_test 2>&1 | grep -v "^PASS\|^libGL"'
	echo "--- fused_test (phase 5: fused = general, every pixel)"; docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'cd /src/gl/out-host && ./fused_test 2>&1 | grep -v "^libGL"'
	echo "--- fused_test, S31GL_FILT8=1 (phase 5: the 8-bit filters on RGB565 textures)"; docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'cd /src/gl/out-host && S31GL_FILT8=1 ./fused_test 2>&1 | grep -v "^libGL" | tail -1'
	echo "--- headless_gears md5"; docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'cd /tmp && /src/gl/out-host/headless_gears 320 240 100 /tmp/hg.ppm >/dev/null 2>&1; md5sum /tmp/hg.ppm'
	echo "--- fmath_test, f2d_test (host arm64 in the rig)"
	docker run --rm -v "$R":/src -w /src s31-glref:latest sh -c 'gcc -O2 -ffp-contract=off -I/src/gl/tinygl/source /src/gl/tests/fmath_test.c /src/gl/tinygl/source/s31_fmath.c -lm -o /tmp/fm && /tmp/fm; gcc -O2 -I/src/gl/tinygl/source /src/gl/tests/f2d_test.c -o /tmp/f2d && /tmp/f2d'
} > "$A/host.log" 2>&1
grep -E "passed|failed|PASS|FAIL|md5|mismatch|error" "$A/host.log" | grep -v "^gcc" | head -20
tools/glref/suite.sh --run phase3a/$L/suite --jobs 10 > "$A/suite.log" 2>&1
python3 gl/bench/suitecmp.py artifacts/gl/phase3a/suite-base/report.json "$A/suite/report.json" > "$A/suitecmp.txt"
tail -1 "$A/suitecmp.txt"; grep -E "WORSE" "$A/suitecmp.txt"
gl/tests/run-pixels.sh /src/artifacts/gl/phase3a/$L/pixels > "$A/pixels.log" 2>&1
# against the post-review-fix runs of the base tree (phase2 SUITE.md 10.3)
sed 's/diff .*//' "$A/pixels/summary.txt" > $F/p.now; sed 's/diff .*//' artifacts/gl/f7/pixels/summary.txt > $F/p.base
cmp -s $F/p.now $F/p.base && echo "pixels: identical to artifacts/gl/f7/pixels" || { echo "pixels: DIFFER"; diff $F/p.base $F/p.now; }
grep -h "differ" "$A"/pixels/logdiff-*.txt 2>/dev/null | sort | uniq -c | head -5
gl/tests/run-raster.sh /src/artifacts/gl/phase3a/$L/raster > "$A/raster.log" 2>&1
sed 's/diff .*//' "$A/raster/summary.txt" > $F/r.now; sed 's/diff .*//' artifacts/gl/f3f6/raster/summary.txt > $F/r.base
cmp -s $F/r.now $F/r.base && echo "raster: identical to artifacts/gl/f3f6/raster" || { echo "raster: DIFFER"; diff $F/r.base $F/r.now; }
docker run --rm -v "$R":/src -w /src s31-glref:latest sh /src/gl/tests/run-prims.sh > "$A/prims.log" 2>&1
echo "prims: $(tail -2 "$A/prims.log" | tr '\n' ' ')"
# phase 5 O1: two texture units and the combiner against Mesa
gl/tests/run-mtex.sh /src/artifacts/gl/phase3a/$L/mtex > "$A/mtex.log" 2>&1
echo "mtex: $(grep -c ' PASS ' "$A/mtex/summary.txt") of 5 pages PASS, $(grep -c 'log: .* 0 differ' "$A/mtex/summary.txt") logs 0 differ"
# phase 5 P: the precision probe against Mesa (glx_prec, 48 bands); the
# phase-5 library holds 99.35% of its values exact (phase 4: 72.94%)
gl/tests/run-prec.sh /src/artifacts/gl/phase3a/$L/prec > "$A/prec.log" 2>&1
PX=$(sed -n 's/^all: \([0-9.]*\)% exact/\1/p' "$A/prec/score.txt" 2>/dev/null)
echo "prec: ${PX:-?}% exact vs Mesa ($(awk -v p="${PX:-0}" 'BEGIN{print (p >= 99.3) ? "PASS, >= 99.3" : "FAIL, < 99.3"}'))"
# phase 5 O2: fused = general over every full frame of the QuakeSpasm traces
# (RV32, instruction counter; the traces are gitignored - skipped if absent)
if ls gl/bench/qstrace/work/*/qs.gltr >/dev/null 2>&1; then
	gl/tests/run-qsr-fused.sh "$R/gl" "$R/gl/bench/out-qsr-fused-$L" > "$A/qsr-fused.log" 2>&1
	echo "qsr-fused: exit $?, $(grep -c 'full frames identical' "$A/qsr-fused.log") traces, $(grep 'full frames identical' "$A/qsr-fused.log" | awk '{s+=$3; n+=$5} END{print s " of " n}') frames fused = general, $(grep 'full frames identical' "$A/qsr-fused.log" | awk '{s+=$12; n+=$14} END{print s " of " n}') fused = the unpacked reference"
else
	echo "qsr-fused: no traces (tools/glref/gltrace/capture-qs.sh), skipped"
fi
{
	S31GL_IMAGE=/src/gl/out-rv32/libGL.so.1 gl/build.sh
	for t in core_test raster_gate d2f_test filt_test fused_test; do
		echo "--- $t (RV32 qemu-user)"
		docker run --rm -v "$R":/src s31-glref-qemu qemu-riscv32 -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true /src/gl/out-rv32/$t.qemu 2>&1 | tail -3
	done
	echo "--- headless_gears (RV32 qemu-user)"
	docker run --rm -v "$R":/src -w /tmp s31-glref-qemu sh -c 'qemu-riscv32 -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true /src/gl/out-rv32/headless_gears.qemu 320 240 100 /tmp/hg.ppm >/dev/null 2>&1; md5sum /tmp/hg.ppm'
} > "$A/rv32.log" 2>&1
grep -E "passed|failed|PASS|FAIL|mismatch|tested|error|/tmp/" "$A/rv32.log" | head -20
rm -rf $F
# phase 5 review P2: gl/bench instruction counts against limits.txt (the
# default build; reported, not failing - artifacts/gl/phase5/REPORT.txt
# lists the lines still over, pending the owner's sign-off)
gl/bench/bench.sh "$R/gl" "$R/gl/bench/out-3a-$L" > "$A/bench.log" 2>&1
echo "bench: exit $?, $(grep '^bench: .* within' "$A/bench.log")"
