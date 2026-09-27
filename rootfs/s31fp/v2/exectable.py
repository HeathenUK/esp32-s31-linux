#!/usr/bin/env python3
"""exectable.py <board-b4.txt>: ms/exec per app and arm (mean of rounds), and the delta to plain"""
import re, sys, collections
d = collections.defaultdict(list)
for l in open(sys.argv[1]):
    m = re.match(r'r\d (.+?) \| (.+?)\s+([\d.]+) ms/exec', l)
    if m: d[(m.group(1).strip(), m.group(2).strip())].append(float(m.group(3)))
apps = []
for a, _ in d:
    if a not in apps: apps.append(a)
arms = ["plain", "v1 (old)", "loaded, off", "scan", "cache"]
print("%-32s" % "app" + "".join("%13s" % a for a in arms))
for a in apps:
    p = sum(d[(a, "plain")]) / len(d[(a, "plain")])
    row = "%-32s" % a
    for arm in arms:
        v = d.get((a, arm))
        row += "%13s" % ("-" if not v else ("%.1f" % p if arm == "plain" else "%+.1f" % (sum(v) / len(v) - p)))
    print(row)
