#!/usr/bin/env python3
"""census.py NM ELF OUT.txt - name the stage lists of a qscensus.sh run
(s31_census.c's 'census' lines), largest pixel count first. s31, MIT."""
import subprocess, sys
nm, elf, out = sys.argv[1:4]
names = {}
for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW':
        names.setdefault(int(p[0], 16), p[2])
frames = 0
rows = []
for l in open(out):
    if l.startswith('qsrf ') and ' count ' in l:
        frames += 1
    if l.startswith('census '):
        p = l.split()
        fused = int(p[3]); ch = int(p[5]); px = int(p[7]); al = int(p[9])
        fns = [names.get(int(a, 16), a) for a in p[11:]]
        rows.append((px, al, ch, fused, fns))
rows.sort(reverse=True)
tot = sum(r[0] for r in rows)
print('# %d counted frames; general-path pixels per frame %.0f (alive %.0f)' % (
    frames, tot / frames, sum(r[1] for r in rows) / frames))
print('#   px/frame  alive/frame  chunks/frame fused  stages (depth, st..., [gen_depth, gen_st...])')
for px, al, ch, fused, fns in rows:
    print('%10.0f %10.0f %9.0f %3d  %s' % (px / frames, al / frames, ch / frames, fused, ' '.join(fns)))
