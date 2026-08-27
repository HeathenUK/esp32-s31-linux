"""Run a script on the board, watching the console rather than waiting on it.

Two failure modes have repeatedly cost hours here, and both come from guessing
at timing instead of reading what the board says.

**Kernel messages landing on the login line.** The LCD driver prints its
mode-set messages exactly when getty shows its prompt, so the prompt is not the
last thing in the buffer and an end-of-buffer match fails. Search the whole
buffer. (Note `rstrip().endswith('# ')` can never be true - rstrip removes the
trailing space it then tests for. That spelling silently made every prompt
check fail, so the wait always ran to its limit.)

**Racing the boot.** Reaching getty from a hard reset takes ~30 s. A fixed
retry budget shorter than that reports NO_SHELL for a perfectly healthy board,
which reads exactly like a dead one - three misdiagnoses in one session. But
the fix is not a longer sleep: the board announces every state it enters, so
wait for the announcement. A booted board answers in well under a second, a
booting one the moment getty appears, and neither costs a fixed delay.

Every step here is therefore send-then-wait-for-a-token. The timeouts are
backstops for a board that has genuinely died, not the normal path.
"""
import base64, os, sys, time, serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console

PORT, BAUD = '/dev/cu.usbserial-130', 1000000


def run(path, timeout=240, boot_wait=0, shell_wait=75.0):
    if boot_wait:
        time.sleep(boot_wait)
    p = serial.Serial(PORT, BAUD, timeout=0.05)

    def until(pred, limit, prod=None, every=2.0):
        """Read until pred(buffer) holds. `prod` pokes a board that is silent
        because it is idle at a prompt, not because it is broken."""
        t, last, o = time.time(), 0.0, ''
        while time.time() - t < limit:
            o += p.read(8192).decode('utf-8', 'replace')
            if pred(o):
                return o, True
            if prod and time.time() - last > every:
                p.write(prod); last = time.time()
        return o, pred(o)

    def cmd(c, tok, limit=20.0):
        p.write(('%s; echo %s\n' % (c, tok)).encode())
        # Ignore the echoed command line; match the token on a line of its own.
        return until(lambda b: ('\n' + tok) in b or b.startswith(tok), limit)

    # Reaching a prompt from a hard reset takes ~85 s here, so any fixed window
    # shorter than that turns "reset, then run" into a guaranteed NO_SHELL -
    # which is why the first call after a reset used to fail and the second
    # succeed. console.wait_for_shell extends its deadline while bytes are
    # still arriving, so a booting board is tracked and a silent one still
    # fails fast. shell_wait is kept as the floor for callers that pass one.
    try:
        out = console.wait_for_shell(p, cap=max(shell_wait, console.HARD_CAP))
    except console.NoShell as e:
        p.close()
        return 'NO_SHELL (%s)' % e

    # Quiet the console so kernel messages do not interleave into the results.
    cmd('dmesg -n 1', 'Q_OK')
    cmd('rm -f /tmp/r.b64', 'R_OK')
    b64 = base64.b64encode(open(path, 'rb').read()).decode()
    for i in range(0, len(b64), 400):
        _, got = cmd("printf %%s '%s' >> /tmp/r.b64" % b64[i:i+400], 'C_OK', 10.0)
        if not got:
            p.close(); return 'UPLOAD_STALLED at byte %d' % i
    _, got = cmd('base64 -d /tmp/r.b64 > /tmp/r.sh', 'UP_OK', 15.0)
    if not got:
        p.close(); return 'UPLOAD_FAILED'

    p.write(b'sh /tmp/r.sh 2>&1; echo RS_DONE\n')
    o, _ = until(lambda b: 'RS_DONE' in b.split('echo RS_DONE')[-1], timeout)
    p.close()
    return o


if __name__ == '__main__':
    print(run(sys.argv[1],
              int(sys.argv[2]) if len(sys.argv) > 2 else 240,
              int(sys.argv[3]) if len(sys.argv) > 3 else 0))
