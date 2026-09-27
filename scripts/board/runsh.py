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


def run(path, timeout=240, boot_wait=0, shell_wait=75.0,
        done_token=None, done_timeout=600, done_regex=None, live=False):
    # boot_wait is a MINIMUM BOARD UPTIME in seconds, not a sleep. It used to
    # be time.sleep(boot_wait) before the port was even opened, so the timedemo
    # harness's `240 170` slept 170 s after every reset although getty is up
    # at ~31 s - two thirds of every arm - and every one-off call that passed
    # a third argument paid it on an already-booted board (2026-09-25). Now
    # the wait happens after the shell is reached, only for the part of it the
    # board has not already been up for: settle time for a fresh boot, zero
    # for a running board.
    import re
    completion_pattern = re.compile(done_regex) if done_regex else None
    p = console.open_port(timeout=0.05, what='runsh.py')

    def until(pred, limit, prod=None, every=2.0, emit=False):
        """Read until pred(buffer) holds. `prod` pokes a board that is silent
        because it is idle at a prompt, not because it is broken."""
        t, last, o = time.time(), 0.0, ''
        while time.time() - t < limit:
            chunk = p.read(8192).decode('utf-8', 'replace')
            o += chunk
            if emit and chunk:
                print(chunk, end='', file=sys.stderr, flush=True)
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

    if boot_wait:
        o, got = cmd('cut -d. -f1 /proc/uptime', 'UT_OK')
        up = None
        for line in o.splitlines():
            if line.strip().isdigit():
                up = int(line.strip())
        if up is not None and up < boot_wait:
            time.sleep(boot_wait - up)

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

    # BOARD-SIDE WATCHDOG. The script must not outlive this call.
    #
    # runsh gives up after `timeout`, but the board's login shell keeps running
    # whatever was started - so the console stays occupied and every later tool
    # reports NO_SHELL, which is indistinguishable from a dead board. That has
    # happened repeatedly (a 1.45 MB copy, a `find /` over the SD card) and
    # each time it was diagnosed as a wedged board.
    #
    # So the board kills it too. The watchdog fires just before the host's
    # deadline, so the caller can receive an explicit timeout marker,
    # rather than leaving silence to be interpreted.
    # The marker is spelled RS_TIME"K"ILL in the command so the literal
    # RS_TIMEKILL appears ONLY in the board's output, never in the echoed
    # command line - otherwise every normal run reports a timekill, which is
    # how a safety net turns into noise everyone learns to ignore.
    #
    # FIRE BEFORE runsh GIVES UP, not after.
    #
    # This was timeout+10, which meant the board killed the script ten seconds
    # AFTER runsh had already stopped listening - so the console was freed
    # (the important half) but RS_TIMEKILL was never seen and the caller still
    # got unexplained silence, which is the whole failure being fixed. Firing
    # a few seconds early means the marker lands inside the window and the
    # caller is TOLD what happened. Found by deliberately overrunning it.
    guard = max(5, int(timeout) - 3)
    #
    # ANNOUNCE, THEN KILL. `kill && echo` loses a race: `wait` returns the
    # instant the script dies and the main shell kills the watchdog before it
    # reaches the echo, so the console frees up but the caller is told
    # nothing - which is the exact failure this is meant to remove. `kill -0`
    # first, so a script that finished normally is never announced.
    p.write(('sh /tmp/r.sh 2>&1 & __rp=$!; '
             # Reap the sleep as well as its shell. Killing only the subshell
             # left an orphan sleep (and its memory) after every short call.
             "( trap 'kill \"$__rs_sleep\" 2>/dev/null; wait \"$__rs_sleep\" 2>/dev/null; exit' TERM INT; "
             'sleep %d & __rs_sleep=$!; wait "$__rs_sleep"; kill -0 $__rp 2>/dev/null && '
             '{ echo RS_TIME"K"ILL; kill -9 $__rp 2>/dev/null; } ) & __rw=$!; '
             'wait $__rp 2>/dev/null; __rc=$?; kill $__rw 2>/dev/null; wait $__rw 2>/dev/null; '
             'printf "RS_EXIT:%%s\\n" "$__rc"; '
             'echo RS_DONE\n' % guard).encode())
    o, _ = until(lambda b: 'RS_DONE' in b.split('echo RS_DONE')[-1], timeout)
    if 'RS_TIMEKILL' in o:
        p.close()
        return o + ('\n[runsh] THE BOARD KILLED THIS SCRIPT at %d s.\n'
                    'Its effects are HALF-APPLIED; this is a harness timeout,\n'
                    'not evidence of a dead board. No completion wait.\n' % guard)
    status = re.search(r'^RS_EXIT:(\d+)\s*$', o, re.MULTILINE)
    if not status or int(status.group(1)) != 0:
        p.close()
        reason = 'missing launcher exit status' if not status else 'launcher exit ' + status.group(1)
        return o + '\n[runsh] LAUNCH_FAILED: ' + reason + '; not waiting for detached completion\n'
    # Optional detached-work completion: read ONLY, no probes/commands, and
    # keep the same port open so a fast job's notification cannot be lost
    # between launch and a second reader. Workload emits token after exit.
    if (done_token or done_regex) and 'RS_TIMEKILL' not in o:
        # Expose evidence while waiting instead of buffering the entire job.
        # Host output only: no extra board command, process or serial write.
        if live:
            print('[runsh] launch transcript (not workload completion):\n' + o,
                  file=sys.stderr, flush=True)
        def completed(buf):
            return any((line.strip() == done_token if done_token else
                        completion_pattern.fullmatch(line.strip())) for line in buf.splitlines())
        if not completed(o):
            more, got = until(completed, done_timeout, emit=live)
            o += more
            if not got:
                o += '\n[runsh] COMPLETION_NOT_RECEIVED: %s (no workload result claimed)\n' % (done_token or done_regex)
    p.close()
    return o


if __name__ == '__main__':
  try:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('script')
    ap.add_argument('timeout', type=int, nargs='?', default=240)
    ap.add_argument('boot_wait', type=int, nargs='?', default=0)
    completion = ap.add_mutually_exclusive_group()
    completion.add_argument('--done', help='exact console line emitted after detached work exits; passive read only')
    completion.add_argument('--done-regex', help='full console-line regex, for an application native completion message')
    ap.add_argument('--done-timeout', type=float, default=600)
    args = ap.parse_args()
    result = run(args.script, args.timeout, args.boot_wait,
                 done_token=args.done, done_timeout=args.done_timeout, done_regex=args.done_regex,
                 live=bool(args.done or args.done_regex))
    print(result)
    if 'COMPLETION_NOT_RECEIVED:' in result:
        sys.exit(4)
    if ('LAUNCH_FAILED:' in result or 'RS_TIMEKILL' in result or
            result.startswith(('NO_SHELL (', 'UPLOAD_STALLED ', 'UPLOAD_FAILED'))):
        sys.exit(5)
  except console.PortBusy as e:
    # Expected condition, not a crash - a traceback here buries the one line
    # that says what to do, and its noise is what callers end up grepping.
    print(str(e), file=sys.stderr)
    sys.exit(3)
