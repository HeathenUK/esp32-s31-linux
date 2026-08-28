#!/usr/bin/env python3
"""Turn a board recording into a video, using the real frame timing.

    mjpeg2mp4.py <in.mjpeg> <out.mp4> [--fps N]

The recorder captures on damage, not on every panel refresh, so frames are
deliberately unevenly spaced and the sidecar carries the moment each one was
captured. The gap to the next frame is how long that image was on screen, which
is what makes per-commit capture equivalent to filming every refresh - without
spending ~21% of the board's CPU recording that nothing changed.

**Slice in ARRIVAL order. Do not sort the sidecar.** The sequence number resets
when the drainer hands over from the boot recording to the session one, so
sorting by it interleaves the two runs and every byte offset after the first
handover is wrong. The symptom is not an obvious crash: 106 of 560 frames
decoded as garbage while the total byte count still matched exactly, because
the sizes summed correctly even though the boundaries did not.

--fps produces a constant-rate file by repeating frames, which costs the board
nothing because it happens here.
"""
import argparse
import os
import shutil
import subprocess
import sys


def load(mjpeg):
    txt = mjpeg + '.txt'
    if not os.path.exists(txt):
        sys.exit("no sidecar %s - the timing cannot be reconstructed" % txt)
    rows, eof = [], None
    for line in open(txt):
        f = line.split()
        if len(f) == 2 and f[0] == 'eof':
            eof = int(f[1])
        elif len(f) == 3 and f[0].isdigit():
            rows.append((int(f[0]), int(f[1]), int(f[2])))
    if not rows:
        sys.exit("sidecar has no frames")
    return rows, eof


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mjpeg')
    ap.add_argument('out')
    ap.add_argument('--fps', type=int, default=0,
                    help="constant-rate output; default keeps the real timing")
    ap.add_argument('--crf', type=int, default=23)
    a = ap.parse_args()

    rows, eof = load(a.mjpeg)
    data = open(a.mjpeg, 'rb').read()
    work = a.out + '.frames'
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    off, lines, bad, resets = 0, [], 0, 0
    for i, (seq, sz, stamp) in enumerate(rows):
        d = data[off:off + sz]
        off += sz
        if not (d[:2] == b'\xff\xd8' and d[-2:] == b'\xff\xd9'):
            bad += 1
        if i and seq != rows[i - 1][0] + 1:
            resets += 1
        fn = os.path.join(work, 'f%05d.jpg' % i)
        open(fn, 'wb').write(d)
        nxt = rows[i + 1][2] if i + 1 < len(rows) else eof
        dur = (nxt - stamp) / 1e9 if nxt else 0.1
        lines.append("file '%s'\nduration %.3f"
                     % (os.path.abspath(fn), max(0.017, min(dur, 4.0))))
    lines.append("file '%s'" % os.path.abspath(fn))

    if off != len(data):
        print("WARNING: sidecar accounts for %d of %d bytes" % (off, len(data)))
    if bad:
        print("WARNING: %d malformed frames" % bad)
    if resets:
        print("note: %d sequence discontinuities (handover, or dropped frames)"
              % resets)

    concat = a.out + '.concat'
    open(concat, 'w').write("\n".join(lines) + "\n")
    cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0',
           '-i', concat, '-vsync', 'vfr', '-pix_fmt', 'yuv420p',
           '-c:v', 'libx264', '-crf', str(a.crf), '-preset', 'slow']
    if a.fps:
        cmd += ['-r', str(a.fps), '-vsync', 'cfr']
    cmd.append(a.out)
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit("ffmpeg failed")
    shutil.rmtree(work, ignore_errors=True)
    os.unlink(concat)
    print("wrote %s from %d frames, %d malformed" % (a.out, len(rows), bad))


if __name__ == '__main__':
    main()
