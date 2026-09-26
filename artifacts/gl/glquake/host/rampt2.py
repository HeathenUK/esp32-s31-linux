#!/usr/bin/env python3
"""Score rampt2.c: per band, the mean signed error (8-bit units) of each
implementation against the exact result truncated to 565, and how many of
the 256 columns lost a whole 565 level."""
import sys
import numpy as np
from PIL import Image
NAMES = ["REPLACE nearest (ref)", "MODULATE color 1.0", "alpha 0.75 over itself",
         "LINEAR mag, texel centres", "2x-modulate lightmap L=128", "trilinear minified const",
         "Gouraud 0..1, untextured", "ONE,ONE add of black"]
def q(v, bits):  # truncate to a 565 level, expand by bit replication
    lv = np.floor(np.asarray(v, float) / 255 * ((1 << bits) - 1) + 1e-9)
    lv = (np.asarray(v).astype(int) >> (8 - bits))
    return (lv << (8 - bits)) | (lv >> (2 * bits - 8))
x = np.arange(256)
exp = {b: x for b in range(8)}
exp[5] = np.repeat(8 * np.arange(16) + 4, 16)
exp[6] = np.clip(np.round((x + 0.5) * 255 / 256), 0, 255).astype(int)
print("%-28s %-5s %9s %9s %9s %11s" % ("band", "impl", "err R", "err G", "err B", "cols -1 lvl"))
for b in range(8):
    for arg in sys.argv[1:]:
        impl, path = arg.split("=")
        a = np.asarray(Image.open(path).convert("RGB")).astype(int)[32 * b + 16]
        eR, eG = q(exp[b], 5), q(exp[b], 6)
        dR, dG, dB = a[:, 0] - eR, a[:, 1] - eG, a[:, 2] - eR
        print("%-28s %-5s %9.2f %9.2f %9.2f %11d" % (NAMES[b], impl, dR.mean(), dG.mean(), dB.mean(),
                                                   int((dR < 0).sum())))
