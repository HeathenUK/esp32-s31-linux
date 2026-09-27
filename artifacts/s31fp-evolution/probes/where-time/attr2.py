import sys,bisect
syms=[]
for l in open('lms.nm'):
    p=l.split()
    if len(p)==3 and p[1] in 'tTWw': syms.append((int(p[0],16),p[2]))
syms.sort(); addrs=[a for a,_ in syms]
def cnt(f):
    tot={}
    for l in open(f):
        p=l.split(',')
        if len(p)<2: continue
        try: pc=int(p[1].strip(),16)
        except: continue
        i=bisect.bisect_right(addrs,pc)-1
        s=syms[i][1]; tot[s]=tot.get(s,0)+1
    return tot
a=cnt(sys.argv[1]); b=cnt(sys.argv[2]); n=int(sys.argv[3])
d={k:b.get(k,0)-a.get(k,0) for k in set(a)|set(b)}
T=sum(d.values())
for s,v in sorted(d.items(),key=lambda x:-x[1])[:12]:
    if v>0: print(f"  {s:24s} {v/n:8.1f}/call {100*v/T:5.1f}%")
print(f"  TOTAL {T/n:.0f}/call")
