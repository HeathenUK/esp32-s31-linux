#!/usr/bin/env python3
"""precscore.py MESA.png OURS.png [--band N] - score gl/tests/glx_prec.c.

Per band (30 bands of 8 rows, values v = 0..255 in columns 0..255, the
band's rows 1-6 from its top): the share of pixels exactly equal to Mesa's,
the mean signed difference ours - Mesa per channel (8-bit units of the 565
expansion) and the mean absolute difference in 565 levels. --band N lists
v, Mesa's and ours' pixel. s31, MIT."""
import sys
from PIL import Image

NAMES = {
    0: 'glClearColor(v/255)', 1: 'glColor3f(v/255) flat', 2: 'glColor3ub(v) flat',
    3: 'smooth ramp 0..1', 4: 'RGB texel v REPLACE', 5: 'RGBA texel v REPLACE',
    6: 'RGB texel v MODULATE 191', 7: 'RGB texel v MODULATE 100',
    8: 'case 3: T180 then lightmap v DST_COLOR,SRC_COLOR', 9: 'case 3: T90 then lightmap v',
    10: 'v at alpha 28 over black', 11: 'v at alpha 128 over 100', 12: 'v ONE,ONE over 40',
    13: 'glDrawPixels RGB ubyte v', 14: 'glDrawPixels RGBA float v/255',
    15: 'LUMINANCE v REPLACE', 16: 'LUMINANCE_ALPHA (v, 255-v) REPLACE', 17: 'INTENSITY v MODULATE white',
    18: 'case 1: T180 x lightmap v COMBINE x2', 19: 'case 1: T90 x lightmap v COMBINE x2',
    20: 'alias: COMBINE texel v x primary 160 x2', 21: 'GL_LINEAR format-4 texel v at centres',
    22: 'LINEAR 32-texel ramp magnified 8x', 23: 'ALPHA texel v MODULATE white over black',
    24: 'smooth ramp 0..64', 25: 'white at alpha v/255 (float) over black',
    26: 'glClearColor((v+0.75)/255)', 27: 'glColor3f((v+0.75)/255) flat',
    28: 'RGBA texel (v, a v) MODULATE white over 200', 29: 'flash (215,186,69) a28 over v/4',
    30: 'LINEAR ramp magnified 7.3x, offset', 31: 'LINEAR two rows at t 0.3', 32: 'trilinear 200/50 lambda v/256',
    33: 'RGB texel v MODULATE smooth 255->64', 34: 'GL_ADD texel v + 100', 35: 'GL_DECAL RGBA (v, a v) over 60',
    36: 'GL_BLEND L v, env 200, colour 40', 37: 'MODULATE 150 then ONE,ONE again', 38: 'ZERO,SRC_COLOR lightmap v over T180',
    39: 'linear fog white -> 80 by v/255',
    40: 'COMBINE ADD_SIGNED texel v, primary 90', 41: 'COMBINE INTERPOLATE v, 90 by 170', 42: 'COMBINE SUBTRACT v - 90 x2',
    43: 'COMBINE MODULATE v x 90 x4', 44: 'COMBINE ADD_SIGNED x2', 45: 'COMBINE INTERPOLATE x2',
    46: 'glDrawPixels RGBA float (v+0.75)/255', 47: 'glClearColor 0.25 | (0.5,0.125,0.75)',
}


def lv(c, ch):
    return c >> 2 if ch == 1 else c >> 3


def main():
    a = sys.argv[1:]
    band = None
    if '--band' in a:
        k = a.index('--band')
        band = int(a[k + 1])
        del a[k:k + 2]
    m = Image.open(a[0]).convert('RGB')
    o = Image.open(a[1]).convert('RGB')
    mp, op = m.load(), o.load()
    print('# band: exact%  mean(ours-mesa) R G B  mean|levels|  name')
    tot_ex = tot = 0
    for b in range(48):
        ex = n = 0
        ds = [0, 0, 0]
        dl = 0
        for y in range(b * 8 + 2, b * 8 + 8):
            for x in range(256):
                mc, oc = mp[x, y], op[x, y]
                n += 1
                if mc == oc:
                    ex += 1
                for ch in range(3):
                    ds[ch] += oc[ch] - mc[ch]
                    dl += abs(lv(oc[ch], ch) - lv(mc[ch], ch))
        tot_ex += ex
        tot += n
        print('%2d: %6.2f%%  %+6.2f %+6.2f %+6.2f  %5.3f  %s' % (
            b, 100.0 * ex / n, ds[0] / n, ds[1] / n, ds[2] / n, dl / (3.0 * n), NAMES.get(b, '')))
        if band == b:
            y = b * 8 + 4
            for x in range(256):
                print('   v %3d  mesa %3d %3d %3d  ours %3d %3d %3d%s' % ((x,) + mp[x, y] + op[x, y] +
                      ('' if mp[x, y] == op[x, y] else '  *',)))
    print('all: %.2f%% exact' % (100.0 * tot_ex / tot))


main()
