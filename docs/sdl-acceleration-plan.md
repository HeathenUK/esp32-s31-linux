# Making off-the-shelf SDL apps fast on this board

Two pieces of work, both aimed at every SDL 1.2 application rather than at one
game. Numbers here are measured on the board unless marked as an estimate.

Where we start: prboom plays at **16.2 fps** at 320x200 (`prboom -timedemo
demo1`, 12.6 -> 13.3 with window-buffer sharing -> 16.2 with Doom's native
8-bit mode). The machine is saturated and the frame rate swings between 20+
and 2-3, which is **swap, not scene complexity**: VmSwap 1276 kB and 8 major
faults a second, each an SD read with a ~2.0 ms floor.

## 1. Fitting libSDL into XIP

**Why it matters.** libSDL costs **204 kB of RSS in every SDL client** today,
demand-paged off the SD card. In XIP it costs zero RSS and executes in place;
`docs/current-state.md` records a binary run from SD measuring 4x worse. This
is the direct attack on the faulting above, and it helps any SDL app.

**The problem.** libSDL-1.2.so.0.11.4 is 366,360 bytes. The rootfs XIP
partition has 323,584 bytes free. Short by 42,776.

**The saving, measured.** Section sizes:

    .text          259,954
    .rodata         27,776
    .eh_frame       45,148     <-- DWARF unwind tables
    .eh_frame_hdr    7,644     <-- its index
    .bss           156,016     (no file space)

`.eh_frame` plus its header is **52,792 bytes of unwind tables in a pure-C
library that never throws**. Removing both gives **313,008 bytes - it fits,
with 10,576 to spare.** Verified functionally: prboom still starts, reports
"SDL buffer, direct access", and renders (screenshot).

**Do it with a compiler flag, not objcopy.** Build SDL with
`-fno-asynchronous-unwind-tables -fno-unwind-tables`. The objcopy above was
only to get the number: stripping the sections afterwards leaves the
`PT_GNU_EH_FRAME` program header pointing at a section that is gone, which
happens to work here but is an inconsistent ELF. Rebuilding reaches the same
size honestly.

**Why this costs no performance.** Unwind tables are data, never executed and
never touched on any hot path. They are read only by an unwinder walking the
stack - C++ exception propagation or `backtrace()`. SDL is C and throws
nothing.

**The one risk, stated plainly.** A C++ program that throws an exception
*through* an SDL frame (say, out of an `SDL_AddTimer` callback) could not
unwind. Nothing here does that. If such a program appears, that library gets
its tables back.

**`BR2_PACKAGE_SDL_FBCON` STAYS ON - decided 2026-09-06, do not "reclaim" it.**
It is tempting because we are X11-only through the shim, but it is not dead
code: it is SDL's framebuffer video backend, and it is what lets an SDL
application run on `/dev/fb0` with **lvdesk not running at all**. Being able
to leave the desktop and still have a usable system is a requirement here.

Note the name collides with something else: the KERNEL's `fbcon`
(`CONFIG_FRAMEBUFFER_CONSOLE`) is the text console you get back when lvdesk
exits and releases DRM master, and no SDL build option affects it. Both are
wanted; only the second is affected by this file.

The unwind tables alone cover the shortfall (52,792 saved against 42,776
needed), so nothing else has to be given up. Expect SDL's fbcon path to be
slow if used - `/dev/fb0` here is fbdev emulation, measured at ~83 ms per
update in `docs/current-state.md` - but slow and available beats absent.

**Do NOT strip unwind tables globally** via `BR2_TARGET_OPTIMIZATION`. C++
packages need them, and this is a per-package decision.

## 2. Proper 8-bit windows

**Why.** Doom renders palette indices natively. Moving it to 8-bit was worth
**+22% (13.3 -> 16.2 fps)** even though SDL still converts to 16-bit itself
every frame, because prboom's 16-bit path does a palette lookup PER PIXEL
inside its column loops. Supporting 8-bit properly removes SDL's conversion
pass as well, and halves client-to-server traffic (64 KB against 128 KB per
frame at 320x200).

### What the shim needs

1. **A depth-8 visual.** Advertise a second visual - depth 8, PseudoColor -
   and add `{8,8}` to the pixmap-format list. Remember that a client takes
   `bits_per_pixel` from the SERVER's format list, never from a guess; that
   rule already cost this project a day.
2. **A real colormap.** `XCreateColormap` currently returns a fake id and
   `XStoreColors` does nothing. The shim must keep 256 entries per colormap
   and associate one with each window.
3. **8-bit surfaces - already half done.** `px_alloc()` allocates `bpp = 1`
   for depth <= 8 and `px_hspan()` has a `bpp == 1` branch.
4. **Expansion at composite time.** The window's indices become RGB565 on the
   way to scanout.

### Which engine does step 4

