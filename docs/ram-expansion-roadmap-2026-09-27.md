# Radically more RAM for applications: design and roadmap (2026-09-27)

**Scope.** ESP32-S31-Korvo-1 running Linux 7.1 on hart1 (SMP with a lent hart0), 16 MiB of PSRAM. The goal is more RAM for applications with **no function lost**.

**Labels.** MEASURED means the figure comes from a board artifact or from a static probe of committed binaries. Everything else is INFERRED. No board was used in this session.

**Kernel caveat.** Section sizes come from the #397-era nm (`artifacts/gl/glquake/kfast/base-nm.txt`). MemTotal comes from #393. Every byte count derived from nm must be taken again from the #401/#402 System.map before anything is built.

---

## 1. Summary

### 1.1 Where the 16,384 kB goes today

Only one full current-era capture exists: kernel #393, QuakeSpasm fullscreen at t=75 s, `min_free_kbytes` 1024 (`artifacts/gl/glquake/arms/pg-census-0927-003021/census.txt:68-106`). The table below uses one basis only, the LRU sum.

| Consumer | kB | Status | Source |
|---|---|---|---|
| **Outside MemTotal** | **1,048** | MEASURED (16,384 − 15,336) | census.txt:68 |
| · Kernel RAM image, permanent: .data 213, percpu template 23.8, `.text..fast` 250.9, .sdata 11, .bss 106.5 | ~595 | MEASURED on #397, **cross-kernel** against the #393 MemTotal | base-nm.txt markers |
| · audio_reserved + opensbi_reserved | 128 | MEASURED | 0056 esp32s31.dtsi:175-182 |
| · memblock: mem_map 128, the percpu first chunk (most or all of meminfo `Percpu: 112`), DT, page tables | ~325 | INFERRED by subtraction across #393 and #397 | — |
| **Inside MemTotal (15,336)** | | | |
| MemFree (1,328 of it CmaFree, which unmovable allocations cannot use) | 2,652 | MEASURED | census |
| LRU: Active 2,628 + Inactive 2,364 + Unevictable 200. Holds AnonPages 4,036, file cache and Shmem 328 | 5,192 | MEASURED | census |
| Slab (SLUB_TINY, all of it SUnreclaim) | 4,248 | MEASURED | census |
| KernelStack / PageTables / VmallocUsed | 648 / 532 / 400 | MEASURED | census |
| Pinned CMA: the driver's `scan_gem` scanout, 768,000 B, which lvdesk renders into directly | 750 | MEASURED size | paging-plan-2026-09-25.md:330-346 |
| **In no counter** | **~800-914** | INFERRED. It is ~914 if Percpu is entirely first-chunk; ~650-760 if a separate ~150 kB fullscreen mode buffer also exists (UNVERIFIED: the 152 kB items in the census are SysV/memfd SHM, already counted in LRU) | arithmetic |

**Pinned CMA at the census point.** CMA in use was 3,072 − 1,328 = 1,744 kB. The scanout accounts for 750 of that. The other ~994 kB is either movable LRU pages, which are already counted, or unseen pinned buffers. Candidates for the pinned part: `jpeg_buf`, a mode plane, or a cursor buffer. This **~650-900 kB of unattributed memory is the largest open lead** (M1).

The old "2 × 774,144 B dma-coherent" ledger (current-state.md:6732, 6752-6758) comes from before direct scanout (patch 0047) and does not describe today's board.

**Idle desktop, 2026-09-05 ledger, UP era (current-state.md:6730-6741):**
- Slab 4,284-4,360
- Daemon anon ~1,220: lvdesk 264, s31-bt 212, bluetoothd 176, udevd 160, dbus 84, ntpd 32
- Stacks, page tables, vmalloc and percpu ~1,230
- Page cache 2,600-3,700
- MemFree 2,300-2,740

### 1.2 Why applications see about 5-7 MB

- **Roughly 8.5 MB is gone before any application runs.**
  - ~1.05 MB outside MemTotal
  - ~4.2 MB of slab
  - ~0.75 MB of pinned scanout (~0.9 MB fullscreen, if a mode plane exists)
  - ~1.2-1.6 MB of kernel stacks, page tables and vmalloc
  - ~1.2 MB of idle daemons
- **The owner's figure is most likely the lvdesk tray.** It is computed as MemFree + Buffers + Cached − Shmem − Mapped, never less than MemFree (lvdesk.c:2597-2605). It was calibrated after MemAvailable read 29% low: 4,264 kB against 6,004 kB actually obtainable (lvdesk.c:2552-2564).
- **Fresh-boot readings at t≈62 s** (31 arms, swappiness 60, min_free 512): MemAvailable 4,348-5,032 kB, median ~4,760. The historical peaks of 6.0-6.8 MB match "7 MB at best". No long-uptime series exists.
- **MemAvailable under-reads.** After drop_caches it rose by 1,252 kB (2,764 → 4,016, current-state.md:6720-6721), and SReclaimable reads 0 under SLUB_TINY. That is a problem with the metric, not missing RAM, and it is counted nowhere below.

