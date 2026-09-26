#!/usr/bin/env python3
# foot.py ELF OBJDIR NM THRESHOLD < qemu -d in_asm,exec,nochain log
# The executed-code footprint of the library (review P4; from the review's
# perfrev/foot.py and ana.py): every translation block executed at least
# THRESHOLD times, mapped to its function, summed over the functions defined
# in OBJDIR's objects, in bytes and in distinct 32-byte cache lines.
# Reported, not gated: XIP through the 16 KB I-cache makes code size time,
# and instruction counts cannot see it. s31, MIT.
import sys, re, subprocess, bisect, collections, glob
elf, objdir, nm, thr = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
tbsize = {}; cnt = collections.Counter()
cur = last = None; lastlen = 0
insn = re.compile(r'^0x([0-9a-f]+):\s+([0-9a-f]+)\s')
for line in sys.stdin:
    if line.startswith('Trace '):
        cnt[int(line.split('[')[1].split('/')[1], 16)] += 1
        continue
    m = insn.match(line)
    if m:
        pc = int(m.group(1), 16)
        if cur is None: cur = pc
        last = pc; lastlen = len(m.group(2)) // 2
        continue
    if cur is not None and not line.startswith('0x'):
        tbsize[cur] = max(tbsize.get(cur, 0), last + lastlen - cur); cur = None
if cur is not None: tbsize[cur] = max(tbsize.get(cur, 0), last + lastlen - cur)
syms = []
for l in subprocess.run([nm, '-S', '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'Tt': syms.append((int(p[0], 16), int(p[1], 16), p[3]))
libsyms = set()
for o in glob.glob(objdir + '/*.o'):
    for l in subprocess.run([nm, o], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in 'Tt': libsyms.add(p[2])
starts = [s[0] for s in syms]
hot = collections.defaultdict(set)
for pc, n in cnt.items():
    sz = tbsize.get(pc, 0)
    if n < thr or sz == 0: continue
    i = bisect.bisect_right(starts, pc) - 1
    name = syms[i][2] if i >= 0 and pc < syms[i][0] + syms[i][1] else '?'
    for a in range(pc, pc + sz, 2): hot[name].add(a)
lib = {k: v for k, v in hot.items() if k.split('.')[0] in libsyms}
lines = set(a >> 5 for v in lib.values() for a in v)
tot = sum(len(v) * 2 for v in lib.values())
print("library hot code: %d B in %d functions, %d distinct 32 B lines (%d B)" %
      (tot, len(lib), len(lines), len(lines) * 32))
for k, v in sorted(lib.items(), key=lambda kv: -len(kv[1]))[:12]:
    print("  %-40s %5d B" % (k, len(v) * 2))
