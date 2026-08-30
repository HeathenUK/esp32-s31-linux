# Moving pixels on this board: which engine, and when

What hardware exists for moving and combining pixels, what each one actually
costs, and therefore which one the driver should pick for a given operation.
Everything here is measured on this board; where a number is inherited from
elsewhere it says so.

## The engines

| Engine | Can do | Reached by | Status |
|---|---|---|---|
| CPU | anything | `memcpy`, per-pixel C | used below the threshold |
| PPA SRM | scale, rotate, mirror | `esp32s31_ppa_scale_rect()` | used above the threshold |
| PPA BLEND | two-layer alpha, solid fill | **NOT IMPLEMENTED** | see below |
| AXI GDMA | `DMA_MEMCPY`, `DMA_MEMSET` | dmaengine | fastest, but 1D only - see below |
| AHB GDMA | `DMA_MEMCPY` | dmaengine | untested; same 1D limitation applies |
| BitScrambler | stream bit manipulation | — | ruled out, see below |

## The cost model

Measured end to end through `ppabench`, which drives the PPA ioctl against a
row-by-row `memcpy` on the same buffers.

Idle board, X stopped:

	  rect      bytes     CPU       PPA
	 32x32       2048   0.03 ms   0.43 ms
	128x128     32768   0.17 ms   1.08 ms
	256x256    131072   2.63 ms   2.16 ms
	640x384    491520   9.66 ms   6.43 ms

Desktop running, and again with continuous pointer motion on top:

	  rect      bytes    quiet CPU/PPA    loaded CPU/PPA
	 64x64       8192   0.06 / 0.44 ms   0.06 / 2.16 ms
	128x128     32768   0.49 / 1.06 ms   2.20 / 3.77 ms
	320x240    153600   3.09 / 2.99 ms  11.98 / 6.96 ms

Three facts fall out, and they are the whole basis for the dispatch rule:

1. **The PPA is faster per byte.** A copy moves twice the bytes it reports;
   491,520 bytes copied in 6.43 ms is 153 MB/s of memory traffic against the
   CPU's 102 MB/s. Kernel-side fills reach 192 MB/s. PSRAM bandwidth is the
   ceiling for both, so the edge is ~1.5x, not orders of magnitude.
2. **The PPA cannot start cheaply.** Programming it is a constant 13 us
   (25 register writes, descriptor in uncached SRAM, no cache maintenance) but
   the completion round trip costs a few hundred microseconds, and that is
   fixed regardless of size.
3. **The CPU has cache and the PPA does not.** A 32 KB `memcpy` runs at
   177 MB/s because it never reaches PSRAM. The engine always does.

Under load the two halves move in opposite directions and both argue for the
same split: the CPU degrades worse at large sizes (3.9x against the engine's
2.3x, since the PPA competes for neither cycles nor cache) so the engine's
advantage grows from 1.03x to 1.72x; while at small sizes the engine gets much
worse because its completion wait is contended. Using the engine below the
threshold would hurt most exactly when the machine is busiest.

**Crossover: ~128 KB, and it holds busy or idle.** One constant is enough.

### A correction worth keeping

An earlier version of this reasoning had the CPU at 22.6 MB/s and concluded the
PPA was worth 8.5x. That figure was X's *fill* rate including X's own overhead,
not `memcpy`. Against a real baseline the engine is worth 1.2-1.7x on large
blits. The difference decided whether to write an accelerated X driver, so it
was worth an afternoon to get right.

## What each operation uses, and why

- **Damage copy, above 128 KB** - PPA SRM. Large, and often needs scaling
  anyway (the reduced render mode), which the CPU cannot do at all.
- **Damage copy, below 128 KB** - CPU. Typical desktop damage is ~69 KB, so
  this is the common case. Only possible at 1:1.
- **Cursor composite** - CPU, per-pixel alpha in C. A 32x32 cursor is ~2 KB,
  where the CPU wins by 10x. Note this is a DRM cursor *plane*, which is an
  API, not a hardware promise: the win came from taking the work away from X
  (a fixed ~19 ms per pointer move of save/restore, damage tracking and shadow
  copy), not from any accelerator.
- **Cache maintenance** - CPU, irreducible. `dma_alloc_coherent()` returns
  cached memory on this SoC, so anything the engine touches must be flushed
  before and invalidated after.

## Ruled out, with reasons

- **BitScrambler.** Its attach list is GDMA peripherals and SDMMC drives its own
  IDMAC, so it cannot reach the storage path (recorded in `current-state.md`).
  For display there is nothing to convert: the client renders RGB565 and the
  panel scans out RGB565.
- **PPA BLEND for the cursor.** Would need per-pixel alpha from an ARGB8888
  source, which means colour-mode register values we have no TRM for - and at
  2 KB the CPU wins anyway.
- **An accelerated X driver (EXA).** Possible without patching Xorg: drivers are
  loadable modules, the ABI is 25.2, `libexa.so` already ships, and the
  `DRM_IOCTL_ESP32S31_PPA_COPY` ioctl exists to reach the engine. But the
  ceiling is the 1.5-1.7x above, only on operations over ~128 KB, and glyphs -
  the thing most visible when typing - are ~256 bytes, thirty times below the
  crossover. Not proportionate to writing and maintaining a driver.

## GDMA mem-to-mem: fastest engine, wrong shape

