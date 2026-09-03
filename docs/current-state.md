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

## Off-the-shelf X11 apps run in lvdesk, with no X server (2026-08-29)

`xclock` - the real Buildroot binary, unmodified - runs as an lvdesk window
with a title bar, buttons and a task bar entry. There is no X server on the
board.

lvdesk speaks the X core protocol itself (`lvdesk/xshim.c`, ~850 lines). Each
client window is a buffer sized to that window, presented as an `lv_image`
whose data pointer *is* that buffer, so there is no copy and no virtual screen.
The request set is not "X11": it is the 19 request types a real xclock was
observed to send, captured with `tools/xstub.py`, of which three actually draw.

    MemAvailable cost of running xclock          684 kB
    Xfbdev resident, no clients connected      2,876 kB
    lvdesk idle, 10 s, xclock running        36 jiffies
    lvdesk idle, 10 s, control               33 jiffies
    xclock idle, 10 s                         0 jiffies

The idle difference is inside this board's noise - the claim is "free", not
"0.3%". xclock's zero is real: an analog clock repaints on the minute.

Shipped: `/usr/bin/lvdesk` in the XIP image carries the shim, so the socket is
there from boot and `xclock &` in lvdesk's own terminal works.

**Not done yet**, in the order the next client will demand them: input routing
to clients, `ImageText8`/`PolyText8` text drawing, and `ConfigureNotify` so
resizing the lvdesk window resizes the client. Also open: xclock costs
2,160 kB RSS purely because libX11/libXt/libXaw are read from SD - `/usr/lib`
is an XIP overlay where mapped binaries cost **zero** RSS, but their closure is
~2.7 MB against 225 kB free in the xip2 partition, so it needs a repartition,
not just a line in `XIP2_ROOTS`.

See `docs/x11-shim.md` for the protocol details, including three traps that
each produce a plausible wrong picture rather than an error.

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

## Filming the board, boot to desktop (2026-08-28)

**This works end to end.** 560 frames, 170 s, from 1.8 s after power-on through
the boot console, lvdesk starting, and the desktop being driven, to a stop
signal. Zero malformed frames.

### How to run one

    on the board:   s31-record arm      # then reboot to film the next boot
                    reboot
                    ...use the desktop...
                    s31-record stop     # completes the file
                    s31-record serve    # prints the curl line to fetch it
    on the host:    curl -o session.mjpeg http://<board>:8080/
                    scripts/board/mjpeg2mp4.py session.mjpeg out.mp4

`s31-record start` films from now without a reboot, and misses the boot.

### Why it is built this way

- **The kernel arms the recorder at scanout, 1.7 s in.** The ext4 root is not
  mounted until ~4.9 s, so anything started from an init script misses the
  whole early console - which is the part worth filming. `/etc/init.d/S02s31-vidcap`
  then *attaches* to that recording rather than starting a new one.
- **The trigger is a magic word in LP_STORE12**, written by `s31-record arm`.
  It survives a warm reboot, is cleared by an EN reset or a power cycle, and
  the driver clears it on read, so one arming films exactly one boot and it can
  never stick. **Deliberately not hart0's NVS**: a trigger in flash survives a
  source revert and a reflash, which once left this board unusable.
- **Most of LP_STORE belongs to the ROM.** `esp_rom/esp32s31/rom/rtc.h`:
  STORE1 slow-clock calibration, 2/3 boot time, 4 ROM log control and crystal
  frequency, 5 deep-sleep entry length, 6 wake entry address and reset cause,
  7 memory CRC, 8 sleep wake stub, 9 LP core wakeup cause. **Reading zero at
  runtime does not mean a slot is free**: STORE5 read back zero every boot
  because the ROM rewrites it, and writing STORE6 wedged hart0 with no console
  at either baud until an EN reset. STORE10..15 are unclaimed; this uses 12.
- **Capture is per commit, not per panel refresh.** LCD_CAM scans out 59 times
  a second whether or not a pixel changed, so filming every refresh would encode
  identical frames: measured, every-other-refresh costs ~21% of a core and
  ~440 KB/s on a completely static screen. Instead each frame carries its
  capture time, the gap to the next is how long it was on screen, and
  `mjpeg2mp4.py` turns that into real-time or constant-rate video on the host
  for free. **30 fps and 59 fps produce identical files** - lvdesk commits
  13-24 times a second during motion (measured: 41 ms best, 75-80 ms median),
  so any cap above ~25 is already "every commit".
- **The recorder must not open /dev/dri/card0.** The first process to open the
  primary node becomes DRM master. A drainer started at S02 therefore took
  master and lvdesk died at S40 with "SET_MASTER: Resource busy"; the panel
  froze and, because nothing committed, the recording captured nothing. The
  driver now advertises DRIVER_RENDER and the drainer opens `renderD128`.

### Getting the file off

The card is soldered, so it cannot be read on another machine. Measured:

    console (base64 over serial)     ~65 KB/s     52 MB -> 13 minutes
    network (s31-serve over Wi-Fi)   ~915 KB/s    52 MB -> 58 seconds

**Never base64 a large file over the console.** It floods a 1 Mbps line for
minutes, during which every other tool reports NO_SHELL - and if the thing that
started it retries, it starts another. That failure mode cost most of an hour.
`s31-serve` is one-shot: it serves the file and exits, leaving nothing running.

### Two traps in reconstruction

- **Slice the sidecar in ARRIVAL order. Do not sort it.** The sequence number
  restarts when the drainer hands over from the boot recording to the session
  one, so sorting interleaves the two runs and every byte offset after the
  handover is wrong. It does not fail loudly: 106 of 560 frames decoded as
  garbage while the total byte count still matched exactly, because the sizes
  summed correctly even though the boundaries did not.
- **One sequence discontinuity is expected** - it is the handover, not a lost
  frame. `mjpegrec` reports it as a gap.

## Syscall cost: already fixed, and what is left is not material (2026-08-29)

Revisited because `clock_gettime` measured 7.2 us from inside lvdesk and that
looked like a systemic tax. `rootfs/syscallbench.c` splits it, three runs:

    getpid                    ~1,495 ns/call     syscall entry floor
    clock_gettime (libc)      ~2,771 ns/call
    clock_gettime (raw)       ~2,792 ns/call
    empty loop                    26 ns/iter     harness

Three conclusions:

- **Entry is ~1.5 us, not 4.5.** The earlier `.text..fast` work on the trap
  path (43.0 -> 9.0 us for a read+write pair) already took this win. There is
  no second one waiting.
- **The clocksource read adds ~1.3 us**, which is the gap between getpid and
  clock_gettime.
- **There is no vDSO.** libc and the raw syscall are within 1% of each other,
  so every time query traps. The kernel does build `vdso.so` and sets
  `HAVE_GENERIC_VDSO`, but `arch/riscv/Kconfig` line 112 reads
  `select GENERIC_GETTIMEOFDAY if HAVE_GENERIC_VDSO && 64BIT` - the generic
  vDSO time path is **64-bit only upstream**. Not a config to flip; it would
  mean porting it to rv32.

**And it does not matter at our rates.** lvdesk makes roughly 600 syscalls a
second (~0.09% of the core) and ~200 clock_gettime calls (~0.06%). Eliminating
every one of them saves a fraction of a percent. The 7.2 us seen from inside
lvdesk against 2.8 us standalone is contention - cache pressure under load -
not a different cost.

The one genuinely expensive "syscall" on this board is the DRM cursor ioctl at
**~1 ms**, 660x a getpid, and that is an atomic commit doing real work rather
than trap overhead. It is already paced to the frame period.

So: the syscall lead is closed, on numbers rather than on effort.

## What the 4.4 MB of slab actually is (2026-08-29)

`/proc/slabinfo` does not exist in the shipping kernel and cannot: it needs
`CONFIG_SLUB_DEBUG`, which is mutually exclusive with **`CONFIG_SLUB_TINY`** -
the minimal-footprint allocator this board is built with. Measuring the slab
therefore means changing the allocator, so the absolute numbers below are from
a throwaway kernel with `SLUB_TINY` off (Slab 5812 kB against the shipping
4380 kB). The **composition** is the useful part:

    kernfs_node_cache     814 kB   9476 objects   sysfs metadata
    kmalloc-1k            384 kB
    dentry                379 kB   reclaimable
    kmalloc-64            340 kB
    debugfs_inode_cache   331 kB   1008 objects   debugfs
    biovec-max            270 kB     90 x 3072    block layer
    kmalloc-128           220 kB
    kmalloc-512           208 kB
    inode_cache           196 kB
    kmalloc-8k            192 kB
    task_struct           191 kB     90 tasks
    shmem/ext4 inodes     ~375 kB   reclaimable

So roughly **1.1 MB is sysfs and debugfs metadata**, and the single largest
item is 9476 kernfs nodes. Three things follow:

- **debugfs costs ~331 kB plus its share of the kernfs nodes**, and it is there
  because we asked for it: `CONFIG_DEBUG_FS` was enabled for usbmon and the LCD
  driver's counters. That is a deliberate trade and now a priced one - this
  session's USB and display work would not have been possible without those
  counters. Turn it off for a build that needs the memory, not before.
- **The filesystem caches (dentry, inode_cache, ext4, shmem: ~950 kB) are
  reclaimable** and will shrink under pressure. They are not a leak.
- What is left is genuine kernel structure - task_struct for 90 tasks, block
  layer vectors, kmalloc for driver state. There is no single large reclaimable
  block hiding in there.

The honest summary: the slab is not where a big RAM win is waiting. Userspace
is already at ~1.8 MB total RSS with everything XIP, and the kernel's 4.4 MB is
mostly metadata for the devices and filesystems the board actually has.

## Boot audit II: the background fix had moved the cost (2026-08-29)

`rcS complete` went **84.35 s -> 43.79 s**, and the whole of it was one thing
wearing a disguise.

The complaint was "S40network takes forever". It does not. Fine-grained
markers written to `/dev/kmsg` from `wlan0-up.sh` split it up:

    28.26  before ifup -a
    36.47  pre-up begins          8.2 s just to enter pre-up
    40.62  ip link show done      4.2 s  for a netlink query
    44.98  iw dev info done       4.4 s
    70.68  wpa_supplicant started 20.8 s
    76.32  associated             5.6 s  <- the only part that is really network

`ip link show` costs **0.23 s** warm. Everything in that window ran ~20x slow,
so this was never a network problem: it was whatever else had the CPU.

**It was udev's coldplug** - and specifically the previous audit's own fix for
it. Backgrounding `udevadm trigger`/`settle` (~32 s of real work: 269 devices,
27 rule files, and a 0.16 s process launch each) stopped it blocking the
desktop, but the work did not go away. It landed on `S40network` instead.
Measured, same boot, one variable:

    |                  | coldplug nice 0 | nice 19 | udev not run |
    | ip link show     |     4.2 s       |  0.21 s |    0.23 s    |
    | iw dev info      |     4.4 s       |  0.23 s |    0.23 s    |
    | wpa_supplicant   |    20.8 s       |  2.4 s  |    2.1 s     |
    | rcS complete     |    84.35 s      | 42.97 s |   37.13 s    |

`nice -n 19` recovers essentially all of it while udev still does its whole job
(`/dev/input` and `/dev/dri` both populated). **Deferring work is not removing
it** - see the note on the kworker hand-off for the same lesson at millisecond
scale.

Two hypotheses were tested and killed first, and neither should be re-run:

- *lvdesk contends with the network* - **no.** Booting with `S40lvdesk` moved
  out of `init.d` entirely: `wpa_supplicant` still took 23.2 s and rcS still
  finished at 81.4 s against 84.4 s.
- *the binaries are demand-paged off SD* - **no**, though the finding it led to
  was real (below). Moving them to XIP changed the stall by nothing at all:
  `wpa_supplicant started` stayed at 34.5 s after pre-up.
- *entropy starvation* - **no.** `crng init done` at **0.842 s**; the board has
  a hardware TRNG (`esp32s31-trng`) and the pool is full. wpa_supplicant's
  "Trying to read entropy from /dev/random" line is startup chatter, not a
  stall.

### /usr/sbin and /bin were never overlaid, and the XIP image already held them

Separate bug, found on the way. `S05xip` looped over `usr/lib usr/bin
usr/libexec lib`. The XIP image *already contained*
`/usr/sbin/wpa_supplicant` (784 KB), `/usr/sbin/iw` (256 KB) and
`/bin/busybox` (734 KB) - **1.77 MB of flash** - and none of it was ever
mounted, so all three ran from the SD card and the flash copies were dead
weight. Adding `usr/sbin` and `bin` to the loop:

    wpa_supplicant RSS   from SD 232 kB  ->  from XIP 88 kB

