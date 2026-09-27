#!/usr/bin/env python3
"""attr.py <nm -n output> <tbprof.txt> <samples> : instructions per sample by function"""
import sys, bisect
syms = []; labels = []
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 3 and p[1] in "tTwW" and not p[2].startswith(("Lm_", "La_")):
        syms.append((int(p[0], 16), p[2]))
    elif len(p) == 3 and p[2].startswith(".L") and p[1] in "tT":
        labels.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [a for a, _ in syms]
tot = {}; n = float(sys.argv[3])
for l in open(sys.argv[2]):
    pc, c = l.split(); pc = int(pc, 16); c = int(c)
    i = bisect.bisect_right(addrs, pc) - 1
    name = syms[i][1] if i >= 0 else "?"
    tot[name] = tot.get(name, 0) + c
all_ = sum(tot.values())
for k, v in sorted(tot.items(), key=lambda x: -x[1])[:16]:
    print("%-22s %9.1f  %5.1f%%" % (k, v / n, 100.0 * v / all_))
print("%-22s %9.1f" % ("TOTAL", all_ / n))
cnt = {}
for l in open(sys.argv[2]):
    pc, c = l.split(); cnt[int(pc, 16)] = int(c)
ent = {a: nm for a, nm in syms}
for a, nm in syms:
    if nm.startswith("s31v2_") and cnt.get(a):
        print("calls/sample %-16s %7.2f" % (nm, cnt[a] / n))
for a, nm in labels:
    if nm.startswith((".Lm_", ".La_")) and cnt.get(a):
        print("  path %-14s %7.2f/sample" % (nm, cnt[a] / n))
