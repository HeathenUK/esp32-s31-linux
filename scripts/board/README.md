# Talking to the board

These exist because they have each been re-written from scratch several times,
badly, in the middle of doing something else. Use them.

    scripts/board/runsh.py  <script.sh> [timeout] [boot_wait]
    scripts/board/deploy_bin.py <file.b64> <dest-on-board>
    scripts/board/reset.py
    scripts/board/screenshot.py <out.png>

`runsh.py` ships a shell script to the board as a file and runs it. It does NOT
flatten the script into a one-liner - `for x; do` becomes `do;` that way and the
shell errors out, producing empty results that look like a hardware fault.

It also survives the login race: the LCD driver prints its mode-set messages at
exactly the moment getty shows its prompt, so a matcher that expects `login:`
last reports NO_SHELL on a perfectly healthy board. If you get NO_SHELL, retry
before concluding anything.

`screenshot.py` reads the live scanout buffer over the serial console and writes
a PNG. The scanout address is **allocated, not fixed** - it takes it from dmesg
rather than hardcoding it.

**`screenshot.py`'s note about the address is the rule, not a detail.** It moved
from `0x50800000` to `0x50900000` the moment the display client restarted, and
encoding the old one produced a perfectly valid JPEG of a buffer nobody was
displaying - which is indistinguishable from a broken encoder and cost most of a
day. Read it from `/sys/kernel/debug/esp32s31_lcd/updates` (`scanout=`) every
time.

## Capturing pictures and video

    scripts/board/screenshot-hw.py <out.jpg> [quality]   # hardware JPEG grab
    scripts/board/fbcap-decode.py <in.pac> <outdir>      # decode an fbcap file

`screenshot-hw.py` is the fast one. It reads the live scanout address and
geometry, asks the JPEG codec to compress the frame in ~7 ms, and pulls ~20 KB
back base64 over the console. `screenshot.py` moves all 768,000 bytes instead
and is the fallback for a kernel without the codec.

For **video**, use the on-board recorder rather than pulling frames one at a
time:

    mjpegrec <out.mjpeg> <seconds> [max_fps] [quality] [ring_kb]

It starts a recording through a DRM ioctl, the kernel captures a frame whenever
the display commits, and everything is drained afterwards into one file. The
output is concatenated JPEGs, which is what MJPEG is: `ffmpeg -f mjpeg -i
out.mjpeg ...` reads it directly. A sidecar `.txt` carries the capture
timestamps, because frames are produced on damage and are deliberately not
evenly spaced.

**Recording costs ~2.4% of the CPU while the screen is changing and ~1.7% when
it is idle**, measured by CoreMark displacement with the activity held constant.
That is low enough to film the system while measuring it, which is the whole
point. Earlier approaches were not: a timer-driven loop cost 15% at 10 fps, and
the first version cost 42%.

## Driving the desktop, and measuring it

    uinject <demo|drag|type|park|keytest> [args]   # inject input, nothing else
    jpegcap <fps> <secs> [q] [w] [h]               # pace encodes without forking
    keylog                                          # log every evdev key event

`uinject` exists because `deskbench` hashes the framebuffer on every iteration
and costs ~53% of the core doing it. That is fine when it *is* the instrument
and ruinous when the point is to film or measure how the desktop behaves.

`keylog` prints every `EV_KEY` and `MSC_SCAN` per device, which is what
separates "the kernel never delivered that key" from "the desktop dropped it".

## Two ways a measurement harness lies

Both of these attributed their own cost to the thing being measured, and both
were found only by measuring the harness separately.

- **busybox applets fork.** Pacing a loop with `usleep` in shell forks a process
  per iteration, and on this board that cost more than a hardware JPEG encode
  did - 42% of the CPU against the driver's real 5%. `jpegcap` opens its control
  file once and paces with `clock_nanosleep`.
- **`dev_info()` in a hot path writes to a 1 Mbps serial console, synchronously.**
  One ~100-byte line is about 1 ms. Logging per frame was the single largest
  cost in the capture path (15% down to 5.5% at 10 fps when removed) and it also
  floods the ring buffer, scrolling away the `scanout started` line that tooling
  parses geometry from. Use `dev_dbg` for anything per-frame.

## Why a call sometimes said NO_SHELL and the retry worked

Fixed 2026-08-27; `console.py` now holds the logic and `runsh`/`deploy_bin`
share it. Three separate faults, all in the tooling rather than the board, and
all of them capable of being misread as a hardware failure:

- **`deploy_bin` never logged in.** It waited for a `#` prompt, but a freshly
  booted board sits at `login:`. So a deploy to a just-booted board could never
  succeed, however long the timeout, and appeared to work only when an earlier
  `runsh` call had happened to log in first.
- **A fixed 75 s window against an ~85 s boot.** Reaching a prompt from a hard
  reset takes ~85 s here, so "reset, then run" always failed on the first call
  and always succeeded on the second. The wait now extends while bytes are
  still arriving and fails fast when nothing ever comes.
- **The board prints at two bauds.** The second-stage bootloader talks at
  **115200** and only then does the console switch to **1 Mbps** - verified by
  reading the same reset at both. At 1 Mbps that phase is unreadable or drops
  entirely to framing errors, so a healthy board looks silent for the first
  seconds. Reporting that as "off, held in reset, or in download mode" is the
  most misleading thing this tooling can say, and it did.

Separately, **`deploy_bin` now runs `sync`.** ext4 defers allocation, so a
board reset shortly after a deploy left a *zero-length* file with the right
name and mode - a deploy that reported success and had silently vanished.
