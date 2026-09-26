#!/usr/bin/env python3
"""pcprof.py ELF NM FRAMES LOG [OBJLIST] - per-function instructions from
gl/bench/qemu/pcprof.c's block counts (the counted frames only), per frame.

With OBJLIST (build_q.sh's obj/core.list) the functions are also summed by
the library object that defines them (zpipe.o, ztriangle_gen.o, ...), and
everything outside the library (the replayer, libc, libgcc) is one group
each. An instruction count, not time: no PSRAM, cache or FPU latency.
s31, MIT."""
import bisect
import collections
import subprocess
import sys

elf, nm, frames, log = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
objlist = sys.argv[5] if len(sys.argv) > 5 else None
syms = []
for l in subprocess.run([nm, '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, n in syms]
fn = collections.Counter()
total = 0
for l in open(log):
    if l.startswith('pcprof '):
        _, pc, n, c = l.split()
        pc, n, c = int(pc, 16), int(n), int(c)
        i = bisect.bisect_right(addrs, pc) - 1
        name = syms[i][1] if i >= 0 else '?'
        fn[name] += n * c
    elif l.startswith('pcprof-total'):
        total = int(l.split()[1])
if not total:
    total = sum(fn.values()) or 1
print('# %s: %.4f M instructions per counted frame (%d frames)' % (elf, total / frames / 1e6, frames))
print('# %-40s %12s %7s' % ('function', 'insn/frame', '%'))
for name, v in fn.most_common(40):
    print('  %-40s %12.0f %6.2f%%' % (name, v / frames, 100.0 * v / total))
if objlist:
    owner = {}
    for o in open(objlist).read().split():
        for l in subprocess.run([nm, '--defined-only', o], capture_output=True, text=True).stdout.splitlines():
            p = l.split()
            if len(p) == 3 and p[1] in 'tTwW':
                owner.setdefault(p[2], o.rsplit('/', 1)[-1])
    by = collections.Counter()
    for name, v in fn.items():
        if name in owner:
            by[owner[name]] += v
        elif name.startswith('d_') or name.startswith('rp_') or name.startswith('null_') or \
                name in ('on_swap', 'on_ctx', 'main', 'fb_hash', 'q_on', 'q_off'):
            by['(replayer)'] += v
        else:
            by['(libc/libgcc: %s)' % name if v > total * 0.005 else '(libc/libgcc other)'] += v
    print('# by object')
    for name, v in by.most_common(30):
        print('  %-40s %12.0f %6.2f%%' % (name, v / frames, 100.0 * v / total))
