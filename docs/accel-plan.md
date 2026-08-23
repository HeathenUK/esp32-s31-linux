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
