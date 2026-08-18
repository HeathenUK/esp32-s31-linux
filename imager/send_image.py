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


def drain(port):
    """Read until the link goes quiet.

    reset_input_buffer() only discards what has already arrived, so an ack still
    in flight lands in the buffer straight afterwards and the sender stays one
    ack behind for the rest of the transfer - one wasted retransmit per frame,
    which doubles a transfer measured in minutes.
    """
    old = port.timeout
    port.timeout = 0.2
    try:
        while port.read(4096):
            pass
    finally:
        port.timeout = old


def await_marker(port, marker, timeout, echo=False):
    """Read until a marker appears, returning everything seen up to it."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = port.read(256)
        if chunk:
            buf += chunk
            if echo:
                sys.stderr.write(chunk.decode("utf-8", "replace"))
                sys.stderr.flush()
            if marker in buf:
                return buf
    return None


def send(port, data, chunk, retry_limit):
    seq, retries, t0 = 0, 0, time.time()
    total = (len(data) + chunk - 1) // chunk

    while seq < total:
        payload = data[seq * chunk:(seq + 1) * chunk]
        port.write(frame(seq, payload))
        reply = port.read_until(b"\n")
        match = ACK_RE.search(reply)

        if match and match.group(1) == b"ACK" and int(match.group(2)) == seq:
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
        # sender recovers if the two ends ever disagree about position.
        if match and match.group(1) == b"NAK":
            want = int(match.group(2))
            if want <= total:
                seq = want
        drain(port)

    port.write(frame(seq, b""))
    port.read_until(b"\n")
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

    print("waiting for IMAGER_READY (reset the board if it is already up)...")
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
