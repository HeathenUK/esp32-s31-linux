#!/bin/bash
# run-threads-rv32.sh [OUT] - phase 6 (S31GL_THREADS): the RV32 library's
# tests (gl/build.sh's *.qemu images, musl, board flags) under qemu-user,
# whose guest threads are host threads - so mode 1 runs the real pthread
# worker, futexes and RV32 atomics concurrently. Every mode's results must
# equal mode 0's. Run on the Mac after S31GL_IMAGE=/src/gl/out-rv32/libGL.so.1
# gl/build.sh:  gl/tests/run-threads-rv32.sh [OUT]
# s31, MIT.
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$R/artifacts/gl/phase6}
mkdir -p "$OUT"
docker run --rm -v "$R":/src -w /tmp s31-glref-qemu sh -c '
Q="qemu-riscv32 -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true"
for m in 0 1 2; do
  export S31GL_THREADS=$m
  echo "== S31GL_THREADS=$m"
  $Q /src/gl/out-rv32/core_test.qemu 2>&1 | tail -1
  $Q /src/gl/out-rv32/raster_gate.qemu 2>&1 | tail -1
  $Q /src/gl/out-rv32/fused_test.qemu 2>&1 | grep -v "^libGL" | tail -1
  $Q /src/gl/out-rv32/zepoch_test.qemu 2>&1 | tail -1
  $Q /src/gl/out-rv32/headless_gears.qemu 320 240 100 /tmp/hg.ppm >/dev/null 2>&1; md5sum /tmp/hg.ppm | cut -c1-32
done' > "$OUT/rv32-threads.log" 2>&1
cat "$OUT/rv32-threads.log"
awk '/^== /{m=$2; next} {print m" "$0}' "$OUT/rv32-threads.log" | sed 's/^S31GL_THREADS=//' > "$OUT/rv32-threads.by"
bad=0
for m in 1 2; do
	diff <(awk '$1==0{$1="";print}' "$OUT/rv32-threads.by") <(awk -v m=$m '$1==m{$1="";print}' "$OUT/rv32-threads.by") >/dev/null ||
		{ echo "run-threads-rv32: mode $m differs from mode 0"; bad=1; }
done
[ $bad = 0 ] && echo "run-threads-rv32: modes 1 and 2 identical to mode 0"
exit $bad
