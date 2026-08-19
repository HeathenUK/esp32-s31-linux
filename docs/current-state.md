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

## Settled: which swap configuration

**SD swap file, no zram, swappiness 100.** Shipped in
`overlay/etc/s31-swap.conf`. Decided over 15+ runs, scoring CoreMark under a
Weston workload *and* the worst of three execs of a library-heavy binary under
the same memory pressure:

    config                        CoreMark   worst exec   Weston
    SD only, swappiness 100          202.8   4.46/4.50 s  yes    <- shipped
    SD only, swappiness 20           205.2   6.46/10.65   yes
    zram 4M + SD, swappiness 100     195.3   5.89/6.24    yes
    zram 24M only, swappiness 20     176.9   26.12        yes
    zram 4M only, swappiness 100      65.7   -            DIES
    no swap at all                       -   -            never starts

Four things this settled, none of which were obvious beforehand:

- **Swap is not optional.** With none, Weston never reaches output-enable.
- **zram loses on a memory-poor machine.** Its zsmalloc pool costs ~1 MB of
  MemAvailable (4404 -> 3504 kB) to save compression work that was never
  expensive - 4.8 MB written over a five-minute run, about 16 kB/s. On a board
  with more RAM the answer would flip.
- **A zram device holds its disksize, not its compression ratio.** 4 MB could
  never cover a ~9 MB shortfall however well pages compressed, which is why that
  row died. Sized at 24 MB it survives.
- **Throughput alone picks the wrong swappiness.** 20 beats 100 on CoreMark and
  doubles worst-case exec latency, because CoreMark is anonymous-memory-heavy
  and cannot see program text being evicted.

Untested lever: `page-cluster` is still 0, chosen for zram. Every SD number
above was therefore taken under the pessimal readahead setting for the medium
that won. That is the next thing to measure.

### What this says about PIE/SIMD for zram

Nothing worth doing. Measured traffic is 4.8 MB compressed per five-minute run
in the shipped config (~16 kB/s) and 30 MB in the worst config (~100 kB/s).
Against a scalar LZO-RLE rate of tens of MB/s that is well under 1% of a core,
so accelerating it saves a fraction of a percent - and would require saving
vendor register state across context switches and in interrupt context, which is
exactly the machinery whose absence made [[s31-hardware-loop-corruption]]
corrupt userspace silently. Large risk, unmeasurable reward.

## The biggest open performance item: ~10 ms per SD request

Every SD request costs a fixed ~10 ms regardless of size, and it is paid in CPU.
This caps swap at 0.39 MB/s on a path that reaches 12.7 MB/s, and it is the
single largest known performance defect on the board.

Measured, with the page cache bypassed:

    request   throughput   time per request
    4k          0.39 MB/s   9.98 ms
    32k         2.80 MB/s  11.15 ms
    64k         5.23 MB/s  11.95 ms
    1M         12.70 MB/s  78.75 ms

The curve fits `time = 10 ms + size / 13 MB/s`. The data clock is therefore
fine - a 4k read should take ~20 us and takes 500x that.

It is CPU, not waiting. CoreMark falls from 916 to 151 under 4k reads moving
0.4 MB/s, and to 136 under 1M reads moving 12.7 MB/s - the same ~85% of the hart
for 32x less data. Cost per request, not per byte.

Ruled out, each by measurement rather than argument:

- **Cache maintenance** - the counters say 55 ms across a 5.27 s run, about 1%.
- **The lost-interrupt poll** - toggling it changes neither throughput
  (10.15/9.71/10.11 ms) nor CPU (114.6/122.7/121.2 CoreMark, the repeat landing
  with the opposite setting). Interrupts also arrive at ~3 per request, so
  nothing is being completed by the timer; the IRQ path works.
- **Queue depth** - 1 to 8 concurrent readers moves 0.401 to 0.462 MB/s. The
  serialised resource is the hart, not the bus.
- **page-cluster** - see 99-s31-memory.conf; no effect, and it addresses swap-in
  of anonymous pages rather than the file-backed text paging that stalls exec.

What it is not. Each of these was a working hypothesis, and each was killed by
a measurement rather than an argument:

- **The busy-wait in dw_mci_wait_while_busy()** - the leading suspect, since it
  runs before every data command and busy-spins. Instrumented in-tree
  (/sys/kernel/debug/mmc0/busy_wait): **waited=0 over 1380 calls, total_ns=0**.
  The card never asserts BUSY. It also could not have been changed to sleep: the
  call runs under host->lock from __dw_mci_start_request(), so the _atomic form
  is required and not a defect.
