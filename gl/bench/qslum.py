#!/usr/bin/env python3
"""qslum.py DIR [DIR2 ...] - the DARKNESS.md measure over a compare-qs.sh
output: per counted frame, the mean Rec.601 luminance of the view (rows
0-191 of the 320x240 frame: above QuakeSpasm's status bar) and of the whole
frame, ours / Mesa, and the share of pixels exactly Mesa's. Prints one
line per frame and the means; with several DIRs, one summary line each.
s31, MIT."""
import sys, os
from PIL import Image


def lum(img, y0, y1):
    px = img.load()
    w = img.size[0]
    s = 0.0
    for y in range(y0, y1):
        for x in range(w):
            r, g, b = px[x, y][:3]
            s += 0.299 * r + 0.587 * g + 0.114 * b
    return s / ((y1 - y0) * w)


def one(d, verbose):
    frames = sorted(int(f[1:-4]) for f in os.listdir(os.path.join(d, 'ours')) if f.endswith('.png') and f.startswith('f'))
    rows = []
    for f in frames:
        o = Image.open(os.path.join(d, 'ours', 'f%d.png' % f)).convert('RGB')
        m = Image.open(os.path.join(d, 'mesa', 'f%d.png' % f)).convert('RGB')
        vo, vm = lum(o, 0, 192), lum(m, 0, 192)
        ao, am = lum(o, 0, 240), lum(m, 0, 240)
        op, mp = o.load(), m.load()
        ex = sum(1 for y in range(240) for x in range(320) if op[x, y] == mp[x, y]) / 768.0
        rows.append((f, vo, vm, ao, am, ex))
        if verbose:
            print('f%-4d view ours %6.2f mesa %6.2f ratio %.4f | frame %6.2f %6.2f %.4f | exact %5.1f%%' %
                  (f, vo, vm, vo / vm if vm else 0, ao, am, ao / am if am else 0, ex))
    n = len(rows)
    mv = sum(r[1] for r in rows) / n; mm = sum(r[2] for r in rows) / n
    rat = sum(r[1] / r[2] for r in rows if r[2]) / n
    lo = min(r[1] / r[2] for r in rows if r[2]); hi = max(r[1] / r[2] for r in rows if r[2])
    ex = sum(r[5] for r in rows) / n
    print('%s: %d frames, view luminance ours %.2f mesa %.2f, ratio mean %.4f (min %.4f max %.4f), exact pixels %.1f%%' %
          (d, n, mv, mm, rat, lo, hi, ex))


a = sys.argv[1:]
for d in a:
    one(d, len(a) == 1)
