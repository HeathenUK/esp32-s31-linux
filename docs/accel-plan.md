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
| PPA BLEND | two-layer alpha, solid fill | internal, debugfs only | implemented, unused |
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
