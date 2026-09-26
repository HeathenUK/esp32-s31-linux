#!/usr/bin/env python3
"""pcann.py ELF OBJDUMP NM FRAMES LOG [FUNC...] - from qsprof.sh's block counts
(OUT/qsr/prof-run-LABEL/pcprof.log, FRAMES counted frames): the top 60
functions with instructions/frame, calls/frame (the entry block's count) and
instructions/call; with FUNC, that function's objdump with each instruction's
executions per frame (0 = never ran). Phase 6 (artifacts/gl/phase6/
GEOMETRY.txt). s31, MIT."""
import bisect, collections, subprocess, sys
elf, od, nm, frames, log = sys.argv[1:6]
frames = int(frames)
funcs = sys.argv[6:]
syms = []
for l in subprocess.run([nm, '-n', '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'tTwW':
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
addrs = [a for a, s, n in syms]
blk = {}
for l in open(log):
    if l.startswith('pcprof '):
        _, pc, n, c = l.split()
        blk[int(pc, 16)] = (int(n), int(c))
ins = collections.Counter(); calls = {}
for pc, (n, c) in blk.items():
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0: continue
    a, s, name = syms[i]
    ins[name] += n * c
    if pc == a: calls[name] = c
if not funcs:
    for name, v in ins.most_common(60):
        cl = calls.get(name, 0) / frames
        print('%-40s %10.0f insn/f %8.0f calls/f %7.1f insn/call' % (name, v / frames, cl, v / max(calls.get(name, 1), 1)))
    sys.exit(0)
for f in funcs:
    for a, s, name in syms:
        if name == f:
            out = subprocess.run([od, '-d', '--no-show-raw-insn', '--start-address=%#x' % a, '--stop-address=%#x' % (a + s), elf], capture_output=True, text=True).stdout
            cur = 0; left = 0
            for l in out.splitlines():
                t = l.strip().split(':')[0]
                try:
                    pc = int(t, 16)
                    if pc in blk: cur = blk[pc][1] / frames; left = blk[pc][0]
                    if left <= 0: cur = 0
                    left -= 1
                    print('%9.0f %s' % (cur, l))
                except ValueError:
                    print('          ' + l)
