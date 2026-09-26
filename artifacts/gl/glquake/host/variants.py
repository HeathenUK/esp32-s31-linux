#!/usr/bin/env python3
"""Luminance of every arm per frame, as a ratio to Mesa with the same
(no-combiner) QuakeSpasm path, and per-channel mean signed error."""
import os
import numpy as np
from PIL import Image
D = "/src/artifacts/gl/glquake/dark"
def L(a): return a @ [0.299, 0.587, 0.114]
def ld(p): return np.asarray(Image.open(p).convert("RGB")).astype(float)
ARMS = [("mesa-full", "mesa/mesa-full.f%d.png"), ("ours(frozen)", "ours/ours.f%d.png"),
        ("exp-base", "exp-base/ours/ours.f%d.png"), ("exp-round", "exp-round/ours/ours.f%d.png"),
        ("exp-lmround", "exp-lmround/ours/ours.f%d.png")]
print("view = rows 0-191; ratio = view luminance / mesa-nocomb; dRGB = mean signed error vs mesa-nocomb")
for f in (250, 400, 550, 700, 850, "p48"):
    if f == "p48":
        ref = ld(D + "/mesa/plain-mesa-nocomb.f48.png"); arms = [(n, p.replace("ours.f%d", "plain-ours.f%d").replace("mesa-full.f%d", "plain-mesa-full.f%d") % 48) for n, p in ARMS]
    else:
        ref = ld(D + "/mesa/mesa-nocomb.f%d.png" % f); arms = [(n, p % f) for n, p in ARMS]
    r = L(ref[:192]).mean()
    print("frame %-4s mesa-nocomb view %.2f" % (f, r))
    for n, p in arms:
        if not os.path.exists(D + "/" + p): continue
        a = ld(D + "/" + p)[:192]
        d = (a - ref[:192]).reshape(-1, 3).mean(0)
        print("   %-13s %6.2f  ratio %.3f  dRGB %+5.2f %+5.2f %+5.2f" % (n, L(a).mean(), L(a).mean() / r, *d))
