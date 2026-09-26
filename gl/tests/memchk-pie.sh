#!/bin/sh
# memchk-pie.sh - (phase 5 review O7) every libc memcpy/memcmp/memset/memmove
# call in the RV32 libGL whose size is a constant >= 64 (the a2 load before
# the call@plt): musl takes the ESP PIE path there, which bounces off the
# lent CPU. Run after gl/build.sh, in the build container:
#   ./docker/build.sh "sh /src/gl/tests/memchk-pie.sh"
# Prints "BIG <fn> <size> <caller> <count>" and a total. s31, MIT.
OD=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-objdump
$OD -d --no-show-raw-insn /src/gl/out-rv32/libGL.so.1.unstripped > /tmp/d.txt
python3 - <<'P'
import re
fn=None; last={}; out=[]
for l in open('/tmp/d.txt'):
    m=re.match(r'^[0-9a-f]+ <(.*)>:',l)
    if m: fn=m.group(1); last={}; continue
    m=re.search(r'\t(li|c\.li)\s+a2,(-?\d+)',l)
    if m: last['a2']=int(m.group(2)); continue
    m=re.search(r'jalr?\s.*<(memcpy|memcmp|memset|memmove)@plt>',l)
    if m:
        out.append((m.group(1),last.get('a2'),fn)); last={}
from collections import Counter
for (k,a,f),n in sorted(Counter(out).items(),key=lambda x:(x[0][0],str(x[0][1]))):
    if a is not None and a>=64: print("BIG",k,a,f,n)
print("total calls",len(out),"const>=64",sum(1 for k,a,f in out if a is not None and a>=64))
P
