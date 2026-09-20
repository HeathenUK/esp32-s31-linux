# Finishing SMP: make the second core pay (2026-09-21)

Supersedes the "next" notes scattered through worklog-2026-09-19.md. Read
smp-plan.md for the architecture and smp-ipi-plan.md for the (finished) IPI
migration.

## Where we stand (measured, not hoped)

- **Stable:** kernel #301 (= the #291 state): hardware doorbell IPIs both ways,
  hart 1 sleeps in wfi again, deferred-hrtimer rearm fix, full FPU world switch.
  smp-soak 5 boots of 5; #291 gate 24/24.
- **Not yet a win.** SDL canaries vs UP: +6..11% (#291: 16.31 vs 14.73 ms,
  32.80 vs 30.82). UP + the size diet is -2% and should ship whatever happens.
- **Why it is not a win:** the second core is nearly idle. PIE (the vector
  unit) exists only on hart 1; musl's strcmp/memcmp/memchr/memrchr are PIE code;
  the kernel pins a task to CPU0 for good the first time it traps on CPU1. So
  all of userspace still shares CPU0, and pays SMP's kernel overhead on top
  (Tree-RCU context tracking on every idle transition, real spinlocks, load
  tracking - profiled 2026-09-21 with the hart0 PC sampler).
- **Closed, do not re-open:** cross-hart atomics are sound (amostore: 0 erased
  in 5M, positive control 755k). The CLIC +0x10000 alias is sound for all four
  bytes. A FROM_CPU line wakes wfi in ~1 us. Relocating SMP objects into
  .text..fast buys <= 2% (context_tracking, ipi-mux, smp.o measured; spinlock.o
  in RAM does not boot).
- **Failed, in the attic:** the PIE lease prototype (patches/attic/0055). Its
  "lease=0" still overrode task_cpu_possible_mask for every task; kernels
  carrying it stalled at boot and froze at random.

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
