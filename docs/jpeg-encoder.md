# The hardware JPEG encoder

The S31 has a JPEG codec at `0x20344000`, immediately below the PPA
(`0x20345000`) and the 2D-DMA (`0x20346000`), with both encode and decode
declared in `soc_caps.h`. This is the state of driving it from Linux.

**Status: the engine runs and the container is right; the picture is wrong.**
Do not believe the file size alone - a structurally perfect JPEG that decodes to
black is exactly what this produced for several hours.

## What is established

- **Addresses, verified rather than inferred.** `DR_REG_JPEG_BASE` is
  `0x20344000` from `reg_base.h`. `JPEG_CTRL0` is `HP_SYS_CLKRST + 0xb8`, and
  the PPA's `+0xb4` already appears in our DT as `0x205870b4`, so JPEG's is
  `0x205870b8`; the same check gives `0x205862a0` for its memory-LP register.
  The interrupt is **101**: computing the index of `ETS_JPEG_INTR_SOURCE` in
  the `interrupts.h` enum gives 101 and the same computation gives 102 for
  `ETS_PPA_INTR_SOURCE`, which is exactly what the DT already says.
- **It shares the PPA's 2D-DMA**, so it lives in that driver: one owner of the
  dma2d registers, one descriptor pool, one mutex. A separate driver would have
  to map dma2d twice and invent an arbitration protocol.
- **The descriptor layout is correct** - checked field by field against
  `dma2d_descriptor_align8_t`: `vb[13:0] hb[27:14] err_eof[28] dma2d_en[29]
  suc_eof[30] owner[31]`, then `va[13:0] ha[27:14] pbyte[31:28]`, then
  `y[13:0] x[27:14] mode[28]`.
- **RGB565 in is native** (colour space 2), which is the panel's format, so
  there is no conversion pass.
- **The hardware emits only the entropy-coded scan.** SOI, DQT, DHT, SOF0 and
  SOS are written in software here, and `tailer_en` makes the engine append
  EOI. The tables in the registers and the tables in the markers must agree.
- **It is fast and deterministic**: 800x480 encodes in **7.1-7.9 ms**, and five
  consecutive runs produced byte-identical output. Quality scales sensibly -
  q30 15,003 bytes, q60 19,956, q80 25,951, q95 42,144.
- The output is a valid file: `file` reports "JPEG image data, JFIF standard
  1.02, baseline, precision 8, 800x480, components 3".

## Three real bugs, found and fixed, none of them sufficient

1. **The produced length is reported on different fields than it is programmed
   on.** The output buffer size goes in on `vb`/`va`; the engine writes back what
   it actually produced on `hb`/`ha`. Reading back the fields that were written
   returns the buffer size - a plausible number rather than an obvious failure.
2. **The codec finishing is not the data landing.** `JPEG_INT_DONE` says the
   encoder is done; the output channel has not necessarily written the
   descriptor back yet. Without waiting for the receive channel's `SUC_EOF`, the
   length read back is whatever was programmed.
3. **The Huffman minimum-code table needs a sentinel and a shift.** For a bit
   length with no codewords the hardware wants **0xFFFF**, not the next code in
   sequence, and each minimum code is left-justified by `15 - i` because the
   register compares against a 16-bit field.

## The remaining fault

Every decode shows **the first MCU row as neutral grey and everything below it
black**, for a real desktop frame and equally for a solid-red test buffer filled
by the PPA. Grey is DC = 0, so the first row decodes as "no signal" and the rows
after it drift as the DC differences accumulate against tables the decoder does
not agree with. The payload is not empty - 119 KB at q85 - so the engine is
encoding *something* for every MCU.

Ruled out: descriptor field placement, the source address (the panel shows the
right image, and a PPA fill reproduces the fault), buffer contention with
lvdesk's framebuffer (the fault persists with lvdesk stopped), and the
quantisation tables (natural order into the registers, zig-zagged into DQT,
exactly as the vendor driver does).

Still suspect: the DHT programming order or the value-table lengths, and the
`dma2d` colour-space conversion on the source channel, which the vendor driver
explicitly sets to none for JPEG and which this driver leaves at whatever the
PPA last used.

