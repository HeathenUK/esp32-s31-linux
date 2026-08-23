"""Run a script on the board, tolerating kernel messages landing on the login line.

runsh.py matches the login prompt at the end of the buffer. The LCD driver
prints its mode-set messages exactly when getty shows the prompt, so the prompt
is no longer last and the match fails - a healthy board reports NO_SHELL. This
searches the whole buffer instead, and quiets the console once logged in.
"""
import base64, sys, time, serial

PORT, BAUD = '/dev/cu.usbserial-130', 1000000

def run(path, timeout=240, boot_wait=0):
    if boot_wait:
        time.sleep(boot_wait)
    p = serial.Serial(PORT, BAUD, timeout=0.3)
    def rd(n):
        t = time.time(); o = b''
        while time.time() - t < n:
            o += p.read(8192)
        return o.decode('utf-8', 'replace')
    rd(1.0)
    out = ''
    for _ in range(6):
        p.write(b'\n'); out = rd(2.0)
        if out.rstrip().endswith('# '):
            break
        if 'login:' in out:
            p.write(b'root\n'); out = rd(4.0)
            if 'assword' in out:
                p.write(b'\n'); out = rd(4.0)
            if '# ' in out:
                break
    if '# ' not in out:
        p.close(); return 'NO_SHELL: ' + repr(out[-200:])
    # Stop kernel messages interleaving into the results.
    p.write(b'dmesg -n 1\n'); rd(1.0)
    b64 = base64.b64encode(open(path, 'rb').read()).decode()
    p.write(b'rm -f /tmp/r.b64\n'); rd(0.5)
    for i in range(0, len(b64), 400):
        p.write(("printf %%s '%s' >> /tmp/r.b64\n" % b64[i:i+400]).encode())
        time.sleep(0.03); p.read(1 << 20)
    p.write(b'base64 -d /tmp/r.b64 > /tmp/r.sh && echo UP_OK\n')
    if 'UP_OK' not in rd(6.0):
        p.close(); return 'UPLOAD_FAILED'
    p.write(b'sh /tmp/r.sh 2>&1; echo RS_DONE\n')
    t = time.time(); o = b''
    while time.time() - t < timeout:
        o += p.read(8192)
        if b'RS_DONE' in o.split(b'echo RS_DONE')[-1]:
            break
    p.close()
    return o.decode('utf-8', 'replace')

if __name__ == '__main__':
    print(run(sys.argv[1],
              int(sys.argv[2]) if len(sys.argv) > 2 else 240,
              int(sys.argv[3]) if len(sys.argv) > 3 else 0))
