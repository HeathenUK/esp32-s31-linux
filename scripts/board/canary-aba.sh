#!/bin/bash
# canary-aba.sh <labelA> <imageA> <labelB> <imageB> [rounds]
#
# A/B/A the gate's SDL canaries across two KERNEL IMAGES, flashing between
# arms, so the comparison is fresh-boot to fresh-boot rather than one arm's
# lucky boot. Prints every arm's mean so the spread is visible; A and A2 tell
# you the noise floor for free, and if they disagree the B result means nothing.
#
# Why: 2026-09-21 a .text..fast batch moved the windowed canary 16.80 -> 12.69 ms
# (-24%) in ONE gate run. The mechanism (43 kB of hot text out of flash) does
# not obviously account for that much, and this project's most expensive
# mistake has always been believing a single run.
set -u
cd "$(dirname "$0")/../.."
LA=${1:?labelA}; IA=${2:?imageA}; LB=${3:?labelB}; IB=${4:?imageB}; N=${5:-1}
OUT=artifacts/smp-finish/canary-aba-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"

arm() {
	local label=$1 img=$2 tag=$3

	[ -r "$img" ] || { echo "missing image: $img"; exit 2; }
	cp "$img" images/xipImage
	cp "${img/xipImage/System.map}" images/System.map 2>/dev/null
	make flash-linux 2>&1 | grep -aq "Hash of data verified" || { echo "$tag: FLASH FAILED"; return; }
	PYTHONUNBUFFERED=1 python3 scripts/board/gate.py --reset --repeats 5 \
		> "$OUT/$tag.log" 2>&1 || true
	echo "  $tag ($label, $(basename "$img")):"
	grep -a "\[canary\]" "$OUT/$tag.log" | sed 's/^ */    /'
}

for ((r = 1; r <= N; r++)); do
	echo "=== round $r"
	arm "$LA" "$IA" "r$r-A"
	arm "$LB" "$IB" "r$r-B"
	arm "$LA" "$IA" "r$r-A2"
done
echo "logs: $OUT"
