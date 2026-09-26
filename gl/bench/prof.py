#!/usr/bin/env python3
# prof.py ELF NM FRAMES < qemu -d in_asm,exec,nochain log
# Flat instruction profile (phase 3a): every executed translation block's
# instruction count times its executions, summed by function, over the
# counted frames only (between q_ui.c's two q_mark() calls), per frame
# (FRAMES = QN, 10). An instruction count, not time: no PSRAM, I-cache or
# FPU latency model. s31, MIT.
import sys, re, subprocess, bisect, collections
elf, nm, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
tbn = {}; cnt = collections.Counter()
cur = None; n = 0
# phase 3a: memory operations per block (qemu prints decompressed
# mnemonics): loads, stores, bytes read, bytes written
LD = {'lw': 4, 'lh': 2, 'lhu': 2, 'lb': 1, 'lbu': 1, 'flw': 4, 'fld': 8}
ST = {'sw': 4, 'sh': 2, 'sb': 1, 'fsw': 4, 'fsd': 8}
tbm = {}; mem = [0, 0, 0, 0]
mark = None
for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[2] == 'q_mark': mark = int(p[0], 16)
on = mark is None
insn = re.compile(r'^0x([0-9a-f]+):\s+([0-9a-f]+)\s')
for line in sys.stdin:
    if line.startswith('Trace '):
        pc = int(line.split('[')[1].split('/')[1], 16)
        if pc == mark: on = not on
        elif on: cnt[pc] += 1
        continue
    m = insn.match(line)
    if m:
        if cur is None: cur = int(m.group(1), 16); n = 0; mem = [0, 0, 0, 0]
        n += 1
        f = line.split()
        op = f[2] if len(f) > 2 else ''
        if op in LD: mem[0] += 1; mem[2] += LD[op]
        elif op in ST: mem[1] += 1; mem[3] += ST[op]
        continue
    if cur is not None:
        if n >= tbn.get(cur, 0): tbm[cur] = mem
        tbn[cur] = max(tbn.get(cur, 0), n); cur = None
if cur is not None:
    if n >= tbn.get(cur, 0): tbm[cur] = mem
    tbn[cur] = max(tbn.get(cur, 0), n)
syms = []
for l in subprocess.run([nm, '-S', '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'Tt': syms.append((int(p[0], 16), int(p[1], 16), p[3]))
starts = [s[0] for s in syms]
fn = collections.Counter()
fst = collections.Counter(); tot_m = [0, 0, 0, 0]
for pc, k in cnt.items():
    mm = tbm.get(pc, [0, 0, 0, 0])
    for j in range(4): tot_m[j] += mm[j] * k
    i = bisect.bisect_right(starts, pc) - 1
    name = syms[i][2] if i >= 0 and pc < syms[i][0] + syms[i][1] else '?'
    fn[name] += k * tbn.get(pc, 0)
    fst[name] += k * mm[1]
tot = sum(fn.values())
import os
if os.environ.get('PROF_FN'):
    want = os.environ['PROF_FN']
    rows = []
    for pc, k in cnt.items():
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and syms[i][2] == want:
            rows.append((pc - syms[i][0], tbn.get(pc, 0), k))
    for off, n, k in sorted(rows):
        print("  %s+0x%x: %d insns x %.1f /frame = %.0f" % (want, off, n, k / frames, n * k / frames))
print("total %.4f M insn per counted frame (%d frames)" % (tot / frames / 1e6, frames))
print("memory per frame: %.0f loads (%.0f B), %.0f stores (%.0f B); stores by function: %s" %
      (tot_m[0] / frames, tot_m[2] / frames, tot_m[1] / frames, tot_m[3] / frames,
       ', '.join('%s %.0f' % (k, v / frames) for k, v in
                 fst.most_common(6) + [(k, fst[k]) for k in ('memset_16', 'ZB_fill16')
                                       if fst[k] and k not in dict(fst.most_common(6))])))
for k, v in fn.most_common(80):
    print("  %-34s %9.0f /frame  %5.1f%%" % (k, v / frames, 100.0 * v / tot))
