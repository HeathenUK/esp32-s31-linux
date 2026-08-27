"""Get a shell on the board's serial console, reliably.

Every script here needs the same three things and only `runsh` used to do all
of them, which is why `deploy_bin`, `screenshot` and `screenshot-hw` failed on
any freshly booted board and "worked" only if a `runsh` call had logged in
first. That failure is indistinguishable from a dead board, and it has been
misread as one more than once - so it lives in one place now.

The three things:

1. **Log in.** A booted board sits at `login:`, not at a shell. Waiting for a
   `#` prompt that will never come is a guaranteed timeout, however long it is.
2. **Prod.** A board idle at a prompt sends nothing at all, so silence is not
   evidence of anything until it has been asked.
3. **Track the boot instead of sleeping through it.** Reaching a prompt from a
   hard reset takes ~85 s here (X and the desktop start on the way), which is
   longer than the 75 s the callers used to allow - so a reset immediately
   followed by a deploy always failed. Rather than raise a fixed number and
   hope, extend the deadline while bytes are still arriving: a board making
   progress is given time, a silent one fails fast.

`rstrip().endswith('# ')` can never be true - rstrip removes the trailing space
it then tests for. Match on '#'.
"""
import time

import serial

PORT, BAUD = '/dev/cu.usbserial-130', 1000000

# How long to wait for the *first* byte before calling the board dead.
#
# This has to be generous, because **the board prints at two different bauds**.
# The second-stage bootloader talks at 115200 and only then does the console
# switch to 1 Mbps - verified by reading the same reset at 115200 and getting
# clean text ("SPI Mode : QIO", the partition table) where 1 Mbps gives
# unreadable bytes, or none at all once the UART starts throwing framing
# errors. So for the first seconds after a reset a perfectly healthy board can
# look completely silent at our baud.
#
# Saying "off, held in reset, or in download mode" during that window is the
# most misleading thing this tooling can do, and it has sent diagnosis down the
# wrong path more than once. Once *any* byte has arrived the board is
# demonstrably alive and only HARD_CAP applies - quiet gaps mid-boot (the
# loader runs silently) are normal and must not be read as death.
QUIET_GIVE_UP = 25.0
HARD_CAP = 180.0


class NoShell(Exception):
    """Raised with a diagnosis, not just a failure."""


def read_until(p, pred, limit, prod=None, every=2.0, buf=''):
    t, last = time.time(), 0.0
    while time.time() - t < limit:
        buf += p.read(8192).decode('utf-8', 'replace')
        if pred(buf):
            return buf, True
        if prod and time.time() - last > every:
            p.write(prod)
            last = time.time()
    return buf, pred(buf)


def _at_shell(b):
    return b.rstrip().endswith('#')


def wait_for_shell(p, cap=HARD_CAP, user='root'):
    """Block until the board offers a shell, logging in if it offers a login.

    Returns the console text seen. Raises NoShell with a diagnosis that says
    whether the board was talking or silent, because those mean opposite things.
    """
    start = time.time()
    buf = ''

    while True:
        buf, hit = read_until(p, lambda b: _at_shell(b) or 'login:' in b,
                              2.0, prod=b'\n', buf=buf)
        if hit:
            break
        if not buf and time.time() - start > QUIET_GIVE_UP:
            raise NoShell('not one byte in %.0fs - board is off, held in '
                          'reset, or in download mode' % QUIET_GIVE_UP)
        if time.time() - start > cap:
            raise NoShell('no prompt after %.0fs, but the board is emitting '
                          'bytes (%d) - still booting or wedged mid-boot'
                          % (cap, len(buf)))

    if _at_shell(buf):
        return buf

    p.write((user + '\n').encode())
    tail, ok = read_until(p, lambda b: '# ' in b or 'assword' in b, 20.0)
    if 'assword' in tail:
        p.write(b'\n')
        tail, ok = read_until(p, _at_shell, 20.0, buf=tail)
    if '# ' not in tail and not _at_shell(tail):
        raise NoShell('reached "login:" but the shell never came up')
    return buf + tail


def open_shell(cap=HARD_CAP):
    """Open the console and return (port, banner) with a shell ready."""
    p = serial.Serial(PORT, BAUD, timeout=0.05)
    try:
        return p, wait_for_shell(p, cap)
    except NoShell:
        p.close()
        raise
