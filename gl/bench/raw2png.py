#!/usr/bin/env python3
"""raw2png.py IN.raw W H OUT.png - an RGB565 bench frame as a PNG. s31, MIT."""
import sys, struct, zlib
a = open(sys.argv[1], 'rb').read(); w, h = int(sys.argv[2]), int(sys.argv[3])
rows = []
for y in range(h):
    r = bytearray([0])
    for x in range(w):
        p = a[2*(y*w+x)] | a[2*(y*w+x)+1] << 8
        r += bytes(((p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3))
    rows.append(bytes(r))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
open(sys.argv[4], 'wb').write(png)
