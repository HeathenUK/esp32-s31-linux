"""Ship a binary to the board over the 1 Mbps console, in printf chunks."""
import sys, time, serial
sys.path.insert(0, '/private/tmp/claude-501/-Users-gadyke-esp32-s31-linux/a3bd6c0f-a207-47f4-a361-ed252dbfd1da/scratchpad')
from runsh import _wait_prompt, PORT, BAUD

def deploy(b64path, dest, timeout=180):
    data = open(b64path).read().replace('\n', '')
    p = serial.Serial(PORT, BAUD, timeout=0.4)
    if not _wait_prompt(p):
        p.close(); return 'NO_SHELL'
    p.read(1 << 20)
    p.write(b"rm -f /tmp/x.b64\n"); time.sleep(0.4); p.read(1 << 20)
    CH = 512
    for i in range(0, len(data), CH):
        p.write(("printf %%s '%s' >> /tmp/x.b64\n" % data[i:i+CH]).encode())
        time.sleep(0.02)
        p.read(1 << 20)
    time.sleep(1)
    p.write(("base64 -d /tmp/x.b64 | gunzip > %s && chmod +x %s && echo DEPLOY_OK && ls -l %s\n"
             % (dest, dest, dest)).encode())
    t = time.time(); out = b''
    while time.time() - t < 30:
        out += p.read(65536)
        if b'DEPLOY_OK' in out and b'\n' in out.split(b'DEPLOY_OK')[-1]:
            break
    p.close()
    return out.decode('utf-8', 'replace')[-300:]

if __name__ == '__main__':
    print(deploy(sys.argv[1], sys.argv[2]))
