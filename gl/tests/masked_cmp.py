#!/usr/bin/env python3
"""masked_cmp.py REF.png TEST.png [--tol 16] - compare.py's tolerant metric,
but ignoring pixels within 2 px of the reference's pure-red text (the GLUT
demos' glBitmap help text, plan F7, not drawn by this rasteriser work), so
what remains is the scene. Prints tolerant-bad % of the unmasked pixels.
s31, MIT."""
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); b = Image.open(sys.argv[2]).convert('RGB')
tol = int(sys.argv[sys.argv.index('--tol') + 1]) if '--tol' in sys.argv else 16
W, H = a.size
pa, pb = a.load(), b.load()
text = [[False] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        r, g, bb = pa[x, y]
        if r > 200 and g < 40 and bb < 40:
            for yy in range(max(0, y - 2), min(H, y + 3)):
                for xx in range(max(0, x - 2), min(W, x + 3)):
                    text[yy][xx] = True
def close(p, q):
    return all(abs(p[i] - q[i]) <= tol for i in range(3))
def ok(src, dst, x, y):
    for yy in range(max(0, y - 1), min(H, y + 2)):
        for xx in range(max(0, x - 1), min(W, x + 2)):
            if close(src[x, y], dst[xx, yy]):
                return True
    return False
n = bad = 0
for y in range(H):
    for x in range(W):
        if text[y][x]:
            continue
        n += 1
        if not ok(pa, pb, x, y) or not ok(pb, pa, x, y):
            bad += 1
print('%.3f%% tolerant-bad of %d unmasked pixels (%d masked as text)' % (100.0 * bad / max(n, 1), n, W * H - n))
