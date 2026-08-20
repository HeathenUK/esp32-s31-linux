# Where this work stands

Read this first after a context reset. It records what is true of the board
right now, what is in flight, and — most importantly — what has already been
tried and failed, so it is not tried again.

## Where the desktop stands

Weston runs on the panel. Behaviour is BIMODAL, not uniformly slow. Measured
2026-08-20, desktop-shell, uinput injection, detector null-validated (see below):

    5 trials at 25 s spacing (trials isolated - spacing > latency):
        66.6   75.8   83.8   63.0   17046.4   ms

    12 trials at 2 s spacing (sustained pressure, no recovery time):
        min 13.7 s   median 16.2 s   max 22.2 s

Typical keystrokes cost 63-84 ms. Roughly one in five costs ~17 s. The fault
counters attribute it exactly: +504 major faults across the 5-trial run, almost
all inside the single slow trial, at ~34 ms of SD latency each.

Both numbers are real and measure different regimes. At 2 s spacing the system
never recovers, so every keystroke pays; at 25 s spacing the working set mostly
stays resident until something evicts it, and then one keystroke pays the whole
bill. The user reports ~20 s from a physical keypress, consistent with hitting
the bad case.

**The defect is periodic eviction of the desktop working set, not per-keystroke
slowness.** Cost of one bad keystroke, measured: ~500 major faults blocked on SD
(~10-17 s), 2.8 s system CPU, and only 0.4 s of actual glyph rendering.

**WITHDRAWN - earlier numbers in this file were wrong.** A previous revision
claimed 40-60 s -> 3.56 s worst and ~10.6 s -> 73 ms median. Those do not
reproduce and must not be quoted. The likely fault: `inputlat` watches the
scanout buffer for *any* change and attributes it to the injected key, so it
latches onto unrelated Weston repaints (cursor, its own damage) and reports an
interval far shorter than the real keystroke-to-glyph path. The same failure
mode was caught once before, when a "13 ms median" was rejected as physically
impossible; it was fixed for that case and evidently not in general. A longer
2 s spacing gives the detector less unrelated churn to mistake for a glyph,
which is why the honest numbers only appeared once spacing was raised.

What survives from the XIP work is the *mechanism*, not the end-to-end claim:
identical code measured 5.98x faster from RAM than flash in a self-contained
microbenchmark, and the timer interrupt went 0.864 -> 0.465 ms. Any
keystroke-to-glyph delta attributed to it is unproven.

Before trusting any latency number from this harness, validate BOTH ways:

1. **Null test** - sample the framebuffer with no input injected. Run
   2026-08-20: 0 spontaneous changes in 19 comparisons on each buffer, so the
   display is static without input and the detector is not timing cursor blink.
   Note `fbdump` takes positional `<base> <len>`; there is no `-d` flag, and a
   first attempt using one silently compared empty strings and would have
   reported a clean result regardless.
2. **Spacing > latency** - if trial spacing is shorter than the real latency,
   the previous glyph lands during the next trial and is credited to it. That
   produces one honest slow trial followed by implausibly fast ones (1.5 ms was
   observed, against a 23.8 ms frame and ~56 ms of render CPU). Always space
   trials wider than the worst case being measured.

The cause of the storm is still believed to be memory: the working set is ~25 MB
against 13.4 MB of RAM, so roughly 15 MB is evicted to the SD card. Measured
during the run above: MemAvailable fell to 1.8 MB with 14.5 MB in swap.

### What produced that

**Hot kernel code moved from XIP flash into RAM.** This kernel executes XIP from
80 MHz **DIO** flash (verified in the boot log and `bootloader/sdkconfig`; this
file previously said QIO, which was wrong) through a 16 KB instruction cache, and identical code measured
**5.98x faster from RAM** (72 KB per side, larger than the cache, so neither can
hold it). 113 KB now lives in RAM via `.text.fast`:

    timer subsystem   hrtimer, timer, tick-sched, tick-common, timekeeping,
                      clockevents
    storage path      dw_mmc, blk-mq, blk-core, bio, blk-merge, blk-mq-sched,
                      mmc_ops, sd_ops

    timer interrupt        0.864 -> 0.465 ms   (kernel watchdog now silent)
    SD wall per request   12.07 -> 8.05 ms     (three runs: 8.75/8.73/8.05)
    4k reads               0.39 -> 0.45 MB/s
    1M reads              12.21 -> 14.95 MB/s