Measured, copying within the scanout buffer:

	   bytes    GDMA us     PPA us     CPU us
	    4096        479          -          -
	   16384        540          -          -
	   32768        610       1060    170-490
	   65536       1322          -          -
	  131072       2067       2159       2631
	  262144       2763          -          -
	  384000       3654          -          -

**It is the fastest engine on this board.** 384,000 bytes copied in 3654 us is
768,000 bytes of memory traffic, 210 MB/s, against the PPA's 153 MB/s and the
CPU's 102 MB/s. It beats the PPA by 4% at 128 KB and ~27% at 384 KB, and its
fixed cost (~479 us at 4 KB) is no worse.

**And it cannot be used for the damage copy in the configuration we ship**,
because `DMA_MEMCPY` is one-dimensional. It needs source and destination rows
to be contiguous, which holds only when the pitches match:

- Native 800x480: client pitch == scanout pitch, so whole-row damage is one
  contiguous run and one descriptor. GDMA works and is the best choice.
- Reduced 640x384: 1280-byte rows written into a 1600-byte-pitch scanout. Every
  row is a separate run, so a full frame needs 384 descriptors at ~479 us of
  fixed cost each - two orders of magnitude worse than either alternative.

`device_prep_dma_sg` was removed from the kernel, and `prep_slave_sg` is for
peripheral transfers, so there is no single-descriptor strided copy available.

This is a genuine constraint of the 640x384 choice, not a defect in the engine,
and it is worth knowing if the memory situation ever allows a return to native
resolution: at 800x480 the damage copy should use GDMA, not the PPA.

Both the damage path and the whole-surface copy now prefer GDMA when the
geometry allows it, and decline cleanly when it does not. Verified at 800x320:
`gdma_rows` climbs, the engine does the work.

It buys the desktop nothing. 800x320 with GDMA active against 640x384 without
gave repaint means of 35.5 and 36.2 ms - and 800x320 costs more framebuffer,
which pushed xcalc back into swap (8 kB resident against 416). The copy was
never the bottleneck. This is kept because it is correct and free when the
geometry suits it, not because it made anything faster.

The prediction that its fixed cost might undercut the PPA's was wrong - both
sit around 480 us - but its bandwidth is the best available. The channel came
from `dma_request_chan_by_mask()` with no DTS change, which matters because a
DTS change needs both `make linux` and `make opensbi`.

## What this is worth

Be honest about the size of the remaining prize. The driver's commit path is now
**0.3-0.6% of wall clock**. The damage copy is ~0.7 ms per update at 40 fps -
under 3% of a core. Removing it entirely buys ~3%.

X is 45-67%. That is where the time is, and reaching it needs the driver that
the numbers above do not justify. The kernel display path is close to finished;
further work here is refinement, not the main lever. The main levers left are
memory (clients still page out under a full desktop) and X's own cost.


## The lvdesk era: where the time actually is (2026-08-29)

Everything above was measured under X11. The desktop is now lvdesk driving KMS
directly, so "X is 45-67%" no longer names the cost. The engine numbers stand;
the conclusion needed re-measuring, and it comes out the same way, harder.

`LVDESK_PROF=1` splits the main loop into its phases (poll wait, input, LVGL
timers, `lv_refr_now`) and reports every 5 s. Measured while scrolling 2000
lines through the terminal:

    prof: loops=118 wait=5127ms input=2462ms timer=203ms refr=5579ms
          (per loop: input=20869us timer=1720us refr=47283us)

and the kernel's own counters over the same kind of workload:

    272 updates, upd_ns 285.9 ms  ->  835-967 us per update, every run

So per frame, under load:

    LVGL rasteriser (lv_refr_now)   47.3 ms    68%
    terminal input + grid            20.9 ms    30%
    LVGL timers                       1.7 ms     2%
    kernel display path (commit)       0.8 ms   ~1%

**Hardware acceleration addresses the 1%.** That is the whole answer to "should
we accelerate more": the PPA and GDMA already run where they win, the driver's
commit path is under a millisecond, and nothing about blending or moving rects
touches the 68%.

### Why the rasteriser is slow, and it is not bandwidth

The terminal is 61x36 characters. A full redraw writes ~140,000 pixels, 281 KB
at 16 bpp - **2.8 ms** of memory traffic at the CPU's measured 102 MB/s. It
takes 47 ms. The missing 44 ms is per-glyph overhead in LVGL's draw path: ~2200
glyphs at **~21 us each**, for an 8x8 1bpp bitmap that a purpose-built blitter
should place in well under a microsecond.

This is why no accelerator helps. A glyph is ~256 bytes - the crossover is
128 KB, and the PPA's fixed cost alone is ~480 us, twenty times the entire
budget for drawing one character.

### What the hardware cannot reach, settled

Asked directly: PIE SIMD, BitScrambler, PPA, GDMA.

- **PIE / SIMD (`xespv2p2`) is disabled for userspace on purpose and must stay
  that way.** The kernel does not save vendor vector state across context
  switches. Its sibling `xesploop` silently corrupted results at ~0.5% per long
  loop, scaling with interrupt rate, and cost days while masquerading as SD
  corruption. The loop CSRs are `0x7Cx` with bits[9:8]=`0b11` - **M-mode only** -
  so S-mode Linux cannot save them even if it wanted to. See
  `s31-hardware-loop-corruption`. And the headroom is not there anyway: `-Os` vs
  `-O3` on the pixel loops measures **0-9%** (`rootfs/pixloop.c`).
- **BitScrambler** - nothing to convert. The client renders RGB565 and the panel
  scans out RGB565.