### 1.3 Top bets and cumulative gain

Each figure is counted once. Overlapping proposals were merged into one item each: `.text..fast` (PM2, PM3, XIP-1, K2, R4), s31-bt (P3, U2, R5) and udevd (U4, R3). The levers split into two classes that must not be added together naively.

**(a) Kernel or unevictable capacity.** These add to MemTotal or free pinned pages, so they help at idle and under load.

| # | Lever | Cons. | Opt. | Basis |
|---|---|---|---|---|
| L1 | Path-qualify the `.text..fast` globs | 16 | 16 | MEASURED ≥16,222 B (#397) |
| L2 | Demote cold whole objects from `.text..fast` | 0 | 134 | MEASURED ceiling on a single workload |
| L3 | Move the percpu template into the freed init region | 24 | 24 | MEASURED 23,820 B (#397) |
| L4 | `coherent_pool=64K` | 0 | 96 | INFERRED pool size |
| L5 | Use the builtin DTB in place in flash | 0 | 20 | MEASURED blob size; that it is copied permanently is INFERRED |
| L6 | Split audio_reserved (step 1) | 0 | 12 | MEASURED unreferenced range |
| L7 | MAX_NR_CONSOLES 63 → 8 | 0 | 126 | ESTIMATE, never measured |
| L8 | s31-bt `MCL_ONFAULT` | 0 | 116 | Mlocked 200 kB MEASURED; the split is INFERRED |
| L11 | `randomize_va_space=1` | 0 | 80 | INFERRED |
| | **Subtotal** | **~40** | **~620** | |

**(b) Swappable anon.** These cut swap traffic and launch-time reclaim. They count at idle; under sustained load the kernel already pushes this memory out (census: udevd 4 kB RSS / 180 kB swap, each shell 4 kB / 252 kB).

| # | Lever | Cons. | Opt. | Basis |
|---|---|---|---|---|
| L9 | udevd exits after coldplug | 0 | 240 | MEASURED 140-160 kB anon + 80 kB tmpfs |
| L10 | Cap ash history | 0 | 560 | MEASURED 280 kB per interactive shell |

**Not counted:**
- **M1 (unattributed ~650-900 kB).** Unknown until located.
- **K1 (tooling buffers).** 512 kB of CMA plus ~1,024 kB of vmalloc, but only on boots where hardware JPEG or recorder tooling has run. That may include armed boot recordings (Phase 0 checks this).
- **L12 (ext4 group info).** ~100 kB, only at a card re-image.
- **Hardware R7.** Roughly +16,256 kB.

**Verdict.** Software adds roughly 40-620 kB of real capacity, which is 1-12% of the owner's ~5 MB. It frees up to ~0.8 MB more at idle (class b). Optimistically, the settled figure could rise by up to about 28%, all without losing function. Nothing here is radical, unless M1 turns up a driver-held allocation. The only lever of 2x or more is a larger-PSRAM part, and it is not known whether one exists.

---

## 2. The levers

**Conventions for every lever:**
- Kernel items need `make linux`, then `make sync-images`, then a `uname -a` check that `#N` matches. Keep `images/ship-401-xipImage` for rollback.
- DTS items also need `make opensbi`, plus an imager rebuild because the imager shares the DTS.
- Every change is a Makefile, module or `/etc` toggle whose default is today's behaviour (CLAUDE.md:386).
- Performance gates always use the §3 protocol.

### 2.1 Memory map

**M1: locate the unattributed ~650-900 kB (Phase-0 lead, 0 kB claimed).**
- *Mechanism.* On the shipping kernel, which needs no debugfs, compare:
  - `/proc/vmallocinfo` (does a `dma_common_contiguous_remap` entry exist for the atomic pool or `jpeg_buf`?)
  - CmaTotal − CmaFree at idle against the 768,000 B scanout
  - `/proc/pagetypeinfo`

  Check whether `jpeg_buf`, the recorder ring or a cursor or mode plane is allocated. C70 (perf-review-2026-09-23.md:180) says direct scanout removed the hardware cursor plane, so whether a cursor background buffer still exists is UNVERIFIED.
- *Decision rule.*
  - Any single allocation above 64 kB held by one of our drivers (esp32s31 LCD, PPA, hosted transport) becomes a lever in that driver, which is allowed under CLAUDE.md:383.
  - If a residue of more than 200 kB stays unexplained, build a default-off `OWNERDIAG=1` variant (PAGE_OWNER + DIAG). Its size check must fail loudly rather than drop the radios or sound. Its totals are for attribution only.
- *Function preserved.* Measurement only.
- *Kill.* Everything turns out to be movable or diffuse `alloc_pages` users.

**L4: DMA atomic coherent pool.**
- *Mechanism.* SOC_ESP32S31 selects DMA_DIRECT_REMAP (scripts/apply-s31-fixes.py:72-74). `dma_atomic_pool_init`, `atomic_pool_*` and `early_coherent_pool` all exist in base-nm. Upstream sizes the pool at max(totalram/8192, 128 KiB), so 128 KiB here, unmovable and counted in no meminfo field. It comes from the buddy allocator because the CMA node has no `linux,cma-default` and CMA_SIZE_MBYTES is 0 (INFERRED: the defconfig CONFIG_CMDLINE is not in the repo).
- *Change.* A Makefile variable, empty by default, that adds `coherent_pool=64K` to CMDLINE_ADD, only for USB_HART0=1 builds. USB_HART0=0 brings back dwc2's atomic `dma_pool_alloc`. Never use `coherent_pool=0`, which silently reverts to 128K.
- *Evidence of low risk.*
  - Every coherent allocation in our patches (LCD, cursor, PPA, JPEG, dw_mmc) uses GFP_KERNEL.
  - GDMA descriptors come from an SRAM `gen_pool` with GFP_NOWAIT kmalloc, not from the atomic pool (patches/0058-esp32s31-gdma-scanout-may-sleep/esp32s31-axi-gdma.c:463-483).
  - The hosted Wi-Fi/BT sources are not in the repo.
- *kB.* 64-96, INFERRED.
- *Function preserved.* The pool grows on demand, and blocking allocations bypass it.
- *Risk.* An atomic burst above 64K returns NULL, silently dropping a Wi-Fi, BT or audio operation.
- *Board.*
  1. Read the `DMA: preallocated N KiB` boot line with conlog.
  2. Then run: Wi-Fi wget at full rate, a BT HID reconnect, A2DP plus the speaker, mjpegrec, SD paging under Quake, and the imager.
- *Gate.* No `DMA: failed` and no pool growth in vmallocinfo.
- *Kill.* The pool is not 128 KiB (or absent), or any DMA failure appears.

**L6: split audio_reserved (step 1 only).**
- *Mechanism.* Replace the node with two: 0x50FE0000/0xC000 (the three rings) and 0x50FEF000/0x1000. The whole last page must stay reserved. Its users:
  - the mailbox at 0xFC0
  - the h1s sampler control word at 0x50FEFFB0 (bootloader/main/s31_vcpu.c:961-965)
  - heartbeat words at 0xFD0-0xFFF (s31_vcpu.c:425-428, 725; s31_vcpu_world.S:84, 256)
  - the CLIC diagnostic `ioremap(0x50fef000)` in patch 0051
- *kB.* 12, MEASURED as unreferenced.
- *Required lockstep.* The hart0 SBI handler rejects guest buffers above `0x50fe0000 - 0x100` (s31_vcpu.c:420, 423) with SBI_ERR_INVALID_ADDRESS (:430). Once 0x50FEC000-0x50FEEFFF becomes Linux RAM, a PIE/HWLoop save that lands there is refused, depending on where the allocator happened to place it. The bounds must therefore change too, which means reflashing the hart0 loader (`hello_world.bin` at 0x20000). Also:
  - delete the stale 0x50FEC000 `dd` comment at s31_vcpu.c:966-967
  - point the freertos_audio `memory-region` at the 48K node
- *Board.* MemTotal +12 kB, both CPUs online, a tinygl/PIE app on cpu1, and an ear test on both the Linux and FreeRTOS rings, over 3 fresh boots.
- *Gate.* Bundle only with another loader+DTS rebuild.
- *Kill.* Any audio, SMP or PIE regression.

### 2.2 Flash / XIP (kernel RAM text)

**L1: path-qualify the fast-object globs.**
- *Mechanism.* The bare globs `*timer.o`, `*hrtimer.o` and `*entry.o` also match:

  | Object | Bytes |
  |---|---|
  | sound/core/timer.o | 8,590 |
  | tcp_timer.o | 3,550 |
  | itimer.o | 2,322 |
  | alarmtimer.o | 1,092 |
  | pcm_timer.o | 470 |
  | dummy_timer.o | 80 |
  | tick-broadcast-hrtimer.o | 118 |

  All of them took 0 of 24,000 GLQuake samples (#397). Fix every glob site in the **shipping** script (patches/0067 vmlinux-xip.lds.S), including the EXCLUDE_FILE lists.
- *kB.* ≥16, MEASURED on #397.
- *Function preserved.* The code runs from XIP flash.
- *Risk.* `timer-esp32s31-systimer.o` also matches `*timer.o`. Fixing the glob once before resurrected its rating-450 clocksource, which is not SMP-safe. Keep `--disable ESP32S31_SYSTIMER_CLOCKSOURCE`.
- *Cloud.* `nm -n` over [__text_fast_start, __text_fast_end) must show no sound/, net/, itimer, alarmtimer or pcm_timer symbols, and no `__*_of_table` entry may move.
- *Board.* `current_clocksource` unchanged, 2 CPUs up, no watchdog clocksource switch after a Quake run. Run pingpong and faultlat over 5 fresh boots.
- *Kill.* Any change in timekeeping.

**L2: demote cold whole objects (`FASTOBJS=lean`, default = current layout).**
- *Mechanism.* `.text..fast` is 250,880 B. Of that, 86,298 B took samples and 165,910 B took none, but that is GLQuake only (5,140 PCs inside the region).
  - Build the hot set as a union from PROF=1 or the hart0 PC sampler, on the exact vmlinux, across: desktop drag (root cursor set), faultlat and random 4K SD reads, A2DP, BT HID, Wi-Fi wget, Doom, Quake, the SDL canary, and one imager stream.
  - Demote one object at a time, largest cold share first: dw_mmc, workqueue, build_utility, blk-mq, the cold parts of sched/core, mmc/core/block.
  - Keep whole: input.o, evdev.o, entry, traps, irq, CLIC, softirq (−19.7% pingpong), fair (0028), spinlock (0067 fails to link without it). Keep the 0062 and 0069 lists.
- *kB.* 0-134, with no claim until the union exists.
- *Performance cost.* Flash is 5.98x slower. Layout swings pingpong by ±15%. The C39 flash repack was CLOSED NEGATIVE (perf-plan:1043-1052).
- *Board.* 5+ fresh boots per arm:
  - pingpong (band 474-497 µs)
  - faultlat p50 and p99
  - cursor latency (uinject + mjpegrec)
  - Quake fps against the ~1.5 fps boot band
  - A2DP ear test

  PROF=1 and DIAG=1 must still fit in 6,160,384 B.
- *Kill.* Any instrument moves outside its band.

**L3: move the percpu template into the freed init region.**
- *Mechanism.* The template spans c0834000 to c0839d0c (23,820 B), below `__init_begin` (c0878000), so it is never freed. The 0067 lds comment (434-449) says the glob problem that forced this placement is gone. Move PERCPU_SECTION into [__init_begin, __init_end), before .sdata and `__bss_start`, under `#ifdef CONFIG_SMP`.
- *kB.* 24, MEASURED on #397.
- *Cloud.* In System.map, runqueues, hrtimer_bases and tick_cpu_device must lie inside the percpu range, and that range inside init.
- *Board.*
  - alive.py across 5 resets (the last percpu misplacement died silently).
  - The initmem "Freeing" line grows by about 24 kB.
  - Cycle `cpu1/online` several times, then run SMP pingpong.
  - Boot the imager once.
- *Kill.* Any failure of SMP bring-up or hotplug.

**L5: use the builtin DTB in place in flash (verify first).**
- *Mechanism.* The blob is 19,976 B and sits inside init (MEASURED). Whether it stays permanently copied via `unflatten_and_copy` is INFERRED, because setup.c is not in the repo. The change touches:
  - head.S (0052:376-381)
  - init.c (0061:308, 1075-1097)
  - setup.c

  It also needs a build-time assert that the DTS has no `rng-seed` or `kaslr-seed`: `fdt_nop_property` on a flash-resident blob would fault.
- *kB.* Either 0 or ~20.
- *Board first.* Confirm the permanent copy exists. Then boot-test the shipping, imager, SMP=0 and PROF=1 kernels, 5 resets each, and `cmp /sys/firmware/fdt` against the built .dtb.
- *Kill.* There is no permanent copy.

### 2.3 Compression / paging

No capacity lever survived review (§5). Paging already supplies the capacity: games run with 15.5-19 MB swapped to the 64 MB SD swapfile. What limits them is fault latency, which belongs to the sd-paging programme.

**P4-metric: an obtainable-memory yardstick (0 kB).** Leave the tray formula as it is. Never fold SwapCached into it: no counter separates clean, unmapped swap cache. Build the allocator arm described in §3.

### 2.4 Kernel

**L7: MAX_NR_CONSOLES 63 → 8.**
- *Mechanism.* All 63 VTs register at boot (/sys/dev has 128 entries). Actual users:
  - lvdesk uses /dev/tty0 (lvdesk.c:762)
  - inittab uses ttyS0 and tty1
  - /dev/console is tty0

  The repo has no chvt, no openvt and no user of tty2 or above.
- *kB.* ~126 KiB ESTIMATE (current-state.md:3070-3074), landing in slab.
- *Function preserved.* fbcon on tty1, the serial console, the getty.
- *Owner decision.* This changes a constant in a uapi header (§4).
- *Measure first.* Build both configs in a container and compare /sys/class/tty counts and the Slab delta over 5 fresh boots.
- *Gate.* Slab drops by at least 64 kB.
- *Kill.* Slab delta below 64 kB, or fbcon restore or the tty1 getty respawn regresses.

**L11: `kernel.randomize_va_space=1` (measure first, owner decision).**
- *Mechanism.* An unrandomised brk puts a small process's heap in the same 4 MB page-table region as its .bss, saving one Sv32 leaf page per process.
- *kB.* 0-80, INFERRED (≈4 kB × 15-20 processes).
- *Measure first.* Phase 0 reads VmPTE per process.
- *Cost.* Weaker ASLR.
- *Kill.* Measured PageTables delta under 40 kB.

**K1: free tooling buffers when idle.**
- *Mechanism.*
  - `jpeg_buf` (512 KiB, allocated with `dma_alloc_coherent` from lcd_reserved CMA, patches/0040 esp32s31-ppa.c:2394-2395) is never freed.
  - `rec_stop` (:3709-3723) never vfrees the recorder ring (default 1,024 KiB, mjpegrec.c:126).
  - lvdesk-qol-plan-2026-09-25.md:2120-2123 lists the fix as still required.
- *Owner baseline.* Not guaranteed to be 0. The kernel arms the recorder at scanout (1.7 s after boot) on boots where `s31-record arm` set the LP_STORE word, and S02s31-vidcap drains that recording. Harness runs that call screenshot-hw.py also allocate `jpeg_buf`. At the census VmallocUsed was 400 kB, so no ring was allocated then. Whether `jpeg_buf` was is unknown (M1).
- *Change.* Module param `jpeg_idle_free_ms`, default 0.
  - Free only under the encode/read mutex, with `jpeg_last_len = 0`, and never while recording or while a recorder fd is open. This avoids a use-after-free in `jpeg_out_read` (:1969-1977).
  - Free the ring only after a completed drain (`read == seq && !running`) or on an explicit `s31-record` ioctl.
  - If re-allocation fails, keep the buffer (CmaFree has been seen at 60 kB under GLQuake).
- *kB.* +512 kB of CMA and +1,024 kB of vmalloc, after tooling has been used.
- *Board.* Over 5 boots:
  - a mid-game screenshot after 10 s idle
  - mjpegrec across an idle gap
  - the S02s31-vidcap drain
- *Kill.* Any screenshot or mjpegrec failure, or a NULL on re-allocation.

**M2: price the kthread and per-CPU cost (measurement with a decision rule).**
- *Mechanism.* KernelStack went from 344 kB (UP, idle) to 648 kB (SMP, game). That ~300 kB is the largest unexplained kernel delta, and it has never been priced.
  - Collect `ps -T` kthreads by type, KernelStack and Slab at idle, at 62 s and at 10 min.
  - Run SMP=0 as a *measurement* only.
  - task_struct shows 90 slots for ~57 tasks; record it.

  NOCB is empty by default (Makefile:459-463) and RPS is a deliberate feature, so neither is a target. Whether the rest of the per-CPU set is structural is UNVERIFIED.
- *Decision rule.* If more than 10 per-CPU kthreads belong to subsystems with no function on this board, price removing them with Kconfig-only switches. Never touch reclaim-path (MMC/swap) workqueues.

### 2.5 Userspace

**L10: cap ash history.**
- *Mechanism, reproduced by the probe on busybox 1.38.0 under s31-qemu.*
  - The stock Buildroot config has `CONFIG_FEATURE_EDITING_HISTORY=999` and `SAVE_ON_EXIT=y`, and `FEATURE_SH_HISTFILESIZE` is not set (build/cloud/probes/ram-userspace/busybox.config:109-111, 1208). busybox.fragment does not override these.
  - Whether the board's busybox uses this config is UNVERIFIED.
  - The heap grows about 0.51 kB per runsh-style line: 336 kB at 600 lines, 528 kB at 999.
  - A **fresh** interactive shell reloads `$HOME/.ash_history`, holding 528 kB before its first command, or 20 kB with `HISTFILE=`.
- *Owner-visible path.* lvdesk's terminal starts `execl("/bin/sh","-sh","-i")` (lvdesk.c:2478), so every terminal window reloads a history that runsh traffic has filled. Menu actions (`-sh -l -c`, :9909) do not.
- *Why the obvious fix fails.* `HISTSIZE` is ignored in this build (ash.c:15151-15158).
- *Fix.*
  1. `CONFIG_FEATURE_EDITING_HISTORY=48` in busybox.fragment (32 lines measured 40 kB).
  2. Rebuild the rootfs and run flash-xip-rootfs (busybox is in XIP_ROOTS).
  3. Truncate `/root/.ash_history` on the card and record that you did.
  4. Leave runsh.py unchanged.
- *kB.* 0-560, class (b). 280 kB per interactive shell (current-state.md:5081-5083).
- *Function preserved.* History and line editing still work, with a shorter list.
- *Board first.* `wc -l /root/.ash_history` and the login shell's smaps.
- *Gate.* Shell Rss+Swap ≤ 60 kB after 10 runsh runs and after a fresh terminal.
- *Kill.* Board smaps shows the 280 kB mapping is not the history list, or `.ash_history` is short.

**L8: s31-bt `mlockall(MCL_CURRENT|MCL_ONFAULT)` with a mandatory pre-touch.**
- *Mechanism.* s31-bt.c:2246 pins every exec-time VMA before `dbus_bus_get`, including a stack VMA of about 132 kB.
  - The audio buffers keep their per-buffer `mlock()` calls (1601-1625).
  - Before calling `mlockall`, pre-touch 32 kB of stack.
  - Behind an env/config toggle.
  - MCL_ONFAULT=4 is in the sysroot, and `mlockall(5)` returned 0 under QEMU.
- *kB.* 0 conservative, 116 optimistic. Mlocked 200 kB is MEASURED. The split is INFERRED from lvdesk's VmStk, since no s31-bt smaps exists.
- *Risk.* The thread is SCHED_FIFO. Under pressure, a first-touch zero-fill fault can enter direct reclaim.
- *Board.*
  - Capture smaps and VmLck first, over 3 fresh boots.
  - Then 30 minutes of A2DP under game load, including stream start/stop and a reconnect after 10 minutes idle.
- *Kill.* Any audible gap or underrun.

**L9: make udevd transient (class b).**
- *Evidence.*
  - The 27 stock rules have no consumer (current-state.md:5118-5122).
  - devtmpfs creates the nodes.
  - lvdesk watches /dev/input with inotify (lvdesk.c:378-390).
  - BlueZ 5.79 built without sixaxis and hid2hci does not link libudev.
- *Change.* `UDEVD_TRANSIENT` in /etc/default/udevd, default 0. Run `udevadm control --exit` inside the existing background subshell, and only after `settle` exits 0. Keep /run/udev by default.
- *kB.* 0-240: 140-160 kB anon plus about 80 kB of tmpfs if the card audit clears it.
- *Function at stake.* Rule processing for devices hot-plugged after the exit (hwdb keymaps, SDL netlink hotplug).
- *Board first.* Inventory the card's `/etc/udev/rules.d` and any libudev `dlopen` in SD-root applications.
- *Kill.* Any failure in the hotplug matrix:
  - uhid BT keyboard
  - BT gamepad
  - hosted USB keyboard
  - SDL joystick hotplug mid-game
  - wlan0 after a hosted re-init
  - keylog

  Also kill if any SD-root app `dlopen`s libudev. Never use mdev (C51).

**L12: ext4 group info at the next re-image (conditional).**
- *Mechanism.* current-state.md:2531-2542 records 954 groups on a 119 GiB card (ext4_groupinfo at 110 kB), while :3040ff records "ext4 -b 4096: 952 → 61 groups" as done. Phase 0 runs `dumpe2fs -h` to settle which is true.
- *kB.* ~100, only if 954 groups are confirmed.
- *Cost.* A re-image loses card state unless that state is captured first. Owner decision.

**U5: long-uptime growth census (measurement).** A static C sampler, started with setsid, writes to the card every 10 minutes for 8 hours idle plus 1 hour of A2DP. It records per-daemon VmRSS, VmSwap, VmData and smaps_rollup, plus the tray value. The reason: s31-bt once grew from 212 kB to 6,996 kB, though under MCL_FUTURE (s31-bt.c:2229-2236). Nothing else may run on the board concurrently, and results are collected over Wi-Fi.

### 2.6 Radical

**R7: a larger-PSRAM part (hardware, and the only 2x lever).**
- *What already fits.* The aperture is 0x50000000-0x53FFFFFF, and pmaaddr7 0x147FFFFF decodes to a 64 MiB NAPOT region. The limiting constant is `S31_PSRAM_SIZE 0x01000000` (shared/s31_memory_layout.h:7).
- *Lower-risk software shape.*
  - Keep 0x50000000-0x50FFFFFF exactly as it is (audio, mailbox, OpenSBI, splash FB 0x50C00000, CMA).
  - Add a second DT range starting at 0x51000000, which costs 128 kB more mem_map.
  - Change only S31_PSRAM_SIZE, the loader mapping loop (main.c:250-267) with its lower-bound check, and the DTs.
- *kB.* ~+16,256 for R32 (INFERRED).
- *Status.* An open question for Espressif; the repo knows only the E1H16R16V part.

---

## 3. Roadmap and measurement protocol

**Protocol (CLAUDE.md, "Measuring anything").**
- **Repeats.** Fresh boot per arm, 5+ repeats, report min/median/max and the tail. Discard warm-up.
- **Identity.** Confirm `uname -a #N` and record card state.
- **Boot counts.** Deterministic MemTotal deltas need one boot. MemAvailable, latency and fps need 5.
- **Capture points.** **Cold-settled (t=62 s)** and **settled (~10 min)**.
- **Headline metric: the obtainable allocator.** It uses mmap and touches pages with swap on, and runs at `oom_score_adj=1000`. It stops at its first own major fault, at any movement in pswpout or pgsteal_direct, or when MemFree − CmaFree drops below 2 × min_free. It runs under setsid with output to the card, as the last step of its arm.
- **Also report.** MemAvailable, the tray value, and a drop_caches arm. drop_caches is a measurement arm only, never a runtime action.
- **Board etiquette.** No board tool while a measurement or conlog holds the port. Run `xsetroot -cursor_name left_ptr` before cursor tests.

**Phase 0: baseline, no build (about 1 board day, on #401/#402).**
- Memory state: full meminfo, vmallocinfo, zoneinfo, pagetypeinfo, per-process smaps_rollup and VmPTE, CmaFree, `ps -T` plus KernelStack. Take these at 62 s and at 10 min.
- Allocations to check: tmpfs `df`, `ipcs -m`, and memfd mappings. This checks whether the unowned 752 kB idle Shmem from 2026-09-03 persists.
- Boot log via reset.py + conlog: the `Memory:` line, the DMA pool line, and the OpenSBI heap banner.
- Tooling state: whether the boot recorder is armed, and whether `jpeg_buf` or the ring is allocated. Test for a cursor buffer.
- Per-component captures: s31-bt smaps, login-shell smaps, `wc -l /root/.ash_history`, `wc -c /sys/firmware/fdt`, the card's udev rules, `dumpe2fs -h`.
- Swap map accounting: 64 MB of swap is 16,384 slots at 1 B each, about 16 kB (ledger line).
- Start U5.
- **Phase 0 decides M1, M2, L4, L5, L8, L10, L11 and L12.**

**Phase 1: userspace, rootfs only.**
- L10.
- L8 (toggle plus ear test).
- L9 (default off until the matrix passes).
- Expected: 0-0.9 MB, mostly class (b).

**Phase 2: one kernel relink bundle.**
- L1, L3 and L4, each behind its own switch.
- Take MemTotal per image, then the performance gates on the bundle over 5 boots; bisect on failure.
- Expected: 40-136 kB.

**Phase 3: L2.** One object per A/B/A (pp-boots.sh, 3 boots per arm), then a 5-boot confirmation. Expected 0-134 kB.

**Phase 4: owner-gated.**
- L7 (container measurement first)
- L11
- L5 (only if the DTB is copied permanently)
- L6 (only alongside a loader+DTS rebuild)
- L12 (at a re-image)
- Any M1 driver fix

**Phase 5.**
- K1 (default-off param).
- Hardware track: the R7 question for Espressif, plus a static audit of the constants.

---

## 4. Owner decisions needed

1. **L7 and a uapi constant.** CLAUDE.md:383 says "**Off-the-shelf software only.** Do not patch Xorg, mesa, or mature kernel drivers." CLAUDE.md:362 says "**The slab is NOT a lever - do not propose reclaiming it.**" The case for an exception: no function is lost, the 300-500 kB verdict never priced unused VT minors, and dw_mmc 0063-0066 are precedent. Needs a measured Slab delta first.
2. **L5 and upstream arch setup.c.** The same rule applies. The case: this is XIP-port code the project already carries (patches 0003, 0052, 0061).
3. **L9 against "without jeopardising any functionality".** After the exit, hot-plugged devices get no rules and SDL hotplug events stop. Is a verified matrix plus a default-off toggle acceptable?
4. **L10.** 48 lines of history instead of 999, and an edit to card state. CLAUDE.md:330: "**The SD card holds state the repo does not.**"
5. **L2, RAM versus measured speed.** CLAUDE.md:358: "**Memory is the binding constraint**, not CPU". But `.text..fast` exists for measured latency wins. Is "no instrument outside its band" the right bar?
6. **L11.** Trade some ASLR for ≤80 kB?
7. **L12.** A card re-image, which loses on-card state unless captured.
8. **Microphone capture ring.** Declining the mic is not consent to remove the FreeRTOS capture ABI (PM6 step 2 was killed). Confirm it stays.
9. **Hardware.** Is a respin or module swap in scope, if an S31 R32/R64 exists?
10. **Defaults.** Every change ships as a toggle defaulting to today's behaviour (CLAUDE.md:386). Flipping each default is the owner's call after its gate passes.

---

## 5. Rejected ideas

| Idea | Verdict and reason |
|---|---|
| zram, any size or cap | Rejected 5 times (etc/s31-swap.conf). Latest: 5.4 vs 7.6 fps. mem_used was 1,097,728 B for 156,861 B compressed, and 258 of 569 pages were same-filled, which SD swap stores for free. |
| zswap / zram writeback | The gating experiment already failed: "zram compaction. Freed 0 kB" (current-state.md:595). No MEMCG, so shrink_worker is inert. Expected gain ≤ 0. |
| Compressed page cache | Needs mm-core patches (CLAUDE.md:383). Text is already XIP. |
| Settle-time MADV_PAGEOUT (P1/U3/R1/E1) | Rejected at perf-plan-2026-09-23.md:1465: "MADV_PAGEOUT schemes (X1-29 / C55: queue SD writes, do not free)". faultlat.c:19-22 confirms the pages sit in swap cache, which neither MemAvailable nor the tray counts. No new evidence. |
| Foreground MADV_COLD/WILLNEED (P2); memcg memory.low | 0 kB. memcg failed at first measurement (20.2 → 18.2 fps, ~85 µs per fault). The non-game set is ~0.75-0.85 MB. |
| SIGSTOP of hidden X clients; drop_caches at runtime | Stalls the xlite rings and timers; evicts hot SD text; moves the tray by about 0. |
| Shrinking CMA | Reusable: 4 → 2 MiB gave +136 kB. 3 MiB is the floor for the scanout plus one 800x480x32 client. |
| fbdev console buffer | Already released on DRM master_set (native-800x480.md). |
| OpenSBI RW shrink (PM5) | NAPOT rounding (sbi_domain.c:106, 768-776) leaves the "freed" pages PMP-denied. Breaks the s31_memory_layout.h:171-175 asserts. |
| Removing the audio capture ring | Removes function; removing capture previously cost playback quality (s31_memory_layout.h:75-76). |
| `.text..fast` ".init/.bss leak" (0050 README) | Refuted. Only T/t/W symbols are present, and INIT_TEXT comes first. At most a few kB of `__initdata` remain. |
| Merging X stub libraries (XIP-4) | Clients map 1-2 stubs; saves 0-8 kB. |
| Daemon text to XIP via repartition (XIP-5) | Clean SD text is already reclaimable, so MemAvailable gains about 0 (census: udevd 4 kB RSS under load). Flash is *not* the blocker (999,819 B free), but a three-place repartition is not worth ~0 kB. |
| App/libGL text in XIP (XIP-6) | 5.3-6.3 vs 7.3-7.4 fps (Makefile:950-958); 0 at idle. |
| SD paging B6/B7 (P5) | Not capacity; belongs in the sd-paging programme. |
| XLITE ring parking (U6) | Census shows 20 kB Rss with 16 kB swapped; adds a protocol race. |
| crond / klogd / ntpd / getty trims (U7, R3) | crond persists the clock (crontabs/root `*/5 ... S30clock save`). ntpd stays for SDL timing (S30clock:63-95). klogd is the only syslog kernel copy. An lvdesk-spawned getty fails on an OOM SIGKILL. |
| hart0 offload (R8); SRAM as Linux RAM | hart0 has 4-12 KiB free, and Wi-Fi init already hit ESP_ERR_NO_MEM. A new memory bank costs 128 kB of mem_map. |
| Slab reclaim, SLUB_TINY off, vfs_cache_pressure | CLAUDE.md:362-372. SLUB_TINY off costs ~390 kB; vfs_cache_pressure wedged the board. |
| MGLRU, THP/mTHP, KSM, hugetlbfs, SMP off, disabling BT/Wi-Fi/audio | Unavailable on rv32/SPARSEMEM, measured at or below noise, or against CLAUDE.md:385. |

---

## 6. What the probes established, and what only the board can answer

**Established statically (#397 artifacts unless stated):**
- **Kernel RAM image.** 609,088 B are permanent. `.text..fast` is 250,880 B with no data or bss symbols, and 165,910 B of it took zero GLQuake samples. Glob pollution is ≥16,222 B. The percpu template is 23,820 B and never freed. The DTB is 19,976 B, in init.
- **Census.** Residue in no counter is ~800-914 kB once only the direct scanout is taken out. The MemAvailable formula at min_free 1024 reproduces 1,462 against the reported 1,464.
- **audio_reserved.** 0x50FEC000-0x50FEEFFF is unreferenced. The hart0 SBI bounds depend on it, and the last page has five users.
- **Shell history.** The ash mechanism is reproduced, and HISTSIZE is a no-op in the probe build.
- **s31-bt.** Locks before D-Bus starts, has no threads, and MCL_ONFAULT works.
- **udev.** No libudev consumer in the committed tree or in BlueZ 5.79.
- **zram.** mem_used is 7x the compressed size, and size-class waste explains only ~5 kB (this disputes current-state.md:596).
- **PMA.** The NAPOT region covers 64 MiB.

**Doc corrections found:**
- **where-the-ram-goes.md:113-116** says DIAG=1 is the default; Makefile:494 is `DIAG ?= 0`. The debugfs cost is given as 400 kB in one place and 1.14 MB in another.
- **The ledger at current-state.md:6732 and 6752-6758** ("2 × 756 kB", cursor clean background) predates direct scanout (paging-plan:330-346; perf-review:180).
- **current-state.md:6750-6751** says "3 MiB is rejected outright - pageblock alignment is 4 MiB". 3 MiB ships with PAGE_BLOCK_ORDER=8.
- **ext4 group counts conflict** between current-state.md:2531-2542 and :3040ff.
- **Flash margin.** Makefile:436 ("140 kB margin") and CLAUDE.md's PROF=1 figure (5,968,137 B) conflict with artifacts/gl/glquake/kfast/build-base2.log: "xipImage 5160565 bytes, 999819 bytes free in the linux partition".
- **The 0050 README "NOT fixed" paragraph** is wrong.
- **s31_vcpu.c:966-967** has a stale 0x50FEC000 comment.
- **CLAUDE.md screenshot guidance** relies on debugfs `scanout=`, which DIAG=0 kernels do not have.

**Only the board can answer:**
- Which metric the owner reads.
- A full idle meminfo on #401/#402, and how it drifts from 62 s to 10 min to 8 h.
- What holds the M1 residue: `jpeg_buf`, a mode or cursor plane, or skbs.
- The DMA atomic pool size.
- Whether the DTB is copied permanently.
- The s31-bt smaps split.
- Whether the 280 kB shell mapping is the history list, and what config the board's busybox uses.
- The card's udev rules and hotplug behaviour after udevd exits.
- The VT slab delta.
- Per-process VmPTE.
- The kthread census.
- The card's ext4 geometry.
- L2's union hot set and its performance gates.
- Whether s31-bt still grows over uptime.
- Whether an S31 part with more than 16 MB of PSRAM exists (a question for Espressif).