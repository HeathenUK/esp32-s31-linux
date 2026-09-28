#!/usr/bin/env python3
# Static census of straight-line soft-double helper-call chains in RV32 ELFs
# (input for the s31fp call-site rewriter rules; see
# docs/s31fp-generic-acceleration-plan-2026-09-28.md Phase 3).
#   python3 tools/s31fp/chainscan.py <file listing ELF paths>
# Needs riscv32-esp-linux-musl-objdump on PATH (. tools/cloud/env.sh).
import re,sys,subprocess,collections
H={'__muldf3':'mul','__adddf3':'add','__subdf3':'sub','__divdf3':'div','__floatsidf':'i2d','__floatunsidf':'u2d','__fixdfsi':'d2i','__fixunsdfsi':'d2u','__extendsfdf2':'f2d','__truncdfsf2':'d2f','__gedf2':'cmp','__gtdf2':'cmp','__ledf2':'cmp','__ltdf2':'cmp','__eqdf2':'cmp','__nedf2':'cmp'}
tot=collections.Counter()
for f in open(sys.argv[1]).read().split():
    d=subprocess.run(['riscv32-esp-linux-musl-objdump','-d','--no-show-raw-insn',f],capture_output=True,text=True).stdout
    fn=None; seq=[]; last=-99; n=0; chains=collections.Counter(); sites=0
    for l in d.splitlines():
        m=re.match(r'^[0-9a-f]+ <(.+)>:',l)
        if m: fn=m.group(1); seq=[]; continue
        m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)',l)
        if not m or fn in H: continue
        n+=1; op=m.group(2); args=m.group(3)
        t=re.search(r'<([^>+]+)',args)
        if op in('jal','call') and t and t.group(1).split('@')[0] in H:
            sites+=1
            if n-last>12: 
                if len(seq)>=2: chains[' '.join(seq)]+=1
                seq=[]
            seq.append(H[t.group(1).split('@')[0]]); last=n
        elif op.startswith('b') or op in('j','jr','ret') or (op in('jal','call','jalr')):
            if len(seq)>=2: chains[' '.join(seq)]+=1
            seq=[]; last=-99
    ending=sum(v for k,v in chains.items() if k.endswith(('d2i','d2u','d2f')))
    if sites:
        print(f"{f}: helper call sites {sites}, straight-line chains of >=2 {sum(chains.values())}, ending in d2i/d2u/d2f {ending}")
        for k,v in chains.most_common(4): print(f"      {v:4d}  {k}")
