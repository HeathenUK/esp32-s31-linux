#!/bin/bash
# run-qsr-fused.sh [GLDIR] [OUT] - plan bar 3 over the QuakeSpasm proxy:
# every trace of QSR_TRACES replayed on the RV32 instruction counter
# (gl/bench/qsreplay.sh) twice, with S31GL_FUSED=0 (the general stage lists
# only) and with the default (the fused fillers of gl/tinygl/source/
# zpipe_fused.c), and every full frame's RGB565 hash must be identical
# between the arms and to the live capture. One build, the replays in
# parallel (each arm in its own directory, sharing the objects).
#
#   gl/tests/run-qsr-fused.sh [GLDIR] [OUT]     (GLDIR: this repo's gl/)
# QSR_TRACES: names under gl/bench/qstrace/work (default: the phase 5
# captures - the default texture mode, S31GL_TEXFILTER=0, QuakeSpasm's five
# other gl_texturemode settings, the wide census capture - and the phase 4
# no-multitexture ones); a name ending in -tf0 replays with
# S31GL_TEXFILTER=0, as it was captured. Missing traces are skipped (they
# are gitignored: tools/glref/gltrace/capture-qs.sh, QS_CFG for the modes).
# Phase 5: a third arm, ref (S31GL_FUSED=0 S31GL_TEX8=2: the general path
# on the unpacked RGBA8 reference storage of every P8 / L8 texture), must
# match too - the P8 / L8 fetches, the fused fillers and tier 1's 8-bit
# filler against the general path on unpacked texels (plan bars 3 and (e)).
# The live capture's hashes are compared only with QSR_LIVE=1 (a build
# whose pixels are the capturing library's: -DS31GL_P4ARITH against the
# phase 4 traces); phase 5's precision changes every frame by design.
# Prints per trace and window the arms' M instructions per frame; exit 1
# on any differing frame. s31, MIT.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
GLDIR=${1:-$R/gl}
OUT=${2:-$R/gl/bench/out-qsr-fused}
W=$R/gl/bench/qstrace/work
TR=${QSR_TRACES:-p5a p5a-tf0 p5a-nmn p5a-nml p5a-lmn p5a-lin p5a-near p5wide p4final p4final-tf0}
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
	[ -f "$W/$t/qs.gltr" ] || { echo "run-qsr-fused: $t: no trace, skipped"; continue; }
	e=; case $t in *-tf0) e=S31GL_TEXFILTER=0 ;; esac
	arm "$t" general "S31GL_FUSED=0 $e" & pids="$pids $!"
	arm "$t" fused "$e" & pids="$pids $!"
	arm "$t" ref "S31GL_FUSED=0 S31GL_TEX8=2 $e" & pids="$pids $!"
done
for p in $pids; do wait $p; done
bad=0
for t in $TR; do
	[ -f "$W/$t/qs.gltr" ] || continue
	g=$OUT/$t-general/qsr/run-general-lib/out.txt; f=$OUT/$t-fused/qsr/run-fused-lib/out.txt
	rf=$OUT/$t-ref/qsr/run-ref-lib/out.txt
	if [ ! -f "$g" ] || [ ! -f "$f" ] || [ ! -f "$rf" ] || ! grep -q "done" "$g" || ! grep -q "done" "$f" ||
	   ! grep -q "done" "$rf"; then
		echo "run-qsr-fused $t: a replay did not finish ($OUT/$t-*/log)"; bad=1; continue
	fi
	awk '$1=="qsrf"{print $2, $5}' "$g" > "$OUT/$t.g"; awk '$1=="qsrf"{print $2, $5}' "$f" > "$OUT/$t.f"
	awk '$1=="qsrf"{print $2, $5}' "$rf" > "$OUT/$t.r"
	n=$(($(wc -l < "$OUT/$t.g"))); same=$(($(paste -d' ' "$OUT/$t.g" "$OUT/$t.f" | awk '$1==$3 && $2==$4' | wc -l)))
	rsame=$(($(paste -d' ' "$OUT/$t.r" "$OUT/$t.f" | awk '$1==$3 && $2==$4' | wc -l)))
	live=$(grep -o '[0-9]* same as live, [0-9]* differ' "$f" | head -1)
	for w in 0 1 2 3 4 5 6 7; do
		lg=$(grep "window $w " "$g" | sed 's/.*: \([0-9.]*\) Minsn.*/\1/'); lf=$(grep "window $w " "$f" | sed 's/.*: \([0-9.]*\) Minsn.*/\1/')
		[ -n "$lg" ] || continue
		echo "run-qsr-fused $t window $w: general $lg, fused $lf Minsn/frame ($(python3 -c "print('%+.1f%%' % (100*($lf-$lg)/$lg))"))"
	done
	echo "run-qsr-fused $t: $same of $n full frames identical, fused vs general; $rsame of $n, fused vs the unpacked reference; fused vs live: $live"
	[ "$n" -gt 0 ] && [ "$same" -eq "$n" ] || { bad=1; paste -d' ' "$OUT/$t.g" "$OUT/$t.f" | awk '$2!=$4' | head -3; }
	[ "$rsame" -eq "$n" ] || { bad=1; paste -d' ' "$OUT/$t.r" "$OUT/$t.f" | awk '$2!=$4' | head -3; }
	if [ -n "$QSR_LIVE" ]; then case "$live" in *" 0 differ") ;; *) bad=1 ;; esac; fi
done
exit $bad
