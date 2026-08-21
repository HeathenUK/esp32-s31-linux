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

**Do not trust the "typical 63-84 ms" reading**: it came from a single 5-trial
run. A later run of the same test on the same board gave 2.4 / 17.0 / 12.2 /
17.1 / 14.8 s - every trial slow. The desktop is routinely multi-second, which
is what the user reported from the keyboard all along. The fault counters do
attribute the slow trials exactly: ~500 major faults each, at ~34 ms of SD
latency.

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

## Where the desktop is now (2026-08-21)

    keystroke-to-glyph, median, 5 trials at 25 s spacing, null-validated detector

        14.76 s   morning: flash in DIO, no XIP
         7.59 s   after switching the flash to QIO
         5.58 s   after XIP libraries via LD_LIBRARY_PATH
         4.85 s   after overlaying the XIP image over /usr/lib
         0.71 s   with a smaller client surface            <- 95% off, usable

The last step was a 3.1x smaller Wayland buffer, obtained crudely with
`weston-terminal --font-size=6`. Trials 633/685/707 ms after a 2.6 s warm-up.
It shrinks the visible window too, so it is a proof of mechanism rather than the
shipping answer - see "surface size is the dominant cost" below.

### 1. The flash was running DIO, not QIO

The single biggest win of the day, and it was free. The kernel executes XIP from
flash, so the SPI mode sets instruction-fetch bandwidth for the whole system.
Same 80 MHz clock, four bits per clock instead of two:

    SD 4k O_DIRECT read     8.12-8.43  ->  4.44-4.74 ms/req   (-46%)
    CoreMark                     ~845  ->  985.8 iter/s       (+17%)
    desktop keystroke          14.76 s  ->  7.59 s            (-49%)

Everything else on this axis is already at its ceiling: 80 MHz is the maximum
the S31 offers (20/40/80 only, no 120), OPI and DTR need octal flash silicon and
this part is a quad NOR, and PSRAM is already octal at 250 MHz - above IDF's own
200 MHz default.

**Beware the image header.** ESP-IDF deliberately writes "dio" into it even when
QIO is selected, because the ROM loader reads that header in DIO to fetch the
second-stage bootloader, which then switches the flash to quad at runtime. Read
the bootloader's own `SPI Mode : QIO` line, not the header.

### 2. Userspace text executes in place from flash

Library text no longer occupies RAM or faults from SD. Four pieces:

  - `drivers/mtd/devices/esp32s31_flash.c` implements `->_point()`, handing out
    the address in the always-mapped 16 MiB flash window instead of a copy.
  - `CONFIG_CRAMFS` + `CONFIG_CRAMFS_MTD`, mounted as `mount -t cramfs
    mtd:rootfs /mnt/xip`.
  - The image is built with **`mkcramfs -X -X`** - `-X` TWICE. Once aligns data
    to 8 bytes and the kernel refuses with "data is not page aligned"; the
    second `-X` sets `opt_xip_mmu` and aligns to a page. The help text does not
    say this, and a single `-X` yields an image that mounts, runs, and silently
    never XIPs.
  - `arch/riscv/mm/cacheflush.c`: `flush_icache_pte()` did
    `page_folio(pte_page(pte))` unconditionally. Text mapped from the flash
    window sits below PHYS_RAM_BASE and has no memmap entry, so this walked a
    wild pointer and oopsed inside execve. Guarded with `pfn_valid()`.

Delivery matters as much as the mechanism. `LD_LIBRARY_PATH` only covers
DT_NEEDED libraries; Weston `dlopen`s its shell and backend plugins by absolute
path, and the ELF interpreter path is baked into every binary. Overlaying the
image over `/usr/lib` catches all three:

    mount --bind /usr/lib /mnt/sdlib
    mount -t overlay overlay -o lowerdir=/mnt/xip/usr/lib:/mnt/sdlib /usr/lib

libc comes along for free because `/lib/ld-musl-riscv32-sf.so.1` is a symlink to
`../usr/lib/libc.so`. Anything not in the image falls through to SD.

Flash layout was regrown for it: linux 8 -> 6.75 MB, rootfs 4 -> 5.25 MB at
0xAC0000, `persist` preserved. **The hart0 loader validates both partitions
against hardcoded constants in bootloader/main/main.c and refuses to boot on a
mismatch** - changing only partitions.csv gives a boot loop printing "Linux
partition not found". That is the check working.

    Weston dependency closure   23 objects, 4.92 MB text, 0.12 MB data  (36:1)
    image shipped               5.13 MB incl. libc and plugins, in a 5.25 MB part

### 3. What is left: it is data now, not text

One keystroke, measured with the overlay active:

    terminal   minflt +568   majflt +430   utime +75   stime +78
    weston     minflt +33    majflt +22
    terminal   VmRSS 1436 kB = RssAnon 120 + RssFile 100 + ~1116 shmem

**452 major faults still per keystroke**, but `RssFile` is down to 100 kB - the
text problem is solved. The terminal's memory is now dominated by ~1.1 MB of
**shmem: the Wayland shared buffers** between client and compositor, which are
RAM pages being swapped out and faulted back in.

### Surface size is the dominant cost, and it is measured

Shrinking the client surface 3.1x (via `--font-size=6`, as a proof of mechanism):

    wayland buffer      1,290,240 B  ->  409,600 B   each, double buffered
    majflt/keystroke            430  ->        150
    median latency          4854 ms  ->     707 ms

That is the whole remaining problem in one measurement. The causal chain -
buffer size -> shmem resident -> major faults -> latency - is confirmed end to
end.

