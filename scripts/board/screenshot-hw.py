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
import base64
import binascii
import urllib.request, subprocess, sys, pathlib

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


# ---------------------------------------------------------------- network
#
# THE PAYLOAD DOES NOT GO OVER THE CONSOLE.
#
# The serial console has THREE writers: hart0's ESP-IDF logging, hart1's
# kernel printk, and whatever our script prints. hart0 does not respect our
# line boundaries, so its Wi-Fi chatter lands in the MIDDLE of a base64 line
# - observed directly as "ZZ MemTotI (31352) wifi:(phy)..." - and the
# transfer decodes to a traceback. That is not a lossy link to be retried
# harder, it is a shared channel being used to frame data.
#
# So the JPEG goes over Wi-Fi, which is what CLAUDE.md has said all along.
# s31-serve sends one file to one connection with sendfile() and exits, so
# there is nothing to clean up afterwards.
CAP_TO_FILE = """
REC=
for c in /root/mjpegrec /usr/bin/mjpegrec; do [ -x $c ] && REC=$c && break; done
[ -n "$REC" ] || { echo "NOREC"; exit 1; }
rm -f /tmp/_shot.mjpeg
$REC /tmp/_shot.mjpeg 2 2 %s 512 >/dev/null 2>&1
[ -s /tmp/_shot.mjpeg ] || $REC /tmp/_shot.mjpeg 6 2 %s 512 >/dev/null 2>&1
[ -s /tmp/_shot.mjpeg ] || { echo "NOFRAME"; exit 1; }
IP=$(busybox ip -o -4 addr show wlan0 2>/dev/null | busybox awk '{print $4}' | cut -d/ -f1)
[ -n "$IP" ] && [ -x /root/s31-serve ] || { echo "NONET"; exit 1; }
echo "NETIP $IP"
echo "NETLEN $(busybox wc -c < /tmp/_shot.mjpeg)"
setsid /root/s31-serve /tmp/_shot.mjpeg %d >/dev/null 2>&1 </dev/null &
sleep 1
echo "SERVING"
"""


def fetch_over_wifi(quality, port=8137):
    """Capture on the board and pull the file over Wi-Fi. None if unavailable."""
    out = run(CAP_TO_FILE % (quality, quality, port), "150")
    if "SERVING" not in out:
        why = ("mjpegrec not on the board" if "NOREC" in out else
               "no frame committed" if "NOFRAME" in out else
               "no wlan0 address or no /root/s31-serve" if "NONET" in out else
               "the board sent no recognisable reply")
        print(f"wifi capture unavailable ({why})", file=sys.stderr)
        return None
    ip = [l.split()[1] for l in out.splitlines() if l.startswith("NETIP ")]
    if not ip:
        return None
    url = f"http://{ip[-1]}:{port}/shot"
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            data = r.read()
    except Exception as e:                                   # noqa: BLE001
        print(f"wifi fetch failed ({e})", file=sys.stderr)
        return None
    print(f"via wifi {ip[-1]} ({len(data)} bytes)")
    return data

# DRM first - it is the path that works without debugfs.
_net = fetch_over_wifi(QUALITY)
if _net is not None:
    nxt = _net.find(b"\xff\xd8", 2)
    if nxt > 0:
        _net = _net[:nxt]
    if _net[:2] != b"\xff\xd8":
        sys.exit(f"not a JPEG: starts {_net[:8].hex(' ')}")
    pathlib.Path(OUT).write_bytes(_net)
    print(f"{len(_net)} bytes -> {OUT}")
    sys.exit(0)

print("falling back to base64 over the serial console - expect corruption "
      "if anything is logging", file=sys.stderr)
out = run(REC_SCRIPT % (QUALITY, QUALITY), "150")
via = "drm/mjpegrec"
if "B64START" not in out and not any(m in out for m in ("NOREC", "NOFRAME")):
    # NO MARKER AT ALL means the BOARD did not answer - not that capture
    # failed. Saying "mjpegrec absent" here sent a whole session chasing a
    # missing binary that was present the entire time (2026-09-06). A busy
    # board is slow, so retry once before believing anything.
    print("capture: no marker in the reply - board busy or console "
          "contended; retrying once", file=sys.stderr)
    out = run(REC_SCRIPT % (QUALITY, QUALITY), "150")
if "B64START" not in out:
    why = ("mjpegrec not on the board" if "NOREC" in out else
           "no frame committed" if "NOFRAME" in out else
           "the board sent no recognisable reply (busy? console in use?)")
    print(f"DRM capture unavailable ({why}); trying debugfs", file=sys.stderr)
    out = run(SCRIPT)
    via = "debugfs/ppa"
    if "NOJPEG" in out:
        sys.exit("no capture path. Check in this order: (1) is the board "
                 "answering at all - run alive.py, and believe runsh.py over "
                 "any silence; (2) is /root/mjpegrec present; (3) a DIAG=0 "
                 "kernel has no debugfs, so the fallback cannot work - "
                 "deploy images/mjpegrec")
if "B64START" not in out or "B64END" not in out:
    sys.exit(f"capture failed:\n{out[-800:]}")

for line in out.splitlines():
    if line.startswith(("GEOM", "LEN", "RECLEN")):
        print(line)

blob = out.split("B64START", 1)[1].split("B64END", 1)[0]
b64 = "".join(blob.split())
try:
    data = base64.b64decode(b64)
except binascii.Error as e:
    # A CONTENDED CONSOLE DROPS BYTES. The transfer arrives with a hole in
    # it and b64decode raises a bare traceback that looks like a bug in the
    # tool rather than a lossy link - which is exactly how it reads at 3am.
    # Say what happened, and say what to do about it.
    sys.exit(f"capture corrupted in transit: {e}\n"
             f"  got {len(b64)} base64 chars. The serial console drops bytes "
             f"when a client is rendering hard.\n"
             f"  Retry, or stop the client first. This is a LINK fault, not "
             f"a capture fault - the board is fine.")
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
