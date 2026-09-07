#!/bin/bash
# Measure lvdesk and prboom cost PER RENDERED FRAME, not per wall-clock second.
#
# WHY. Every A/B on 2026-09-07 was ambiguous, and the instrument was the
# reason. `top` sampled %CPU at a fixed offset into the demo, so the sample
# landed wherever boot timing put it: prboom read 40% in one run and 53% in
# the next FROM THE SAME BINARY, because one sample caught a heavy scene and
# the other did not. Against that, a real ~2-point effect is invisible, and a
# pair of lucky samples looks like a 13-point win - which is exactly the
# retraction that had to be written.
#
# Ticks-per-frame divides the scene out. A heavy scene raises the ticks AND
# lowers the frame count, so the ratio is stable where the rate is not.
# Frames come from lvdesk's FRAMES counter, which ticks once per refresh in
# lv_display_flush_is_last() - ABOVE the expansion, so it is identical on both
# arms. An earlier version counted BLIT (once per flush RECTANGLE) against
# EXPAND (once per frame): different units per arm, which made the direct arm
# look like it raised prboom's per-frame cost by 26%. A denominator that means
# something different in each arm is worse than no normalisation at all.
#
#   perframe.sh [settle_s] [window_s]      default 40 60
#
# Prints ticks/frame for both processes. Lower is better. Compare arms; the
# absolute unit (USER_HZ) does not matter for a comparison.
set -u
cd "$(dirname "$0")/../.."
D=${CLAUDE_JOB_DIR:-/tmp}/tmp; mkdir -p "$D"
SETTLE=${1:-40}
WINDOW=${2:-60}

cat > "$D/pf_run.sh" <<SH
export DISPLAY=:0
export DOOMWADDIR=/root/doom/wads
for p in \$(ps | awk '/prboom/ && !/awk/ {print \$1}'); do kill -9 \$p 2>/dev/null; done
sleep 1
: > /var/log/lvdesk.log 2>/dev/null
cd /root/doom/wads
setsid /root/doom/prboom -width 320 -height 200 -nosound -timedemo demo1 \\
	>/dev/null 2>&1 </dev/null &
sleep $SETTLE
# Sample 1. Frames = counter lines x 200; both arms print one per 200.
L=\$(ps | awk '/[l]vdesk/{print \$1; exit}')
P=\$(ps | awk '/[p]rboom/{print \$1; exit}')
lt1=\$(awk '{print \$14+\$15}' /proc/\$L/stat)
pt1=\$(awk '{print \$14+\$15}' /proc/\$P/stat)
f1=\$(grep -c 'FRAMES' /var/log/lvdesk.log)
sleep $WINDOW
lt2=\$(awk '{print \$14+\$15}' /proc/\$L/stat 2>/dev/null)
pt2=\$(awk '{print \$14+\$15}' /proc/\$P/stat 2>/dev/null)
f2=\$(grep -c 'FRAMES' /var/log/lvdesk.log)
echo "ZZ lv=\$((lt2-lt1)) pb=\$((pt2-pt1)) frames=\$(( (f2-f1) * 200 ))"
echo "ZZ path=\$(grep -c EXPAND /var/log/lvdesk.log)expand (0 = direct path)"
SH

OUT=$(python3 scripts/board/runsh.py "$D/pf_run.sh" $((SETTLE + WINDOW + 40)) 2>&1 | grep '^ZZ ')
echo "$OUT" | sed 's/^ZZ /  /'
LV=$(echo "$OUT" | sed -n 's/.*lv=\([0-9]*\).*/\1/p')
PB=$(echo "$OUT" | sed -n 's/.*pb=\([0-9]*\).*/\1/p')
FR=$(echo "$OUT" | sed -n 's/.*frames=\([0-9]*\).*/\1/p')
if [ -z "${FR:-}" ] || [ "$FR" -le 0 ]; then
	echo "  NO FRAMES COUNTED - the window was too short to cross a 200-frame"
	echo "  boundary, or the client never drew. Nothing here is meaningful."
	exit 1
fi
awk -v lv="$LV" -v pb="$PB" -v fr="$FR" 'BEGIN{
	printf "  lvdesk %.3f ticks/frame   prboom %.3f ticks/frame   (%d frames)\n",
	       lv/fr, pb/fr, fr }'
