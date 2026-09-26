#!/usr/bin/env python3
"""Attribute long frames from a swapstamp capture (rootfs/swapstamp.c).

    swt-analyse.py swt.txt [--long 45]

For every frame whose swap-to-swap time is >= --long ms, splits it into
render CPU, render not-running, swap CPU, swap not-running, glClear, with
the switch counts, CPUs and the peer's (lvdesk's) ticks/CPU. Then totals:
where the extra time of long frames went, against the median frame.
"""
import sys
rows = []
for l in open(sys.argv[1]):
    if l.startswith('#') or not l.strip():
        continue
    p = l.split()
    rows.append(dict(f=int(p[0]), t=int(p[1]), gap=int(p[2]), swap=int(p[3]), cr=int(p[4]),
                     cs=int(p[5]), clr=int(p[6]), nclr=int(p[7]), vr=int(p[8]), ir=int(p[9]),
                     vs=int(p[10]), is_=int(p[11]), minf=int(p[12]), majf=int(p[13]),
                     c0=int(p[14]), c1=int(p[15]), pt=int(p[16]), pc=int(p[17]), ps=p[18],
                     ctr=int(p[19]) if len(p) > 19 else 0))
thr = 45
if '--long' in sys.argv:
    thr = float(sys.argv[sys.argv.index('--long') + 1])
for r in rows:
    r['tot'] = (r['gap'] + r['swap']) / 1000.0
tots = sorted(r['tot'] for r in rows)
n = len(tots)
med = tots[n // 2]
span = (rows[-1]['t'] - rows[0]['t']) / 1000.0 if n > 1 else 0
print(f"{n} frames over {span:.1f} s = {n/span:.1f} fps; frame ms median {med:.1f} p90 {tots[int(n*.9)]:.1f} p99 {tots[int(n*.99)]:.1f} max {tots[-1]:.1f}")
def hist(v):
    b = [0]*6
    for x in v:
        b[0 if x < 25 else 1 if x < 35 else 2 if x < 50 else 3 if x < 100 else 4 if x < 200 else 5] += 1
    return f"<25:{b[0]} <35:{b[1]} <50:{b[2]} <100:{b[3]} <200:{b[4]} >=200:{b[5]}"
print("hist", hist(tots))
def comp(sel):
    k = max(1, len(sel))
    g = sum(r['gap'] for r in sel)/k/1000; s = sum(r['swap'] for r in sel)/k/1000
    cr = sum(r['cr'] for r in sel)/k/1000; cs = sum(r['cs'] for r in sel)/k/1000
    cl = sum(r['clr'] for r in sel)/k/1000
    return f"render {g:.1f} (cpu {cr:.1f}, clear {cl:.1f}) swap {s:.1f} (cpu {cs:.1f}) | vcs r/s {sum(r['vr'] for r in sel)/k:.2f}/{sum(r['vs'] for r in sel)/k:.2f} ivcs r/s {sum(r['ir'] for r in sel)/k:.2f}/{sum(r['is_'] for r in sel)/k:.2f} | cpu1 frac {sum(1 for r in sel if r['c0']==1)/k:.2f} | peer ticks {sum(r['pt'] for r in sel)/k:.2f} peer cpu1 {sum(1 for r in sel if r['pc']==1)/k:.2f} | ctr {sum(r['ctr'] for r in sel)/k:.3f}"
normal = [r for r in rows if r['tot'] < med * 1.3]
long_ = [r for r in rows if r['tot'] >= thr]
print("normal (%d):" % len(normal), comp(normal))
print("long >= %g ms (%d):" % (thr, len(long_)), comp(long_))
print(f"\n{'f':>5} {'t_s':>7} {'tot':>6} {'gap':>6} {'crend':>6} {'clr':>5} {'swap':>6} {'cswap':>6} vr ir vs is mf c0c1 pt pc ps ctr")
for r in rows:
    if r['tot'] >= thr:
        print(f"{r['f']:5d} {r['t']/1000:7.2f} {r['tot']:6.1f} {r['gap']/1000:6.1f} {r['cr']/1000:6.1f} {r['clr']/1000:5.1f} {r['swap']/1000:6.1f} {r['cs']/1000:6.1f} {r['vr']:2d} {r['ir']:2d} {r['vs']:2d} {r['is_']:2d} {r['majf']:2d} {r['c0']}{r['c1']}   {r['pt']:2d} {r['pc']:2d} {r['ps']} {r['ctr']}")
