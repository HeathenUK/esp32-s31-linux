#!/usr/bin/env python3
"""ab-summary.py [prefix=kf-] : fps and per-frame L1 next-level refills of each
GLQuake arm in artifacts/gl/glquake/arms/<prefix><X><n>-*/r1.txt (GQ_CC=1).
Columns: DBUS0/DBUS1 = shared D-cache refills by hart0/hart1, IBUS0/IBUS1 =
I-refills of hart0 (Linux CPU1) / hart1 (Linux CPU0), in thousands per frame
over the whole run (launch to end)."""
import glob, re, sys, statistics
pre = sys.argv[1] if len(sys.argv) > 1 else "kf-"
rows = {}
for d in sorted(glob.glob("artifacts/gl/glquake/arms/%s*" % pre)):
    m = re.match(r".*/%s([A-Z])(\d+)-" % re.escape(pre), d)
    if not m: continue
    try: t = open(d + "/r1.txt", errors="replace").read()
    except OSError: continue
    r = re.search(r"RESULT\s+(\d+) frames ([\d.]+) seconds\s+([\d.]+) fps", t)
    if not r: continue
    k = re.search(r"#(\d+) SMP", t)
    cc = {}
    for l in t.splitlines():
        p = l.split()
        if len(p) == 8 and p[0] == "CC": cc[p[1]] = [int(x, 16) for x in p[2:]]
    fr = int(r.group(1)); fps = float(r.group(3))
    ref = [(cc["end"][i] - cc["0"][i]) / fr / 1000 for i in (0, 2, 4, 5)] if "end" in cc else [0]*4
    rows.setdefault(m.group(1), []).append((int(m.group(2)), fps, k.group(1) if k else "?", ref))
for x, rs in sorted(rows.items()):
    for n, fps, k, ref in sorted(rs):
        print("%s%-2d #%s %5.2f fps  D0 %5.1f D1 %5.1f I0 %5.1f I1 %5.1f k/frame" % (x, n, k, fps, *ref))
    f = [r[1] for r in rs]; i1 = [r[3][3] for r in rs]
    print("  %s: n=%d fps median %.2f mean %.2f range %.1f-%.1f | I1 k/frame mean %.1f range %.1f-%.1f" % (
        x, len(f), statistics.median(f), statistics.mean(f), min(f), max(f), statistics.mean(i1), min(i1), max(i1)))
