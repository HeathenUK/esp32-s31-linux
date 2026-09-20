# Cross-hart IPIs on real doorbell lines - plan (2026-09-21)

Status: PLAN, nothing implemented. Agreed with the user before any code.

## Why

Today a Linux IPI to hart 1 is a software-set pending bit on CLIC slot 47,
written through the +0x10000 cross-hart alias (patches/0051). It delivers, but:

- It does **not wake a hart asleep in `wfi`.** So with a second CPU online hart
  1 no longer sleeps: `arch_cpu_idle()` returns at once and the idle loop spins.
  Measured with the hart0 PC sampler on the SDL1 canary (4000 samples each):
  UP spends 9.1% halted in `wfi`; SMP spends 7.5% *executing* the idle path,
  4.8% of the whole run in Tree-RCU context tracking alone.
- It rests on alias behaviour nobody documents as an IPI mechanism.
  (The alias itself is sound for IE/IP/ATTR/CTL - self-tested 6 boots of 6 -
  and stays in use for irq mask/unmask from the lent CPU.)

The silicon's intended mechanism is `HP_SYSTEM_CPU_INT_FROM_CPU_n`: writing 1
asserts a **level** into the interrupt matrix, routed to a CLIC slot like any
peripheral. ESP-IDF's own cross-core interrupts use it; the hosted transport's
hart0->hart1 doorbell is one of these lines and demonstrably wakes an idle
Linux. Level semantics also mean an IPI cannot be lost between send and take.

## Facts the plan rests on (all read from source, 2026-09-21)

| register | addr | matrix source | today |
|---|---|---|---|
| FROM_CPU_0 | 0x20586010 | 65 | IDF FreeRTOS yield on hart0 - untouchable |
| FROM_CPU_1 | 0x20586014 | 66 | hart1 -> hart0: vCPU doorbell (s31_vcpu.c `ipi_isr`) |
| FROM_CPU_2 | 0x20586018 | 67 | hart0 -> hart1: hosted kick; DT `interrupts = <40 67 EDGE_RISING>` |
| FROM_CPU_3 | 0x2058601c | 68 | hart1 -> hart0: hosted kick; `esp_intr_alloc(..FROM_CPU_3.., h1_doorbell_isr)` |

Two of the four do the same job in the same direction (1 and 3: "hart1 wakes
hart0"). Merging them frees one.

## Target layout

| register | direction | carries |
|---|---|---|
| FROM_CPU_0 | - | IDF yield (unchanged) |
| FROM_CPU_1 | **-> hart 1** | **dedicated Linux IPI line** (new role) |
| FROM_CPU_2 | -> hart 1 | hosted kick (unchanged) |
| FROM_CPU_3 | -> hart 0 | hosted kick **and** Linux IPIs to the lent CPU (shared) |

Chosen over "merge hosted into FROM_CPU_1" because it leaves the hosted
transport's registers, DT node and Linux driver completely untouched.

## Changes, by component

1. **hart0 monitor** (`bootloader/main/s31_vcpu.c`)
   - Stop allocating FROM_CPU_1. Hook FROM_CPU_3 instead, SHARED with hosted's
     `h1_doorbell_isr` (`ESP_INTR_FLAG_SHARED` on both allocations; both ISRs
     are ours).
   - A shared level line needs a reason: Linux sets a flag word in internal
     SRAM (uncached for both harts) before ringing; the monitor's ISR injects an
     IPI only if the flag is set, and clears it. Hosted's ISR keeps acking the
     register as it does today. No spurious guest wake-ups from hosted traffic.
2. **OpenSBI** (`esp32s31.c`): `hart_start(0)` rings FROM_CPU_3 instead of _1.
3. **Linux CLIC driver** (`irq-esp32s31-clic.c`)
   - IPI to hart 1: `writel(1, FROM_CPU_1)`. Route source 66 -> slot 47 on hart
     1's matrix bank, LEVEL. Handler: `writel(0, FROM_CPU_1)` *then*
     `ipi_mux_process()` (clear-before-process, as now).
   - IPI to the lent CPU: set the SRAM flag, `writel(1, FROM_CPU_3)`.
   - Self-IPI on hart 1 (irq_work): same FROM_CPU_1 write - one mechanism.
   - Drop the slot-47 software-pending path.
4. **Idle** (`arch/riscv/kernel/process.c`): remove the "hart 1 must not wfi"
   special case. This is the payoff; it goes in LAST, alone, so its effect is
   measured by itself.
5. **DT**: nothing for hosted. The IPI line is set up by the CLIC driver, not a
   DT consumer (same as today's slot 47).

## Order, each step with its own check - stop at the first failure

0. **Prove the premise before building on it** (I skipped this kind of step
   once today and paid for it): on the CURRENT kernel, route source 66 -> a
   free slot on hart 1, put hart 1 in `wfi` (idle), have CPU1 write FROM_CPU_1,
   and confirm hart 1 wakes and takes it *promptly*. One-shot, read-mostly,
   removed afterwards. If a FROM_CPU line does NOT wake `wfi`, the plan is void
   and we stop here having lost one build.
1. Monitor + OpenSBI move to FROM_CPU_3 (Linux still uses the old slot-47 path
   toward hart 1). Check: 2 CPUs up, 5 boots, gate.
2. Linux sends/receives hart-1 IPIs on FROM_CPU_1; idle policy unchanged
   (still polling). Check: IPI accounting balances in both directions
   (heartbeat counters), 5 boots, gate.
3. Remove the polling-idle special case. Check: 5 boots, gate, canaries, and a
   sampler capture - hart 1 should be back in `arch_cpu_idle`.
4. Strip diagnostics (heartbeat, sampler stays as a tool), update patches/,
   worklog, memory.

Rollback at every step is one revert of that step; images of the last good
state are kept (`images/xipImage-*-smp-*`, `hello_world-vcpu-*.bin`).

## Risks and unknowns, stated up front

- **Premise unverified** until step 0: that a FROM_CPU level wakes hart 1's
  `wfi`. Strong evidence (hosted's doorbell does), not proof.
- Hosted declares its line `EDGE_RISING` on a level source. Why? If level
  handling has a problem on this CLIC (re-trigger storm until cleared), step 2
  inherits it; clear-then-process in the handler is the mitigation. To check in
  step 0.
- Three places must agree on the register roles (monitor, OpenSBI, CLIC
  driver) - same discipline as the partition-geometry rule in CLAUDE.md. One
  shared header (`shared/s31_memory_layout.h`) gets the definitions.
- hart0 SRAM flag word: address moves with the loader build unless pinned.
  Pin it in the shared layout (internal SRAM region already reserved for
  shared use), do not rely on `nm`.
- The rare boot stall with CPU1 in `do_raw_spin_lock` (seen once) and the
  vblank timeout boot (seen once) are NOT addressed by this plan and are not
  understood. Two sightings in ~25 boots: noted, not chased. If level IPIs
  make them disappear that is a bonus, not a claim.
- Loader images are layout-sensitive (a pad in s31_vcpu_asm.S dodges an
  unexplained early death). Every loader rebuild in this plan must be boot-
  checked with alive.py before anything is concluded from it.

## What this does not change

Delivery TO the lent CPU stays emulated by the hart0 monitor: FreeRTOS's M-mode
interrupts leave hart0's supervisor interrupt level blocked, so hardware S-mode
delivery is unavailable there. Faster is possible later (take the doorbell as a
raw vector instead of through IDF's dispatch); native is not.
