#!/usr/bin/env python3
"""cells.py DIFF.png [cols rows] - tolerant-bad pixels per cell of a
glx_raster page, from compare.py's diff image (red = tolerant-bad).
Prints an 8x6 table of percentages (of the 40x40 cell). s31, MIT."""
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 8
rows = int(sys.argv[3]) if len(sys.argv) > 3 else 6
cw, ch = im.width // cols, im.height // rows
px = im.load()
for r in range(rows):
    line = []
    for c in range(cols):
        bad = 0
        for y in range(r * ch, (r + 1) * ch):
            for x in range(c * cw, (c + 1) * cw):
                R, G, B = px[x, y]
                if R > 200 and G < 60 and B < 60:
                    bad += 1
        line.append('%5.1f' % (100.0 * bad / (cw * ch)))
    print('row %d: %s' % (r, ' '.join(line)))
