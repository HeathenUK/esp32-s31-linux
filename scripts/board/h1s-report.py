#!/usr/bin/env python3
"""Report on a hart-1 PC capture taken by the hart0 sampler.

    h1s-report.py <System.map> <samples.txt> [top N]

The sampler (bootloader/main/s31_vcpu.c, "H1 PC SAMPLER") has hart0 read hart
1's program counter from the bus monitor at 1 kHz. Unlike /proc/profile it sees
everything hart 1 executes - user code, kernel code with interrupts OFF, the
M-mode firmware - and it costs hart 1 nothing, so a UP and an SMP capture of the
same workload can be compared symbol by symbol.

samples.txt is one hex PC per line (devmem output). Take it like this, with the
two addresses from `nm hello_world.elf | grep s31_h1s_`:

    devmem <s31_h1s_ctrl> 32 4000 ; <workload, >= 4 s> ;
    i=0; while [ $i -lt 4000 ]; do devmem $((<s31_h1s_buf> + i*4)) 32; i=$((i+1)); done

WHERE the code runs from matters as much as which code it is: kernel text in
flash costs ~6x what the same text costs in RAM (.text..fast) on this board.
"""
import bisect
import collections
import sys


def load_map(path):
    syms = []
    marks = {}
    for line in open(path):
        parts = line.split()
        if len(parts) < 3:
            continue
        addr = int(parts[0], 16)
        name = parts[2]
        if name in ("__text_fast_start", "__text_fast_end", "_exiprom", "_sdata", "_end"):
            marks[name] = addr
        if parts[1] in "tTwW":
            syms.append((addr, name))
    syms.sort()
    return syms, marks


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    syms, marks = load_map(sys.argv[1])
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    addrs = [a for a, _ in syms]
    fast_lo = marks.get("__text_fast_start", 0)
    fast_hi = marks.get("__text_fast_end", 0)
    rom_hi = marks.get("_exiprom", 0xC0800000)

    pcs = []
    for line in open(sys.argv[2]):
        line = line.strip()
        if line:
            pcs.append(int(line, 16))
    n = len(pcs)
    if not n:
        sys.exit("no samples")

    where = collections.Counter()
    ksym = collections.Counter()
    ksym_where = {}
    for pc in pcs:
        if fast_lo <= pc < fast_hi:
            cat = "kernel, RAM text (.text..fast)"
        elif 0xC0000000 <= pc < rom_hi:
            cat = "kernel, FLASH text (XIP)"
        elif pc >= 0xC0000000:
            cat = "kernel, other"
        elif 0x40000000 <= pc < 0x50000000 or 0x2F000000 <= pc < 0x30000000:
            cat = "M-mode firmware (OpenSBI)"
        elif pc >= 0x80000000:
            cat = "user: shared libraries"
        elif pc == 0:
            cat = "no sample (0)"
        else:
            cat = "user: the program itself"
        where[cat] += 1
        if cat.startswith("kernel"):
            i = bisect.bisect_right(addrs, pc) - 1
            name = syms[i][1] if i >= 0 else hex(pc)
            ksym[name] += 1
            ksym_where[name] = "RAM" if "RAM" in cat else "flash"

    print("%d samples" % n)
    for cat, c in where.most_common():
        print("  %5.1f%%  %5d  %s" % (100.0 * c / n, c, cat))
    print("\ntop kernel symbols (share of ALL samples)")
    for name, c in ksym.most_common(top):
        print("  %5.1f%%  %5d  %-5s %s" % (100.0 * c / n, c, ksym_where[name], name))


if __name__ == "__main__":
    main()
