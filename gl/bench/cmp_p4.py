#!/usr/bin/env python3
"""cmp_p4.py BASE.log FILLERS.log NOW.log [names...] - phase 4 bench
comparison: M instructions per frame of each bench line against the
phase 3a final (BASE) and the fillers' final (FILLERS), with the frame hash
check. Default names: the phase 4 bar (gears, glxgears-like, texobj,
teapotf at 320x240 and 640x400). s31, MIT."""
import re, sys
def load(p):
    d = {}
    for l in open(p):
        m = re.match(r'(.+?): ([\d.]+) Minsn.*fb (\w+)', l)
        if m: d[m.group(1)] = (float(m.group(2)), m.group(3))
    return d
b, f, n = load(sys.argv[1]), load(sys.argv[2]), load(sys.argv[3])
names = sys.argv[4:] or ['gears 320x240', 'gears 640x400', 'glxgears 300x300',
    'glxgears-2buf 300x300', 'texobj 320x240', 'texobj 640x400',
    'teapot 320x240', 'teapot 640x400']
if names == ['all']: names = sorted(n)
worst = 0.0
print('| case | phase 3a final | fillers final | now | vs 3a final | vs fillers | hash |')
print('|---|---|---|---|---|---|---|')
for k in names:
    if k not in n: continue
    nb = b.get(k, (0, '')); nf = f.get(k, (0, ''))
    d3 = 100 * (n[k][0] / nb[0] - 1) if nb[0] else float('nan')
    df = 100 * (n[k][0] / nf[0] - 1) if nf[0] else float('nan')
    h = 'same' if n[k][1] == nb[1] or not nb[1] else 'DIFF %s vs %s' % (n[k][1], nb[1])
    if nb[0] and d3 > worst: worst = d3
    print('| %s | %.4f | %.4f | %.4f | %+.3f%% | %+.3f%% | %s |' % (k, nb[0], nf[0], n[k][0], d3, df, h))
print('worst vs phase 3a final: %+.3f%%' % worst)
