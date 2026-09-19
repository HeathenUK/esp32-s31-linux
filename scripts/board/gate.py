#!/usr/bin/env python3
"""The fast regression gate. Run it after every flash; it is cheap enough.

    gate.py                 contract + smoke + perf canary      (~2.5 min)
    gate.py --quick         contract + smoke only               (~40 s)
    gate.py --reset         reset first, wait for the desktop, then gate
    gate.py --update-baseline   accept the canary numbers as the new baseline

WHY THIS EXISTS. The two most expensive incidents in September were not
hardware: CONFIG_FUTEX silently vanished from the build (2.25x slower SDL2),
and the USB DMA/full-speed flags were lost across rebuilds (mouse never
appeared). Both were "load-bearing configuration that nothing asserted". The
Makefile now asserts them at build time; this asserts them ON THE BOARD, where
a stale flash, a hand-edited card, or an old lvdesk.env can undo any of them.

THREE STAGES, EACH REUSING WHAT ALREADY EXISTS:

  1. contract   ONE runsh round trip. Kernel build number matches
                images/xipImage; cmdline carries the USB contract; ISA has
                a real FPU; CMA is the configured size; futex works (probed
                by sdlbench's own probe); codec present and the microphone
                sidetone is NOT routed to the DAC; input devices, Wi-Fi,
                memory and the desktop arm reported.
  2. smoke      scripts/board/smoke.py, unchanged: desktop maps, repaints,
                no OOM, no crash.
  3. canary     two sdlbench cases, three repeats each, through
                sdlbench.py's settle gate, compared with gate-baseline.json.
                FAIL above baseline*(1+tol). Faster is reported, not failed.

Everything prints one line per check and one verdict. Artifacts go to
artifacts/gate/<timestamp>/.
"""
import argparse
import json
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

BASELINE = HERE / "gate-baseline.json"

# The cmdline contract. Each of these was lost at least once and cost a day.
CMDLINE_REQUIRED = ["dwc2.desc_dma=0", "dwc2.host_full_speed=1", "usbcore.autosuspend=-1"]
CMDLINE_FORBIDDEN = ["profile=6", "dwc2.sof_irq=1"]
CMA_KB = 5120

CANARY = [  # (label, sdl, case, timers) - the two cases the futex incident moved most
    ("sdl1-indexed", 1, "indexed_frame", False),
    ("sdl2-indexed-timers", 2, "indexed_frame", True),
]

CONTRACT = r'''
ok() { echo "CHK|$1|PASS|$2"; }
no() { echo "CHK|$1|FAIL|$2"; }
nf() { echo "CHK|$1|INFO|$2"; }
echo "CHK|uname|INFO|$(uname -r) $(uname -v)"
echo "CHK|cmdline|INFO|$(cat /proc/cmdline)"
echo "CHK|isa|INFO|$(sed -n 's/^isa[ \t]*: //p' /proc/cpuinfo | head -1)"
echo "CHK|meminfo|INFO|$(awk '/MemTotal|MemAvailable|CmaTotal|CmaFree/{printf "%s%s ", $1, $2}' /proc/meminfo)"
echo "CHK|env|INFO|$(tr '\n' ' ' < /etc/lvdesk.env 2>/dev/null)"
echo "CHK|tcp|INFO|rmem=$(cat /proc/sys/net/ipv4/tcp_rmem | tr '\t' ',') max=$(cat /proc/sys/net/core/rmem_max)"
W=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')
[ "$W" = COMPLETED ] && ok "wifi associated" "$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^ip_address=//p')" || no "wifi associated" "state=${W:-none}"
K=$(grep -c 'Handlers=.*kbd' /proc/bus/input/devices); M=$(grep -c 'Handlers=.*mouse' /proc/bus/input/devices)
nf "input devices" "kbd=$K mouse=$M: $(sed -n 's/^N: Name="\(.*\)"/\1/p' /proc/bus/input/devices | tr '\n' ';')"
grep -q '^ 0 \[Korvo1' /proc/asound/cards 2>/dev/null && ok "codec card 0" "" || no "codec card 0" "$(head -1 /proc/asound/cards 2>/dev/null)"
S=$(amixer -c 0 sget 'ADC2DAC Mixer' 2>/dev/null | sed -n 's/.*Mono: \([0-9]*\) .*/\1/p')
if [ -z "$S" ]; then no "sidetone off (ADC2DAC Mixer=0)" "control not found"
elif [ "$S" = 0 ]; then ok "sidetone off (ADC2DAC Mixer=0)" ""
else no "sidetone off (ADC2DAC Mixer=0)" "ADC2DAC Mixer=$S - the volume slider is driving the microphone into the speaker"; fi
if [ -x /root/sdlbench1 ]; then
	F=$(DISPLAY=:0 /root/sdlbench1 --case idle_wait --frames 1 --warmup 0 2>/dev/null | grep '"type":"futex"' | grep -c '"supported":true')
	[ "$F" = 2 ] && ok "futex" "both probes EAGAIN" || no "futex" "only $F of 2 probes supported - CONFIG_FUTEX lost?"
else
	no "futex" "/root/sdlbench1 missing - deploy it (sdlbench.py --deploy)"
fi
echo "CHK|md5|INFO|$(md5sum /root/sdlbench1 /root/sdlbench2 /usr/bin/lvdesk 2>/dev/null | awk '{printf "%s=%s ", $2, substr($1,1,8)}')"
echo "CHK|END|"
'''


