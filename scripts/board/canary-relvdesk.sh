#!/bin/bash
# canary-relvdesk.sh <boots> - narrow the boot lottery: is the fixed state
# LVDESK's (allocated once at desktop start) or the DRIVER/KERNEL's?
#
# What is already known (2026-09-21):
#  - ~1 boot in 4 is ~17% slow on the windowed canary, steady all run.
#  - It is NOT task placement: squeezing every task through CPU0 and releasing
#    it does not recover a slow boot (canary-reset.sh).
#  - It is NOT per-client memory: the canary runs three FRESH processes per
#    boot, each with a new SHM segment and heap, and the three agree to ~0.5%.
# So the state is allocated once and kept. lvdesk is the obvious holder of
# such state (its buffers, its mapping of the scanout buffer), and restarting
# it re-does exactly those allocations while leaving the kernel and driver
# alone - including the scanout buffer, which is now reserved at probe.
#
#   slow boot goes fast after a restart -> lvdesk's own allocations
#   slow boot stays slow                -> driver/kernel side; chase the
#                                          scanout/CMA/page placement instead
#
# Records the scanout physical address per boot too: it should now be constant
# (reserved at probe), and if it is, it is eliminated as the variable.
set -u
cd "$(dirname "$0")/../.."
N=${1:?boots}
OUT=artifacts/smp-finish/relvdesk-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<'EOF'
cd /root
echo "SCAN $(dmesg | grep -a 'scanout buffer .* at ' | tail -1 | sed 's/.*at //')"
echo PHASE_BEFORE
i=0; while [ $i -lt 2 ]; do
	DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
	i=$((i+1))
done
/etc/init.d/S40lvdesk restart >/dev/null 2>&1 || {
	for p in $(ps | awk '/[l]vdesk/ {print $1}'); do kill $p; done
	sleep 3
	/etc/init.d/S40lvdesk start >/dev/null 2>&1
}
sleep 8
echo "RESTART lvdesk=$(ps | grep -c '[l]vdesk') $(grep -a 'DIRECT scanout' /var/log/lvdesk.log | tail -1 | cut -c1-40)"
echo PHASE_AFTER
i=0; while [ $i -lt 2 ]; do
	DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 60 --warmup 5 2>/dev/null | grep -a '"type":"measure"'
	i=$((i+1))
done
echo RUN_DONE
EOF

echo "canary-relvdesk: $N fresh boots -> $OUT"
for ((b = 1; b <= N; b++)); do
	python3 scripts/board/reset.py >/dev/null 2>&1
	python3 scripts/board/runsh.py "$S" 220 170 > "$OUT/boot-$b.log" 2>&1
	python3 - "$OUT/boot-$b.log" "$b" <<'PY'
import json, re, statistics, sys
txt = open(sys.argv[1], errors='replace').read()
def means(seg):
    return [json.loads(m.group(0))["mean_ns"] / 1e6
            for m in re.finditer(r'\{.*?"type":\s*"measure".*?\}', seg)]
before = means(txt.split('PHASE_AFTER')[0])
after = means(txt.split('PHASE_AFTER')[-1]) if 'PHASE_AFTER' in txt else []
scan = re.search(r'SCAN (\S+)', txt)
alive = re.search(r'RESTART lvdesk=(\d+)', txt)
f = lambda v: ("%.3f" % statistics.median(v)) if v else "NONE"
note = ""
if before and after:
    b, a = statistics.median(before), statistics.median(after)
    if b > 15:
        note = "   <<< SLOW BOOT WENT FAST" if a < 15 else "   <<< stayed slow"
print("  boot %s: scanout %s  lvdesk_after=%s  before %s  after %s%s" % (
    sys.argv[2], scan.group(1) if scan else "?",
    alive.group(1) if alive else "?", f(before), f(after), note))
PY
done
echo "logs: $OUT"
