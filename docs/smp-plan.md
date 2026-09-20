# A second hart for Linux: scope and plan (2026-09-20)

Status (current): SMP/doorbell IPIs/WFI are implemented. Kernel #291 is the
validated checkpoint (three #290 reset/stress cycles plus #291 full gate).
The vblank timer bug is fixed; SDL canaries still6–11% slower than UP.

Active step: #294 temporary PIE affinity experiment (patch0055), not accepted
until regression and workload tests pass. Only hart1/Linux CPU0 has PIE SIMD.
The generic scheduler compatibility-affinity API saves/restores user masks;
100ms lease is an initial tunable policy, not a proven optimum.

Next steps, in order:
1. Validate stock-memcpy restoration, fork, explicit user affinity, and live
   vector state across migration with pie-affinity-test. Preserve #291 rollback.
2. Measure usefulCPU1 execution and frame/audio results in stock SDL apps,
   especially Tyrian synthesis versus game/render threads. Frequent libc
   calls may defeat a coarse lease; mask restoration alone is not acceptance.
3. Compare automatic placement with measured per-thread affinity experiments
   for synthesis/game/lvdesk. s31route executes on the app's audio thread.
4. Measure hart0 monitor IPI/wake overhead and effective CPU capacity; expose
   useful constraints to Linux rather than permanently pinning whole apps.

Open unrelated reports: user-observed Quake crash (deferred at their request)
and an uncaptured 'unexpected interrupt latency' boot hang, likely the known
SD/MMC warning string. Preserve recurrence; do not call these fixed.

The original plan below predates implementation. Its motivating measurement
was: windowed prboom with sound is 60% of the core, lvdesk 35%, all else
~3% (cpushare, 2026-09-20). `scripts/board/cpushare.sh` set the bar itself:
"above ~30% non-game CPU a second hart is the next big project".

## 1. What is true today

Hardware (vendor `soc_caps.h`, `/opt/esp-idf` in the build container): two
HP cores (PIE SIMD is available only on hart1), `rv32imafc` + Zba/Zbb/Zbc/Zbs + Zaamo/Zalrsc (atomics are
there, the kernel already reports them), a CLIC per core, one interrupt matrix,
one shared external-memory cache with write-back.

Division of labour:

| | hart0 | hart1 |
|---|---|---|
| Runs | ESP-IDF v6.1, FreeRTOS `UNICORE=y`, M-mode | OpenSBI (M) + Linux 7.1.10 XIP (S/U) |
| Owns | Wi-Fi + BT controller blobs (ESP-Hosted over shared SRAM), USB host + HID (since 2026-09-19), boot splash, reset/power-off, coex | everything else: LCD/DRM, PPA, JPEG, I2S+codec, SD, I2C/touch |
| Tasks | `hosted_rx`, `s31_audio`, `hid_attach`, `usb_lib`, IDF system tasks | - |
| Load | unmeasured; believed small outside radio bursts | 100% busy under any game |

How hart1 starts (`bootloader/main/core1_trampoline.S`): hart0's loader writes
the core1 boot address and releases it; the trampoline clears hart1's M-mode
CLIC slots, programs hart1's PMA entry 7 (PSRAM, write-back, cacheable) and a
full-RWX PMP entry, and jumps into OpenSBI at the top of PSRAM with `a0 = 1`.
OpenSBI (`opensbi-esp32-s31/platform/generic/espressif/esp32s31.c`) exposes ONE
hart, a timer device on the core-local `mtimecmp` window at 0x1000_4000, and
says in as many words "This platform currently exposes one hart and has no IPI
device". The kernel is `CONFIG_SMP` off, `PREEMPT_NONE`, `HZ=100`.

What is already SMP-shaped, because it was written that way:
- `drivers/irqchip/irq-esp32s31-clic.c`: per-CPU `clic_per_cpu`/`clic_lock`,
  `num_harts`, an `irq_set_affinity` that knows the interrupt matrix steers a
  source to a core, a vector-table install "on every hart", and the documented
  `+0x10000` window onto the OTHER core's CLIC "for cross-core IPI". Today it
  hard-routes every source to physical hart1.
- The CLIC and the `mtimecmp` window are address-virtualised per core, so the
  same driver code runs on either hart unchanged.

## 2. The design question: who owns hart0?

Linux cannot simply take hart0. The radio blobs, the BT controller, the USB
host stack and the power/reset path run there under FreeRTOS, and they are the
part of this port that took longest to make reliable.

**Option A - Linux owns both harts, radios move into Linux.** GrieferPig's
design (section 4). Everything hart0 does today is re-hosted on Linux: the
blobs' OS adaptation layer as kernel code, USB back on dwc2 or re-hosted, the
reset path in OpenSBI. Cleanest end state, and it is a rewrite of the half of
the system that currently works.

**Option B - FreeRTOS keeps hart0 and LENDS it to Linux.** hart0 stays exactly
as it is. A lowest-priority FreeRTOS task on hart0 drops to S-mode and runs as
Linux's second CPU. Every FreeRTOS interrupt is M-mode and therefore preempts
Linux on that hart transparently; when a radio or USB task becomes ready,
FreeRTOS switches away from the "Linux vCPU" task like any other, and Linux
sees a CPU that occasionally loses a few milliseconds (steal time). FreeRTOS
is, in effect, a very small hypervisor with one guest CPU scheduled at idle
priority. Nothing that works today is rewritten.

**Recommendation: B.** It keeps the radio/USB/audio-transport work intact, it
can be built and abandoned in stages without breaking the single-hart system,
and its cost to Linux - a secondary CPU that is sometimes late - is one Linux
already tolerates on every virtualised machine. A remains the fallback if B's
trap forwarding turns out to be unworkable.

## 3. Option B, piece by piece

1. **Entering Linux from a FreeRTOS task (hart0).** The task sets up the same
   hart-local state the trampoline gives hart1 (PMA entry for PSRAM, a PMP
   entry that lets S-mode reach PSRAM and the peripherals Linux owns, S-mode
   CLIC window), then enters OpenSBI's warm-boot path for a secondary hart and
   never returns. FreeRTOS context-switches it like any task: the RISC-V port
   saves the GPRs, `mepc` and `mstatus` (hence MPP = S) per task. S-mode CSRs
   (`sepc`, `scause`, `stvec`, `satp`, `sscratch`) are used by no other task, so
   they survive untouched. The FPU needs care: IDF saves F state lazily per
   task; Linux on hart0 must not be able to corrupt, or be corrupted by, a
   FreeRTOS task that uses float.
2. **Traps from S-mode on hart0.** IDF owns `mtvec`/`mtvt` on hart0. An `ecall`
   from Linux, or any exception taken with `mstatus.MPP = S`, arrives in IDF's
   exception vector and must be forwarded to OpenSBI's trap handler with
   hart0's scratch area instead of going to IDF's panic handler. This is the
   one genuinely invasive change on the IDF side, and the first thing to
   prototype, because if it cannot be done cleanly B is dead.
3. **OpenSBI: two harts.** HSM `hart_start` for hart0 becomes "ring FreeRTOS":
   a flag in shared SRAM plus an interrupt, which the vCPU task waits on. Add
   an IPI device built on the CLIC software interrupt through the `+0x10000`
   cross-core window; on hart0 that M-mode interrupt lands in IDF's vector
   table and is forwarded like the traps. Timer: hart0's own `mtimecmp` window
   (FreeRTOS ticks from SYSTIMER, so it should be free - to verify), delivered
   as it is on hart1.
4. **Kernel.** `CONFIG_SMP=y`, `NR_CPUS=2`, boot CPU = physical hart1, secondary
   = physical hart0; DT gains the second `cpu` node; the CLIC driver's
   secondary-CPU init and IPI plumbing get finished; interrupts stay routed
   to hart1 at first. XIP and SMP have not been tried together on this tree.
   Cost: per-CPU areas and a larger kernel image (290 KB free in the
   partition today), some tens to low hundreds of kB of RAM.
5. **Memory safety between the worlds.** Linux on hart0 must not be able to
   reach IDF's internal SRAM; IDF must keep ignoring PSRAM except the shared
   windows. PMP on hart0 has to express both, per privilege level.
6. **Lock-holder preemption.** If FreeRTOS takes hart0 away while Linux holds a
   spinlock there, hart1 spins for the length of the radio burst. Believed to
   be short; to be measured, not assumed. `PREEMPT_NONE` stays.

## 4. What GrieferPig's tree actually does (studied 2026-09-20)

Read from `grieferpig/main` and its history. The kernel, OpenSBI, U-Boot and
published docs are submodules NOT present in our clone; the facts below come
from the superproject Makefile and from docs that were in-tree until commit
`24d9e8d` (after SMP landed in `97ab167`). Read with
`git show 24d9e8d:docs/s31_hardware/<file>`.

- **It is Option A, completely.** Commit `97ab167` deleted ESP-Hosted and the
  FreeRTOS resident. The Wi-Fi/BT/coex/PHY blobs are linked into a relocatable
  payload that runs INSIDE Linux S-mode, on kthreads, behind one global "blob
  gate", on a ~1900-line FreeRTOS-ABI shim (`firmware/radio/s31_rtos/`,
  `radio_stack.c`). Radio tasks had to be `SCHED_RR/80` with the deferred-ISR
  worker at `SCHED_FIFO/90`: under CFS the closed BLE controller asserted.
  Boot is U-Boot SPL -> FIT(U-Boot + OpenSBI) -> Linux 6.18 XIP. Both harts
  belong to Linux; hart0 is the boot hart, hart1 the HSM secondary.
