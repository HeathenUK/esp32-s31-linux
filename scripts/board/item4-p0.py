#!/usr/bin/env python3
"""item4-p0.py <log>: parse scripts/board/item4-p0.sh output.

Per arm: faultlat's major-fault times (MAJ line) against the sdtrace ring's
READ requests of the touch phase (deduped by seq across the two windows).
Prints fault p50/p90, ring read total p50/p90, the per-fault difference in
means and medians (the software above the driver = item 4's ceiling), and
the ring's gap (previous end -> this submit) for reads.
"""
import re
import statistics as st
import sys

HOPS = ["prep", "i2c", "c2d", "d2x", "i2bh", "x2bh", "bh2st", "stophw",
        "stop", "st2rd", "indrv", "rd2po", "po2bl", "bl2end", "total", "gap"]


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0


arms, cur = [], None
for line in open(sys.argv[1], errors="replace"):
    line = line.rstrip("\r\n")
    m = re.match(r"ARM (\S+) run (\d+)", line)
    if m:
        cur = {"label": m.group(1), "run": int(m.group(2)), "maj": [], "ring": {}}
        arms.append(cur)
        continue
    if cur is None:
        continue
    if line.startswith("MAJ "):
        cur["maj"] = [int(x) for x in line.split()[1:]]
    m = re.match(r"(\d+) (\d+) (\d+) ([RW]) (\d+)/(\d+)/(\d+) (\d+) (\d+) \|((?: \d+){16})$", line)
    if m:
        h = dict(zip(HOPS, map(int, m.group(10).split())))
        h.update(op=int(m.group(2)), blk=int(m.group(3)), rw=m.group(4),
                 cpu=m.group(5) + "/" + m.group(6) + "/" + m.group(7))
        cur["ring"][int(m.group(1))] = h

print("arm       run  faults p50/p90/mean us | ring reads n blk  total p50/p90/mean | above-driver med/mean | gap p50 | c2d p50 | writes")
agg = {}
for a in arms:
    reads = [r for r in a["ring"].values() if r["rw"] == "R"]
    writes = [r for r in a["ring"].values() if r["rw"] == "W"]
    tot = [r["total"] for r in reads]
    f = a["maj"]
    if not f or not tot:
        print(a["label"], a["run"], "no data")
        continue
    blks = sorted(set(r["blk"] for r in reads))
    dmed = st.median(f) - st.median(tot)
    dmean = st.mean(f) - st.mean(tot)
    agg.setdefault(a["label"], []).append((dmed, dmean))
    print(f"{a['label']:8s} {a['run']:3d}  {len(f):3d} {st.median(f):6.0f}/{pct(f,.9):5d}/{st.mean(f):5.0f} | "
          f"{len(reads):3d} {blks} {st.median(tot):5.0f}/{pct(tot,.9):5d}/{st.mean(tot):5.0f} | "
          f"{dmed:5.0f}/{dmean:5.0f} | {st.median([r['gap'] for r in reads]):5.0f} | "
          f"{st.median([r['c2d'] for r in reads]):4.0f} | {len(writes)}")
for k, v in agg.items():
    print(f"AGG {k}: above-driver median-diff {[round(x[0]) for x in v]} mean-diff {[round(x[1]) for x in v]}")
