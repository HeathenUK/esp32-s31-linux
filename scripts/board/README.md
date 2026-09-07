# Talking to the board

## Is the board alive? Ask `alive.py`, not `runsh.py`

    scripts/board/alive.py             # poke it and report what stage it is at
    scripts/board/alive.py --reset     # reset and watch a whole boot through

`runsh.py`'s `NO_SHELL` answers "I did not see a prompt", which is true of at
least five different situations - unpowered, hart0 up but no Linux, Linux
wedged part way, sitting at `login:`, and sitting at a shell prompt. **The last
two are healthy**, and an idle healthy board is byte-for-byte identical to a
dead one on a serial line. Treating them alike produced two wrong diagnoses in
one session: once "the kernel hangs" when it was booting perfectly, and once a
real wedge that was nearly dismissed as the same false alarm.

`alive.py` reports a stage - `NOTHING`, `HART0_ROM`, `HART0_APP`, `HANDOFF`,
`KERNEL_EARLY`, `DISPLAY_UP`, `ROOT_MOUNTED`, `USERSPACE`, `LOGIN`, `SHELL` -
with the lines it saw. Two things make that possible:

- **It reads each phase at the baud that phase uses.** hart0's second-stage
  bootloader talks at 115200 and the console only then switches to 1 Mbps, so
  watching a reset at 1 Mbps shows the first seconds as framing garbage. Silence
  early proves nothing.
- **It pokes.** A newline at a login or shell prompt gets an answer, which is
  the only way to tell idle from dead.

A warm `reboot` takes ~85 s to come back. Polling before that and getting
silence means "still booting", which is exactly what `SILENT` says.


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

## Filming a whole session, boot to desktop

    on the board          on the host
    ------------          -----------
    s31-record arm        (then reboot to film the next boot)
    reboot
    ...use the desktop...
    s31-record stop       the file is complete when the final name appears
    s31-record serve      curl -o session.mjpeg http://<board>:8080/
                          scripts/board/mjpeg2mp4.py session.mjpeg out.mp4

`s31-record start` films from now, without a reboot, and misses the boot.

The kernel arms the recorder at scanout (1.7 s), long before userspace exists;
`/etc/init.d/S02s31-vidcap` attaches to it and drains to the card. `stop` sends
SIGTERM, so the drainer finishes its frame, writes the closing timestamp,
fsyncs and renames `.part` to the final name - **that rename is the completion
signal**, and a `.part` left behind means the session was cut short. It still
plays, up to its last whole frame: MJPEG has no trailer to corrupt.

Getting it off the board (the card is soldered, so it cannot be read elsewhere):

| transport | 52 MB | notes |
|---|---|---|
| `s31-serve` over Wi-Fi | **58 s** (915 KB/s) | one-shot, exits after serving |
| base64 over the console | 13 min (65 KB/s) | floods the line; **do not** |

Reconstruction detail that matters: **slice the sidecar in arrival order, never
sort it.** The sequence number restarts at the boot-to-session handover, so
sorting scrambles every byte offset after it - and it fails quietly, because
the sizes still sum to the right total. `mjpeg2mp4.py` does this correctly and
warns about malformed frames and discontinuities.

## Driving the desktop, and measuring it

    uinject <demo|drag|type|park|keytest> [args]   # inject input, nothing else
    jpegcap <fps> <secs> [q] [w] [h]               # pace encodes without forking
    keylog                                          # log every evdev key event

**These live in `/root`, not on `$PATH`** - they are hand-deployed tools, not
part of the image, so call them as `/root/uinject`. A bare `uinject` gives
"not found", which reads as "the tool does not exist here" rather than "it is
one directory away". Re-imaging the card loses them; rebuild from `rootfs/*.c`
and ship them with `scripts/board/deploy.py`.

`uinject` also understands `altkey CODE [n] [hold]`, which holds Alt, taps
CODE n times and then releases Alt - Alt-Tab commits on the *release*, and the
press and the release cannot be separate runs because the uinput device is
destroyed on exit and the held modifier dies with it. `hold` keeps Alt down so
the switcher can be photographed before it commits; kill that run and the
release never arrives, so the next `altkey` steps a switcher that is still
open. That contaminated a test once and read as a broken commit.

It also understands `rclick X Y` (the RIGHT button - clients use Button3 for
context menus and it is unreachable otherwise), and `wheel N X Y` moves to the
point before scrolling. **`dragto`'s destination is now paced like its start.**
It used the fixed 24-step move, which is fast enough to trip the desktop's
pointer acceleration, so the drag ended somewhere other than asked - 600,300
put the pointer at 799,451, hard against the screen edge. A gesture that lands
on the wrong pixel still looks like it worked, and it invalidated several UI
tests before it was noticed.