## A trap this created

The bring-up interface takes raw physical addresses and the only bound is "in
the reserved window". That window is the **CMA pool lvdesk allocates its
framebuffer from**, so encoding into it overwrites the desktop - the panel went
solid grey mid-session. Stopping lvdesk first is a workaround; the fix is for
the driver to own a buffer rather than accepting an address.


## Three more bugs, from actually reading the vendor code

Prompted by tiny386's P4 screenshot path, which does at least one thing this
driver was not.

**The encoder must not be given Huffman tables.** `esp_driver_jpeg`'s encode
path never writes the DHT registers - it only emits the DHT *marker* in
software. The registers exist for the decoder, which has to load whatever
tables arrive in the file it is handed. The encoder has the standard tables
built in, so programming them here was overwriting good tables with a
hand-rolled encoding of the same data. (The min-code format was wrong too:
absent bit lengths want a 0xFFFF sentinel and each code is left-justified by
`15 - i`. Both are now moot, and the code is gone.)

**Cache maintenance, in both directions, in the right order.** DMA memory is
cached on this SoC, so the source must be flushed before the engine reads it -
tiny386 does exactly this - and the destination invalidated. The order matters:
invalidating the destination *after* writing the header discards the header, and
the file then starts with whatever pixels were in that memory. Invalidate first,
then write the header, then start.

**SOS must be the last marker.** The header is padded to a cache line so the DMA
can append at an aligned offset, and that padding was a COM segment emitted
*after* SOS. Everything after SOS is entropy-coded data, so its 0xFF read as an
unstuffed marker and every decoder stopped there. Padding now goes before SOS,
sized so that SOS's own 14 bytes land on the boundary.

With those fixed the output is no longer uniform: it has real structure that
tracks the screen. It is still not a correct picture - the geometry or the
sampling of the source fetch is wrong - so the remaining suspect is the 2D-DMA
block geometry (`hb`/`vb`, macro-block size) or `pixel_reverse` for RGB565 byte
order.

## The bring-up interface was dangerous, and is gone

It took a destination physical address and bounds-checked it against the
reserved window. That window is a **reusable** CMA pool: when it is not holding
DMA buffers it holds movable pages, page cache included. Writing into it
corrupted the running system twice - once painting over lvdesk's framebuffer so
the panel went solid grey, and once zeroing the pages backing busybox, after
which `ls` and `cat` no longer existed.

The driver now owns a 512 KB buffer from `dma_alloc_coherent()`, the debugfs
write takes only `<src> <w> <h> <quality>`, and the result is read back from a
`jpeg_out` blob. Nothing in the loop touches `/dev/mem`.


## Working

Two more faults, and one of them was not the driver at all.

**The scanout address is allocated, not fixed.** It moved from `0x50800000` to
`0x50900000` when the display client restarted, and the encoder was being
pointed at the old one. That produced a perfectly valid JPEG of a buffer nobody
was displaying any more, which is indistinguishable from an encoder bug and
sent several hours in the wrong direction. `screenshot.py` has always read this
address from debugfs for exactly this reason; the rule was written down in this
repo and ignored anyway. Never hardcode it.

**Macro-block reorder has its own enable bit.** Setting
`out_macro_block_size` to 16x16 is not sufficient - `out_reorder_en_chn`
(OUT_CONF0 bit 16) is what actually turns the raster fetch into the MCU order
the encoder consumes, and **only TX channel 0 has the feature at all**. Without
it the picture is recognisable but sheared, with a sawtooth along every edge
whose period is the fetch block width. That is why sweeping the block width
changed the artefact without ever fixing it: the geometry was right the whole
time and the reordering was simply off.

The bit is cleared again after each encode, because the PPA shares TX0 and does
not want reordering.

### Numbers

800x480 RGB565 to baseline JPEG, 4:2:0, read straight off the panel:

	 quality   bytes    encode
	   30      12,422   7.28 ms
	   50      14,096   7.13 ms
	   70      16,569   7.16 ms
	   85      19,745   7.24 ms
	   95      26,255   7.42 ms

Five consecutive runs at q85 produced **19,745 bytes every time**. Encode time
is independent of quality, as it should be for a fixed-function DCT - only the
entropy-coded output changes size.

