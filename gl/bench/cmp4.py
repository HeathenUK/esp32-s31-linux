#!/usr/bin/env python3
"""cmp4.py BASE.log NEW.log [filter] - phase 4: per bench line, M insn/frame
before -> after, the change, and whether the frame hash is the same."""
import re, sys
def load(p):
    d = {}
    for l in open(p):
        m = re.match(r'(.+?): ([\d.]+) Minsn/frame.*fb (\w+)', l)
        if m: d[m.group(1)] = (float(m.group(2)), m.group(3))
    return d
a = load(sys.argv[1]); b = load(sys.argv[2])
flt = sys.argv[3] if len(sys.argv) > 3 else ''
for k in a:
    if k in b and flt in k:
        x, y = a[k], b[k]
        print('%-28s %.4f -> %.4f %+6.2f%% %s' % (k, x[0], y[0], (y[0] / x[0] - 1) * 100 if x[0] else 0,
              'same' if x[1] == y[1] else 'HASH ' + x[1] + '->' + y[1]))
