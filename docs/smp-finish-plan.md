# Finishing SMP: make the second core pay (2026-09-21)

Supersedes the "next" notes scattered through worklog-2026-09-19.md. Read
smp-plan.md for the architecture and smp-ipi-plan.md for the (finished) IPI
migration.

## Where we stand (measured 2026-09-21, kernels #314 SMP / #315 UP)

**SMP beats UP for the first time**, both kernels carrying the same fixes,
fresh boot per run, fullscreen Doom timedemo, 0 faults throughout:

| | UP+diet #315 | SMP #314 | |
|---|---|---|---|
| idle system | 32.80 (32.4-33.1, n=4) | **34.25** (34.1-34.7, n=4) | +4.4%, ranges disjoint |
| 400 KB/s Wi-Fi load | 18.25 (18.1-18.8, n=4) | **19.70** (19.4-23.3, n=3) | +7.9%, ranges disjoint |

(before: SMP #301 29.50 idle, SMP #303 16.30 under load.) smp-soak 5/5.

What changed it - two things, and neither was a tuning knob:
1. **The CLIC interrupt level was never left** (patches/0057). Every "random"
   SMP stall since #294 was this; SMP only ever looked stable because the PIE
   pin kept all of userspace on CPU0.
2. **Migrate on a PIE trap, do not pin** (patches/0056, `PIE_BOUNCE=20`).
   Steady-state PIE use is rare (wget: 0 traps in 2 s of CPU; idle lvdesk: 1
   in 7.5 s); the permanent pin, inherited through fork, had put EVERY
   process on CPU0. Now the stock scheduler places work; 924 bounces in a
   boot, nobody pinned.

- **Closed, do not re-open:** cross-hart atomics are sound (amostore). The CLIC
  +0x10000 alias is sound for all four bytes. A FROM_CPU line wakes wfi in
  ~1 us. Relocating SMP objects into .text..fast buys <= 2%. An OpenSBI MPIL
  mask for M-mode interrupts changes nothing (tried, reverted).
- **Step 2 verdict: do not ship the affinity knobs.** Workqueue mask + RPS on
  CPU1 were +5.5% only while userspace was stuck on CPU0 (#303: 17.20 vs
  16.30); with tasks free to migrate they COST ~7% (#314: 18.40 vs 19.70).
- **Failed, in the attic:** the PIE lease prototype (patches/attic/0055) - it
  overrode task_cpu_possible_mask for every task. (Its stalls, though, were
  the CLIC level bug, not the lease.)

Status of the steps below: 1 done; 2 done (rejected); 3 done (PIE use is
rare); 4 done differently (migrate, no hold timer needed); 5 open - now worth
doing, since work given its own thread really can land on CPU1; 6 - SMP
qualifies on the numbers, pending the gate on #314.

## What happens next, in order (2026-09-21)

0. **DONE 2026-09-21 - and it killed three ideas.** The #320 result was not
   real: A/B/A across flashes made the batch a 5% regression, and it is
   reverted. The canary turned out to be bimodal ACROSS boots (~17%) while
   ~1% repeatable within a session, which invalidates every single-gate-run
   comparison made before today - including "SMP is 14% off parity". Two
   causes found (the PIE pin, now defaulting to never pin; asymmetric CPU
   capacity, tried and reverted), one still OPEN: ~1 boot in 4 is ~17% slow
   with nothing pinned. **Shipping kernel #323 is ~6% FASTER than UP on the
   windowed canary on 3 boots in 4.** Next: chase the open bimodality by
   recording the scanout and SHM segment addresses per boot (shared D-cache
   aliasing is the leading suspect), then re-baseline UP and decide shipping.

1. ~~Confirm or kill the #320 canary result.~~ One gate run put the windowed
   sdl1 canary at 12.685 ms against the 14.725 UP baseline - a 24% swing from
   #314 out of 43 kB of RAM text. The mechanism does not obviously account for
   that much, so it is not believed until `scripts/board/canary-aba.sh` has
   run #314 / #320 / #314 across fresh flashes. A and A2 give the noise floor;
   if they disagree, B means nothing.
2. **If it holds, re-baseline UP with the SAME batch.** PELT, div64,
   timerqueue, fs/select and uaccess are not SMP-specific - a UP kernel gets
   the same win, and comparing SMP-with-fast-text against UP-without is
   exactly the unfair comparison this plan exists to avoid. Build UP #321 with
   S31_FAST_PROFILED, re-run canaries + Doom idle + Doom under load.
3. **Ship decision (step 6).** SMP becomes the default build only if it still
   wins with both kernels carrying the same fixes. If it does: `SMP=1
   PIE_BOUNCE=20` becomes the Makefile default, docs and memory follow.
4. **Late present (lever B, step 5).** Built and staged, not yet measured;
   A/B on a warm board via /etc/lvdesk.env, no reflash between arms. Aimed at
   the ~6% placement cost specifically, which is the part of the windowed gap
   that .text..fast cannot touch.
5. **Owed hygiene, in value order:** repeat the SMP load arm with rx recorded
   (n=3, one unexplained 23.3 fps run); exercise a flash write on SMP to prove
   the work_on_cpu path in patches/0056; retry locking/spinlock.o in RAM now
   the CLIC level bug is gone; make the loader's test sdkconfig a named
   committed config; find the loader's layout sensitivity (the `.rept 391` pad).
6. **Re-test the dw_mmc "Unexpected interrupt latency" hang.** It is a
   plausible victim of the CLIC level bug ON UP - a kthread woken from an
   interrupt ran deaf to every device until something sret'd - and may simply
   be gone. Same for the deferred Quake crash.

## Progress log (newest first; numbers live in worklog-2026-09-19.md)

- 2026-09-21 **IN PROGRESS - closing the windowed-path gap** (sdl1 canary
  +14.1% vs UP = ~6% placement + ~10% SMP kernel cost, A/B/A measured).
  - Lever A, kernel cost: profiled batch into .text..fast. #316 (with
    time/sched_clock.o) DOES NOT BOOT - bisected to that one object, a
    bootstrap self-reference like memcpy (early printk timestamps call
    sched_clock before the XIP copy runs); recorded in the .lds.S.
    **#320** = PELT (sched/build_policy.o) + div64 + timerqueue + fs/select +
    riscv uaccess, +43 kB, eight hot symbols verified in RAM, boots. Measuring:
    gate canaries + Doom idle. New tool: scripts/board/fastarm.sh (one arm end
    to end, aborts rather than flashing a stale image after a failed build).
    Then, separately: retry locking/spinlock.o in RAM (its old no-boot predates
    the CLIC level fix and was never explained).
  - Lever B, placement (plan step 5): **LATE PRESENT**, built, staged, not yet
    measured. The survey found no thread to add and no thread needed: lvdesk is
    one poll() loop, and a windowed SDL frame is ShmPutImage + XSync in ONE
    write, handled in order, with the replies flushed only at the end of
    client_data(). So the client's XSync returned after copy -> palette
    expansion -> PRESENT ioctl: the two processes alternate, and on two CPUs
    every frame pays a cross-CPU wake with nothing overlapping. The copy out of
    the client's segment is all the client is entitled to wait for (xshim never
    adopts the segment). So ShmPutImage now QUEUES the drawable, client_data()
    flushes the replies, and only then is the queue drained: the client's next
    frame and our present of this one run side by side. No thread, no hand-off,
    identical order of work on one CPU. `XSHIM_LATEPRESENT=1` in
    /etc/lvdesk.env, default off. Arm: sdl1 canary off vs on, warm board.
