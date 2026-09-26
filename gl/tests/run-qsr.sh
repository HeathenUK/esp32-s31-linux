#!/bin/bash
# run-qsr.sh - the QuakeSpasm proxy guard: replay the phase-4-final trace
# (it uses no feature newer than phase 4) on a tree's library objects under
# the RV32 instruction counter and hold it to the phase-4 final
# (gl/bench/qstrace/baseline-p4final.txt): every full frame hash-identical
# to the live capture, and each window's M instructions per frame within
# +0.5% (plan bar 2), in the default configuration and, with
# QSR_ARMS="default tf0", also with S31GL_TEXFILTER=0.
#
#   gl/tests/run-qsr.sh [GLDIR] [OUT]
#     GLDIR  default this repo's gl/;  OUT  gl/bench/out-qsr-guard
# The trace is gitignored (69 MB); recreate it, deterministic, with
#   QS_LIBGL=/src/gl/bench/base5/out-host/libGL.so.1 tools/glref/gltrace/capture-qs.sh p4final
#   QS_LIBGL=/src/gl/bench/base5/out-host/libGL.so.1 QS_GLENV=S31GL_TEXFILTER=0 \
#       tools/glref/gltrace/capture-qs.sh p4final-tf0
# (gl/bench/base5 = git archive 246e832, its host libGL built as
# tools/glref/README.md says). A deliberate pixel change (the DARKNESS.md
# rounding fixes, say) fails the hash half by design: record it.
# s31, MIT.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
GLDIR=${1:-$R/gl}
OUT=${2:-$R/gl/bench/out-qsr-guard}
BL=$R/gl/bench/qstrace/baseline-p4final.txt
W=$R/gl/bench/qstrace/work
bad=0
first=1
for arm in ${QSR_ARMS:-default}; do
	case $arm in
	default) env=""; tr=$W/p4final/qs.gltr; key=default ;;
	tf0) env="S31GL_TEXFILTER=0"; tr=$W/p4final-tf0/qs.gltr; key=S31GL_TEXFILTER=0 ;;
	*) echo "run-qsr: unknown arm $arm"; exit 2 ;;
	esac
	[ -f "$tr" ] || { echo "run-qsr: no $tr (see the header)"; exit 2; }
	nb=; [ $first = 1 ] || nb=1
	first=0
	QSR_NOBUILD=$nb QSR_NULL=0 QSR_LABEL=guard-$arm QSR_ENV="$env" \
		"$R/gl/bench/qsreplay.sh" "$GLDIR" "$OUT" "$tr" > "$OUT.$arm.log" 2>&1 || { cat "$OUT.$arm.log"; exit 1; }
	for w in 0 1; do
		line=$(grep "window $w " "$OUT.$arm.log")
		got=$(echo "$line" | sed 's/.*: \([0-9.]*\) Minsn.*/\1/')
		hs=$(echo "$line" | sed 's/.*hashes \([0-9]*\/[0-9]*\).*/\1/')
		ref=$(awk -F' *[|] *' -v k="$key" -v w=$w '$1==k && $2==w {print $4}' "$BL")
		v=$(python3 -c "g,r=$got,$ref; print('%+.2f%% %s' % (100*(g-r)/r, 'ok' if g <= r*1.005 else 'OVER'))")
		echo "run-qsr $arm window $w: $got Minsn/frame vs phase-4 final $ref: $v; hashes $hs same as live"
		case "$v" in *OVER*) bad=1 ;; esac
		[ "${hs%/*}" = "${hs#*/}" ] || bad=1
	done
done
exit $bad