On a board with ~2 MB free that is worth having on its own, and it costs
nothing - the bytes were already in flash.

### keylog was starting on every boot

`S06keylog` started `/root/keylog` whenever the binary existed. It was found
running at **56% of the core**, holding idle at 0% and lvdesk at 12% CPU;
killing it gave 40% idle and lvdesk at 4%. It is not in the repo - it lives
only on the card, which is exactly the "SD holds state the repo does not"
trap. It is now behind `/etc/s31-keylog-enable`, and `usbtrace off` kills it.

### Placement, as it actually stands

| where | what | cost |
|---|---|---|
| XIP flash, overlaid | `/usr/lib`, `/usr/bin`, `/usr/libexec`, `/usr/sbin`, `/lib`, `/bin` | lvdesk: 723 KB binary at **96 kB RSS** |
| `.text..fast` (RAM) | 242,192 bytes, 2925 functions - blk 193, mmc 174, input 92, dwc 85, bio 72, snd 62, sched 56 | ~1.6% of RAM for the ~6x hot paths |
| SD | everything else in `/usr/share`, `/etc`, data | |

### Still open

- **One `Oops [#1]`** was seen at 139 s of a heavily-poked boot (overlays
  mounted by hand, wpa_supplicant killed repeatedly). The board went silent
  before the trace could be read and it has **not** recurred across five clean
  boots since. Unexplained; do not assume it is gone.
- **Association is 5.6-6.2 s** and is now the largest single item in
  `S40network`. It is a hidden-SSID scan (`scan_ssid=1` forces active probing)
  plus the WPA handshake. Worth attacking only after the above.
- ~~bluetoothd does not fit in XIP~~ **Done** - see below. It took removing
  opkg *and* leaving libasound on the card.

### bluetoothd is in XIP, and opkg and Weston are gone (2026-08-29)

`bluetoothd` is the largest resident thing on the board and never exits, so it
was the right target. It needed **two** evictions, not one:

    opkg          libarchive 623,772 + libopkg 174,424 + the binary, plus
                  libexpat and libz which turned out to be its alone
    libasound     943,548, NEEDED by lvdesk but only for the volume mixer and
                  the odd PCM write - a human moving a slider, not per frame

Being NEEDED is not the same as being hot, and `XIP_SKIP` now expresses that:
an object in the closure can be left on the card, where it still loads through
the SD lower layer of the same overlay. Only its residency changes. Without it
the two images total **8,205,052 against 7,602,176** of partition and no
arrangement fits.

    XIP image  5,951,488 bytes, 208,896 free   bluetoothd, glib, pcre2, dbus
    xip2 image 1,216,512 bytes, 225,280 free   ip, udevd, libkmod, libblkid

Measured after, with the pre-change figure for the same binary:

    bluetoothd binary-backed Rss   184 kB (SD)  ->  28 kB (XIP)
    glib-backed Rss                                  8 kB

Bluetooth still works - `hci0` present, `Bluetooth: MGMT ver 1.23`, bluetoothd
driving it - and lvdesk still runs with libasound coming off the card.

**Weston was never in the defconfig**; the card was carrying leftovers from an
older image - binaries, `libweston-15.so.0.0.0`, the shells, `/usr/share/weston`,
`/etc/xdg/weston`. Removed along with opkg: **3,128 kB** of SD freed. They cost
no RAM, so this is tidiness, not a performance change.

Removing files under `/usr/bin`, `/usr/lib` and friends has to go through the
bind mounts (`/mnt/sd-usr-bin`, `/mnt/sd-usr-lib`): the overlays are read-only,
so `rm` against the merged path silently fails to remove the SD copy.

### xip2 is in use: ip and udevd (2026-08-29)

`XIP2_ROOTS` was empty, so the image staged at **4,096 bytes** while reserving
1,441,792. It now carries `sbin/ip` and `sbin/udevd` with their closures
(libkmod 79,160, libblkid 329,436): **1,216,512 bytes, 225,280 free**. libc is
NOT duplicated - image 1 holds it and `EXCLUDE_DIR` skips it.

Measured on the board, with a binary still on the card as the control:

    udevd       XIP2   263,680 byte binary    28 kB resident from it
    ip          XIP2   581,604 byte binary    24 kB resident from it
    bluetoothd  SD     797,012 byte binary   184 kB resident from it

**It changed boot time by nothing** (rcS 43.29 s against 43.79 s), which is the
expected result: the boot bottleneck is udev's coldplug burning CPU, not
paging. This is a memory win, not a latency one.

Three traps, all of which cost a round trip here:

- **The overlay loop guarded on image 1 alone.** `[ -d "$XIP_MNT/$d" ] ||
  continue` ran before image 2 was consulted, so `/sbin` - which exists in
  image 2 and not image 1 - was skipped entirely. The flash was written, the
  image mounted, the files visibly present under `/mnt/xip2/sbin`, and nothing
  used them. Build the lower stack first, then skip only if *both* are empty.
- **`make xip2-rootfs` drags in a full Buildroot `target-finalize`**, which is
  minutes and can fail for unrelated reasons. `xip2-image` is the
  no-dependency form, and `xip-fast` now builds both images in the required
  order (image 2 must stage after image 1 for `EXCLUDE_DIR` to work).
- **`mkxipstage.py` printed the whole closure unannotated**, so libc appeared
  in both images' listings and read as a 690 KB duplicate that was never
  staged. It now marks skipped entries.

## Boot audit: 83 s to a desktop became 24 s (2026-08-28)

Every line of the boot log was walked, with per-script timestamps written to
`/dev/kmsg` from `rcS` - which is the only way to see this, because the two
scripts that dominated the boot printed nothing at all while they ran. That is
why it read as a hang at the last input device rather than a slow script.

    S10udevd      34.0s -> 0.52s   coldplug now runs in the background
    S04s31-swap   15.7s -> 0.56s   ls -l instead of wc -c on a 64 MB file
    S30dbus       6.34s            moved behind lvdesk
    S40bluetoothd 2.67s            moved behind lvdesk
    desktop up    83.0s -> 24.2s

- **udev**: `udevadm trigger` is 11.2 s here and the `settle` after it 20.9 s -
  real work, not the 30 s timeout expiring. Nothing needs to wait: devtmpfs
  makes the nodes, udev only applies rules, and lvdesk opens /dev/input itself
  and rescans every 2 s.
- **swap**: `wc -c < /swapfile` READS 64 MB, 15.3 s at ~4.4 MB/s, every boot,
  to learn a size the directory entry already holds. The comment warning that
  this busybox lacks `stat` is correct - `ls -l | awk` is the right answer.
- **profile=6 removal saved nothing** - the commit that made it claimed 253 KB
  on fabricated numbers (two different "bytes free" for one image size, which
  cannot happen). Rebuilt with it restored: 6,086,665 vs 6,086,673 bytes, an
  eight byte difference. CONFIG_PROFILING is disabled by the Makefile's
  kconfig-tweak, so the argument is inert - /proc/profile does not exist on the
  running board. The 253 KB in these notes refers to enabling CONFIG_PROFILING.
- **dbus/bluetoothd**: 9.0 s combined, needed by nothing on the way to a
  desktop. Renamed in `post-build.sh`; an overlay cannot remove the names
  Buildroot installs, so both copies would run.

Everything left is under 1.7 s except **S40network at 54.4 s**, which starts at
23.8 s - after the desktop - so it delays Wi-Fi and crond, not the desktop.
That is the next target: `ifup -a` doing a hidden-SSID scan (`scan_ssid=1`
forces active probing) plus DHCP.

### The mmc errors are real but were NOT reproduced

Seen once, at 9.5 s and 9.8 s of one boot:

    data error: mintsts=0x00000200 idsts=0x00000102 rintsts=0x00010028
    Unexpected data interrupt latency

`mintsts` bit 9 is DRTO (data read timeout); `rintsts` carries a data CRC error
with it. The driver recovered - nothing surfaced to the filesystem. **192 MB of
sustained reads afterwards produced zero errors**, and no boot since has
repeated them, so the trigger is unknown. Do not attribute them to the
swapfile read without new evidence; that hypothesis was tested and failed.

### xip2 is genuinely empty - 1.4 MB of flash doing nothing

`cramfs: linear cramfs image on mtd:xip2 appears to be 4 KB in size` is not a
mis-report. `images/rootfs-xip2.cramfs` is **4096 bytes** on the host too.
Image 2 is built with EXCLUDE_DIR against image 1, and image 1 (5,259,264 of
6,160,384 bytes) already holds the entire closure, so there is nothing left for
image 2. The xip2 partition is 1,441,792 bytes, so **1,437,696 bytes of flash
are reserved for an empty filesystem** - on a board where the linux partition
has been down to ~53 KB of slack. Reclaiming it is free space, and `S05xip`
would stop mounting a third overlay for nothing (1.63 s).

### The remaining log noise is benign

`supply ... not found, using dummy regulator` (panel, dwc2, es8389) is the
DT declaring no regulator for rails that are hard-wired. `fifo-depth property
not found` falls back to reading FIFOTH, which is correct. `Direct firmware
load for regulatory.db failed` means no CRDA database - the radio uses its
built-in defaults. `Console: colour dummy device` at 0.001 s and again at
24.08 s is fbcon before the driver binds, and the desktop taking over.

### Input: what was eliminated, and what remains

**lvdesk does not drop keystrokes under test.** 36/36 characters at 90, 20 and
5 ms per key, and again under 64 MB of concurrent I/O. But the harness lied
first: `uinject`'s keycode map knew only a-z, space, newline, `- . /` and the
digits 1, 2, 3, and skipped everything else silently, so `echo abc...0123456789
> /tmp/f` arrived as `echo abc...123  /tmp/f`. That is indistinguishable from
the desktop eating input. **Measure the harness before believing the result.**

Two real hazards were found and fixed, neither yet observed in the wild:

- **SYN_DROPPED was discarded.** Every evdev reader gets a kernel ring; if it
  is not read for long enough the kernel throws the backlog away and reports it
  exactly once as `EV_SYN`/`SYN_DROPPED`, which lvdesk's "not EV_KEY, skip it"
  filter dropped. A stall therefore ate every keystroke made during it, with no
  trace - the exact shape of "seconds of typing simply gone". Now counted and
  logged, along with any gap over 500 ms between input polls, and modifiers are
  cleared afterwards because a release may have been among the lost events.
- **The pty write discarded the character with the error** (`if (write(...) <
  0) { }`) when the non-blocking master returned EAGAIN. Now retried briefly,
  then counted.

**Still open, and the best remaining hypothesis: the 2.4 GHz receivers.** Both
HID receivers are 2.4 GHz, on a bus-powered hub declaring `bMaxPower=100mA`
while feeding 98 mA + 100 mA, centimetres from the board's own 2.4 GHz Wi-Fi
antenna. Interference or brown-out would stop reports arriving for seconds with
no USB error logged, because the receiver stays enumerated. Independent
evidence that this path misbehaves already exists: the kernel was seen
auto-repeating KEY_M that nobody was holding, which means a HID report was lost
or garbled. Cheap tests: plug the receiver straight into the board with no hub,
and check whether the loss correlates with Wi-Fi traffic.

## The X11 client stack moved into XIP flash (2026-08-30)

`xcalc` now runs from `/usr/bin` against our own libraries in `/usr/lib`, and
costs **124-180 kB resident**. It was 2,104 kB when it first ran.

    2,104 kB  stock libX11/libXt/libXaw/libXmu/libICE/libSM/libXext/libXpm
      592 kB  after xlite + xtlite + xstubs replaced them
      296 kB  after the font cache and the buffer fixes (see docs/xlite.md)
      180 kB  from XIP flash - every r-xp text mapping costs zero RSS

`make x11-stage` installs the eight replacement libraries into the Buildroot
**overlay**, which is what makes them survive a target rebuild and what makes
the closure packable at all: staged from the stock libraries, xcalc's closure
is 2.51 MB against a 1.41 MB partition, because the stock libX11 alone is
1.3 MB and drags libxcb, libXau and libXdmcp behind it. With ours it is
**528,384 bytes, 913,408 free**. The full sequence:

    ./docker/build.sh 'sh /src/xlite/build.sh'      # and xtlite, xstubs
    make x11-stage
    ./docker/build.sh 'cd /src && $S31_MAKE xip-fast'
    ./docker/build.sh 'cp /src/build/rootfs-xip*.cramfs /src/images/'
    make flash-xip-rootfs flash-xip2-rootfs

