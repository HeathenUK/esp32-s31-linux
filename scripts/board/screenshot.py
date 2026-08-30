#!/usr/bin/env python3
"""Capture what is actually on the panel, as a PNG.

Reads the live scanout buffer over the serial console. Written down because it
has been re-implemented several times mid-task, each time re-learning that:

  - the scanout address is ALLOCATED, not fixed. Hardcoding 0x50c00000 or
    0x50900000 reads the wrong memory and produces a plausible-looking image of
    nothing.
  - and dmesg is NOT good enough for it. dmesg records the address at mode-set
    time; the engine moves afterwards. This read 0x50900000 from the log while
    the display was scanning out 0x50a00000 and produced a torn, tiled mess
    with RGB noise where the buffer ran out. Take it from the driver's debugfs
    'scanout=', which is the address being read *now*, and note that field is
    NOT on the first line any more - cursor_moves= was added above it.
  - /dev/fb0 is fbdev emulation and is NOT what Xorg with modesetting paints
    into. Read the scanout buffer, not fb0.
  - the panel is 800x480 RGB565 when the driver is scaling, and the desktop
    sits letterboxed inside it - but NOT always. If the CMA pool is exhausted
    by client buffers the driver logs "no scanout buffer ... scaling off" and
    scans out the client plane directly, so the buffer becomes 640x384 and
    491520 bytes. Assuming 800x480 then decodes 1280-byte rows as 1600 and
    reads 276480 bytes past the end: a tiled, sheared image with RGB noise
    across the bottom third. Geometry is therefore read from the driver, never
    assumed. The debugfs size= field cannot be used for this - it reports the
    native 768000 in that mode.
"""
import base64
import gzip
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from runsh import run                                    # noqa: E402


CAPTURE = r'''
mount -t debugfs none /sys/kernel/debug 2>/dev/null
U=/sys/kernel/debug/esp32s31_lcd/updates
ADDR=$(tr ' ' '\n' < $U 2>/dev/null | sed -n 's/^scanout=0x//p' | tail -1)
[ -z "$ADDR" ] && ADDR=$(dmesg | sed -n 's/.*scanout buffer [0-9]* bytes at 0x\([0-9a-f]*\).*/\1/p' | tail -1)
[ -z "$ADDR" ] && ADDR=$(dmesg | sed -n 's/.*scanout started.*fb=0x\([0-9a-f]*\).*/\1/p' | tail -1)
LINE=$(dmesg | grep -a 'scanout started' | tail -1)
BYTES=$(echo "$LINE" | sed -n 's/.*fb=0x[0-9a-f]* \([0-9]*\) bytes.*/\1/p')
WIDTH=$(echo "$LINE" | sed -n 's/.*": [0-9]* [0-9]* \([0-9]*\) .*/\1/p')
[ -z "$BYTES" ] && BYTES=768000
[ -z "$WIDTH" ] && WIDTH=800
echo "ADDR $ADDR"
echo "GEOM $WIDTH $BYTES"
# nice, and gzip -1 rather than -9.
#
# This is the most expensive thing any of this tooling does to the board: 768 kB
# out of /dev/mem, compressed, then base64 over a 1 Mbps console. At -9 the
# compression alone is seconds of CPU, and it runs while somebody may be
# typing. -1 gives up a little size for a large fraction of the time, and nice
# means it yields to the desktop rather than competing with it.
#
# If you only want to SEE the screen, use screenshot-hw.py instead: the
# hardware JPEG encoder does it in ~7 ms.
nice -n 19 dd if=/dev/mem bs=1024 skip=$(( 0x$ADDR / 1024 )) count=$(( ($BYTES + 1023) / 1024 )) 2>/dev/null | nice -n 19 gzip -1 > /tmp/fb.gz
echo BEGIN_B64
nice -n 19 base64 /tmp/fb.gz
echo END_B64
'''


def cost_note():
    """Say what this costs, every time, so the cheap path gets used.

    Measured on the board, 5 runs: dd 290 ms + gzip 820 ms + base64 260 ms,
    about 1.1 s of CPU per capture before a byte reaches the console. The
    hardware JPEG encoder does the same job in 16.9 ms. This path exists for
    pixel-exact comparison; for looking at the screen it is 65x too expensive.
    """
    sys.stderr.write("screenshot.py: ~1.1 s of BOARD cpu per capture; "
                     "use screenshot-hw.py (16.9 ms) unless you need the "
                     "exact pixels\n")


def chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data +
            struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))


def capture(out_path, script_path='/tmp/_shot.sh'):
    with open(script_path, 'w') as f:
        f.write(CAPTURE)
    text = run(script_path, 200)
    if 'BEGIN_B64' not in text:
        raise SystemExit('capture failed: %s' % text[-200:])
    b64 = ''.join(text.split('BEGIN_B64')[1].split('END_B64')[0].split())
    data = gzip.decompress(base64.b64decode(b64))

    geom = [l for l in text.splitlines() if l.startswith('GEOM ')]
    if not geom:
        raise SystemExit('capture failed: driver geometry not reported')
    W = int(geom[-1].split()[1])
    nbytes = int(geom[-1].split()[2])
    H = nbytes // (W * 2)
    if len(data) < nbytes:
        raise SystemExit('short read: %d of %d bytes' % (len(data), nbytes))
    print('geometry %dx%d (%d bytes) at the address the engine is reading'
          % (W, H, nbytes))

    rows = bytearray()
    for y in range(H):
        rows.append(0)                       # PNG filter byte: none
        off = y * W * 2
        for x in range(W):
            v = data[off + x * 2] | (data[off + x * 2 + 1] << 8)
            rows.append(((v >> 11) & 31) * 255 // 31)
            rows.append(((v >> 5) & 63) * 255 // 63)
            rows.append((v & 31) * 255 // 31)

    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) +
           chunk(b'IEND', b''))
    open(out_path, 'wb').write(png)
    return out_path


if __name__ == '__main__':
    cost_note()
    print(capture(sys.argv[1] if len(sys.argv) > 1 else 'screen.png'))