- **IPIs are native S-mode, NOT SBI.** Doorbells are the CPU_INTR_FROM_CPU
  registers through the interrupt matrix (CPU0->CPU1 `0x20586014`, source 66;
  CPU1->CPU0 `0x20586010`, source 65), multiplexed with `ipi_mux`. The driver
  calls `riscv_ipi_set_virq_range()` BEFORE `sbi_ipi_init()` so
  `riscv_sbi_for_rfence` stays false - because with SBI remote fences, TLB
  shootdowns entered OpenSBI's M-mode IPI path and DEADLOCKED with both harts
  in IRQ-disabled sections.
- **Timers are per-hart SYSTIMER targets in S-mode**; S-mode access to the
  MTIME/MTIMECMP window faults. (We differ: our OpenSBI owns `mtimecmp` in
  M-mode and delivers the tick - that path already works on hart1.)
- **An unresolved silicon-level hazard**: an S-mode CLIC slot can sit at
  IP=1, IE=1, SIE=1 and the hart never enters its handler. Rejected fixes are
  listed in `s31_smode_clic_irq_findings.txt`; what shipped is IRQ-enabled
  idle polling of the doorbell, SYSTIMER and every enabled external ID, with a
  documented hole when both harts stay non-idle. Also: an M-mode CLIC
  interrupt crossing from S-mode leaves a 0xff sentinel in the supervisor
  cause - dispatch must mask `xcause` with 0xfff. Under Option B M-mode
  interrupts cross S-mode on hart0 CONSTANTLY, so this matters more for us.
