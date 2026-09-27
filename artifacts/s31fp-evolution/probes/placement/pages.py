import re,sys
# copy lengths from v2.o nm -S (upper bound for ge/le: full routine size); subdf3 = 0x444-0x210
CL={'muldf3':0x210,'adddf3':0x22c,'subdf3':0x234,'divdf3':0x262,'fixdfsi':0x70,'fixunsdfsi':0x78,
 'floatsidf':0x36,'floatunsidf':0x24,'extendsfdf2':0x44,'truncdfsf2':0x8c,'gedf2':0xd0,'ledf2':0xd0,'eqdf2':0x5c,'unorddf2':0x38}
def pages(a,n): return set(range(a>>12,(a+n-1)>>12|0+1)) if False else set(range(a>>12,((a+n-1)>>12)+1))
for f in sys.argv[1:]:
    syms={}
    for l in open(f):
        m=re.match(r'([0-9a-f]+) ([0-9a-f]+) [tT] __(\w+)$',l.strip())
        if m and m.group(3) in CL and m.group(3) not in ('gtdf2','ltdf2','nedf2'): syms[m.group(3)]=(int(m.group(1),16),int(m.group(2),16))
    if not syms: continue
    span=set(); cp=set(); cpnd=set(); ent=set(); d={}
    for s,(a,n) in syms.items():
        span|=pages(a,n); cp|=pages(a,CL[s]); ent|=pages(a,8)
        if s!='divdf3': cpnd|=pages(a,CL[s])
    lo=min(a for a,n in syms.values()); hi=max(a+n for a,n in syms.values())
    print(f"{f.split('/')[-1]:32s} n={len(syms):2d} span=0x{lo:x}-0x{hi:x} span_pages={len(span)} copy_all={len(cp)} copy_no_div={len(cpnd)} entry_only={len(ent)}")
    for s,(a,n) in sorted(syms.items(),key=lambda x:x[1]): print(f"    {s:12s} 0x{a:x} pg 0x{a>>12:x}")
