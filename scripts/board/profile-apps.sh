#!/bin/bash
# profile-apps.sh [seconds] - where does the KERNEL time go, across several
# real applications at once? A shortlist of .text..fast candidates, chosen by
# what is hot in aggregate rather than in one benchmark.
#
# Uses the hart0 PC sampler (rootfs/h1s + bootloader "H1 PC SAMPLER"), which
# needs nothing in the target: it reads hart 1's PC from the bus monitor at
# 1 kHz and sees user code, kernel code with interrupts off, and M-mode alike.
#
# WHY AGGREGATE. Profiling one app misleads. Doom's own profile (2026-09-21)
# has NO kernel symbol above 0.7% and 19% idle - from Doom alone you would
# conclude there is nothing left to move, which is false for the desktop:
# softirq.o, invisible in Doom, is -19.7% on syscall/IPC round trips.
#
# TWO RULES, both learned the hard way, before acting on anything here:
#  1. Pick by EXECUTION FREQUENCY, not sample share. A batch chosen purely by
#     profile share (PELT, div64, timerqueue, select, uaccess - 43 kB) was a
#     5% REGRESSION: .text..fast is cached PSRAM and the D-cache is SHARED
#     between the harts, so added RAM text evicts data both CPUs use. A
#     candidate must be hot AND small. softirq.o won at 3,662 bytes.
#  2. VALIDATE with scripts/board/pp-boots.sh (same-hart medians, 3 boots),
#     never with the app - the windowed canary has a ~17% per-boot lottery.
#
# The sampler addresses move with every loader build:
#   ./docker/build.sh 'riscv32-esp-elf-nm /src/bootloader/build/hello_world.elf | grep s31_h1s_'
set -u
cd "$(dirname "$0")/../.."
SECS=${1:-6}
CTRL=${H1S_CTRL:-0x2f031968}
BUF=${H1S_BUF:-0x2f026630}
OUT=artifacts/smp-finish/profapps-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh

# label:command - anything missing on the card is skipped, not fatal.
APPS=${APPS:-"doomfs:cd /root/doom && DISPLAY=:0 ./prboom -width 320 -height 200 -timedemo demo1
doomwin:cd /root/doom && DISPLAY=:0 ./prboom -width 320 -height 200 -window -timedemo demo1
sdlbench:DISPLAY=:0 /root/sdlbench1 --case indexed_frame --frames 600 --warmup 5
idle:sleep 12"}

echo "profile-apps: ${SECS}s of samples per app -> $OUT"
while IFS= read -r line; do
	[ -z "$line" ] && continue
	label=${line%%:*}; cmd=${line#*:}
	cat > "$S" <<EOF
chmod +x /root/h1s 2>/dev/null
for p in \$(ps | awk '/[p]rboom|[s]dlbench/ {print \$1}'); do kill -9 \$p 2>/dev/null; done
rm -f /root/prof.txt
setsid sh -c '$cmd' </dev/null >/dev/null 2>&1 &
sleep 12
/root/h1s $CTRL $BUF $((SECS * 1000)) > /root/prof.txt 2>&1
# The app's maps, taken DURING the capture: user PCs cannot be resolved
# without them (PIE bases change every boot). h1s-report.py --maps reads it.
P=\$(ps | awk '/[p]rboom|[s]dlbench|[t]iopex|[t]yrquake|[o]pentyrian|[c]hocolate/ {print \$1}' | head -1)
[ -n "\$P" ] && cat /proc/\$P/maps > /root/maps.txt 2>/dev/null || : > /root/maps.txt
for p in \$(ps | awk '/[p]rboom|[s]dlbench/ {print \$1}'); do kill -9 \$p 2>/dev/null; done
echo "SAMPLES \$(grep -ac . /root/prof.txt)"
echo PROF_BEGIN
cat /root/prof.txt
echo PROF_END
echo MAPS_BEGIN
cat /root/maps.txt
echo MAPS_END
EOF
	python3 scripts/board/runsh.py "$S" 120 170 2>/dev/null | tr -d '\r' > "$OUT/$label.raw"
	sed -n '/^PROF_BEGIN/,/^PROF_END/p' "$OUT/$label.raw" | grep -aE "^[0-9a-f]{8}$" > "$OUT/$label.txt"
	sed -n '/^MAPS_BEGIN/,/^MAPS_END/p' "$OUT/$label.raw" | grep -av "^MAPS_" > "$OUT/$label.maps"
	n=$(grep -ac . "$OUT/$label.txt" 2>/dev/null || echo 0)
	echo "  $label: $n samples, $(grep -ac . "$OUT/$label.maps") mappings"
	# User symbols resolve through the maps against images/<basename>.nm
	# (`nm -nS` of the unstripped twin: rootfs/*.dbg, buildroot staging libs).
	[ "$n" -gt 100 ] && python3 scripts/board/h1s-report.py images/System.map "$OUT/$label.txt" 8 \
		--maps "$OUT/$label.maps" --symdir images | sed -n '2,24p' | sed 's/^/      /'
done <<< "$APPS"

cat "$OUT"/*.txt > "$OUT/all.txt" 2>/dev/null
echo
echo "=== AGGREGATE across all apps ==="
python3 scripts/board/h1s-report.py images/System.map "$OUT/all.txt" 25
echo "logs: $OUT"
