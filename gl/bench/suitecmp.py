#!/usr/bin/env python3
"""suitecmp.py BEFORE AFTER - compare two tools/glref suite runs
(artifacts/gl/<run>/report.json): per app the verdict, and per frame the
tolerant-bad % and strict-bad %. Exit 1 if any verdict is worse or any
frame's tolerant-bad % grew (phase 3a correctness bar: no regression
against artifacts/gl/phase2/SUITE.md 10.2). s31, MIT."""
import json, sys
RANK = {'EXACT': 0, 'PASS': 1, 'FAIL': 2, 'MISSING-SYMBOL': 2, 'ERROR': 3,
        'TIMEOUT': 3, 'CRASH': 4, 'CAPTURE-FAILED': 4}
def load(p):
    r = json.load(open(p))
    v = {s['app']: s['verdict'] for s in r['summary']}
    f = {}
    for row in r['rows']:
        c = row.get('cmp') or {}
        f[(row['app'], row['frame'])] = (c.get('tolerant_bad_pct'), c.get('strict_bad_pct'))
    return v, f
bv, bf = load(sys.argv[1]); av, af = load(sys.argv[2])
bad = 0
for app in bv:
    a = av.get(app, 'MISSING')
    worse = RANK.get(a, 9) > RANK.get(bv[app], 9)
    fr = sorted(k[1] for k in bf if k[0] == app)
    cells = []
    for fn in fr:
        b = bf[(app, fn)]; x = af.get((app, fn), (None, None))
        if b[0] is None or x[0] is None:
            cells.append('f%s -' % fn); continue
        d = x[0] - b[0]
        if d > 1e-9: worse = True
        cells.append('f%s %.3f->%.3f (strict %.3f->%.3f)' % (fn, b[0], x[0], b[1], x[1]))
    tag = 'WORSE' if worse else ('same' if all(bf[(app,f)] == af.get((app,f)) for f in fr) else 'ok')
    bad += worse
    print('%-18s %-14s %-14s %-5s %s' % (app, bv[app], a, tag, '; '.join(cells)))
print('suitecmp: %d apps worse' % bad)
sys.exit(1 if bad else 0)
