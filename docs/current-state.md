# Where this work stands

Read this first after a context reset. It records what is true of the board
right now, what is in flight, and — most importantly — what has already been
tried and failed, so it is not tried again.

## Where the display stands

Weston runs and paints. Confirmed by eye on the panel, and confirmed here by
dumping the scanout buffer and rendering it (see the harness below).

    keystroke -> glyph, steady state      22-68 ms   (1-3 frames, usable)
    keystroke -> glyph, first ~4 events   8-29 s     <- the remaining problem

The first figure is fine. The second is what "unbelievably slow to type" was:
cold-cache startup of the client and its libraries off a card that charges
~10 ms per request. It is not a display problem.

Fixed to get here, all committed:

- **The page flip was ignored.** Weston double-buffers; the driver logged
  "update wants 0x50c00000 but scanout is at 0x50d00000" and did nothing, so the
  hardware kept displaying one buffer while weston drew into the other. Scanout
  is a cyclic DMA that never stops, so a flip cannot go through dmaengine.
  esp32s31_axi_gdma_retarget_cyclic() rewrites the self-linking descriptors'
  buffer pointers in place; the engine re-reads them each pass, so the switch
  lands at a frame boundary with nothing stopped and no tearing.
- **There was no vblank.** drm_vblank_init() was skipped and flip events were
  completed by hand with invented timestamps, so weston - which schedules from
  "last presentation + refresh" - logged "abnormal: -2880 msec". Vblank now runs
  from a timer at the frame period.
- **eth0** - 30 s of every boot spent DHCPing an interface that does not exist.

## The harness - measure before changing anything

`inputlat` injects through uinput and watches the scanout buffer; `fbdump`
copies the buffer out so it can be looked at as a PNG on the host.

    inputlat <trials> <wait_s> <pointer?> [fps_secs]
    fbdump 0x50d00000 > /tmp/fb.raw     # gzip is ~28 KB, fine over the console

Hard-won rules, each of which cost a wrong answer:

- **Create the uinput device before the compositor starts.** Create it after and
  every key is discarded; the client received nothing over a full minute.
- **Watch the buffer being scanned out, not the one weston renders into.** They
  differ, and the driver logs which is which.
- **Read whole rows.** A glyph changes ~20 bytes per row, so sampling one word
  every 512 bytes misses it - that timed out 18 trials of 20.
- **A framebuffer diff cannot attribute a change to your input.** Unrelated
  repaints read as instant latency; that is how "0.8 ms" got reported for a
  pipeline whose frame is 23.8 ms. Quiesce first, or count plane updates.
- **Use a window longer than the effect.** A 3 s timeout cannot measure a 30 s
  stall; every "timeout" was a discarded measurement.
- **Truncate logs before grepping them for readiness.** A stale "enabled with
  head" reports weston up in 0 s when it never started.

## What to do next, in order

1. **The ~10 ms fixed cost per SD request.** This is the cold-start cost and so
   the remaining user-visible delay, and it also slows boot, imaging and swap.
   Still unexplained after eliminating the busy-wait (0), the lost-IRQ poll
   (~7%), cache maintenance (1%), queue depth, controller config, context
   switches and interrupt count. Next instrument: timestamp a single request end
   to end through the mmc block layer.
2. **Make the VSYNC interrupt fire.** It is wired (source 13, CLIC slot 42) and
   its count stays at zero, so a drifting timer is carrying vblank today. Either
   the CLIC matrix is not routing the source or LCD_CAM needs more than
   LC_DMA_INT_ENA set. A hardware frame boundary does not drift against scanout.
3. **Prime the page cache at boot** so the first launch is not paying full
   cold-start price. Cheap to try; bounded by 13.4 MB of RAM.
4. **Then, and only then, acceleration.** With repaints happening at frame rate
   these finally become measurable against the harness:
   - **The shadow-buffer copy.** Weston logs "uses shadow framebuffer": an extra
     750 KB memcpy per repaint, ~9-17 ms. Best removed by rendering straight
     into the scanout buffer; failing that DMA2D or PPA can do the blit with no
     CPU. Best-evidenced target.
   - **Scanout bandwidth.** The panel eats 32 of ~90 MB/s (36%) continuously at
     18 MHz / 42 Hz, whether anything changed or not.
   - **PIE/SIMD is not a target for pixman** - copies, fills and realistic glyph
     blends all measured at or near the memory ceiling. Possibly worth it for
     FreeType rasterisation, which has not been measured.
   - **Hardware JPEG** would let the panel be streamed continuously for visual
     debugging rather than single snapshots.

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
