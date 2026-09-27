import re,sys,collections
# ranges of soft-double helpers (from nm): name:(start,end)
R={'__adddf3':(0x67a44,0x688b4),'__divdf3':(0x688b4,0x695d0),'__muldf3':(0x695d0,0x69e5a),
   '__subdf3':(0x69e5a,0x6a2b0),'__fixdfsi':None}
tot=0; helper=0
for line in sys.stdin:
    m=re.search(r'0x([0-9a-f]+):',line)
    if not m: continue
    pc=int(m.group(1),16); tot+=1
    for nm,rg in R.items():
        if rg and rg[0]<=pc<rg[1]: helper+=1; break
print("total",tot,"helper",helper,"share %.1f%%"%(100*helper/tot if tot else 0))
