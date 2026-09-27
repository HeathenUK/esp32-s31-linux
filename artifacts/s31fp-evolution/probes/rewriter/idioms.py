import re,sys,collections
D=sys.argv[1]
calls=[];fn=None
for l in open(D):
    m=re.match(r'^([0-9a-f]+) <(.+)>:',l)
    if m: fn=m.group(2); calls.append(('FN',fn,0)); continue
    m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s+(.*)',l)
    if not m: continue
    a,op,rest=m.groups()
    t=re.search(r'<([^>+]+)>',rest)
    if op in('jal','call','jalr','tail','j') and t and (op!='j'):
        calls.append(('C',t.group(1),int(a,16)))
# chains of helper calls within a function, broken by non-helper call
H=lambda s:s.startswith('__') and (s.endswith('df2') or s.endswith('df3') or s.endswith('dfsi') or s.endswith('sidf') or 'sfdf' in s or 'dfsf' in s)
chains=collections.Counter();cur=[];last=0
def flush():
    global cur
    if len(cur)>=2: chains[' '.join(c.replace('__','') for c in cur)]+=1
    cur=[]
for k,n,a in calls:
    if k=='FN' or not H(n): flush(); continue
    if cur and a-last>64: flush()
    cur.append(n); last=a
flush()
tot=sum(chains.values())
print('chains>=2 helpers (<=64B apart):',tot)
for c,n in chains.most_common(25): print(n,c)
