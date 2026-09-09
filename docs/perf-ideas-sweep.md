# Performance idea sweep, 2026-09-09

Eight independent agents, each given the full measured state and the kill list,
each reading the actual code from a different angle. 68 candidate levers.

**STATUS: UNVERIFIED.** The adversarial verification phase died on a session
limit (151 of 213 agents failed), so the "3 survivors / 65 refuted" counts the
run reported are an artefact of API failures, not judgement. Everything below
is a *claim with a citation*, not a result. Several contradict conclusions in
`current-state.md` - that is the point of the exercise, but each still has to be
measured before it is believed.

## Claims that CONTRADICT what we currently believe

These matter most, because we are currently acting on the opposite.

### 1. The palette expansion is NOT memory-bound - our own two loops disagree by 3.3x

`current-state.md` records the expansion as memory-bound at ~38-51 MB/s and
closes the CPU-side of it. But two loops in the *same frame*, on the same core,
through the same PSRAM, differ by 3.3x per byte:

| loop | traffic | time | rate |
|---|---|---|---|
| ShmPutImage copy+hash (`xshim.c` ~4574) | 128 kB | ~800 us | **~160 MB/s** |
| palette expansion (`lvdesk.c` ~4256) | 192 kB | 3.75 ms | **~51 MB/s** |

If the machine could only do 51 MB/s the copy+hash could not do 160. So the
expansion is **latency-bound, not bandwidth-bound**: ~18.75 cycles/pixel where
the arithmetic is 3-4. The inner loop is a strictly serial dependent chain -
`lbu` index, `sh1add` into the LUT, `lhu` entry, `sh` store - and on an in-order
core iteration k+1's load cannot issue until k's chain retires.

This reopens the largest cost lvdesk controls. Candidate fixes, all cheap:
- **Word-source loads**: one `lw` fetches four indices, four independent LUT
  lookups issue back-to-back. (The closed word-vs-scalar experiment changed the
  *store* width, which cannot fix a *load* stall - that is why it measured 1.5%.)
- **Unrolling**: GCC does not unroll this at -O2; nothing is ever in flight to
  cover a miss.
- **Two pre-shifted 32-bit palettes** so two pixels merge without a shift.
- **Manual D-cache preload** - `cache_ll_l1_dcache_preload()` exists with a real
  setter, unlike the icache autoload dead end.

Test with `rootfs/expbench.c`, which already exists, plus an arm with a constant
index to isolate the chain from the traffic.

### 2. Both harts share a unified L1 D-cache - hart0 offload is cache-coherent

`docs/s31_hardware/s31_cache.txt`: *"each CPU (Core0 and Core1) has a private
instruction cache, while both CPUs share a unified data cache"*. One 64 KB,
64 B-line, 2-way write-back L1 D-cache, both harts addressing PSRAM through the
same cached alias.

That removes the objection that has always killed hart0 offload here: there is
**no coherency problem**, no `dma_sync`, no flush. hart0 is the only offload
engine on this SoC that is cache-coherent with hart1.

Null experiment first, and it needs no handshake: run a busy loop on hart0
touching PSRAM at ~30 Hz and measure whether Doom's fps moves. That prices bus
contention before any co-processor exists.

### 3. There IS a performance monitor

CLAUDE.md states flatly that there is no PMU. `/opt/esp-idf/components/soc/
esp32s31/register/soc/axi_perf_mon_reg.h` exposes a bus performance monitor
nothing here has ever touched. It would settle "is this memory-bound?" directly
instead of by inference - which is the question that has cost this project the
most.

### 4. The CPU-time instrument is quantised at 10 ms and biased toward the syscall

Our profilers use `CLOCK_THREAD_CPUTIME_ID`, but this kernel has
`TICK_CPU_ACCOUNTING` (no `VIRT_CPU_ACCOUNTING`; the GEN variant needs
`NO_HZ_FULL`, which is closed). So utime/stime advance only in
`account_process_tick()`, in whole 10 ms jiffies at HZ=100, attributed to
whatever was running when the tick landed.

**Every attribution in current-state.md rests on this instrument**, including
the ~350 us/syscall figure that we wrote off as structural. Bracket 200
consecutive calls in one `CLOCK_MONOTONIC` span and divide - wall clock has
nanosecond resolution and the batch removes the bias.

## Claims about things nobody has looked at

### 5. prboom is compiled -Os and has never been profiled

