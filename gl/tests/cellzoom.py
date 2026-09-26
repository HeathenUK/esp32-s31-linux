#!/usr/bin/env python3
"""cellzoom.py DIR PAGE OUT.png col,row [col,row ...] - the cells of a page
from DIR/mesa and DIR/ours side by side (mesa | ours | |difference| x4),
scaled 6x. s31, MIT."""
import sys
import numpy as np
from PIL import Image
d, page, out = sys.argv[1:4]
cells = [tuple(int(v) for v in a.split(',')) for a in sys.argv[4:]]
A = Image.open('%s/mesa/%s.f2.png' % (d, page)).convert('RGB')
B = Image.open('%s/ours/%s.f2.png' % (d, page)).convert('RGB')
h = A.size[1]
rows = []
for col, row in cells:
    box = (col * 40, h - (row + 1) * 40, (col + 1) * 40, h - row * 40)
    a, b = A.crop(box), B.crop(box)
    dd = np.clip(np.abs(np.asarray(a).astype(int) - np.asarray(b).astype(int)) * 4, 0, 255).astype(np.uint8)
    strip = Image.new('RGB', (124, 40), (255, 255, 255))
    strip.paste(a, (0, 0)); strip.paste(b, (42, 0)); strip.paste(Image.fromarray(dd), (84, 0))
    rows.append(strip)
img = Image.new('RGB', (124, 42 * len(rows)), (255, 255, 255))
for i, r in enumerate(rows):
    img.paste(r, (0, i * 42))
img.resize((img.size[0] * 6, img.size[1] * 6), Image.NEAREST).save(out)
