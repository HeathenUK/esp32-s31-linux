import re,sys
def rd(p):
    d={}
    for f in ['results.txt','feat.txt','pix.txt','prim.txt','geo.txt','filt.txt','p4.txt']:
        for l in open(p+'/'+f):
            m=re.match(r'(.+?): ([\d.]+) Minsn.*fb (\w+)',l)
            if m: d[m.group(1)]=(float(m.group(2)),m.group(3))
    return d
lim={}
for l in open('limits.txt'):
    if l.startswith('#') or not l.strip(): continue
    f=[x.strip() for x in l.split('|')]; lim[f[0]]=float(f[1])
a=rd(sys.argv[1]); b=rd(sys.argv[2]); c=rd(sys.argv[3])
over=0
for k in sorted(a, key=lambda k: -(c[k][0]/a[k][0])):
    r=c[k][0]/a[k][0]-1
    L=lim.get(k); o = L is not None and c[k][0]>L
    over+=o
    if abs(r)>0.004 or c[k][1]!=b[k][1] or o or '-v' in sys.argv:
        print("%-24s p4 %8.4f  prev %8.4f  new %8.4f  %+6.2f%% vs p4 %+6.2f%% vs prev %s %s %s"%(k,a[k][0],b[k][0],c[k][0],100*r,100*(c[k][0]/b[k][0]-1),'same' if c[k][1]==a[k][1] else 'HASH', '' if c[k][1]==b[k][1] else 'CHANGED-vs-prev', 'OVER' if o else ''))
print('over limits:',over)