It is 53.7% of the machine and the one binary nobody has examined from a codegen
angle. `BR2_TARGET_OPTIMIZATION="-Os"` applies to every Buildroot package with
no per-package override. **This is a build-flag change, not a source change** -
it respects "never modify prboom source".

-Os to -O2 on this kind of code is typically 8-15%; PGO trained on the timedemo
typically a further 5-10%. Conservatively 10% off prboom = **5.4% of the
machine**. Risk: text growth on an SD-backed binary with little slack.

Same argument applies to SDL 1.2, which sits between prboom and xlite on every
frame.

### 6. XIP_KERNEL silently disables every Zbb code path in the kernel

`arch/riscv/Kconfig`: `RISCV_ALTERNATIVE depends on !XIP_KERNEL`, and
`RISCV_ISA_ZBB depends on RISCV_ALTERNATIVE`. We set `CONFIG_XIP_KERNEL=y`, so
`RISCV_ISA_ZBB` is unconditionally **n** - the kernel's ffs/fls/strlen/memchr
zbb paths are all off, even though `-march` advertises zbb and the DT declares
it. Confirmable with a grep of the generated `.config`, zero board time.

### 7. lvdesk polls the same descriptors twice per pass

`lvdesk.c:7955` polls every fd including all client fds; `xshim_poll()` then
builds its own pollfd array and polls again with timeout 0 (`xshim.c:7304`). The
second poll cannot learn anything - no time has passed. Measured at 13.2 ms/s,
~1.3% of the machine, for about thirty lines of change and no design risk.

### 8. xlite's XShmPutImage flushes when real libXext does not

`xlite/xlite_req.c:2405` ends with an unconditional `XFlush(dpy)`. Real
Xlib/libXext does not flush there. SDL then calls XSync, which would have
appended GetInputFocus to the *same* output buffer - so our flush splits one
44-byte write into two, and the server wakes, polls and recvmsgs **twice per
frame instead of once**. ~2.8% of the machine, one line, behind an env gate.

### 9. CONFIG_FUTEX is off

`esp32s31_defconfig:53`. `futex()` returns -ENOSYS, so musl's `__timedwait`
falls through and `__wait` becomes a busy spin. Not a Doom lever, but it means
**eventfd, not futex, is the cheap wakeup primitive here** - which constrains
the design of any shared-memory transport.

### 10. The kernel linear map is all 4 KB pages

`arch/riscv/mm/init.c` `best_map_size()` gates megapages on `IS_ENABLED(CONFIG_64BIT)`.
On RV32 that is false, so all 15.4 MB is mapped with 4 KB PTEs (~3,850 entries)
even though Sv32 supports 4 MB megapages. One-line change; it either boots or it
does not. Charged to every kernel-mode instruction stream, so potentially broad.

## Structural ideas, larger but real engineering

- **Delete the DIRTYFB copy** (3.40 ms/frame, 384 kB/frame - 43% of all
  compositor bus traffic). See `docs/scanout-direct-plan.md`.
- **A private damage ioctl** instead of the full DRM atomic-commit machinery
  per frame: ~1.5 ms of the 3.40 ms is state alloc, blob creation, ww_mutex
  acquisition and commit-tail walking, not pixels. Size it first by sending a
  1x1 rect at the same rate and reading LVPROF.
- **Fuse copy+hash+expand into one pass** - the expansion re-reads exactly the
  bytes ShmPutImage just wrote, after they have fallen out of cache.
- **Scanline scatter-gather compositing**: scanout is a cyclic GDMA descriptor
  list, and a descriptor caps at 4095 bytes - so a row can already be assembled
  from multiple sources. That is a hardware overlay on hardware documented as
  having none.
- **Shared-memory ring transport** between xlite and xshim (both ends ours),
  keeping the socket as doorbell. Prove the primitive with a ping-pong bench
  before building any protocol.
- **Skip unchanged pixels, not just rows** - the index plane already holds the
  previous frame. Count equal-vs-differing words first; if under ~15%, drop it.

## Cheap diagnostics worth running before any of the above

1. Grep the generated `.config` for `RISCV_ISA_ZBB` (claim 6) - free.
2. `expbench` with a constant-index arm (claim 1) - decides the biggest item.
3. Bracket 200 syscalls in one CLOCK_MONOTONIC span (claim 4) - revalidates
   every existing attribution.
4. hart0 busy-loop null experiment (claim 2) - prices bus contention.
5. Read `CACHE_L1_CACHE_WRAP_AROUND_CTRL_REG` (0x2C000028): critical-word-first
   is bit 4 and its reset default is 0.
