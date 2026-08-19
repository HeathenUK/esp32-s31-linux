# Where this work stands

Read this first after a context reset. It records what is true of the board
right now, what is in flight, and — most importantly — what has already been
tried and failed, so it is not tried again.

## The board today

Linux 6.12 on hart1 of an ESP32-S31-Korvo-1 V1.1, kernel XIP from flash, root
on microSD. hart0 runs ESP-IDF and owns the radio.

Working: LCD via a native DRM/KMS driver, USB HID keyboard, microSD root at
40 MHz, Wi-Fi (hidden SSID, see below), I2S audio confirmed by ear, a serial
console at 1 Mbps, and an SD imager that writes and verifies the card over that
console.

**Weston runs.** `Output 'DPI-1' enabled with head(s) DPI-1`, pixman renderer,
RGB565, ~5.7 MB RSS. **It has not been visually confirmed painting** — the panel
showed black at the last check, before several of the fixes below landed. That
confirmation is the immediate open question and needs human eyes.

## Numbers worth not re-measuring

    RAM total                 13.4 MB (16 MB PSRAM less carve-outs)
    CoreMark, idle            ~920 iterations/sec
    PSRAM read, cached        90 MB/s
    PSRAM read, uncached      7.6 MB/s   (the 0xC0000000 alias - a trap, see below)
    internal SRAM read        341 MB/s   (uncached, 512 KB total, mostly spoken for)
    SD read / write           4.75 / 1.7 MB/s at 40 MHz
    LCD scanout               31 MB/s continuous = 34% of PSRAM bandwidth
    Weston RSS                5.7 MB; needs ~7.9 MB of swap to survive
    zram compression          3.6x with lzo-rle
    imaging a 128 MB image    ~4 min transfer, ~6 min including verify

## In flight: which swap configuration

Partial results, and the harness has been the problem rather than the board.

    config      CoreMark idle   under Weston   CPU used   Weston survives
    neither          925             72          92%          no - dies
    SD only          922            249          73%          yes (7.9 MB swapped)
    zram only        921            202          78%          run truncated
    both              -               -           -           never ran

Solid: **swap is the difference between Weston running and dying.** Unsettled:
which kind. Note the surprise — SD swap left *more* CPU free than zram, because
zram spends CPU compressing while SD spends I/O wait that CoreMark can use.

Invalid: the `startup_s` column. Weston appends to its log, so the grep for
"enabled with head" matched the previous run instantly. Fix by truncating the
log before each run.

Also note the comparison is tilted: `swappiness=100` and `page-cluster=0` were
chosen for zram, so the SD-only row runs settings picked for a different medium.
A low-swappiness run is needed to separate medium from tuning.

## Tried and failed - do not repeat

**On the black screen / Weston bring-up.** Five separate faults had to be fixed;
each hid the next, and several plausible theories were wrong:

- *FPU not enabled for userspace* - disproved, `awk` computes floats correctly.
- *My SD DMA change (`clean_before_fromdevice=N`) corrupting page cache* -
  disproved, the SIGSEGV/SIGILL faults occurred with it set both ways. Those
  faults were memory exhaustion, and vanished once swap existed.
- *Missing DRM format modifiers* - patched, did not help. Weston uses the
  non-modifier `drmModeAddFB2` path for dumb buffers; its source says so.
- *Framebuffer pool too small for AddFB2* - it was the pixel format, not size.
  A 2 MB pool holds exactly two 800x480 RGB565 buffers (each 750 KB rounds to a
  1 MB slot via `get_order()`).
- *A 4 MB pool before swap existed* - actively harmful, it starved Weston of the
  RAM it needed and the compositor could no longer start at all.
- `--drm-format=rgb565` on the command line - not an option in Weston 15, fatal.
- `[output] format=rgb565` in weston.ini - silently ignored. The key is
  **`gbm-format`**, in `[core]` and per-output. With the wrong key Weston asks
  for XRGB8888 and this RGB565-only plane refuses it.

What actually fixed it: `CONFIG_EPOLL`/`TIMERFD`/`EVENTFD` (missing, so
`wl_display_create()` failed at `epoll_create1`), `CONFIG_FILE_LOCKING` (missing,
so `flock` returned ENOSYS and Weston reported "failed to add socket"), dropping
`DRM_FBDEV_EMULATION` so fbcon stops holding the only pool slot, swap, and
`gbm-format`.

**Elsewhere.**

- *GT1158 touch* - blocked on hardware, not software. The controller answers at
  0x14 with a valid 800x480 config but never scans, and Espressif's own BSP marks
  both its INT and RESET lines `GPIO_NUM_NC`. See `docs/touch-gt1158.md`.
- *BitScrambler for the SD path* - impossible; its attach list is GDMA
  peripherals and SDMMC drives its own IDMAC.
- *A DMA bounce buffer in the uncached PSRAM alias* - a trap. At 7.6 MB/s it is
  slower than the cache-maintenance path it would replace. SRAM at 341 MB/s is
  the only sensible target, and only 512 KB exists.
- *The Wi-Fi "copy path"* in PERFORMANCE-ROADMAP.md - struck. Copying is under
  1% of the cost; loopback costs the same CPU as the radio.
- *`swapon -p`* - busybox has no priority option. Not needed: the kernel
  auto-assigns descending priorities in swapon order, so starting zram (S03)
  before the SD file (S04) already makes zram primary and the card overflow.

## Instrumentation - the actual cost centre

More time has gone on driving the board than on the board itself. Rules:

- Use `scratchpad/runsh.py`, which ships a script as a file and runs `sh` on it.
  Never flatten a script to one line with `; ` - `for x; do` becomes `do;`.
- **`python3 -u` always**, write results incrementally, `sync`, and verify every
  setup step by echoing it back. Runs have been lost to an unverified deploy, a
  config write that was never synced before a reset, and a check written as
  `x in y or 'assumed'` which prints "assumed" regardless.
- The console becomes unusable while Weston renders. Write results to a file on
  the card, reset the board, then read them on a clean boot.
- Cold boot to login prompt is ~22 s; budget generously.
- hart0's power-manager log is silenced by `CONFIG_S31_QUIET_PM_LOG`; turn it
  back on only when debugging frequency switching.
- Wi-Fi is a **hidden** SSID, so `wpa_supplicant` needs `scan_ssid=1`.
  Credentials are in memory, deliberately not in this repo.

## Next

1. Confirm by eye that Weston paints. Everything else is downstream of that.
2. Finish the swap comparison: truncate the log per run, complete zram-only and
   both, add a low-swappiness run.
3. Then the accelerated-compositor work in `docs/weston-acceleration-plan.md`,
   which needs `drm_simple_display_pipe` replaced by a full CRTC plus planes
   before PPA can be used at all.
