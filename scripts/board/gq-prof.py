#!/usr/bin/env python3
"""gq-prof.py <System.map> <pcs> <game-maps> <symdir> [lvdesk-maps lvdesk-nm]

Category breakdown of an h1s capture of the GLQuake stack (glquake-run.sh
GQ_PROF=...), built on h1s-report.py's loaders: kernel symbols bucketed by
subsystem (paging, SD/block, scheduler+tick, IRQ entry, sound, idle), user
PCs by library, libGL and quakespasm by function. The game's maps resolve
shared libraries; lvdesk's program text is resolved separately when its
maps and nm are given (shared-library PCs cannot be split per process).
"""
import bisect, collections, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location("h1s", os.path.join(os.path.dirname(os.path.abspath(__file__)), "h1s-report.py"))
h1s = importlib.util.module_from_spec(spec); spec.loader.exec_module(h1s)

KCAT = [
    ("idle", r"cpu_idle|default_idle|do_idle"),
    ("paging/mm", r"fault|swap|folio|page|shrink|lru|rmap|mtree|vma|pte|pmd|mm_|zone|kswapd|vmscan|reclaim|filemap|readahead|memcg|mapping|anon|mas_|mmap|memset|memcpy|clear_user|copy_"),
    ("SD/block", r"dw_mci|mmc|blk|bio|request|esp32s31_cache|sdhci|scsi|elv|dd_|bfq|submit|end_io"),
    ("sound", r"snd|pcm|i2s|es8389|dma|gdma|asoc|soc_"),
    ("sched/tick/timer", r"sched|update_|hrtimer|tick|clock|ktime|rcu|load_avg|vruntime|timer|pelt|decay|dl_|calc_delta|task|curr|enqueue|dequeue|pick_|wake|rq|switch|avg|cpuacct|posix"),
    ("irq/exception entry", r"irq|clic|exception|softirq|call_on|ret_from|handle_|entry|trap|fstate|irqentry"),
    ("syscall/fs/net", r"sys_|vfs|ksys|fd|file|poll|select|sock|unix|inet|tcp|futex|ep_|pipe|ioctl|read|write|drm|xshim"),
]
def kcat(name):
    for c, rx in KCAT:
        if re.search(rx, name):
            return c
    return "kernel other"

def main():
    a = sys.argv[1:]
    syms, marks = h1s.load_map(a[0]); addrs = [x for x, _ in syms]
    pcs = [int(l, 16) for l in open(a[1]) if l.strip()]
    maps = h1s.load_maps(a[2]); symdir = a[3]
    tables = {}
    for _, _, _, b in maps:
        p = os.path.join(symdir, b + ".nm")
        if os.path.exists(p) and b not in tables:
            tables[b] = h1s.load_nm(p)
    lmaps = h1s.load_maps(a[4]) if len(a) > 5 else []
    lnm = h1s.load_nm(a[5]) if len(a) > 5 else None
    fast_lo, fast_hi = marks.get("__text_fast_start", 0), marks.get("__text_fast_end", 0)
    n = len(pcs)
    top = collections.Counter(); sub = collections.defaultdict(collections.Counter)
    for pc in pcs:
        if pc >= 0xC0000000:
            i = bisect.bisect_right(addrs, pc) - 1
            name = syms[i][1] if i >= 0 else hex(pc)
            where = "RAM" if fast_lo <= pc < fast_hi else "flash"
            c = kcat(name)
            top["kernel: " + c] += 1; sub["kernel: " + c]["%s %s" % (where, name)] += 1
            continue
        if 0x40000000 <= pc < 0x50000000 or 0x2F000000 <= pc < 0x30000000:
            top["M-mode firmware"] += 1; continue
        hit = None
        for lo, hi, base, b in maps:
            if lo <= pc < hi:
                s = h1s.lookup(tables.get(b), pc - base) if tables.get(b) else None
                hit = (b, s or "?"); break
        if hit is None and pc < 0x80000000:
            for lo, hi, base, b in lmaps:
                if lo <= pc < hi and b == "lvdesk":
                    hit = ("lvdesk", h1s.lookup(lnm, pc - base) or "?"); break
        if hit is None:
            hit = ("unresolved user", "?")
        top[hit[0]] += 1; sub[hit[0]][hit[1]] += 1
    print("%d samples" % n)
    for k, c in top.most_common():
        print("%6.1f%%  %5d  %s" % (100.0 * c / n, c, k))
        for s, cc in sub[k].most_common(12 if k in ("libGL.so.1.2.0", "quakespasm") else 5):
            print("           %5.1f%%  %s" % (100.0 * cc / n, s))

if __name__ == "__main__":
    main()
