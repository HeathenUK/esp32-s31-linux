#!/usr/bin/env python3
"""qsrhash.py A.txt B.txt - two qsreplay.sh outputs (OUT/qsr/LABEL.txt) of the
same trace: how many full-frame hashes (qsrf lines) are identical, and the
windows' M instructions/frame A -> B. The bit-identity check of phase 6
(artifacts/gl/phase6/GEOMETRY.txt). s31, MIT."""
import sys
def load(p):
    h = {}; w = []
    for l in open(p):
        f = l.split()
        if l.startswith('qsrf ') and len(f) >= 5:
            h[f[1]] = f[4]
        if l.startswith('qsr ') and ' window ' in l and '-null' not in f[1]:
            w.append(float(f[6]))
    return h, w
a, wa = load(sys.argv[1]); b, wb = load(sys.argv[2])
same = sum(1 for k in a if k in b and a[k] == b[k])
diff = [k for k in a if k in b and a[k] != b[k]]
print('frames %d same %d differ %d %s' % (len(a), same, len(diff), ' '.join(diff[:8])))
print('M/frame  ' + '  '.join('%.4f -> %.4f (%+.2f%%)' % (x, y, 100 * (y / x - 1)) for x, y in zip(wa, wb)))