**Client buffers are ARGB8888 on an RGB565 display.** Each buffer is 0x13B000
bytes for a ~800x403 window: exactly 4 bytes/pixel. Weston converts down to
RGB565 for scanout every frame, so we pay double the memory *and* a conversion
pass. Moving clients to RGB565 would halve the buffers with *less* CPU, and the
display, the PPA and Weston's pixman renderer all handle it natively - but
`wl_shm` format choice is client-side and weston-terminal builds cairo surfaces
as ARGB32, so it needs a patched client. Lower colour depth is otherwise a dead
end: 12-bit is not byte-aligned and has no hardware path, 8-bit needs a palette.

**The shipping form is a smaller DRM mode plus PPA SRM upscaling**, because it
works entirely in the driver: advertise e.g. 400x240, upscale into the 800x480
scanout buffer, and every client's buffers shrink without the client knowing or
the visible content getting smaller. Note it does not reduce scanout traffic -
the LCD DMA reads 768 KB every frame regardless - but that is not the bottleneck;
client buffers are.

## Plan: minimise client buffer memory (next work)

Client buffer today is 800x403x4B = 1.26 MB, double buffered. The floor worth
aiming at is 400x240x2B = 192 KB - a 6.6x reduction - by attacking both terms.

                 pixels     bytes/px     buffer     total (x2)
    now          800x403        4       1,290 KB     2,580 KB
    phase 1      400x240        4         384 KB       768 KB
    phase 3      400x240        2         192 KB       384 KB

### Phase 1 - smaller DRM mode + PPA SRM upscale   (4x, driver only)

The core work, and the only lever that shrinks buffers for *every* client
without modifying any of them, which the off-the-shelf constraint requires.

  1. Implement PPA SRM in esp32s31-ppa.c alongside the working fill and blend.
     Same 2D-DMA pattern: one TX channel feeding the source, one RX taking the
     scaled result. Registers PPA_SRM_* (0x20, 0x28, 0x64); IDF's ppa_srm.c is
     the reference and uses SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX/RX.
  2. Advertise a scaled mode in esp32s31-lcd.c. The connector exposes only the
     native 800x480 today; add 400x240 (and maybe 640x384). Weston picks it up
     as an ordinary mode - no Weston changes.
  3. Upscale on flip: in the atomic commit path, when the framebuffer is smaller
     than the panel, SRM-scale it into the scanout buffer rather than pointing
     the scanout DMA at it.
  4. Wire damage clips. The driver logs "drm_plane_enable_fb_damage_clips() not
     called"; with them, SRM only reprocesses damaged rectangles, so a cursor
     blink costs a tiny scale rather than a full-screen one.

  Cost: a full-screen 400x240 -> 800x480 SRM is ~1 MB of traffic, extrapolating
  from the measured blend to ~5 ms against a 23.8 ms frame. Damage clips cut it.
  Risk: text upscaled 2x is slightly soft - but full size, unlike the font hack.

### Phase 2 - reclaim the framebuffer reservation   (~750 KB back)

With Weston rendering at 400x240 its own buffers drop 768 -> 192 KB each. The
lcd_reserved region is 2 MB and `nomap`, carved out of system RAM at boot:

    now       2 x 768 KB scanout                 = 1.5 MB of a 2 MB reservation
    phase 2   2 x 192 KB render + 1 x 768 KB out = 1.15 MB -> reserve 1.25 MB

Requires the DT reservation and the driver's buffer allocation to change
together.

### Phase 3 - 32 -> 16 bit colour   (a further 2x)

Display, PPA and Weston's pixman renderer all speak RGB565 natively, and Weston
already converts down to it every frame, so this saves memory *and* CPU. The
obstacle is that wl_shm format choice is client-side.

  - First check whether the compositor can force it: if Weston can advertise
    only RGB565 in wl_shm, cairo clients should negotiate down without patching,
    keeping this off-the-shelf.
  - If not, it needs a client patch - park it rather than break the constraint.

  Not pursued: 12-bit is not byte-aligned and has no hardware path; 8-bit needs
  a palette.

### Phase 4 - verify

Re-measure with the standing methodology: 5 trials, 25 s spacing, null-validated
detector, fresh boot between comparisons (DRM master does not release promptly
on killall - running two compositors in one session invalidates the result).
Track buffer size, majflt/keystroke and median latency together; those three are
the causal chain.

**Expected:** phase 1 alone takes buffers to 384 KB total and, on the measured
430->150 fault relationship, should put latency near the 0.71 s already observed
- but with a full-size readable screen. Phase 3 roughly halves the rest again.

**Order matters:** phase 1 first, largest win and unblocks phase 2; phase 3 last
because it is the one that risks needing a client change.

## Why the desktop is slow: it does not fit, by ~2.8 MB

Measured 2026-08-21, and this is the conclusion the rest of the tuning should be
read against:

    desktop working set   6.6 MB   weston 3.8 MB + weston-terminal 2.8 MB
    available at rest     3.8 MB
    deficit              ~2.8 MB

Weston's RSS collapses from 3784 kB to 432 kB once the terminal starts - that is
its text being evicted, not it using less. Every keystroke then faults it back
in from SD.

### What does not work, with numbers

    disable Bluetooth + IPv6 (both subsystems)   +364 kB   and latency unchanged
    kill on-screen keyboard + crond              +100 kB
    relocate mmc core hot path to RAM            ~5% on SD, not on latency
    relocate mm reclaim/swap path to RAM (97 kB) no measurable effect (reverted)
    PPA / DRM plane integration                  0 - see below

Trimming yields hundreds of KB per change against a multi-MB deficit. It cannot
close the gap.