- **PPA / GDMA** - a glyph is ~256 bytes against a 128 KB crossover, and the
  engine's ~480 us fixed cost is twenty times the entire per-glyph budget.

**None of them can help, because the problem is not moving pixels.** Under a
scroll the driver flushes 117-594 KB per update; writing even 594 KB at the
CPU's measured 102 MB/s is 5.8 ms, against 47 ms spent in `lv_refr_now`. The
rasteriser is spending roughly an order of magnitude more than its own memory
traffic, and that excess is per-object and per-glyph software overhead inside
LVGL. An accelerator moves bytes; it cannot remove software.

### Config knobs: tested, and they are not it

`LV_OBJ_STYLE_CACHE` 0 -> 1, three runs each, 1200 lines scrolled through the
terminal via `/dev/pts/0`:

    baseline      2190  2150  2270 ms
    style cache   2210  2320  2070 ms

Identical inside the harness's +-3% noise floor. Reverted. `LV_CACHE_DEF_SIZE`
is 0 and `LV_USE_FONT_COMPRESSED` already off; there is no knob here that
recovers 47 ms.

### A repeatable harness, at last

Driving the terminal with `uinject` was too unreliable to measure with - three
separate arms were invalidated by a click that missed, a window that did not
resize, and an Enter that never registered, each producing plausible-looking
numbers. **Write to the pty instead:**

    seq 1 1200 > /dev/pts/0

That needs no focus, no pointer and no keyboard, and it reproduced to +-3%
across three runs where injection had been swinging by 70%.

### The main rasteriser: four theories tested, three dead

The terminal is one client; the question was about LVGL's rasteriser generally.
Measured with the pty harness, 1200 lines scrolled, repeats shown.

**1. The framebuffer mapping is uncached, so blends crawl. FALSE.**
The driver uses the drm_gem_dma helpers and does not set `map_noncoherent`, so
the dumb buffer reaches userspace **write-combine**, and LVGL renders DIRECT
into it. That looked damning - a rasteriser reads the destination it is about
to write. It is not the problem:

    fb   read-modify-write   13,289 us
    heap read-modify-write   12,770 us     (cached, pre-faulted)

**2. So render into cached memory and copy out. NO GAIN.**
`LVDESK_PARTIAL=1` forces PARTIAL mode into a heap buffer with a copy in the
flush callback, against DIRECT into the framebuffer:

    DIRECT    2180  2140  2190 ms
    PARTIAL   2130  2140  2140 ms

1.5%, inside the noise. The DIRECT choice stands, and now for a measured
reason rather than an assumed one. Note the copy is also not costing anything
visible, which is the same finding from the other side.

**3. Style lookups. NO GAIN.** `LV_OBJ_STYLE_CACHE` 0 -> 1: 2210/2320/2070
against 2190/2150/2270. Reverted.

**4. It is per-glyph cost. NOT ESTABLISHED, and the obvious model is wrong.**
Same 1200 scrolled lines, varying only the glyphs per line:

    BLANK   0 glyphs/line    3 updates    820  910 ms
    SHORT   4 glyphs/line   11 updates   2140 2200 ms
    LONG   60 glyphs/line    6 updates   1450 1580 ms

Drawing glyphs is clearly a real fraction - blank lines are 2.5x cheaper than
short ones. But **60 glyphs per line is cheaper than 4**, so cost does not
scale with glyph count, and normalising per update does not rescue it either
(273, 200, 241 ms per update respectively). The arms differ in how the pty
data chunks against the poll loop, and this harness cannot separate
per-render from per-line cost. **The earlier "~21 us per glyph" figure was an
inference from 47 ms / 2200 glyphs and should not be quoted** - it assumes the
model this experiment just contradicted.

What survives: the cost is inside LVGL's draw path, it is not the memory it
writes to, and it is not reachable by any engine on this SoC. Sizing a fix
needs the per-render and per-line terms separated first - instrument
`term_scroll` and `lv_refr_now` counts independently before writing anything.

### How this is done elsewhere, and what of it applies (2026-08-29)

Researched rather than guessed, then each idea tested on the board.

**esp_lvgl_port / LVGL's own Espressif guidance** is emphatic about one thing:
put the draw buffer in **internal SRAM**, sized 10-25% of the screen, with DMA.
PSRAM writes are quoted at 3-4x slower, and their measurements are explicitly
"valid for frame buffer in internal SRAM. Placing the frame buffer into
external PSRAM will yield worse results" - 41 FPS internal+DMA against 11-31
PSRAM.

That reframed an earlier null result here: DIRECT-into-framebuffer versus
PARTIAL-into-heap measured identical, but **both arms were PSRAM**, because
Linux's heap on this board *is* PSRAM. The board has one `mmio-sram` node,
`sram@2f062000`, **0x8000 = 32 KB, entirely owned by the audio DMA pool**.

So the recommendation cannot be followed directly. But it predicts something
testable without repartitioning anything: if memory speed were the limit, a
partial buffer small enough to stay in cache would behave like fast memory -
`accel-plan.md` already measures a 32 KB memcpy at 177 MB/s against ~102 MB/s
once it reaches PSRAM. Swept, two runs each, 1200 lines scrolled:

    DIRECT   (PSRAM fb)        2200  2250 ms
    PARTIAL   8 rows  12.8 KB  2350  2550 ms
    PARTIAL  16 rows  25 KB    2450  2440 ms
    PARTIAL  32 rows  51 KB    2370  2200 ms
    PARTIAL  64 rows 102 KB    2160  2170 ms

