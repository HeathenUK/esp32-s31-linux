#!/usr/bin/env python3
"""Stream a gzipped disk image to the SD imager kernel over the serial console.

Frames are magic + seq + len + crc32, acknowledged one at a time. The framing
exists because the console is shared with hart0's log output, and the acks exist
because the UART has no flow control while SD cards stall for hundreds of
milliseconds on erase blocks - the ack is what makes the sender wait for the
card rather than overrun it.

The whole image is never held on the board: it is decompressed and written as it
arrives, which is the only way a 512 MB image reaches a device with ~14 MB of RAM.
"""
import argparse
import binascii
import gzip
import hashlib
import os
import re
import struct
import sys
import time

import serial

MAGIC = b"S31I"
ACK_RE = re.compile(rb"(ACK|NAK) (\d+)")


def frame(seq, payload):
    return (MAGIC + struct.pack("<III", seq, len(payload),
                                binascii.crc32(payload) & 0xffffffff) + payload)


def read_some(port, limit=256):
    """Read whatever is available, blocking only for the first byte.

    port.read(n) waits for n bytes or the full timeout, so asking for 256 makes
    a 6-byte ack sit in the buffer until hart0 happens to print enough log text
    to fill the rest - seconds per frame, with no retries to show for it. Take
    one byte, then only what is already waiting.
    """
    data = port.read(1)
    if not data:
        return b""
    waiting = getattr(port, "in_waiting", 0)
    if waiting:
        data += port.read(min(waiting, limit - 1))
    return data


def await_marker(port, marker, timeout, echo=False):
    """Read until a marker appears, returning everything seen up to it."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = read_some(port)
        if chunk:
            buf += chunk
            if echo:
                sys.stderr.write(chunk.decode("utf-8", "replace"))
                sys.stderr.flush()
            if marker in buf:
                return buf
    return None


def wait_ack(port, want, timeout):
    """Scan the return stream for this frame's ack.

    hart0 prints to the same console several times a second, so reading a single
    line back gets a log line far more often than an ack. Reading one line and
    calling anything else a failure costs a full retransmit of the frame every
    time hart0 speaks - measured at more retries than frames, and 2.6 KiB/s on a
    link good for 11. So accumulate and search instead, and match ACK on the
    exact sequence number, which makes a stale ack harmless rather than a frame
    silently skipped.
    """
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = read_some(port)
        if not chunk:
            continue
        buf += chunk
        for match in ACK_RE.finditer(buf):
            kind, num = match.group(1), int(match.group(2))
            if kind == b"ACK" and num == want:
                return ("ACK", num)
            if kind == b"NAK":
                return ("NAK", num)
        buf = buf[-256:]        # enough tail for a match split across reads
    return None


def send(port, data, chunk, retry_limit):
    seq, retries, t0 = 0, 0, time.time()
    total = (len(data) + chunk - 1) // chunk

    while seq < total:
        payload = data[seq * chunk:(seq + 1) * chunk]
        port.write(frame(seq, payload))
        reply = wait_ack(port, seq, port.timeout)

        if reply and reply[0] == "ACK":
            seq += 1
            if seq % 16 == 0 or seq == total:
                sent = seq * chunk
                rate = sent / max(time.time() - t0, 0.001) / 1024
                eta = (len(data) - sent) / max(rate * 1024, 1)
                print(f"\r  {sent // 1024:>6} / {len(data) // 1024} KiB   "
                      f"{rate:5.1f} KiB/s   eta {eta / 60:4.1f} min   "
                      f"retries {retries}", end="", flush=True)
            continue

        retries += 1
        if retries > retry_limit:
            print(f"\ngave up at frame {seq} after {retries} retries",
                  file=sys.stderr)
            return None
        # A NAK carries the frame the receiver actually wants, which is how the
        # sender recovers if the two ends ever disagree about position. No drain
        # is needed: acks are matched by sequence number, so anything stale in
        # the buffer is ignored rather than mistaken for this frame's reply.
        if reply and reply[0] == "NAK" and reply[1] <= total:
            seq = reply[1]

    port.write(frame(seq, b""))
    wait_ack(port, seq, port.timeout)
    print(f"\n  sent {len(data)} bytes in {time.time() - t0:.0f}s, "
          f"{retries} retries")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="raw disk image (uncompressed)")
    ap.add_argument("--port", default="/dev/cu.usbserial-130")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--chunk", type=int, default=4096)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--retry-limit", type=int, default=200)
    ap.add_argument("--wait", type=float, default=180.0,
                    help="seconds to wait for the imager to announce itself")
    ap.add_argument("--no-reset", action="store_true",
                    help="do not pulse EN; use when the board is already waiting")
    args = ap.parse_args()

    raw = open(args.image, "rb").read()
    expect = hashlib.md5(raw).hexdigest()
    print(f"image {args.image}: {len(raw)} bytes, md5 {expect}")
    payload = gzip.compress(raw, 6)
    print(f"compressed to {len(payload)} bytes "
          f"({100 * len(payload) / len(raw):.1f}%), "
          f"~{len(payload) * 10 / args.baud / 60:.1f} min on the wire")

    port = serial.Serial(args.port, args.baud, timeout=args.timeout)
    port.reset_input_buffer()

    # Reset the board so the handshake cannot be missed. The imager announces
    # itself exactly once, and compressing a 512 MB image above takes long
    # enough that a board reset by the flasher has already said it and moved on.
    if not args.no_reset:
        print("resetting the board...")
        port.dtr = False        # GPIO0 high: run, do not enter the bootloader
        port.rts = True         # EN low
        time.sleep(0.15)
        port.rts = False        # EN high: boot
        port.reset_input_buffer()

    print("waiting for IMAGER_READY...")
    if not await_marker(port, b"IMAGER_READY", args.wait):
        print("imager never reported ready", file=sys.stderr)
        return 1

    if send(port, payload, args.chunk, args.retry_limit) is None:
        return 1

    print("waiting for the card to be written and read back...")
    tail = await_marker(port, b"IMAGER_DONE", 1800.0)
    if tail is None:
        print("imager did not finish", file=sys.stderr)
        return 1

    got = re.search(rb"IMAGER_SUM ([0-9a-f]{32})", tail)
    if not got:
        print("no checksum reported by the imager", file=sys.stderr)
        return 1
    got = got.group(1).decode()
    if got != expect:
        print(f"VERIFY FAILED: card {got} != image {expect}", file=sys.stderr)
        return 1
    print(f"VERIFIED: card matches image ({got})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