**A latent bug in `rootfs/mkxipstage.py` that only surfaced here.** It
recreated the SONAME symlinks a loader looks for, but only names derived from
the object's own basename - so `libXaw.so.7 -> libXaw7.so.7 ->
libXaw7.so.7.0.0` lost its first hop. xcalc's DT_NEEDED says `libXaw.so.7`, so
the file was staged under a name nothing asks for and the binary would not
start from XIP at all. It now also copies any alias the target directory
already has for a staged object.

**Card state this repo does not hold.** `/usr/share/X11/app-defaults/` did not
exist on the ext4 root - the app-defaults only lived in the `/root/x11`
development staging tree - so an xcalc started with `XFILESEARCHPATH` pointing
at the standard path built its widget tree with NO resources and came up as a
90x60 box showing only the display. That is the same degenerate layout a
missing app-defaults has always produced, and it looks like a rendering bug.
They are now installed at `/usr/share/X11/app-defaults/`.


## lvdesk: Caps Lock was inverted for the whole session (2026-08-30)

Caps Lock behaved backwards in the terminal - caps ON typed lower case, and
pressing caps to correct it typed upper. The state machine was right; the
starting assumption was wrong.

On a USB keyboard **the lock is entirely a host-side concept**. The keyboard
only ever sends `KEY_CAPSLOCK`; whoever tracks the state owns it, and owns the
LED. lvdesk initialised `mod_caps = 0` and never asked, so if caps was already
on when it started, every letter was inverted until the user toggled it - which
inverted the inversion rather than fixing it.

Reading the state at startup was necessary but **not sufficient**, and the
first version still drifted. With `LVDESK_NO_GRAB=1` the console keyboard
handler is processing Caps Lock as well and keeps a lock state of its own, so
there were two owners each with a private toggle: they diverge permanently the
first time either misses a press, and pressing caps to correct it inverts the
inversion instead.

The fix is to stop keeping a rival toggle. The LED is the state the kernel
actually holds, and evdev reports every change of it as an `EV_LED` event, so
lvdesk follows that:

    lvdesk: keyboard on /dev/input/event1 (caps on)
    lvdesk: caps off (from the kernel's LED)

It reads `EVIOCGLED`/`LED_CAPSL` when it opens each keyboard, then tracks
`EV_LED` events thereafter. It only toggles and writes the LED itself if no
`EV_LED` has ever arrived - i.e. when nothing else is driving it - because
doing both applies the same press twice. Devices are opened `O_RDWR` for the
write path, falling back to read-only. When it does write, it writes to *every*
keyboard node: a composite receiver presents several, and the lock is a
property of the session, not of one interface.

Also set alongside `DISPLAY`: **`XFILESEARCHPATH`**. Without it an Xt client
started from lvdesk's terminal finds no app-defaults and lays itself out
degenerately - xcalc comes up as a 90x60 box showing only its display, which
reads as a broken display server rather than a missing environment variable.

### Two observations recorded, not yet chased

- The hardware cursor plane stays drawn in fbcon after lvdesk exits; it should
  be disabled on the way out.
- lvdesk's own "System" window opens partly off the right edge of the panel at
  boot. It is not a leaked X client - it is there on a clean boot with no
  clients at all.

### Ruled out: a leak in the X client lifecycle (2026-08-30)

Suspected after a session of apparent input lag. Five xcalc open/close cycles,
lvdesk's RSS and the shim's own accounting (`SIGUSR1` -> `xshim_mem_report`)
after each:

    start   rss=660 kB   1 window buffer 189 kB   (a live client)
    cycle1  rss=536 kB   0 window buffers
    cycle2  rss=536 kB   0 window buffers
    cycle3  rss=536 kB   0 window buffers
    cycle4  rss=536 kB   0 window buffers
    cycle5  rss=536 kB   0 window buffers

Flat to the kilobyte, and the shim releases each client's buffer on exit.
3,156 kB still available afterwards. A 1,008 kB reading taken earlier in the
same session was a longer-lived lvdesk holding a live client's 189 kB buffer,
not growth - a single RSS number with an unknown number of windows open says
nothing at all.

**The remaining unfalsified explanation for that session's lag is external
load**: ~1 MB binary deploys over Wi-Fi and 768 kB screenshot reads, repeatedly,
while a human was trying to type. `rootfs/inputalign.c` now records its own
scheduling, so that is testable rather than arguable - run a capture while
deliberately deploying and screenshotting, and look for STALL lines.

## Audio works, verified by ear (2026-08-31)

Clean 440 Hz from a cold boot: 48 kHz S16 stereo through the ES8389, mono
sources duplicated to both channels and 22.05 kHz resampled by alsa-lib's
plug layer (both ear-verified), so the card behaves like any ALSA chipset.
Rebuilt from scratch against the vendor ground truth for this exact board -
esp-dev-kits factory_demo -> esp32_s31_korvo BSP -> esp_codec_dev 1.6.2 -
trusting nothing already here. Two independent faults, either alone enough
for the "loud crackle" this replaced; both are in `patches/0022`:

- **The I2S controller was companding.** hw_params wrote TX_CONF whole from
  four fields, clearing bits that RESET TO 1 - above all `tx_pcm_bypass`
  (bit 12): cleared, every sample runs through the hardware A-law compander.
  A companded sine is loud pitched noise. Bit positions now come from IDF's
  `soc/esp32s31/register/soc/i2s_struct.h`; the `i2s_ll.h` the old comment
  cited does not exist for this SoC. Same fix on RX.
- **The codec ran its engine at half speed.** The vendor programs the
  {64, 3072000} coefficient row - `rate * bits * 4` - against a 32x-fs wire
  clock, with the "internal reference" bits set (0x23[7], 0xF0=0x1A,
  0x0F=0x10) that evidently double the internal clock. Mainline picks the
  {32, 1536000} row with them clear. `dac_ref=0` module param restores
  mainline behaviour for A/B.

Also settled: the flashed loader's audio really is off (strings on the
binary); GPIO7 PA is genuinely driven (GPIO_ENABLE bit 7, FUNC_OUT_SEL 256,
out high); the Korvo BSP routes MCLK on GPIO2 but sets `use_mclk=false`, so
SCLK-sourcing (0x02=0x40) is right and the old "no MCLK trace" note was
another board's BSP.

**The 16 KiB DMA ring is 85 ms, and that is a fact to live with, not fix.**
Under deliberate SD+CPU load aplay underruns in ~350 ms bursts (3 in a 6 s
WAV) because it has no lookahead beyond the ring; the same file from tmpfs
plays clean under the same load. Growing the ring to the map's limit (24 KiB
play = 128 ms; above that means moving `S31_AUDIO_DMA_BASE` in the shared
two-hart layout plus a loader reflash) still would not cover 350 ms, so it
was not done. Real players carry their own decode buffers and are fine;
raw file playback should use `s31-play` (overlay `/usr/bin`, stages <=4 MB
files to tmpfs first). Open lead if it ever matters: SD scheduler fairness -
one dd starves a reader for ~350 ms.

RAM review of the volume path (2026-08-31, measured with controls): NOT a
leak - lvdesk RSS/VmData flat to the kilobyte over repeated popover cycles
and bong runs, no shadow surfaces (DIRECT render, widgets in LVGL's static
512 KB pool). The real costs were 1.5 MB of test WAVs left in tmpfs
(deleted), alsa-lib's parsed config tree held forever after first mixer use,
and a per-bong re-parse + plug chain in the fork. Fixed:
snd_config_update_free_global() after mixer load and in the bong child, and
the bong opens hw:0,0 since the tone is already native format. Verified
under a 4-client desktop: first volume use now costs 48 KB of heap
(VmData 3188 -> 3236) and stays byte-flat through every later popover and
bong; MemAvailable ends higher than it starts. The bong was always
synthesized programmatically - no PCM is stored. Note MemAvailable jitters
~200 KB from any process launch (page cache) and recovers lazily; that is
noise, not growth.

lvdesk's own volume path is verified end to end: tray icon -> popover ->
slider click writes BOTH DACL and DACR (0x46/0x47: 0xBF -> 0x8E at 55%,
back to 0xBF at 74%) and the confirmation bong plays at the newly chosen
level - two bongs, the first audibly quieter, by ear. The popover's
read-back percentage tracks the real register (74% == 0xBF/255).

Capture compiled in and carries the same RX fix, but remains unverified by
ear since the 2026-08-28 session (task #14).

## RAM economy round, and a flash reshuffle that bit (2026-08-31)

Idle desktop MemAvailable ~3.2-3.5 MB after: 1.5 MB of test WAVs deleted from
tmpfs; syslogd/klogd moved to S06/S07 (after the XIP overlay) so their busybox
text runs from cramfs - 968 KB resident down to 76 KB; libasound moved into
xip2 so the volume path's 324 KB of SD-backed text now costs nothing; the
2 s input-device rescan replaced with an inotify watch (was ~130 ms per 5 s
idle plus the 40-90 ms hitches); alsa's config tree kept for the process
lifetime (freeing it returns nothing - musl keeps the pages - and charged an
SD re-parse per bong). Measured and waiting: LVGL's static pool peaks at
25 KB of 512 KB (5%) on a working desktop - re-read `lvmem` via the control
FIFO after heavy use, then shrink LV_MEM_SIZE with margin. The stashed
uniform-pixmap work (~705 KB across X clients) remains the biggest X-session
lever, gated on a full examined sweep.

**The flash layout changed - record of what and how to undo.** To fit
libasound (943,548 bytes): factory 0x200000 -> 0x1A0000 (loader app is
1.62 MB; 82 KB headroom - an audio-enabled loader build will NOT fit),
xip2 0x2A0000/0x160000 -> 0x1C0000/0x1C0000, opensbi 0x2A0000... -> 0x380000.
linux and rootfs did not move. Rollback = revert the five-home commit set,
rebuild loader + opensbi, reflash bootloader/table/app + opensbi + both xip
images. **The opensbi address lives in FIVE homes**: partitions.csv, TWO
defines in bootloader/main/main.c, FW_TEXT_START in the Makefile, the assert
in opensbi's fw_base.ldS - and a bare `li t0, ...` in
bootloader/main/core1_trampoline.S. The fifth was missed: hart1 jumped into
the middle of the relocated xip2 image and died silently, and alive.py's
raw-buffer stage matching reported SHELL from framing garbage for four
resets running (both fixed; alive.py now prints whole phases with byte
counts and classifies only filtered lines). Also learned: XIP_SKIP demotes a
library to "the SD lower layer" - which only works if the card actually
holds a copy. This card predates the X11 stack; libxkbfile/libxcb/libXau/
libXdmcp had to be backfilled onto the ext4, and the sweep is what caught
xclock failing to relocate XkbStdBell.

## Touch works: taps click X apps (2026-08-31)

Rebuilt from the factory demo's ground truth, trusting nothing local, same
method as audio. The GT1158 was never "blocked on hardware": it scans from
power-on with neither INT nor RESET wired, and only wants its status register
(0x814E) acknowledged with a 0 write on every poll - the vendor component's
exact rhythm, mirrored by the new `gt1158_polled` kernel driver (polled I2C,
20 ms default, module param; standard MT type B plus single-touch emulation,
so it is an ordinary Linux touchscreen at /dev/input/event0). lvdesk gained a
touchscreen device class: ABS_X/ABS_Y position the pointer absolutely and
BTN_TOUCH is the left button, so taps click - verified by finger on an
unmodified xcalc. Two traps for the record: INPUT_MT_DROP_UNUSED is
load-bearing (without it slots never release, BTN_TOUCH latches after the
first touch ever, and the cursor follows a finger that can never click); and
docs/touch-gt1158.md now opens with the correction of its own confident
wrong conclusion. rootfs/gtprobe.c is the userspace register-level probe.

## Bluetooth Classic works end to end (2026-08-31)

The S31 was never BLE-only - SOC_BT_CLASSIC_SUPPORTED=1 and the controller
is literally the BTDM (dual-mode) part. Three gates said otherwise, all
ours: the loader's CONFIG_BTDM_CTRL_MODE_BLE_ONLY; a hardcode in
esp-hosted's slave_bt.h ("only BLE for chipsets other than ESP32") that no
sdkconfig could override - the S31 branch added there maps BTDM_CTRL_MODE_*
and VHCI; and the kernel defconfig's BT_BREDR off (now on, plus BT_HIDP via
the Makefile tweaks - HIDP in the defconfig is dropped by olddefconfig
because the defconfig disables INPUT, the same trap as the touchscreen).
Verified: hart0 logs "BT/BLE dual mode", hosted capabilities 0xe8 -> 0xf8,
hci0 shows classic feature pages, HIDP loaded.

**The dual-mode controller library cost 300 KB of loader**: factory grew to
0x1F0000 (110 KB headroom) and xip2 shrank to 0x170000 at 0x210000 by
evicting udevd+libkmod to SD (no loadable modules exist; coldplug is
backgrounded). The boundary shuffle lands opensbi EXACTLY where it was -
0x380000 - so none of the five address homes moved. Modem sleep stays off
(BT_CTRL_SLEEP_ENABLE default n), per decision.

Userspace: bluez rebuilt with client/hid/hog/audio plugins (bluetoothd
797 KB -> 1,224 KB; fits XIP after evicting pcre2, glib's regex engine
that nothing here calls - and NOTE the XIP_SKIP trap again: pcre2 had to
be backfilled onto the SD card, which never had it). BlueALSA exposes A2DP
as ALSA PCMs (S47bluealsa, a2dp-source+sink); /var/lib/bluetooth is a real
SD directory now, so pairings persist - it was a tmpfs symlink that forgot
everything at reboot. bluetoothd runs 160 KB resident from XIP;
bluetoothctl and the bluealsa tools live on the SD. Classic keyboards go
through kernel hidp straight to evdev (bluetoothd out of the per-key
path); BLE keyboards go through bluetoothd's HoG into uhid (CONFIG_UHID
still to enable when one shows up). Pairing itself awaits a real device.

### Bluetooth correction: classic-only is the vendor-validated mode (2026-09-01)

Dual-mode BTDM inits and enables without error, the hosted layer advertises
"BT/BLE dual mode" - and the controller then answers Read_Local_Features
with "BR/EDR Not Supported" (features[4] bit 37). The kernel therefore
refused all classic security ("hci0: security requested but not available"
was an LE-SMP message from bluez courting the earbuds' Fast-Pair side; the
classic bearer never actually existed), bluez treated the adapter as
LE-only, and every "classic" discovery was silently LE. The classic code IS
shipped (libbredr_app.a, 551 symbols, linked and initialized) - but
Espressif's own S31 A2DP example in IDF master uses
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY with BLE off, and in that mode the
feature mask comes back honest (0xf8 -> 0x98, SSP host bits present),
bluetoothd (ControllerMode=bredr in /etc/bluetooth/main.conf on the card)
adopts the adapter, and inquiry runs. Classic-only loader is also 98 KB
SMALLER than the old BLE-only one (1,523,376 bytes; 508 KB factory
headroom back).

Traded away, deliberately: BLE (so BLE-HID/HOG waits; note the unresolved
smp.c "security requested but not available" for whenever LE returns, and
that CONFIG_UHID is still off). Combined BTDM's dishonest feature mask is
worth re-testing on future IDF drops - the blob may mature. Pairing the
soundcore Liberty 4 NC awaits the next session; the sequence that should
now work: scan on, pair C0:BB:2D:C8:DF:A2, trust, connect, then
aplay -D bluealsa:DEV=C0:BB:2D:C8:DF:A2 through s31-play.

### IDF bumped to master; dual-mode BT restored by the new blob (2026-09-01)

ESP_IDF_REF went a602e67b (June 17) -> 2067f3ae (Aug 28) in both
Dockerfiles; only the IDF layers rebuild (apt cached). The window carried
the whole BR/EDR maturation (LMP negotiation blocking, ACL underflow
deadlock, memory-safety/DoS fixes, controller deep-review), the S31
DFS clk_tree refcount fix, a Wi-Fi beacon timestamp workaround, a PHY lib
update, and PSRAM/PMP permission hardening. Fallout fixed on the way: the
new esp gcc 16.1 dropped `-mespv-spec=2p2` (the spec now rides only in the
-march `_xespv2p2` suffix - flag removed from bootloader cmake), and the
stale build cache pointed at the old toolchain path (rm -rf
bootloader/build). The BSP/Korvo repo has no changes since June; esp-hosted
upstream nothing consumable.

With the new blob, CONFIG_BTDM_CTRL_MODE_BTDM reports HONESTLY:
features[4] 0xf8 -> 0xd8 (BR/EDR-not-supported clear, LE set, SSP host
bits present), bluetoothd adopts the adapter. Dual-mode loader is
1,757,504 bytes (274 KB headroom). The ControllerMode=bredr policy was
then REMOVED (/etc/bluetooth/main.conf deleted): with an honest
controller both bearers run - classic page 0xd8, LE feature page live,
central+peripheral roles. For classic pairing of dual-mode devices, use a
bredr transport filter in the bluetoothctl session so discovery creates
BR/EDR-native records rather than chasing a device's Fast-Pair LE side. udevd stays on SD: measured
164 KB RSS there vs 28 KB in XIP, but the pages are clean and evictable
and no comfortable partition geometry brings it back (xip2 free 57 KB vs
udevd 264 KB; thinning factory below ~140 KB headroom is imprudent when
the controller blob swings hundreds of KB between drops).

## Hosted Wi-Fi throughput: the gap was power save, not the transport (2026-09-01)

The question was why Linux TCP saw ~300 KB/s when hart0's raw-injection test
had measured 25.8 Mbit/s. Answer: **esp_wifi was running WIFI_PS_MIN_MODEM**,
and nothing in the current stack ever turned it off.

**The fix lives in `esp-hosted-fg/slave/main/slave_wifi_std.c`** (our fork):
`esp_wifi_set_ps(WIFI_PS_NONE)` + readback log on every STA_CONNECTED. Two
earlier attempts went into `bootloader/main/hosted_wifi.c` and did nothing -
**that file is dead code**: `CONFIG_S31_HOSTED_WIFI_ENABLE` is not set and
Wi-Fi association is owned end-to-end by the esp-hosted slave RPC path
(`slave_wifi_std`), driven from the Linux driver. The tell was hart0's log
saying `slave_wifi_std: Sta mode connected` and `wifi:pm start, type: 1`
(1 = MIN_MODEM) while hosted_wifi.c's own log line never appeared. After the
fix the same line reads `type: 0` and the slave logs
`power save forced off, readback=0`.

Numbers (16 MB HTTP download from a host on the same 2.4 GHz BSS, x3 runs):

    before   267 / 326 / 290 KB/s   ping RTT 13-135 ms, 400-900 ms comas
    after    681 / 650 / 690 KB/s   ping RTT 5.3/7.2/16.4 ms (min/avg/max)

The power-save signature, for next time: host->board ping shows a descending
staircase (613, 509, 398, 293, 182, 73 ms...) - the AP's queue draining after
the STA wakes. RSSI was -60 dBm throughout; signal was never the problem.
Also eliminated by measurement: Bluetooth coex (powering hci0 off changed
nothing), hart0 DFS (pinning the cpufreq floor changed nothing), TCP loss
(zero retransmits in the window-limited runs).

**What the path can actually carry** (UDP, measured with `rootfs/udpblast.c`
on the board and a python receiver/sender on the host):

    host->board flood     3.4-3.7 MB/s delivered at wlan0, near-zero drops
                          (but NO listening socket - kernel-only cost)
    board->host 16 KB     ~1.45 MB/s end-to-end (511/512 datagrams arrived)
    board->host 1400 B    ~385 KB/s - and the same blast against LOOPBACK
                          does ~237 KB/s. The radio is not the TX limit.

**The remaining wall is hart1 CPU per syscall/packet**, consistent with the
old af_unix measurement (read ~1.3 ms / write ~2.1 ms): a blocking 1400 B
sendto costs ~3.6-5.8 ms wall regardless of destination, and 16 KB datagrams
get 10x the bytes for the same call count. wget uses only ~27% CPU during a
download; the rest of the per-packet cost is kernel-side.

**Measured and rejected: `.text..fast` on the network spine.** 42 functions -
socket syscall layer, skb alloc/free, netif/NAPI/GRO core, IPv4 rx/tx, TCP
fast paths, UDP, and the s31-hosted driver's poll/xmit/irq (~70 KB of the
275 KB section) - moved to RAM and verified at 0xc08xxxxx in System.map.
Result: loopback 237->219 KB/s, Wi-Fi blast 385->354, TCP downloads
identical. Reverted. The arithmetic says why: 5.8 ms at 320 MHz is ~2M
cycles per sendto - no 6x fetch penalty on a ~30k-instruction path explains
that; the cost is structural (somewhere in entry/wakeup/softirq scheduling,
not in the flash residency of the stack bodies). Finding those cycles is a
separate investigation - see the "socket syscalls cost ms" memory.

Practical guidance that follows from the numbers:
- Transfers to/from the board should use big buffers per syscall (wget and
  curl already do; anything hand-rolled should copy >=16 KB per call).
- `tcp_rmem` stays at the 131072 default: post-fix BDP is ~5 KB, the window
  is nowhere near binding, and a 1 MB bump measured no better.
- Offered load far above ~3 MB/s collapses delivery to ~zero rather than
  shedding gracefully (both before and after the fix) - do not stream
  blindly at the board.
- Do not diagnose air problems from ping alone: the Mac's own power save
  pollutes board->Mac RTT, and the gateway deprioritises ICMP. Calibrate
  with a second station before blaming the link.

## xfiles right-click, the stub census, and a desktop-killing SIGPIPE (2026-09-01)

**xfiles has no built-in context menu by design.** Right-click spawns an
external `xfilesctl menu <files>` script (upstream README); we ship none, so
the click is delivered (verified: `posix_spawnp: No such file or directory`
in xfiles' stderr) and nothing visible happens. Opening a FILE has the same
shape: xfiles execs `$OPENER` (default xdg-open), also not shipped -
double-click on a folder navigates, double-click on a file silently fails.
Every file *operation* (menu, open, delete, drop actions) funnels through
these missing scripts, so a future context menu means shipping an xfilesctl
(recommended: lvdesk-native menu over the ctl socket + busybox file ops, not
an xmenu port that would need real grab semantics in xlite).

**Stub census under a fully VERIFIED interactive session** (window position
moves between launches - every aim was re-derived from a screenshot and each
interaction confirmed to land: selection underline, directory change):
click-select, folder open, keyboard nav, rubber-band, icon-drag-onto-folder,
right-click hit 4 of xlite's stubs, 6 calls total:
    XWindowEvent x3        ICCCM gettimestamp() for selection timestamps
    XSetSelectionOwner x1  claiming PRIMARY for the selected files
    XGetSelectionOwner x1  ditto
    XUngrabPointer x1      after right-click
None affect anything visible: the selection machinery only matters when
another client pastes (none here), and grabs are no-ops on a single-seat
shim. The other 26 stubbed functions xfiles links (fontset fallback,
thumbnails via XInitImage, focus/reparent WM calls, error-text formatting)
stayed dormant. Two real gaps, neither stub-caused: no opener, no xfilesctl.
In-window drag-move of a file onto a folder does not complete (ctrldnd's
event loop hits the stubbed selection/grab calls and bails) - but even
completed it would only call the missing xfilesctl.

**The desktop died silently during this testing - SIGPIPE, now fixed.**
`killall xfiles` after a right-click left queued server events; the next
write() to the dead client socket raised SIGPIPE, whose default disposition
killed lvdesk with no log line and no kernel record. out_flush() always
handled write failure correctly, but that path was unreachable until
`signal(SIGPIPE, SIG_IGN)` (now in lvdesk main). Every x11sweep's killall
had been rolling the same dice. Verified: the same kill pattern now leaves
the desktop standing.

## xfiles context menu and opener - lvdesk-native, shipped (2026-09-01)

Right-click in xfiles now works end to end with zero new resident RAM:

    xfiles --spawns--> /usr/bin/xfilesctl (busybox sh, XIP)
      --ctl fifo--> lvdesk draws a NATIVE LVGL menu at the pointer
      --reply fifo--> script runs the action with busybox tools

- **lvdesk ctl grew `menu` and `run`.** `menu <replyfifo> <a>|<b>|...`
  pops a popover-styled list at the pointer (clamped on-screen, light
  dismissal via the existing scrim); the chosen label - or an empty line on
  dismissal, sent from popover_close() so every path answers exactly once -
  goes back on the caller's fifo. `run <cmd>` types a line into the built-in
  terminal's shell and raises the window; it is how "Open" views files
  (busybox less) and "Properties" shows listings, since the desktop has no
  standalone viewer. The client side is PURE SHELL: mkfifo + `exec 3<>` (no
  blocking open) + `read -t 30`. No lvmenu binary was needed.
- **The ctl fifo is now line-parsed and in the main poll set.** It was
  read with a fixed buffer (stream splits/merges corrupted commands) and
  NEVER polled - a ctl command sat unread until some other fd or timer woke
  the loop, which is why every scripted `raise` always felt laggy and menu
  actions ran seconds late. The write itself is now the wakeup; actions are
  instant (click -> less running measured under 1 s).
- **The terminal is a login shell now** (`argv[0] "-sh"`): /etc/profile is
  read, which is where PATH and OPENER come from. `OPENER=s31-open` is in
  the overlay's /etc/profile AND appended to the live card's (the card
  predates the overlay change - the usual SD-state trap).
- **Menu v1 actions**: file selection -> Open / Delete (with a confirm
  submenu) / Properties; empty space -> Terminal here / New folder /
  Properties. Rename and free-text prompts wait for a native text-entry
  popover (the wifi password machinery is the natural donor).
- **Staging trap, cost an iteration**: /usr/bin IS the XIP overlay, so a
  new overlay script must ALSO be in XIP_ROOTS in the Makefile or xip-fast
  packs an image without it and /usr/bin hides the ext4 copy. Relatedly,
  /bin is read-only at runtime too (busybox is in XIP_ROOTS) - the old
  note that "/bin is not overlaid" is stale; SD-test deploys go in /root.
- RAM: menu = a dozen transient LVGL objects from the existing pool;
  scripts are XIP (zero RSS); busybox does the file ops. Nothing resident.

## Hardware thumbnails: JPEG decode + PPA scale into xfiles (2026-09-01)

xfiles thumbnails now come from the codec: `xfilesthumb` IS `s31-thumb`
(a symlink - the shell wrapper it briefly hid behind cost one ~0.35 s
process spawn per file, which was half the per-thumbnail time; the binary
gates on JPEG magic bytes instead of filenames, reading 4 bytes before the
bulk). It feeds the JPEG through DRM_ESP32S31_JPEG_THUMB - hardware
decode, then PPA SRM scale, one ioctl.
Board-generated captures AND the first frame of any mjpeg (the tool bounds
the feed at the first EOI) thumbnail correctly; progressive, grayscale and
anything decoding over 1280x960 (a 2.4 MB transient CMA cap, not a codec
limit) exit nonzero and keep the generic icon. Enabled by
XDG_CACHE_HOME=/root/.cache in /etc/profile (overlay AND the live card);
the PPM cache persists on SD, so revisits are instant. Measured (amortised
over 10 runs - per-run $() timing brackets cost more than the tool and
inflated every earlier number): **135 ms per warm thumbnail, 81 ms of which
is the bare spawn floor** (fork+exec+exit of the 2.4 KB freestanding
binary from a busybox shell). The work itself is ~54 ms: ioctl ~30
(persistent decode buffer, /dev/s31-jpeg misc device instead of the
64-194 ms DRM open), SD read + single-write PPM the rest. The decode is
~8 ms, the scale ~2 ms. Correction from the same round: a TRIVIAL syscall
is ~5 us here - the ms-class costs are subsystem-specific (sockets,
spawns, DRM open, CMA), not the entry path. Below ~80 ms means a resident
daemon (rejected: RAM). Decode register lore lives in
patches/0023 (OUT descriptors take their length from HB/HA; DQT_INFO is a
table-id map; RX REORDER rebuilds rasters; completion is DCT_DONE).

**The medium was the harder half: xlite was not thread-safe, and xfiles
draws thumbnails from a worker thread.** Three separate failures stacked:

- The widget composes BGRA32 client images against DefaultDepth - correct
  on the 24-bit servers upstream targets, undefined on our 16-bit one.
  xlite now recognises that caller shape (depth<=16, pad 32, no stride),
  carries the image as 32bpp and declares depth 24 on the wire, where
  xshim's PutImage already converts BGRA to RGB565.
- Both threads build requests in place in one output buffer. A flush from
  one thread mid-fill of the other's tore requests: the thumbnail pixels
  reached the layer pixmap and the commitdraw after them vanished. xlite
  now holds a recursive output lock from xlite_req() to xlite_send() -
  XInitThreads() had been returning success all along.
- The reply path was unlocked, so the worker's first AllocColor reply was
  eaten by the main thread's blocking event read and the worker hung
  forever - which also silently stopped thumbnail GENERATION after one
  file. Replies now hold the lock end-to-end; the event waits drain
  nonblockingly under the lock and sleep in poll() outside it (100 ms cap,
  or a worker's queued events would wait for the next input event).

Debug leverage that made this tractable: XSHIM_TRACE (existing) shows every
request with resource types; XSHIM_IMGDBG (new) logs PutImage geometry and
clip state. The stuck-client signature - a log that simply STOPS after a
worker-thread request - is the reply-eaten deadlock.

## The 2026-09-01 "everything got slow" triage (measured, closed)

The complaint: +10 s boot, −1 MB RAM, laggy desktop, all appearing the day
context menus and JPEG thumbnails landed. All three were measured; none of
the three is the thumbnail or menu code.

- **Boot +10 s is the service tail, not the desktop.** lvdesk still starts
  at 27.7 s from reset - unchanged. rcS now runs to 48.9 s because
  S40network takes 14.7 s and the Bluetooth-week services (dbus,
  bluetoothd, bluealsa, crond) queue behind it. The desktop is usable long
  before rcS finishes; the "slower boot" is the login prompt, not the UI.
- **RAM −1.1 MB is the daemon stack, itemised by killing services one at a
  time:** bluealsa 256 KB + bluetoothd 372 KB + dbus ~180 KB + keylog,
  crond, udevd ~596 KB. With all of them stopped the no-apps desktop is
  back at 2400 KB free. CmaFree 16 KB is *healthy* - CMA is movable page
  cache and reclaims on demand; do not read it as exhaustion. The 915 KB
  persistent decode buffers were also reverted to transient allocs.
- **Input lag: thumbnails are exonerated by fresh-boot A/B.** Four
  fresh-boot arms (uinject demo load, repaired in_lag instrument):
  thumbs ON avg 81 / 249 ms, OFF avg 140 / 525 ms - ON is not worse, and
  run-to-run variance (3x between identical arms) dwarfs any toggle
  effect. Baseline without xfiles: avg 64 ms. The cost driver is having an
  X client on screen at all (motion forwarding through xshim's ~2 ms
  socket writes), plus intermittent multi-hundred-ms stalls that hit some
  boots and not others - that is the open lead, not the thumbnailer.
  Same-boot arm pairs are worthless here: ON 211→614 / OFF 285→781 in
  sequence position, i.e. the second arm always loses.
- **The in_lag tray metric had been broken since birth**: it compared
  kernel input timestamps (CLOCK_REALTIME by default) against lv_tick's
  loop-accumulated ms. Fixed with EVIOCSCLOCKID(CLOCK_MONOTONIC) at mouse
  open and clock_gettime(CLOCK_MONOTONIC) at read. Historical in_lag
  numbers predating this are garbage.

## Concurrent JPEG encode + decode wedges the SoC (guarded)

Running the thumbnail decoder while mjpegrec's encoder is active
reproducibly wedged the chip - full serial silence, needing two resets
(the first reset after a wedge stalls before hart1; the second works).
Decode alone: 20/20 clean. A drain-to-idle on the decode exit path
(DCT_DONE wait + INLINK/OUTLINK_STOP) reduced but did not eliminate it.
Without a TRM the failing hand-off between the shared codec/DMA2D state
machines is not attributable, so the driver now refuses decode with
-EBUSY while the recorder runs (s31-thumb falls back to the generic
icon). Verified: decode during recording returns EBUSY, no wedge.

## Shim libraries can never fall back to stock again

Stale upstream X11 libraries were found on the SD *under* the XIP
overlay - so any overlay failure silently demoted every client to the
fat stock libs instead of failing. The live card is cleaned (12/12 shim
under overlay) and post-build.sh now deletes any target/usr/lib copy of
the replaced libraries that differs from the overlay's shim, so a
re-image can't reintroduce them. Policy: shims live in XIP only; a
missing shim must fail loudly.

## lvdesk is NOT pinned - mlockall measured WORSE and was reverted

**2026-09-02: the section below is kept for the reasoning, but its
conclusion was wrong and the pin is gone.** Fresh-boot arms (xfiles up,
uinject demo load, repaired in_lag) with the identical binary and kernel:

| lvdesk        | avg lag        | worst          |
|---------------|----------------|----------------|
| mlockall on   | 367, 411 ms    | 1163, 1270 ms  |
| mlockall off  | 170, 313 ms    | 610, 996 ms    |
| off, shipped  | 192 ms         | 582 ms         |

The arithmetic that should have been done first: the paging the pin
prevented was 12 major faults in 13 minutes, about 38 ms of SD round
trips *in total*, against an input latency averaging hundreds of ms. The
harm is MCL_FUTURE combined with xshim living inside lvdesk - every
client pixmap the shim allocates (618, 539, 309, 254 KB for a single
xfiles window) becomes unevictable for the desktop's lifetime, so the
pressure lands on the clients, which is exactly what the user waits for.
Memory is the binding constraint: do not trade MB for ms here.

## Superseded reasoning: lvdesk is pinned - the compositor can never page (2026-09-01)

mlockall(MCL_CURRENT | MCL_FUTURE) at the top of main (LVDESK_NO_MLOCK
opts out). Before: 13 minutes of uptime had already swapped 60 KB of the
compositor's heap to SD and charged it 12 major faults - each one a
~3.2 ms SD round trip taken while moving the pointer or opening a menu.
After: VmLck == VmRSS (1252 KB idle, ~2060 KB with a full-screen client,
because MCL_FUTURE locks client surfaces as xshim allocates them), VmSwap
0, and under xfiles + two uinject demo passes majflt stayed exactly flat
and pgscan_direct did not move. Locks do not survive fork, so terminal
children are not pinned.

The honest cost: ~1.25 MB stays resident instead of being evicted. The
old "lvdesk is only 212 KB RSS" was the *symptom* of the problem - the
desktop had been paged out and re-faulted on demand. Clients can still
page (that is what swap is for); the compositor, cursor, menus and tray
cannot.

Next knob if foreground stalls reappear: min_free_kbytes (currently
1024) - pgscan_direct was nonzero after long uptimes, and raising the
watermark moves reclaim onto kswapd instead of the allocating thread.
Runtime-tunable, A/B before adopting.

## bluealsa was eating the whole machine (2026-09-02)

**`bluealsa` busy-waits at ~92% of the single core with no Bluetooth
device connected, nothing playing, and a clean log.** The spinning
thread is glib's `pool-spawner` (8817 ticks against 151 for the main
thread), so this is glib's thread pool under musl, not Bluetooth work.

Measured, idle desktop, fresh boot, 10 s windows:

| | system idle | lvdesk input lag |
|---|---|---|
| bluealsa running | **0 ticks of 1000** | 170-411 ms avg, 582-1270 worst |
| bluealsa stopped | 388 of 600 (65%) | **25 ms avg, 159 ms worst** |

This is the real cause of "the desktop got slow during Bluetooth week" -
not thumbnails, not the context menu, not the kernel. It also explains
why the earlier lag A/B arms were so noisy and why thumbnails ON kept
beating OFF: every arm was competing with a spinning daemon for the only
core, so the arms were measuring scheduler luck.

`S47bluealsa` no longer autostarts. `touch /etc/s31-btaudio-on` enables
it for Bluetooth audio; the glib spin is worth fixing at source before
that becomes the default again.

**Method note:** this was found by per-process CPU accounting
(`/proc/<pid>/stat` utime+stime deltas over a fixed window) during real
interaction, after the xshim request profile showed requests that draw
NOTHING - QueryPictFormats, a 140-byte static reply - "taking" 20-847 ms.
Wall time in a handler on a one-core box is not the handler's cost. The
profiler now records CLOCK_THREAD_CPUTIME_ID alongside wall time so the
two can never be confused again.

## Spawn cost: dynamic linking is ~20 ms of every exec (2026-09-02)

`rootfs/spawnbench.c` splits a process spawn into its parts:

| | cost |
|---|---|
| fork + exit | 13 ms |
| vfork + exit | 12.6 ms (vfork buys nothing - fork is not the problem) |
| exec, static 258 KB binary | 14 ms |
| exec, dynamic busybox 734 KB | 52 ms |

Size is not the variable; the `ld.so` resolve is. busybox is the shell
behind every init script, every helper the desktop spawns and every
applet in a pipeline, and **boot forks 301 times**. Building it static
(`CONFIG_STATIC=y` in busybox.fragment) took a spawn from 53.6 ms to
29.4 ms and boot from rcS-ends-48.9 s to 40.5 s, with the desktop
starting at 25.2 s instead of 27.7 s.

It costs 582 KB of flash, paid for by moving `bluetoothd` and
`libglib` out of the XIP image - Bluetooth is opt-in now, so those pages
cost nothing at all until `/etc/s31-bluetooth-on` exists. The XIP image
went from 233 KB OVER its partition to 2.18 MB free.

**Bluetooth is now opt-in:** `touch /etc/s31-bluetooth-on && reboot`
starts bluetoothd and bluealsa. Off, they cost no CPU, no RAM and no
flash; on, bluetoothd runs from the SD lower layer (~800 KB RSS).

## Where the desktop's time goes now (2026-09-02, end of day)

Measured on a fresh boot with the realistic `uinject session` load
(one uinput device for the whole run - respawning uinject per action
makes the desktop's inotify rescan charge the input path 1.4-1.8 s per
5 s window that no user would ever pay):

| | before today | now |
|---|---|---|
| input lag, client up | 170-411 ms avg | **15 ms avg**, worst 190 |
| system idle, idle desktop | 0% | 38-65% |
| MemFree | 1892 kB | 3264 kB |
| process spawn | 53.6 ms | 29.0 ms |
| desktop starts | 27.7 s | 25.2 s |
| rcS ends | 48.9 s | 40.4 s |
| xfiles launch | 11.4 s | 5.0 s |

**Still slow, and where it actually goes:**

- **Maximise/restore xfiles: ~2.6 s** (`rootfs/resizebench.c` sends the
  ctl command and samples both processes' CPU until they go quiet). The
  split is the finding: **lvdesk 140 ticks against the client's 20** -
  seven eighths of a resize is ours. One maximise paints 6.1 M fill
  pixels and 1.5 M composite pixels, about 20 screenfuls, and the fills
  arrive as single large rectangles (798x410 into a 768x515 pixmap),
  not as many small ones.
- **One xlite-SHM GetPixmapFd costs 86-355 ms** (once 1347 ms) inside a
  resize: `px_share()` allocates a memfd, mmaps it and copies 733 KB.
  MAP_POPULATE was tried and reverted (3149/3623 ms against 2549/2722) -
  prefaulting moves the page cost, it does not remove it. The fix has to
  avoid the allocate-and-copy, e.g. by backing large pixmaps with a
  memfd from creation so sharing is free.
- **xfiles' own CPU is 2.1 s of a 5.0 s launch** against xclock's 0.56 s,
  so most of what remains in launch is the client, not the desktop.

**Profiling method that found all of this:** per-process CPU accounting
over a fixed window during the real interaction, plus xshim's request
profiler now recording pixels touched and CLOCK_THREAD_CPUTIME_ID next
to wall time, plus per-phase WORST-single-visit timers in lvdesk's loop.
Averages hid every one of these; the worst-visit line is what exposed
the input path stalling for 600-950 ms at a time.

## Bluetooth: what is actually possible here (2026-09-02)

**A2DP cannot be carved out to hart0 while Linux keeps HID.** The hosted
Bluetooth is a VHCI transport: hart0 runs the *controller*, Linux runs
the *host* stack (bluez). ESP-IDF's A2DP source is part of Bluedroid,
which is itself a host stack - so putting it on hart0 means two host
stacks contending for one controller over one HCI, which is not a thing
a controller supports. It is one or the other:

- Linux is the host (today): bluez gives HID, pairing and SDP natively,
  and audio needs a userspace A2DP source on Linux.
- hart0 is the host: Bluedroid owns everything, and every profile Linux
  wants - HID especially - has to be re-exported to Linux over a custom
  channel. That is a large amount of work to keep one keyboard working.

So the plan is to keep bluez and **replace bluealsa**, which is the only
piece that misbehaves: `bluetoothd` measured 0% CPU, while bluealsa's
glib `pool-spawner` thread busy-waits on 92% of the core. bluez is not
the problem and a lighter Bluetooth *stack* would not fix anything.

A minimal A2DP source is a bounded job: register an endpoint on bluez's
Media API over D-Bus, take the transport fd it hands back, encode SBC,
write. One direction, one codec, no glib - the same reasoning that
produced xlite. bluealsa is general-purpose (sink and source, HFP,
multiple codecs) and we use one corner of it.

**Coexistence is still unexplained.** SW coex IS enabled
(CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y in the built loader config), and
the Wi-Fi power-save hypothesis was tested and NOT supported - see the
comment in esp-hosted-fg slave_wifi_std.c. Paging still times out with
wlan0 up. Separately, the SoC hard-wedged three times during BT connect
attempts with Wi-Fi up, hart0 logging
`OLC: r_olc_page_recycle_sch resched failed` - a controller-level
scheduling failure that needs its own investigation before Bluetooth is
trustworthy alongside Wi-Fi.

## Thumbnails are enabled and mostly work; two real defects

Reported as "all black". They are not disabled (`s31-thumbs status` says
on, no kill file) and most images decode: of six JPEGs, four produced
real pixels and rendered. The defects are:

- **`red.jpg` (903 bytes) fails to decode**, driver error 0x8000, ret=-5.
  s31-thumb correctly writes no file on failure.
- **A missing thumbnail renders as an EMPTY CELL**, not the generic image
  icon, so a failed or not-yet-generated thumbnail looks like a blank.
  That is the visible complaint and it is worth fixing at the xfiles
  fallback rather than by faking a placeholder PPM.

**Correction to an earlier claim:** thumbnails ON did measure "faster"
than OFF, but that was scheduler noise from bluealsa spinning on the
core, not a real effect. Decoding an image is not cheaper than not
decoding it; the honest statement is that the thumbnail path is fast
enough not to matter, at ~160-220 ms per image of which ~56 ms is the
process spawn.

## s31-a2dp: our own A2DP source (2026-09-02)

`rootfs/s31-a2dp.c`, 496 lines, libdbus + libsbc, **no glib**. Registers
an A2DP *source* endpoint on bluez's Media API, lets bluez negotiate SBC
with the sink, acquires the transport fd and writes RTP-framed SBC.

**Idle cost: 3 ticks in 5 s (0.6% of a core) against bluealsa's ~460
(92%).** That is the whole point of the exercise.

Status 2026-09-02: **WORKS, CONFIRMED BY EAR.** 345 packets, no errors,
the whole 8 s file to the soundcore Liberty 4 NC, and a human heard the
four-note arpeggio correctly - C4/E4/G4/C5, distinct notes, clean
envelope, stereo audible. BlueALSA is no longer needed for playback.

Known cosmetic fault in the TEST FILE, not the path: it is 44.1 kHz sent
over a transport negotiated at 48 kHz, so it plays ~9% sharp. Either
resample, or offer 44.1 kHz first and hold the peer to it.

Costs: **0.6% of a core registered and idle** (3 ticks in 5 s, against
bluealsa's ~460) and **43% while actually encoding** (216 ticks in 5 s)
for 48 kHz joint-stereo SBC at bitpool 43. That is the right shape - pay
only while playing - but the encode itself is worth optimising: a lower
bitpool or 44.1 kHz would cut it.

Two things this cost, both worth knowing:

- **BlueZ 5.79 may never call SetConfiguration on your endpoint.** When
  the sink is already configured, the stream appears under the REMOTE
  SEP at `.../devXX/sepN/fdN` and the application has to go and find it.
  Waiting for SetConfiguration alone left us registered and idle beside
  a perfectly good SBC transport. s31-a2dp now also walks ObjectManager
  for anything implementing MediaTransport1 and reads the negotiated
  blob from the transport's own Configuration property.
- **`sbc_encode()` returns INPUT bytes consumed, not output length** -
  the written size comes back through its last argument. Using the
  return value advanced the packet by 512 bytes a frame instead of 77,
  so every write overran the L2CAP MTU with EMSGSIZE and the sink heard
  nothing. The symptom looks like an MTU problem and is an API problem.

**Trap found doing this: XIP_SKIP exposes the SD copy, and the SD copy
can be STALE rather than missing.** Moving bluetoothd out of the XIP
image made the system run `/usr/libexec/bluetooth/bluetoothd` from the
card - an **Aug 23 build, 797,012 bytes, with zero "a2dp" strings** -
while the current build is 1,224,572 bytes with the plugin. The symptom
was `org.bluez.Media1` having no `RegisterEndpoint`, i.e. exactly the
failure that the LE-era `-p gap` allow-list used to cause, from a
completely different cause. Checking that the file EXISTS on the SD
lower layer is not enough; check that it is the file you just built.
The card's copy has been refreshed.

## 256 KB moved from the linux partition to rootfs (2026-09-02)

Evicting bluetoothd from XIP to pay for the static busybox was a bad
trade and is reverted. It put the daemon on the card, where an **Aug 23
copy without the a2dp plugin** was still sitting, and A2DP broke in a way
that looked exactly like the old LE-only `-p gap` bug from a completely
different cause. Anything that RUNS belongs in flash.

The space came from dead slack: with debugfs compiled out the kernel is
5,894,105 bytes in what was a 6,422,528 byte partition. 256 KB moved
across, so now:

    linux    0x400000  0x5E0000   kernel 5,894,105, slack 266,279
    rootfs   0x9E0000  0x620000   XIP image 6,221,824, free 200,704

**Geometry lives in THREE places and all three must agree**:
`bootloader/partitions.csv`, `LINUX_PARTITION_SIZE`/
`ROOTFS_PARTITION_SIZE` in the Makefile, and the constants in
`bootloader/main/main.c` - which appear TWICE there. The loader validates
the flashed table against its constants and refuses to boot on a
mismatch, reporting "Linux partition not found", which reads like a
missing partition rather than a size disagreement. The linux OFFSET did
not change, so core1_trampoline.S was untouched.

Now XIP-resident again, all at Rss 0 for their text: bluetoothd (192 kB
RSS total), libglib, and **s31-a2dp (60 kB RSS)**. Only `iw` stays on the
SD lower layer - it is a hand-typed CLI that nothing on the boot or
runtime path calls, and it was that or 40 KB over the partition.

## Why A2DP crackles, measured (2026-09-02)

Per packet (8 SBC frames = 21.3 ms of audio at 48 kHz), streaming to the
Liberty 4 NC:

| | wall | cpu |
|---|---|---|
| fread of PCM | 8.4 ms | |
| sbc_encode | 39.3 ms | **11.6 ms** |
| write to L2CAP | 6.5 ms | |
| **total** | **~54 ms for 21.3 ms of audio** | |

So the stream runs at about 40% of real time and falls behind ~1.3 s
every 5 s, which is what the ear hears as continuous crackling.

**It is NOT coexistence.** The control matters: with Wi-Fi fully DOWN
the lateness is identical (3112 ms at 5 s, 6632 at 10 s). Wi-Fi
association and even a ping flood change it very little. Two earlier
claims in this file - that paging fails with wlan0 up and succeeds
instantly with it down - were also confounded and are corrected below.

**It is not preemption either**, though it looked like it: 28 of the
39 ms in sbc_encode is off-CPU, but SCHED_FIFO priority 5 plus mlockall
changed nothing measurable. The remaining 11.6 ms of CPU per packet is
itself 54% of a core, and rootfs/sbcbench.c says the codec alone should
cost 0.23 ms per frame (1.8 ms per packet) - so encode is running ~6x
slower in the daemon than in the bench and that gap is unexplained.

Levers, in order of expected value:
- **Cut the work**: 44.1 kHz instead of 48 (8%), a lower bitpool, or 4
  subbands. sbcbench measures all of these; 4 subbands was 7.1% against
  8.5% for a whole core of real-time audio.
- **Explain the 6x**: the bench encodes one cache-hot buffer repeatedly
  and the daemon streams fresh data. If that is the whole difference it
  is a memory-bandwidth story, which is a known shape on this board.
- **hart0 offload** is the structural answer but is not simple: the
  L2CAP channel is owned by bluez on Linux, so hart0 cannot inject into
  it without breaking the one-host model.

## Corrections to earlier coexistence claims

- "BT paging times out with wlan0 up and succeeds instantly when down"
  was **confounded** - the earbuds were in pairing mode in the fast
  cases. With an awake, bonded sink: **15-20 s either way**, Wi-Fi up
  costing ~3-4 s and one failure in three. Under a ping flood, 4 of 8
  attempts fail. Wi-Fi hurts paging, but it does not block it.
- `esp_coex_preference_set(ESP_COEX_PREFER_BT)` was tried and made it
  **worse**: 0 of 8 connects against the default's 4 of 8, with a
  control (Wi-Fi down, same firmware) connecting in 16.6 s to prove the
  sink was awake. Reverted; see the note in esp-hosted slave_bt.c.
- We are on the **latest esp-idf master** (2067f3ae, 2026-08-28) - a
  live fetch shows zero commits since, and the recent a2dp/bt fixes are
  already ancestors of it.

## The A2DP bottleneck is the hosted transport's interrupt cost, not SBC

**There is no hardware SBC on this SoC.** Checked: no SBC symbols in the
S31 ROM or its linker scripts, and the only audio peripherals in
`soc/esp32s31/register/soc/` are `dac_*` and `i2s_*`. The JPEG codec is
the sole media accelerator. `SOC_BLE_AUDIO_SUPPORTED` is LE Audio
*protocol* support in the controller, not a codec block. SBC on this
family is always software - Bluedroid ships its own encoder for the ESP
side, and we use libsbc on the Linux side.

**And the codec is not the problem.** The same sbcbench run, identical
work, measured:

| | cost |
|---|---|
| system quiet | 453 ms CPU per 5 s of audio = **9.1% of a core** |
| Bluetooth link streaming | 1383 ms = **27.7% of a core** |

Three times the CPU for byte-identical work. The cause is interrupt
load, counted directly on the hosted-transport IRQ (21):

| | interrupts |
|---|---|
| BT connected, idle | 34 in 10 s = **3/s** |
| streaming A2DP | 479 in 10 s = **47/s** |

That is one interrupt per A2DP packet, and the arithmetic closes: the
bench lost 930 ms over 5 s = 186 ms/s, against 47 IRQ/s, so **each
hosted-transport interrupt costs ~4 ms**. At 47/s that is ~19% of the
core stolen from every process on the machine, which is exactly the
inflation measured, and it is why the encoder "takes" 11.6 ms of CPU per
packet when the codec itself needs 1.8 ms.

**So the lever is the transport, not the codec**: fewer interrupts per
unit of audio (batch several ACL packets per doorbell) or a cheaper
handler (`esp32s31-hosted-sram.c` is ours; its hot path runs from XIP
flash at ~6x). Hardware SBC would not have helped even if it existed -
the packets, and therefore the interrupts, would be identical.

## A2DP alongside Wi-Fi: FIXED - tell coex that A2DP is streaming (2026-09-02)

**The fix is one status bit, set at the right moment.** ESP-IDF's
coexistence scheduler time-slices the radio between Wi-Fi and Bluetooth
(period >= 100 ms in the Wi-Fi-connected scheme), and this controller
gives the host only 4 ACL credits, so at most 4 A2DP packets can wait
for BT's slice. Under a Wi-Fi download that starved the stream: btmon
showed ACL transmissions collapsing from ~43/s to 7-20/s with completion
events still tracking them - the controller was completing fewer packets
over the air, not losing credits. IDF's own A2DP source fixes exactly
this by calling `esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT,
ESP_COEX_BT_ST_A2DP_STREAMING)`; with the host stack on Linux, nobody
did.

Now `s31-a2dp` sets that bit through a new hosted control message
(`S31_HOSTED_CTRL_COEX_SET`, ioctl `S31_HOSTED_IOC_COEX` on `/dev/esps0`)
when it acquires the transport, and clears it when it stops. Measured,
same 5.9 MB download while streaming:

| coex hint | Wi-Fi | stream |
|---|---|---|
| A2DP_STREAMING set (daemon) | 245-263 KB/s | **late_max 0 ms, 0 stalls** - every arm |
| cleared, same session | 328 KB/s | late_max 2832 ms, 83 stalls |

Preference (wifi/bt/balance) and scheme interval (x0.5, x2) made no
further difference once the bit was set. The cost is ~22% of Wi-Fi
throughput *while music plays*, which is the trade the arbiter exists
to make.

**Setting the bit at boot does nothing** - tried, ESP_OK, no effect.
The scheme is chosen when the link state changes, so the hint has to
be set while the A2DP link exists. That is why it is the daemon's job.

`s31-coex get|prefer|bt-set|bt-clear|interval|wifi-set|wifi-clear` is
the runtime knob for anything further; a coexistence hypothesis is an
`echo` now, not an eight-minute loader reflash.

**Wi-Fi TX was never the problem**: `udpblast` at 648 KB/s alongside the
stream was already late_max 0 before any of this.

### What else this investigation established

- **musl's `sched_setscheduler()` is an ENOSYS stub**, so the daemon's
  real-time priority never applied until it used the raw syscall. With
  it, per-packet write fell 3.7 -> 0.76 ms and encode 5.9 -> 2.0 ms.
- **bluealsa is gone.** The unified Bluetooth flag had been starting its
  92% spinner alongside bluetoothd, which contaminated a day of
  measurements; the "crackle" root cause was that, not coexistence.
- **The profiling kernel fits with the radios now** (5,968,137 of
  6,160,384 bytes) - `make linux PROF=1` also appends `profile=6`, and
  `scripts/board/resolve-profile.py` resolves a dump. Under a Wi-Fi
  download hart1 was ~38% idle; under Wi-Fi TX it is saturated by the IP
  stack spread thin (`s31_send_payload_meta` 7.2% on top).
- **A full ring on hart0 drops the frame - HCI included**
  (`bootloader/main/hosted_sram.c`, `ring->drops++`). Not implicated in
  this failure (credits tracked), but a flow-control event must never be
  dropped; that is open.
- **The 8BitDo receiver sits behind a full-speed hub** (`214b:7260`),
  the split-transaction case that costs ~55% of the core; 1161 USB
  interrupts/s were present through every measurement here.
- `/tmp` is tmpfs: decoding a btmon capture there wedged the board.
  Write captures to `/root` and stream the decode through awk.

## What A2DP playback costs, and what Wi-Fi adds (2026-09-02)

Real track (collectathon.wav, 85 s, 44.1 kHz stereo, 15 MB on the card),
played through s31-a2dp from XIP to the Liberty 4 NC. Per-process
utime+stime over the window; the `idle` column is /proc/stat's, which
this board under-reports (see s31-cpu-measurement), so trust the
process rows over it.

| arm | idle | s31-a2dp | hci0 kworker | Wi-Fi | stream |
|---|---|---|---|---|---|
| A idle, no playback | 87% | - | - | - | - |
| B music, Wi-Fi idle | 55% | 20.4% | 8.0% | - | 0 ms late, 0 stalls |
| C music + download | 23% | 27.5% | 10.3% | 290 KB/s | 0 ms, 0 stalls |
| D music + upload | 17% | 24.2% | 12.7% | 459 KB/s | 0 ms, 0 stalls |
| E music + both | 24% | 31.0% | 15.5% | 257 + 120 KB/s | 0 ms, 0 stalls |

**Playback alone is ~30% of the core**: the daemon 20% (codec ~6-9%,
the rest per-packet read/write syscalls and pacing at 43 packets/s) and
the kernel's HCI transmit worker 8%, at 47 hosted interrupts/s. The
per-process figures rise in the Wi-Fi arms because interrupt work lands
on whoever is running; the daemon is not doing more.

**Wi-Fi on top costs Wi-Fi, not audio.** With the coex hint set the
stream never fell behind in any arm. What the hint spends: a download
runs at ~250-290 KB/s instead of ~328, an upload at 459 KB/s instead of
958 (udpblast, 1400 B), and both together at 257 + 120 KB/s. Idle drops
to ~20%, so there is headroom left for the desktop but not a lot.

**Two of today's "wedges" were my own scripts holding the console** - a
`sleep` given a tick count instead of seconds sat in the login shell,
and alive.py then reports "emitting bytes, no prompt". Bound every
network wait in a board script (`wget -T`), never `set --` inside a
function that still needs its arguments, and read alive.py's line dump
before calling a board dead.

## Motion batching, memfd-born surfaces, and the bitpool knob (2026-09-02)

**One socket flush per loop pass, not per MotionNotify.** `xshim_pointer()`
flushed the client after every motion event, and a socket write is 1-6 ms
here. Buttons still flush immediately; motion now waits for
`xshim_flush()`, which lvdesk calls right before it sleeps in `poll()`,
so nothing is delayed past the point the desktop yields the CPU. Same
25 s `uinject session`, same day, xfiles up:

| | lag avg | worst | xshim writes | write time |
|---|---|---|---|---|
| before | 24 ms | 360 ms | 3070 | 844 ms |
| after | **6 ms** | 163 ms | 650 | 280 ms |

**Surfaces over 64 KB are born as memfds.** A client loading pixels via
xlite-SHM asks for the fd behind a drawable, and `px_share()` answered by
creating one and copying the surface into it - 733 KB per maximised
window, two such requests measured **292 ms of a 3.0 s maximise**. With
`px_alloc()` allocating anything over 64 KB as a memfd from the start the
same two requests cost **11 ms**. The maximise overall did not move
clearly (2.9 / 2.0 / 5.0 s across three runs against 3.0 before): during
a maximise we send exactly one Configure, one Expose and one Map, and
the ~100 requests that follow are xfiles repainting its icon cells -
legitimate work, each paying the ~2 ms flash-fetch floor per request.
That floor is the profile-guided hot-text item, not a shim change.
Shmem rises accordingly (3.1 MB with xfiles maximised) as AnonPages
falls; the pages were always there, they are just shared now.

**`S31_A2DP_BITPOOL`** picks the SBC bitpool within what the sink
accepted. The codec is 6-9% of a core at any bitpool; what a smaller
frame buys is fewer packets per second inside the sink's 679-byte MTU -
every packet being a socket write, an HCI worker pass and a transport
interrupt. From the frame arithmetic (joint stereo, 8 subbands, 16
blocks: frame = 13 + 2 x bitpool bytes): bitpool 32 -> 8 frames/packet,
43 packets/s; 26 -> 10, 34/s; 20 -> 12, 29/s. NOT yet measured - the
earbuds were asleep when the arms ran - so the daemon default is still
libsbc's choice. The daemon also does its D-Bus housekeeping every 16th
packet instead of every packet.

## Flash is already QIO at 80 MHz - the maximum this IDF supports for the S31 (2026-09-03)

A "flash may be running DIO" hypothesis was raised and is WRONG; do not
re-chase it. The mechanism is exactly the standard ESP-IDF one:

- The ROM boots the 2nd-stage bootloader in the safe mode stamped in its
  header (DIO). Both `bootloader.bin` and `hello_world.bin` headers say
  DIO (`esptool image-info`).
- The 2nd stage is compiled with `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`, and
  `bootloader_flash_config_esp32s31.c:246` is
  `#if CONFIG_ESPTOOLPY_FLASHMODE_QIO || ..._QOUT -> bootloader_enable_qio_mode();`
  - gated on the **Kconfig symbol, not the header byte**. It issues the
  flash chip's Quad-Enable and switches the controller to QIO. hart1's
  XIP reads go through that same controller, so the kernel runs QIO too.
