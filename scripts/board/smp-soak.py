#!/usr/bin/env python3
"""Repeat real resets and bounded cross-CPU checks, retaining complete logs.

Uses the existing console/reset tools. Never run beside another board tool.
Build/deploy rootfs/smpstress first. A passing boot requires executed commands,
two online CPUs, the expected kernel, a completed stress test and no fatal log.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FATAL = re.compile(r"Kernel panic|Oops:|BUG:|Guru Meditation|CPU_LOCKUP|"
                   r"rcu[^\n]*(?:stall|starvation)|unhandled signal|"
                   r"Unable to handle kernel|Call Trace:", re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--boots", type=int, default=5)
    ap.add_argument("--iterations", type=int, default=2000)
    ap.add_argument("--kernel", required=True, help="expected build number, e.g. 284")
    ap.add_argument("--output", type=pathlib.Path, required=True)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    script = out / "check.sh"
    script.write_text(f'''uname -a
echo SMP_ONLINE=$(cat /sys/devices/system/cpu/online)
/root/smpstress {a.iterations} 1
echo SMP_STRESS_RC=$?
echo SMP_WIFI=$(wpa_cli -i wlan0 status | sed -n 's/^wpa_state=//p')
echo SMP_HID=$(cat /sys/kernel/esp32s31-hid/attach)
dmesg
echo SMP_CHECK_COMPLETE
''')
    results = []
    for i in range(1, a.boots + 1):
        raw = out / f"boot-{i}.raw"
        boot = subprocess.run([sys.executable, str(HERE / "alive.py"),
                               "--reset", "--log", str(raw)],
                              capture_output=True, text=True, timeout=110)
        (out / f"boot-{i}.txt").write_text(boot.stdout + boot.stderr)
        fatal = FATAL.findall(raw.read_text(errors="replace"))
        if fatal:
            result = dict(boot=i, passed=False, fatal=fatal, phase="boot")
        else:
            check = subprocess.run([sys.executable, str(HERE / "runsh.py"),
                                    str(script), "60"], capture_output=True,
                                   text=True, timeout=240)
            text = check.stdout + check.stderr
            (out / f"check-{i}.txt").write_text(text)
            fatal = FATAL.findall(text)
            expected = [f"#{a.kernel} SMP", "SMP_ONLINE=0-1",
                        "smpstress: PASS cpus=0,1", "SMP_STRESS_RC=0",
                        "SMP_WIFI=COMPLETED", "SMP_CHECK_COMPLETE"]
            missing = [s for s in expected if s not in text]
            result = dict(boot=i, passed=not missing and not fatal,
                          missing=missing, fatal=fatal, alive_rc=boot.returncode)
        results.append(result)
        (out / "verdict.json").write_text(json.dumps(results, indent=2))
        print(json.dumps(result), flush=True)
        if not result["passed"]:
            return 1  # preserve the failed board for investigation
    return 0


if __name__ == "__main__":
    sys.exit(main())