- **Cache**: private I-cache per hart, ONE shared D-cache with write-back; the
  Sv32 page-table walker does not snoop it, so their `head.S` writes page
  tables back before loading SATP. hart1 needs its own I-bus enable and PMA
  entry 7 (we already do both). Writable flash must be serialised against
  both XIP harts (their `mtd.xip-smp` test).
- **Device IRQs stay on one hart** (`irqaffinity=0`); `PREEMPT_VOLUNTARY`,
  `HZ_100`, `NR_CPUS=2`, hotplug on, guarded WFI idle.
- **Results**: radio-off SMP validated with dual pinned CoreMark 1048 + 1064
  iterations/s (single: 1059) - i.e. NO measurable memory-bus penalty for two
  compute-bound harts. With radios: 90 s of CoreMark on both harts + ping +
  HTTP + BLE discovery clean after the scheduling-class fix.

What we take from it: the native-IPI design and its ordering rule, the
xcause mask, the page-table write-back rule, the lockout finding and its
polling recovery, and the dual-CoreMark number (it retires the PSRAM-contention
worry for compute; copy-bound work is still unmeasured). What we do not take:
the radio re-hosting - it is the expensive half of their project and the
reason to prefer Option B.

Still to fetch before stage 2: `GrieferPig/linux-esp32-s31@affdd96b` (CLIC
irqchip with native IPIs, SYSTIMER clockevent, DTS) and
`GrieferPig/opensbi-esp32-s31@9723ce5b` (HSM, M-mode CLIC trap path).

## 5. What it is worth, honestly

- Moving lvdesk (35%) and audio threads off the game's core helps every
  client, not one title. That is the point.
