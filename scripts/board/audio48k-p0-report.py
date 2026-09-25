#!/usr/bin/env python3
"""Per-file shares of an audio48k-p0.sh capture (docs/audio-48k-plan-2026-09-25.md, step 0).

    audio48k-p0-report.py <System.map> <prof.txt> <maps.txt> <symdir> [ksym-files.txt]

Resolves every hart-1 (CPU0) sample the way h1s-report.py does (same map, same
maps-file load bases, same <basename>.nm tables), then sums:

  * user samples by FILE (libasound, libSDL2, the s31route plugin, libc, the
    game, ...), with the top symbols of the audio-path files;
  * kernel samples by SOURCE FILE, using ksym-files.txt ("<symbol> <obj path>",
    nm over every object of the #N build), so sound/ and drivers/dma/ are
    summed from where the code lives rather than guessed from name prefixes;
  * the decision-rule total: plugin + libasound + kernel sound/ + dma/ +
    the ioctl entry (fs/ioctl.o), as a share of all CPU0 samples.

Caveat (recorded 2026-09-24): user PCs are resolved against the GAME's maps.
Another process on CPU0 whose library lands at an overlapping address (lvdesk's
libc) is counted as the game's; libasound/SDL2/the plugin are mapped only by
the game here, so their shares are not affected unless another process maps
them too.
"""
import bisect
import collections
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("h1s", os.path.join(HERE, "h1s-report.py"))
h1s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h1s)

AUDIO_FILES = ("libasound", "libasound_module_pcm_s31route", "libSDL2")


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    smap, prof, mapsf, symdir = sys.argv[1:5]
    kfiles = sys.argv[5] if len(sys.argv) > 5 else os.path.join(symdir, "ksym-files.txt")
    syms, marks = h1s.load_map(smap)
    addrs = [a for a, _ in syms]
    fast_lo, fast_hi = marks.get("__text_fast_start", 0), marks.get("__text_fast_end", 0)
    rom_hi = marks.get("_exiprom", 0xC0800000)
    sym2file = collections.defaultdict(set)
    if os.path.exists(kfiles):
        for line in open(kfiles):
            p = line.split()
            if len(p) == 2:
                sym2file[p[0]].add(p[1])
    maps = h1s.load_maps(mapsf)
    tables = {}
    for _, _, _, b in maps:
        c = os.path.join(symdir, b + ".nm")
        if b not in tables and os.path.exists(c):
            tables[b] = h1s.load_nm(c)
    pcs = [int(l, 16) for l in open(prof) if l.strip()]
    n = len(pcs)
    cls = collections.Counter()
    ufile = collections.Counter()
    usym = collections.Counter()
    kfile = collections.Counter()
    ksym = collections.Counter()
    for pc in pcs:
        if pc >= 0xC0000000:
            i = bisect.bisect_right(addrs, pc) - 1
            name = syms[i][1] if i >= 0 else hex(pc)
            where = "RAM" if fast_lo <= pc < fast_hi else ("flash" if pc < rom_hi else "other")
            files = sym2file.get(name) or sym2file.get(name.split(".")[0])
            f = sorted(files)[0] if files else "?"
            if files and len(files) > 1:
                f += " (+%d)" % (len(files) - 1)
            cls["kernel"] += 1
            if name == "arch_cpu_idle" or name == "default_idle_call":
                cls["kernel idle"] += 1
            kfile[f] += 1
            ksym[(name, where, f)] += 1
        elif 0x40000000 <= pc < 0x50000000 or 0x2F000000 <= pc < 0x30000000:
            cls["M-mode"] += 1
        elif pc == 0:
            cls["zero"] += 1
        else:
            cls["user"] += 1
            hit = None
            for lo, hi, base, b in maps:
                if lo <= pc < hi:
                    nm = tables.get(b)
                    s = h1s.lookup(nm, pc - base) if nm else None
                    hit = (b, s or "?")
                    break
            if hit is None:
                hit = ("unmapped (another process)", "?")
            ufile[hit[0]] += 1
            usym[hit] += 1

    pct = lambda c: 100.0 * c / n
    print("%d samples (CPU0 = hart 1)" % n)
    for k in ("user", "kernel", "kernel idle", "M-mode", "zero"):
        print("  %-12s %5.1f%%  %5d" % (k, pct(cls[k]), cls[k]))
    print("\nuser, by file")
    for f, c in ufile.most_common(20):
        print("  %5.2f%%  %5d  %s" % (pct(c), c, f))
    print("\naudio-path user symbols")
    for (f, s), c in usym.most_common():
        if f.startswith(AUDIO_FILES):
            print("  %5.2f%%  %5d  %s:%s" % (pct(c), c, f, s))
    print("\nkernel, by source file (top 40)")
    for f, c in kfile.most_common(40):
        print("  %5.2f%%  %5d  %s" % (pct(c), c, f))
    snd = sum(c for f, c in kfile.items() if f.startswith("sound/"))
    dma = sum(c for f, c in kfile.items() if f.startswith("drivers/dma/"))
    ioc = sum(c for f, c in kfile.items() if f.startswith("fs/ioctl"))
    print("\nkernel sound/ and drivers/dma/ and fs/ioctl symbols")
    for (s, w, f), c in ksym.most_common():
        if f.startswith(("sound/", "drivers/dma/", "fs/ioctl")):
            print("  %5.2f%%  %5d  %-5s %s  [%s]" % (pct(c), c, w, s, f))
    ua = {k: sum(c for f, c in ufile.items() if f.startswith(k)) for k in AUDIO_FILES}
    total = ua["libasound"] + snd + dma + ioc
    print("\nDECISION-RULE SUMS (share of all CPU0 samples)")
    print("  plugin (s31route)        %5.2f%%  %d" % (pct(ua["libasound_module_pcm_s31route"]), ua["libasound_module_pcm_s31route"]))
    print("  libasound (+plugin)      %5.2f%%  %d" % (pct(ua["libasound"]), ua["libasound"]))
    print("  libSDL2 (all threads)    %5.2f%%  %d" % (pct(ua["libSDL2"]), ua["libSDL2"]))
    print("  kernel sound/            %5.2f%%  %d" % (pct(snd), snd))
    print("  kernel drivers/dma/      %5.2f%%  %d" % (pct(dma), dma))
    print("  kernel fs/ioctl          %5.2f%%  %d" % (pct(ioc), ioc))
    print("  RULE TOTAL (plugin+libasound+sound+dma+ioctl) %5.2f%%  %d  (threshold 2%%)" % (pct(total), total))


if __name__ == "__main__":
    main()
