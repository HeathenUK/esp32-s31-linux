#!/usr/bin/env python3
"""Mean luminance (Rec.601, 0-255) of QuakeSpasm frames by region, plus a
side-by-side strip. Run in the s31-glref container:
  analyze.py <out-strip.png> <label=png> [<label=png>...]
A png may be 'board' = the board screenshot, cropped to the window client
(origin 151,81 - aligned on the HUD, mean abs 4.8 against ours f30)."""
import sys
import numpy as np
from PIL import Image

BOARD = "/src/artifacts/gl/stage6/quakespasm-first.jpg"
REGIONS = {  # (y0, y1, x0, x1) in the 320x240 window
    "full": (0, 240, 0, 320),
    "view": (0, 192, 0, 320),       # 3D view (status bar is 48 rows)
    "view_low": (130, 192, 0, 320),  # below the console in the plain f30 frames
    "hud": (192, 240, 0, 320),
}

def load(p):
    if p == "board":
        return Image.open(BOARD).convert("RGB").crop((151, 81, 471, 321))
    return Image.open(p).convert("RGB")

def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]

out = sys.argv[1]
imgs = []
print("%-22s" % "arm" + "".join("%10s" % r for r in REGIONS))
for arg in sys.argv[2:]:
    label, path = arg.split("=", 1)
    im = load(path)
    a = np.asarray(im).astype(float)
    L = lum(a)
    print("%-22s" % label + "".join("%10.2f" % L[y0:y1, x0:x1].mean()
                                    for (y0, y1, x0, x1) in REGIONS.values()))
    imgs.append(im)
w = sum(i.width for i in imgs)
strip = Image.new("RGB", (w, max(i.height for i in imgs)))
x = 0
for i in imgs:
    strip.paste(i, (x, 0)); x += i.width
strip.save(out)
