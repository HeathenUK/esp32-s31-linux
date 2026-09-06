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

## Part 2 research: the S31 PPA HAS a 256-entry CLUT - PROVEN ON THE BOARD

Espressif's S31 documentation lists no indexed colour mode and no CLUT, and
their driver carries `// TODO: Support CLUT to support L4/L8 color mode`. The
silicon has it anyway. Verified by `devmem` against the live chip:

    PPA base 0x20345000 (from the DTS; /proc/iomem confirms)
      0x20345000  BLEND0_CLUT_DATA   (ARGB8888 per entry, FIFO port)
      0x20345004  BLEND1_CLUT_DATA
      0x2034500C  CLUT_CONF
    CLUT_CONF bits: 0 apb_fifo_mask (0 = FIFO mode)
                    1 blend0_mem_rst      3 blend0_rdaddr_rst
                    6 blend0_force_pu     7 blend0_clk_ena

Wrote 0xC0 to CLUT_CONF (clk_ena | force_pu), pulsed the resets, pushed 256
words through the FIFO, reset the read address and read them back:

    entry   0 = 0xA5000000   (written 0xA5000000)
    entry 255 = 0xA50000FF   (written 0xA50000FF)
    entry 256 = garbage       - the RAM is exactly 256 deep

**So a full 256-entry ARGB8888 palette RAM is present and works.** Our
driver's register map already matches the P4's (`PPA_BLEND_COLOR_MODE 0x024`
in `esp32s31-ppa.c` equals `DR_REG_PPA_BASE + 0x24`), so the P4 register
documentation is authoritative for this chip.

### The colour modes, and which engine can reach them

    blend0_rx_cm (bg): 0 ARGB8888  1 RGB888  2 RGB565  4 L8  5 L4  8 YUV420  12 GRAY
    blend1_rx_cm (fg): 0 ARGB8888  1 RGB888  2 RGB565  4 L8  5 L4  6 A8  7 A4
    blend_tx_cm (out): 0 ARGB8888  1 RGB888  2 RGB565  8 YUV420  12 GRAY
    sr_rx_cm/sr_tx_cm: 0 ARGB8888  1 RGB888  2 RGB565  8 YUV420  12 GRAY
                       -- "others: Reserved". NO L8/L4 ON SRM.

**Only BLEND can expand indexed colour.** That splits the work usefully,
because SRM is already implemented in `esp32s31-ppa.c` while blend is only
partly there (`PPA_BLEND_COLOR_MODE`, `_FIX_ALPHA`, `_TX_SIZE`, `_TRANS_MODE`
are already defined and written):

| client depth | X11 depth | engine | state |
|---|---|---|---|
| 32bpp ARGB8888 | 24/32 | SRM `rx_cm=0 tx_cm=2` | engine exists, needs colour-mode plumbing |
| 24bpp RGB888   | 24    | SRM `rx_cm=1 tx_cm=2` | engine exists, needs colour-mode plumbing |
| 16bpp RGB565   | 16    | none                  | native today |
| 8bpp indexed   | 8     | BLEND `rx_cm=4` + CLUT | CLUT proven; blend path to finish |
| 4bpp indexed   | 4     | BLEND `rx_cm=5` + CLUT | same |

That is every depth an SDL application can ask for, in hardware.

## Part 2 RESULT: 12.6 -> 27.2 fps, measured by prboom's own timedemo

    12.6 fps  baseline (pixels crossed the socket, 16-bit)
    13.3 fps  + window buffers shared with the client
    16.2 fps  + Doom's native 8-bit mode (SDL still converting internally)
    20.7 fps  + a REAL depth-8 PseudoColor visual (SDL stops converting)
    27.2 fps  + zero-copy for that 8-bit window

**2.16x the baseline**, same fixed 5026-gametic workload, everything from XIP,
no X11 library on the SD card, no LD_PRELOAD, and prboom unmodified.

### What is and is NOT hardware

