#!/usr/bin/env python3
"""Attribute low-fps seconds from a dipwatch (C) capture.

    dip-analyse.py dip.txt [--dip 15]

Columns: up fps busy game_cpu lvdesk_cpu game_majflt lvdesk_majflt sd_rd_kB
swapout_kB memavail_kB hosted_irq (all per-second deltas except memavail).
"""
import sys
rows=[]
for l in open(sys.argv[1]):
    if l.startswith('#') or not l.strip(): continue
    try: rows.append([int(x) for x in l.split()])
    except ValueError: pass
thr=15
if '--dip' in sys.argv: thr=int(sys.argv[sys.argv.index('--dip')+1])
print(f"{'up':>5} {'fps':>4} {'busy':>4} {'game':>4} {'lvd':>4} {'gmf':>4} {'lmf':>4} {'sdkB':>6} {'swapo':>6} {'avail':>6} {'hirq':>5}")
kinds={'idle':0,'paging':0,'swap':0,'contention':0,'ok':0}
for r in rows[1:]:
    up,fps,busy,g,l,gm,lm,sd,so,av,hi=r
    if fps<thr:
        print(f"{up:5d} {fps:4d} {busy:4d} {g:4d} {l:4d} {gm:4d} {lm:4d} {sd:6d} {so:6d} {av:6d} {hi:5d}")
        if busy<80: kinds['idle']+=1
        elif so>0: kinds['swap']+=1
        elif gm+lm>=5 or sd>=64: kinds['paging']+=1
        else: kinds['contention']+=1
    else: kinds['ok']+=1
f=sorted(r[1] for r in rows[1:])
if f: print(f"\n{len(f)}s: median fps {f[len(f)//2]}, p5 {f[len(f)//20]}, min {f[0]}, mean {sum(f)/len(f):.1f}")
print("game cpu mean %.0f%%, lvdesk cpu mean %.0f%%, busy mean %.0f%%" % (sum(r[3] for r in rows[1:])/max(1,len(rows)-1), sum(r[4] for r in rows[1:])/max(1,len(rows)-1), sum(r[2] for r in rows[1:])/max(1,len(rows)-1)))
print("dip seconds by cause:", kinds)