- **Measured** (`rootfs/flashbw.c`, streaming a 1.3 MB XIP-mapped file):
  22-32 MB/s. DIO at 80 MHz cannot exceed ~17 MB/s sustained (2 data
  lines plus per-cache-line command/address/dummy overhead); QIO at
  80 MHz works out to ~30 MB/s. PSRAM streams at ~90 MB/s for the same
  loop, so the XIP penalty is ~3x for sequential reads and ~6x for
  branchy instruction fetch (the earlier .text..fast measurement).

The one genuine oddity is cosmetic: the derived string
`CONFIG_ESPTOOLPY_FLASHMODE="dio"` disagrees with the `_QIO=y` symbol
(sdkconfig was not regenerated after the symbol changed), so esptool
stamps DIO into the app header. Harmless today because the QIO enable
is compiled in regardless; worth regenerating sdkconfig for hygiene.

**The frequency lever is closed in this IDF.** `spi_flash/esp32s31/
Kconfig.flash_freq` offers only 80/40/20 MHz, and there is no
`mspi_timing_tuning/port/esp32s31/` - so despite `soc_caps` advertising
`SOC_MEMSPI_TIMING_TUNING_BY_DQS`, IDF cannot yet train the S31 above
80 MHz. The XIP penalty is inherent at QIO-80; the remaining levers are
hot-text placement and narrower code paths, not the flash mode.

