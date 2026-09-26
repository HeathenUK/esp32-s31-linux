#!/usr/bin/env python3
"""Score rampt5.c: mean luminance per lightmap band for each arm, as a ratio
to the exact 2*T*L/255 (T = the 16 world-texture greys, 67..187)."""
import sys
import numpy as np
from PIL import Image
LM = [20, 39, 55, 71, 100, 135, 199, 255]
T = np.repeat(8 * np.arange(16) + 67.0, 16)
arms = [(a.split("=")[0], np.asarray(Image.open(a.split("=")[1]).convert("RGB")).astype(float)) for a in sys.argv[1:]]
print("%-4s %8s " % ("L", "exact") + " ".join("%13s" % n for n, _ in arms))
for b, L in enumerate(LM):
    ex = np.minimum(255, 2 * T * L / 255).mean()
    vals = [(im[32 * b + 16] @ [0.299, 0.587, 0.114]).mean() for _, im in arms]
    print("%-4d %8.2f " % (L, ex) + " ".join("%6.2f (%.3f)" % (v, v / ex) for v in vals))
