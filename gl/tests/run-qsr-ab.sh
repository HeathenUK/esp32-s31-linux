#!/bin/bash
# run-qsr-ab.sh - bit-identity gate over the QuakeSpasm GL workload proxy:
# replay one gltrace twice on the RV32 instruction counter
# (gl/bench/qsreplay.sh), once per runtime-toggle arm, and require every
# full frame's RGB565 hash to be identical between the arms.
#
#   gl/tests/run-qsr-ab.sh "ENV_A" "ENV_B" [GLDIR] [TRACE] [OUT]
#     ENV_A/ENV_B  space-separated KEY=VALUE toggles ("" = the defaults),
#                  e.g. "S31GL_FUSED=0" "" once a fused-filler switch exists:
#                  the general path forced against the fused one (plan bar 3)
#     GLDIR        default this repo's gl/;  TRACE  default
#                  gl/bench/qstrace/work/qs/qs.gltr;  OUT  gl/bench/out-qsr-ab
# Prints both arms' per-window instructions and "ab: N of N frames
# identical"; exit 1 on any differing frame (the first ones are listed).
# s31, MIT.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
A=$1; Bv=$2
GLDIR=${3:-$R/gl}
TRACE=${4:-$R/gl/bench/qstrace/work/qs/qs.gltr}
OUT=${5:-$R/gl/bench/out-qsr-ab}
QSR_NULL=0 QSR_LABEL=arm-a QSR_ENV="$A" "$R/gl/bench/qsreplay.sh" "$GLDIR" "$OUT" "$TRACE" | grep window
QSR_NOBUILD=1 QSR_NULL=0 QSR_LABEL=arm-b QSR_ENV="$Bv" "$R/gl/bench/qsreplay.sh" "$GLDIR" "$OUT" "$TRACE" | grep window
Q=$OUT/qsr
awk '$1=="qsrf"{print $2, $5}' "$Q/run-arm-a-lib/out.txt" > "$Q/ab-a.txt"
awk '$1=="qsrf"{print $2, $5}' "$Q/run-arm-b-lib/out.txt" > "$Q/ab-b.txt"
n=$(($(wc -l < "$Q/ab-a.txt")))
same=$(($(paste -d' ' "$Q/ab-a.txt" "$Q/ab-b.txt" | awk '$1==$3 && $2==$4' | wc -l)))
echo "ab: [$A] vs [$Bv]: $same of $n frames identical"
[ "$n" -gt 0 ] && [ "$same" -eq "$n" ] || {
	paste -d' ' "$Q/ab-a.txt" "$Q/ab-b.txt" | awk '$2!=$4' | head -5
	exit 1
}
