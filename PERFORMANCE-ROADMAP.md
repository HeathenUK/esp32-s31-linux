# Performance roadmap

Ordered by expected value. Every figure here was measured on the board, not
estimated. The measurement that can be trusted is **throughput displacement**:
run `cpubench` with and without the load and compare, because tick-sampled CPU
percentages on this port are unreliable (see "Why not just read top", below).

---

## 1. Raise the SD card clock — biggest remaining win

**Now:** the controller already uses its internal DMA (IDMAC), but the device
tree caps `max-frequency` at 12 MHz and it settles on 10 MHz:

    mmc_host mmc0: Bus speed (slot 0) = 10000000Hz (slot req 12000000Hz)

Measured sequential read: **2.29 MB/s** (19.6 MB in 8.58 s). That is only ~46%
of what even a 10 MHz 4-bit bus allows, and high-speed SD is specified to
50 MHz — so there may be several times the throughput available. The root
filesystem lives here, so this is what makes the whole system feel slow.

**Writes are far worse than reads.** Streaming a 512 MB image onto the card with
`dd bs=1M` (2026-08-18, via the SD imager) sustained only about **125 KB/s** —
roughly 2.5% of what the bus allows, and ~18× slower than reads on the same
card at the same clock. Reads and writes differing by that margin points at the
driver rather than the clock: the write path is the first thing to look at
before raising `max-frequency`, because a clock change will not fix a factor of
eighteen. `DW_MMC_QUIRK_LOST_IRQ_POLL` is the prime suspect — if writes complete
via poll timeout rather than interrupt, every transfer pays the poll interval.

**Before changing the number, understand two things:**

- Why the cap is so conservative. Signal integrity on the Korvo-1's SD traces
  is a plausible reason, in which case the ceiling is physical.
- `DW_MMC_QUIRK_LOST_IRQ_POLL`, which the port sets in
  `drivers/mmc/host/dw_mmc-pltfm.c`. The driver is polling to paper over lost
  SD interrupts, and that may be *why* someone kept the clock low. Raising the
  clock without understanding it risks trading throughput for corruption.

**Verify with:** the same `dd` read, plus a large write-and-read-back compare to
prove data integrity at the higher clock, across several remounts.

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

