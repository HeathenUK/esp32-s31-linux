#!/usr/bin/env python3
"""icattr.py ELF DUMP [NFRAMES] - attribute icsim.c's per-line dump to functions.
Per function: lines executed, instructions, modelled refills, and how many of
its lines ran in every counted frame. Phase 6 tier 6. s31, MIT."""
import subprocess, sys, os, collections
elf, dump = sys.argv[1], sys.argv[2]
nf = int(sys.argv[3]) if len(sys.argv) > 3 else 0
TC = os.environ.get('S31_BENCH_TC', os.path.expanduser('~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf'))
syms = []
for l in subprocess.run([TC + '/bin/riscv32-esp-elf-nm', '-n', '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'tTwW':
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
import bisect
starts = [s[0] for s in syms]
def fn(a):
    i = bisect.bisect_right(starts, a) - 1
    while i >= 0 and syms[i][0] + max(syms[i][1], 1) <= a and i > 0 and syms[i-1][0] == syms[i][0]:
        i -= 1
    return syms[i] if i >= 0 else (0, 0, '?')
F = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0])
tl = ti = tr = te = 0
for l in open(dump):
    w = l.split(); a = int(w[0], 16); ins = int(w[1]); ref = int(w[2]); fr = int(w[3])
    s = fn(a)
    # a line can hold the tails/heads of two functions: charge the one at the line start
    f = F[s[2]]
    f[0] += 1; f[1] += ins; f[2] += ref; f[3] += (fr >= nf) if nf else 0; f[4] = s[1]; f[5] = s[0]
    tl += 1; ti += ins; tr += ref; te += (fr >= nf) if nf else 0
print(f"# {tl} lines ({tl*64/1024:.1f} kB) executed, {te} in every frame; per frame {ti/max(nf,1)/1e6:.3f} M insns, {tr/max(nf,1):.0f} refills")
print(f"# {'function':32} {'size':>6} {'lines':>5} {'every':>5} {'kinsn/f':>9} {'refill/f':>8}")
for k, v in sorted(F.items(), key=lambda kv: -kv[1][2]):
    print(f"  {k:32} {v[4]:6d} {v[0]:5d} {v[3]:5d} {v[1]/max(nf,1)/1e3:9.1f} {v[2]/max(nf,1):8.1f}")
