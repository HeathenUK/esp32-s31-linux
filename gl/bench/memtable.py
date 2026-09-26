#!/usr/bin/env python3
# memtable.py OUT_A [OUT_B ...] - markdown tables from mem.sh's mem_*.txt
# (review 3a M3): per image, the bytes stored per frame to the colour
# buffers, the depth buffer (both reach PSRAM on the board), the stack and
# everything else (D-cache resident), the 64-byte lines dirtied in the
# buffers, and the dearest frame's buffer bytes; with several OUT dirs the
# buffer bytes of each against the first. s31, MIT.
import sys, re, glob, os
def parse(f):
    t = open(f).read()
    out = {}
    for tag in ('memtotal', 'memmax'):
        m = re.search(r'^%s (\d+):(.*)$' % tag, t, re.M)
        if not m: return None
        v = {}
        for part in m.group(2).split('|'):
            x = part.split()
            if len(x) < 13: continue
            v[x[0]] = dict(st=float(x[2]), B=float(x[4]), dl=float(x[6]), ld=float(x[8]), lB=float(x[10]))
        out[tag] = (int(m.group(1)), v)
    return out
dirs = sys.argv[1:]
imgs = sorted(set(os.path.basename(f)[4:-4] for d in dirs for f in glob.glob(d + '/mem_*.txt')))
data = {d: {i: parse('%s/mem_%s.txt' % (d, i)) for i in imgs if os.path.exists('%s/mem_%s.txt' % (d, i))} for d in dirs}
k = lambda b: '%.0f' % (b / 1e3)
for d in dirs:
    print('\n%s (kB per frame; stores in thousands)\n' % d)
    print('| image | frames | colour st / kB | depth st / kB | buffers kB | buffer lines kB | stack kB | other kB | dearest frame: buffers kB |')
    print('|---|---|---|---|---|---|---|---|---|')
    for i in imgs:
        p = data[d].get(i)
        if not p: continue
        n, a = p['memtotal']; f, x = p['memmax']
        print('| %s | %d | %.1f / %s | %.1f / %s | %s | %s | %s | %s | %s (frame %d) |' % (
            i, n, a['col']['st'] / 1e3, k(a['col']['B']), a['z']['st'] / 1e3, k(a['z']['B']),
            k(a['col']['B'] + a['z']['B']), k((a['col']['dl'] + a['z']['dl']) * 64),
            k(a['stack']['B']), k(a['other']['B']), k(x['col']['B'] + x['z']['B']), f))
if len(dirs) > 1:
    print('\nbuffer bytes stored per frame (colour + depth, kB), against %s\n' % dirs[0])
    print('| image | ' + ' | '.join(dirs) + ' |')
    print('|---|' + '---|' * len(dirs))
    for i in imgs:
        row = []
        b0 = None
        for d in dirs:
            p = data[d].get(i)
            if not p: row.append('-'); continue
            a = p['memtotal'][1]; b = a['col']['B'] + a['z']['B']
            if b0 is None: b0 = b; row.append(k(b))
            else: row.append('%s (%+.0f%%)' % (k(b), 100 * (b / b0 - 1)) if b0 else k(b))
        print('| %s | %s |' % (i, ' | '.join(row)))
