"""Record the serial console to a file. Read-only: never sends a byte.

WHY THIS EXISTS. Every tool here talks to the board and needs an answer, so
when the board dies mid-workload they all report the same thing - NO_SHELL, or
alive.py's STAGE SILENT - and none of them can say WHY. dmesg is useless for
this by construction: you can only read it from a board that is still alive, so
the one failure worth diagnosing is the one that erases its own evidence. A
kernel panic, a BUG, a hung-task warning and a stack trace all go to the
console at 1 Mbps, and until now nothing was listening.

This listens. It sends nothing, asserts no control line, and never resets - so
it is not another console runner and not another reset sequence. It is the
recorder that should have been here the first time a run wedged.

    conlog.py <out.log> [seconds]

It holds the port for as long as it runs, so a caller that also needs runsh
must fire the workload FIRST (setsid, output to a file on the card), let runsh
exit, and only then start recording. Two readers on one tty steal each other's
bytes.

Prints a line to stdout whenever something alarming appears, so it can be
driven by Monitor and wake a caller at the moment of failure rather than after
a timeout.
"""
import re
import sys
import time

import serial

from console import PORT, BAUD

# What is worth interrupting a run for. Deliberately broad: a missed panic
# costs the whole diagnosis, a false positive costs one line of output.
ALARM = re.compile(
    rb"panic|Oops|BUG:|WARNING:|Unable to handle|call trace|stack trace|"
    rb"hung_task|blocked for more than|rcu_sched|watchdog|"
    rb"Out of memory|oom-kill|segfault|unhandled signal|"
    rb"Kernel panic|CPU\d+: stopping|emergency",
    re.I,
)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: conlog.py <out.log> [seconds]", file=sys.stderr)
        return 2
    out = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0

    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.5)
    except Exception as e:                          # noqa: BLE001
        print(f"conlog: cannot open {PORT}: {e}", file=sys.stderr)
        return 1

    deadline = time.time() + secs
    last = time.time()
    buf = b""
    quiet_reported = False
    with open(out, "wb") as f:
        print(f"conlog: recording {PORT} to {out} for {secs:.0f}s", flush=True)
        while time.time() < deadline:
            try:
                chunk = ser.read(4096)
            except Exception as e:                  # noqa: BLE001
                print(f"conlog: read failed: {e}", flush=True)
                break
            if chunk:
                f.write(chunk)
                f.flush()
                last = time.time()
                quiet_reported = False
                buf += chunk
                # Scan whole lines only, so a pattern split across two reads
                # is still caught.
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if ALARM.search(line):
                        print("ALARM " + line.decode("utf-8", "replace")[:300],
                              flush=True)
            else:
                # Silence is itself a finding when it follows traffic: that is
                # the shape of a wedge, as opposed to an orderly idle.
                gap = time.time() - last
                if gap > 20 and not quiet_reported:
                    print(f"QUIET no console output for {gap:.0f}s", flush=True)
                    quiet_reported = True
    ser.close()
    print("conlog: done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