## Reassessment against the record, measured (2026-09-03)

A round of "what is left" that started by reading this file, the plans and
memory, then measuring on the live board rather than proposing. Everything
below is a number taken today; the earlier list that proposed a splash screen
(hart0 has drawn one since day one), starting lvdesk before fbcon (the
handover is deliberate, `docs/native-800x480.md`) and faster flash (already
QIO-80) is withdrawn in full.

### Boot: where 26.4 s to the desktop actually goes

    0.0 -  0.6   kernel core init
    0.6 -  7.6   esp32s31-crypto probe: RSA (2 s), ECC P-256 (2 s) and
                 P-384 (2 s) self-tests ALL time out (-110). Every later
                 probe - LCD, USB, SD - waits behind it.
    7.6 -  8.9   LCD scanout 7.85, USB hub, SD card up at 8.74
    8.9 - 10.3   cramfs /init -> ext4 mount, journal recovery every boot
                 (the board is never unmounted cleanly)
   10.3 - 14.0   overlay /init, busybox init, inittab sysinit, rcS start
   14.0 - 25.3   init scripts: S05xip 2.3, S02sysctl 1.1, S10udevd 1.1,
                 S11modules 0.65 (CONFIG_MODULES is off - a no-op),
                 S30clock 0.6, S01growroot 0.56, S01seedrng 0.4 (the board
                 has a hardware TRNG), syslogd+klogd 0.6
   25.3 - 26.4   lvdesk start -> its own scanout