The expansion from palette index to RGB565 is a **CPU lookup loop** in
`xshim_window_pixels()`. The PPA's CLUT is NOT wired up. The hardware has one
- proven on silicon, 256 ARGB8888 entries - but the accel cost model argues
against using it at this size: 128 KB sits at the measured crossover, and
under load the PPA lost outright (3.77 ms against the CPU's 2.20 ms). It is
the right engine for a FULL-SCREEN client, not for 320x200.

### Six things had to be true at once

Each was silently false, and each failure looked like something else:

1. The shim must advertise the depth-8 visual and a `{8,8}` pixmap format.
2. `XGetVisualInfo` returned one hardcoded 16-bit TrueColor entry and ignored
   the template - so the visual existed and was invisible.
3. `XMatchVisualInfo`, which is the call SDL actually uses, compared against
   `DefaultVisual` alone. Both now share one visual table.
4. `CreateWindow` discarded the depth byte, so `px_alloc` always chose 2 bytes
   per pixel. Symptom: a perfectly formed, entirely BLUE picture, because an
   index of 0-255 read as RGB565 is the blue channel and nothing else.
5. `XCreateColormap` returned a constant without telling the server and
   `XStoreColors` was a do-nothing stub, so the palette never left the client.
   Symptom: perfect GREYSCALE - the shim's placeholder ramp.
6. The palette must be per-SCREEN. SDL 1.2 opens TWO connections and sends
   `XStoreColors` on the graphics one while the window lives on the other.

Then two fast paths had to learn about byte depths: xlite's zero-copy
`XPutImage` required `bits_per_pixel == 16` (so adopting 8-bit silently
disabled window sharing - the two wins cancelled), and the shim's PutImage
span path was gated on `bpp == 2`, dropping 8-bit clients into a 64,000-call
per-pixel loop.

### Sharing a CHILD window, and why it is restricted

SDL draws into a child of the window it hands the window manager, so
"top-level only" excluded exactly the clients the fast path was written for.
A child is now shared, but ONLY when it is its parent's only child
(`win_share_ok()`), because a direct write bypasses the server's clipping and
a child with siblings could paint over whichever one overlaps it. xcalc's
dozens of widget windows keep the socket path, which is right for them.

**The hazard that remains, written down rather than hidden:** if the PARENT is
resized its buffer is reallocated, and a mapping handed out earlier points at
pages the server no longer composites - the window would freeze on its last
frame. xlite drops the mapping on ConfigureNotify, which covers a child
resized along with its parent. Nothing here resizes a parent without the
child following.

## Phase 2 (PPA CLUT): NOT wired up, and why - with a correction

**Correction first.** An earlier note here said the PPA "loses under load" at
these sizes, quoting 3.77 ms against the CPU's 2.20 ms. That is the 32 KB row
of the table in `docs/accel-plan.md` and it does NOT generalise:

    rect      bytes    quiet CPU/PPA    loaded CPU/PPA
    64x64       8192   0.06 / 0.44      0.06 / 2.16
    128x128    32768   0.49 / 1.06      2.20 / 3.77
    320x240   153600   3.09 / 2.99     11.98 / 6.96   <- PPA wins 1.7x

**Under load, at 153 KB, the PPA is 1.7x FASTER.** The crossover under load
sits between 32 KB and 153 KB, so for a large window the hardware is the right
answer and the instinct to use it is correct.

**The blocker is memory, not the engine.** `esp32s31_ppa_in_range()` refuses
any buffer outside the region declared for the PPA, and that region is
`lcd_reserved` - the CMA pool. Our 8-bit window buffer is a memfd of ordinary
kernel pages, shared with the client through XLITE-SHM. **The blend engine
cannot address it at all**, so there is nothing to switch on with a threshold.

What Phase 2 actually requires, in order:

1. **A CMA-backed buffer that can still be handed to a client.** There is no
   `DMABUF_HEAPS` in the kernel config, so today there is no path. Either
   enable `DMABUF_HEAPS_CMA`, or allocate a DRM dumb buffer in lvdesk and
   PRIME-export it - XLITE-SHM already passes an fd, so the client side needs
   no protocol change.
2. **CLUT + L8 in the driver.** `esp32s31_ppa_blend()` already works; add a
   CLUT loader (256 ARGB8888 words through `PPA_BLEND0_CLUT_DATA` at PPA base
   + 0x0, with `CLUT_CONF` at +0xC: bit 0 fifo mode, bits 1/3 resets, bits
   6/7 power and clock) and set `blend0_rx_cm = 4` (L8) with
   `blend_tx_cm = 2` (RGB565). Proven present on this silicon by devmem.
3. **A size threshold in the shim**, mirroring the existing damage-copy
   dispatch rule, with the CPU LUT below it.

**Why it is not built yet.** Step 1 spends CMA, and CLAUDE.md records the
failure mode: when the pool is exhausted by client buffers the driver logs
"no scanout buffer ... scaling off" and the panel drops to 640x384. That is a
visible, silent display regression, taken on for a win that only applies to
LARGE 8-bit windows - and the only 8-bit client today is Doom at 320x200
windowed, where the CPU expansion is ~2.2 ms of a ~37 ms frame (~6%).

**Do it when there is a full-screen 8-bit client**, where the expansion is
~13 ms of CPU per frame and the table above says the PPA would roughly halve
it. Until then the threshold would gate a path that cannot run.

## What the shim accepts today - the honest matrix

Pixmap formats advertised: {1,1}, {8,8}, {16,16}, {24,32}. VISUALS, which is
what decides the depth a client can render at natively, are only two:

| client depth | window? | transport | expansion |
|---|---|---|---|
| 16bpp RGB565   | yes, native   | zero-copy | none needed |
| 8bpp indexed   | yes, native   | zero-copy | CPU LUT (PPA when the above lands) |
| 24/32bpp       | **no visual** | images accepted and converted per pixel | client converts internally |
| 4bpp indexed   | no            | -         | - |

So it is NOT yet "any depth". A 32bpp client works but converts in its own
process, exactly as an 8-bit client did before this work - and the fix is the
same shape: advertise a depth-24 TrueColor visual, teach `px_alloc` a 4-byte
surface, and give PutImage and the SHM path a 32bpp arm. The SRM engine can
already convert ARGB8888 and RGB888 to RGB565 (`sr_rx_cm` 0 and 1), and unlike
L8 that path is implemented - so 24/32bpp is the cheaper of the two remaining
depths to finish.

## The hardware path, measured end to end - and why there is nothing to choose

Asked which elements of the hardware path to enable to maximise performance,
and whether moving a window into GEM buffers is a once-off call. Both halves
were measured with `-timedemo demo1` (a fixed 5,026-gametic workload), a fresh
boot per arm, two boots per arm, at Doom's 320x200.

Two runtime knobs were added so the two questions could be asked separately -
with one compile-time constant they could only ever be answered together:

    XSHIM_PPA_MIN_PX=<n>   pixel count at which a window is born in GEM
    XSHIM_CLUT=cpu         expand on the CPU even when the window IS in GEM

| arm | index plane lives in | expander | realtics | fps |
|---|---|---|---|---|
| A | ordinary memory (memfd), cached | CPU LUT | 6489 / 6367 | 27.1 / 27.6 |
| B | GEM, write-combine | CPU LUT | 6492 | 27.1 |
| C | GEM, write-combine | **PPA** | 6447 | 27.3 |

**The noise is bigger than the effect.** Two boots of arm A, byte-identical
configuration, differ by 122 realtics - 1.9%. The largest gap between two
different configurations is C against A's first boot, 42 realtics - 0.65%.
Every arm sits inside the spread of a single arm repeated.

So the answer to "which elements should we use" is **none of them matter at
this window size**, and the answer to "is GEM a once-off call" is **it is not a
call worth taking**. Neither the palette expansion nor the buffer's memory type
is where a 320x200 frame goes.

Two beliefs died here:

* **Write-combine is not the penalty.** A against B is 3 realtics in 6,490 -
  0.05%. The uncached mapping that GEM hands out was blamed for gating the
  hardware CLUT off; it costs nothing measurable at this size. The earlier
  reasoning - that moving Doom's render target to WC would hurt more than the
  CLUT saves - was never measured end to end, only inferred.
* **The PPA's 2.5x on the expansion buys 0.65% of the frame.** `clutbench`
  timed the expansion honestly, and the expansion really is faster on the PPA.
  It is simply too small a slice of a frame for that to reach the frame rate.
  A component speedup is not a system speedup, and this is the second time on
  this board that a microbenchmark has been right about its own scope and
  useless as a prediction.

`PPA_MIN_PX` therefore stays high, but for a **new and better reason**: not
"the WC mapping costs more than the PPA saves" (false), but "nothing in this
decision is measurable below a full-screen window". The threshold is a bet on
the large-window case that the table in the previous section still supports and
which remains unmeasured end to end.

**What this does NOT say.** Every number here is 320x200. The expansion scales
with pixels and the noise does not, so a full-screen 8-bit client - 800x480, 6x
the pixels - is a different question and the one worth measuring next.
