#!/usr/bin/env python3
"""plsum.py r1.txt... - per run: fps, per-frame I/D refills from the CC lines
(t = 30 s .. the last sample, as tier 5), where the game's main thread and
lvdesk were at each sample (PL lines, /proc field 39), the colours of
libGL's hot pages (PC line). GL phase 6 tier 6. s31, MIT."""
import re, sys
# the hot range [__s31hot_start, __s31hot_end) of each library, by the name
# the game maps it under: the shipped SD-root 91771d02, and tier 6's
# ordered build in /root/t6 (8b3d03fd; build log gl/bench/out-t6br/d5c.log)
HOT = {'libGL.so.1.2.0': (0x566e0, 0x61afc), 'libGL.so.1': (0x54220, 0x61e8c)}
for f in sys.argv[1:]:
    t = open(f, errors='replace').read()
    fps = float(re.search(r'RESULT \d+ frames ([\d.]+) seconds\s+([\d.]+) fps', t).group(2))
    secs = float(re.search(r'RESULT \d+ frames ([\d.]+) seconds', t).group(1))
    cc = [(int(m.group(1)), [int(x, 16) for x in m.group(2).split()]) for m in re.finditer(r'^CC (\d+) ((?:0x[0-9A-F]+ ?){6})', t, re.M)]
    a = [c for c in cc if c[0] == 30][0]; b = [c for c in cc if c[0] != 'end'][-1]
    dt = b[0] - a[0]; fr = fps * dt
    d = [(y - x) / fr for x, y in zip(a[1], b[1])]
    pls = re.findall(r'^PL (\d+) (.*)$', t, re.M)
    q0 = l0 = n = 0
    for ts, rest in pls:
        if int(ts) < 30: continue
        toks = rest.split()
        q = [x for x in toks if x.startswith('quakespasm:')]
        l = [x for x in toks if x.startswith('lvdesk:')]
        if q: q0 += q[0].split(':')[1] == '0'
        if l: l0 += l[0].split(':')[1] == '0'
        n += 1
    m = re.search(r'^PC (libGL\S*) 0x0 (\S+)', t, re.M)
    if m:
        h0, h1 = HOT[m.group(1)]
        hot = m.group(2)[h0 >> 12:((h1 - 1) >> 12) + 1]
    else:
        hot = '?'
    hc = [hot.count(str(c)) for c in range(4)]
    lib = m.group(1) if m else '?'
    print(f"{f.split('/')[-2]:26} {lib:15} {fps:4.1f} fps  I1 {d[5]/1e3:5.1f}k I0 {d[4]/1e3:5.1f}k D1rd {d[2]/1e3:5.1f}k /frame | quake on CPU0 {q0}/{n} lvdesk on CPU0 {l0}/{n} | libGL hot pages {hot} per colour {hc}")
