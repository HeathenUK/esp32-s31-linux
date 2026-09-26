#!/bin/sh
# compare-qs.sh - image check of a gltrace: replay it on the host against
# our libGL and against Mesa (llvmpipe), then score every counted frame of
# ours against Mesa with tools/glref/compare.py (the suite's metric:
# tolerant <= 1.0% bad pixels at tol 16 is PASS).
#
#   tools/glref/gltrace/compare-qs.sh TRACE OUTDIR [OURS_DIR]
#
# Container paths (/src/...). OURS_DIR defaults to /src/gl/out-host. Writes
# OUTDIR/{ours,mesa}/ (replay-host.sh output), OUTDIR/compare.txt (one line
# per frame) and prints the worst frames and the verdict counts. The ours
# replay also re-checks its hashes against the live capture (only
# meaningful when OURS_DIR is the library the trace was recorded with).
# s31, MIT.
set -u
if [ ! -x /usr/bin/Xvfb ]; then
	R=$(cd "$(dirname "$0")/../../.." && pwd)
	exec docker run --rm -v "$R":/src -w /src -e QS_GLENV s31-glref:latest sh /src/tools/glref/gltrace/compare-qs.sh "$@"
fi
TRACE=$1; OUT=$2; OURS=${3:-/src/gl/out-host}
T=/src/tools/glref/gltrace
mkdir -p "$OUT"
sh $T/replay-host.sh ours "$TRACE" "$OUT/ours" "$OURS" &
sh $T/replay-host.sh mesa "$TRACE" "$OUT/mesa" "$OURS" > /dev/null &
wait
frames=$(awk '$5=="count"{print $1}' "$OUT/ours/hashes.txt")
for f in $frames; do
	echo "$f"
done | xargs -P "$(nproc)" -I{} sh -c "
	python3 $T/trace.py raw2png $OUT/ours/f{}.raw $OUT/ours/f{}.png 320 240 &&
	python3 $T/trace.py raw2png $OUT/mesa/f{}.raw $OUT/mesa/f{}.png 320 240 &&
	echo \"{} \$(python3 /src/tools/glref/compare.py $OUT/mesa/f{}.png $OUT/ours/f{}.png --diff $OUT/d{}.png)\"
" | sort -n > "$OUT/compare.txt"
echo "compare-qs: $(wc -l < "$OUT/compare.txt") counted frames, ours vs Mesa:"
awk '{print $2}' "$OUT/compare.txt" | sort | uniq -c
echo "worst (tolerant %):"
sed 's/.*tolerant \([0-9.]*\)%.*/\1 &/' "$OUT/compare.txt" | sort -rn | head -3 | cut -d' ' -f2-