**PPA does not help this.** The premise was that overlay planes would free
Weston's pixman shadow buffer. There is no shadow: Weston's pixman renderer
draws directly into DRM dumb buffers, and there are two of them (770048 bytes
each) inside the 2 MB `lcd_reserved` region, which is `nomap` and already
excluded from MemTotal. They cost system RAM nothing. CPU is not the constraint
either - 0.4 s of a 13.7 s keystroke. The PPA work is good hardware
acceleration and worth having, but it is not the fix for this.

### The two changes big enough to matter

  1. **A lighter terminal, ~2.4 MB.** weston-terminal is 2.8 MB almost entirely
     because of cairo/pango/freetype. A bitmap-font Wayland terminal is
     200-400 kB. Biggest single lever on the board, and it keeps the real
     desktop shell.
  2. **Reclaim the framebuffer reservation, ~1 MB.** `lcd_reserved` is 2 MB and
     Weston double-buffers into 1.5 MB of it. Single-buffering allows shrinking
     the reservation to ~1 MB; because the region is carved out of system RAM at
     boot, the difference goes back to the kernel. Worth three times what
     removing two kernel subsystems was.

## SD request cost: what the ~8 ms actually is

Measured 2026-08-21. A 4k O_DIRECT read costs **~7.8-8.2 ms**, and the cost is
per *request*, not per byte: 64k costs 11 ms, so 16x the data adds only 32% more
time. Reproducible to +-0.15 ms when measured properly.

    card busy wait      0 ms      0 waits in 2617 calls - the card never stalls
    inside dw_mmc       1.62 ms   before/after req_timing over 469 requests
    idle exit           0 ms      no cpuidle driver; busy-loop A/B disproved it
    runtime PM          0 ms      "echo on > power/control" changed nothing
    unaccounted        ~6.5 ms

The unaccounted part is **CPU-bound, not waiting**: pinning the CPU with a busy
loop made a read burst 56% *worse* (8.4 -> 13.0 ms/req), which cannot happen if
the path is idling on hardware.

### Profile of a 1500-request read burst

`/proc/profile` with `profile=6`, resolved against System.map:

    finish_task_switch      26.6%   (largely idle - see the caveat below)
    process_scheduled_works  5.8%
    __queue_work             4.1%
    __wait_for_common        2.8%
    swake_up_locked          2.1%
    bh_worker                1.7%
    __schedule               1.3%
    __pm_runtime_resume      1.3%
    mmc_blk_mq_issue_rq      1.3%

Caveat: `/proc/profile` attributes idle to whatever the idle path touches, and
`finish_task_switch` is where the CPU lands after any switch including returning
from idle. Read that 26.6% as ~2 ms/request of waiting for the card, not CPU.

What *is* real CPU: workqueue dispatch - `process_scheduled_works` +
`__queue_work` + `bh_worker` + `mod_delayed_work_on` is ~12%, about **0.9 ms per
request**, for a path that is one IRQ and one completion.

The MMC and block code is nearly absent from the profile (`mmc_blk_mq_issue_rq`
1.3%), which explains why relocating the mmc core hot functions to RAM bought
only ~5%. Those annotations are kept - they are cheap, ~1.3 KB - but this is not
where the time goes.

### Measurement traps, both hit

  - **`dd` startup is ~150 ms.** Over count=100 that is 1.5 ms/req of pure
    process overhead; over count=500 it is 0.3 ms. Comparing runs with different
    counts manufactures differences that are not there. Use count>=500.
  - **`profile=2` allocates a buffer the size of the text segment**, ~4 MB on a
    12.8 MB machine. The kernel boots and then userspace starves; it looks
    exactly like "hangs in udev and never reaches a shell", which is what it was
    misdiagnosed as for most of a day. Use `profile=6` (256 KB).

### Not yet explained

About 3-4 ms per request remains unattributed after card wait, driver time and
workqueue overhead. `/proc/profile` at 250 Hz over 1500 requests does not have
the resolution to split it further; this needs per-request tracing rather than
sampling.

## PPA (Pixel Processing Accelerator)

Working under Linux as of 2026-08-21: `drivers/gpu/drm/espressif/esp32s31-ppa.c`,
DT node `pixel-accelerator@20345000`. Fill is implemented and verified; blend and
SRM are not yet written.

    800x480 RGB565 fill (768 KB)   4.43 - 4.97 ms   (~165 MB/s), off-CPU
    64x64 fill (8 KB)              0.39 - 0.56 ms   (overhead-dominated)
    completion                     interrupt-driven, 361 us for a small fill

The point of this is memory, not CPU. Weston's pixman renderer holds an 800x480
RGB565 shadow buffer - 768 KB of a 12.8 MB machine - and a hardware blend path
lets that go. Measured, the desktop's bad keystroke is ~500 major faults blocked
on SD and only 0.4 s of actual rendering, so free RAM is the constraint.

### What it took, and what will bite again

Four separate enables, in three register blocks, none visible from the vendor's
operation-level code. Symptom of missing any of them: the 2D-DMA arms, completes,
and reports INFIFO_UDF - it starved - and zeros land in the target buffer.

    HP_SYSTEM PPA/2DDMA mem_lp_ctrl   LP_EN defaults to 1: both blocks' internal
    (0x2058629c / 0x20586210)         memories are POWERED DOWN out of reset.
                                      This was the one that mattered.
    DMA2D RST_CONF.CLK_EN (0xa04)     a second, global module clock, separate
                                      from the HP_SYS_CLKRST gate
    DMA2D RST_CONF AXI FIFO resets    read and write master FIFOs
    PPA REG_CONF.CLK_EN (0x6c)        engine clock, defaults to auto-gated

Also required: single-block descriptor mode (not multiple), macro-block size
NONE (the field defaults to 8x8, wrong for a plain RGB565 fill), descriptor
burst enabled, and the descriptor in **uncached** SRAM - `dma_alloc_coherent`
returns cached memory here, so the engine reads a stale descriptor and never
starts. The AXI GDMA driver has the same constraint and says so.

