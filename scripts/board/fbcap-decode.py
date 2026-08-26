#!/usr/bin/env python3
"""Decode a .pac panel recording from rootfs/fbcap.c into PNG frames.

Format: [u32 magic 'PAC1'][u32 w][u32 h][u32 nframes], then per frame
[u32 nbytes] followed by (u16 value, u16 count) RLE pairs in RGB565.

The board records RLE because it has no hardware JPEG driver yet and software
encoding there would compete with the responsiveness being filmed. All the
real work happens here, where it costs nothing that matters.
"""
import struct, sys, os
from PIL import Image

src = sys.argv[1]
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)
d = open(src, 'rb').read()
magic, W, H, N = struct.unpack('<IIII', d[:16])
if magic != 0x50414331:
    raise SystemExit("bad magic %08x" % magic)
print("%dx%d, %d frames" % (W, H, N))

off = 16
for i in range(N):
    (nb,) = struct.unpack('<I', d[off:off+4]); off += 4
    body = d[off:off+nb]; off += nb
    px = bytearray(W * H * 3)
    o = 0
    for j in range(0, len(body), 4):
        v = body[j] | (body[j+1] << 8)
        run = body[j+2] | (body[j+3] << 8)
        r = ((v >> 11) & 31) * 255 // 31
        g = ((v >> 5) & 63) * 255 // 63
        b = (v & 31) * 255 // 31
        for _ in range(run):
            if o + 3 <= len(px):
                px[o] = r; px[o+1] = g; px[o+2] = b
                o += 3
    Image.frombytes('RGB', (W, H), bytes(px)).save("%s/f%04d.png" % (outdir, i))
print("wrote %d PNGs to %s" % (N, outdir))
