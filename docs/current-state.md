# Where this work stands

Read this first after a context reset. It records what is true of the board
right now, what is in flight, and — most importantly — what has already been
tried and failed, so it is not tried again.

## Where the desktop stands (2026-08-23)

**Xorg + modesetting, jwm, st, xcalc, xfiles.** Weston, foot, X11Libre's Xfbdev
and cairo have been removed from the build entirely. Everything below the next
`---` describes the *current* stack; the Weston material further down is kept
because its measurements and dead ends are still instructive, but it is history.

The server talks to `/dev/dri/card0` directly. That matters for one specific
reason: Xfbdev reached the panel through DRM's fbdev emulation, whose deferred
I/O timer fires at `HZ/20` and caps **any** fbdev client at 20 fps regardless of
what sits above it. That ceiling is gone.

What the display path actually costs, measured with `xfill` (one long-lived
connection doing nothing but full-root damage, XSync per fill - so no process
startup is included):

    bare server           4.6 ms median   211 fps sustained
    + jwm + st            9.2 ms median    max 58 ms
    + xcalc + xfiles     32.1 ms median    max 1174.9 ms   <- the problem
    after idling          4.6 ms median   190 fps (clients page out)

**The graphics stack is not the bottleneck.** The server repaints the whole
screen 211 times a second. Two things dominate instead:

1. **Process startup: 330 ms for one `xsetroot`** - about 50x the cost of
   drawing. Launching is the slow part of this desktop, not rendering.
2. **The swap tail.** With four clients open ~7-9 MB is in swap and a single
   repaint could exceed a second. zram now absorbs it: max 1174.9 -> 65.5 ms.
   See `etc/s31-swap.conf` for the table and the trade it costs.

Memory, measured on the board rather than assumed:

    Xorg, no clients      3,092 kB anon   (Xfbdev was 2,876 kB - a wash)
    + jwm                   +448 kB
    + st                    +660 kB
    unreclaimable slab     4,352 kB       <- 28% of RAM, largest single item
    of which kernfs_node     764 kB (8,878 objects), inode 364, dentry 272

XIP is working exactly as intended: `/usr/bin/Xorg` maps 1,832 kB at **Rss 0**,
as do libc, libfreetype and libpixman. Binaries in flash cost no RAM at all.

### Later the same day: the mouse

Three changes, in the order they had to happen:

1. **The native mode killed a pointless PPA pass.** Xorg picked the driver's
   reduced 640x384 mode, which made `esp32s31_lcd_scaling()` true, so every
   commit went through a PPA copy that was not scaling anything (640x384 lands
   1:1 centred at +80+48). Driver cost 12.8-15.9% -> 0.1-0.6% of a core.
2. **The PPA was sleeping, not working.** 6.2 ms per operation was
   `wait_for_completion()` parking the caller on a loaded machine. Spinning
   300 us first: 14,527 us -> 276 us under load. The engine actually sustains
   ~100 MB/s against the CPU's 22.6 MB/s, so it is worth using above ~7.3 kB
   per operation - below that its ~250 us of setup dominates.
3. **A DRM cursor plane, composited in the driver.** X's software cursor cost a
   *fixed* ~19 ms per pointer move - 11 ms user, 8 ms system - and it was fixed,
   not proportional: dropping the screen 36% moved it 11 ms -> 10 ms. Pointer
   motion went 17.3-25.7 fps -> 43.1-45.2 fps. Then the commit tail was made to
   skip `drm_atomic_helper_wait_for_vblanks()` for cursor-only commits, which
   had been pinning cursor updates to the 42 Hz frame rate and backing up X's
   input queue: 540/532/630 moves per 500 injected events, i.e. one-for-one.

LCD_CAM scans out one linear buffer and cannot overlay a second, so the cursor
is still composited in software - just in the driver, on ~2 KB, instead of in X
across the whole damage pipeline. It costs a private 768 KB scanout buffer,
because X writes its framebuffer from userspace and only *then* calls DirtyFB,
so the driver can never lift the cursor out first and save-under would go stale.

### And then memory, again

With the desktop up, `xcalc` sat at **8 kB resident** and everything else was
swapped. Clicks and hovers were slow because a click had to fault the whole
client back in - nothing to do with drawing. Two reversals fixed it:

