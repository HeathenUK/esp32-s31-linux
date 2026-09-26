#!/usr/bin/env python3
"""cmp5.py BASE_OUT NEW_OUT - phase 5: every gl/bench line of NEW against
BASE (instructions per frame and frame hash), worst first. s31, MIT."""
import re, sys, os
def load(d):
    r = {}
    for f in ['results.txt','feat.txt','pix.txt','prim.txt','geo.txt','filt.txt','p4.txt']:
        p = os.path.join(d, f)
        if not os.path.exists(p): continue
        for l in open(p):
            m = re.match(r'(.+?): ([\d.]+) Minsn/frame.*?fb ([0-9a-f]+)', l)
            if m: r[m.group(1)] = (float(m.group(2)), m.group(3))
    return r
a, b = load(sys.argv[1]), load(sys.argv[2])
rows = []
for k in sorted(set(a) | set(b)):
    if k not in a or k not in b:
        rows.append((0, k, 'missing in ' + ('base' if k not in a else 'new'))); continue
    d = (b[k][0] / a[k][0] - 1) * 100 if a[k][0] else 0
    h = 'same' if a[k][1] == b[k][1] else 'HASH %s->%s' % (a[k][1], b[k][1])
    rows.append((d, k, '%.4f -> %.4f M (%+.3f%%) %s' % (a[k][0], b[k][0], d, h)))
rows.sort(key=lambda r: -r[0])
for d, k, t in rows: print('%-28s %s' % (k, t))
worst = max(r[0] for r in rows) if rows else 0
nh = sum(1 for r in rows if 'HASH' in r[2])
print('cmp5: %d lines, worst %+.3f%%, %d hash changes' % (len(rows), worst, nh))
