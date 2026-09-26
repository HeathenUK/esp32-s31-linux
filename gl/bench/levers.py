#!/usr/bin/env python3
"""levers.py DIR... - one row per bench output directory (a lever state):
M instructions per frame for the phase 3a cases, soft-double calls per
frame (gears/glxgears/teapot), the library objects' .text sum, and the
change against the previous row and the first. Markdown on stdout.
s31, MIT."""
import sys, re, subprocess, os
SZ = os.path.expanduser('~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-size')
cols = [('gears 320x240', 'gears 320'), ('gears 640x400', 'gears 640'), ('glxgears 300x300', 'glxgears 300'),
        ('teapot 320x240', 'teapotf 320'), ('teapot 640x400', 'teapotf 640'),
        ('texobj 320x240', 'texobj 320'), ('texobj 640x400', 'texobj 640'),
        ('geo1 320x240', 'clear 320'), ('geo1 640x400', 'clear 640'),
        ('geo3 320x240', 'indexed'), ('geo4 320x240', 'rotates'), ('geo5 320x240', 'mech-like'),
        ('geo6 320x240', 'spec+spot'), ('geo7 320x240', 'colormat'), ('geo8 320x240', 'strips')]
def load(d):
    r = {}; dc = {}; fb = {}
    for f in ('results.txt', 'geo.txt'):
        p = os.path.join(d, f)
        if not os.path.exists(p): continue
        for l in open(p):
            m = re.match(r'(.+?): ([\d.]+) Minsn/frame, (\d+) soft-double calls/frame, fb (\w+)', l)
            if m: r[m.group(1)] = float(m.group(2)); dc[m.group(1)] = int(m.group(3)); fb[m.group(1)] = m.group(4)
    out = subprocess.run([SZ] + sorted(os.path.join(d, 'obj', o) for o in os.listdir(os.path.join(d, 'obj')) if o.endswith('.o')),
                         capture_output=True, text=True).stdout.splitlines()[1:]
    text = sum(int(l.split()[0]) for l in out)
    return r, dc, fb, text
rows = [(d, load(d)) for d in sys.argv[1:]]
hdr = '| state | ' + ' | '.join(c[1] for c in cols) + ' | dcalls g/gx/t | .text |'
print(hdr); print('|' + '---|' * (len(cols) + 3))
base = rows[0][1]; prev = None
for d, (r, dc, fb, text) in rows:
    cells = []
    for k, _ in cols:
        v = r.get(k)
        if v is None: cells.append('-'); continue
        s = '%.4f' % v
        if prev is not None and k in prev[0]:
            s += ' (%+.1f%%)' % (100.0 * (v / prev[0][k] - 1))
        cells.append(s)
    dcs = '/'.join(str(dc.get(k, '-')) for k in ('gears 320x240', 'glxgears 300x300', 'teapot 320x240'))
    t = '%d' % text + ('' if prev is None else ' (%+d)' % (text - prev[3]))
    print('| %s | %s | %s | %s |' % (os.path.basename(d.rstrip('/')), ' | '.join(cells), dcs, t))
    prev = (r, dc, fb, text)
r0, _, fb0, t0 = base; r1, _, fb1, t1 = rows[-1][1]
print('\n| case | first | last | change | frame hash first -> last |')
print('|---|---|---|---|---|')
for k, n in cols:
    if k in r0 and k in r1:
        print('| %s | %.4f | %.4f | %+.1f%% | %s -> %s%s |' % (n, r0[k], r1[k], 100.0 * (r1[k] / r0[k] - 1), fb0[k], fb1[k],
              '' if fb0[k] == fb1[k] else ' (differs)'))
print('\nlibrary objects .text: %d -> %d (%+d B)' % (t0, t1, t1 - t0))