Fill colour is **ARGB8888** whatever the output colour mode; the engine converts.
Passing a raw RGB565 value fills near-black. Output is already RGB565 - there is
no CPU-side conversion anywhere.

### Interrupt source numbers - do not count enum entries

The PPA interrupt was dead because the DT source was 94 rather than 102.
Deriving it by counting matches of `ETS_.*_INTR_SOURCE` in `soc/interrupts.h`
undercounts by 8: `ETS_GPIO_INTR0-3_SOURCE` and `ETS_CPU_INTR_FROM_CPU_0-3_SOURCE`
do not end in `_INTR_SOURCE`. Checking the method against LCD_CAM appeared to
validate it only because LCD_CAM sits *before* those entries.

Verify against a known-good node (USB OTGHS is source 99) or read the live
interrupt matrix: `devmem 0x20585800 + source*4` on the running board shows
`PASS_LEVEL | slot`. **busybox `devmem` is on the rootfs** and is by far the
fastest way to test a register hypothesis - no rebuild, no reflash.

## LCD vblank: the hardware interrupt works

Previously believed dead. It is not. Enabling `LCD_VSYNC_INT` by hand:

    devmem 0x20396070 32 0x1    # INT_CLR
    devmem 0x20396064 32 0x1    # INT_ENA
    -> 82 interrupts in 2 s on CLIC slot 42, i.e. ~41 Hz, the panel's refresh

The interrupt, its routing and the driver's handler are all fine. The count is
zero only because nothing enables it: the driver arms it in the vblank-enable
path, and that is only reached when a DRM client asks for vblank. So the driver
can use real hardware vblank instead of the hrtimer approximation - worth doing,
because the hrtimer only approximates frame boundaries.

Note `LCD_UNDERRUN` (bit 4) is latched in `int_raw` and has not been looked at.

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

### PPA SRM works - phase 1 of the buffer plan

Hardware scaling is up. `/sys/kernel/debug/esp32s31_ppa/srm` takes
`<hex src> <sw> <sh> <hex dst> <dw> <dh>` and scales RGB565 to RGB565.

Measured, solid-colour source, buffers in `lcd_reserved`:

        400x240 -> 800x480    7.75 ms
        320x192 -> 640x384    5.38 ms
        400x240 -> 400x240    2.90 ms
        800x480 -> 400x240    7.19 ms

Correctness was checked two ways: a solid source must produce a bit-uniform
output (gzip of the result drops from 40-90 KB when broken to ~200-800 bytes
when clean), and a banded source must land its bands on the expected rows.
Both pass; boundaries show the two-row bilinear transition a scaler should
produce.

Two things cost the whole bring-up and are worth writing down:

- **Leave the PPA macro block size at its 32x32 reset default.** IDF's
  `ppa_ll_srm_get_mb_size()` only *reads* the field - nothing ever writes it -
  so the vendor driver silently depends on the default. Forcing 16x16 (which
  tiles 400x240 exactly, so it looked like the better choice) wedges the
  engine: `PPA_SRM_STATUS` shows the input side scanning and the output side
  stuck, `param_err` stays 0, and the output DMA never fetches its descriptor.
- **The descriptor-port block is macro block + a one-pixel border per side**,
  so 32x32 needs **34x34**. 16x16 needs 18x18, and generalising that as
  "+4 for 32x32" is wrong. This one does not fail loudly: it corrupts three
  output pixels at every macro-block seam - every 64 output pixels at 2x -
  and the bad values are red-ish interpolations that look like legitimate
  scaler noise rather than a bug.

Debugging notes for next time:

- `PPA_BLEND_FIX_PIXEL` is **ARGB8888 regardless of output colour mode**, so
  the `fill` debugfs entry takes ARGB8888. Passing RGB565 (`f800`) writes
  `0x07c0`, and that near-miss wasted a verification round.
- `fbdump` needs a **page-aligned** address; `mmap` refuses otherwise and the
  dump comes back empty. Every 64th row of an 800-wide RGB565 picture is
  page aligned.
- **Silence the console (`dmesg -n 1`) before dumping base64 over serial.**
  Driver `dev_info` lines interleave with the stream and corrupt it.
- Cached (`0x50c00000`) and uncached (`0xc0c00000`) reads of PPA output agreed
  here, so the DMA-vs-cache trap did not bite - do not assume it is the cause.

### Rendering below panel resolution - phase 1 complete

`esp32s31-lcd.c` advertises a reduced-size mode next to the panel's own and
upscales every flip into a private scanout buffer with the PPA. `render=WxH` is
a module parameter, writable at runtime
(`/sys/module/esp32s31_lcd/parameters/render`) so an A/B needs no reflash - set
it, force a connector reprobe (`echo detect > /sys/class/drm/card0-DPI-1/status`),
then start weston. Weston needs no configuration change; it just sees a mode.

**Only some sizes are legal.** The scaling coefficient is 8.4 fixed point, so
there must be an integer K with `render * K / 16 == native`, and both axes must
share one K or the image is stretched. For this 800x480 panel that is exactly:

        800x480 (K=16)  640x384 (K=20)  400x240 (K=32)
        320x192 (K=40)  200x120 (K=64)  160x96  (K=80)

640x480 is **not** usable: it is 4:3 against a 5:3 panel, so filling the panel
would need a horizontal-only 1.25x stretch. It is refused, as is 512x320
(K=25 horizontally, 24 vertically - subtly anisotropic).

