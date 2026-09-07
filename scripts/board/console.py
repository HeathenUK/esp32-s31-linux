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
import atexit
import errno
import fcntl
import os
import time

import serial

PORT, BAUD = '/dev/cu.usbserial-130', 1000000

# ---------------------------------------------------------------- port lock
#
# ONE PROCESS AT A TIME OWNS THE SERIAL PORT.
#
# Two readers on one tty steal each other's bytes, so a probe that runs while
# another tool holds the port CANNOT succeed - and every tool here reports that
# failure as NO_SHELL or STAGE SILENT, which is indistinguishable from a dead
# board. On 2026-09-07 a liveness check was run against a board that a
# background A/B job was already driving; it reported the board dead, and the
# board was fine. That is not a bug in the probe, it is a missing lock.
#
# Advisory flock, so it costs nothing and disappears if a process is killed.
# The holder writes its pid and argv into the file, so the error can say WHO
# has the port rather than just refusing.
LOCKFILE = os.environ.get('S31_PORT_LOCK', '/tmp/s31-serial-port.lock')


class PortBusy(RuntimeError):
    """Raised instead of letting a caller misread contention as a dead board."""


_lock_fh = None


def take_port_lock(what='', block=0.0):
    """Claim the serial port. Raises PortBusy with the holder's identity.

    block=N waits up to N seconds for the holder to finish, which is what a
    queued measurement wants; the default fails immediately, which is what an
    interactive probe wants - waiting silently is how a wedged tool looks like
    a wedged board.
    """
    global _lock_fh

    if _lock_fh is not None:              # already ours, re-entrant
        return
    fh = open(LOCKFILE, 'a+')
    deadline = time.time() + block
    while True:
        try:
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError as e:
            if e.errno not in (errno.EACCES, errno.EAGAIN):
                raise
            if time.time() >= deadline:
                fh.seek(0)
                holder = fh.read().strip() or '(unknown)'
                fh.close()
                # Deliberately does NOT contain the literal NO_SHELL. That
                # token is a VERDICT about the board, and tooling greps for
                # it; putting it inside an explanation makes a busy port look
                # like a dead board to exactly the callers this is protecting.
                raise PortBusy(
                    'SERIAL PORT BUSY - held by %s.\n'
                    '  This is NOT a dead board. Two readers on one tty steal\n'
                    "  each other's bytes, so any probe run now reports a\n"
                    '  no-shell verdict whatever the board is doing. Wait for\n'
                    '  the holder to finish, or stop it, then retry.' % holder)
            time.sleep(0.25)
    fh.seek(0)
    fh.truncate()
    fh.write('pid %d: %s' % (os.getpid(), what or ' '.join(os.sys.argv)))
    fh.flush()
    _lock_fh = fh
    atexit.register(release_port_lock)


def release_port_lock():
    global _lock_fh

    if _lock_fh is not None:
        try:
            fcntl.flock(_lock_fh, fcntl.LOCK_UN)
            _lock_fh.close()
        except Exception:
            pass
        _lock_fh = None


def open_port(timeout=0.05, what='', block=0.0):
    """serial.Serial(), but only if nothing else is driving the board."""
    take_port_lock(what=what, block=block)
    return serial.Serial(PORT, BAUD, timeout=timeout)

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
    p = open_port(timeout=0.05, what='console.shell()')
    try:
        return p, wait_for_shell(p, cap)
    except NoShell:
        p.close()
        raise