**PPA CLUT - the right answer, and the hardware has it.** `ppa_ll.h` carries a
full Color Look-Up Table interface: CLUT memory for both blend layers
(`PPA_LL_BLEND0_CLUT_MEM_ADDR_OFFSET 0x400`, BLEND1 at 0x800), FIFO and
memory access modes, per-layer reset, and clock/power enables. Entries are
written in **ARGB8888**. The silicon supports L8/L4 indexed input on the blend
path; Espressif's driver simply never wired it up:

    // TODO: Support CLUT to support L4/L8 color mode
    // PPA_BLEND_COLOR_MODE_L8 = COLOR_TYPE_ID(COLOR_SPACE_CLUT, COLOR_PIXEL_L8),
    //     /*!< only available on blend input */

We write our own PPA driver, so this is ours to take - but note L8 is a
**blend** input, and `docs/accel-plan.md` lists PPA BLEND as NOT IMPLEMENTED
here. The work is: implement blend, load the CLUT, feed L8.

**Where it will and will not pay.** The measured PPA cost model says a fixed
~13 us to program and a crossover around 128 KB. A 320x200 8-bit window is
64 KB in and 128 KB out - right at the boundary, so **expect little or nothing
at Doom's window size**. Full screen at 800x480 is 384 KB in and 768 KB out,
comfortably above, and should win. Gate it on size exactly like the existing
damage-copy threshold, with a CPU path underneath.

**CPU fallback, which is needed anyway.** A 256-entry `uint16_t` table, one
load and store per pixel. At 320x200 that is 64 KB read and 128 KB written; at
the measured 88 MB/s that is ~2.2 ms.

**BitScrambler - reject, on capability.** It reorders bits within a DMA
stream and has no random-access table lookup, so it cannot do a 256-entry
palette. `docs/accel-plan.md` already rejected it, but for the wrong reason to
cite now ("nothing to convert" was true only while every client rendered
RGB565); with 8-bit windows there IS something to convert, and it still cannot
do it.

**GDMA - reject.** Fastest engine for a straight copy (210 MB/s measured, vs
PPA 153 and CPU 102) but it moves bytes without transforming them.

### Sequencing

- **Phase 1, no hardware:** depth-8 visual, colormap, CPU LUT expansion at
  composite. This alone removes SDL's per-frame conversion. Measure with
  `-timedemo` before going further.
- **Phase 2:** PPA blend + CLUT for large and full-screen windows, CPU below
  the threshold.

Gate the whole thing on the client asking for depth 8, so the RGB565 path that
every current client uses is untouched.

## Part 1: DONE, and done without touching SDL (2026-09-06)

Shipped by reclaiming idle flash, not by recompiling. The custom
`-fno-asynchronous-unwind-tables` build DID work (.eh_frame 45,148 -> 424) but
was rejected: a bespoke libSDL is a maintenance burden and a deviation from
off-the-shelf, for space that turned out to be free elsewhere.

**Where the space came from.** `factory` is 2,031,616 bytes and the loader
(`hello_world.bin`) is 1,755,264 - **276,352 bytes of genuinely idle flash**.
Taking 128 KB of it costs nothing at runtime, unlike evicting something that
executes.

    factory  0x020000  0x1F0000 -> 0x1D0000   (loader keeps 145,280 spare)
    xip2     0x210000  0x170000 -> off 0x1F0000, size 0x190000

xip2's END stays at 0x380000, so **opensbi, linux and rootfs do not move** -
which matters, because the opensbi address has five homes including a bare
`li` in `core1_trampoline.S`. Only two files changed: `partitions.csv` and
`XIP2_PARTITION_SIZE`. Every flash offset is derived from the CSV by the
Makefile, and `S05xip` mounts `mtd:xip2` by NAME, so nothing else needed
touching. The partition table is generated with `gen_esp32part.py` and
flashed at **0x8000**.

**And a rebalance.** `s31-bt` moved from image 1 to image 2, taking `libsbc`
with it (verified: bluetoothd does not link libsbc, so it is s31-bt's alone).
Both images are XIP flash, so moving a binary between them costs nothing.

    image 1  6,365,184 of 6,422,528   57,344 free   <- now holds libSDL
    image 2  1,556,480 of 1,638,400   81,920 free

**Measured on the board.**

    libSDL RSS per client   204 kB  ->  0 kB
    prboom VmSwap         1,276 kB  ->  548 kB
    major faults/second         8   ->  0.67      (12x fewer SD reads)
    idle MemAvailable       ~4,200  ->  4,660 kB
    timedemo                 16.2   ->  16.0 fps  (UNCHANGED - see below)

**The average frame rate did not move, and that is expected.** 8 faults a
second at a ~2.0 ms floor is ~2% of wall clock, so it was never going to show
in an average. What it removes is the STALLS - the 2-3 fps troughs against a
20+ fps peak. Do not quote this change as a frame-rate win; quote it as 204 kB
of RAM per SDL client and 12x less SD paging.

Desktop suite 12/12 green on the new layout.
