#!/usr/bin/env python3
"""Run a command holding the board's serial-port lock.

    withlock.py [--wait SECONDS] -- <command> [args...]

WHY. Everything in scripts/board/ goes through console.open_port() and cannot
collide any more, but the Makefile shells out to esptool directly and knows
nothing about that lock. A flash or a reset landing inside somebody else's
measurement is the WORST collision available: a contending read only steals
bytes, whereas this reboots the board under them and rewrites its flash, so
the run they were timing silently becomes a run of something else - and
nothing in the output says so.

Exit status is the command's, so `make` still fails when a flash fails.

--wait N queues instead of failing, which is what a flash usually wants: it is
better to start 40 s late than to corrupt a measurement or to make the user
re-run the build. The default fails immediately and names the holder.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console


def main(argv):
    wait = 0.0
    if argv and argv[0] == '--wait':
        wait = float(argv[1])
        argv = argv[2:]
    if argv and argv[0] == '--':
        argv = argv[1:]
    if not argv:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        console.take_port_lock(what='withlock: %s' % ' '.join(argv[:3]),
                               block=wait)
    except console.PortBusy as e:
        print(str(e), file=sys.stderr)
        print('\n  Refusing to flash or reset while the board is in use.',
              file=sys.stderr)
        return 3
    try:
        return subprocess.call(argv)
    finally:
        console.release_port_lock()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
