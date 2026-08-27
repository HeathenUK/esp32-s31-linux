#!/usr/bin/env python3
"""Capture the panel as a JPEG using the hardware encoder.

The software path (screenshot.py) reads the scanout buffer over the console a
byte at a time and reassembles it here; a 768,000-byte frame takes a while. This
asks the JPEG codec to compress the same buffer in ~7 ms and transfers ~20 KB
instead.

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
tmp = pathlib.Path("/tmp/_hwshot.sh")
tmp.write_text(SCRIPT)
res = subprocess.run([sys.executable, str(HERE / "runsh.py"), str(tmp), "120"],
                     capture_output=True, text=True)
out = res.stdout
if "NOJPEG" in out:
    sys.exit("no JPEG codec in this kernel (need the esp32s31_ppa jpeg node)")
if "B64START" not in out or "B64END" not in out:
    sys.exit(f"capture failed:\n{out[-800:]}")

for line in out.splitlines():
    if line.startswith(("GEOM", "LEN")):
        print(line)

blob = out.split("B64START", 1)[1].split("B64END", 1)[0]
data = base64.b64decode("".join(blob.split()))
if data[:2] != b"\xff\xd8":
    sys.exit(f"not a JPEG: starts {data[:8].hex(' ')}")
pathlib.Path(OUT).write_bytes(data)
print(f"{len(data)} bytes -> {OUT}")
