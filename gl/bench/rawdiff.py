#!/usr/bin/env python3
"""rawdiff.py A.raw B.raw W H - how two RGB565 bench frames differ:
pixels that differ, and the largest per-channel step (in 8-bit units).
s31, MIT."""
import sys, struct
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
w, h = int(sys.argv[3]), int(sys.argv[4])
n = 0; mx = 0; first = None
for i in range(0, min(len(a), len(b)), 2):
    pa = a[i] | a[i+1] << 8; pb = b[i] | b[i+1] << 8
    if pa != pb:
        n += 1
        ca = ((pa >> 11) << 3, ((pa >> 5) & 63) << 2, (pa & 31) << 3)
        cb = ((pb >> 11) << 3, ((pb >> 5) & 63) << 2, (pb & 31) << 3)
        d = max(abs(x - y) for x, y in zip(ca, cb)); mx = max(mx, d)
        if first is None: first = (i // 2 % w, i // 2 // w)
print('%d of %d px differ (%.4f%%), max channel step %d, first at %s' % (n, w*h, 100.0*n/(w*h), mx, first))
