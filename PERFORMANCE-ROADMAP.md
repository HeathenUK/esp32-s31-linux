# Performance roadmap

Ordered by expected value. Every figure here was measured on the board, not
estimated. The measurement that can be trusted is **throughput displacement**:
run `cpubench` with and without the load and compare, because tick-sampled CPU
percentages on this port are unreliable (see "Why not just read top", below).

---

## 1. SD card — done, and the CPU cost was not what it looked like

**Clock:** 10 -> 40 MHz. `ESP32S31_SDMMC_LS_DIV` pinned the CIU source at 80/8,
so `dw_mmc` sat at divider 0 with nowhere to go; the device tree's 12 MHz cap was
never the constraint. 40 MHz is the ceiling for this divider (the next step is
80 MHz, past the 50 MHz high-speed limit), and high speed at 4 bits is the
ceiling for the board: UHS-I needs 1.8V signalling and the card reports no S18A.

**Where the CPU actually went.** A sustained read appeared to cost 60% of the
core, which is absurd for DMA. Three suspects were ruled out by measurement
before the real one was found:

- the lost-IRQ poll: runtime toggle, no measurable difference either way;
- interrupt overhead: ~1170/s for a 32 MB read, ~6% at any plausible cost;
- cache maintenance: counters in the S31 cache driver put it at **1% of wall
  clock**, despite the plausible story about busy-waiting on a hardware block.

A profile settled it. The time goes to `get_page_from_freelist`, `shrink_node`,
`shrink_slab`, `shrink_folio_list` and `kswapd` — **page reclaim**, not block
I/O. `dw_mci_edmac_start_dma` is 37 samples out of 2710. `min_free_kbytes` was
256 kB, so a streaming read allocated page-cache pages faster than kswapd could
free them and every allocation reclaimed synchronously.

    2.29 MB/s   where this started, 10 MHz
    5.11 MB/s   40 MHz + not writing back buffers the card is about to overwrite
    6.60 MB/s   + min_free_kbytes 1024, readahead 512K
   14.95 MB/s   O_DIRECT, which allocates no page cache at all

CoreMark under load, against 890 idle: 411 through the page cache, 691 with
O_DIRECT — and much of that remainder is `dd`'s own userspace copy, not the
driver. **DMA on this SoC is close to free, as it should be.**

**If more is wanted**, the honest order is: use O_DIRECT for bulk I/O that does
not want caching (already 15 MB/s, over half the theoretical ceiling); then look
at readahead and reclaim behaviour again, because 14 MB of RAM is the real
constraint on cached I/O and no driver change will alter that. A bounce buffer in
SRAM was considered and is *not* justified — it was proposed when cache
maintenance looked like the bottleneck, and cache maintenance is 1%.

**Ruled out as hardware options, checked rather than assumed:** the BitScrambler
cannot reach SDMMC (its attach list is GDMA peripherals — AES, GPSPI2/3, I2S,
LCD_CAM, PARL_IO, RMT, SHA, UHCI — and SDMMC drives its own IDMAC), and GDMA
cannot perform a bounce copy because a DMA write into cached page cache
reintroduces the coherency problem being avoided.

**Profiling.** This kernel ships without `CONFIG_PROFILING`/`KALLSYMS` to save
1.2 MB. Re-enable both and add `profile=7` to the cmdline to get `readprofile`
back; use 7 rather than 2, or the profile buffer and symbol table together will
OOM a 14 MB machine.

---

## Memory: 14.4 MB, and where it goes

Low `MemFree` during I/O is page cache doing its job, not a leak — it is
reclaimable and handed back on demand. `MemAvailable` after dropping caches is
**6.7 MB of 14.4**, which is comfortable.

Measured, rather than read off RSS:

    Slab                  ~3.2 MB   unreclaimable; the largest single consumer
      kernfs_node_cache     727 KB  sysfs metadata, scales with registered devices
      inode_cache           429 KB
      kmalloc-1k            304 KB
      64-byte merged cache  279 KB
    Reserved (device tree) ~1.1 MB  framebuffer 1 MB, audio 64 KB, opensbi 64 KB
    Userspace              ~0.2 MB  everything optional, together

**Trimming userspace is not worth doing.** Killing bluetoothd, dbus-daemon,
crond, syslogd and klogd together returns about **170 kB** — RSS suggests
bluetoothd alone holds 1.9 MB, but almost all of that is shared libc pages that
stay resident for other processes anyway.

