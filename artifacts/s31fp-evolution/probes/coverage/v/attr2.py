import re,sys
base=int(sys.argv[1],16)
R={'__adddf3':(0x67a44,0x688b4),'__divdf3':(0x688b4,0x695d0),'__muldf3':(0x695d0,0x69e5a),'__subdf3':(0x69e5a,0x6a2b0)}
tot=0;helper=0;byname={k:0 for k in R}
for line in sys.stdin:
    i=line.find('0x')
    if i<0: continue
    try: pc=int(line[i+2:i+10],16)
    except: continue
    off=pc-base
    if off<0 or off>0xa8000: continue
    tot+=1
    for nm,(a,b) in R.items():
        if a<=off<b: helper+=1; byname[nm]+=1; break
print("libc insns",tot,"helper",helper,"share %.1f%%"%(100*helper/tot if tot else 0), byname)
