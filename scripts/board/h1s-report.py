#!/usr/bin/env python3
"""Report on a hart-1 PC capture taken by the hart0 sampler.

    h1s-report.py <System.map> <samples.txt> [top N]
                  [--maps <maps.txt>] [--sym <basename>=<nm>]... [--symdir <dir>]
                  [--fit <basename>=<nm -S file>]

The sampler (bootloader/main/s31_vcpu.c, "H1 PC SAMPLER") has hart0 read hart
1's program counter from the bus monitor at 1 kHz. Unlike /proc/profile it sees
everything hart 1 executes - user code, kernel code with interrupts OFF, the
M-mode firmware - and it costs hart 1 nothing, so a UP and an SMP capture of the
same workload can be compared symbol by symbol.

samples.txt is one hex PC per line (rootfs/h1s output).

WHERE the code runs from matters as much as which code it is: kernel text in
flash costs ~6x what the same text costs in RAM (.text..fast) on this board.

USER SYMBOLS (2026-09-23). Without this, "user: the program itself 59%" was the
end of the story and every decision was made on the kernel's 32%. Two ways to
resolve user PCs:

  --maps maps.txt    a copy of /proc/<pid>/maps taken DURING the capture
                     (scripts/board/profile-apps.sh saves one). Each r-xp
                     mapping names its file; the file's load base is its
                     LOWEST mapping minus that mapping's file offset, exactly
                     as rootfs/pcsample.c computes it. PIE bases change per
                     boot (ASLR is on), so a maps file from another boot is
                     wrong by a page multiple and resolves to junk.
  --sym name=file    symbol table for a file named in maps, matched by
                     basename: `nm -nS binary.dbg > images/name.nm`. Sizes
                     (-S) are optional here but required for --fit.
  --symdir dir       look for <basename>.nm in dir for every mapped file.
  --fit name=file    no maps file (old captures): FIT the load base of one
                     PIE binary by sliding it a page at a time over the
                     program-bucket PCs and keeping the base that puts the
                     most samples INSIDE a sized symbol. Needs `nm -nS`.
                     Prints the fitted base and the coverage; below ~90%
                     coverage do not believe the fit.
"""
import bisect
import collections
import os
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


def load_nm(path):
    """`nm -n` or `nm -nS` output -> sorted [(addr, size or None, name)] of text symbols."""
    out = []
    for line in open(path):
        p = line.split()
        if len(p) == 4 and p[2] in "tTwW":
            out.append((int(p[0], 16), int(p[1], 16), p[3]))
        elif len(p) == 3 and p[1] in "tTwW":
            out.append((int(p[0], 16), None, p[2]))
    out.sort()
    return out


def load_maps(path):
    """/proc/<pid>/maps -> [(lo, hi, base, basename)] for executable mappings."""
    lowest = {}
    rows = []
    for line in open(path):
        p = line.split()
        if len(p) < 6 or "x" not in p[1]:
            continue
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        off = int(p[2], 16)
        f = p[5]
        rows.append((lo, hi, off, f))
        if f not in lowest or lo - off < lowest[f]:
            lowest[f] = lo - off
    return [(lo, hi, lowest[f], os.path.basename(f)) for lo, hi, off, f in rows]


def lookup(nm, off):
    i = bisect.bisect_right([a for a, _, _ in nm], off) - 1
    if i < 0:
        return None
    addr, size, name = nm[i]
    if size is not None and off >= addr + size:
        return None
    return name


def fit_base(nm, pcs):
    """Slide a PIE base a page at a time; keep the one putting most PCs inside sized symbols."""
    sized = [(a, s) for a, s, _ in nm if s]
    if not sized:
        sys.exit("--fit needs sizes: produce the table with `nm -nS`")
    starts = [a for a, _ in sized]
    text_hi = max(a + s for a, s in sized)
    best, best_n = None, -1
    # Stray samples (~1% land below 0x100000, in SRAM or in ROM) make min()
    # and percentiles useless as bounds. Anchor on the densest 1 MB region:
    # the binary is where the samples are.
    dens = collections.Counter(pc >> 20 for pc in pcs)
    mode = dens.most_common(1)[0][0]
    core = [pc for pc in pcs if abs((pc >> 20) - mode) <= 1]
    lo = min(core) & ~0xfff
    # The base is at most lo and at least lo - text_hi; max() is no use as
    # a bound because a few samples land past the end of .text.
    for base in range(max(0, (lo - text_hi) & ~0xfff), lo + 0x1000, 0x1000):
        n = 0
        for pc in pcs:
            off = pc - base
            if off < 0 or off >= text_hi:
                continue
            i = bisect.bisect_right(starts, off) - 1
            if i >= 0 and off < sized[i][0] + sized[i][1]:
                n += 1
        if n > best_n:
            best, best_n = base, n
    return best, best_n