def say(s=""):
    print(s, flush=True)


def image_build_number():
    img = ROOT / "images" / "xipImage"
    if not img.exists():
        return None
    data = img.read_bytes()
    m = re.search(rb"#(\d+) [A-Z][a-z]{2} [A-Z][a-z]{2} +\d+ [\d:]+ UTC \d{4}", data)
    return m.group(1).decode() if m else None


def run_board(script, timeout):
    # A subprocess, not an in-process runsh.run(): the port flock lives for
    # the life of the process that took it, and the next stage (smoke.py)
    # then reports SERIAL PORT BUSY held by ourselves.
    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
        f.write(script)
        path = f.name
    r = subprocess.run([sys.executable, str(HERE / "runsh.py"), path, str(timeout)], text=True, capture_output=True)
    return r.stdout + r.stderr


def stage_contract(out, results):
    raw = run_board(CONTRACT, 60)
    (out / "contract.log").write_text(raw)
    rows = re.findall(r"^CHK\|([^|]*)\|([^|]*)\|?(.*)$", raw, re.M)
    info = {n: d for n, s, d in rows if s == "INFO"}
    if not any(n == "END" for n, _, _ in rows):
        results.append(("contract", "board answered", "FAIL", "no END marker - see contract.log"))
        return
    # host-side judgements on the reported facts
    want = image_build_number()
    have = re.search(r"#(\d+)", info.get("uname", ""))
    if want and have:
        results.append(("contract", "kernel build matches images/xipImage", "PASS" if have.group(1) == want else "FAIL",
                        f"board #{have.group(1)} image #{want}"))
    else:
        results.append(("contract", "kernel build matches images/xipImage", "FAIL", f"board={info.get('uname')!r} image=#{want}"))
    cl = info.get("cmdline", "")
    missing = [t for t in CMDLINE_REQUIRED if t not in cl.split()]
    present = [t for t in CMDLINE_FORBIDDEN if t in cl.split()]
    results.append(("contract", "cmdline USB contract", "PASS" if not missing and not present else "FAIL",
                    ("missing " + " ".join(missing) if missing else "") + (" forbidden " + " ".join(present) if present else "")))
    isa = info.get("isa", "")
    results.append(("contract", "ISA has hardware float", "PASS" if re.match(r"rv32im?af", isa) else "FAIL", isa))
    mem = dict(re.findall(r"(\w+):(\d+)", info.get("meminfo", "")))
    results.append(("contract", f"CMA is {CMA_KB} kB", "PASS" if mem.get("CmaTotal") == str(CMA_KB) else "FAIL",
                    f"CmaTotal={mem.get('CmaTotal')} CmaFree={mem.get('CmaFree')}"))
    results.append(("contract", "arm (/etc/lvdesk.env)", "INFO", info.get("env", "").strip() or "(empty - stock arm)"))
    results.append(("contract", "tcp", "INFO", info.get("tcp", "")))
    results.append(("contract", "input devices", "INFO", info.get("input devices", "")))
    for n, s, d in rows:
        if s in ("PASS", "FAIL") and n not in ("END",):
            results.append(("contract", n, s, d))
    # sdlbench binaries on the board must be the ones in the tree, or the canary compares apples to oranges
    md5s = dict(re.findall(r"(\S+)=([0-9a-f]{8})", info.get("md5", "")))
    for v in (1, 2):
        local = ROOT / "rootfs" / f"sdlbench{v}.bin"
        if local.exists():
            import hashlib
            h = hashlib.md5(local.read_bytes()).hexdigest()[:8]
            results.append(("contract", f"sdlbench{v} on board is the tree's", "PASS" if md5s.get(f"/root/sdlbench{v}") == h else "FAIL",
                            f"board {md5s.get(f'/root/sdlbench{v}')} tree {h}"))


