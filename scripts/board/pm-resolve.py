#!/usr/bin/env python3
"""Resolve a hart0 post-mortem / timed dump (bootloader/main/s31_vcpu.c).

    pm-resolve.py <System.map> <console-recording>

Prints hart 1's sampled PCs, then the return addresses found on each CPU's
stack, as symbols - i.e. both call chains of a Linux that can no longer speak.
"""
import bisect, re, sys
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in open(sys.argv[1])
              if len(l.split()) >= 3 and l.split()[1] in 'tTwW')
addrs = [a for a, _ in syms]
etext = max(a for a, n in syms if a < 0xc0800000)
def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    return "%s+0x%x" % (syms[i][1], a - syms[i][0])
data = open(sys.argv[2], 'rb').read().decode('latin1')
on = False
for l in data.splitlines():
    l = l.replace('  | ', '')
    if 'TIMED DUMP' in l or 'POST-MORTEM' in l:
        on = True
    if not on:
        continue
    if re.search(r'stack sp|cpu1 pc|vpending|sclic cfg|TIMED DUMP|POST-MORTEM', l):
        print(l.rstrip())
        for m in re.findall(r'\b(?:pc|ra) (c[0-9a-f]{7})', l):
            print("      ", m, sym(int(m, 16)))
    elif re.match(r'^\s*h1 [0-9a-f]{8}/', l):
        for m in re.findall(r'h1 ([0-9a-f]{8})/', l):
            print("   h1 pc", m, sym(int(m, 16)))
    elif re.match(r'^( [0-9a-f]{8}){8}', l):
        for w in l.split():
            v = int(w, 16)
            if 0xc0000000 <= v <= etext or 0xc0860000 <= v < 0xc0881000:
                print("    ", w, sym(v))
    if 'end2' in l:
        break
