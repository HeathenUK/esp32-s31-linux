#!/bin/bash
# run-qsr-threads.sh [GLDIR] [OUT] - phase 6 (S31GL_THREADS) over the
# QuakeSpasm proxy: every trace of QSR_TRACES replayed on the RV32
# instruction counter (gl/bench/qsreplay.sh) with S31GL_THREADS=0 and with
# S31GL_THREADS=2 (the worker's records run deferred, at each sync, on the
# same hart - the bare-metal image has no threads; a missing sync point
# shows as a wrong frame every time), at the default bands and at a second
# band layout; every full frame's RGB565 hash must be identical between
# the arms. The mode-2 arms also give the work split: per window, the
# instructions the worker ran (qsrt lines) and main's remainder, and
# max(main, worker) - the instruction proxy of a two-thread frame.
# (Real concurrency is gated on the host: gl/tests/run-threads-host.sh.)
#   gl/tests/run-qsr-threads.sh [GLDIR] [OUT]    (GLDIR: this repo's gl/)
# QSR_TRACES: names under gl/bench/qstrace/work (default: the phase 5
# captures; missing ones are skipped). Exit 1 on any differing frame.
# s31, MIT.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
GLDIR=${1:-$R/gl}
OUT=${2:-$R/gl/bench/out-qsr-threads}
W=$R/gl/bench/qstrace/work
TR=${QSR_TRACES:-p5f2 p5f2-tf0 p5a p5a-tf0 p5a-nmn p5a-nml p5a-lmn p5a-lin p5a-near p5wide}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
"$R/gl/bench/build_q.sh" "$GLDIR" "$OUT" > "$OUT/build_q.log" 2>&1 || { cat "$OUT/build_q.log"; exit 1; }
arm() {   # trace, arm name, env
	local d=$OUT/$1-$2
	rm -rf "$d"; mkdir -p "$d"
	ln -s "$OUT/obj" "$d/obj"; ln -s "$OUT/demo" "$d/demo"
	QSR_NOBUILD=1 QSR_NULL=0 QSR_LABEL=$2 QSR_ENV="$3" \
		"$R/gl/bench/qsreplay.sh" "$GLDIR" "$d" "$W/$1/qs.gltr" > "$d/log" 2>&1 || true
}
pids=
for t in $TR; do
	[ -f "$W/$t/qs.gltr" ] || { echo "run-qsr-threads: $t: no trace, skipped"; continue; }
	e=; case $t in *-tf0) e=S31GL_TEXFILTER=0 ;; esac
	arm "$t" t0 "$e" & pids="$pids $!"
	arm "$t" t2 "S31GL_THREADS=2 $e" & pids="$pids $!"
	arm "$t" t2b "S31GL_THREADS=2 S31GL_TBAND=4 S31GL_TSPLIT=3/5 $e" & pids="$pids $!"
done
for p in $pids; do wait $p; done
bad=0
for t in $TR; do
	[ -f "$W/$t/qs.gltr" ] || continue
	a=$OUT/$t-t0/qsr/run-t0-lib/out.txt
	grep -q "done" "$a" 2>/dev/null || { echo "run-qsr-threads $t: t0 did not finish"; bad=1; continue; }
	awk '$1=="qsrf"{print $2, $5}' "$a" > "$OUT/$t.t0"
	for n in t2 t2b; do
		b=$OUT/$t-$n/qsr/run-$n-lib/out.txt
		grep -q "done" "$b" 2>/dev/null || { echo "run-qsr-threads $t: $n did not finish"; bad=1; continue; }
		awk '$1=="qsrf"{print $2, $5}' "$b" > "$OUT/$t.$n"
		tot=$(($(wc -l < "$OUT/$t.t0"))); same=$(($(paste -d' ' "$OUT/$t.t0" "$OUT/$t.$n" | awk '$1==$3 && $2==$4' | wc -l)))
		echo "run-qsr-threads $t: $n $same of $tot full frames identical to S31GL_THREADS=0"
		[ "$tot" -gt 0 ] && [ "$same" -eq "$tot" ] || { bad=1; paste -d' ' "$OUT/$t.t0" "$OUT/$t.$n" | awk '$2!=$4' | head -3; }
		# the split, per window (frames < 300: window 0)
		awk -v t=$t -v n=$n '
		$1=="qsrf" && $3=="count" {tot[$2]=$4}
		$1=="qsrt" && $3=="count" {w[$2]=$5; tri[$2]=$7; pipe[$2]=$9; by[$2]=$19}
		END { for (k in tot) { i = (k+0 < 300) ? 0 : 1; c[i]++; T[i]+=tot[k]; Wk[i]+=w[k]; TR[i]+=tri[k]; P[i]+=pipe[k]; B[i]+=by[k];
		        m = tot[k]-w[k]; M[i] += (m > w[k] ? m : w[k]) }
		  for (i = 0; i < 2; i++) if (c[i]) printf("run-qsr-threads %s %s window %d: total %.2f, worker %.2f (%.1f%%), main %.2f, max %.2f M/frame; %.0f tri, %.1f pipe copies, %.0f kB ring /frame\n",
		    t, n, i, T[i]/c[i]/1e6, Wk[i]/c[i]/1e6, 100*Wk[i]/T[i], (T[i]-Wk[i])/c[i]/1e6, M[i]/c[i]/1e6, TR[i]/c[i], P[i]/c[i], B[i]/c[i]/1024) }' "$b"
	done
	for w in 0 1; do
		l0=$(grep "window $w " "$a" | sed 's/.*: \([0-9.]*\) Minsn.*/\1/')
		[ -n "$l0" ] && echo "run-qsr-threads $t window $w: S31GL_THREADS=0 $l0 Minsn/frame"
	done
done
exit $bad