Matched runs, cold boot each, identical script, keystroke to visible change:

        render      pixels   median      min    MemAvailable
        800x480      100%   5751.7 ms   5236 ms      708 kB
        640x384       64%   4763.4 ms   -            764 kB
        400x240       25%   2586.1 ms   2132 ms     1032 kB
        320x192       16%   2403.5 ms   1939 ms      824 kB

400x240 is reproducible (2579 ms and 2586 ms on two separate boots). Verified
visually: the panel shows a correctly upscaled Weston desktop, right colours,
no seam artifacts.

**The curve flattens hard.** Below 400x240 a further 36% cut in pixels buys 7%.
Resolution is a real lever but a bounded one, and the floor it converges on
(~2.4 s) is still far from usable. Whatever dominates the remaining time is not
pixel work.

**The mechanism is not what the plan assumed.** The plan predicted the win from
smaller *client* buffers. It is not: `weston-terminal`'s window is a fixed pixel
size, so its RSS did not fall - it rose. The gain is compositor-side, a quarter
of the pixels to composite in pixman and a quarter of the buffer to flush per
frame. Two consequences:

- The remaining phases aimed at client buffers (32->16 bit colour) will not pay
  what the plan estimated, and should be re-costed before being built.
- At 400x240 a default `weston-terminal` no longer fits on screen. Rendering
  small trades screen real estate for latency, which matters for the "off the
  shelf desktop" goal - apps assume a certain pixel budget.

Notes:

- The CRTC is always driven at the panel's native timing; the plane framebuffer
  is the small one and is never scanned out directly.
- The connector belongs to the panel bridge, so its helper vtable is copied and
  `get_modes` chained rather than replaced.
- `drm_plane_enable_fb_damage_clips()` was never called, so every update
  reported full-surface damage and the per-scanline flush degenerated to the
  whole buffer. Now enabled.


### RGB565 end to end: as far as it can go - closed

The scanout buffer, the DRM format (`DRM_FORMAT_RGB565` is the only one the
driver advertises) and weston's own output buffer (`gbm-format=rgb565`) are all
565. That was the original fight and it is won. The one remaining 32-bit buffer
is the **client surface**, and it cannot be removed by configuration.

`weston-terminal` allocates ARGB8888/XRGB8888, so every composite converts down
to 565. It cannot be forced off, for two independent reasons:

- **libwayland hardcodes it.** `bind_shm()` sends `WL_SHM_FORMAT_ARGB8888` and
  `WL_SHM_FORMAT_XRGB8888` to every client unconditionally, before any
  compositor-added formats. A compositor cannot withdraw them. The protocol
  says so too: *"All renderers should support argb8888 and xrgb8888."*
- **The toytoolkit hardcodes it.** `clients/window.c` uses
  `CAIRO_FORMAT_ARGB32` and asks for XRGB8888/ARGB8888 without ever querying
  what else is advertised - and weston's pixman renderer *does* advertise
  RGB565.

So it is patch-only. What that costs, measured on the board with `pixbench`
(pixman directly, nothing patched), ms per composite:

        surface        8888->565   565->565   penalty
        800x480 SRC       61.67      16.63      3.7x
        800x480 OVER     108.56      16.74      6.5x
        640x384 OVER      71.19      10.73      6.6x
        400x240 SRC       15.35       4.40       11 ms
        400x240 OVER      27.13       4.43       23 ms

Two readings, and the second matters more:

1. **At today's latency it is noise** - 11-23 ms against a 2586 ms keystroke.
   It is not why the desktop is slow.
2. **It is a permanent ceiling on smoothness.** An OVER composite at 800x480
   costs 108 ms, which is 4.5 frame times at the panel's 42 Hz. Even with
   memory pressure solved, 800x480 can never hit frame rate while clients hand
   over 8888. At 400x240 it is 27 ms against a 24 ms budget. This is an
   argument for a reduced render size that is independent of the memory story.

### Colour depth below 565 is not a lever - closed

Checked against Weston 15's own `libweston/pixel-formats.c` rather than
assumed. The only formats its pixman renderer can render into are **RGB565 at
16 bpp** and a set of 32 bpp variants. There is nothing between, and nothing
below. Weston states the rule in a comment: *"Indexed/greyscale formats, and
formats not containing complete colour channels, are not supported."*

- We are **already** at the floor: `gbm-format=rgb565`, and the driver
  advertises only `DRM_FORMAT_RGB565`.
- The hardware could go lower - the PPA converts GRAY8 (8 bpp) or YUV420
  (12 bpp) to RGB565 on the fly - but Weston cannot produce either without
  being patched.
- `XRGB4444` is in Weston's format table but has no pixman mapping, and is
  still 16 bits in memory, so it would save nothing even if it worked.
- RGB888 is 24 bpp: more traffic, not less. Not a reduction lever.

This also **kills phase 3 of the plan**, which was written as "32 -> 16 bit
colour". Weston's own framebuffers were already 16-bit. The 32-bit buffers are
the *client* `wl_shm` ones: `weston-terminal` draws through cairo, which needs
ARGB32, and `wl_shm` mandates ARGB8888/XRGB8888. That is the client's choice,
not a compositor setting, so there is nothing to turn down.

### Where the remaining latency actually goes

MemTotal is **12804 kB**. The breakdown at rest, weston + terminal running:

        Slab            ~4400-4800 kB   (SUnreclaim; SReclaimable is 0)
        Cached           ~2800 kB
        AnonPages        ~1550 kB
        MemAvailable     ~1200-1460 kB

Slab is a third of all RAM and is not reclaimable. `/proc/slabinfo` needs
`CONFIG_SLUB_TINY=n` + `CONFIG_SLUB_DEBUG=y` - the production kernel has
SLUB_TINY, which disables both slabinfo and `/sys/kernel/slab`. With a
throwaway diagnostic kernel the top caches are `kernfs_node_cache` 748 kB
(8602 objects), `kmalloc-64` 368 kB, `inode_cache` 364 kB, `kmalloc-1k`
328 kB, `dentry` 256 kB. The kernfs nodes are real devices in sysfs, so
shrinking that means removing drivers.