def stage_smoke(out, results):
    r = subprocess.run([sys.executable, str(HERE / "smoke.py")], text=True, capture_output=True)
    (out / "smoke.log").write_text(r.stdout + r.stderr)
    for line in r.stdout.splitlines():
        m = re.match(r"\s+(PASS|FAIL)\s+(.*?)(?:\s{3}(.*))?$", line)
        if m:
            results.append(("smoke", m.group(2), m.group(1), m.group(3) or ""))
        elif line.startswith("smoke: the board did not finish"):
            results.append(("smoke", "smoke finished", "FAIL", "board did not finish - see smoke.log"))


def stage_canary(out, results, repeats, update):
    base = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    fresh = {}
    for label, sdl, case, timers in CANARY:
        d = out / f"canary-{label}"
        cmd = [sys.executable, str(HERE / "sdlbench.py"), "--case", case, "--sdl", str(sdl), "--frames", "60",
               "--repeats", str(repeats), "--require-futex", "--output", str(d)]
        if timers:
            cmd.append("--timers")
        r = subprocess.run(cmd, text=True, capture_output=True)
        (out / f"canary-{label}.log").write_text(r.stdout + r.stderr)
        res = d / "results.jsonl"
        if not res.exists():
            results.append(("canary", label, "FAIL", "no results - see log"))
            continue
        rows = [json.loads(x) for x in res.read_text().splitlines() if x.strip()]
        bad_runs = {x["run"] for x in rows if x["type"] == "run_status" and not x["ok"]}
        means = [x["mean_ns"] / 1e6 for x in rows if x["type"] == "measure" and x["run"] not in bad_runs]
        p95s = [x["p95_ns"] / 1e6 for x in rows if x["type"] == "measure" and x["run"] not in bad_runs]
        if bad_runs or not means:
            results.append(("canary", label, "FAIL", f"{len(bad_runs)} failed runs (exit 90 = desktop died)"))
            continue
        med, p95 = statistics.median(means), statistics.median(p95s)
        fresh[label] = {"mean_ms": round(med, 3), "p95_ms": round(p95, 3), "tol": base.get(label, {}).get("tol", 0.15),
                        "spread_ms": [round(min(means), 3), round(max(means), 3)]}
        b = base.get(label)
        if not b:
            results.append(("canary", label, "INFO", f"mean {med:.3f} ms (range {min(means):.3f}-{max(means):.3f}); no baseline yet"))
            continue
        lim = b["mean_ms"] * (1 + b.get("tol", 0.15))
        delta = (med / b["mean_ms"] - 1) * 100
        st = "FAIL" if med > lim else "PASS"
        note = f"mean {med:.3f} ms vs baseline {b['mean_ms']:.3f} ({delta:+.1f}%), p95 {p95:.3f}, range {min(means):.3f}-{max(means):.3f}"
        if st == "PASS" and delta < -b.get("tol", 0.15) * 100:
            note += "  << faster than baseline; --update-baseline if intended"
        results.append(("canary", label, st, note))
    if update and fresh:
        base.update(fresh)
        base["_updated"] = time.strftime("%Y-%m-%d %H:%M")
        base["_kernel"] = image_build_number()
        BASELINE.write_text(json.dumps(base, indent=1) + "\n")
        results.append(("canary", "baseline updated", "INFO", str(BASELINE.relative_to(ROOT))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="contract + smoke only")
    ap.add_argument("--reset", action="store_true", help="reset the board first and wait for the desktop")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--output", type=pathlib.Path)
    a = ap.parse_args()
    out = a.output or ROOT / "artifacts" / "gate" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    if a.reset:
        say("reset: waiting for the desktop (board-ok.sh)")
        subprocess.run([sys.executable, str(HERE / "reset.py")], capture_output=True)
        r = subprocess.run(["bash", str(HERE / "board-ok.sh"), "110"], text=True, capture_output=True)
        say("  " + (r.stdout.strip().splitlines() or ["?"])[-1])
        if r.returncode == 2:
            say("GATE FAIL: board never came up")
            return 2
    results = []
    say("contract:")
    stage_contract(out, results)
    say("smoke:")
    stage_smoke(out, results)
    if not a.quick:
        say(f"canary: {len(CANARY)} cases x {a.repeats}")
        stage_canary(out, results, a.repeats, a.update_baseline)
    bad = 0
    for stage, name, st, detail in results:
        if st != "INFO":
            bad += st == "FAIL"
        say(f"  {st:4}  [{stage}] {name}" + (f"   {detail}" if detail else ""))
    n = sum(1 for _, _, s, _ in results if s != "INFO")
    say(f"GATE {'FAIL' if bad else 'PASS'}: {n - bad}/{n} in {time.time() - t0:.0f}s  ({out.relative_to(ROOT)})")
    (out / "verdict.json").write_text(json.dumps([dict(stage=s, name=n, status=st, detail=d) for s, n, st, d in results], indent=1))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