**Monotonically worse as the buffer gets smaller.** Cache residency buys
nothing, which says the rasteriser is not waiting on memory - and therefore
that moving the draw buffer into internal SRAM would not help either. That is
worth knowing before repartitioning SRAM with hart0, which is the only way to
get a useful amount of it.

(It also independently reproduces esp_lvgl_port's "below 10% of the screen has
a severe negative effect": 12.8 KB is 1.7% of this screen.)

**LVGL's cache.** The docs say every font backend goes through LVGL's cache
system - a glyph-descriptor cache and a draw-data cache - and ours was off.
`LV_CACHE_DEF_SIZE` 0 -> 64 KB: 2280/2280/2180 against ~2200. Nothing, which is
the right answer for a static bitmap font where fetching a glyph bitmap is a
pointer computation. That cache is for decoded images and FreeType/TinyTTF.

**TFT_eSPI is not applicable, and it is worth saying why.** Its speed comes
from the *transport* - DMA to an SPI display, `startWrite`/`endWrite`
batching, sprites composed in RAM then pushed as one DMA image. We already
have the equivalent and better: a parallel RGB panel scanning out of PSRAM
continuously, with the commit path measured at 0.8 ms. Nothing in TFT_eSPI
addresses rasterisation cost.

**What modern terminal emulators actually do** (Windows Terminal AtlasEngine,
kitty, Alacritty) is three things, and the GPU is only one of them:

1. **Glyph atlas** - rasterise each character once, then every later frame is a
   blit of a cached cell, never a rasterisation.
2. **Per-row damage bitset** - the parser flips a dirty bit per row touched;
   the renderer redraws only those rows.
3. **Scroll is a memcpy on contiguous memory**, not a redraw.

All three are CPU-side and all three apply here. We have (2) in `rowdirty` and
then throw it away - `term_scroll()` calls `term_mark_all()`, so every scrolled
line dirties the whole screen. We have no (1) and no (3).

### The desktop, profiled properly, and the hardware cursor (2026-08-29)

The terminal was a load generator, not the desktop. Profiled with
`LVDESK_PROF=1` plus LVGL's own hooks (`LV_USE_PROFILER` pointed at
`lvdesk/lv_prof_hooks.h`, which aggregates per section instead of writing a
trace), the desktop's cost was somewhere else entirely.

**Pointer motion alone cost 30-46% of the core.** The tree said why:

    lv_timer_handler       2341 ms
      timer_cb             2329 ms
        lv_display_refr_timer 2329 ms
          refr_invalid_areas  2175 ms  n=119   18,278 us/call
            refr_area         1746 ms  n=204    8,558 us/call
              refr_obj_and_children 1698 ms n=612
                EVENT_DRAW_MAIN     1258 ms n=1781  706 us/call

The pointer was an LVGL object, so **every move ran a full refresh cycle and
redrew ~15 objects**. Note this also corrects an earlier reading here: LVGL's
refresh timer runs *inside* `lv_timer_handler`, so rendering was hiding in the
"timer" bucket while the separate `lv_refr_now()` measured ~0.

The driver has had a working cursor plane all along, and its own comment
recorded that X11 lost 2.4x on pointer motion when that plane refused itself -
while noting lvdesk "draws its pointer in LVGL and never uses the DRM cursor
plane, so cursor_moves and cursor fb_changes both read 0". Wiring lvdesk to it
(`kms_cursor_init`/`kms_cursor_move`, ARGB8888, 64x64 max):

    window drag   SW cursor  9010 8620 8750 ms   57-60% of core
                  HW cursor  3670 3740 ms        24%
    idle          3% either way

**2.4x, independently reproducing the driver's X11-era figure.**

Two traps inside that change, both self-inflicted and both measured:

- **The legacy cursor ioctl costs ~1 ms**, because it pulls the primary plane
  into the atomic state - the driver says so explicitly. Unpaced it became the
  largest single item in the input phase (478 ms per 5 s window). Paced to
  16 ms it is 181 ms, with a settle so the pointer never rests behind the hand.
- The first settle ran every loop and never updated the last-sent position, so
  it re-sent the same coordinates for ever and cost **81% of the core**.

**Where a drag now spends its time** (input phase, per 5 s window):

    term_poll   254 ms      <- with an IDLE terminal
    mouse_poll  374 ms      (of which cursor ioctl 181)
    kbd_poll    139 ms
    wifi_poll   136 ms
    waitpid      83 ms

That is ~600 ms per window of per-loop polling overhead that has nothing to do
with dragging, against ~200 ms now spent in LVGL. **The remaining cost is the
poll loop, not rendering.**

### Would per-window planes help, like the cursor?

Asked directly, and the answer is a low ceiling - for a reason that is easy to
miss. **There is no hardware compositor on this SoC.** LCD_CAM is "a dumb
scanout engine: a timing generator plus a FIFO" (esp32s31-lcd.c, line 5). The
cursor "plane" is composited by the CPU, per-pixel alpha in C. Its win came
from removing LVGL's redraw cycle, not from free hardware blending - and that
is affordable only because a 12x19 cursor is 228 pixels.

A 500x310 window is 155,000 pixels, 310 KB per composite. The PPA could do it
(PPA BLEND exists and is unused, and 310 KB is well above the 128 KB
crossover), so it is *feasible* - but the prize is now small: LVGL rendering is
~200 ms of the ~1550 ms a drag costs, because the cursor plane already took the
big redraw away. Reserving ~1 MB of PSRAM for window buffers would target ~13%
of a drag while adding a permanent compositing cost whenever anything moves,
and the scanout copy currently costs nothing at all when the screen is static.

