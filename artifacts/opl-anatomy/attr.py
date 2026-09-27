import re,bisect,collections,sys
dis,nm,bb,NS=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4])
syms=[(int(a,16),n) for a,t,n in (l.split() for l in open(nm))]
addrs=[a for a,_ in syms]
def fn(o):
    i=bisect.bisect_right(addrs,o)-1; return syms[i][1] if i>=0 else '?'
ins={}; order=[]
for l in open(dis):
    m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)',l)
    if m: a=int(m.group(1),16); ins[a]=(m.group(2),m.group(3)); order.append(a)
idx={a:i for i,a in enumerate(order)}
perfn=collections.Counter(); calls=collections.Counter()
for l in open(bb):
    pc,n,c=l.split(); o=int(pc,16); n=int(n); c=int(c)
    if o not in idx: perfn['<other>']+=n*c; continue
    i=idx[o]
    for k in range(n):
        a=order[i+k]; perfn[fn(a)]+=c
        op,args=ins[a]
        if op in('jal','call','jalr'):
            m=re.search(r'<([^>+]+)',args)
            if m: calls[(fn(a),hex(a),m.group(1))]+=c
tot=sum(perfn.values())
print(f"TOTAL {tot/NS:.0f} instr/sample")
for f,v in perfn.most_common(16): print(f"{v/NS:9.1f}/sample {100*v/tot:5.1f}%  {f}")
print("calls/sample by site:")
for (f,a,t),v in sorted(calls.items(),key=lambda x:-x[1]):
    if v/NS>=0.3: print(f"{v/NS:8.2f}  {f:22s} {a:>8s} -> {t}")
