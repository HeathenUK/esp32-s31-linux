# Getting hot kernel code out of flash

## Why

The timer interrupt costs ~860 us. Measured three ways that agree: an ftrace
timeline put it at 864 us, the hrtimer breakdown attributed 480 us of that to
the scheduler-tick callback alone, and the kernel's own watchdog printed
`hrtimer: interrupt took 856063 ns` without being asked. At 250 Hz that is ~22%
of the CPU spent ticking, and every wakeup, I/O completion and repaint queues
behind it.

The mechanism is instruction fetch. The kernel runs XIP from flash through a
**16 KB** instruction cache. The tick path spans tens of KB of scheduler, RCU
and timer code, so it evicts itself and refetches every tick. Flash is 80 MHz
QIO, about 40 MB/s; 30 KB at that rate is ~0.75 ms, which is the right order for
the 480 us measured.

This also explains the asymmetry that confused things for a long time:
kernel-heavy work is hit hard (`sched_yield` 4.3x slower under load) while
pure-userspace work is barely touched (`cpubench` -12%), because userspace runs
from PSRAM and only the kernel runs from flash.

> Since that was written, some userspace *does* run from flash (the XIP cramfs
> images), and it was measured on 2026-08-27: identical code is only **1.5%**
> slower from XIP flash than from RAM, inside the noise, against the kernel's
> 5.98x. The asymmetry is the icache, not the medium - the tick path evicts
> itself every tick, a userspace loop does not. See `docs/current-state.md`.
 An earlier "icache thrash is
disproved" note was wrong: that test used a userspace workload and never
exercised the flash path.

## What is not available

- **A bigger icache.** CACHE_L1_ICACHE_SIZE is a promptless Kconfig with a fixed
  default and there are no cache_ll size setters. Fixed in silicon.
- **Faster flash.** Already 80 MHz QIO - the bootloader enables quad mode at
  runtime regardless of the DIO header the ROM uses. IDF maps FLASHFREQ_120M to
  '80m' for this part, so 80 MHz is the ceiling.
- **Running the whole kernel from RAM.** Kernel text is 4365 KB against 12.4 MB
  of usable RAM on a board already swapping.
- **A lower tick rate.** Tried: HZ 250 -> 100 left wake latency unchanged
  (median 1432 vs 1487 us) and made SD worse (12.07 -> 16.93 ms per request).
  Fewer ticks, but each still costs 860 us. Reverted.

  > **Does not reproduce, 2026-08-27.** The build has been forcing HZ=100 ever
  > since regardless of that "reverted", and re-measured on the 7.1 kernel
  > HZ=100 is slightly *better*: SD 4k p50 3.76 ms against 4.00 at HZ=250, and
  > DIRTYFB 1.62 ms against 2.08. HZ=100 kept. `make linux KHZ=250` switches
  > it now instead of an edit.

## Plan

### Phase 0 - quantify the payoff before doing any linker work

Execute the same synthetic loop from flash and from PSRAM and compare. PSRAM is
90 MB/s against flash's 40 MB/s, so the ceiling for this whole exercise is about
2.25x on fetch-bound code; SRAM would be far better but is scarce. If a
PSRAM-resident loop does not run measurably faster than a flash-resident one,
the mechanism is wrong and nothing below is worth building.

Also establish the SRAM budget: Linux reservations currently run
0x2F062000-0x2F079C00 and hart0 owns the rest, so how much is genuinely free
decides whether SRAM is an option at all or whether this is a PSRAM exercise.

### Phase 1 - relocate the hot paths

Add a `.text.fast` output section to `arch/riscv/kernel/vmlinux-xip.lds.S` with
a RAM VMA and a flash LMA, copied during early boot exactly as `.data` already
is for XIP. Mark functions with a `__fasttext` attribute macro.

Start with the smallest set that covers the measured cost, in this order:

1. trap entry and exit (`arch/riscv/kernel/entry.S`)
2. IRQ dispatch and the CLIC driver's handler
3. `tick_sched_timer`, `hrtimer_interrupt` and the timer wheel
4. the scheduler core reached from the tick - `__schedule`, `pick_next_task`

Tens of KB, not megabytes. Grow the set only while the trace says it pays.

### Phase 2 - measure each step

Re-run, in order: the ftrace timer-IRQ duration (the headline number), `waklat`
for wake latency, `req_timing` for the SD breakdown, and the input-latency
harness for the user-visible figure. Keep whichever additions move the headline
and drop the rest - the budget is small enough that only the hottest code earns
its place.