Revisit it only if the poll-loop overhead above is fixed first and rendering
becomes the majority again.

### The canvas terminal was built, measured, and rejected (2026-08-29)

Implemented in full - an lv_canvas the size of the content area, the grid drawn
into it with one lv_draw_label per colour run, scrolling as a memmove of the
pixel buffer with only the newly exposed rows rasterised, and the scroll applied
ONCE per render rather than once per line. It works and renders correctly. It
does not pay:

    workload                    labels    canvas
    bulk flood (2000 lines)     3230 ms   3410 ms   canvas 6% worse
    paced 15-line batches       1950 ms   1850 ms   canvas 5% better
    single lines (typing)        780 ms    850 ms   canvas 9% worse

for **281 kB of permanent RAM** at the default window size, on a board with
~3.6 MB free. Reverted.

Three reasons it cannot win here, all worth knowing before anyone tries again:

1. **Bulk output defeats every incremental scheme.** `seq 1 2000` floods fast
   enough that each render sees ~180 scrolled rows against 36 on screen, so the
   whole screen genuinely changed and the "redraw everything" path runs anyway -
   now with a canvas blit on top. No incremental technique helps the flood case;
   only cheaper full-screen rendering does.
2. **`lv_canvas_finish_layer()` invalidates the whole canvas.** Drawing one row
   still flushes the entire content area, so a single-line update went from the
   label path's 57k pixels to 709k. Adding a band invalidation of only the rows
   redrawn changed nothing, because the canvas invalidates itself first.
3. **The labels are not as bad as assumed.** For a single-line update LVGL
   invalidates just that label - 9.5k pixels - which is already close to optimal.
   The 36-label cost only appears on a scroll, and a scroll moves every pixel on
   screen no matter how it is drawn.

What remains true is the profile that motivated it: during a scroll LVGL spends
~49 ms per loop in timer+refr, and the canvas does collapse that to ~6 ms. The
work simply moves into term_poll and the canvas blit, for no net gain. Beating
it needs a cheaper glyph path - the atlas, blitting pre-rendered cells - not a
different place to put the pixels.

### The "redundant" scanout copy is not redundant (2026-08-29)

Chased because it looked like the largest free win left: the driver performs a
CPU copy of every damage rectangle into a private buffer (`path: cpu=477`
during one drag, ~119 MB and ~10 MB/s over a 12 s drag), while the boot log
said the panel was scanning out the *client's* framebuffer:

    enable scanout at 0x50800000, 768000 bytes, from plane fb

If both were true the copy would be writing to memory nobody reads. **They are
not both true.** The log line was wrong: its `"from %s"` tested
`(plane_state && plane_state->fb)`, which is true whenever a client has a
framebuffer at all, including every case where scanout is the private buffer.
The line immediately above it gives the game away - `scaling: scanout buffer
768000 bytes at 0x50800000` is the PRIVATE buffer, at the very same address.

`esp32s31_lcd_composite()` returns **true unconditionally**, and its comment
says why: this DMA engine cannot be retargeted mid-session (it fails -ENXIO),
and a cursor appears long after `.enable` runs, so the driver always composites.
The copy is how content reaches the panel. The visible cursor is the proof -
it is composited into the private buffer, and it shows.

So there is no free win here, and the documented trade stands: ~1 ms of copy
per content update to keep a cursor move at ~0.3 ms, against X's 19 ms. Fixed
the log message (patches/0020) so the next person does not spend the afternoon
this cost.

### The glyph atlas: three attempts, all wrong bitmaps (2026-08-29)

The idea survived the canvas post-mortem because it avoided both of that
attempt's failures: fill a buffer we own with our OWN blitter (not
lv_draw_label) and present it as an lv_image (not a canvas, so no forced
whole-object invalidation). The scaffolding all worked - image object, buffer
sizing, memmove scroll, band invalidation, fallback to labels. What never
worked was getting correct glyph bitmaps out of the font.

Three attempts, each failing differently:

1. `lv_font_get_glyph_bitmap(&g, NULL)` - **segfault before the first glyph**.
   The API dereferences the draw buffer unconditionally; it is not optional.
2. A real `lv_draw_buf_t`, reading the result as A8 - **457 lit pixels across
   95 glyphs**, a blank terminal. `g.stride` is documented as "0 means no
   padding", so indexing with `row * g.stride` reads row 0 for every row.
3. `req_raw_bitmap = 1` and decoding A1 by hand - **650 lit pixels and visible
   garbage** on screen, speckled columns rather than text.

The thing that made attempt 3 legible at all: `fmt=1 box=2x7 stride=0` for `!`.
**unscii-8 has variable-width glyphs**, not the fixed 8x8 cell the whole design
assumed - so a fixed-cell atlas needs per-glyph box_w/box_h/ofs_x/ofs_y
handling, and getting that subtly wrong produces exactly the speckle seen.

Going further means depending on `lv_font_fmt_txt` internals - glyph index
tables, bitmap formats, possible compression - which is the kind of
internals-dependence that has cost this project repeatedly. Stopped and
reverted.

**Worth keeping regardless: the ink check.** Counting lit pixels in the atlas
is what caught attempt 2. Without it the builder reported "95/95 glyphs" over
an entirely empty table and the terminal simply rendered black - a success
message over a broken result.