The crypto self-test is the single largest item and is pure waiting:
`drivers/crypto/esp32s31-crypto.c` polls `RSA_QUERY_IDLE` and `ECC_INT_RAW`
for 2,000,000 us each and the blocks never answer. Nothing in userspace uses
kernel crypto offload (wpa_supplicant carries its own; BlueZ uses software
AES). Turning `CONFIG_CRYPTO_DEV_ESP32S31` off, or fixing whatever clock or
power gate the block needs, is worth ~6.2 s of a 26 s boot.

Console rendering is a real boot cost: with fbcon bound, 100 lines to
`/dev/tty0` take **0.95 s** (0.27 s to the 1 Mbps serial console, 0.15 s to
/dev/null) - ~8 ms per scrolled line, which is one 768 KB framebuffer move
in PSRAM. Boot prints 185 kernel lines (115 of them KERN_INFO) plus ~300 init
lines to both consoles: roughly 2-3 s. `loglevel=5` would keep warnings and
the init progress lines on the panel and drop the driver chatter; that is a
`CONFIG_CMDLINE` rebuild and a taste decision, recorded here rather than made.

### The CPU frequency governor does nothing

`ondemand` is the governor, 53-320 MHz, sampling every 150 ms with a
100 ms declared transition latency. Its `time_in_state` says the CPU was at
**80 MHz for 96%** of a 3.3 h uptime and it made 4,388 transitions. But
`cpuinfo_cur_freq` - the value hart0 actually reports - read **320 MHz on
every sample**, idle, with Wi-Fi disconnected, and with bluetoothd stopped
and hci0 down. hart0 holds the shared CPU clock at maximum (the Wi-Fi
driver in PS_NONE keeps a power-management lock). So there is no 80 MHz
latency trap, and there is no lever here either: the governor's 4,388
"transitions" were hosted control round trips that changed nothing.
`performance` would only remove that traffic. The memory note claiming
hart0 "reclocks APB 160<->240 constantly" is out of date.

