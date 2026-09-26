#!/usr/bin/env python3
"""pcannot.py ELF OBJDUMP FUNC FRAMES LOG - annotate one function's
disassembly with gl/bench/qemu/pcprof.c's block counts (instructions per
counted frame executed at each address), for tuning a filler. s31, MIT."""
import collections, re, subprocess, sys
elf, od, func, frames, log = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
dis = subprocess.run([od, '-d', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout
lines, on = [], False
for l in dis.splitlines():
    if re.match(r'^[0-9a-f]+ <%s>:' % re.escape(func), l): on = True; continue
    if on and re.match(r'^[0-9a-f]+ <[^.]', l) and not re.match(r'^[0-9a-f]+ <\.', l): break
    if on:
        m = re.match(r'\s*([0-9a-f]+):\s*(.*)', l)
        if m: lines.append((int(m.group(1), 16), m.group(2)))
addrs = [a for a, _ in lines]
cnt = collections.Counter()
idx = {a: i for i, a in enumerate(addrs)}
for l in open(log):
    if l.startswith('pcprof '):
        _, pc, n, c = l.split(); pc, n, c = int(pc, 16), int(n), int(c)
        if pc in idx:
            for k in range(n):
                if idx[pc] + k < len(lines): cnt[lines[idx[pc] + k][0]] += c
tot = 0
for a, t in lines:
    c = cnt[a] / frames
    tot += c
    print('%10.0f  %8x  %s' % (c, a, t))
print('# %s: %.0f instructions per frame' % (func, tot))