- **zram off.** It was enabled to kill a 1174.9 ms repaint tail and did (65.5
  ms). That tail is now 44.5 ms *without* it, because the cursor plane and the
  mode change removed the work causing it - so zram was charging 2.8-3.2 MB of
  RAM, several times MemAvailable, for nothing. See `etc/s31-swap.conf`.
- **640x384.** The reasons for the native mode were both gone (the PPA is cheap,
  the cursor is the driver's), and it costs 553 kB more of framebuffer plus 36%
  more drawing. At 640x384 every client is fully resident: st swap 0, xcalc
  swap 0. See `etc/X11/xorg.conf`.

Both are recorded with their tables in those files, including what they cost.

Things measured and rejected, so they are not retried:

- **Reclaim in `.text.fast`.** kswapd0 was the largest consumer after X
  (12.7-18%), so vmscan/rmap/workingset/swap/page_io/swap_state and the lzo
  codec were relocated to RAM, 33 kB of it. 17.1 vs 17.6 kswapd ticks/s - no
  difference. Reclaim is bound by data, not instruction fetch; the 5.98x
  flash-vs-RAM figure only applies to code that refetches itself.
- **zram compaction.** Freed 0 kB. zsmalloc's 2.5x packing overhead is
  size-class granularity plus incompressible pages, not fragmentation.
- **Running Xorg from the SD instead of XIP flash.** 4x *worse* (4.7 fps): the
  1.9 MB of page cache it needs costs more than any instruction-fetch gain.

- **Pruning X extensions** (DRI2/DRI3/Present/XVideo/RECORD/DGA/VidMode/
  X-Resource/DBE). X anon 6,040 -> 6,028 kB. Noise. The 6 MB is heap, not
  extension code.
- **`vm.vfs_cache_pressure=500`** to make the kernel drop metadata instead of
  swapping X. Made it worse and wedged the board - with root on a slow card,
  every dropped dentry has to be re-read.
- **Investigating slab further** needs `CONFIG_SLUB_DEBUG`, which is mutually
  exclusive with the `CONFIG_SLUB_TINY` this board ships, so the diagnostic
  changes the allocator being diagnosed. A throwaway build was used once to get
  the breakdown above and then reverted.

Worth knowing: `SReclaimable` reads **0** under `SLUB_TINY` even with ~1.3 MB of
dentry/inode cache present, so `MemAvailable` understates what is really
reclaimable here.

---

## Historical: the Weston era

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

**CORRECTED 2026-08-22: there IS a shadow, and it was the largest consumer.**
This section previously claimed "there is no shadow: Weston's pixman renderer
draws directly into DRM dumb buffers". Weston's own log says
`DRM: output DPI-1 uses shadow framebuffer.`, and the measurement agrees:
shrinking the render size drops Weston's *anonymous* footprint from 2096 kB to
688 kB, which dumb buffers in `lcd_reserved` cannot explain because that region
is excluded from MemTotal. The dumb-buffer half of the old claim stands; the
"no shadow" half was wrong and load-bearing, so anything derived from it should
be re-checked.

The rest of the original point survives on its own evidence: CPU is not the
constraint - 0.4 s of a 13.7 s keystroke - so a hardware blend path is not what
fixes this. CPU is not the constraint
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

## USB: a hub used to cost 39% of the core

Fixed 2026-08-21. The board has one USB port, so a hub is the normal way to
attach a keyboard and a mouse at once, and until now that was ruinous.

    both arms on one boot, same hub, same two HID receivers, no compositor

                 dwc2 irq/s   CoreMark        descriptor DMA
    high speed         8541   599.0 / 601.1   off
    full speed         1040   981.9 / 980.0   on

CoreMark idle on this board is ~986, so the controller went from costing about
39% of the core to roughly 0.5%.

**Why a hub was expensive.** A full-speed device behind a HIGH-speed hub needs
split transactions, and this core cannot schedule those in descriptor DMA mode:
`dwc2_hcd_qh_init_ddma()` rejects every `do_split` QH and `dwc2_hcd_qh_create()`
then fails the URB. A hub therefore knocks the whole periodic schedule off the
hardware scheduler and onto software driven by the SOF interrupt - and because
the root port is now high speed, SOF fires once per *microframe*, 8 kHz rather
than 1 kHz. Both penalties arrive together, which is why the number is so large.

**The fix** is `host_full_speed` in `dwc2_set_esp32s31_params()`, default on.
Pinning the port to full speed makes the hub a plain full-speed repeater, so
nothing behind it needs a split, `do_split` is false everywhere, and
`dma_desc_fs_enable` puts descriptor DMA back on. The price is a 12 Mbit/s
ceiling for the entire bus - irrelevant for HID, and the reason this is a
runtime knob rather than a hard-coded choice:

    /sys/module/dwc2/parameters/host_full_speed     write 0 or 1
    /sys/bus/platform/drivers/dwc2/20300000.usb     unbind, then bind

**Measurement traps hit here.** A first A/B run with Weston up gave 175 vs 388
CoreMark - right direction, wrong magnitude, because memory pressure dominated.
Measure USB cost with no compositor running. Unbinding `usbhid` isolates HID
cost without unplugging anything, and Weston re-attaches to the renumbered
event nodes by itself.

**Still unexplained:** a Logitech receiver behind the hub spontaneously
disconnected once, 99 s after enumerating, with no transfer errors before it.
It has not recurred since the change. The hub is bus-powered and declares
`bMaxPower = 100mA` while feeding two receivers that each declare 100 mA, which
is an untested suspect.

## Where SD and swap stand (2026-08-22)

Measured with `sdlat` (verified O_DIRECT, random offsets, percentiles) rather
than `dd`, which does not honour `iflag=direct` here and produced numbers
implying 70 MB/s on a 20 MB/s bus.

    4k random read, p50      3.78 ms  ->  2.84 ms      new card + HZ=100
    10 MB swap-in           12.91 s   -> 10.43 s
    CoreMark                    unchanged, 959-988 -> 964-971

Two changes did it: a different microSD (14%, and it is bigger, though its tail
is worse - p99 182 ms at 32k against 5.8) and dropping the scheduler tick from
250 Hz to 100 Hz (12%, free, since HIGH_RES_TIMERS provides precision and HZ
only sets the tick).

**The remaining cost is not the controller and not the card.** Of a 2.86 ms
request, only **0.43 ms is inside dw_mmc**. A command with no data payload
(`cmdlat`, CMD13) completes in 0.845 ms through the whole ioctl path. So ~2.4 ms
per request is spent above the driver - in blk-mq, the mmc core, or waking the
submitter - which is what the note at the top of dw_mmc.c said before any of
this work started.

Eliminated by measurement, on top of the eight recorded previously: the card
(second card, 14%), Linux interrupt delivery (a 0.5 ms MINTSTS poll does not
see completions earlier), card clock gating (CLKENA bit 16, 3.781 vs 3.778 ms),
the BIU and CIU clocks (80 and 40 MHz, both correct), interrupt loss (3.0 per
request), edge-vs-level triggering (fixed because it was wrong, worth 0 ms), and
CONFIG_PREEMPT (no change to SD, and it cost 3-5% of CoreMark).

**And beware the instrument.** The "2 ms command phase" reported during this
investigation was an artifact of the driver's own timestamps: `t_data` was
stamped before `t_cmd` when both completions arrived in one interrupt, so those
requests were dropped from one accumulator and charged whole to another. Fixed;
`issue_to_cmd` is 0.25 ms.

**Swap is no longer on the interactive path at all.** With `render=640x384`,
pointer motion writes zero bytes to swap; the remaining swap traffic is app
startup and switching. `vm.page-cluster` stays at 0 and zram stays off - both
were measured against the real workload and rejected, see
`/etc/sysctl.d/99-s31-memory.conf`.

## SD reads are CPU-bound, not I/O-bound

Profiled 2026-08-22 with `profile=6`, 2000 random 4k O_DIRECT reads, resolved
against System.map. 586 samples at 100 Hz is 5.9 s against 5.58 s of wall clock
for the run, so **the CPU is essentially never idle during SD reads**. It is not
waiting for the card; it is executing.

    (before relocating the hot path to RAM)
    workqueue / completion       18.9%
    scheduler / context switch   15.4%
    mmc core / block layer       12.6%
    uart / console                6.7%   <- measurement contamination, see below
    user copy / gup               4.3%
    cache / DMA                   2.6%
    dw_mmc driver                 0.0%
    unattributed                 37.4%

**dw_mmc is 0.0%**, which independently confirms the driver's own timing: only
0.43 ms of a 2.84 ms request is inside it. Every fix aimed at the controller or
the card was aimed at a seventh of the problem, which is why the card swap and
the interrupt-type fix bought so little.

The biggest identifiable target looked like the **workqueue hop**:
`__queue_work` alone is 8.0%, and dw_mmc's interrupt handler does
`queue_work(system_bh_wq, &host->bh_work)` on every completion before the mmc
core sees it - up to three times per request.

**Tried and it does nothing.** Guarding those calls with `work_pending()`, so
the second and third reach only a bit test instead of walking into
`__queue_work`, changed the result not at all: 2.793 / 2.812 / 2.804 ms against
a baseline of 2.789 / 2.747 / 2.843. Reverted rather than carry a divergence
from upstream for no measured benefit. What that tells us is that the 8% is the
first, unavoidable queue per request, not redundant repeats - so removing it
means not deferring at all, and the bottom half calls into the mmc core where
that is not safe. `finish_task_switch` at 12.3% should be read with the usual
caveat - it is where the CPU lands after any switch - but with the CPU ~100%
busy these are real switches rather than returns from idle.

**The profiler cannot see .text.fast.** `do_profile_hits()` computes
`pc = (addr - _stext) >> prof_shift` and drops anything with `pc >= prof_len`,
where `prof_len` covers only `_stext.._etext`. Relocated code lives after
`_etext`, so every sample in it is discarded. A re-profile after moving the hot
path showed 286 samples against 586 before and 0.0% in `.text.fast` - that is
not a halving of CPU time, it is the profiler no longer counting what moved.
Confirm with `/proc/stat` instead, which is independent: `idle` is still 0
during a 2000-request run, so the path is still CPU-bound, just cheaper.

Two traps when repeating this:

  - `CONFIG_PROFILING` is now on, but the profiling buffer is only allocated
    when `profile=` is on the kernel command line, which is compiled in
    (`CMDLINE_FORCE`). So enabling it needs a rebuild, and leaving it enabled
    costs 253 KB. Add `profile=6` to CMDLINE in the Makefile, and take it out
    afterwards.
  - `/proc/profile`'s first word is the step in **bytes** (64 for `profile=6`),
    not a shift. Treating it as a shift puts every sample in one symbol and
    looks like a smoking gun. Bucket i maps to `_stext + i * step`.
  - The serial console shows up at 6.7% because the harness drives the board
    over it. Discount it, or profile a run that produces no console output.

## Render size is the memory lever, and it stops the swapping

Measured 2026-08-22, warm, weston + desktop-shell + foot, a reboot between arms.
`esp32s31_lcd.render=WxH` renders below the panel and upscales with the PPA, so
every client's buffers and Weston's shadow shrink without any client knowing or
the desktop getting visibly smaller.

                MemAvailable  weston VmRSS  cursor runs 1-3   swap written
    800x480       1148 kB       2884 kB     10.5 13.6 18.0    -768 -768 0 kB
    640x384       2724 kB        768 kB     16.7 17.8 17.8       0    0  0
    400x240       3168 kB        796 kB     18.1 18.7 18.2       0    0 +256

**640x384 was the default until 2026-08-27; native 800x480 is now.** The table
above is Weston's, and Weston is gone - the desktop is lvdesk, which holds no
shadow buffer at all (it renders straight into the dumb buffer with
`LV_DISPLAY_RENDER_MODE_DIRECT`), so the memory argument that chose 640x384 no
longer applies to the client that exists. What kept native out afterwards was
three bugs, all now fixed: see `docs/native-800x480.md`. 640x384 remains
available at runtime via `render=`, and the numbers here still stand for any
client that does keep a full-size shadow.

The original reasoning, for that case: 640x384 is exactly 1.25x in both axes, so
it fills the panel with no pillarboxing and no aspect distortion, and it takes
the whole win - 400x240 shrinks Weston no further and costs a soft 2x upscale.

Two things worth reading off that table. Native **swaps during ordinary pointer
motion** and needs three runs to reach full speed, which is the "takes a while
to become responsive" behaviour reported from the board; the reduced modes are
at full speed immediately and never touch swap. And Weston's anonymous memory
falling 2096 -> 688 kB is the shadow framebuffer, not client buffers - see the
correction under PPA below.

## The damage copy is done inline, not deferred (2026-08-27)

**DIRTYFB was more than twice as expensive as it needed to be**, and the cost
was invisible because it was charged to the *next* commit.

The driver used to hand the damage copy to a kworker so that a cursor move
would not hold the CRTC lock across it. But DIRTYFB is a **synchronous** atomic
commit, so a client that damages every frame never escapes the copy - it pays
on the following commit, where `copy_sync()` calls `flush_work()` and blocks
until the kworker runs. That appears as fixed per-commit overhead rather than
as pixel cost, which is exactly why it looked like DRM machinery.

Measured with `lvdesk/dirtybench.c` (times the ioctl directly, no debugfs), 50
reps, median:

                          deferred   inline
     single 64x16          3.66 ms   1.62 ms
     2 opposite corners    3.80 ms   1.71 ms
     4 scattered           3.99 ms   1.86 ms

2.15-2.26x, and the tail moves with it (p90 4.10 -> 2.31 ms). The driver's own
counters account for all of it: **107 `flush_work()` waits over ~400 commits,
787 ms in total - 7.4 ms per wait**, which is what getting a kworker scheduled
costs on this board. 107 x 7.4 / 400 = 1.97 ms per commit against a measured
difference of 1.96.

The rationale was also dormant. lvdesk draws its pointer in LVGL and never
touches the DRM cursor plane - `cursor_moves` and cursor `fb_changes` both read
0 - so this was an X11-era optimisation still being paid for by a client that
cannot benefit from it. `defer_copy=` remains for a client that does drive the
cursor plane.

**`upd_ns` had been lying the whole time.** The early-out for an unchanged
plane is a `goto arm_event`, and `arm_event` ends with `t_enter =
ktime_get_ns() - t_enter` - so every skipped commit subtracted an
*uninitialised* stack value and added the result. The counter read 3.08 seconds
per commit, which is roughly uptime. GCC did not warn. Any earlier reading of
it is void.

Where the 3.66 ms actually went, once the counter was fixed: 2.50 ms inside
`pipe_update` (of which only 0.11 ms cache flush and 0.32 ms PPA/GDMA copy were
real pixel work) and ~1.0 ms in the DRM atomic machinery outside it.

## The kworker hand-off costs ~8 ms, and it is NOT the tick rate (2026-08-27)

The 2x win from doing the damage copy inline is real and reproduces. **The
explanation first offered for it was wrong**, and the correction matters more
than the guess did.

The hypothesis was that `CONFIG_PREEMPT_NONE` plus `CONFIG_HZ=100` sets the
granularity: a woken kworker cannot preempt a running task, so it waits for a
scheduling point, and a jiffy is 10 ms. The measured 7.4 ms sat neatly inside
that. Tested by building HZ=250, where a jiffy is 4 ms:

     flush_work() wait   HZ=100  7.4 ms      HZ=250  8.0 ms

Unchanged. **The wait is not tick-quantised**, so the mechanism is not the tick
rate and is currently *not established*. Recorded as an open question rather
than a story, because a plausible-but-wrong cause is what sent this the wrong
way once already.

HZ=250 is worse on everything measured here, so it was reverted:

                        HZ=100    HZ=250
     SD 4k seq p50      3.76 ms   4.00 ms
     SD 4k rand p50     3.79 ms   3.93 ms
     DIRTYFB inline     1.62 ms   2.08 ms

That also **fails to reproduce the note in `docs/hot-text-plan.md`** that HZ
250 -> 100 "made SD worse (12.07 -> 16.93 ms per request)". It does not, on
this kernel: 100 is slightly better. The Makefile has been forcing HZ=100 since
regardless, so the tree and that note had disagreed for a long time. `KHZ=250`
now switches it, so the next person can retest in one flag rather than an edit.

**The ~10 ms fixed cost per SD request is not tick-quantised either.** The p50
is 3.7-3.8 ms (card and bus) and the cost lives in the tail - p99 10-16 ms, max
to 34 ms - which did not move with HZ. Task #21 stays open, minus one theory.

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

**Hot userspace text in RAM: rejected, 2026-08-27, null result.** The idea was
the userspace analogue of `.text..fast` - move the hot part of a binary out of
XIP flash into RAM and keep most of the speed for a fraction of the RSS. It
does not pay, because **there is no speed to recover**.

Measured with `rootfs/footprintbench.c` (400 functions, 28.7 KB of text, well
past the 16 KB icache), the *same binary* staged into the XIP image and copied
to ext4, five interleaved runs each:

     XIP flash   33.5 32.9 32.7 33.0 33.6 ns/call   mean 33.14   spread  2.7%
     RAM         31.6 31.9 32.5 31.8 35.5 ns/call   mean 32.66   spread 12%

1.5% apart, with the RAM arm's own spread at 12%. Splitting hot text out would
cost ~600 kB of RSS on a board where memory is the binding constraint, and buy
nothing measurable.

Two things this corrects. First, the "XIP costs a measured 8%" figure that
motivated the idea **does not exist** - the 8% in these notes is `__queue_work`
in the dw_mmc path, unrelated. Second, the kernel's 5.98x flash-vs-RAM result
does *not* generalise to userspace: the kernel thrashes a 16 KB icache from the
timer path on every tick, while a userspace loop has the external-memory cache
mostly to itself.

It is also now a small target. `/mnt/xip/usr/bin` holds **lvdesk and opkg** and
nothing else - the X stack that made userspace XIP worth 572 kB is gone, so the
only binary this could apply to is lvdesk.

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

### USB unplug: the host pull-downs were the whole problem

**Symptom:** unplugging any device produced a 2-3 s burst of `-71`/XactErr, and
USB was then dead until reboot - replugging anything, including the same
device, produced nothing in dmesg at all. Plugging a mouse in could oops the
kernel in `dma_pool_alloc`.

**Cause:** `esp32s31_usb_set_pulldowns()` was *clearing* the D+/D- pull-downs.
A host port must hold both lines low through 15k pull-downs; that is what makes
"nothing attached" a defined SE0. With them off the port floats, and about half
a second after a real unplug the controller reports a **phantom connect**. The
hub cannot enumerate a device that is not there, retries, power-cycles the port
as its last recovery step, gives up, and **leaves the port switched off** -
after which nothing is ever detected again.

Every observed symptom follows from that: the error burst is the hub arguing
with a device that has gone; it happened with any device because nothing about
it is device-specific; and the replug did nothing because the port was already
unpowered by then.

**The bad note.** A note from August recorded the opposite - that asserting the
bits clamped the bus and hid devices - on a measurement showing SE0 with a
keyboard attached. **It claimed 15k swamps a device's 1.5k pull-up, which is
electrically impossible**; that ratio is a 10:1 divider and reads as a solid J.
The impossible claim was written down as fact and believed for months. The
original reading was real but confounded by other PHY init faults (suspend/PLL
routing, line-state keepalive bits, reset ordering) fixed afterwards.

Re-measured, port powered:

        empty port, pull-downs off : PRTLNSTS = J    <- floating, reads attached
        empty port, pull-downs on  : PRTLNSTS = SE0  <- correct, stable
        keyboard,   pull-downs on  : HPRT0 = 0x00021405, enumerates first time

**After the fix**, on hardware: unplug gives four `Not connected` messages over
240 ms and a clean disconnect; the port then sat empty for 70 s inventing
nothing; a different device (Logitech receiver) enumerated first time; and the
`dma_pool_alloc` oops did not recur - it was downstream of the phantom-connect
churn, not a separate DMA bug.

Also raised `bPwrOn2PwrGood` for this board from 1 (2 ms) to 50 (100 ms).
`hub_power_on_good_delay()` applies no 100 ms floor to *root* hubs, so the hub
retried 4 ms after powering a port - too soon for a device that just lost
power, which turned a last-resort recovery into a permanent failure. Belt and
braces now that the phantom connect is gone.

**The `pulldowns` sysfs attribute on the phy device makes this a 60-second
test.** Use it rather than trusting any note, including this one.

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

The display stack is **pivoting from Wayland to X11** - see
`docs/desktop-plan.md` for the reasoning and the measured budget. The short
version: the goal is real desktop applications, FLTK is the toolkit that fits,
and FLTK on Wayland needs Pango and glib (~2.4 MB) while FLTK on X11 needs
neither. X11 and Wayland are alternatives, not additions, so the comparison is
X11's cost against Wayland's, not against zero.

Measured so far:

    X11Libre Xfbdev   1.60 MB    against 3.0 MB for Xorg 21 plus modules
    X11 client libs   1.92 MB
    FLTK 1.3.7        1.28 MB    X11 backend, zero Wayland linkage
    freed by Weston  ~2.30 MB

1. **Confirm Xfbdev paints on the panel.** Everything else is downstream. It
   draws into `/dev/fb0`, which here is DRM fbdev emulation; fbcon reaches the
   plane update path that way, so an X server should, but this is untested and
   the whole pivot rests on it. Use `s31-desktop server`, which starts the
   server alone and paints a flat colour, before adding a window manager or a
   terminal. If it does not reach the plane, fall back to Xorg + modesetting,
   which talks DRM directly and costs 3.0 MB.
2. Confirm the reduced render size and 1:1 centred placement survive - the
   server must not be given an explicit `-screen`.
3. Trim the kernel (Bluetooth, IPv6, netfilter, unused drivers). The budget
   closes only just, and this is where the margin comes from.
4. Then the FLTK applications themselves: file manager, terminal, calculator.

Weston stays in the build until step 1 passes. It costs nothing on the SD
rootfs, and only the XIP flash images are budget-constrained.

`docs/weston-acceleration-plan.md` is superseded unless the pivot is reversed.


## The CMA pool did not run out of room - it ran out of migratable pages

Symptom: with the desktop up, the driver logged

	*ERROR* no scanout buffer for 800x480 (pool exhausted by the client
	buffers); scaling off

and drove the panel at the client's 640x384 timing instead. Screenshots taken
afterwards were tiled and sheared with RGB noise across the bottom third,
because `screenshot.py` assumed 800x480 and read 768,000 bytes from a
491,520-byte buffer.

**The message's diagnosis was wrong, and so was its name.** "Client buffers"
described the *old* 2 MB coherent pool, whose `bitmap_find_free_region()`
allocator rounded to power-of-two page orders and turned two 640x480 clients
into 1 MB each. That region is now 4 MB of page-granular CMA, and the clients
are nowhere near filling it.

What actually happens is that `reusable` CMA is **shared with ordinary movable
allocations**. Measured on this board:

	 CmaTotal 4096 kB, CmaFree 1356 kB   at boot, with no X running at all

2.7 MB of the region is already holding page cache and anonymous pages before
the desktop starts. Satisfying a 750 KB *contiguous* request therefore means
migrating those pages somewhere else, and on a 15 MB machine under desktop
pressure that migration fails. `dma_alloc_coherent()` returns NULL, and the
driver silently degrades. This is the flip side of the `reusable` choice
recorded in the DTS - it is what stops the region costing 4 MB of MemTotal, and
the price is that a late contiguous allocation can fail.

**Fix: take the buffer early.** It was allocated lazily at the first modeset
that wanted scaling. It is permanent either way - the allocator is idempotent
and nothing ever frees it - so allocating late only meant allocating at the
worst possible moment. It is now taken in `get_modes()`, the first point the
size is known. Probe itself is too early: `get_modes` has not run by the end of
`drm_dev_register()`, and an attempt there reported "native mode unknown".

Verified: the scanout buffer now lands at **0x50800000**, the base of the
region, rather than 0x50900000 a megabyte in - it is allocated before anything
else can take that ground - and a full desktop keeps scaled 800x480.

## Timekeeping, and why there is no RTC driver (2026-08-28)

**The board cannot keep time in hardware.** Measured, not assumed:

- **No external RTC.** An I2C scan finds two devices - the ES8389 codec at 0x10
  and touch at 0x14. No DS3231, no PCF8563, no coin cell.
- **The SoC's RTC timer does not retain.** `rtc_timer@20800000` is a real
  block, and reading it directly (write BIT(27) to +0x10 to latch, read +0x14
  lo / +0x18 hi) gives 185,478,790 at 1391 s uptime and 10,904,651 at 80 s
  uptime *after a reset* - both ~136 kHz, both proportional to uptime. It
  restarts from zero on an esptool reset. It is an uptime counter, not a clock.

So an RTC-class driver over it would be **worse than nothing**: Linux would get
an RTC reading "1970 + uptime", `CONFIG_RTC_HCTOSYS` would set the system clock
back to 1970 on every boot - overriding the saved timestamp - and `hwclock -w`
would appear to work while persisting nothing.

What ships instead (`/etc/init.d/S30clock`): a timestamp saved hourly by cron
and at shutdown, restored at boot and only ever moved forwards, plus busybox
ntpd as a retrying daemon. Same pair Raspberry Pi OS uses, for the same reason.
Timezone is Europe/London from zoneinfo, so BST is handled rather than
hardcoded.

Ordered S30, ahead of the desktop at S40: musl caches the timezone on first
use, so a desktop that starts first shows UTC for ever - 12:23 on the panel
against 13:23 on the console until the ordering was fixed.

## Undo has to cover flash state, not just source (2026-08-28)

**This cost hours and left the board completely unusable.** Read it before
changing anything on hart0.

Teaching the ESP-Hosted co-processor to associate early (so screen capture could
start before Linux) meant writing two things into the `nvs` partition: a
credential slot, and `state.auto_connect = 1`. Backing the change out did
nothing at all, because the **stock** code reads `auto_connect` from NVS and
associates on `WIFI_EVENT_STA_START` regardless of who put it there. The radio
then powered up during the loader's splash and starved the LCD scanout DMA:

- the splash tore and shifted, wrapping round the panel
- it was **intermittent**, depending on when association landed
- `git checkout` plus a rebuild produced a loader of byte-identical size and
  changed nothing, which made it look like the revert had failed

The fix was to erase the store:

    esptool -p $PORT -b 2000000 erase-region 0x11000 0xF000    # nvs

Linux's own credentials are in `/etc/wpa_supplicant.conf` on the card and are
not affected.

**The rule: the moment a change writes persistent flash - NVS, retention
registers, the card - write down that it did and how to clear it. That note is
the only undo.** Before concluding a revert has failed, ask what the change
*persisted*.

### Two more from the same incident

- **The loader is three pieces and they must match**: `bootloader.bin` @ 0x2000,
  `partition-table.bin` @ 0x8000, and the app `hello_world.bin` @ 0x20000.
  Flashing only the app against a second-stage from another build boot-loops, or
  hands off into silence with no console output at all.
- **Old loader binaries in `images/` are not a rollback.** They carry partition
  geometry compiled into `main.c` (twice) and reset ~350 ms in when it disagrees
  with the current table - which it now does, since the linux partition grew to
  6,422,528. `images/hart0.bin` (20 Aug) and `images/qio/hello_world.bin`
  (21 Aug) both boot-loop. The only rollback is a rebuild from source, so do not
  overwrite `images/hello_world.bin` without keeping the previous copy.

### Also settled, and worth keeping

hart0's link is fast enough for streamed screen capture: **25.8 Mbit/s
(3.1 MB/s) sustained, essentially lossless**, measured by injecting raw
Ethernet/IPv4/UDP through `esp_wifi_internal_tx()` - no lwIP, which hart0 cannot
use because the hosted path hands receive to Linux. That is ~78 fps at a 40 KB
frame, against a JPEG encoder that does 7.2 ms/frame. Serial is 8-25x too slow.
The S31 has **no** hardware H.264 (`soc_caps.h`: JPEG and PPA only), so MJPEG is
the codec.

**But capture must not run during the loader's splash**: the probe saturating
the link is what first tore the panel, before the NVS problem was understood.
Whatever drives capture has to pace itself and stay off the bus while the
loader owns the display.
