import sys,bisect
syms=[]
for l in open('lms.nm'):
    p=l.split()
    if len(p)==3 and p[1] in 'tTWw': syms.append((int(p[0],16),p[2]))
syms.sort(); addrs=[a for a,_ in syms]
tot={}
for l in open(sys.argv[1]):
    if not l.startswith('0x'): continue
    pc,tc,ic,ec=[x.strip() for x in l.split(',')]
    pc=int(pc,16); n=int(ic)*int(ec)
    i=bisect.bisect_right(addrs,pc)-1
    s=syms[i][1] if i>=0 else '?'
    tot[s]=tot.get(s,0)+n
T=sum(tot.values())
for s,n in sorted(tot.items(),key=lambda x:-x[1])[:10]: print(f"{s:24s} {n/2000:9.1f}/call {100*n/T:5.1f}%")