If this is picked up again, start by dumping one known glyph's bytes and
comparing them against what the label path draws, before wiring any of the
rest. The scaffolding is not the hard part; the font is.

### The options, by measured headroom

1. **Draw the terminal grid directly instead of through LVGL labels** - targets
   the 47 ms. One label per row already replaced one label per screen (which
   cost 450 ms a keystroke); the next step is to stop using labels for the grid
   at all and blit the font ourselves. Biggest prize by a wide margin.
2. **Coalesce terminal scrolls** - targets part of the 21 ms. Bulk output
   scrolls the grid once per line: ~11.5 KB of memmove (grid + attributes) each,
   ~17 lines per loop under `seq`. The net scroll could be computed once.
3. **Idle wakeup cost** - idle is 55 loops per 5 s at ~7 ms each, 7.6% of the
   CPU to display a screen that is not changing. `lv_refr_now` costs 2.4 ms
   with nothing invalid, and `kbd_scan`/`mouse_scan` open 32 device nodes every
   2 s.
4. **Accelerate the scroll itself** - the ONE place an engine genuinely applies.
   Scrolling the framebuffer is ~750 KB, far above the 128 KB crossover, and
   GDMA reaches 210 MB/s. But it only pays after (1), because today the whole
   area is re-rasterised anyway and moving pixels that are about to be
   overwritten buys nothing.

Note the ordering: 4 is the only item on this list that is "more hardware
acceleration", and it is last, and it is contingent on 1.

## Which crossover governs what - and a knob that does not do what it says

Three numbers in this repo have been used interchangeably and should not be.

- **`accel-plan.md`'s ~128 KB crossover** was measured through `ppabench`, which
  drives the **ioctl** path: syscall, GEM lookups, and the cache maintenance the
  kernel does because DMA memory here is cached. It is the right number for
  deciding whether an accelerated **X driver** is worth writing.
- **`esp32s31-ppa.c`'s "worth using above roughly 1 KB"** is a **kernel-side**
  fill measurement, and it compares against 22.6 MB/s - the CPU figure this file
  later corrected to ~102 MB/s, because 22.6 was X's fill rate including X's own
  overhead. Recomputed against 102 MB/s the same table puts the fill crossover
  nearer 32 KB, not 1 KB.
- **`ppa_min_bytes` (131072)** governs the **kernel's damage copy**, which
  touches no ioctl at all. It was set from the first number, so an in-kernel
  decision is being made from a userspace-path measurement.

### Measured, kernel side, at full-screen damage

`xfill` on the desktop, alternating within one boot, control repeated:

	                repaint median        flush per update
	 always-PPA   27.3 28.7 28.0 25.7 ms     295-372 us
	 never-PPA         29.7  29.4 ms        1117-1161 us

**The PPA is ~7-8% faster at 491,520 bytes, and the reason is cache maintenance,
not bandwidth.** The CPU path writes the scanout buffer with the CPU, so the
written region has to be flushed for the scanout DMA - 3-4x the flush cost. The
engine writes by DMA and the flush largely goes away. That is a better argument
for the engine than the MB/s figures, and it is invisible in any benchmark that
does not count cache maintenance.

### The knob does not disable the engine

`ppa_min_bytes=99999999` still leaves `ppa_ops` at roughly one per update - 55
and 51 in the arms above, against 48 and 50 for always-PPA. **It does not gate
every use of the PPA**, only the damage copy, so a sweep across it compares two
configurations that both use the engine.

That explains four null desktop sweeps: the arms were never as separated as the
knob's name implies. Any future comparison must check `ppa_ops` in the debugfs
rather than trusting the parameter, and if a true "no PPA at all" arm is wanted
it needs a real switch adding.


## Correction: PPA BLEND does not exist, and the crossover above is a COPY crossover

Two errors in this document, both found 2026-08-30 while deciding how to
implement RENDER `Composite` with a mask in the X shim.

**1. The table said PPA BLEND was "implemented, unused". It is not implemented
at all.** `esp32s31-ppa.h` exports exactly two functions:

    int esp32s31_ppa_scale(...)
    int esp32s31_ppa_scale_rect(...)

There is no blend entry point, no blend ioctl, and `esp32s31-lcd.c` alpha-
blends the *cursor* in software (`/* Alpha-blend the ARGB8888 cursor into the
RGB565 scanout buffer. */`) - which it would not do if the engine were
reachable. Anything in these notes that reasons from "BLEND exists and is
unused" is reasoning from a table entry, not from the driver.

**2. The ~128 KB crossover is a COPY crossover and must not be applied to
blending.** `rootfs/ppabench.c` drives one ioctl, `DRM_IOCTL_ESP32S31_PPA_COPY`
- a blit. A blend is a very different job for the CPU: two source reads, three
multiply-adds and a write per pixel, against a copy's move. The PPA's fixed
cost (~13 us to program) does not change. So the blend crossover must sit at a
**smaller** rectangle than the copy crossover, and quoting 128 KB to reject
hardware blending is quoting the wrong number.

The X shim's masked `Composite` is therefore implemented in software for now,
with the composite sizes logged under `XSHIM_TRACE=1` so the real distribution
is known before anyone writes a blend path. Two customers would share it: the
shim's masked composite and glyph blending, and the driver's software cursor
blend.

**Do not repeat the mistake this document caused:** check the driver header for
the entry point before reasoning about whether an engine is available.


## 2026-08-30: the PPA was waiting on a flag it could never see

