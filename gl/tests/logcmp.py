#!/usr/bin/env python3
"""logcmp.py MESA.log OURS.log [prefixes] - compare the "query", "read",
"error" and "glu" lines two runs of a test printed, in order, with a
numeric tolerance: 'read' values are pixels (bytes +-16, the tolerance of
tools/glref/compare.py; floats +-0.07; "(5-bit)" lines +-1), 'query' and
'glu' floats +-0.01 relative, integers exact, 'error' lines exact.
Prints each difference; exit 1 if any. s31, MIT."""
import re
import sys

NUM = re.compile(r'^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$')


def lines(path, prefixes):
    out = []
    for ln in open(path, errors='replace'):
        ln = ln.rstrip('\n')
        if any(ln.startswith(p) for p in prefixes):
            out.append(ln)
    return out


def tok_ok(a, b, kind, five):
    if a == b:
        return True
    if not (NUM.match(a) and NUM.match(b)):
        return False
    fa, fb = float(a), float(b)
    isint = '.' not in a and '.' not in b and 'e' not in a.lower()
    if kind == 'read':
        if isint:
            return abs(fa - fb) <= (1 if five else 16)
        return abs(fa - fb) <= 0.07
    if kind in ('query', 'glu'):
        if isint:
            return False
        return abs(fa - fb) <= 0.01 * max(1.0, abs(fa))
    return False


def main():
    prefixes = sys.argv[3].split(',') if len(sys.argv) > 3 else ['query', 'read', 'error', 'glu']
    a, b = lines(sys.argv[1], prefixes), lines(sys.argv[2], prefixes)
    bad = 0
    if len(a) != len(b):
        print('line count differs: mesa %d, ours %d' % (len(a), len(b)))
        bad += 1
    for la, lb in zip(a, b):
        kind = la.split()[0]
        ta, tb = la.replace(':', ' : ').split(), lb.replace(':', ' : ').split()
        five = '5-bit' in la
        ok = len(ta) == len(tb) and all(tok_ok(x, y, kind, five) for x, y in zip(ta, tb))
        if not ok:
            bad += 1
            print('- mesa: ' + la)
            print('+ ours: ' + lb)
    print('%d lines compared, %d differ' % (min(len(a), len(b)), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
