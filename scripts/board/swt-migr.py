#!/usr/bin/env python3
"""Per-task migrations over a gears-swt.sh window: swt-migr.py <dir> <label>
(swm0/swm1-<label>.txt = /proc/*/task/*/sched nr_migrations before/after,
swn-<label>.txt = task comms taken before the window)."""
import re, sys, os
d, L = sys.argv[1], sys.argv[2]
def load(f):
    r = {}
    for l in open(f):
        m = re.match(r'/proc/(\d+)/task/(\d+)/sched:se.nr_migrations\s*:\s*(\d+)', l)
        if m: r[m.group(2)] = int(m.group(3))
    return r
a = load(os.path.join(d, f'swm0-{L}.txt')); b = load(os.path.join(d, f'swm1-{L}.txt'))
n = {}
for l in open(os.path.join(d, f'swn-{L}.txt')):
    p = l.split(None, 1); m = re.match(r'/proc/\d+/task/(\d+)/comm', p[0])
    if m and len(p) > 1: n[m.group(1)] = p[1].strip()
for dd, k in sorted(((b[k] - a.get(k, 0), k) for k in b if b[k] - a.get(k, 0) > 0), reverse=True)[:15]:
    print(dd, k, n.get(k, '(started in window)'))
t = open(os.path.join(d, f'swc-{L}.txt')).read()
m0 = re.search(r'PIEB0 (\d+)', t); m1 = re.search(r'PIEB1 (\d+)', t)
if m0 and m1: print('PIE bounces in window', int(m1.group(1)) - int(m0.group(1)))
