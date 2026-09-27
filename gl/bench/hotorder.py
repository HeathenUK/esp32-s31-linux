#!/usr/bin/env python3
"""hotorder.py ELF DUMP NFRAMES OBJDIR [MODE] - api/hotorder.list from an
icsim.c dump (phase 6 tier 6): every library function the counted frames
executed, as "object function", hottest first. MODE picks the key:
  density  instructions per executed line (default: the loop kernels first)
  insns    instructions per frame
  fetches  line fetches (entries) per executed line: what the code costs
           when it is evicted - per-triangle and per-span code first
Functions of the replay harness and libc (not in OBJDIR's objects) are left
out. s31, MIT."""
import bisect, collections, os, re, subprocess, sys
elf, dump, nf, objdir = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
mode = sys.argv[5] if len(sys.argv) > 5 else 'density'
TC = os.environ.get('S31_BENCH_TC', os.path.expanduser('~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf'))
CLONE = re.compile(r'(\.(part|isra|constprop|cold|lto_priv)\.?\d*)+$')
syms = []
for l in subprocess.run([TC + '/bin/riscv32-esp-elf-nm', '-n', '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'tTwW':
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
starts = [s[0] for s in syms]
# which object holds .text.<fn> (the library's objects only)
home = collections.defaultdict(list)
for o in sorted(os.listdir(objdir)):
    if not o.endswith('.o'):
        continue
    out = subprocess.run([TC + '/bin/riscv32-esp-elf-readelf', '-SW', os.path.join(objdir, o)], capture_output=True, text=True).stdout
    b = o[:-2]; b = b[4:] if b.startswith('tgl_') else b
    for n in re.findall(r'\] (\S+)', out):
        for pre in ('.text.', 's31hot_text'):
            pass
        if n.startswith('.text.') and not n.startswith('.text.unlikely.'):
            home[CLONE.sub('', n[6:])].append(b)
F = collections.defaultdict(lambda: [0, 0, 0])
for l in open(dump):
    w = l.split(); a = int(w[0], 16); ins = int(w[1]); vis = int(w[4]) if len(w) > 4 else 0
    i = bisect.bisect_right(starts, a) - 1
    if i < 0:
        continue
    fn = CLONE.sub('', syms[i][2])
    F[fn][0] += ins; F[fn][1] += 1; F[fn][2] += vis
# functions renamed into s31hot_text already have no .text.<fn> section:
# find them through ramtext.list
here = os.path.dirname(os.path.abspath(__file__))
for line in open(os.path.join(here, '../api/ramtext.list')):
    line = line.split('#', 1)[0].split()
    for fn in line[1:]:
        if line[0] not in home[fn]:
            home[fn].append(line[0])
key = {'density': lambda kv: -kv[1][0] / kv[1][1], 'insns': lambda kv: -kv[1][0],
       'fetches': lambda kv: -kv[1][2] / kv[1][1]}[mode]
n = 0
for fn, (ins, lines, vis) in sorted(F.items(), key=key):
    if fn not in home:
        print('# not in the library: %s (%d lines, %.1f k insns/frame)' % (fn, lines, ins / nf / 1e3))
        continue
    for o in home[fn]:
        print('%-14s %-32s # %4d lines, %9.1f k insns/frame, %8.1f fetches/line/frame' % (o, fn, lines, ins / nf / 1e3, vis / lines / nf))
        n += 1