### Phase 3 - SRAM for the hottest subset, if the budget allows

PSRAM caps the win at ~2.25x. Internal SRAM is far faster, so once Phase 1 shows
which functions actually matter, move that subset to SRAM if there is room.

## Results so far

Phase 0 measured the payoff with 72 KB of identical code on each side, larger
than the icache so neither can cache it:

    flash 17.77 ms    RAM 2.97 ms    = 5.98x

That is far better than the ~2.25x the bandwidth figures suggested, because on a
cache miss it is flash *latency* that dominates, not throughput.

Phase 1 relocated hrtimer_interrupt, __hrtimer_run_queues, tick_sched_handle and
tick_nohz_handler - 992 bytes:

    riscv-timer irq   0.864 -> 0.725 ms
    tick callback     0.480 -> 0.383 ms   (now at 0xc08c5552, in RAM)

and the kernel's "hrtimer: interrupt took" watchdog stopped firing.

Adding the obvious callees next - update_process_times, __run_timers,
run_local_timers, sched_tick, rcu_sched_clock_irq, ktime_get - grew the section
by only 576 bytes and did not improve anything (0.768 ms, inside the noise).
Annotating a function relocates that function's own code only; where the work is
inlined into a caller still in flash, nothing moves.

## Two constraints that shape what is left

- **Whole-object relocation is blocked by section ordering.** Collecting
  `*hrtimer.o(.text)` into `.text.fast` produces an empty section: the main
  `.text` output section appears earlier and has already claimed `*(.text)` from
  every object. Excluding objects from it means rewriting the kernel-wide
  TEXT_TEXT macro.
- **Function tracing cannot be used to find the rest.** `arch/riscv/Kconfig`
  has `select HAVE_DYNAMIC_FTRACE if !XIP_KERNEL`: dynamic ftrace patches call
  sites, and this kernel's text is read-only flash. The precise attribution tool
  is simply unavailable here.

So the remaining ~380 us needs hand instrumentation - ktime stamps around
candidate callees inside the tick, the same approach that localised the SD
request cost - or a custom text section for this architecture that can claim
whole objects before TEXT_TEXT sees them.

## Unrelated but free, found in the same trace

IRQ 20 runs two handlers and the first, `20300000.usb`, returned `unhandled` on
all 1365 invocations at 0.093 ms each - 127 ms of pure waste in a 3 s trace.
Whatever registered that second handler should not have.

## What else was worth adding (2026-08-27)

Profiled with `/proc/profile` on a `PROF=1` kernel and measured three arms.
**The input path went in; nothing else did.**

    A  baseline                              idle 946.2   under load 459.0
    B  input.o + evdev.o in .text..fast      idle 960.7   under load 476.4
    C  control: cold fs/ext4/xattr.o in RAM  idle 929.1   under load 426.3

CoreMark under continuous pointer motion, 5 runs per arm, fresh boot per arm.
B is **+3.8% under load** for **18 KB** of RAM, and the ranges do not overlap.
C exists because moving 18 KB out of flash relocates everything after it: a
cold object of the same size made things **7.1% worse**, so this is the input
path and not code-layout luck.

Three things this run established that are worth keeping:

- **The profile is otherwise flat.** After the tick path (already moved, and
  structurally invisible to the profiler because samples past `_etext` are
  discarded), the top non-idle symbol is `input_event` at 12.8% and everything
  after it is under 5%. There is no third obvious candidate: the remaining
  cost is spread thin, so further additions buy little and cost RAM each.
- **Fix the harness first.** A shell loop respawning `uinject drag` profiled
  fork, exec and path lookup - `link_path_walk`, `path_openat`, `dup_mmap`,
  `do_exit` all near the top. Idle went 50.8% -> 74.9% once the load came from
  a single process, so about half the apparent work was the measurement.
- **`memcpy` cannot go in `.text..fast`.** The relocation that populates that
  section *is* a memcpy, so it would be called at an address that has not been
  written yet - the kernel dies before printing anything. Same self-reference
  as `arch_sync_dma_for_device`.

**Profiling no longer costs the radios.** `make linux PROF=1` fits with
Bluetooth, Wi-Fi and sound all enabled - 6,103,417 bytes with 319 KB spare -
because the linux partition was grown to 6,422,528. The note elsewhere that
profiling and the radios are mutually exclusive is stale.
