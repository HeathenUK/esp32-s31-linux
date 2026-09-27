import re,sys,struct
src=open('/home/user/esp32-s31-linux/rootfs/s31fp/v2/sigs3.h').read()
bodies={m.group(1):bytes(int(x,16) for x in m.group(2).split(',')) for m in re.finditer(r'body_(\w+)\[\d+\] = \{([^}]*)\}',src)}
masks={m.group(1):[int(x,0) for x in m.group(2).split(',')] for m in re.finditer(r'mask_(\w+)\[\] = \{([^}]*)\}',src)}
order=re.findall(r'\{ "__(\w+)", (\d+), (\d+),',src)
# v2 copy lengths from assembled v2f.o copytab (measured above)
copy={'muldf3':528,'adddf3':556,'subdf3':564,'divdf3':0,'fixdfsi':112,'floatsidf':54,'fixunsdfsi':120,'floatunsidf':36,
 'extendsfdf2':68,'truncdfsf2':140,'gedf2':62,'ledf2':62,'eqdf2':92,'unorddf2':56}
def text(path):
    d=open(path,'rb').read()
    phoff,=struct.unpack_from('<I',d,28); phnum,=struct.unpack_from('<H',d,44)
    for i in range(phnum):
        t,off,va,pa,fs,ms,fl,al=struct.unpack_from('<8I',d,phoff+32*i)
        if t==1 and fl&1: return d[off:off+fs],va
def ok(b,at,name,nmask):
    g=bodies[name]; L=len(g)
    if at+L>len(b): return False
    stops=[masks[name][m] for m in range(nmask)]+[L]; pos=0
    for s in stops:
        if b[at+pos:at+s]!=g[pos:s]: return False
        pos=s+4
    return True
for path in sys.argv[1:]:
    b,va=text(path); sites=[]
    p=0
    while p<len(b)-60:
        hit=None
        for name,L,nm in order:
            g=bodies[name]
            if b[p:p+2]==g[:2] and ok(b,p,name,int(nm)): hit=(name,int(L)); break
        if hit: sites.append((p,)+hit); p+=hit[1]
        else: p+=2
    print(path, 'text=%d'%len(b))
    for mode in ('copy','tramp'):
        tot=0; big=0; rows=[]
        for off,name,L in sites:
            c=copy[name] if mode=='copy' else 0
            used=c if c else 8
            used=(used+3)&~3  # align thunk start
            free=L-used; tot+=free; big=max(big,free); rows.append('%s@%x:%d'%(name,off+va,free))
        lo=min(s[0] for s in sites)&~4095; hi=(max(s[0]+s[2] for s in sites)+4095)&~4095
        print(' mode=%s sites=%d dead_total=%d largest=%d pages_cowed=%d'%(mode,len(sites),tot,big,(hi-lo)//4096))
        print('  ',' '.join(rows))
