#!/usr/bin/env python3
"""Capture the panel as a JPEG using the hardware encoder.

The software path (screenshot.py) reads the scanout buffer over the console a
byte at a time and reassembles it here; a 768,000-byte frame takes a while. This
asks the JPEG codec to compress the same buffer in ~7 ms and transfers ~20 KB
instead.

Two paths, and the ORDER matters:

  1. **mjpegrec, through a DRM ioctl on /dev/dri/card0.** This is the one that
     works on a shipping kernel. It needs no debugfs, so it is tried first.
  2. The debugfs PPA node, kept as a fallback for kernels that have it.

**Do not assume debugfs is there.** DIAG=0 is the default and compiles it out
(it costs 1.14 MB of RAM), so the debugfs path fails on every shipping image -
which reads as "no JPEG codec in this kernel" even though the codec is present
and working. That message cost a detour into rebuilding a DIAG=1 kernel purely
to take a picture, when mjpegrec was sitting on the board the whole time. The
capture hardware is reached by ioctl; the debugfs node was only ever the
diagnostic surface.

Two things this must not hardcode, both learned the hard way:

  - **The scanout address is allocated, not fixed.** It moves whenever the
    display client restarts. Encoding a stale address produces a perfectly
    valid JPEG of whatever used to be there, which looks like an encoder bug
    and is not one.
  - **The geometry comes from the driver**, not from an assumption of 800x480.
"""
import base64, subprocess, sys, pathlib

HERE = pathlib.Path(__file__).resolve().parent
OUT = sys.argv[1] if len(sys.argv) > 1 else "panel.jpg"
QUALITY = sys.argv[2] if len(sys.argv) > 2 else "85"

# The DRM path. mjpegrec records on damage into a kernel ring and drains it
# afterwards, so a single frame costs one commit. A static desktop still
# commits (clock, cursor), but not necessarily within a second - hence the
# second, longer attempt before giving up.
REC_SCRIPT = """
REC=
for c in /root/mjpegrec /usr/bin/mjpegrec; do [ -x $c ] && REC=$c && break; done
[ -n "$REC" ] || { echo "NOREC"; exit 1; }
rm -f /tmp/_shot.mjpeg
$REC /tmp/_shot.mjpeg 2 2 %s 512 >/dev/null 2>&1
[ -s /tmp/_shot.mjpeg ] || $REC /tmp/_shot.mjpeg 6 2 %s 512 >/dev/null 2>&1
[ -s /tmp/_shot.mjpeg ] || { echo "NOFRAME"; exit 1; }
echo "RECLEN $(busybox wc -c < /tmp/_shot.mjpeg) bytes"
echo "B64START"
base64 /tmp/_shot.mjpeg
echo "B64END"
"""

SCRIPT = f"""
mount -t debugfs none /sys/kernel/debug 2>/dev/null
D=/sys/kernel/debug/esp32s31_ppa
U=/sys/kernel/debug/esp32s31_lcd/updates
[ -e $D/jpeg ] || {{ echo "NOJPEG"; exit 1; }}
A=$(busybox tr ' ' '\\n' < $U | busybox sed -n 's/^scanout=0x//p')
# The scanout buffer is always the panel's native size - the driver scales
# the client into it - so 800x480 is the right default. dmesg is only a
# cross-check, and it may well have scrolled away.
W=800; H=480
G=$(dmesg | busybox grep "scanout started" | busybox tail -1)
DW=$(echo "$G" | busybox sed -n 's/.*"\\([0-9]*\\)x\\([0-9]*\\)".*/\\1/p')
DH=$(echo "$G" | busybox sed -n 's/.*"\\([0-9]*\\)x\\([0-9]*\\)".*/\\2/p')
[ -n "$DW" ] && W=$DW
[ -n "$DH" ] && H=$DH
echo "GEOM $A $W $H"
echo "$A $W $H {QUALITY}" > $D/jpeg || {{ echo "ENCFAIL"; exit 1; }}
echo "LEN $(cat $D/jpeg_last_len) NS $(cat $D/jpeg_last_ns)"
echo "B64START"
base64 $D/jpeg_out
echo "B64END"
"""
def run(script, timeout="120"):
    tmp = pathlib.Path("/tmp/_hwshot.sh")
    tmp.write_text(script)
    return subprocess.run([sys.executable, str(HERE / "runsh.py"),
                           str(tmp), timeout],
                          capture_output=True, text=True).stdout

# DRM first - it is the path that works without debugfs.
out = run(REC_SCRIPT % (QUALITY, QUALITY), "150")
via = "drm/mjpegrec"
if "B64START" not in out:
    why = ("mjpegrec not on the board" if "NOREC" in out else
           "no frame committed" if "NOFRAME" in out else "unknown")
    print(f"DRM capture unavailable ({why}); trying debugfs", file=sys.stderr)
    out = run(SCRIPT)
    via = "debugfs/ppa"
    if "NOJPEG" in out:
        sys.exit("no capture path: mjpegrec absent AND no debugfs PPA node "
                 "(a DIAG=0 kernel has no debugfs - deploy images/mjpegrec)")
if "B64START" not in out or "B64END" not in out:
    sys.exit(f"capture failed:\n{out[-800:]}")

for line in out.splitlines():
    if line.startswith(("GEOM", "LEN", "RECLEN")):
        print(line)

blob = out.split("B64START", 1)[1].split("B64END", 1)[0]
data = base64.b64decode("".join("".join(blob.split()).split()))
# mjpegrec writes concatenated JPEGs (that is what MJPEG is). Keep the first.
if via == "drm/mjpegrec":
    nxt = data.find(b"\xff\xd8", 2)
    if nxt > 0:
        data = data[:nxt]
if data[:2] != b"\xff\xd8":
    sys.exit(f"not a JPEG: starts {data[:8].hex(' ')}")
print(f"via {via}")
pathlib.Path(OUT).write_bytes(data)
print(f"{len(data)} bytes -> {OUT}")
