#!/usr/bin/env python3
"""paging-p1.py <run.log> - summarise one scripts/board/paging-p1.sh window.

Prints: the scheduler line; vmstat and block-stat deltas over the window
(item 2's direct-reclaim share, item 5's mean read latency from stat field 4
- the per-request sum of start_time_ns -> completion, so it DOES include the
block-layer queue wait, divided by field 1); the deduped sdtrace ring (reads
only: p50/p90/p99 of total and of each hop; how many reads were dispatched
straight after a write, and the write run in front of them); the sdlat
sampling probe; per-process VmRSS/VmSwap (item 1's game vs the rest); the
lvdesk SIGUSR1 blocks (item 3) verbatim.
"""
import re
import sys

HOPS = ["prep", "i2c", "c2d", "d2x", "i2bh", "x2bh", "bh2st", "stophw",
        "stop", "st2rd", "indrv", "rd2po", "po2bl", "bl2end", "total", "gap"]
RING = re.compile(r"(\d+) (\d+) (\d+) ([RW]) (\d+)/(\d+)/(\d+) (\d+) (\d+) \|((?: \d+)+)( \(incomplete\))?")


def kv(line):
    d = {}
    toks = line.split()
    for i in range(0, len(toks) - 1, 2):
        try:
            d[toks[i]] = int(toks[i + 1])
        except ValueError:
            pass
    return d


def pct(xs, p):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p / 100))]