It also understands `click X Y`, `dragto x1 y1 x2 y2`,
`dragholdto x1 y1 x2 y2` (holds the button so the snap preview can be
photographed), `dblclick X Y`, `wheel N` and `key CODE`. Every invocation homes
the pointer to the top-left first and pays `UINJECT_SETTLE` ms (default 1500)
before its first event. **That default must stay above the desktop's 2000 ms
device-rescan period**: every run creates a fresh uinput device, so a shorter
settle races discovery and the events go to a node nothing has opened yet. At
the old 1500 ms the FIRST run of a test landed exactly where it aimed and every
later one drifted 2-3 px from wherever the pointer already was - aimed 300,200
gave 300,200, then aimed 650,400 gave 302,202. A harness that is accurate once
and then silently stops moving invalidates whatever it was asked to prove, and
this one invalidated several UI results before it was caught.

The settle also means two gestures in separate runs can never fall inside
the desktop's 400 ms double-click window - `dblclick` exists for that reason.
**Raise the settle to ~2500 ms when the desktop has just started**: discovery
is on a 2 s rescan, and evdev only delivers events queued after the reader
opens the node, so an early gesture is silently dropped and looks exactly like
a broken drag.

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

### deploy.py - the one to use

    scripts/board/deploy.py <local-file> <dest-on-board>

Picks the transport by size and by whether the board has an address:

| transport | 734 KB | when |
|---|---|---|
| network (HTTP over Wi-Fi) | **3.2 s**, 226 KB/s | anything >= 32 KB with wlan0 up |
| console (base64 over serial) | **> 10 minutes** | small files, or no network |

Both ends are checksummed. A truncated transfer leaves a file of the right name
and mode, which is indistinguishable from a good deploy until something behaves
oddly hours later - so it is verified, not assumed.

`deploy_bin.py` is the console path and still works; it is what deploy.py falls
back to. Do not reach for it directly for anything large. Its docstring said
"for anything large prefer the network" for months and that advice was ignored
every time, because following it meant hand-rolling a server each session.
`--console` forces it if you need to test that path.


## Cost to the board, and how to stay out of the way

These tools run on a machine somebody may be using. Measured, 5 runs each:

| operation | board CPU | note |
|---|---|---|
| `screenshot.py` | **~1.1 s** | dd 290 ms + gzip 820 ms + base64 260 ms, then the console transfer |
| `screenshot-hw.py` | **16.9 ms** | hardware JPEG encoder - 65x cheaper |
| `deploy.py`, unchanged file | **0** | skipped entirely on an md5 match |
| `deploy.py`, 1 MB over Wi-Fi | seconds | now `nice -n 19` |

Three changes came out of a session where a human was trying to type while
this tooling ran, and reported lag that was never reproduced once it stopped:

- **`deploy.py` no longer re-sends a file the board already has.** It checks
  the destination's md5 first. Over that session most deploys were byte
  identical to what was already there - pure cost for no change. `--force`
  overrides.
- **The board-side work is `nice -n 19`.** A deploy that takes a second longer
  costs nothing; a dropped keystroke costs a debugging session.
- **`screenshot.py` says what it costs, every time.** The cheap tool already
  existed and was not reached for, for a whole session. Use `screenshot-hw.py`
  unless the exact pixels are needed - it is lossy JPEG, so pixel-comparison
  work still wants the raw path.

`gzip -1` rather than `-9` in the raw path: 820 ms against 1010 ms median, and
**identical output size** (12,093 bytes both), because a desktop framebuffer is
mostly flat colour. Compressing it hard buys nothing here.


## Driving the desktop without synthesising input

`LVDESK_CTL=1` gives lvdesk a control FIFO at `/tmp/lvdesk.ctl`:

    echo "list"           > /tmp/lvdesk.ctl   # index, size and position
    echo "max 1"          > /tmp/lvdesk.ctl   # toggle maximise
    echo "size 1 700 400" > /tmp/lvdesk.ctl   # resize, and tell the client

It is off unless the variable is set, and it is a FIFO rather than a socket
because the only client is a busybox shell - `echo` needs no tool that is not
on the card.

**Use this for anything that is not specifically testing the input stack.**
Asking "does double-clicking the title bar maximise" through synthetic evdev
means guessing a pixel, hoping the desktop is not stalled when the events land,
and hoping the two clicks fall inside the 400 ms double-click window. Three
separate harness faults were diagnosed as desktop bugs that way, and a maximise
that worked perfectly was reported broken for an hour. The control FIFO names
the operation instead of approximating it.

## The console has three writers - do not frame data on it

hart0's ESP-IDF logging, hart1's kernel printk and our own script all write to
the same serial line, and hart0 does not respect our line boundaries. Its
Wi-Fi chatter lands in the MIDDLE of our output - seen directly as
`ZZ MemTotI (31352) wifi:(phy)...` - so a base64 payload arrives with a hole in
it and decodes to a traceback that reads like a bug in the tool.

`screenshot-hw.py` therefore sends the JPEG over **Wi-Fi**: it captures to a
file, starts `/root/s31-serve` (one file, one connection, sendfile, then it
exits) and the host fetches it. Measured working at 640x400 with prboom
rendering, which corrupted every time over serial. The console still carries
the control chatter, which is small enough not to matter. Serial base64 remains
as a fallback and prints a warning when it is used.