The SD gain compounds: the latency tail *is* eviction to that card, so cheaper
page-outs should shorten the storm - but the end-to-end gain is UNPROVEN;
see the withdrawal above.

**Also fixed:** vblank plumbing and the page-flip DMA retarget (the driver was
ignoring flips and scanning out the stale buffer), high-resolution timers
(`CONFIG_HIGH_RES_TIMERS` was off, quantising every sleep to 4 ms), eth0 (~30 s
per boot spent DHCPing an interface that does not exist), and `FILE_LOCKING`.

### How the relocation works

`.text.fast` sits between `_sdata` and `__bss_start`, so the existing XIP copy in
`arch/riscv/mm/init.c` relocates it to RAM at boot with no extra code, and RAM is
executable because this architecture does not enforce kernel RWX.

`TEXT_TEXT` is redefined in `vmlinux-xip.lds.S` for that script alone: the main
`.text` output section appears first and claims `*(.text)` from every object, so
collecting whole objects into `.text.fast` without the override produces an
**empty section** - which is what a first attempt did.

Two traps worth keeping:

- **Do not add `traps.o` or `irq.o`.** Including them lost the irq tracepoints
  (`/sys/kernel/tracing/events/irq` disappeared), removing the instrument.
- **`__fasttext` conflicts with `__sched`.** Both are section attributes; annotate
  such functions via the linker instead.
- Annotating a function relocates only *that* function. Where the work is inlined
  into a caller still in flash, nothing moves - which is why the first round of
  callee annotations grew the section 576 bytes and changed nothing.

### What is left, and what it needs

The remaining ~16 s is the eviction storm. Two routes, no others:

1. **More of the paging path in RAM** - `vmscan`, `page_io`, `ext4`. Whether this
   pays should be measured, not assumed: every KB moved to RAM is a KB less for
   the working set that is already 15 MB over. A profiling kernel (PROFILING +
   KALLSYMS, `profile=2`) sampled during the storm would rank the candidates;
   during a storm the CPU is genuinely busy, so the usual idle-counts-as-kernel
   objection does not apply.
2. **A lighter client stack.** Weston plus a cairo/pango terminal cannot fit in
   13.4 MB. Dropping the background image and panel removed two of four slow
   interactions, confirming the mechanism, but ~12 MB cannot be trimmed from that
   stack.

Not available, each checked: the icache is fixed in silicon; flash is already at
its 80 MHz ceiling with quad mode enabled at runtime; function tracing needs
`HAVE_DYNAMIC_FTRACE`, which riscv selects only `if !XIP_KERNEL`; HZ=100 was
tried and reverted (SD got worse, wake latency unchanged); zram was tried and
reverted (median 13 s -> 60 s - it compresses 10.9x but takes its pages from the
RAM already exhausted).

### Cautions for whoever picks this up

- **Watch `arch/riscv/configs/esp32s31_defconfig` for uncommitted changes.** It
  accumulated 16 lines of drift that were silently changing every build, and
  reverting it to HEAD removed `FILE_LOCKING` and stopped weston starting at all.
  `FILE_LOCKING` is now set from the top-level Makefile instead, because the
  defconfig carries an explicit "is not set" further down that overrides a
  prepended line.
- **Killed builds corrupt `.cmd` files.** A tool timeout mid-write left
  "unterminated call to function 'wildcard'", which silently skipped relinking so
  a stale image was flashed and measured for several cycles. Delete the truncated
  files and rebuild.
- **The bootloader lives at 0x2000, not 0x0**, the partition table at 0x8000 and
  the hart0 app at 0x20000. Writing the bootloader to 0x0 bricks the boot.
  Deleting `bootloader/sdkconfig` regenerates it at 2 MB flash size and 115200
  baud, which reboot-loops the board.
- **`fbdump 0x50d00000` plus a small RGB565->PNG script** shows the panel; the
  live scanout buffer gzips to 4-28 KB, so stills over the console are cheap.
  `weston-screenshooter` returns solid black and cannot be used.

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
