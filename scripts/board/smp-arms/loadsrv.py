#!/usr/bin/env python3
"""A background-load server with a FIXED byte rate.

    loadsrv.py <port> <KB/s>

Any GET streams pseudo-random bytes at the given rate until the client goes
away. WHY THE RATE IS FIXED: an unthrottled download is not the same load on
two kernels - on 2026-09-21 the SMP arm pulled 813 KB/s where UP pulled
545 KB/s, so the "faster" kernel was handed 49% more network work and its
Doom number looked worse for it. A comparison needs the same bytes per second
on both arms; pick a rate both can sustain (400 KB/s).
"""
import os
import socket
import sys
import threading
import time


def serve(conn, rate):
    try:
        conn.recv(4096)
        conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n")
        chunk = os.urandom(rate // 20)          # 50 ms of data
        nxt = time.monotonic()
        while True:
            conn.sendall(chunk)
            nxt += 0.05
            delay = nxt - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.monotonic()          # client fell behind: no burst to catch up
    except OSError:
        pass
    finally:
        conn.close()


def main():
    port, rate = int(sys.argv[1]), int(sys.argv[2]) * 1024
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(4)
    print(f"loadsrv: {rate // 1024} KB/s per connection on :{port}", flush=True)
    while True:
        conn, _ = s.accept()
        threading.Thread(target=serve, args=(conn, rate), daemon=True).start()


if __name__ == "__main__":
    main()
