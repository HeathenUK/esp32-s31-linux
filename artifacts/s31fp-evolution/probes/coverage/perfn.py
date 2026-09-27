import re,sys,collections
fns=sys.argv[2].split(',')
cur=None; d=collections.defaultdict(collections.Counter); size={}
for line in open(sys.argv[1]):
    m=re.match(r'^([0-9a-f]+) <(.+)>:$',line)
    if m: cur=m.group(2); continue
    m=re.search(r'\t(jal|j|c\.jal|c\.j)\s+(?:ra,)?\s*[0-9a-f]+ <([^>+]+)>',line)
    if m and cur: d[cur][m.group(2)]+=1
    if cur and re.search(r'\t(f[a-z.]+)\s',line): d[cur]['<fpinsn>']+=1
    if cur and re.search(r'\tesp\.',line): d[cur]['<esp.*>']+=1
for f in fns: print(f, dict(d[f]))