## alive.py proves liveness with a nonce, and a busy board is slow, not dead

It used to poke with a newline and look for a prompt, but `readable_lines()`
drops lines under four printable characters and a prompt is `# `. It matched
only because hart0's chatter supplied longer lines, so a board with a client
rendering - hart0 quiet - reported `STAGE SILENT` while healthy. That one false
negative produced four wrong root causes in a day.

It now echoes a nonce and retries. **If it disagrees with `runsh.py`, believe
`runsh.py`**, and treat the disagreement as a bug in alive.py. Validate any
change to it on all four cases: loaded, idle, mid-boot, and no board at all.

## conlog.py records the console read-only

`conlog.py <out.log> [seconds]` listens and never sends, so it is neither a
console runner nor a reset sequence. It exists because `dmesg` can only be read
from a board that is still alive, which makes it useless for the one failure
worth diagnosing. It prints `ALARM` on panic/BUG/hung-task/OOM patterns so it
can drive Monitor. It holds the port, so fire the workload with `runsh` first
(setsid, output to a file) and start recording after runsh exits. Note that a
healthy idle board is also silent - have the board emit a heartbeat if you need
to distinguish idle from dead.

## board-ok.sh - one line of truth after any flash or reset

    scripts/board/board-ok.sh [budget_seconds]     # default 75

Exit 0 = executing commands AND the screen has content; 1 = executing but the
screen is blank; 2 = never executed a command inside the budget.

It exists because "no answer" was repeatedly reported as a dead board when it
only meant *probed too early*. esptool resets on completion and boot to lvdesk
is ~40 s, during which the panel is legitimately black - so on 2026-09-06 a
board was called dead to a user who was looking at a screen that was black for
that reason. Neither observation meant anything was wrong.

The order is the whole point: **wait for the board to execute a command, then
look at the screen, then say one line.** Never report board state without all
three. `BOARD DEAD` says explicitly that the budget is not the boot window.

## verify-sdl.sh - prove an SDL client got the fast path AND drew

    scripts/board/verify-sdl.sh <w> <h> [--timedemo]

Four claims, each from evidence rather than inference, because inference here
produced four wrong conclusions in a row: a busy board read as dead, a
`tail -2` that hid the very line being looked for, a trace mode that killed the
client it was tracing, and an fps measured across an ntpd clock step.

1. **zero-copy** - `xshim: ZEROCOPY window` in the lvdesk log.
2. **CPU LUT** - checked in the SOURCE, not the log. `xshim_window_pixels()`
   has one depth-8 path (`pal = pal8;` and a scalar loop) and no PPA path, so
   construction proves it and no printf is needed. It also fails if a PPA call
   reappears in `xshim.c`, so the gate cannot silently drift.
3. **it painted** - screenshots taken DURING the run, gated at 28000 bytes.
4. **it was fast** - prboom's own fps, cross-checked against `/proc/uptime`.

Three traps it now avoids, each of which cost a run:

- **Sticky evidence.** A share is logged once, early. A probe late in the run
  will not see it again, and a busy board answering nothing is not a negative.
  Once seen, it stays seen - treating a failed probe as `NO` produced a bogus
  "zero-copy: NO" on a run whose own log had four shares in it.
- **The pixel gate is calibrated on measured frames**, not a round number:
  ~15.8 kB bare desktop, ~19.2 kB window present but BLACK, ~36.1 kB gameplay,
  ~47.4 kB title screen. It was set to 40000 from the title screen alone and
  aborted a run that was rendering gameplay perfectly. 28000 separates black
  from drawing with margin on both sides.
- **A gate that cannot pass is not a gate.** It used to REQUIRE
  `CLUT ... HARDWARE (PPA)`, a string the 2026-09-06 rollback deleted from
  `xshim.c`. It therefore demanded evidence the agreed baseline is incapable of
  producing and scored a healthy board FAIL. Whenever a harness fails, check
  that its evidence strings still exist in the source before believing it.

It bails after two consecutive black shots with no good one, because timing a
window that is drawing nothing yields a number that means nothing, and waiting
out a five-minute timedemo to discover that is the waste it exists to prevent.

## keyview - what a real X client actually receives

`rootfs/keyview.c`, built by `rootfs/build-keyview.sh`. xev is not on this
board, and the question that matters when keys go missing is whether the loss
is BELOW X (keyboard, receiver, HID, evdev, lvdesk's reader) or ABOVE it (our
terminal widget). keyview is the dumbest possible client - one window, select
KeyPress/KeyRelease, print every event - so it cannot be blamed for missing
anything. If a key is absent here it never reached an X client at all.

The GAP column is the millisecond delta by the SERVER's clock, which separates
two different faults: bursty loss shows normal gaps with keys simply absent, a
stall shows one huge gap. It also warns on unmatched press/release, since a
lost release is what makes a key repeat for ever.