def main():
    text = open(sys.argv[1], errors="replace").read()
    lines = [l.rstrip("\r") for l in text.splitlines()]
    get = {}
    for l in lines:
        for tag in ("UNAME", "SCHED", "PRE_STATUS", "QPID", "RESULT", "QUAKE", "SDLAT", "SCANOUT_FAIL",
                    "VM_A", "VM_B", "SD_A", "SD_B", "ST_A", "ST_B", "MEM_A", "MEM_B", "Q_A", "Q_B", "K_A", "K_B",
                    "SNAP_A", "SNAP_B", "UPTIME_AT_START"):
            if l.startswith(tag + " "):
                get[tag] = l[len(tag) + 1:]
    for tag in ("UNAME", "UPTIME_AT_START", "SCHED", "PRE_STATUS", "QPID", "RESULT", "QUAKE", "SCANOUT_FAIL"):
        print("%-8s %s" % (tag, get.get(tag, "-")))
    dt = None
    if "SNAP_A" in get and "SNAP_B" in get:
        ua = float(get["SNAP_A"].split("=")[1].split()[0])
        ub = float(get["SNAP_B"].split("=")[1].split()[0])
        dt = ub - ua
        print("window   %.1f s" % dt)
    if "VM_A" in get and "VM_B" in get:
        a, b = kv(get["VM_A"]), kv(get["VM_B"])
        d = {k: b[k] - a.get(k, 0) for k in b}
        show = [k for k in d if d[k]]
        print("vmstat delta: " + " ".join("%s=%d" % (k, d[k]) for k in show))
        sk, sd = d.get("pgsteal_kswapd", 0), d.get("pgsteal_direct", 0)
        ck, cd = d.get("pgscan_kswapd", 0), d.get("pgscan_direct", 0)
        if sk + sd:
            print("ITEM2 pgsteal direct share %.1f%% (%d of %d), pgscan direct share %.1f%%, allocstall_normal %d, "
                  "allocstall_movable %d, kswapd eff %.2f, majflt/s %s, pswpin/s %s, pswpout/s %s" % (
                      100.0 * sd / (sk + sd), sd, sk + sd, 100.0 * cd / max(ck + cd, 1),
                      d.get("allocstall_normal", 0), d.get("allocstall_movable", 0), sk / max(ck, 1),
                      "%.1f" % (d.get("pgmajfault", 0) / dt) if dt else "?",
                      "%.1f" % (d.get("pswpin", 0) / dt) if dt else "?",
                      "%.1f" % (d.get("pswpout", 0) / dt) if dt else "?"))
    if "SD_A" in get and "SD_B" in get:
        a = [int(x) for x in get["SD_A"].split()]
        b = [int(x) for x in get["SD_B"].split()]
        d = [y - x for x, y in zip(a, b)]
        rio, rsec, rtk, wio, wsec, wtk = d[0], d[2], d[3], d[4], d[6], d[7]
        print("ITEM5 blockstat: reads %d (%.1f/s, %.1f KiB avg) mean read latency %.2f ms (field4/field1, queue included); "
              "writes %d (%.1f KiB avg) mean write latency %.2f ms" % (
                  rio, rio / dt if dt else 0, rsec / 2.0 / max(rio, 1), rtk / max(rio, 1),
                  wio, wsec / 2.0 / max(wio, 1), wtk / max(wio, 1)))
    if "Q_A" in get and "Q_B" in get and dt:
        a, b = dict(x.split("=") for x in get["Q_A"].split()), dict(x.split("=") for x in get["Q_B"].split())
        print("quake    utime %+.0f%% stime %+.0f%% of one CPU, majflt %d (%.1f/s), cpu=%s" % (
            (int(b["utime"]) - int(a["utime"])) / dt, (int(b["stime"]) - int(a["stime"])) / dt,
            int(b["majflt"]) - int(a["majflt"]), (int(b["majflt"]) - int(a["majflt"])) / dt, b.get("cpu")))
    if "K_A" in get and "K_B" in get and dt:
        a, b = dict(x.split("=") for x in get["K_A"].split()), dict(x.split("=") for x in get["K_B"].split())
        print("kswapd0  %.1f%% of one CPU (utime+stime ticks %d over %.0f s), last cpu=%s" % (
            (int(b["utime"]) + int(b["stime"]) - int(a["utime"]) - int(a["stime"])) / dt,
            int(b["utime"]) + int(b["stime"]) - int(a["utime"]) - int(a["stime"]), dt, b.get("cpu")))
    for tag in ("MEM_A", "MEM_B"):
        print("%-8s %s" % (tag, get.get(tag, "-")))

    # ---- ring
    rows = {}
    for l in lines:
        m = RING.match(l)
        if m:
            vals = [int(x) for x in m.group(10).split()]
            seq = int(m.group(1))
            op = int(m.group(2))
            # op 13 = CMD13, the write's busy polls (0063): part of the write
            # in front, not a request of their own; kept as a marker only.
            rows[seq] = {"rw": "B" if op == 13 else m.group(4), "op": op, "blk": int(m.group(3)),
                         "h": dict(zip(HOPS, vals)), "inc": bool(m.group(11))}
    if rows:
        seqs = sorted(rows)
        # blk == 1 (a 512 B read) is the keepalive probe (sdlat 512b fixed), never a fault
        ka = [rows[s] for s in seqs if rows[s]["rw"] == "R" and rows[s]["blk"] == 1 and not rows[s]["inc"]]
        if ka:
            print("RING keepalive 512 B reads: %d, total p50/p99 %d/%d us" % (
                len(ka), pct([r["h"]["total"] for r in ka], 50), pct([r["h"]["total"] for r in ka], 99)))
        reads = [rows[s] for s in seqs if rows[s]["rw"] == "R" and rows[s]["blk"] != 1 and not rows[s]["inc"] and rows[s]["h"]["total"]]
        writes = [rows[s] for s in seqs if rows[s]["rw"] == "W" and not rows[s]["inc"] and rows[s]["h"]["total"]]
        print("RING %d unique rows (seq %d..%d, %.0f%% coverage): %d reads, %d writes" % (
            len(rows), seqs[0], seqs[-1], 100.0 * len(rows) / (seqs[-1] - seqs[0] + 1), len(reads), len(writes)))
        for name, rs in (("reads", reads), ("writes", writes)):
            if not rs:
                continue
            print("  %-6s " % name + " ".join("%s=%d/%d/%d" % (h, pct([r["h"][h] for r in rs], 50),
                                                              pct([r["h"][h] for r in rs], 90),
                                                              pct([r["h"][h] for r in rs], 99))
                                              for h in ("prep", "c2d", "indrv", "po2bl", "bl2end", "total")) +
                  "  (us p50/p90/p99)")
        # reads dispatched directly after a write (consecutive seq, prev is W)
        after_w, runs = [], []
        for s in seqs:
            r = rows[s]
            if r["rw"] != "R" or r["inc"]:
                continue
            n, k = 0, s - 1
            while k in rows and rows[k]["rw"] in "WB":
                n += rows[k]["rw"] == "W"
                k -= 1
            if (s - 1) in rows:
                runs.append(n)
                if n:
                    after_w.append(r)
        if runs:
            print("  reads with a known predecessor: %d, of which right after a write: %d (%.0f%%); write run in front: "
                  "p50 %d p90 %d max %d" % (len(runs), len(after_w), 100.0 * len(after_w) / len(runs),
                                            pct([x for x in runs if x], 50) if after_w else 0,
                                            pct([x for x in runs if x], 90) if after_w else 0, max(runs)))
            if after_w:
                print("  reads right after a write: total p50/p99 %d/%d us, c2d p50/p99 %d/%d us" % (
                    pct([r["h"]["total"] for r in after_w], 50), pct([r["h"]["total"] for r in after_w], 99),
                    pct([r["h"]["c2d"] for r in after_w], 50), pct([r["h"]["c2d"] for r in after_w], 99)))
        # The card's idle wake (found 2026-09-24): a read that follows >= ~6-14 ms
        # of card idle pays ~5.8 ms of c2d. Split the reads by the gap in front.
        if reads:
            slow = [r for r in reads if r["h"]["c2d"] > 4000]
            ok_gap = [r for r in reads if r["h"]["gap"] < 4294000]
            for lo, hi in ((0, 5000), (5000, 14000), (14000, 4294000)):
                b = [r for r in ok_gap if lo <= r["h"]["gap"] < hi]
                if b:
                    print("  reads gap %5.0f-%-7.0f ms: n=%4d  slow(c2d>4ms) %3.0f%%  total p50/p99 %d/%d us" % (
                        lo / 1000.0, hi / 1000.0, len(b), 100.0 * sum(r["h"]["c2d"] > 4000 for r in b) / len(b),
                        pct([r["h"]["total"] for r in b], 50), pct([r["h"]["total"] for r in b], 99)))
            print("  IDLEWAKE reads with c2d > 4 ms: %d of %d (%.0f%%), mean excess c2d %.2f ms -> %.2f ms per read on average" % (
                len(slow), len(reads), 100.0 * len(slow) / len(reads),
                sum(r["h"]["c2d"] - 300 for r in slow) / max(len(slow), 1) / 1000.0,
                sum(r["h"]["c2d"] - 300 for r in slow) / len(reads) / 1000.0))
    print("SDLAT    %s" % get.get("SDLAT", "-"))

    # ---- procs
    samples, cur, name = [], None, None
    for l in lines:
        if l.startswith("PROCS_BEGIN"):
            cur = []
        elif l.startswith("PROCS_END"):
            samples.append(cur)
            cur = None
        elif cur is not None:
            t = l.split()
            if len(t) == 4 and t[0].isdigit():
                cur.append((t[0], t[1], int(t[2]), int(t[3])))
            m = re.match(r"/proc/(\d+)/status:(Name|VmRSS|VmSwap):\s+(\S+)", l)
            if m:
                pid, k, v = m.groups()
                if not cur or cur[-1][0] != pid:
                    cur.append([pid, "?", 0, 0])
                e = cur[-1]
                if k == "Name":
                    e[1] = v
                elif k == "VmRSS":
                    e[2] = int(v)
                else:
                    e[3] = int(v)
    if samples:
        print("ITEM1 per-sample kB: game rss/swap | others rss/swap | top non-game swap")
        for i, s in enumerate(samples):
            g = [x for x in s if x[1].startswith("tyr-quake")]
            o = [x for x in s if not x[1].startswith("tyr-quake")]
            top = sorted(o, key=lambda x: -x[3])[:6]
            print("  s%-2d game %5d/%5d | others %5d/%5d | %s" % (
                i, sum(x[2] for x in g), sum(x[3] for x in g), sum(x[2] for x in o), sum(x[3] for x in o),
                " ".join("%s:%d" % (x[1], x[3]) for x in top)))
    # ---- lvdesk
    on = False
    for l in lines:
        if l.startswith("LVSTAT_BEGIN"):
            on = True
        if on:
            print("  " + l)
        if l.startswith("LVSTAT_END"):
            on = False


if __name__ == "__main__":
    main()
