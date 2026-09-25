#!/usr/bin/env python3
"""glref compare: score one captured frame against its reference.

    compare.py REF.png TEST.png [--diff OUT.png] [--tol 16] [--max-bad 1.0]
               [--json OUT.json]

Two scores, both reported:

  strict    a pixel is bad if any channel differs from the same pixel in the
            other image by more than --tol.
  tolerant  a pixel is bad only if NO pixel in the 3x3 neighbourhood of the
            same position in the other image is within --tol, checked in both
            directions (REF->TEST and TEST->REF; a pixel is bad if either
            direction fails). This forgives a one-pixel shift of an edge -
            Mesa and TinyGL use different rasterisation rules (top-left fill
            convention, sub-pixel precision, span start rounding), so a
            polygon edge legitimately lands one pixel apart - but it does NOT
            forgive a wrong colour, a missing polygon, a wrong texture or a
            flipped image.

Why --tol 16 (of 255): the panel and the Xvfb screen are RGB565. One LSB of a
5-bit channel is 8.2 levels after expansion to 8 bits and one of the 6-bit
green is 4.0. Mesa rounds to nearest while TinyGL's fixed-point Gouraud
stepping truncates, and either may or may not dither, so an honest match can
be one 5-bit LSB off from rounding and one more from interpolation error:
2 x 8 = 16. More than that is a different colour, not a different rounding.

Why --max-bad 1.0 (% of pixels, tolerant): after edge forgiveness what is
left between two correct renderers is lighting and specular precision
(TinyGL evaluates the same per-vertex GL lighting in float, Mesa in its own
float path; highlights differ by a few pixels at their rim) and texture
sampling phase. On a 320x240 frame 1% is 768 pixels - several highlight rims
- while a single missing gear tooth, a wrong texenv mode or an unlit object
costs far more than that. Both thresholds are flags so a stricter gate can
be run.

The verdict is EXACT (bit-identical), PASS or FAIL; exit code 0 for EXACT and
PASS, 1 for FAIL, 2 for unusable input (missing file, size mismatch).

The diff image shows the reference in grey with tolerant-bad pixels in red
and strict-only (edge) differences in yellow.
"""
import argparse
import json
import sys

from PIL import Image

try:
    import numpy as np
except ImportError:  # the rig has numpy; keep a slow path for bare hosts
    np = None


def load(path):
    im = Image.open(path).convert("RGB")
    return im


def score_numpy(a, b, tol):
    A = np.asarray(a, dtype=np.int16)
    B = np.asarray(b, dtype=np.int16)
    h, w, _ = A.shape
    d = np.abs(A - B).max(axis=2)
    strict_bad = d > tol

    def nearest(X, Y):
        # for every pixel of X, the smallest max-channel difference to any
        # pixel of Y in its 3x3 neighbourhood (edge-replicated)
        Yp = np.pad(Y, ((1, 1), (1, 1), (0, 0)), mode="edge")
        best = np.full((h, w), 32767, dtype=np.int16)
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                s = Yp[dy:dy + h, dx:dx + w]
                best = np.minimum(best, np.abs(X - s).max(axis=2))
        return best

    tol_bad = (nearest(A, B) > tol) | (nearest(B, A) > tol)
    return {
        "max_err": int(d.max()),
        "mean_abs_err": float(np.abs(A - B).mean()),
        "strict_bad": int(strict_bad.sum()),
        "tolerant_bad": int(tol_bad.sum()),
        "exact": bool((d == 0).all()),
    }, strict_bad, tol_bad


def score_python(a, b, tol):
    w, h = a.size
    pa, pb = a.load(), b.load()

    def md(p, q):
        return max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))

    strict_bad = [[False] * w for _ in range(h)]
    tol_bad = [[False] * w for _ in range(h)]
    max_err = 0
    tot = 0
    for y in range(h):
        for x in range(w):
            e = md(pa[x, y], pb[x, y])
            tot += sum(abs(pa[x, y][c] - pb[x, y][c]) for c in range(3))
            max_err = max(max_err, e)
            if e > tol:
                strict_bad[y][x] = True
                for X, Y in ((pa, pb), (pb, pa)):
                    ok = False
                    for yy in range(max(0, y - 1), min(h, y + 2)):
                        for xx in range(max(0, x - 1), min(w, x + 2)):
                            if md(X[x, y], Y[xx, yy]) <= tol:
                                ok = True
                                break
                        if ok:
                            break
                    if not ok:
                        tol_bad[y][x] = True
    sb = sum(map(sum, strict_bad))
    tb = sum(map(sum, tol_bad))
    return {
        "max_err": max_err,
        "mean_abs_err": tot / (w * h * 3.0),
        "strict_bad": sb,
        "tolerant_bad": tb,
        "exact": max_err == 0,
    }, strict_bad, tol_bad


def write_diff(a, strict_bad, tol_bad, path):
    g = a.convert("L").point(lambda v: 40 + v * 100 // 255).convert("RGB")
    px = g.load()
    w, h = g.size
    for y in range(h):
        for x in range(w):
            if tol_bad[y][x]:
                px[x, y] = (255, 0, 0)
            elif strict_bad[y][x]:
                px[x, y] = (255, 220, 0)
    g.save(path)


def compare(ref, test, tol=16, max_bad=1.0, diff=None):
    try:
        a, b = load(ref), load(test)
    except (OSError, FileNotFoundError) as e:
        return {"verdict": "ERROR", "why": str(e)}
    if a.size != b.size:
        return {"verdict": "ERROR",
                "why": "size mismatch: ref %dx%d, test %dx%d" % (a.size + b.size)}
    if np is not None:
        r, sbad, tbad = score_numpy(a, b, tol)
        if diff:
            sbad, tbad = sbad.tolist(), tbad.tolist()
    else:
        r, sbad, tbad = score_python(a, b, tol)
    n = a.size[0] * a.size[1]
    r.update({
        "width": a.size[0], "height": a.size[1], "tol": tol, "max_bad_pct": max_bad,
        "strict_bad_pct": 100.0 * r["strict_bad"] / n,
        "tolerant_bad_pct": 100.0 * r["tolerant_bad"] / n,
    })
    if r["exact"]:
        r["verdict"] = "EXACT"
    elif r["tolerant_bad_pct"] <= max_bad:
        r["verdict"] = "PASS"
    else:
        r["verdict"] = "FAIL"
    if diff and not r["exact"]:
        write_diff(a, sbad, tbad, diff)
        r["diff"] = diff
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("--diff")
    ap.add_argument("--tol", type=int, default=16)
    ap.add_argument("--max-bad", type=float, default=1.0)
    ap.add_argument("--json")
    o = ap.parse_args()
    r = compare(o.ref, o.test, o.tol, o.max_bad, o.diff)
    if o.json:
        with open(o.json, "w") as f:
            json.dump(r, f, indent=1)
    if r["verdict"] == "ERROR":
        print("ERROR: %s" % r["why"])
        sys.exit(2)
    print("%s  %dx%d  strict %.3f%% (%d px >%d)  tolerant %.3f%% (%d px)  max %d  mean %.2f%s" % (
        r["verdict"], r["width"], r["height"], r["strict_bad_pct"], r["strict_bad"], r["tol"],
        r["tolerant_bad_pct"], r["tolerant_bad"], r["max_err"], r["mean_abs_err"],
        ("  diff " + r["diff"]) if r.get("diff") else ""))
    sys.exit(0 if r["verdict"] in ("EXACT", "PASS") else 1)


if __name__ == "__main__":
    main()