- 2026-09-21 gate 24/24 on SMP #314; sdl2 canary -3.9% (ahead of UP for the
  first time), sdl1 canary +14.1% (NOT parity) -> SMP stays a build option.
- 2026-09-21 fair table: SMP #314 beats UP #315 on fullscreen Doom, idle +4.4%
  and under a 400 KB/s load +7.9%, ranges disjoint. Affinity knobs rejected.
- 2026-09-21 root cause of every SMP stall fixed (CLIC level, patches/0057);
  migrate-on-PIE (patches/0056); soak 5/5.

Owed measurements: repeat the SMP load arm with rx recorded (n=3, one
unexplained 23.3 fps run); exercise a flash write on SMP (work_on_cpu path).

## Principles (the user's, binding)

1. Stock behaviour first: prefer mechanisms any Linux has (sysfs knobs, IRQ /
   workqueue / RPS / NAPI affinity) over custom kernel policy.
2. No manual per-app pinning as the end state; no client app rebuilt, relinked
   or reconfigured; no musl customisation; keep PIE wherever it is not a net
   loss. Platform code of OURS (lvdesk, xshim, daemons, init) may be arranged.
3. Every step is measured against the step before it: fresh boot per arm, warm
   board, volume at the floor, the TAIL (dips) not just the mean, >= 5 repeats
   and the spread reported. One change per arm.