The consumers are kernel-side, so savings come from dropping subsystems, which
removes both code and the sysfs nodes behind `kernfs_node_cache`. Keep
`CONFIG_SLUB_TINY`: disabling it to get slab introspection cost ~390 KB of slab,
which is a fair measure of what it saves.

To look at slab again, `SLUB_TINY` must come off (it is mutually exclusive with
`SLUB_DEBUG`/`SLUB_SYSFS`), so the numbers shift slightly while you measure.

**There is little left to reclaim, and the obvious candidates are already ruled
out:**

- **The 1 MB framebuffer cannot shrink to 768 KB.** See the comment in
  `shared/s31_memory_layout.h`: the coherent pool allocator rounds the request
  to `get_order()` = order 8, so a snug pool fails with -ENOMEM however well it
  appears to fit. Recovering that 256 KB means not allocating it from a coherent
  pool at all — `ioremap` of a plain reserved region — which is a real change.
- **Kernel features cost flash, not RAM.** This is an XIP kernel, so dropping a
  subsystem removes text from flash and only its data structures from memory.
  Expect far less than on a normal system.
- **`SReclaimable: 0` is misleading.** The slab does yield: walking the whole
  filesystem grew it by 536 KB and `drop_caches=2` returned ~400 KB.
  `vfs_cache_pressure` at 500 or 1000 changes nothing, so the ~3.45 MB floor is
  in use rather than tunable.
- Remaining levers are small and cost diagnostics: `LOG_BUF_SHIFT` 16 -> 15 is
  32 KB of dmesg history, and `CONFIG_SWAP` off is tens of KB on a board with no
  swap device.

---

## 2. Wi-Fi / Bluetooth transport — measured, and not worth optimising

**Struck.** This entry claimed 24 `memcpy` sites in
`esp32s31-hosted-sram.c` were moving every packet through hart0's SRAM ring with
the CPU. Measured, the premise does not hold twice over.

**The datapath is not 24 copies.** Most of those sites are control plane —
station info, slot state, serial reassembly, IE building — and never touch a
packet. Per packet it is two copies each way:

    TX  kmalloc staging frame -> memcpy(skb->data) -> memcpy_toio(slot) -> kfree
    RX  memcpy_fromio(slot) -> staging -> napi_alloc_skb -> skb_put_data

One staging copy in each direction is genuinely redundant, and TX also does a
kmalloc/kfree per packet. So there *is* something to remove.

**But copying is under 1% of the cost.** Measured against a real access point:

    RX  24 MB download   0.35 MB/s (2.8 Mbit/s)   CoreMark 316 of 879   ~64% CPU
    TX  UDP blast        0.15 MB/s (1.2 Mbit/s)   CoreMark 169 of 879   ~81% CPU

At 0.35 MB/s with two copies, total copy volume is 0.7 MB/s against ~90 MB/s for
a memcpy — 0.8% of the CPU. TX works out at 0.3%. Removing a redundant copy
would buy a fraction of a percent while 64-81% goes elsewhere.

**And the transport is not where it goes.** The same generator aimed at loopback,
which touches no SRAM ring, no doorbell and no hart0, costs the same:

    loopback   0.12 MB/s,  91 pps   CoreMark 297 of 830   ~64% CPU
    wlan0      0.26 MB/s, 195 pps   CoreMark 314 of 830   ~62% CPU

Loopback is no cheaper and moves less. The per-packet cost lives in the network
stack and syscall path on this CPU, not in the ESP-Hosted transport. (Both were
measured under CoreMark contention, so compare them with each other rather than
reading either as peak throughput.)

**If network performance matters**, the lever is packets per second, not bytes:
fewer, larger packets, and GRO/GSO. Rewriting the transport's copy path is not
the answer, and profiling to find the real hot function needs finer symbols than
this kernel can afford — `profile=7` with partial kallsyms misattributes badly
enough to show thousands of samples in `kernel_init`, which runs once at boot.

**Unrelated defect found while testing:** hart0 logs
`s31_wifi_cfg: dropped Wi-Fi message type 23 after retries` during scans. The RPC
path is losing messages; worth chasing on its own terms.

**Note for testing:** the network is a hidden SSID, so it never appears in a
scan and `wpa_supplicant` needs `scan_ssid=1` in the network block.

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