**XIP works, and it is verified, not assumed.** `rootfs/xipmap.c` mmaps a file
and prints the kernel's own accounting for that VMA:

        libpixman (385 kB) from cramfs XIP    Rss    16 kB   VmFlags ... mm
        libpixman (385 kB) from SD/ext4       Rss   388 kB   VmFlags ... (none)

`mm` is VM_MIXEDMAP - pages mapped straight out of the flash window. They are
not page cache, never age and never refault. **overlayfs preserves this**: the
same library through `/usr/lib` on the overlay maps at 16 kB with `mm`, while a
library present only on the SD lower layer maps at full size without it.

**The library side is done.** With the overlay mounted, weston has *zero* kB of
file-backed mapping costing RAM. weston-terminal has 1672 kB, and only 108 kB
of that is its own binary - the other **1488 kB is `/memfd:weston-shared`**, the
wl_shm client buffers.

**That is the whole remaining problem.** 427 major faults per keystroke x 4 kB
is ~1.7 MB, which is exactly the terminal's in-RAM footprint: its entire
working set is evicted and reloaded on every keystroke. And those buffers are
twice the size they need to be, because clients must use ARGB8888 - see the
RGB565 section. They are also larger than the screen, because weston-terminal
sizes its window in pixels and does not fit in 400x240.

**Set up the overlay, not just LD_LIBRARY_PATH.** `LD_LIBRARY_PATH=/mnt/xip/usr/lib`
misses libc entirely - the interpreter path is baked into every binary - and
misses anything dlopened by absolute path, like `libweston-15/drm-backend.so`.
Measured at 400x240: LD_LIBRARY_PATH only, median 2586 ms; full overlay, median
2237 ms with trials trending to 1123 ms as the working set warms.

        mkdir -p /mnt/xip /mnt/sdlib
        mount -t cramfs mtd:rootfs /mnt/xip
        mount --bind /usr/lib /mnt/sdlib
        mount -t overlay overlay -o lowerdir=/mnt/xip/usr/lib:/mnt/sdlib /usr/lib

### The client was the problem, not the stack

`weston-terminal` is a **toytoolkit demo**, and measuring it was measuring the
wrong thing. Replacing it with `foot` - an off-the-shelf, Wayland-native
terminal that does real damage tracking - changes the result by an order of
magnitude. Matched runs, one client each, fresh boot, native 800x480:

        client            median     min      max
        foot              193.8 ms    98.2     316
        weston-terminal  7827.1 ms  6961.0    8338

**40x on the median**, and foot's last three trials were 102, 109 and 98 ms -
it settles around 100 ms once warm, which is inside the ~150 ms usability
threshold. At **native 800x480**, with no render-downscale. foot also idles at a fraction of the footprint:
RssShmem 0-180 kB against weston-terminal's 1488 kB, and 0 faults on a warm
keystroke.

This overturns the previous section's conclusion. The Wayland client-side
rendering model is **not** the ceiling; `weston-terminal` redrawing its whole
surface through cairo/pango on every keystroke was. Note weston-terminal's
*min* of 172 ms - it can be fast when warm, it just almost never is.

Consequences:

- **Option C (switching to X11) is not needed.** It was proposed on the premise
  that Wayland had no route to acceptable latency. That premise was wrong.
- **The objective is essentially met at native resolution**: ~100 ms warm,
  194 ms median, off-the-shelf, no customisation, no downscale.
- Most of the earlier variance was **measurement contamination**, not the
  system. See below.

`BR2_PACKAGE_FOOT` and `BR2_PACKAGE_DEJAVU`/`_MONO` are now in the rootfs
defconfig. The image had **no fonts at all** before - weston-terminal was
falling back to a cairo builtin, and foot refuses to start without a real one.

**Use the SD imager to deploy rootfs changes** (`docs/sd-imager.md`), not
ad-hoc file copies. Hand-carrying binaries misses everything the package
would have installed - fonts, in this case. Note the imager rewrites the whole
card, so anything deployed ad-hoc is destroyed: `inputlat` and `fbdump` were
lost and had to be rebuilt. They are not part of any package and should be.

### weston.ini is now tracked - it was silently changing the experiment

`weston.ini` existed only as a hand-edit on the SD card. Re-imaging replaced it
with buildroot's default, which does two damaging things:

- pins `[output] mode=800x480`, overriding the driver's preferred scaled mode;
- leaves the desktop shell's **panel** enabled, whose launcher starts a
  **second terminal**.

So runs that were supposed to compare one client at 400x240 were actually
measuring two clients at 800x480, on a machine with ~1 MB free. That is where
the wild variance came from: foot measured 141-1193 ms contaminated, and
98-316 ms clean. It also made the render-downscale path look like a large
regression (medians of 6-8 s) when the second client was the cause.

The file now lives in `buildroot-external/board/esp32-s31/overlay/etc/xdg/weston/`
with `panel-position=none`, no `mode=` pin, and `gbm-format=rgb565`.

**Damage-aware scaling.** `esp32s31_ppa_scale_rect()` scales a sub-rectangle,
so the driver honours the client's damage instead of rescaling the whole frame
on every flip - otherwise a one-glyph keystroke costs a full-surface SRM pass
and throws away exactly the property that makes a good client fast. Verified
visually: a clean 2x upscale with no stale regions.

The same call places a smaller image inside the scanout buffer without
stretching it, which is how a mode whose aspect ratio does not match the panel
gets **pillarboxed** - e.g. 640x480 copied 1:1 to destination x=80, with the
two 80-px bars filled once at modeset rather than every frame.

