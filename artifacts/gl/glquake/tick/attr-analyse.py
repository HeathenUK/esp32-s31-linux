import sys,re,os,bisect,collections
d=sys.argv[1]; smap='/Users/gadyke/esp32-s31-linux/images/ship-393-System.map'
m=[(int(l.split()[0],16),l.split()[2]) for l in open(smap) if len(l.split())>=3 and l.split()[1] in 'tTwW']
m.sort(); ks=[a for a,_ in m]
def sym(s):
    if not s.startswith('0x'): return s
    v=int(s,16); i=bisect.bisect_right(ks,v)-1; return m[i][1]
R=lambda f: open(os.path.join(d,f),errors='replace').read()
dt=float(R('upB'))-float(R('upA')); print('window %.1f s'%dt, R('sf').strip())
def irq(f):
    o={}
    for l in R(f).splitlines()[1:]:
        p=l.split()
        try: o[p[0]+' '+' '.join(p[3:] if not p[0].startswith('IPI') else p[3:])]=(int(p[1]),int(p[2]))
        except: pass
    return o
a,b=irq('irqA'),irq('irqB')
print('\n-- IRQ/s  CPU0  CPU1')
for k in b:
    x=(b[k][0]-a[k][0])/dt,(b[k][1]-a[k][1])/dt
    if x[0]+x[1]>0.5: print('%-60s %7.1f %7.1f'%(k[:60],x[0],x[1]))
def stat(f):
    return {l.split()[0]:[int(v) for v in l.split()[1:]] for l in R(f).splitlines()}
sa,sb=stat('statA'),stat('statB')
print('ctxt/s %.0f'%((sb['ctxt'][0]-sa['ctxt'][0])/dt))
for c in ('cpu0','cpu1'):
    dd=[y-x for x,y in zip(sa[c],sb[c])]; t=sum(dd) or 1
    print(c,'user %.0f%% sys %.0f%% idle %.0f%% iowait %.0f%% irq %.0f%% softirq %.0f%%'%tuple(100*dd[i]/t for i in (0,2,3,4,5,6)))
def sirq(f):
    o={}
    for l in R(f).splitlines()[1:]:
        p=l.split(); o[p[0]]=(int(p[1]),int(p[2]))
    return o
qa,qb=sirq('sirqA'),sirq('sirqB')
print('\n-- softirq/s'); print('  '.join('%s %.0f/%.0f'%(k,(qb[k][0]-qa[k][0])/dt,(qb[k][1]-qa[k][1])/dt) for k in qb))
def ctx(f):
    o={}
    for l in R(f).splitlines():
        p=l.split()
        if len(p)==5: o[(p[0],p[1],p[2])]=(int(p[3]),int(p[4]))
    return o
ca,cb=ctx('ctxA'),ctx('ctxB')
rows=[]
for k in cb:
    if k in ca:
        v=(cb[k][0]-ca[k][0])/dt; n=(cb[k][1]-ca[k][1])/dt
        if v+n>=0.3: rows.append((v+n,v,n,k))
rows.sort(reverse=True)
print('\n-- switches/s  vol  nonvol  pid tid comm')
for r in rows[:25]: print('%6.1f %6.1f %6.1f  %s'%(r[0],r[1],r[2],' '.join(r[3])))
print('\n-- armed hrtimers by function, CPU0/CPU1, summed over 4 snapshots')
cnt=collections.Counter()
for f in ('tlA','tlA2','tlA3','tlB'):
    cpu=None
    for l in R(f).splitlines():
        if l.startswith('cpu:'): cpu=l.split()[1]
        mm=re.match(r' #\d+: <[0-9a-f]+>, ([^,]+),',l)
        if mm: cnt[(sym(mm.group(1)),cpu)]+=1
for (fn,c),n in sorted(cnt.items(),key=lambda x:-x[1]): print('%3d cpu%s %s'%(n,c,fn))
ev=[ [int(x.split()[-1]) for x in R(f).splitlines() if 'nr_events' in x] for f in ('tlA','tlB')]
print('hrtimer nr_events/s', [ '%.0f'%((y-x)/dt) for x,y in zip(*ev)])
va={l.split()[0]:int(l.split()[1]) for l in R('vmA').splitlines()}; vb={l.split()[0]:int(l.split()[1]) for l in R('vmB').splitlines()}
print('vm/s',{k:round((vb[k]-va[k])/dt,1) for k in va})
print(R('slack'))
