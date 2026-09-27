import sys,re,subprocess,collections
f=sys.argv[1]; pat=re.compile(sys.argv[2]) if len(sys.argv)>2 else None
out=subprocess.run(['riscv32-esp-linux-musl-objdump','-d','--no-show-raw-insn',f],capture_output=True,text=True).stdout
cur='?'; calls=collections.Counter(); by=collections.defaultdict(collections.Counter)
for line in out.splitlines():
    m=re.match(r'^[0-9a-f]+ <(.+)>:$',line)
    if m: cur=m.group(1); continue
    m=re.search(r'\t(jal|j|c\.jal|c\.j|tail|call)\s+(?:ra,)?\s*[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>',line)
    if m and m.group(3) is None:
        op=m.group(1); t=m.group(2).replace('@plt','')
        if op in('j','c.j') and t==cur: continue
        if op in ('j','c.j') and not t.startswith('__') and t not in ('memset','memcpy','memmove'): continue
        if pat and not pat.search(t): continue
        calls[t]+=1; by[t][cur]+=1
for t,n in calls.most_common():
    print(f"{n:5d} {t}   <- "+", ".join(f"{c}:{k}" for c,k in by[t].most_common(8)))