def main():
    args = [a for a in sys.argv[1:]]
    opts = {"maps": None, "sym": {}, "symdir": None, "fit": {}}
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--maps":
            opts["maps"] = args[i + 1]; i += 2
        elif a == "--sym":
            k, v = args[i + 1].split("=", 1); opts["sym"][k] = v; i += 2
        elif a == "--symdir":
            opts["symdir"] = args[i + 1]; i += 2
        elif a == "--fit":
            k, v = args[i + 1].split("=", 1); opts["fit"][k] = v; i += 2
        else:
            pos.append(a); i += 1
    if len(pos) < 2:
        sys.exit(__doc__)
    syms, marks = load_map(pos[0])
    top = int(pos[2]) if len(pos) > 2 else 25
    addrs = [a for a, _ in syms]
    fast_lo = marks.get("__text_fast_start", 0)
    fast_hi = marks.get("__text_fast_end", 0)
    rom_hi = marks.get("_exiprom", 0xC0800000)

    pcs = []
    for line in open(pos[1]):
        line = line.strip()
        if line:
            pcs.append(int(line, 16))
    n = len(pcs)
    if not n:
        sys.exit("no samples")

    # User symbol tables, keyed by basename.
    tables = {}
    for name, path in opts["sym"].items():
        tables[name] = load_nm(path)
    maps = load_maps(opts["maps"]) if opts["maps"] else []
    if opts["symdir"]:
        for _, _, _, base_name in maps:
            cand = os.path.join(opts["symdir"], base_name + ".nm")
            if base_name not in tables and os.path.exists(cand):
                tables[base_name] = load_nm(cand)
    fitted = {}
    for name, path in opts["fit"].items():
        nm = load_nm(path)
        prog = [pc for pc in pcs if 0 < pc < 0x80000000]
        base, hit = fit_base(nm, prog)
        if base is None:
            sys.exit("--fit %s: no candidate base (no program-bucket samples?)" % name)
        fitted[name] = (base, nm)
        print("fit: %s base 0x%08x covers %d of %d program-bucket samples (%.1f%%)"
              % (name, base, hit, len(prog), 100.0 * hit / max(1, len(prog))))

    where = collections.Counter()
    ksym = collections.Counter()
    ksym_where = {}
    usym = collections.Counter()
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
        elif cat.startswith("user"):
            hit = None
            for lo, hi, base, base_name in maps:
                if lo <= pc < hi:
                    nm = tables.get(base_name)
                    s = lookup(nm, pc - base) if nm else None
                    hit = "%s:%s" % (base_name, s or "?+0x%x" % (pc - base))
                    break
            if hit is None:
                for name, (base, nm) in fitted.items():
                    if cat == "user: the program itself":
                        s = lookup(nm, pc - base)
                        if s:
                            hit = "%s:%s" % (name, s)
                            break
            usym[hit or ("?" if cat.endswith("libraries") else "?prog")] += 1

    print("%d samples" % n)
    for cat, c in where.most_common():
        print("  %5.1f%%  %5d  %s" % (100.0 * c / n, c, cat))
    print("\ntop kernel symbols (share of ALL samples)")
    for name, c in ksym.most_common(top):
        print("  %5.1f%%  %5d  %-5s %s" % (100.0 * c / n, c, ksym_where[name], name))
    if tables or fitted:
        print("\ntop user symbols (share of ALL samples; file:symbol)")
        for name, c in usym.most_common(top):
            print("  %5.1f%%  %5d  %s" % (100.0 * c / n, c, name))


if __name__ == "__main__":
    main()