### Idle: what wakes the core when nothing is happening

10 s windows on an idle desktop, per `/proc/interrupts` and
`voluntary_ctxt_switches`:

    dwc2 USB            1,055 irq/s   full-speed SOF; physical (hub), known
    riscv-timer           669 irq/s   see below
    esp32s31-lcd           53 irq/s   hardware vblank, counted, not used
                                      (hw_vblank=0, the hrtimer still runs)
    ksoftirqd              91 wake/s
    kworker events_freezable 55/s     the GT1158 touch poll (20 ms)
    kworker events         44/s
    lvdesk                 19/s       2.2% of the core at idle

**The touchscreen poll is 550 of the 669 timer interrupts.** Setting its
`poll` to 0 took riscv-timer from 669 to **118/s**. Each 20 ms poll is two
I2C transfers, and `i2c-esp32s31.c` waits for each with
`readl_poll_timeout(..., 10, ...)` - a 10 us `usleep_range` per iteration,
so ~11 hrtimer expiries and context switches per poll. Tick accounting could
not resolve the CPU cost (49-52 busy ticks per 10 s in every arm), so it is
under ~2%; the fix is interrupt-driven or spin-polled I2C in our own driver,
and a slower poll while nothing is touching. `poll` is writable in sysfs
(max is clamped to 20).

