"""Ship a gzipped binary to the board over the 1 Mbps console, in printf chunks.

Input is gzip+base64; the board gunzips on arrival. For anything large prefer
the network - at 1 Mbps with a round trip per chunk this is slow, and the whole
point of the opkg feed is to stop moving files this way.

Imports from the sibling runsh, deliberately: this used to import _wait_prompt
from a hardcoded scratchpad path, so rewriting runsh silently broke it and the
breakage only showed up on the next deploy. Session-scoped paths do not belong
in checked-in tooling.
"""
import base64, gzip, os, sys, time, serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from runsh import PORT, BAUD


def deploy(path, dest, timeout=300, shell_wait=75.0):
    raw = open(path, 'rb').read()
    if path.endswith('.b64'):                 # pre-encoded, as before
        data = raw.decode().replace('\n', '')
    else:                                     # take a plain file and do the work
        data = base64.b64encode(gzip.compress(raw)).decode()

    p = serial.Serial(PORT, BAUD, timeout=0.05)

    def until(pred, limit, prod=None, every=2.0):
        t, last, o = time.time(), 0.0, ''
        while time.time() - t < limit:
            o += p.read(8192).decode('utf-8', 'replace')
            if pred(o):
                return o, True
            if prod and time.time() - last > every:
                p.write(prod); last = time.time()
        return o, pred(o)

    def cmd(c, tok, limit=30.0):
        p.write(('%s; echo %s\n' % (c, tok)).encode())
        return until(lambda b: ('\n' + tok) in b, limit)

    _, ok = until(lambda b: b.rstrip().endswith('#'), shell_wait, prod=b'\n')
    if not ok:
        p.close(); return 'NO_SHELL'

    cmd('rm -f /tmp/x.b64', 'RM_OK')
    for i in range(0, len(data), 512):
        _, got = cmd("printf %%s '%s' >> /tmp/x.b64" % data[i:i+512], 'C_OK', 15.0)
        if not got:
            p.close(); return 'STALLED at byte %d of %d' % (i, len(data))
    out, ok = cmd('base64 -d /tmp/x.b64 | gunzip > %s && chmod +x %s && ls -l %s'
                  % (dest, dest, dest), 'DEPLOY_OK', timeout)
    p.close()
    return out if ok else 'DEPLOY_FAILED: ' + out[-300:]


if __name__ == '__main__':
    print(deploy(sys.argv[1], sys.argv[2]))
