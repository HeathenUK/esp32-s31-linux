#!/usr/bin/env python3
"""h1sann.py PCS MAPS LIB ELF NM FUNC [OBJDUMP] - one function's disassembly
annotated with the board's hart-1 PC samples (rootfs/h1s via
scripts/board/gq-prof.py: PCS one hex PC a line, MAPS the game's
/proc/PID/maps taken during the capture). LIB is the mapped file's name as
MAPS shows it (e.g. /root/thr/libGL.so.1); ELF the same bytes on the host
(the stripped library is enough when NM gives its symbols: nm -n output,
e.g. artifacts/gl/glquake/prof8/libGL-thr.nm). Check the function's bytes
against the build you mean before reading anything into the result.

Prints: samples, share of the function and the instruction for every
address (a sample is where hart 1 was when hart0 looked: a load that
stalls shows on its neighbours), then the function's total. Phase 6 zf8
(artifacts/gl/phase6/zf8/REPORT.txt). s31, MIT."""
import collections, subprocess, sys

pcs, maps, lib, elf, nmf, func = sys.argv[1:7]
od = sys.argv[7] if len(sys.argv) > 7 else '/opt/homebrew/bin/llvm-objdump'
base = None
for l in open(maps):
    p = l.split()
    if len(p) >= 6 and p[5] == lib:
        lo = int(p[0].split('-')[0], 16) - int(p[2], 16)
        base = lo if base is None else min(base, lo)
if base is None:
    sys.exit('h1sann: %s is not in %s' % (lib, maps))
syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in open(nmf))
              if len(p) == 3 and p[1] in 'tTwW')
start = [a for a, n in syms if n == func]
if not start:
    sys.exit('h1sann: no %s in %s' % (func, nmf))
start = start[0]
end = min(a for a, n in syms if a > start)
cnt = collections.Counter()
for l in open(pcs):
    try:
        o = int(l.split()[0], 16) - base
    except (ValueError, IndexError):
        continue
    if start <= o < end:
        cnt[o] += 1
tot = sum(cnt.values())
dis = subprocess.run([od, '-d', '--no-show-raw-insn',
                      '--mattr=+m,+a,+f,+c,+zba,+zbb,+zbc,+zbs',
                      '--start-address=%#x' % start, '--stop-address=%#x' % end, elf],
                     capture_output=True, text=True).stdout
for l in dis.splitlines():
    t = l.strip()
    if ':' not in t or not t.split(':')[0].strip().isalnum():
        continue
    try:
        a = int(t.split(':')[0], 16)
    except ValueError:
        continue
    c = cnt.get(a, 0)
    print('%5d %5.1f%%  %x  %s' % (c, 100.0 * c / max(tot, 1), a, t.split(':', 1)[1].strip()))
print('# %s: %d samples at [%#x, %#x) (load base %#x)' % (func, tot, start, end, base))