### What the remaining time is NOT

Three hypotheses were tested and killed, which is worth more than the one that
survived:

- **Not SD I/O.** System-wide `iowait` during a keystroke is **10-30 ms** out of
  ~3900 ms. The ~430 major faults per keystroke are being served from swap
  *cache* (`SwapCached` ~1 MB), not from `/swapfile`. The known ~10 ms per SD
  request is not in this path at all.
- **Not swap readahead.** `vm.page-cluster` swept 0/3/5 with the default (3,
  8 pages) repeated to detect drift: 0 gives 1090 faults and a 4354 ms median,
  5 gives 404 faults and 3867 ms, 3 gives 327-452 faults and 2985-3133 ms. The
  default is already the optimum; turning readahead off costs ~1.4 s.
- **Not the compositor.** Weston uses ~50 ms of CPU per keystroke and has zero
  kB of file-backed mapping in RAM. weston-terminal uses ~1510 ms.

**`/proc/stat` is unusable here - confirmed quantitatively.** Over a 4154 ms
wall window on a single hart it reported user+system+idle+iowait = 11480 ms,
a 2.7x over-count. Do not quote CPU percentages from it; see
[[s31-cpu-measurement]]. The reliable substitute is displacement: run a
competing CPU hog and watch wall-clock latency.

Doing that: baseline median 4160 ms, with one hog 5797 ms, baseline again
3322 ms (note the drift - always bracket). Against the mean baseline of
~3740 ms that is 1.55x. If a fraction f of the latency is CPU work that has to
share with the hog, the ratio is 1+f, so **~55% of a keystroke is CPU-bound and
~45% is not**.

**So the dominant cost is the client redrawing itself**, not compositing, not
paging, not I/O. weston-terminal redraws its surface through cairo/pango on
every keystroke, and its window is *larger than the screen* at 400x240, so it
is drawing more pixels than are displayed. That is why the old font-size
experiment was so effective - it shrank the window, i.e. the client's drawing
area - and why cutting the output resolution helped less than the plan
predicted: a clipped window still draws at full size.

### Diagnostics take the scanout address from the driver

`/sys/kernel/debug/esp32s31_lcd/updates` now ends with
`scanout=0x... size=...`, and `fbdump` and `inputlat` read it when given no
address. This is not a convenience: the buffer is **not at a fixed place**.
Without scaling it follows the compositor's page flips; with scaling it is a
CMA allocation that moves with the memory map. The old hardcoded 0x50c00000 /
0x50d00000 now read unrelated memory, which presents as a corrupted frame or
as "every trial timed out" rather than as an obvious mistake - both of which
happened during this work.

`scanout=0x00000000` is normal before the first page flip: the address is
published on enable, so reading it the instant weston says "enabled with head"
is too early. The tools distinguish that case ("nothing is scanning out yet")
from a missing debugfs mount.

All three of `fbdump`, `inputlat` and `pixbench` are now built by the
`s31-tools` package. They were previously deployed by hand onto the card, so
re-imaging destroyed them - which it did, in the middle of a measurement.

### Three cheap RAM wins: tracing, the log buffer, and the ext4 block size

Measured, cumulative, on the running board:

                          MemTotal   Slab   MemAvailable
        starting point     14888    4348      ~4864 kB
        after 1 and 2      15516    3792       6768 kB
        after 3            15516    3728       6796 kB

**~1.25 MB**, from three lines and one reformat. None of it is slab pruning in
the sense of removing drivers.

1. **`FTRACE`, `ENABLE_DEFAULT_TRACERS` and `BLK_DEV_IO_TRACE` off.** These
   were enabled for the latency investigation. ~577 ftrace events register
   eagerly at boot at ~304 B of slab each, and their metadata
   (`print_fmt_*`, `trace_event_fields_*`, `event_class_*`) is another ~394 KiB
   of `.data`, which is RAM even though the kernel is XIP. `BLK_DEV_IO_TRACE`
   is what selects `GENERIC_TRACER`, so all three must go together - disabling
   only one leaves the tracer alive. The kernel image shrank by **1,056,072
   bytes**. Re-enable all three together if ftrace is needed again.
2. **`LOG_BUF_SHIFT` 16 -> 14.** `__log_buf` is 64 KiB of `.bss` and the static
   printk ringbuffer descriptors are ~200 KiB of `.data`; 264 KiB of a 15 MiB
   machine spent on dmesg history.
3. **`mkfs.ext4 -b 4096` for the rootfs image**
   (`BR2_TARGET_ROOTFS_EXT2_MKFS_OPTIONS`). Block group size is
   8 blocks/group * blocksize, so 1 KiB blocks gave 8 MiB groups - **952 of
   them** once growroot expanded onto the card, each costing an
   `ext4_groupinfo_1k`. At 4 KiB it is 128 MiB groups, **61 of them**.
   `resize2fs` preserves the block size, so the growroot path needed no change,
   and it is now a property of the built image rather than of whoever formatted
   the card. Observed saving 64 KiB rather than the predicted 105 KiB, because
   group info is allocated lazily as groups are touched.

**Re-imaging no longer destroys the tooling.** `foot`, `inputlat`, `fbdump`,
`pixbench` and `weston.ini` all came back from the image, because they are in
the buildroot package and overlay rather than hand-copied onto the card.

Not done, and still available if memory gets tight: `DEBUG_FS=n` (~315 KiB, but
this project leans on debugfs - a production-image flag), and patching
`MAX_NR_CONSOLES` 63 -> 2 (~126 KiB; all 63 VTs register eagerly, which is why
`/sys/dev` has 128 entries). Note also that `SReclaimable` reads 0 while
`dentry` and `inode_cache` are created `SLAB_RECLAIM_ACCOUNT`, so ~620 KiB of
the reported slab should in fact be reclaimable under pressure.

