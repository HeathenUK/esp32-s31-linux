# Performance roadmap

Ordered by expected value. Every figure here was measured on the board, not
estimated. The measurement that can be trusted is **throughput displacement**:
run `cpubench` with and without the load and compare, because tick-sampled CPU
percentages on this port are unreliable (see "Why not just read top", below).

---

## 1. SD card — clock raised, CPU cost still unexplained

**Done (2026-08-18):** the card clock is 40 MHz, up from 10. The device tree's
12 MHz cap was never the binding constraint — `ESP32S31_SDMMC_LS_DIV` pinned the
CIU source clock at 80/8 = 10 MHz, so `dw_mmc` sat at divider 0 with nowhere to
go. `LS_DIV = 2` gives 40 MHz, and that is the ceiling for this divider: the next
step is 80 MHz, well past the 50 MHz high-speed limit.

    sequential read   2.29 -> 4.25 MB/s
    sequential write          1.62 MB/s
    integrity         cmp against a RAM reference, 3/3 pass

The old "corruption at 40 MHz" was `md5sum` miscomputing through the vendor
hardware-loop extension in libc, not the card. Verify with `cmp` against a known
reference, never a checksum alone.

**Already at the hardware ceiling:** the bus is 4-bit, which is the maximum for
SD (8-bit is eMMC). UHS-I — SDR50, SDR104, DDR50 — is unreachable: those need
1.8V signalling and the card reports no S18A in its OCR (`0x00300000`, 3.3V
only). So high speed at 4 bits, ~25 MB/s theoretical, is the hard ceiling.

**The open problem is CPU cost.** A sustained read drops CoreMark from 890 to
353 iterations/sec — a 60% loss. Two candidates are ruled out by measurement:

- **Not the lost-IRQ poll.** It is now a runtime toggle
  (`/sys/module/dw_mmc/parameters/lost_irq_poll`), so both arms run on one boot.
  With it off, CPU displacement is identical (2.58 s both) and reads are
  slightly *faster* (3.37 vs 3.04 MB/s), with integrity still passing. It may be
  removable entirely — worth deciding deliberately rather than leaving a
  workaround in place that costs throughput and buys nothing measurable.
- **Not interrupt overhead.** 1170 interrupts/sec during a 32 MB read, about one
  per 8 KB. Even at 50 µs each that is ~6% of the core, not 60%.

The cost is genuinely the I/O path, not the benchmark harness. `dd` copies every
byte into userspace, so an earlier version of this measurement charged its memcpy
to the SD path. Serving the same copy volume from page cache separates them: the
copy accounts for 6%, the I/O path for the rest.

**Root cause: cache maintenance is a busy-wait on this SoC.** The S31 has no
Zicbom. `drivers/cache/esp32s31_cache.c` programs a hardware block and spins on
its done bit with interrupts disabled, and it issues every writeback-class
request *twice* as an ESP-IDF errata workaround. So DMA is free but the coherency
around it is not, and it costs in proportion to bytes transferred.

**Fixed (2026-08-18):** `arch_sync_dma_clean_before_fromdevice()` returned true,
so a read wrote back its whole destination before the transfer — memory the card
was about to overwrite — then invalidated it after. Turned off (page-aligned
block I/O buffers cannot share a cache line with live data), leaving a runtime
parameter `dma_clean_before_fromdevice` to restore it:

    sequential read   3.38 -> 5.02 MB/s      (~a third less CPU per MB)
    integrity         six 6 MB cmp runs, repeated cache-dropped re-reads

**What now limits it.** A read still needs an invalidate before the transfer, to
drop dirty lines that would otherwise write back over incoming data, and one
after, to see it. Two passes over the buffer, both busy-waited. Working back from
the measurements the cache block sustains only ~14 MB/s, against ~44 MiB/s for a
software memcpy — so the SD path saturates the CPU somewhere near 7 MB/s no
matter how fast the card or bus is. That is the wall, not the 40 MHz clock.

**The promising direction is a bounce buffer in uncached memory.** DMA into the
uncached PSRAM alias at `0xC0000000` needs no cache maintenance at all, and the
subsequent copy into page cache runs at memcpy speed — 44 MiB/s, three times the
cache block's 14 MB/s. Internal SRAM is also uncached and would serve, but only
32 KB is carved out and hart0 owns the rest, whereas the uncached alias has no
such limit. This is worth measuring before it is worth building: it trades a
busy-wait for a copy, and the copy is the faster of the two.

---

