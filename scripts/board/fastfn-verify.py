#!/usr/bin/env python3
"""fastfn-verify.py <list> <System.map of the built kernel> [<System.map of the baseline>]

After a FASTFN build: which listed functions actually landed in RAM text
(address >= _etext) and which stayed in flash. objcopy --rename-section is a
silent no-op for a section that does not exist, so assembly (entry.S, fpu.S),
__lockfunc (.spinlock.text), __exit and __sched code all stay put without a
word. With a baseline map it also prints the RAM/flash text size deltas.
"""
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__)

def load(path):
    m, marks = {}, {}
    for l in open(path):
        p = l.split()
        if len(p) == 3:
            m.setdefault(p[2], int(p[0], 16))
            if p[2] in ("_etext", "__text_fast_end", "_stext"):
                marks[p[2]] = int(p[0], 16)
    return m, marks

lst = [l.split() for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
m, marks = load(sys.argv[2])
etext = marks["_etext"]
moved = [(o, f) for o, f in lst if m.get(f, 0) >= etext]
stayed = [(o, f) for o, f in lst if m.get(f, 0) < etext]
print("listed %d: moved to RAM %d, stayed in flash %d" % (len(lst), len(moved), len(stayed)))
for o, f in stayed:
    print("  stayed 0x%08x %-30s %s" % (m.get(f, 0), f, o))
if len(sys.argv) > 3:
    b, bm = load(sys.argv[3])
    print("RAM text end %x -> %x (%+d B); flash _etext %x -> %x (%+d B)" % (
        bm["__text_fast_end"], marks["__text_fast_end"], marks["__text_fast_end"] - bm["__text_fast_end"],
        bm["_etext"], etext, etext - bm["_etext"]))
sys.exit(1 if stayed else 0)