- **The lost-interrupt poll** - ~7% by CoreMark displacement, and the handler
  does honour its module parameter. The 0.5 ms interval is not the 10 ms.
- **Cache maintenance** - 1% of wall time by the driver's own counters.
- **Queue depth** - 1 to 8 concurrent readers moves 0.401 to 0.462 MB/s.
- **Controller misconfiguration** - ios reports 40 MHz, 4-bit, SD high-speed.

**The CPU really is being consumed** - this was in doubt, because CoreMark has a
working set in PSRAM and SD I/O does DMA plus cache invalidation, so its
collapse was equally consistent with memory contention on an idle CPU. cpubench
settles it: a serial dependent add chain touching **no memory**, it falls the
same way.

    load        cpubench (no memory)   CoreMark
    idle              64.4 M/s           895.7
    4k reads           9.0 M/s (-86%)    130.7 (-85%)
    1M reads           8.6 M/s (-87%)    112.9 (-87%)

So the hart is genuinely unavailable ~86% of the time, in kernel or interrupt
context, and CoreMark displacement was trustworthy all along.

**/proc/profile cannot localise it, and knowing why matters.** Calibrated
against known loads, samples/sec is identical for an idle machine and a fully
kernel-bound one:

    phase                        secs   samples/sec
    idle                         10.3         284.3
    kernel-bound (zero->null)     9.9         284.0
    user-bound (CoreMark)        13.8          32.5
    SD 4k reads                  11.1         326.0

The idle task runs in kernel mode, so profiling counts it exactly like work;
only user mode is excluded. It measures "time not in userspace", not "time
busy". Anything future needs an instrument that excludes idle: per-task
accounting, ftrace, or IRQ time accounting.

**One real bug found and fixed along the way**, though it is not the answer:
the IDMAC never set IDMAC_DES0_DIC, so every 4 KB descriptor raised a completion
interrupt - 217 per 1 MB request. Setting it cuts that to 88.9 and returns about
28% of the hart under streaming load (cpubench 8.6 -> 11.0 M/s). Request cost is
unchanged, so interrupt count is not what the 10 ms is made of.

**Context switches are not the cost either.** It is 8.3 per 4k request, and
dividing wall time by switches gives a tempting ~1.3 ms each - but the machine
does 1556 switches/sec at idle at full CoreMark, against 764/sec under load. The
rate falls under load; that arithmetic is an artefact.

**Found: it is USB, not SD.** Unbinding dwc2 - touching nothing in the storage
path - halves the cost of every SD request:

                    cpubench    SD 4k cost   dwc2 IRQs
    before           62.1 M/s   13.91 ms/req    1144/s
    dwc2 unbound     69.7 M/s    6.95 ms/req       0/s

dwc2 fires ~971 interrupts/sec on a completely idle machine - the 1 kHz USB SOF
rate - against the timer's 249/s and the SD controller's 2/s. With IRQ time
accounting enabled, an idle board reports 476 of 840 ticks in hard IRQ context.
Trap entry here is extraordinarily expensive (order 10^5 cycles), so a thousand
interrupts a second interleave with every SD request and double its latency.

This is why seven hypotheses inside the SD driver all died: the driver was never
the problem. It also revises [[s31-usb-sof-cpu-accounting]] - with real IRQ
accounting this is not tick aliasing, it is genuine CPU being consumed.

Caveat on magnitude: IRQ accounting says 57% of the CPU, while cpubench gains
only 12% when USB goes away, so tick-based IRQ attribution over-counts (a tick
landing mid-IRQ charges the whole tick to it). The 2x on SD request cost is a
clean A/B inside one run and is the number to trust.

Residual after USB is gone: 6.95 ms per 4k request, still far above the ~20 us a
4k read should take, so there is a second cost underneath. But the first one to
fix is the interrupt rate.

Direction: dwc2 keeps the SOF interrupt unmasked to schedule periodic transfers.
The attached device is a HID keyboard polling at ~10 ms, so servicing every one
of 1000 frames a second is not needed. Either mask SOF when no periodic transfer
is due, or suspend the port when idle. Note [[dont-patch-upstream-drivers]] -
dwc2 is mature and this rate is normal elsewhere; what is abnormal here is the
per-trap cost, so the platform side deserves suspicion too.

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