## 2. Wi-Fi / Bluetooth transport — the copy path

`drivers/net/ethernet/espressif/esp32s31-hosted-sram.c` has 24 `memcpy` sites
moving every packet through hart0's shared SRAM ring with the CPU. This is
structurally the same problem as the audio ring that used to cost 18.7% of a
core before Linux took I2S over directly.

**Options:** a GDMA-assisted copy, or having hart0 place data straight into
memory Linux can use, removing one copy entirely.

**Measure first.** Get a throughput baseline (iperf-style, or a large HTTP
fetch) and the displacement it causes. If the radio tops out well below the
point where copying matters, this is not worth the disruption.

---

## 3. Framebuffer console — stop the per-blink atomic commit

The LCD already scans out via AXI GDMA and flushes only damaged scanlines. But
fbcon's cursor plus deferred I/O triggers a **full DRM atomic commit** on every
blink, costing ~1.8% of the core at idle (measured: cpubench 3.086 s with the
cursor blinking vs 3.028 s with it off).

Scanout is free-running from a fixed buffer, so damage needs only cache
maintenance — not a commit. A custom `.dirty` callback in
`drivers/gpu/drm/espressif/esp32s31-lcd.c` doing just the `dma_sync` should
remove nearly all of it. Small, self-contained, low risk.

---

## 4. Stop the per-USB-interrupt kworker wakeup

Descriptor DMA (commit `1930f6ebc`) already fixed the *cost*: USB fell from ~7%
of the core to **0.6%**. What remains is purely a reporting problem — the shim
still schedules a work item on every interrupt, waking a kworker ~1000 times a
second, which is what makes an idle board claim ~99% system time.

**Two approaches are already known to fail — do not repeat them:**

- Re-enabling the interrupt inline instead of via the work item **breaks HID
  input**: the first key pressed then repeats. Confirmed by ear.
- Removing the deferral in `_dwc2_hcd_irq` entirely is worse still: USB
  overhead triples, from 7.3% to 21%, because the handler gets re-entered
  mid-flight instead of processing a stable snapshot.

Any fix must keep the DWC level output low across the CLIC return boundary
without waking a thread per interrupt.

---

## 5. Give I2C its interrupt

`drivers/i2c/busses/i2c-esp32s31.c` busy-waits with `readl_poll_timeout` and
never requests the IRQ that is already in its DT node. Irrelevant for codec
register writes, which are microseconds. It becomes worth doing when the GT1151
touch controller lands and starts polling the bus continuously.

---

## Why not just read `top`

Precise CPU accounting (`VIRT_CPU_ACCOUNTING_GEN`) is 64-bit only and this is
rv32, so the kernel samples CPU state at each timer tick. Everything on this
board wakes *on* tick boundaries — the USB work item, fbcon's cursor timer,
deferred I/O's `HZ/20` delay — so tick-synchronised work is charged whole ticks
it never used. A kworker costing 1.8% of the core has been observed reporting
59%.

`NO_HZ_IDLE` does not help and currently makes it worse: with ~1000 wakeups a
second every idle stretch is shorter than a jiffy, and idle accounting works in
whole ticks, so two thirds of wall-clock time simply vanishes from `/proc/stat`.
Revisit it once the wakeup rate is down.

Until then, measure with displacement, and sanity-check a suspected consumer by
removing it — e.g. `echo 1-1 > /sys/bus/usb/drivers/usb/unbind` for USB.

---

## Audio: working, with verification outstanding

Playback is confirmed by ear as of 2026-08-18 — a 440 Hz square plays cleanly
at 48 kHz on both channels. Four independent faults had to be fixed to get
there; see the commits and `s31-audio-verify-by-ear` in memory.

Still open, all needing someone at the board (task #14):

- **Channel mapping** — left-only/right-only tones were played but never
  reported on. Establish which side a single connected speaker responds to.
- **Rate accuracy by ear** — 8k/16k/44.1k/96k. Period counting already proves
  the rates exact, but that cannot detect wrong *samples*, which is precisely
  the trap that made this take an evening.
- **Play something real**, not a square wave. A tone can sound right while
  sample packing or interleaving is subtly wrong.

And two follow-ups (task #15): the codec's mixer state needs applying at boot
now that `alsactl` exists, and the es8389 clock-source fix should go upstream —
it is a mainline bug, not board-specific.

**Standing rule from this episode:** never report audio as working on the
strength of xrun counts, sample-rate measurements, register readback or DAPM
state. All four were correct and green while the board emitted pure noise.

