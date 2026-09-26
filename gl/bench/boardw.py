#!/usr/bin/env python3
# boardw.py CALIB_PROF PROF... - a board-weighted PROJECTION of prof.py
# profiles (review 3a M1). NOT a measurement.
#
# Calibration: the board's hart-1 PC samples of stock glxgears 300x300 at
# 31 fps (2026-09-26, the shipped libGL = HEAD 7f822b6 = gl/bench/base3a),
# as a share of all CPU0 samples, against CALIB_PROF = prof.py of
# g_glxgears_300 built from that same tree. share x 32.3 ms x 320 MHz /
# instructions = cycles per instruction on the board, per function. The
# clear is modelled per STORE (bandwidth-bound: 90.0k 4-byte stores took
# ~1.03 M cycles), not per instruction. Functions the board profile does not
# list take DEFAULT_CPI (the geometry functions' ~1.25); soft-double and
# libm (sqrt, __adddf3 ...) are not in the board profile at all, so their
# share is the most uncertain part (flagged "unlisted").
#   PROF: prof.py outputs (per-function instructions per frame, and the fill
#   routine's stores in the "stores by function" line)
# Prints, per PROF: projected libGL cycles and ms per frame and the change
# against the first PROF, split by class. s31, MIT.
import sys, re
FRAME_MS, MHZ = 1000.0 / 31, 320.0
BOARD = {  # share of CPU0 samples (the task's board evidence)
    'memset_16': 10.0, 'ZB_fillTriangleFlat_lt': 8.4, 'glopVertex': 3.9,
    'gl_shade_vertex': 2.1, 'ztri_setup': 1.0, 'glopCallList': 0.8,
    'gl_transform_to_viewport': 0.7, 'ZB_fillTriangleSmooth_lt': 0.7,
    'gl_V3_Norm': 0.3}
DEFAULT_CPI = 1.25
# the vertex op was renamed by phase 3a (glopVertex -> gl_vertex4f*); both
# take glopVertex's CPI
ALIAS = {'gl_vertex4f.part.0': 'glopVertex', 'gl_vertex4f': 'glopVertex',
         'gl_vertex_indexed': 'glopVertex'}
FILL = ('memset_16', 'ZB_fill16')
def load(f):
    fn, st = {}, {}
    for l in open(f):
        m = re.match(r'\s+(\S+)\s+(\d+) /frame', l)
        if m: fn[m.group(1)] = int(m.group(2))
        if l.startswith('memory per frame'):
            for k, v in re.findall(r'(\S+) (\d+)(?:,|$)', l.split('stores by function:')[1]):
                st[k] = int(v)
    return fn, st
cal_fn, cal_st = load(sys.argv[1])
cyc_total = FRAME_MS * 1e-3 * MHZ * 1e6
cpi = {}
for k, s in BOARD.items():
    if k == 'memset_16': continue
    if cal_fn.get(k): cpi[k] = s / 100 * cyc_total / cal_fn[k]
per_store = BOARD['memset_16'] / 100 * cyc_total / cal_st['memset_16']
print('calibration (%s): clear %.1f cycles per 4-byte store; CPI %s; others %.2f' % (
    sys.argv[1], per_store, ', '.join('%s %.2f' % (k, v) for k, v in sorted(cpi.items())), DEFAULT_CPI))
base = None
for f in sys.argv[2:]:
    fn, st = load(f)
    cls = {'clear': 0.0, 'listed': 0.0, 'unlisted': 0.0}
    for k, n in fn.items():
        if k in FILL: continue
        c = cpi.get(ALIAS.get(k, k))
        if c is None: cls['unlisted'] += n * DEFAULT_CPI
        else: cls['listed'] += n * c
    cls['clear'] = sum(st.get(k, 0) for k in FILL) * per_store
    tot = sum(cls.values())
    ms = lambda c: c / MHZ / 1e3
    line = '%s: libGL+runtime %.2f ms/frame (clear %.2f, listed %.2f, unlisted %.2f)' % (
        f, ms(tot), ms(cls['clear']), ms(cls['listed']), ms(cls['unlisted']))
    if base:
        line += '; vs first: %+.2f ms (clear %+.2f, listed %+.2f, unlisted %+.2f)' % (
            ms(tot - sum(base.values())), ms(cls['clear'] - base['clear']),
            ms(cls['listed'] - base['listed']), ms(cls['unlisted'] - base['unlisted']))
    else:
        base = cls
    print(line)
