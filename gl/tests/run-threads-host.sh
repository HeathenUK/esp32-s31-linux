#!/bin/bash
# run-threads-host.sh [OUT] - phase 6 (S31GL_THREADS) on the host rig: the
# library's own tests in every thread mode (0 one thread, 1 two real
# threads, 2 deferred), which must agree with mode 0 bit for bit:
# core_test, raster_gate, filt_test, fused_test (both S31GL_FILT8 modes) pass counts,
# headless_gears' image md5, and the QuakeSpasm traces replayed through
# GLX (tools/glref/gltrace/replay-host.sh) with every full frame's hash
# compared against mode 0. Mode 1 runs each replay twice with different
# bands (races show as differing frames, not always the same ones).
# Run on the Mac after gl/host-build.sh:
#   gl/tests/run-threads-host.sh [OUT]   (OUT default artifacts/gl/phase6/host)
# QTR_TRACES: names under gl/bench/qstrace/work (default p5f2 p5f2-tf0 p5a).
# QTR_ENV: extra library toggles for every arm (e.g. S31GL_RAMTEXT=1).
# Exit 1 on any difference. s31, MIT.
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$R/artifacts/gl/phase6/host}
mkdir -p "$OUT"
TR=${QTR_TRACES:-p5f2 p5f2-tf0 p5a}
WORK=$(cd -P "$R/gl/bench/qstrace/work" && pwd)
bad=0
# the unit tests, each mode (TCHECK: every PIPE copy is scanned for an
# unrelocated pointer into the application thread's structures)
docker run --rm -v "$R":/src -w /src/gl/out-host -e QTR_ENV="${QTR_ENV:-}" s31-glref:latest sh -c '
for kv in $QTR_ENV; do export "$kv"; done
for m in 0 1 2; do
  export S31GL_THREADS=$m S31GL_TCHECK=1
  echo "== S31GL_THREADS=$m"
  ./core_test 2>&1 | tail -1
  ./raster_gate 2>&1 | tail -1
  ./filt_test 2>&1 | tail -1
  ./fused_test 2>&1 | grep -v "^libGL" | tail -1
  S31GL_FILT8=1 ./fused_test 2>&1 | grep -v "^libGL" | tail -1
  ./zepoch_test 2>&1 | tail -1
  ./headless_gears 320 240 100 /tmp/hg.ppm 2>&1 | grep TCHECK; md5sum /tmp/hg.ppm | cut -c1-32
  S31GL_TBAND=1 S31GL_TSPLIT=1/3 ./headless_gears 320 240 100 /tmp/hg.ppm >/dev/null 2>&1; md5sum /tmp/hg.ppm | cut -c1-32
done' > "$OUT/unit.log" 2>&1
cat "$OUT/unit.log"
# every mode's lines must equal mode 0's
awk '/^== /{m=$2; next} {print m" "$0}' "$OUT/unit.log" | sed 's/^S31GL_THREADS=//' > "$OUT/unit.by"
for m in 1 2; do
	if ! diff <(awk '$1==0{$1="";print}' "$OUT/unit.by") <(awk -v m=$m '$1==m{$1="";print}' "$OUT/unit.by") >/dev/null; then
		echo "run-threads-host: unit tests differ, mode $m vs 0"; bad=1
	fi
done
grep -q "TCHECK" "$OUT/unit.log" && { echo "run-threads-host: TCHECK reported an unrelocated pointer"; bad=1; }
# the traces through GLX
for t in $TR; do
	W=/src/gl/bench/qstrace/work/$t/qs.gltr
	[ -f "$R/gl/bench/qstrace/work/$t/qs.gltr" ] || { echo "run-threads-host: $t: no trace, skipped"; continue; }
	e=; case $t in *-tf0) e=S31GL_TEXFILTER=0 ;; esac
	for arm in "t0:S31GL_THREADS=0" "t1:S31GL_THREADS=1" "t1b:S31GL_THREADS=1 S31GL_TBAND=4 S31GL_TSPLIT=3/5" "t2:S31GL_THREADS=2"; do
		n=${arm%%:*}; env=${arm#*:}
		# (the work directory may be a symlink out of the tree: mount it)
		docker run --rm -v "$R":/src -v "$WORK":/src/gl/bench/qstrace/work -w /src \
			-e QS_GLENV="$e $env S31GL_TCHECK=1 ${QTR_ENV:-}" s31-glref:latest \
			sh /src/tools/glref/gltrace/replay-host.sh ours "$W" "/src/${OUT#$R/}/$t-$n" > "$OUT/$t-$n.log" 2>&1 &
	done
	wait
	for n in t1 t1b t2; do
		a=$OUT/$t-t0/hashes.txt; b=$OUT/$t-$n/hashes.txt
		if [ ! -s "$a" ] || [ ! -s "$b" ]; then echo "run-threads-host $t $n: no hashes ($OUT/$t-$n.log)"; bad=1; continue; fi
		tot=$(wc -l < "$a"); same=$(paste -d' ' "$a" "$b" | awk '$1==$6 && $2==$7' | wc -l)
		echo "run-threads-host $t: $n $same of $tot full frames identical to S31GL_THREADS=0"
		[ "$same" -eq "$tot" ] || bad=1
		grep -h "TCHECK" "$OUT/$t-$n/replay.log" 2>/dev/null | head -2
	done
	rm -f "$OUT"/$t-*/f*.raw
done
exit $bad
