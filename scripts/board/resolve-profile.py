#!/usr/bin/env python3
"""Resolve a /proc/profile dump against System.map.

    board$  echo > /proc/profile           # clear
    board$  <run the workload>
    board$  gzip -c /proc/profile | base64 # small: the buffer is mostly zeros
    host$   resolve-profile.py System.map dump.b64 [top]

/proc/profile is one unsigned int holding the SAMPLE STEP - the bucket size
in bytes, 1 << prof_shift, so 64 for profile=6 - followed by one counter per
bucket from _stext. Reading that word as the shift maps every sample past
the end of the text and onto the last symbol, which is how a first draft of
this reported 100% in evdev_ioctl for five different workloads. Samples land at the *sampled PC*,
so a symbol's share is CPU spent inside it, not including callees. Idle is
attributed to whatever the idle path touches (finish_task_switch and friends)
- see docs/current-state.md before reading the top entry as work.
"""
import base64, gzip, struct, sys, bisect

def main():
    smap, dump = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    syms = []
    stext = None
    for line in open(smap):
        a, t, n = line.split()[:3]
        if t in "tTwW":
            syms.append((int(a, 16), n))
            if n == "_stext":
                stext = int(a, 16)
    syms.sort()
    addrs = [a for a, _ in syms]
    raw = gzip.decompress(base64.b64decode(open(dump).read()))
    step = struct.unpack("<I", raw[:4])[0]
    counts = struct.unpack("<%dI" % ((len(raw) - 4) // 4), raw[4:4 + ((len(raw) - 4) // 4) * 4])
    total = sum(counts)
    per = {}
    for i, c in enumerate(counts):
        if not c:
            continue
        addr = stext + i * step
        k = bisect.bisect_right(addrs, addr) - 1
        name = syms[k][1] if k >= 0 else "?"
        per[name] = per.get(name, 0) + c
    print("step=%d bytes, buckets=%d, samples=%d (%.1f s of kernel-mode ticks at 250 Hz; user mode and NOHZ idle are not sampled)" % (step, len(counts), total, total / 250.0))
    for name, c in sorted(per.items(), key=lambda kv: -kv[1])[:top]:
        print("%6.1f%%  %7d  %s" % (100.0 * c / total, c, name))

if __name__ == "__main__":
    main()
