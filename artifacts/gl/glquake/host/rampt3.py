#!/usr/bin/env python3
"""Score rampt3.c: mean output per band and channel for each implementation,
next to the exact value (float blend) truncated to 565."""
import sys
import numpy as np
from PIL import Image
x = np.arange(256.0)
def q(v, bits):
    lv = np.floor(np.clip(v, 0, 255) / 255 * ((1 << bits) - 1) + 1e-6).astype(int)
    return np.floor(lv * 255 / ((1 << bits) - 1) + 0.5)
col = np.array([215, 186, 69.0])
al = [0.3, 0.1, 0.5]
def exact(b):
    if b == 0: return np.stack([x, x, x], 1)
    if 1 <= b <= 3: return col * al[b - 1] + np.stack([x, x, x], 1) * (1 - al[b - 1])
    if b == 4: v = np.repeat(16 * np.arange(16) + 5.0, 16); return np.stack([v, v, v], 1)
    return np.tile(col * al[b - 5], (256, 1))
NAMES = ["ramp", "ramp+flash a.3", "ramp+flash a.1", "ramp+flash a.5", "flat greys 16k+5",
         "black+flash a.3", "black+flash a.1", "black+flash a.5"]
imgs = {a.split("=")[0]: np.asarray(Image.open(a.split("=")[1]).convert("RGB")).astype(float) for a in sys.argv[1:]}
print("%-18s %-22s %s" % ("band", "exact->565 R G B", "  ".join("%-6s R G B" % k for k in imgs)))
for b in range(8):
    e = exact(b)
    eq = np.stack([q(e[:, 0], 5), q(e[:, 1], 6), q(e[:, 2], 5)], 1).mean(0)
    s = "  ".join("%6.2f %6.2f %6.2f" % tuple(im[32 * b + 16].mean(0)) for im in imgs.values())
    print("%-18s %6.2f %6.2f %6.2f   %s" % (NAMES[b], *eq, s))