For comparison, the software path reads all 768,000 bytes of the frame over the
console; this compresses in 7 ms and transfers ~20 KB.

`scripts/board/screenshot-hw.py` drives it end to end: it reads the live
scanout address and geometry from debugfs, triggers the encode, and pulls the
result back base64 over the console.

### Still worth doing

- The destination is invalidated in full (512 KB) on every encode. Only the
  header and the produced payload actually need it.
- Decode is unimplemented. The hardware supports it, and unlike encode it *does*
  need the DHT registers programmed, because it must load whatever tables
  arrive in the file.
- The encode is polled rather than interrupt-driven. The codec has its own
  interrupt (101) and at 7 ms a sleeping wait would free the CPU.

## What it costs to record, and where the cost was

The point of the encoder is MJPEG capture of the running system that does not
change the system it is recording. Measured by CoreMark throughput displacement
(the only method that works here - see `docs/current-state.md` on why absolute
CPU% lies), full 800x480 frames:

	                                      10 fps        20 fps
	 shell loop (echo + usleep)            -42%           -
	 fork-free pacer                       -23%          -42%
	 no per-frame ioremap, scoped flush    -20%           -
	 codec interrupt instead of polling    -15%          -27%
	 no per-frame dev_info                 -5.5%         -10%

Five things, in the order they were found, and the last one dwarfed the rest:

1. **The harness forked.** `usleep` is a busybox applet, so pacing the loop in
   shell forked a process per frame and cost more than the encode did.
   `rootfs/jpegcap.c` opens the control file once and paces with
   `clock_nanosleep`. **Measure the harness before optimising the driver.**
2. **`ioremap_wc()`/`iounmap()` per frame** to write the header, left over from
   when the destination was a physical address from userspace. The buffer comes
   from `dma_alloc_coherent()` and already has a kernel mapping; each
   map/unmap pair edits the vmalloc area and flushes the TLB.
3. **Invalidating the whole 512 KB output buffer** every frame when only the
   header region needs it.
4. **Polling.** ~7 ms per frame of `cpu_relax()`, then of `usleep_range()`.
   The codec has its own interrupt; wiring it saved ~5 points.
5. **`dev_info()` on every encode.** This was the largest single cost by far -
   from -15% to -5.5% at 10 fps. It writes to a 1 Mbps serial console
   synchronously, so a ~100-byte line is ~1 ms of console time per frame, and
   it also floods the ring buffer and scrolls away the `scanout started` line
   that tooling parses geometry from. It is `dev_dbg` now.

**No source flush.** The vendor's screenshot path flushes its input, but that
path encodes a buffer the CPU has just written. This encodes the scanout buffer,
which the display driver fills by DMA and has already cache-maintained. Cleaning
768 KB per frame cost time and evicted whatever the rest of the system was
working on. A caller handing in a CPU-written buffer must flush it itself.

### The floor, and what is left

The engine takes ~7.3 ms per full frame, so at 10 fps it is busy 7.3% of the
time, and it reads 768 KB of PSRAM per frame - 7.7 MB/s - on a board where
PSRAM bandwidth is the ceiling for everything. Encoding a 23x smaller frame at
the same rate only recovered 4.7 of 19.7 points at the time it was tried, so
bandwidth is a real but secondary term.

At -5.5% for 10 fps this is usable for measurement. Getting further means not
encoding frames that nobody changed: the display driver already tracks damage,
so a capture driven from its commit path would cost nothing on an idle desktop
and full price only while something is moving - which is exactly when a
recording is worth having.

### Would moving the encode to hart0 help?

Not for the part that is left. What remains is the engine's own duty cycle and
its PSRAM traffic, and neither moves if a different hart programs the registers
- the DMA reads the same 768 KB either way. Worse, the JPEG codec and the PPA
share the 2D-DMA, and reorder only exists on TX channel 0, so both must use it:
splitting ownership across harts means one of them races the other through a
channel it cannot see. hart0 would only be attractive for **egress** - it owns
the radio natively, so it could stream frames off the board without Linux
touching the network stack - and that is a separate problem from encoding.