4. Nothing ships that has not passed smp-soak (5 boots) and the gate.

## The work, in order

### Step 1 - the real baseline (no code)
prboom timedemo + the SDL canaries, three arms, same session:
  a) UP + diet (the kernel we would ship today)
  b) SMP #301 as it is
  c) both again WITH background load (Wi-Fi transfer + A2DP playing), because
     the dips this whole effort is about are contention, and an idle-system
     benchmark cannot show what a second core is for.
Deliverable: a table with mean, p95, worst frame, and dip count per arm.
This is the yardstick for everything below.

### Step 2 - give CPU1 kernel work with STANDARD knobs (no kernel change)
None of this touches PIE, because none of it is userspace:
  - unbound workqueues -> CPU1   (/sys/devices/virtual/workqueue/cpumask)
  - Wi-Fi receive processing -> CPU1: RPS (rx-0/rps_cpus = 2) and/or threaded
    NAPI with the napi kthread on CPU1
  - SD/MMC and block completion kthreads, ksoftirqd where the kernel allows
  - RCU callback offload to CPU1 if the config has it (rcu_nocbs), else skip
Device *interrupts* stay on hart 1 - hart 0 cannot take S-mode interrupts in
hardware. Each knob is one arm in Step 1's table; keep what pays, in an init
script under buildroot-external/ (tracked), drop what does not.

### Step 3 - measure how PIE-bound our own processes really are
Before designing any affinity policy, get the number nobody has: how often
does lvdesk / xshim / s31-a2dp / st execute a PIE instruction once it is
running? Instrument, do not guess: a per-task trap counter in our trap handler
plus a measurement-only mode that bounces a task back to CPU1 after a trap
(enabled by hand on a warm board, never at boot). Outcome decides Step 4:
  - rare (a few per second or less): a temporary hold is viable
  - constant: that process lives on CPU0, full stop, and the second core's
    value is Step 2 plus whatever we restructure in Step 5

### Step 4 - only if Step 3 says yes: a SMALL temporary hold
Not 0055. No scheduler hook, no task_cpu_possible_mask override. On a PIE trap
on CPU1: narrow the task to CPU0 exactly as today, remember the mask it had,
and restore that mask from a timer after T ms without a further trap. Off by
default; turned on by an init script only after it has survived smp-soak with
it enabled FROM BOOT (0055 only ever passed warm).

### Step 5 - arrange OUR platform so work can run in parallel
The user's suggestion, and probably the largest win: lvdesk's present path,
xshim's request servicing, the audio route and A2DP encode are ours. Where a
unit of work is PIE-free and parallel to the client's frame (expansion/present
of the previous frame, audio mixing, SBC encode), give it its own thread so
the scheduler CAN place it on CPU1. Clients untouched. Each move is judged on
wall-clock and tail, counting the copy and wake-up it adds (a kworker hand-off
costs ~8 ms here; a futex wake across CPUs measured ~1.4 ms round trip).

### Step 6 - decide what ships
SMP ships as the default only if Step 1's table says it beats UP + diet on the
real workloads, tail included. Otherwise UP + diet ships and SMP stays a build
option with everything above recorded.

## Hygiene owed regardless
- Loader sdkconfig for SMP is a test configuration (prototype + Linux mode on,
  HW stack guard off, int WDT on): make it a named, committed config.
- Loader images are layout-sensitive (a pad in s31_vcpu_asm.S dodges an
  unexplained early death). Find the cause or at least detect it at build time.
- The test guests, bisect pad and heartbeat are diagnostics: keep them behind
  their switches, out of the shipping image.
- flash and SRST firmware calls made on CPU1 are refused by the hart0 monitor:
  route them to CPU0 in the kernel wrappers.
- A UP kernel has never been booted on the two-hart OpenSBI + two-cpu DT.
  That is the shipping combination if UP ships; test it in Step 1a.
