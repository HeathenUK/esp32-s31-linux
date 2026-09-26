#!/usr/bin/env python3
"""Score rampt.c captures: per band (lightmap L), mean output per channel
against the exact 2*x*L/255 and against the best a 565 target can do
(round to nearest 5/6-bit level, expanded by bit replication)."""
import sys
import numpy as np
from PIL import Image
import os
LM = [0] + [int(v) for v in os.environ.get("LMV", "16,32,48,64,96,128,255").split(",")]
x = np.arange(256, dtype=float)
def q565(v, bits):  # round to nearest level, expand as the capture does
    n = (1 << bits) - 1
    lv = np.clip(np.floor(v * n / 255 + 0.5), 0, n)
    return np.floor(lv * 255 / n + 0.5)
print("%-5s %-4s %8s %8s %8s %8s %8s %8s" % ("impl", "L", "exact", "ideal565", "R", "G", "B", "Y/exact"))
for arg in sys.argv[1:]:
    impl, path = arg.split("=")
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)
    for b, L in enumerate(LM):
        row = a[32 * b + 16]
        exact = x if b == 0 else np.minimum(255, 2 * x * L / 255)
        ideal = (q565(exact, 5).mean() * 2 + q565(exact, 6).mean()) / 3
        Y = row @ [0.299, 0.587, 0.114]
        print("%-5s %-4s %8.2f %8.2f %8.2f %8.2f %8.2f %8.3f" % (
            impl, "tex" if b == 0 else L, exact.mean(), ideal, row[:, 0].mean(),
            row[:, 1].mean(), row[:, 2].mean(), Y.mean() / exact.mean()))
