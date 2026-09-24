#!/usr/bin/env python3
"""check.py <runsh log> [--ref-min 1.17]

Parses the output of scripts/board/sdtrace-idle.sh (as captured by
runsh.py) and applies the sdtrace validation gate before any hop number is
believed, then prints the idle hop table and the two decision inputs
(B3 stop leg, B4 software legs / above-driver share).

Exit 0 = PASS, 1 = KILL (instrument not trusted), 2 = log unparseable.
"""
import re
import sys

HOPS = ["prep", "i2c", "c2d", "d2x", "i2bh", "x2bh", "bh2st", "stophw",
        "stop", "st2rd", "indrv", "rd2po", "po2bl", "bl2end", "total", "gap"]


def parse(text):
    arms = []
    cur = None
    in_ring = False
    for line in text.splitlines():
        line = line.rstrip("\r")
        m = re.match(r"ARM (\S+) mode=(\d) kb=(\d+) n=(\d+)(?: cpu=(\d))?", line)
        if m:
            cur = {"label": m.group(1), "mode": int(m.group(2)),
                   "kb": int(m.group(3)), "n": int(m.group(4)),
                   "cpu": int(m.group(5)) if m.group(5) is not None else -1,
                   "ring": [], "med": {}, "p90": {}, "max": {}, "avg": {}}
            arms.append(cur)
            continue
        if cur is None:
            continue
        m = re.search(r"min ([0-9.]+)\s+p50 ([0-9.]+)\s+p90 ([0-9.]+)\s+p99 ([0-9.]+)\s+max ([0-9.]+) ms", line)
        if m and line.startswith("SDLAT"):
            cur["min"], cur["p50"], cur["p99"] = float(m.group(1)), float(m.group(2)), float(m.group(4))
            continue
        m = re.match(r"DELTA irq=(\d+) ctxt=(\d+) n=(\d+)", line)
        if m:
            n = int(m.group(3))
            cur["irq_per_req"] = int(m.group(1)) / n
            cur["ctxt_per_req"] = int(m.group(2)) / n
            continue
        m = re.match(r"ring n=(\d+) med/p90/max us:(.*)", line)
        if m:
            cur["ring_n"] = int(m.group(1))
            for h, a, b, c in re.findall(r"(\w+)=(\d+)/(\d+)/(\d+)", m.group(2)):
                cur["med"][h], cur["p90"][h], cur["max"][h] = int(a), int(b), int(c)
            continue
        m = re.match(r"since reset n=(\d+) avg us:(.*)", line)
        if m:
            cur["reset_n"] = int(m.group(1))
            for h, v in re.findall(r"(\w+)=(\d+)", m.group(2)):
                cur["avg"][h] = int(v)
            continue
        m = re.match(r"nirq med (\d+) max (\d+) avg_x100 (\d+) nbh med (\d+) max (\d+) avg_x100 (\d+)", line)
        if m:
            cur["nirq_med"], cur["nirq_avg"] = int(m.group(1)), int(m.group(3)) / 100
            cur["nbh_med"], cur["nbh_avg"] = int(m.group(4)), int(m.group(6)) / 100
            continue
        if line == "RING_BEGIN":
            in_ring = True
            continue
        if line == "RING_END":
            in_ring = False
            continue
        if in_ring:
            m = re.match(r"(\d+) (\d+) (\d+) ([RW]) (\d+)/(\d+)/(\d+) (\d+) (\d+) \|((?: \d+)+)( \(incomplete\))?", line)
            if m:
                vals = [int(x) for x in m.group(10).split()]
                cur["ring"].append({"seq": int(m.group(1)), "op": int(m.group(2)),
                                    "blk": int(m.group(3)), "rw": m.group(4),
                                    "cpu": (int(m.group(5)), int(m.group(6)), int(m.group(7))),
                                    "nirq": int(m.group(8)), "nbh": int(m.group(9)),
                                    "h": dict(zip(HOPS, vals)),
                                    "incomplete": bool(m.group(11))})
    return arms


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ref_min = 1.17
    if "--ref-min" in sys.argv:
        ref_min = float(sys.argv[sys.argv.index("--ref-min") + 1])
    text = open(sys.argv[1], errors="replace").read()
    if "SDTRACE_MISSING" in text:
        print("KILL: kernel has no sdtrace param (wrong kernel flashed - check uname #N)")
        return 1
    if "RS_TIMEKILL" in text or "BOARD KILLED" in text:
        print("KILL: the board killed run.sh - read the log, do not re-run blind")
        return 1
    arms = parse(text)
    if not arms or "RUN_DONE" not in text:
        print("UNPARSEABLE: no ARM blocks or no RUN_DONE - read the log")
        return 2

    verdicts = []

    def kill(msg):
        verdicts.append(("KILL", msg))

    def warn(msg):
        verdicts.append(("WARN", msg))

    def ok(msg):
        verdicts.append(("PASS", msg))

    # hop table
    print("hop table, med/p90 us (ring, last 64 completed):")
    print("%-8s" % "hop" + "".join("%18s" % ("%s m%d %dk c%d" % (a["label"], a["mode"], a["kb"], a["cpu"])) for a in arms))
    for h in HOPS:
        print("%-8s" % h + "".join("%18s" % ("%d/%d" % (a["med"].get(h, -1), a["p90"].get(h, -1))) for a in arms))
    print("%-8s" % "nirq" + "".join("%18s" % ("%s/%.2f" % (a.get("nirq_med", "?"), a.get("nirq_avg", 0))) for a in arms))
    print("%-8s" % "nbh" + "".join("%18s" % ("%s/%.2f" % (a.get("nbh_med", "?"), a.get("nbh_avg", 0))) for a in arms))
    print("%-8s" % "sdlat" + "".join("%18s" % ("%.2f/%.2f" % (a.get("min", 0), a.get("p50", 0))) for a in arms))
    print("%-8s" % "irq/req" + "".join("%18s" % ("%.2f" % a.get("irq_per_req", 0)) for a in arms))
    print("%-8s" % "ctxt/rq" + "".join("%18s" % ("%.2f" % a.get("ctxt_per_req", 0)) for a in arms))
    print()

    for a in arms:
        lab = "%s(mode %d, %dk)" % (a["label"], a["mode"], a["kb"])
        if a.get("ring_n", 0) == 0:
            kill("%s: ring holds 0 completed entries - SDT_END never lands (hook not reached / stub won)" % lab)
            continue
        if a.get("reset_n", 0) < a["n"] * 0.9:
            kill("%s: since-reset n=%d for %d sdlat reads - requests are not being paired to END" % (lab, a.get("reset_n", 0), a["n"]))
        blk = [r for r in a["ring"] if r["blk"] > 0]
        # THE instrument's pass rule: the stamps are monotonic (every link of
        # the serial chain is present) and the chain sums to the request's
        # total within 10%. prep+i2c+c2d+d2x+x2bh+bh2st+stop+st2rd+rd2po+
        # po2bl+bl2end partitions sub -> end when every stamp landed in order
        # (i2bh and stophw overlap other links and are left out). d2x may be
        # 0 legitimately (XFER before DTO in one hardirq) and is not a link.
        chain = ("prep", "i2c", "c2d", "x2bh", "bh2st", "stop", "st2rd",
                 "rd2po", "po2bl", "bl2end")
        done = [r for r in blk if not r["incomplete"] and r["h"]["total"]]
        mono = [r for r in done if all(r["h"][k] > 0 for k in chain)]
        within = [r for r in mono
                  if abs(sum(r["h"][k] for k in chain) + r["h"]["d2x"] - r["h"]["total"])
                  <= 0.10 * r["h"]["total"]]
        if done:
            resid = [abs(sum(r["h"][k] for k in chain) + r["h"]["d2x"] - r["h"]["total"]) for r in mono]
            msg = "%s: chain sums to total within 10%% on %d/%d rows, monotonic on %d/%d (max residual %d us)" % (
                lab, len(within), len(done), len(mono), len(done), max(resid) if resid else -1)
            if len(mono) >= 0.95 * len(done) and len(within) >= 0.95 * len(done):
                ok(msg)
            else:
                kill(msg + " - a stamp lands out of order or a hop is not a partition")
        miss = [r for r in blk if r["h"]["po2bl"] == 0 or r["h"]["bl2end"] == 0]
        if blk and len(miss) > 0.05 * len(blk):
            kill("%s: SDT_BLK/SDT_END missing on %d of %d block entries (>5%%) - mrq back-scan mis-pairing" % (lab, len(miss), len(blk)))
        else:
            ok("%s: block-layer stamps present on %d/%d block entries" % (lab, len(blk) - len(miss), len(blk)))
        if a["kb"] == 4:
            if a["med"].get("stop", 0) == 0:
                kill("%s: stop hop is 0 on 4 KiB reads - this path is NOT CMD18+CMD12; re-derive 0039's cmds_all 2.1 before B3 is discussed" % lab)
            p = a["med"].get("prep", 0)
            if p == 0 or p > 200:
                kill("%s: prep median %d us (expect 30-60 idle) - slot reset after issue / ordering bug" % (lab, p))
            if "nirq_med" in a and "irq_per_req" in a:
                if abs(a["nirq_avg"] - a["irq_per_req"]) > 0.10 * max(a["irq_per_req"], 1e-9):
                    kill("%s: nirq avg %.2f vs /proc/interrupts %.2f per request (>10%%) - hardirqs missed or double-counted" % (lab, a["nirq_avg"], a["irq_per_req"]))
                else:
                    ok("%s: nirq avg %.2f agrees with /proc/interrupts %.2f per request" % (lab, a["nirq_avg"], a["irq_per_req"]))
            if a["mode"] == 2 and "min" in a:
                if a["min"] > ref_min + 0.03:
                    kill("%s: sdlat min %.3f ms is > %.2f + 0.03 - the ring is perturbing the read; cut the three IRQ-entry stamps first" % (lab, a["min"], ref_min))
                else:
                    ok("%s: sdlat min %.3f ms within 0.03 of the #369 reference %.2f" % (lab, a["min"], ref_min))

    m2 = [a for a in arms if a["mode"] == 2 and a["kb"] == 4 and a.get("ring_n")]
    m0 = [a for a in arms if a["mode"] == 0 and a["kb"] == 4 and a.get("ring_n")]
    if m2 and m0:
        r2 = max(a["med"].get("rd2po", 0) for a in m2)
        r0 = min(a["med"].get("rd2po", 0) for a in m0)
        if r0 >= 100 and r2 <= 30 and r0 >= 3 * max(r2, 1):
            ok("kworker hop: rd2po mode0 %d us vs mode2 %d us - the ring sees the known change" % (r0, r2))
        else:
            kill("kworker hop NOT resolved: rd2po mode0 %d us vs mode2 %d us (need >=100 / <=30 / >=3x) - stamps mis-paired or hook unreached" % (r0, r2))
        c0 = min(a.get("ctxt_per_req", 0) for a in m0)
        c2 = max(a.get("ctxt_per_req", 9) for a in m2)
        if c0 > c2:
            ok("ctxt/request mode0 %.2f > mode2 %.2f" % (c0, c2))
        else:
            warn("ctxt/request mode0 %.2f vs mode2 %.2f - expected 3 vs 2" % (c0, c2))
        if all("min" in a for a in m2 + m0):
            dt_ring = (m0[0]["med"].get("total", 0) - sum(a["med"].get("total", 0) for a in m2) / len(m2)) / 1000
            dt_sdlat = m0[0]["min"] - sum(a["min"] for a in m2) / len(m2)
            if dt_sdlat > 0 and abs(dt_ring - dt_sdlat) <= 0.3 * dt_sdlat + 0.02:
                ok("ring total delta %.3f ms vs sdlat min delta %.3f ms (within 30%%)" % (dt_ring, dt_sdlat))
            else:
                warn("ring total delta %.3f ms vs sdlat min delta %.3f ms - not within 30%%; medians vs mins, read the ring lines" % (dt_ring, dt_sdlat))
        same = [x for x in m2 if x["cpu"] == m2[0]["cpu"]]
        a, b = same[0], same[-1]
        for h in ("indrv", "total", "stop"):
            x, y = a["med"].get(h, 0), b["med"].get(h, 0)
            if x and y and abs(x - y) > 0.10 * max(x, y):
                warn("repeat-to-repeat %s median %d vs %d us differ >10%% - re-run the arm before recording" % (h, x, y))

    # decisions
    if m2:
        a = [x for x in m2 if x["cpu"] == m2[0]["cpu"]][-1]
        stop = a["med"].get("stop", 0)
        sw = sum(a["med"].get(h, 0) for h in ("i2bh", "x2bh", "bh2st", "st2rd"))
        above = a["med"].get("total", 0) - a["med"].get("indrv", 0) + a["med"].get("gap", 0)
        print("DECISION B3 (auto-stop): stop leg median %d us (hw %d) -> %s" % (
            stop, a["med"].get("stophw", 0),
            "PROCEEDS (>=150)" if stop >= 150 else "CLOSED (<100)" if stop < 100 else "GREY 100-150: price against the 512 B CMD17 arm"))
        print("DECISION B4 (polled completion): software legs %d us (>=100 needed), above-driver+gap %d us (>=600 needed) -> %s" % (
            sw, above, "PROCEEDS" if sw >= 100 and above >= 600 else "RE-PRICE from the table before any code"))
    print()
    rc = 0
    for v, msg in verdicts:
        print("%s: %s" % (v, msg))
        if v == "KILL":
            rc = 1
    print("OVERALL: %s" % ("KILL - do not record any hop number" if rc else "PASS - the instrument is believed"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