### RAM: the census, so it is not re-argued

    MemTotal 15,428   MemFree 4,788 (idle, no X clients)   Slab 3,992
    AnonPages 1,504   Shmem 752   KernelStack 360   PageTables 348
    "4197K kernel code" in the boot Memory: line is flash, not RAM:
    System.map puts .data+.bss+.text..fast at ~770 KB, matching the
    1,028 K "reserved".

sysfs is 6.7k nodes: `/sys/devices/platform/soc` 2,724,
`/sys/devices/virtual` 1,473, `/sys/firmware` (the device tree) 816,
`/sys/bus` 490, `/sys/module` 239. Nothing dominates; kernfs is 814 KB only
because the board has that many real devices. Closed.

Per-process: udevd 140 KB anon plus 80 KB of `/run/udev`; each long-lived
interactive shell holds a single **280 KB** anonymous mapping (a fresh
`sh -c` is 36 KB, static and dynamic busybox alike - it is something the
interactive shell does, not the static link). Two such shells are 560 KB and
unexplained.

`/var/log/lvdesk.log` had grown to **644 KB of tmpfs** because the card's
S40lvdesk still exported `LVDESK_PROF=1 XSHIM_PROF=1` from the profiling
sessions; removed on the card (the repo copy never had it), log truncated,
lvdesk restarted with "console keyboard off" confirmed.

hart0's internal SRAM heap: 233 KB total, **86 KB free, 84 KB largest
block, 82 KB historical minimum** (`s31-freertos-mem`). That is the budget
for `docs/hot-text-plan.md` phase 3 (kernel hot text in SRAM): ~64 KB with
margin. Temper it: the code already in `.text..fast` is what would move,
and the tick's remaining ~380 us is in inlined flash code that is not.

### The S31 cache has a hardware prefetcher, and it is off - and it cannot be turned on from Linux

`cache_reg.h` for the S31 has per-core instruction-cache **autoload**
(`CACHE_L1_ICACHE1_AUTOLOAD_CTRL_REG` at 0x2C0000FC: ENA bit 0, trigger
miss/hit/both in bits 3-4, two address sections SCT0/SCT1 at 0x100-0x10C)
and a data-cache autoload at 0x110. On the running board every one reads
**0x2** (DONE set, ENA clear) for both harts, and nothing in IDF's S31
startup or our loader enables it; the ROM API is
`Cache_Enable_L1_CORE1_ICache(CACHE_LL_CACHE_AUTOLOAD)` at cache-enable
time. For a kernel that fetches from 80 MHz flash through a 16 KB icache
this is the one SoC feature nobody had tried.

Tried at runtime, twice, with `devmem`: sections set first (flash 16 MB +
PSRAM 16 MB, then kernel partition only), then ENA. **Both times the hart
died the instant ENA was written** - no output at either baud, the
card-resident log ends at the line before the write, `reset.py` recovers it
and the register comes back clear. Do not retry from Linux. If it is tried
again it belongs in the loader, at the point `main.c` prepares hart1's
icache (~line 272), through the ROM call, with a flash-back plan.

### Small items confirmed today

- udev: 27 stock rule files, nothing in our scripts or lvdesk consumes
  them (devtmpfs makes the nodes, lvdesk watches `/dev/input` itself). The
  boot-audit table already measured `udev not run` as the fastest arm
  (rcS 37.1 vs 43.0 s). Removing S10udevd and the no-op S11modules is
  ~1.8 s of rcS, ~220 KB of RAM and the whole nice-19 coldplug.
- The LCD driver's software vblank hrtimer runs at the frame rate forever
  because `hw_vblank` defaults to 0, while the hardware vblank interrupt
  fires 53/s and is only counted. ~42 hrtimer wakeups/s for nothing;
  `/sys/module/esp32s31_lcd/parameters/hw_vblank` is the runtime knob the
  driver comment asks to be confirmed and then switched.
- `page-cluster` is 0 and `read_ahead_kb` 512 on a device whose cost is
  per request (4k 8 ms, 64k 11 ms). Swap is barely used (76 KB), so this is
  moot today and worth revisiting only if swap-ins reappear.
- `CONFIG_SHMEM=y` and `CONFIG_MEMFD_CREATE=y`, so the memfd-born surfaces
  from 2026-09-02 support `fallocate(FALLOC_FL_PUNCH_HOLE)` - a
  whole-surface clear to zero on a memfd pixmap can drop its pages instead
  of writing them, with no NULL px and no munmap sizing, which is where the
  three uniform-pixmap attempts died. The ~705 KB target stands.

## Four of the reassessment's items, taken forward (2026-09-03)

### Boot: the crypto engines had no compute clock - 26.4 s to the desktop is 21.0 s

The RSA/ECC self-test was not a broken block. `CRYPTO_CTRL0` bit 2
(`SEC_CLK_EN`, the compute clock for the RSA and ECC cores) was never set by
`esp32s31-crypto.c`, and hart0's `CONFIG_ESP_CRYPTO_CLK_ON_DEMAND=y` gates
the PLL_F240M reference those cores run from between its own operations, so
`QUERY_CLEAN` answered (bus clock) and `START_MODEXP` never completed. IDF's
own `system_internal.c` documents the same stuck-in-ROM failure mode.

Fix: the driver sets bit 2 (`patches/0025`), and the loader is built with
`CONFIG_ESP_CRYPTO_CLK_ON_DEMAND=n` and hart0's hardware MPI/ECC off
(`bootloader/sdkconfig.defaults`), so the clocks stay on and the engines
belong to Linux. Measured on the next boot:

    kernel scanout        7.85 s  ->  1.68 s
    SD card up            8.74 s  ->  2.22 s
    first rcS script     14.01 s  ->  8.27 s
    S40lvdesk starts     25.33 s  -> 19.84 s
    lvdesk on the panel  26.43 s  -> 21.00 s
    rcS complete         40.86 s  -> 35.32 s

Still open: the self-test now fails *instantly* with -EIO (a result
mismatch) instead of -110, so the probe still does not register the AES/SHA
offload. Nothing in userspace needs it; it is a driver correctness item,
not a boot one.

### Idle: the touch poll no longer owns the timer

`i2c-esp32s31.c` now waits for each transfer on the controller's interrupt
(CLIC 21, already in the device tree, never requested) instead of a 10 us
sleep-poll; `use_irq=0` restores polling at runtime. `gt1158_polled.c`
polls at 20 ms only while touched and backs off to `idle_poll_ms` (60)
after `idle_after` (25) quiet polls. Idle desktop, 10 s windows:

    riscv-timer interrupts/s   669  ->  125-154
    i2c interrupts/s             -       45      (3 per poll at 60 ms)
    busy ticks per 10 s       49-52  ->  50      (unchanged; the cost was
                                                  wakeups, not CPU time)

### RAM: xfiles' two full-window masks are 8 kB instead of 705 kB

The uniform-pixmap idea, done the fourth way. A `struct res` now carries a
`hole` flag: set when a surface is allocated (memfd and calloc pages are
zero until touched) and when a whole-surface clear to zero is turned into
`fallocate(PUNCH_HOLE)` on a memfd-born surface; cleared by every write
path (`op_target_ex`, `blend_px`, the SHM `Damaged` notification) and
carried across alias installs. Two readers honour it: a zero fill into a
hole surface returns without writing (xfiles clears its A8 mask sheets a
cell at a time - 62 partial `FillRectangles` per maximise - and each memset
was refilling the pages), and `Composite` through a hole mask returns for
Over/Add/Atop/OverReverse/Dst. The last part is what makes it stick: a
read fault on shared memory allocates, so the first attempt punched holes
that the compositor immediately refilled (Shmem 2,564 -> 2,488 kB, the
"win" that was not one).

Like for like, xfiles maximised, `/proc/<lvdesk>/smaps` per surface:

                              stock lvdesk    hole-aware
    768x515 A8 mask              388 kB          4 kB
    798x410 A8 mask              320 kB          4 kB
    MemAvailable               1,532 kB      2,440 kB
    Shmem                      3,072 kB      2,364 kB

The census (`SIGUSR1`) no longer walks a hole - walking it would
materialise it - and prints `HOLE (not walked)` instead. Shipped in the
XIP image; the screenshot after the change is pixel-identical in content.

### Kernel hot text in SRAM: the plan, and a finding that came first

hart0 has 86 KB of internal SRAM free (84 KB largest block, 82 KB
historical minimum), so a 64 KB window at `0x2F052000-0x2F062000` (directly
below the audio DMA reservation) is possible with ~18 KB of hart0 margin.
The full plan - loader reservation, a `.text.sram` output section after
`_end` with VMA `0xC1800000`, a static early PTE page plus a late
`create_pgd_mapping` with `PAGE_KERNEL_EXEC`, the bootstrap copy next to
`__copy_data` - is in the session notes and is not started, because
planning it found something cheaper: **the interrupt spine is in flash.**
`entry.o`'s code is `.irqentry.text`, so the `*entry.o(.text .text.*)`
line in `.text.fast` matched nothing, and `irq-esp32s31-clic.o`,
`timer-riscv.o`, `kernel/irq/{chip,handle}.o` and `softirq.o` were never
listed. `handle_exception`, `esp32s31_clic_handle_irq`,
`riscv_timer_interrupt`, `handle_fasteoi_irq` and `handle_softirqs` all
ran from flash on every interrupt and tick. Moving them is a linker-script
change (in progress); the first attempt does not boot and is being
bisected with the new `make linux EARLYCON=1` knob.
