#!/usr/bin/env python3
"""glref report: compare a suite run with the Mesa references, write report.md.

Called by suite.sh; can be re-run by hand on an existing run directory:
    report.py --run-dir artifacts/gl/RUN --ref-dir artifacts/gl/ref-mesa \
              --impl ours --apps-file tools/glref/apps.txt
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare import compare  # noqa: E402

# worst first: the per-app verdict is the worst of its frames
ORDER = ["MISSING-SYMBOL", "CRASH", "TIMEOUT", "CAPTURE-FAILED", "ERROR", "NOT-BUILT",
         "NO-REF", "FAIL", "PASS", "EXACT"]


def read_status(stem):
    d = {}
    try:
        with open(stem + ".status") as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    d[k] = v
    except FileNotFoundError:
        return None
    return d


def parse_apps(path):
    apps = []
    for line in open(path):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        name, kind, cap, frames, cmd = [c.strip() for c in line.split("|", 4)]
        apps.append(dict(name=name, kind=kind, capture=cap, frames=frames, cmd=cmd))
    return apps


def log_tail(stem, n=6):
    try:
        lines = [l.rstrip() for l in open(stem + ".log", errors="replace") if not l.startswith("#")]
    except FileNotFoundError:
        return []
    return lines[-n:]


def glxinfo_summary(stem):
    keys = ("OpenGL vendor string", "OpenGL renderer string", "OpenGL version string",
            "direct rendering", "GLX version")
    out = []
    try:
        txt = open(stem + ".log", errors="replace").read()
    except FileNotFoundError:
        return out
    for k in keys:
        m = re.search(r"^\s*%s:\s*(.*)$" % re.escape(k), txt, re.M)
        if m:
            out.append("%s: %s" % (k, m.group(1)))
    return out


def classify_run(st):
    """Map a run.sh status to a verdict word, or None if it produced an image/exit."""
    if st is None:
        return "ERROR"
    r = st.get("result", "error")
    return {"missing-symbol": "MISSING-SYMBOL", "crash": "CRASH", "timeout": "TIMEOUT",
            "capture-failed": "CAPTURE-FAILED", "error": "ERROR",
            "not-built": "NOT-BUILT"}.get(r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--ref-dir", required=True)
    ap.add_argument("--impl", default="ours")
    ap.add_argument("--apps-file", required=True)
    ap.add_argument("--frames", default="3 20 60")
    ap.add_argument("--apps", default="")
    ap.add_argument("--run-name", default="")
    ap.add_argument("--tol", type=int, default=16)
    ap.add_argument("--max-bad", type=float, default=1.0)
    o = ap.parse_args()

    sel = o.apps.split()
    apps = [a for a in parse_apps(o.apps_file) if not sel or a["name"] in sel]
    diffdir = os.path.join(o.run_dir, "diff")
    os.makedirs(diffdir, exist_ok=True)
    rows, summary = [], []
    for a in apps:
        frames = ["0"] if a["kind"] == "exit" else (a["frames"] or o.frames).split()
        app_rows = []
        for fr in frames:
            base = "%s.f%s" % (a["name"], fr)
            tstem = os.path.join(o.run_dir, o.impl, base)
            rstem = os.path.join(o.ref_dir, "mesa", base)
            ts, rs = read_status(tstem), read_status(rstem)
            row = dict(app=a["name"], frame=fr, impl_result=(ts or {}).get("result", "absent"),
                       ref_result=(rs or {}).get("result", "absent"),
                       unimplemented=int((ts or {}).get("unimplemented", "0") or 0),
                       missing=(ts or {}).get("missing", "").strip(),
                       unresolved=(ts or {}).get("unresolved", "").strip(), notes=[])
            v = classify_run(ts)
            if ts and ts.get("signal"):
                row["notes"].append("signal %s" % ts["signal"])
            if ts and ts.get("result") == "stalled":
                row["notes"].append("%s stalled at swap %s" % (o.impl, ts.get("meta_swap")))
            if rs and rs.get("result") == "stalled":
                row["notes"].append("mesa stalled at swap %s" % rs.get("meta_swap"))
            if v is None and a["kind"] == "exit":
                v = "PASS" if ts.get("result") == "exit" else "FAIL"
                row["notes"] += glxinfo_summary(tstem)
            elif v is None:
                rp, tp = rstem + ".png", tstem + ".png"
                if not os.path.exists(rp):
                    v = "NO-REF"
                    row["notes"].append("mesa reference: %s" % row["ref_result"])
                else:
                    dp = os.path.join(diffdir, base + ".png")
                    c = compare(rp, tp, o.tol, o.max_bad, dp)
                    row["cmp"] = c
                    v = c["verdict"]
                    if v == "ERROR":
                        row["notes"].append(c["why"])
                        v = "FAIL"
                    if ts.get("meta_swap") != rs.get("meta_swap"):
                        row["notes"].append("captured at swap %s vs mesa %s" %
                                            (ts.get("meta_swap"), rs.get("meta_swap")))
            if row["unimplemented"]:
                row["notes"].append("%d 'libGL: unimplemented' lines" % row["unimplemented"])
                if v in ("PASS", "EXACT", "FAIL"):
                    # an unimplemented call is a missing symbol in all but name
                    row["notes"].append("counted as MISSING-SYMBOL (image alone: %s)" % v)
                    v = "MISSING-SYMBOL"
            if v in ("CRASH", "ERROR", "TIMEOUT", "MISSING-SYMBOL", "CAPTURE-FAILED"):
                row["notes"] += ["log: " + l for l in log_tail(tstem, 4)]
            row["verdict"] = v
            app_rows.append(row)
        worst = min((r["verdict"] for r in app_rows), key=ORDER.index)
        summary.append((a["name"], worst, a["cmd"]))
        rows += app_rows

    rel = lambda p: os.path.relpath(p, o.run_dir)
    counts = {}
    for _, v, _ in summary:
        counts[v] = counts.get(v, 0) + 1
    L = []
    L.append("# glref report: %s" % (o.run_name or os.path.basename(o.run_dir)))
    L.append("")
    L.append("Implementation under test: **%s**; reference: Mesa llvmpipe (cached in `%s`)." %
             (o.impl, os.path.relpath(o.ref_dir, os.path.dirname(o.run_dir))))
    L.append("Xvfb 800x480x16, deterministic time (1/60 s per swap), LD_BIND_NOW=1. "
             "Thresholds: channel tolerance %d/255, tolerant-bad <= %.2f%% to PASS "
             "(see tools/glref/compare.py for why)." % (o.tol, o.max_bad))
    L.append("")
    L.append("**Apps: %d.** " % len(summary) +
             ", ".join("%s %d" % (k, counts[k]) for k in ORDER if k in counts))
    L.append("")
    L.append("| app | verdict | command |")
    L.append("|---|---|---|")
    for n, v, c in summary:
        L.append("| %s | **%s** | `%s` |" % (n, v, c.replace("/src/gl/ref-apps/build/mesa-demos/src/", "")))
    L.append("")
    L.append("## Per frame")
    L.append("")
    L.append("| app | frame | verdict | strict bad %% | tolerant bad %% | max err | mean err | %s run | mesa ref | notes |" % o.impl)
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        c = r.get("cmp")
        if c and "strict_bad_pct" in c:
            nums = "%.3f | %.3f | %d | %.2f" % (c["strict_bad_pct"], c["tolerant_bad_pct"],
                                                 c["max_err"], c["mean_abs_err"])
        else:
            nums = "- | - | - | -"
        notes = "; ".join(x.replace("|", "\\|") for x in r["notes"])
        if r["missing"]:
            notes = ("missing: " + r["missing"] + "; " + notes).rstrip("; ")
        if r.get("unresolved"):
            notes = ("all unresolved GL imports (%d): %s; " % (len(r["unresolved"].split()),
                     r["unresolved"]) + notes).rstrip("; ")
        L.append("| %s | %s | **%s** | %s | %s | %s | %s |" % (
            r["app"], r["frame"], r["verdict"], nums, r["impl_result"], r["ref_result"], notes))
    L.append("")
    L.append("## Images")
    L.append("")
    L.append("Mesa reference, %s, diff (grey = reference, red = tolerant-bad, yellow = "
             "edge-only). EXACT frames have no diff image." % o.impl)
    L.append("")
    for r in rows:
        if r["frame"] == "0":
            continue
        base = "%s.f%s.png" % (r["app"], r["frame"])
        rp = os.path.join(o.ref_dir, "mesa", base)
        tp = os.path.join(o.run_dir, o.impl, base)
        dp = os.path.join(diffdir, base)
        cells = []
        for p in (rp, tp, dp):
            cells.append("![](%s)" % rel(p) if os.path.exists(p) else "-")
        L.append("**%s f%s: %s**  " % (r["app"], r["frame"], r["verdict"]))
        L.append(" ".join(cells))
        L.append("")
    allu = sorted({u for r in rows for u in r.get("unresolved", "").split()})
    if allu:
        i = L.index("## Per frame")
        users = {u: sorted({r["app"] for r in rows if u in r.get("unresolved", "").split()}) for u in allu}
        blk = ["## GL entry points the apps import that %s does not export (%d)" % (o.impl, len(allu)), "",
               "| symbol | apps |", "|---|---|"]
        blk += ["| `%s` | %s |" % (u, ", ".join(users[u])) for u in allu]
        L[i:i] = blk + [""]
    with open(os.path.join(o.run_dir, "report.md"), "w") as f:
        f.write("\n".join(L) + "\n")
    with open(os.path.join(o.run_dir, "report.json"), "w") as f:
        json.dump({"summary": [dict(app=n, verdict=v) for n, v, _ in summary], "rows": rows}, f, indent=1)
    print("report: %s" % os.path.join(o.run_dir, "report.md"))
    for n, v, _ in summary:
        print("  %-18s %s" % (n, v))


if __name__ == "__main__":
    main()