**Every number in this document that compares the PPA against the CPU was taken
with a fixed cost an order of magnitude too high.**

`esp32s31_ppa_spin_done()` polled `PPA_INT_RAW` while the driver's own ISR did
`writel(st, ppa->ppa + PPA_INT_CLR)`. The flag is set and cleared before the
spin can observe it, so the spin always burned its full budget and then fell
through to a sleep. The tell is that the wait scaled with the timeout:

    ppa_spin_us = 300   ->  blend 64x64 wait 1,121 us
    ppa_spin_us = 3000  ->  blend 64x64 wait 3,304 us

**The authority is Espressif's own driver.** In
`esp-idf/components/esp_driver_ppa/src/`, `ppa_blend.c`, `ppa_srm.c` and
`ppa_fill.c` all complete on the 2D-DMA receive EOF
(`dma2d_rx_event_callbacks_t.on_recv_eof`); none of them uses the PPA's EOF
interrupt. This driver's own timeout fallback already polled that bit, so the
correct signal was in the file the whole time.

    blend 64x64   1,322 us -> 169 us   (setup 9 us, wait 160 us)
                  24 KB in 160 us = 150 MB/s - the engine's real rate

### What that does to the decisions here

Blend against a CACHED CPU blend (the honest baseline - DRM dumb buffers are
mapped uncached, and a CPU blend through them runs at ~2.4 us per PIXEL, which
flatters the engine by 30x):

    rect       bytes     PPA us   CPU cached   ratio (before -> after)
    32x32      2,048       222        197      0.20 -> 0.89
    64x64      8,192       268        625      0.53 -> 2.33
    96x96     18,432       448      1,530      1.54 -> 3.42
    128x128   32,768     1,326      2,817      2.23 -> 2.12
    400x300  240,000     4,315     20,493      4.80 -> 4.75

**The blend crossover moved from ~18 KB to ~2 KB.** At icon size the PPA went
from losing 2:1 to winning 2.3:1.

The same fix is now applied to the fill and SRM paths, because the vendor
driver says they share the completion signal. **The SRM/copy crossover in the
tables above therefore needs re-measuring** - it was taken with the same broken
wait, and the ~128 KB figure is not trustworthy. Single runs after the fix were
too noisy to quote; that needs the repeat discipline the rest of this document
uses.

### Two further corrections to this document

- **PPA BLEND is implemented.** `esp32s31_ppa_blend()` has existed since
  bring-up and works; it was simply not exported from `esp32s31-ppa.h` and was
  reachable only from debugfs. It now has `esp32s31_ppa_blend_layers()` and
  `DRM_IOCTL_ESP32S31_PPA_BLEND`. Reading the header and concluding "not
  implemented" was wrong - grep the `.c`.
- **`ppabench` measures a BLIT.** It drives `DRM_IOCTL_ESP32S31_PPA_COPY` only,
  so its crossover is a copy crossover and says nothing about blending. Use
  `rootfs/blendbench.c` for blends; it also verifies correctness (every alpha
  lands on the exact expected RGB565 value) before timing anything.

### Do not aim raw addresses at the PPA

The debugfs interfaces take physical addresses and the driver bounds them to
`lcd_reserved` - but that region is a `reusable` CMA pool, so an address inside
it is very likely a LIVE allocation. Writing blend output to `0x50900000` while
the desktop was running corrupted CMA and killed several processes. Use the
ioctls with GEM handles, which is what they are for.

## Shim pixmaps cannot move into CMA yet (measured 2026-08-30)

The PPA can only touch the reserved `lcd_reserved` region, so using it for the
shim's compositing needs the drawables to live in DRM dumb buffers. That was
attempted and is **rejected on the numbers**, not on principle.

DRM dumb buffers are mapped write-combine, and the shim does far more CPU
drawing into pixmaps than PPA blitting out of them. `esp32s31_lcd_lcd_gem_create_object()`
was added to set `map_noncoherent = true`, which should have made the mapping
cached. It did not: the CPU blend through a dumb buffer is unchanged.

    blendbench, alpha 128, 20 iterations, 3 runs on a quiet board

      rect       PPA us    CPU dumb-buffer    CPU cached malloc    PPA vs cached
      64x64      294-401       8,919-9,175           624-840          1.7-2.1x
      256x256  2,574-2,838   148,917-153,541      11,725-14,245      4.4-5.0x
      400x300  4,254-4,305   272,707-277,929      20,571-25,023      4.8-5.8x

The middle column is the cost of putting a drawable where the PPA can reach it:
**~13.7x slower for CPU access** at 64x64. Any win from hardware blending is
swamped by every ordinary draw into that surface. Note the first column also
confirms the completion-signal fix holds - see [[s31-ppa-completion-signal]].

So: **PPA blend is worth using where both surfaces are already in CMA** (it wins
1.7x at icon size and ~5x at window size), and is not worth relocating drawables
to reach. Revisit only if a genuinely cached CMA mapping can be demonstrated -
the `map_noncoherent` route was tried and does not do it here.

## Where the PPA can still help, after the completion-signal fix (2026-08-30)

A survey of lvdesk and the whole xlite family, re-asked now that a PPA
operation costs ~200 us of fixed overhead instead of ~1,300.

**The filter that decides everything: the PPA can only address `lcd_reserved`.**
So the question is never "is this a blend?" but "are BOTH surfaces already in
CMA?" - because relocating a surface to reach the engine costs 13.7x on every
CPU access to it (measured above). Two surfaces qualify: the scanout dumb
buffer and any other dumb buffer.

