#!/bin/bash
# pp-boots.sh <boots> [pairs] - cross-hart wake penalty, per fresh boot.
#
# Report the RATIO cross/same, never the absolute: the same kernel and loader
# gave same-hart 519-691 us across boots (2026-09-21), so absolutes carry the
# per-boot lottery. Within a boot the same-hart figure is +-2% and the cross
# median over 6 pairs is about +-4%, so a per-boot ratio is solid; the ratio
# still moves +-7% between boots, which is why this takes a few of them.
# ~2.5 min per boot - keep <boots> at 3 (10-minute rule).
set -u
cd "$(dirname "$0")/../.."
N=${1:?boots}; P=${2:-6}
OUT=artifacts/smp-finish/ppboots-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
printf 'i=0; while [ $i -lt %d ]; do /root/pingpong 1 1 8000; /root/pingpong 1 2 8000; i=$((i+1)); done\necho PP_DONE\n' "$P" > "$S"
echo "pp-boots: $N boots x $P pairs -> $OUT"
for ((b = 1; b <= N; b++)); do
	python3 scripts/board/reset.py >/dev/null 2>&1
	python3 scripts/board/runsh.py "$S" 180 170 > "$OUT/boot-$b.log" 2>&1
	grep -a "pingpong " "$OUT/boot-$b.log" | python3 -c "
import sys,re,statistics
same=[];cross=[]
for l in sys.stdin:
    m=re.search(r'pingpong (\w)<->(\w)\s+([0-9.]+)',l)
    if m: (same if m.group(1)==m.group(2) else cross).append(float(m.group(3)))
if same and cross:
    print('  boot $b: same %.0f  cross %.0f  RATIO %.3f'%(statistics.median(same),statistics.median(cross),statistics.median(cross)/statistics.median(same)))
else:
    print('  boot $b: NO RESULT')
"
done
