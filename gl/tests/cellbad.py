#!/usr/bin/env python3
"""cellbad.py MESA.png OURS.png [cell] - tolerant-bad pixels (tools/glref
compare.py's rule: no pixel of the other image's 3x3 neighbourhood within
16 on every channel, either direction) per 40x40 cell, rows from the
bottom as the tests number them. s31, MIT."""
import sys
import numpy as np
from PIL import Image
A = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.int16)
B = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.int16)
C = int(sys.argv[3]) if len(sys.argv) > 3 else 40
h, w, _ = A.shape
def nearest(X, Y):
    Yp = np.pad(Y, ((1, 1), (1, 1), (0, 0)), mode='edge')
    best = np.full((h, w), 32767, dtype=np.int16)
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            best = np.minimum(best, np.abs(X - Yp[dy:dy + h, dx:dx + w]).max(axis=2))
    return best
bad = (nearest(A, B) > 16) | (nearest(B, A) > 16)
for row in range(h // C):
    y0, y1 = h - (row + 1) * C, h - row * C
    print('row %d ' % row + ' '.join('%4d' % bad[y0:y1, c * C:(c + 1) * C].sum() for c in range(w // C)))
print('total %d (%.3f%%)' % (bad.sum(), 100.0 * bad.sum() / (h * w)))