**Ruled out, with reasons:**

- **The entire xlite family** (xlite, xrlite, xftlite, xtlite, xstubs) runs in
  the *client* process, drawing into malloc'd pixmaps. It has no DRM master and
  nothing in CMA. Reaching the PPA would mean allocating every pixmap as a dumb
  buffer and paying 13.7x on all the CPU drawing that dominates it. Structural,
  not a tuning question.
- **The X shim's compositing inside lvdesk** - same reason, its pixmaps are
  malloc'd. This is what was measured and rejected above.
**Cursor compositing IS a PPA blend candidate** - an earlier draft of this
section wrongly called it free. lvdesk does use the DRM cursor plane
(`kms_cursor_init`/`kms_cursor_move`), but this panel has no hardware overlay:
the plane is **composited by the CPU inside the driver**, in
`esp32s31_lcd_cursor_paint()`, as a per-pixel ARGB8888-over-RGB565 alpha blend
run on every pointer move. "DRM cursor plane" is not the same as "hardware
cursor", and conflating them hides a real per-motion cost.

  What makes it a candidate rather than a certainty:

  - The destination, `lcd->scan_cpu`, is the private scanout buffer and is
    **already in CMA**. No relocation, so the 13.7x penalty does not apply.
  - It runs **in the kernel**, so it skips the DRM ioctl that dominates the
    ~200 us fixed cost measured from userspace - the in-kernel setup was 9 us.
  - The source, `lcd->cur_argb`, is kmalloc'd and would have to move to CMA.
    That is cheap and one-off: it is already copied once per cursor fb change,
    not per move.
  - Two changes needed in the wrapper: `esp32s31_ppa_blend_layers()` currently
    takes a FIXED alpha, while a cursor needs **per-pixel** alpha from an
    ARGB8888 source, and it must blend ARGB8888 over RGB565 rather than one
    format throughout. The PPA supports both; our wrapper does not expose them.
  - Size is the open question: a 32x32 cursor is 2 KB, right at the measured
    blend crossover, so this must be measured in place rather than assumed.

  The cursor's other blit - restoring the vacated rectangle - is a plain copy of
  the same ~2 KB, three orders of magnitude below the 131 KB copy crossover.
  That one stays on the CPU.
- **The driver's damage copy** - `esp32s31_lcd_copy_rect()` picks CPU or PPA per
  rectangle at a ~128 KB crossover. Re-measured after the fix and **the table
  still holds**: 640x384 is 10.9 ms CPU against 6.9 ms PPA (was 9.66 / 6.43),
  and PPA is still 6x worse at 128x128. The fix roughly halved the engine's
  fixed cost but memcpy is cheap, so the crossover did not move. **The copy
  decisions were not distorted by the bug - only the blend ones were.**

**The drag ghost is a copy, not a blend, and it is above the copy crossover.**
`drag_ghost_begin()` snapshots a window with `lv_snapshot_take()` in RGB565 -
no alpha, no opacity set - so dragging blits an opaque ~310 KB image per frame
(500x310). That is comfortably past the 131 KB copy crossover, worth ~1.4-1.6x.
It also has an unusually good memory story: `drag_snap` is written once and
thereafter only read by whoever blits it, so putting it in CMA costs nothing in
CPU access - the 13.7x penalty only bites surfaces the CPU keeps drawing into.
Reaching it needs either an LVGL draw unit or lvdesk doing that one blit itself.

**Alpha compositing into the scanout buffer - smaller than it looks.**

lvdesk renders DIRECT into the mapped dumb buffer, so LVGL's destination is
already in CMA. Every rounded corner, shadow, fade and translucent panel LVGL
draws is a read-modify-write blend - against write-combine memory - and PPA
blend now beats a *cached* CPU blend by 1.7-2.1x at 64x64 and 4.4-5.8x at
256x256 and up. The uncached destination makes the CPU's side of that
comparison considerably worse than the "cached" column.

**LVGL already ships the draw unit for this**: `lvgl/src/draw/espressif/ppa/`,
gated by `LV_USE_PPA` (currently 0). It cannot simply be switched on - it is
written against ESP-IDF (`driver/ppa.h`, `esp_cache.h`, `ppa_do_fill()`), and
on hart1 the PPA belongs to our kernel DRM driver. But its structure - fill,
img and buf dispatch behind LVGL's draw-unit interface - is exactly right, so
the work is re-pointing its three back-end calls at our ioctls rather than
designing anything. Fills need no source surface, so they qualify immediately;
images would need the source in CMA and mostly will not.

One caveat found while surveying: lvdesk **deliberately disables shadows**
(`lv_obj_set_style_shadow_width(..., 0, 0)` in four places), and sets no
opacities, so the desktop's own alpha volume is far lower than a stock LVGL
theme's. The remaining blends are icon-sized images and 4bpp glyph coverage -
both far below the ~2 KB blend crossover, both staying on the CPU. Do not
expect a large win here without first re-introducing decoration that was
removed on purpose.

**Also worth settling while here, though it is not a PPA question:**
`LVDESK_PARTIAL` already toggles rendering into a cached heap buffer with a
copy-out, against rendering direct into write-combine. The comment at that
toggle poses the question and today's 13.7x figure is the first hard evidence
that rendering against uncached memory - not the copy - may be the real cost.
Measure it: fresh boot per arm, 5+ runs, discard warm-up.
