#!/usr/bin/env python3
"""Loopback test: the real sdrecv binary over a pty, driven by send_image.send().

Uses incompressible data so the stream spans many frames, and asserts that each
injected fault actually fired - a fault that never happens proves nothing.
"""
import gzip, hashlib, os, pty, subprocess, sys, termios, time, tty
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import send_image

SCRATCH = os.environ.get("SDRECV_BUILD_DIR", "/tmp")


class PtyLink:
    def __init__(self, fd):
        self.fd, self.buf, self.timeout = fd, b"", 5.0
    def write(self, data):
        while data:
            data = data[os.write(self.fd, data[:1024]):]
    @property
    def in_waiting(self):
        # Mirror pyserial: how many bytes could be read without blocking.
        import array, fcntl, termios
        if self.buf:
            return len(self.buf)
        n = array.array("i", [0])
        fcntl.ioctl(self.fd, termios.FIONREAD, n, True)
        return n[0]

    def read(self, n):
        import select
        if self.buf:
            out, self.buf = self.buf[:n], self.buf[n:]
            return out
        if not select.select([self.fd], [], [], self.timeout)[0]:
            return b""
        try:
            return os.read(self.fd, n)
        except OSError:
            return b""
    def read_until(self, term):
        deadline = time.time() + 5
        while term not in self.buf and time.time() < deadline:
            chunk = self.read(256)
            if not chunk:
                break
            self.buf += chunk
        if term in self.buf:
            line, _, self.buf = self.buf.partition(term)
            return line + term
        return b""
    def reset_input_buffer(self):
        self.buf = b""


def run(label, payload, raw, corrupt=(), noise_at=None, dup_at=None,
        drop_ack_at=None, throttle=0.0, timeout=None, retry_limit=200):
    master, slave = pty.openpty()
    tty.setraw(slave); tty.setraw(master)
    out = f"{SCRATCH}/loop_out.bin"
    # A throttle stands in for the SD card: the ack cannot come back until the
    # consumer has taken the data, so a slow consumer pushes ack latency past
    # the sender's timeout exactly as a real card does on the image's zero tail.
    sink = (f"python3 -c \"import sys,time\nw=sys.stdout.buffer\n"
            f"while True:\n b=sys.stdin.buffer.read(16384)\n if not b: break\n"
            f" time.sleep({throttle})\n w.write(b)\n w.flush()\nw.flush()\" > {out}"
            if throttle else f"cat > {out}")
    dev = subprocess.Popen(f"exec {SCRATCH}/sdrecv_host 3 3>&0 | gunzip -c | {sink}",
                           shell=True, stdin=slave, stdout=slave,
                           stderr=subprocess.DEVNULL, close_fds=False)
    os.close(slave)
    link = PtyLink(master)
    orig_write, fired = link.write, {"corrupt": 0, "noise": 0, "dup": 0, "drop": 0}
    seen = {"n": 0}

    def hooked(data):
        if data.startswith(b"S31I"):
            n = seen["n"]; seen["n"] += 1
            if n == noise_at:
                orig_write(b"\r\nI (1234) hart0: wifi rx queue full\r\n")
                fired["noise"] += 1
            if n in corrupt:
                bad = bytearray(data); bad[-1] ^= 0xff
                orig_write(bytes(bad)); fired["corrupt"] += 1
            if n == dup_at:
                # Sender-side retransmit of a frame the receiver already took:
                # exactly what a timeout during an SD erase stall produces.
                orig_write(data); link.read_until(b"\n"); fired["dup"] += 1
            if n == drop_ack_at:
                orig_write(data); link.read_until(b"\n")   # swallow the ack
                fired["drop"] += 1
        orig_write(data)

    link.write = hooked
    if timeout is not None:
        link.timeout = timeout
    import io, contextlib
    cap = io.StringIO()
    with contextlib.redirect_stdout(cap):
        ok = send_image.send(link, payload, 4096, retry_limit)
    sys.stdout.write(cap.getvalue()[-80:] + "\n")
    import re as _re
    m = _re.search(r"(\d+) retries", cap.getvalue())
    total_retries = int(m.group(1)) if m else 0
    try:
        dev.wait(timeout=30)
    except subprocess.TimeoutExpired:
        dev.kill(); ok = False
    got = open(out, "rb").read()
    os.close(master)

    want_fire = {k: v for k, v in
                 (("corrupt", len(corrupt)), ("noise", 1 if noise_at is not None else 0),
                  ("dup", 1 if dup_at is not None else 0),
                  ("drop", 1 if drop_ack_at is not None else 0)) if v}
    missed = [k for k, v in want_fire.items() if fired[k] < v]
    good = bool(ok) and got == raw and not missed
    print(f"{'PASS' if good else 'FAIL'}  {label}  [{total_retries} retries total]")
    print(f"        {len(got)}/{len(raw)} bytes, md5 {hashlib.md5(got).hexdigest()[:12]}"
          f" vs {hashlib.md5(raw).hexdigest()[:12]}, faults fired {fired}")
    if missed:
        print(f"        NOT EXERCISED: {missed}")
    return good


raw = os.urandom(400 * 1024)          # incompressible: forces many frames
comp = gzip.compress(raw, 6)
print(f"payload {len(raw)} raw -> {len(comp)} compressed = "
      f"{(len(comp) + 4095) // 4096} frames\n")
results = [
    run("clean transfer", comp, raw),
    run("corrupt CRC on frames 3, 9, 20", comp, raw, corrupt={3, 9, 20}),
    run("hart0 log line injected mid-stream", comp, raw, noise_at=5),
    run("duplicate frame after a lost ack", comp, raw, dup_at=7),
    run("ack swallowed, sender retransmits", comp, raw, drop_ack_at=11),
    run("everything at once", comp, raw, corrupt={2, 30}, noise_at=6,
        dup_at=15, drop_ack_at=25),
    # Consumer slower than the ack timeout: every frame times out at least once,
    # so this only passes if the retry budget is per frame rather than a running
    # total. With a cumulative budget it aborts partway, as it did on hardware.
    run("consumer slower than the ack timeout", comp, raw,
        throttle=1.2, timeout=0.15, retry_limit=25),
]
print("\nALL PASS" if all(results) else "\nFAILURES PRESENT")