### fbcon is back on, and its 752 KiB idle cost is accepted

`DRM_FBDEV_EMULATION` is enabled again. It was dropped when fbcon held the only
slot in a 2 MiB coherent pool; that constraint died with the move to CMA. The
driver already called `drm_fbdev_dma_setup()` and already installed the
`.dirty` callback deferred I/O needs, so it was only the config.

What it gives: the kernel log and a login prompt on the panel from boot, and
**the console comes back when a compositor exits** - `drm_fb_helper` restores it
on last-close, verified by `scanout` returning to the fbdev buffer and the
console redrawing. No work needed for that.

What it costs: **752 KiB of CMA**, held whether or not anything is on screen.
Measured on a clean boot: `CmaFree` 4096 -> 3344, `fb0` 800x480 at 16 bpp.
There is **no second shadow buffer** - `drm_fbdev_dma` with deferred I/O uses a
vmap of the same DMA object as `screen_buffer`.

**Decision: accept it.** The alternatives were examined and are worse:

- **A smaller fbcon mode via `video=DPI-1:400x240` costs *more*, not less.**
  Today `scanout` *is* the fbdev buffer - fbcon is scanned out directly,
  zero-copy. Any reduced mode forces the PPA path, which needs a full-size
  destination: 192 KiB fbdev + 750 KiB scanout = 942 KiB, ~190 KiB worse.
  The current arrangement is already optimal.
- **Unbinding fbcon does not free it.** `echo 0 > /sys/class/vtconsole/vtcon1/bind`
  switches the console to the dummy device and returns ~20 KiB of fbcon's own
  structures; `CmaFree` does not move and `/dev/fb0` remains. There is no sysfs
  to unregister a DRM fbdev, and `CONFIG_MODULES=n` rules out unload tricks.
- Releasing the buffer on master-set and reallocating on last-close is the only
  thing that would actually do it, and DRM exposes no driver API for that - it
  would mean driving `drm_fb_helper` internals from our driver.

**Worth doing if the desktop is ever run downscaled:** in the scaled path the
driver allocates its own native-sized scanout buffer while fbdev already owns
an identical one that is idle (fbcon is suspended whenever a master holds the
device). Those could be the same allocation - **750 KiB back** in any reduced
resolution configuration, and nothing lost at native.

### The framebuffer region is CMA, which bought 2 MB of system RAM

`lcd_reserved` used to be a `shared-dma-pool` with `no-map`, which routes it to
the reserved-memory **coherent** allocator. That allocator is
`bitmap_find_free_region(mem->bitmap, mem->size, order)` - **power-of-two page
orders only**. The consequences were absurd rather than merely wasteful:

        buffer            bytes    pages needed   pages taken   waste
        640x384 client   491520        120         128 (ord 7)    6%
        640x480 client   614400        150         256 (ord 8)   41%
        800x480 scanout  768000        188         256 (ord 8)   36%

Two 640x480 clients therefore claimed the whole 2 MiB region and the scanout
buffer could not be allocated at all - the driver fell back to driving the
panel at the client's timing and the picture was garbage. 640x480 cost twice
640x384 for 25% more pixels.

**Now `reusable` instead of `no-map`**, which selects `rmem_cma_setup()` -
`rmem_dma_setup()` rejects `reusable` outright, so the property is the whole
switch. CMA allocates by page count, so nothing rounds away, *and* the region
counts towards MemTotal with the kernel free to put movable pages in whatever
the display is not using.

        Reserved memory: created CMA memory pool at 0x50800000, size 4 MiB

                        before      after
        MemTotal       12844 kB   14888 kB
        MemAvailable    ~1200 kB    5764 kB   (idle, before weston)
        CmaFree              -      4096 kB

**And that was the remaining bottleneck.** Memory pressure, not pixels, not
I/O. With the same kernel, same client, same clean weston.ini, at native
800x480:

        foot, before CMA   median 193.8 ms   min 98.2
        foot, after CMA    median  20.7 ms   min  9.8   (10 trials, 9.8-33.7)

**Null test.** Every trial being under a frame is exactly the shape a broken
harness produces, so it was checked: with the client killed, `inputlat` reports
17-20 s - nothing changes the framebuffer, as it should. One spurious 10.1 ms
appeared immediately after `killall`, which is the compositor repainting the
window away, not a measurement artefact.

`inputlat` measures **input to framebuffer-in-memory**, not input to photons.
The panel shows the result up to one frame later, so end to end is roughly
21 + <=24 = **30-45 ms**.

**Constraints that fixed the geometry.** CMA needs base *and size* aligned to
`PAGE_SIZE * pageblock_nr_pages`; with no huge pages `pageblock_order =
MAX_PAGE_ORDER`, so 4 MiB. 0x50800000 is the highest 4 MiB-aligned base that
still clears `audio_reserved` (0x50fe0000) and `opensbi_reserved` (0x50ff0000),
so **nothing had to be relocated** - and the old 2 MiB `no-map` hole at
0x50c00000 went back to the system, which is where the +2 MB comes from.

`CONFIG_CMA_SIZE_MBYTES` is set to 0: the default global CMA area is unused
here and otherwise logs "cma: Failed to reserve 16 MiB".

**The scanout buffer address moved** (0x50a00000 at the time of writing, and it
is allocated, not fixed). Diagnostics that hardcode 0x50c00000/0x50d00000 -
`inputlat`'s FB_BASE, `fbdump` invocations - now read the wrong memory. Take
the address from `dmesg | grep "scaling: scanout buffer"`.

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
