#!/usr/bin/env python3
"""prelock-pick.py <System.map> <samples.txt> <window bytes> [top N]

Rank fixed, window-aligned slices of the FLASH kernel text ([_stext, _etext),
which excludes the RAM .text.fast region) by hart-1 PC samples, for the
I-cache prelock sweep (perf-plan 2026-09-23 C23). Prints, per window: sample
share, address, and the symbols it holds most of. The prelock takes a
64-byte-aligned address and a size up to 16383, so any window here is
directly writable to /sys/module/esp32s31_cache/parameters/icache_prelock.
"""
import bisect
import sys
from collections import Counter

if len(sys.argv) < 4:
    sys.exit(__doc__)
smap, spath, win = sys.argv[1], sys.argv[2], int(sys.argv[3])
top = int(sys.argv[4]) if len(sys.argv) > 4 else 8

syms = []
marks = {}
for line in open(smap):
    p = line.split()
    if len(p) < 3:
        continue
    a = int(p[0], 16)
    if p[2] in ("_stext", "_etext", "_sfast", "_efast"):
        marks[p[2]] = a
    if p[1] in "tTwW":
        syms.append((a, p[2]))
syms.sort()
addrs = [a for a, _ in syms]
lo, hi = marks["_stext"], marks["_etext"]

pcs = []
for line in open(spath):
    line = line.strip()
    if not line:
        continue
    try:
        pcs.append(int(line, 16))
    except ValueError:
        pass
n = len(pcs)
flash = [pc for pc in pcs if lo <= pc < hi]
ram = [pc for pc in pcs if pc >= hi and pc < 0xc1000000]
user = [pc for pc in pcs if pc < lo]
print("samples %d: flash text %d (%.1f%%), other kernel %d (%.1f%%), below _stext %d (%.1f%%)" % (
    n, len(flash), 100.0 * len(flash) / max(n, 1), len(ram), 100.0 * len(ram) / max(n, 1),
    len(user), 100.0 * len(user) / max(n, 1)))

bins = Counter((pc - lo) // win for pc in flash)
for k, c in bins.most_common(top):
    base = lo + k * win
    inside = Counter()
    for pc in flash:
        if base <= pc < base + win:
            i = bisect.bisect_right(addrs, pc) - 1
            inside[syms[i][1]] += 1
    names = ", ".join("%s %d" % (s, c2) for s, c2 in inside.most_common(4))
    print("%5.2f%%  0x%08x+%d  %s" % (100.0 * c / max(n, 1), base, win, names))