- It is NOT a 2x. A windowed SDL frame is serial today: the game blocks in
  XSync while lvdesk copies and expands the frame (5.7 ms of lvdesk's ~10 ms).
  To overlap the two, the shim must reply as soon as it has CONSUMED the
  segment (the 64 KB copy, ~1.5 ms) and expand on the other CPU afterwards.
  That restructuring is small and is part of this project.
- Both harts share one PSRAM bus and one D-cache. GrieferPig's dual pinned
  CoreMark (1048 + 1064 vs 1059 single) says compute does not contend; two
  COPY-bound workloads (our 13.6 MB/s ceiling) are unmeasured and remain the
  largest performance unknown.
- OpenTyrian's synth needs ~1.8 cores by itself; a second hart does not fix
  it alone.

## 6. Stages, each with its own exit test

0. **Measure hart0's idle share** - DONE 2026-09-20. Loader built with
   FreeRTOS run-time stats; every `s31_freertos_mem` request makes hart0 log
   its idle-task share since the previous one. Desktop idle, radios
   associated, USB HID attached: **84% idle over 20 s**. During a Wi-Fi HTTP
   download: **82% over 16 s**. So ~0.8 of a core is there to lend. (The 16%
   floor at rest is itself worth a look: something on hart0 is busy.)
   Still to measure: during A2DP streaming and under USB input.
1. **Trap forwarding prototype** - DONE 2026-09-20, exit test PASSED: `make
   gate` 24/24 with an S-mode guest running on hart0 (Wi-Fi associated, USB
   HID attached, audio, desktop, SDL canaries all normal). The guest ran
   ~40 M loops/s, was preempted ~160 times/s by FreeRTOS's M-mode
   interrupts, and made ~40 ecalls/s through our trap path.
   Code: `bootloader/main/s31_vcpu_asm.S`, `s31_vcpu.c`,
   `CONFIG_S31_VCPU_PROTOTYPE` (off in the default build; the tested image is
   `images/hello_world-vcpu-stage1.bin`). What it taught us:
   - mscratch is free in ESP-IDF; our mtvec + a copy of its mtvt with the
     ordinary slots pointed at our entry is all the interposition needed.
     Vectors installed with no guest change nothing (Wi-Fi + Linux normal).
   - IDF's interrupt exit restores mstatus from the HANDLER'S ENTRY SNAPSHOT
     into whichever task it resumes (`a0 = s2`, vectors.S). Fabricating the
     guest's resume context with MPIE=0 therefore left an innocent task
     running with interrupts off, spinning in vPortYield. Fabricate an
     ordinary MPP=M/MPIE=1 context; the resume path disables MIE itself.
   - S-mode runs on hart0 under IDF's PMP as-is for IRAM/DRAM addresses.
     mintthresh arrives as 0x0f (IDF's open value), not a sentinel.
   - Instruments that found it: the bus-monitor PC/SP record registers
     (0x2d002048/4c, hart0's live PC readable from Linux with devmem),
     breadcrumbs in SRAM, and FreeRTOS variables via the ELF's symbols.
   - HART0'S HEAP IS THE REAL CONSTRAINT: 180 KB total, ~10 KB free, largest
     block 4 KB before this work. A guest that never idles starves IDLE, and
     with the prototype's stacks Wi-Fi init failed with ESP_ERR_NO_MEM.
     Moved unused light-sleep and Wi-Fi sleep/extra code out of IRAM (+8 KB).
     Still only ~4 KB free with everything running: needs a real rebalance
     (audio DMA pool 2 x 32 KB, BT controller mode, Wi-Fi buffers).
   - OPEN: Linux's console (possibly Linux) stops ~100 ms after the guest is
     told to LEAVE, while hart0 is demonstrably idle and ticking. Prime
     suspect: hart0's log line colliding with Linux on the shared UART. The
     silent-leave retest was not completed. Leaving is a test path only.
   - The HW stack guard must be off while a guest runs (guest sp is outside
     the task's stack bounds); stage 2 should stop/start it around the guest.
1b. (was 1): a FreeRTOS task on hart0 drops to S-mode,
   runs a 20-line S-mode stub that makes an SBI call and loops, while Wi-Fi
   and USB keep working. Exit test: `make gate` passes with the stub running.
2. **OpenSBI two-hart**: HSM start, IPI device, per-hart timer. Exit test: the
   S-mode stub on hart0 receives an IPI sent from Linux on hart1 and a timer
   interrupt of its own.
3. **Kernel SMP bring-up** on a diagnostic build: secondary boots, `nproc`
   says 2, `make gate` passes, radios/USB unaffected.
4. **Soak + measure**: lock-holder stalls, PSRAM contention (dblbench/membench
   on both CPUs), Doom/Quake/SDL canaries, A2DP while gaming.
5. **Make it pay**: early XSync reply in xshim, lvdesk and audio threads'
   affinity, interrupt affinity.

Rollback at every stage is "do not start the vCPU task": the single-hart system
is untouched until stage 3's kernel is flashed, and `nosmp`/`maxcpus=1` on the
command line restores it after that.
