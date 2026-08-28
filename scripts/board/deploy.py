#!/usr/bin/env python3
"""Ship a file to the board by whichever path is actually fast.

    deploy.py <local-file> <dest-on-board> [--console] [--port /dev/cu.usb...]

There are two transports and the difference is not marginal:

    console (base64 over the 1 Mbps serial line)   734 KB -> >10 minutes
    network (HTTP over Wi-Fi, measured 191 KB/s)   734 KB -> 4 seconds

deploy_bin.py has always said "for anything large prefer the network", and that
advice was ignored every single time, because following it meant hand-rolling
an HTTP server, finding the host's address on the board's subnet, and cleaning
up afterwards. So this does it: ask the board for its address, and if it has
one, serve the file and fetch it. The console is the fallback, not the default.

Verified by md5 on both ends, because a truncated transfer that leaves a file
of the right name and mode is indistinguishable from a working deploy until
something behaves oddly hours later.
"""
import hashlib
import http.server
import os
import re
import socket
import socketserver
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runsh

# runsh.run() takes a *file*, deliberately: flattening a script into "; "
# one-liners breaks every multi-line construct and yields empty output that
# looks like a hardware fault. So write a temp script and hand it over.


def run_script(text, timeout=120):
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.sh', delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        return runsh.run(tmp, timeout)
    finally:
        os.unlink(tmp)


# Below this, the console's round trips cost less than standing up a server.
NET_THRESHOLD = 32 * 1024


def board_ip(timeout=90):
    """The board's own address, or None if it has no network."""
    out = run_script(
        "busybox ip -o -4 addr show wlan0 2>/dev/null | "
        "busybox awk '{print $4}' | busybox cut -d/ -f1", timeout)
    m = re.search(r'(\d+\.\d+\.\d+\.\d+)', out or '')
    return m.group(1) if m else None


def host_ip_for(dest):
    """Our address on the interface that reaches `dest`.

    Asking the routing table beats guessing en0: this box has several
    interfaces and only one of them is on the board's subnet.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((dest, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def serve_dir(directory):
    """A one-shot HTTP server on an ephemeral port, bound to this thread."""
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def log_message(self, *a):
            pass

    httpd = socketserver.TCPServer(("", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.server_address[1]


def deploy_net(path, dest, ip, timeout=180):
    want = hashlib.md5(open(path, 'rb').read()).hexdigest()
    size = os.path.getsize(path)
    httpd, port = serve_dir(os.path.dirname(os.path.abspath(path)) or '.')
    try:
        url = "http://%s:%d/%s" % (host_ip_for(ip), port,
                                   os.path.basename(path))
        t0 = time.time()
        out = run_script(
            "mkdir -p %s\n"
            "wget -q %s -O %s.part || { echo DEPLOY_WGET_FAIL; exit 1; }\n"
            "mv %s.part %s && chmod 755 %s && sync\n"
            "echo SUM $(busybox md5sum %s | busybox cut -d' ' -f1)\n"
            % (os.path.dirname(dest) or '/', url, dest, dest, dest, dest, dest),
            timeout)
        dt = time.time() - t0
    finally:
        httpd.shutdown()

    if 'DEPLOY_WGET_FAIL' in (out or ''):
        return False, "wget failed"
    m = re.search(r'SUM ([0-9a-f]{32})', out or '')
    if not m:
        return False, "no checksum returned"
    if m.group(1) != want:
        return False, "checksum mismatch (%s != %s)" % (m.group(1), want)
    return True, "%d bytes in %.1fs (%.0f KB/s) over the network" % (
        size, dt, size / 1024.0 / max(dt, 0.001))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    force_console = '--console' in sys.argv
    if len(args) < 2:
        print(__doc__)
        return 2
    path, dest = args[0], args[1]
    size = os.path.getsize(path)

    if not force_console and size >= NET_THRESHOLD:
        ip = board_ip()
        if ip:
            ok, msg = deploy_net(path, dest, ip)
            print(("DEPLOY_OK " if ok else "DEPLOY_FAIL ") + msg)
            if ok:
                return 0
            print("falling back to the console")
        else:
            print("board has no address; using the console")

    # The console path, unchanged, for small files and for a board with no
    # network - which is exactly when it is still the right answer.
    import deploy_bin
    if size >= NET_THRESHOLD:
        print("warning: %d KB over the console will take minutes" % (size // 1024))
    return deploy_bin.deploy(path, dest)


if __name__ == '__main__':
    sys.exit(main() or 0)
