#!/bin/bash
# Real-world A/B: Doom's own timedemo, fresh boot per run, arms interleaved.
#
#   ab.sh <name> <repeats> <armA-env> <armB-env> [armC-env ...]
#
#   ab.sh direct 3 "" "LVDESK_DIRECT=1"
#   VS_MODE=window VS_SOUND=1 ab.sh sound 3 "" "S31ROUTE_BUF=8192"
#
# Each arm is a VS_ENV string written to /etc/lvdesk.env by verify-sdl.sh
# before the reset, so it is fixed for the whole boot and confirmed on the
# board. "" is the stock arm (the file is emptied, so it is provable too).
#
# WHY THIS SHAPE. Every rule in it was paid for:
#   - fresh boot per run: performance decays run-over-run as memory pressure
#     builds, so two arms sharing a boot measure the decay (memory:
#     s31-desktop-measurement). verify-sdl.sh resets before every run.
#   - interleaved A B C, C B A: an order effect shows up as a trend within an
#     arm instead of hiding as a difference between arms.
#   - the metric is prboom's own "frames per second" from a 5026-gametic
#     timedemo, cross-checked against the monotonic clock inside verify-sdl.
#     That is the number the user feels; sdlbench primitives are for
#     attribution, not for the headline.
#   - spread is printed with the median. Two 12-18% "wins" here were inside a
#     +-22-40% band. If the ranges overlap, the answer is "no difference".
#
# Budget: ~4 min per run (50 s boot, ~190 s demo). 3 repeats x 2 arms = ~25 min.
# Results land in artifacts/ab/<name>-<timestamp>/ with every verify-sdl log.
set -u
cd "$(dirname "$0")/../.."
NAME=${1:?name}; REP=${2:?repeats}; shift 2
[ $# -ge 2 ] || { echo "need at least two arms"; exit 2; }
ARMS=("$@")
OUT=artifacts/ab/$NAME-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
MODE=${VS_MODE:-fullscreen}; W=${AB_W:-320}; H=${AB_H:-200}
echo "ab: $NAME  ${#ARMS[@]} arms x $REP  ${W}x${H} $MODE sound=${VS_SOUND:-1}  -> $OUT"

for ((r = 1; r <= REP; r++)); do
	order=$(seq 0 $((${#ARMS[@]} - 1)))
	[ $((r % 2)) = 0 ] && order=$(seq $((${#ARMS[@]} - 1)) -1 0)
	for i in $order; do
		arm=${ARMS[$i]}; tag="arm$i-r$r"
		echo "--- $tag  [${arm:-stock}]"
		# One retry when a HARNESS gate rejected the run (clock, boot
		# window, launch) - those are not the board's result. A fps
		# verdict, pass or fail, is never retried.
		for attempt in 1 2; do
			VS_ENV="$arm" VS_MODE=$MODE bash scripts/board/verify-sdl.sh "$W" "$H" --timedemo > "$OUT/$tag.log" 2>&1
			grep -qE "clock never settled|no command executed within|could not start the client|env not applied" "$OUT/$tag.log" || break
			[ $attempt = 1 ] && { echo "    $tag: harness gate ($(grep -oE 'clock never settled|no command executed within|could not start the client|env not applied' "$OUT/$tag.log" | head -1)) - retrying once"; cp "$OUT/$tag.log" "$OUT/$tag.attempt1.log"; }
		done
		fps=$(sed -n 's/.*= \([0-9.]*\) frames per second.*/\1/p' "$OUT/$tag.log" | tail -1)
		res=$(grep -o "RESULT *: [A-Z]*" "$OUT/$tag.log" | tail -1)
		echo "$i|${arm:-stock}|$r|${fps:-NA}|${res:-NORESULT}" >> "$OUT/results.psv"
		echo "    $tag: ${fps:-NA} fps  ${res:-NORESULT}"
	done
done

echo
echo "=== $NAME: ${W}x${H} $MODE, timedemo fps per arm (fresh boot each) ==="
python3 - "$OUT/results.psv" <<'PY'
import statistics, sys, collections
rows=[l.rstrip('\n').split('|') for l in open(sys.argv[1])]
by=collections.OrderedDict()
for i,arm,r,fps,res in rows:
    by.setdefault((i,arm),[]).append((fps,res))
for (i,arm),v in by.items():
    good=[float(f) for f,res in v if f!='NA' and res.endswith('PASS')]
    bad=len(v)-len(good)
    if good:
        med=statistics.median(good)
        print(f"  arm{i} [{arm}]: median {med:.1f} fps  range {min(good):.1f}-{max(good):.1f}  n={len(good)}" + (f"  ({bad} rejected)" if bad else ""))
    else:
        print(f"  arm{i} [{arm}]: no accepted runs ({bad} rejected)")
PY
echo "overlapping ranges mean NO difference. logs: $OUT"
