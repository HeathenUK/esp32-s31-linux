#!/bin/bash
# xip-census.sh - which platform code still executes from the SD card?
#
# Walks every process's /proc/<pid>/maps and groups the file-backed mappings
# by device: 00:19 is the XIP overlay (cramfs, zero RSS, a page is a cache
# fill), b3:xx is the card (a cold page is a 2.5 ms request and it competes
# for the 15 MB). Reports each SD-resident file with its mapped size and, if
# the kernel exposes smaps, its resident bytes - the bill for keeping it on
# the card. Apps stay on SD by rule; the list is for PLATFORM pieces that
# should not be there (docs/perf-plan-2026-09-23.md, Phase 3c). ~30 s.
set -u
cd "$(dirname "$0")/../.."
OUT=artifacts/perf-plan/xipcensus-$(date +%H%M%S); mkdir -p "$OUT"
cat > "$OUT/run.sh" <<'EOS'
for p in /proc/[0-9]*; do
	pid=${p#/proc/}; comm=$(cat $p/comm 2>/dev/null) || continue
	[ -r $p/maps ] || continue
	awk -v pid=$pid -v comm="$comm" '$6 ~ /^\// { split($1,a,"-"); sz=strtonum("0x" a[2]) - strtonum("0x" a[1]); print pid, comm, $4, $2, sz, $6 }' $p/maps 2>/dev/null
done > /tmp/census.txt 2>/dev/null
if [ ! -s /tmp/census.txt ]; then
	# busybox awk has no strtonum; do the arithmetic in the shell
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}; comm=$(cat $p/comm 2>/dev/null) || continue
		while read range perms off dev ino path rest; do
			case "$path" in /*) ;; *) continue;; esac
			lo=${range%-*}; hi=${range#*-}
			echo "$pid $comm $dev $perms $((0x$hi - 0x$lo)) $path"
		done < $p/maps 2>/dev/null
	done > /tmp/census.txt
fi
echo "CENSUS_BEGIN"; cat /tmp/census.txt; echo "CENSUS_END"
echo "SMAPS $( [ -r /proc/self/smaps ] && echo yes || echo no )"
for p in /proc/[0-9]*; do [ -r $p/smaps ] || continue; comm=$(cat $p/comm); awk -v c="$comm" '/^[0-9a-f]+-[0-9a-f]+ / {f=$6; d=$4} /^Rss:/ { if (f ~ /^\// && d !~ /^00:19/) print "RSS", c, d, $2, f }' $p/smaps 2>/dev/null; done
echo CX_DONE
EOS
python3 scripts/board/runsh.py "$OUT/run.sh" 60 40 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" <<'PY'
import sys, re, collections
t = open(sys.argv[1], errors='replace').read()
body = t.split('CENSUS_BEGIN')[1].split('CENSUS_END')[0] if 'CENSUS_BEGIN' in t else ''
xip = collections.defaultdict(set); sd = collections.defaultdict(set); size = {}
for l in body.strip().split('\n'):
    p = l.split()
    if len(p) < 6: continue
    pid, comm, dev, perms, sz, path = p[0], p[1], p[2], p[3], int(p[4]), p[5]
    (xip if dev.startswith('00:19') else sd)[path].add(comm)
    if 'x' in perms: size[path] = max(size.get(path, 0), sz)
rss = collections.defaultdict(int)
for m in re.finditer(r'^RSS (\S+) (\S+) (\d+) (\S+)$', t, re.M):
    rss[m.group(4)] += int(m.group(3))
print("executables/libraries mapped from the SD CARD (dev != 00:19), text size, resident kB if known, users:")
tot = 0
for path in sorted(sd, key=lambda k: -size.get(k, 0)):
    tot += size.get(path, 0)
    print("  %8d B  %6s kB  %-45s %s" % (size.get(path, 0), rss.get(path, '-'), path[:45], ",".join(sorted(sd[path]))[:60]))
print("  total mapped text from SD: %d kB in %d files; XIP-mapped files: %d" % (tot // 1024, len(sd), len(xip)))
print("smaps available:", 'SMAPS yes' in t, "| CX_DONE:", 'CX_DONE' in t)
PY
echo "log: $OUT/run.log"
