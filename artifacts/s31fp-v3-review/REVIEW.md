# Review in progress, 2026-09-27

Purpose: accelerate unavoidable slow stock code, or replace unsafe execution
paths, through the permitted preload/in-memory interception route. Account for
launch, dispatch, cache and code-size costs; do not modify musl/ld.so/libm.

Existing v3 (7da299bf) is symbol interposition for strings and clocks, alongside
v2's copy patcher. It does not extend body-copy interception to new functions.
Do not conflate those coverage boundaries or promise internal libc coverage.

Initial source findings, not yet fixed or board-tested:

- s31str thr_start allocates tstate without clearing it. rs_register initializes
  cpu_id fields only, leaving rseq_cs and flags as allocator residue. However,
  this kernel explicitly clears both at registration (kernel/rseq.c:442), so
  this is NOT a demonstrated fault on #401. Explicit initialization would
  remove an unnecessary kernel-version dependency.
- Hot string routing increments shared non-atomic debug counters even with debug
  disabled: overhead plus data races. Counters must be opt-in and race-safe.
- rseq unregister return is ignored before freeing registered memory. Failure
  would leave a kernel-visible pointer to freed storage; teardown needs review.
- s31clk try_lock publishes lock owner tid before owner pid; a second thread
  can observe stale pid and steal a live lock as though it came from fork.
- Clock fallback after an ahead-of-kernel estimate may go backwards, including
  signal/reentrancy fallbacks, not just bad timebase. This is not transparent
  CLOCK_MONOTONIC behavior. Do not enable on the strength of QEMU happy paths.
- Scalar memmem/strstr replace linear-time musl algorithms with repeated naive
  comparison, potentially quadratic. Safety dispatch must not create a severe
  algorithmic slowdown on repetitive inputs.

Copy validation remains first. Review will drive a bounded selection of useful
safe candidates, not automatic shipping of all this WIP.

## Work completed during the quiet copy application window

- Added `make s31fp-v3`, dependent on the existing v2 build/relocation gate;
  v3 now consumes that freshly built lgref.o instead of a stale source/out file.
- String counters are debug-only relaxed atomics; no shared writes normally.
- Explicitly initialize rseq fields; keep storage allocated if unregister fails.
- Force-scalar mode no longer allocates per-thread rseq state.
- Reuse musl 1.2.5's two-way memmem algorithm from the existing toolchain,
  with local scalar calls and original license. No system libc changes.
- Fix detached test completion synchronization and cleanup on exit/cancellation.
- Add repetitive long-needle search checks. QEMU: 150001 cases and 1950013
  calls in each of dispatch/scalar modes, four threads plus detached cleanup,
  zero mismatches. QEMU lacks rseq; real CPU routing still needs board proof.
- Clock path remains opt-in/off and is not accepted for deployment.

## Placement requirement from owner

Current candidate is SD-backed; shipped v2 is XIP-backed. Compare identical
shipped bytes at both placements, separately for trampoline, coloured copy,
and uncoloured copy. Existing b10 harness now has a placement arm using the
same oplbench workload, three alternating rounds and audio hashes. Candidate
SD is an additional arm, not a substitute for the identical-byte comparison.
Do not infer XIP/SD performance from differing library builds. New string
interposition remains library-resident, so its eventual placement matters too.

## First real-board results and captured harness fault

board-result.txt: libc CPU0 and dispatch CPU0/CPU1 each passed 5000 cases
(65000 calls), zero mismatches including guard pages/repetitive inputs.
Routing counters: CPU0 24998 libc / 0 scalar; CPU1 0 libc / 24998 scalar;
rseq registration succeeded on each. Forced migration then SIGSEGV.

diag-result.txt: threaded dispatch without affinity hammer passed 22501
cases / 292513 calls; all nine rseq registrations succeeded. BOTH scalar
and dispatch migration arms faulted at libc offset 0x4eb06, the entry
`lw a0,16(a0)` of pthread_setaffinity_np (libc.dis). The affinity hammer
continued accessing handles while main joined and freed their storage.
This is a demonstrated test harness use-after-free, not a string mismatch.

Fix: wait for workers to finish their work, stop/join the affinity hammer,
then join/free worker handles. Cancellation targets also remain alive until
cancelled rather than allowing a very short test to free a detached handle
before pthread_cancel. Candidate library ac8e317f remains unchanged for the
board rerun, isolating this harness fix.

The first repaired-handle rerun timed out in detached cancellation: parent
used `i % 3` while worker used `(1000+i) % 3`, so it cancelled different
threads. The old guessed-sleep test had hidden this lack of coverage. The
parent now uses the worker's actual id; cancellation workers stay alive
until cancellation. Fixed host hammer: both dispatch/scalar complete with
zero mismatches (fixed-hammer-qemu.txt). A host test was initially launched
before its asynchronous rebuild completed and faulted; that run is invalid,
not library evidence. Re-running only after confirmed build completion passed.

## Breadth beyond OPL (review after current candidates)

Existing `artifacts/gl/glquake/prof9/PROFILE.txt` attributes 53.2% of samples
to libGL, with the world filler alone 20.1%; QuakeSpasm is 8.3% and libc 4.2%.
Executable mul/add double helpers are about 0.5%; libc's add/mul/sub helpers
about 1.8%, memset about 0.7%, sincos 0.2%. These are an existing profile,
not measurements of the new candidate. The renderer's own tier optimizations
and SD placement are already implemented; don't recreate them in a preload.

Next useful survey: stock memory/clear/search routines, integer/compiler
runtime helpers, and math entry points used across Quake/GL/other apps;
require actual call coverage and cost before implementation. Internal libc
calls are outside current interposition coverage and the owner's prohibition
on modifying musl/ld.so/libm remains in force. No approximate double-to-float
substitution or app-specific renderer rewrite is implied by this survey.

## Corrected real-board suite passed

fixed2-result.txt: library ac8e317f, test 077b9627, #401. Each single-thread
arm again passed 5000 cases; forced migration passed 16503 cases / 214539
calls, zero mismatches, nine successful rseq registrations. The same v3work
started CPU1 and ended CPU0 with dispatch off, stayed CPU1 with it on.

Exploratory microbenchmarks demonstrate why dispatch cost must be measured:
4 KB memcpy strongly favors libc PIE on CPU0 (about 3 us versus 9 us scalar),
while equal 4 KB memcmp favored scalar (17.2 us versus 27.9 us). These are
single passes and their old timer included a result-label write. A focused
comparison test now stops timing before printing and covers early/late/absent
mismatches before any policy change. Small-call dispatch overhead is real.

## Owner targets and sustained audio (2026-09-27)

Owner observes title music eventually skipping/crackling despite starting
well. Track native underrun logs only after fixed playback windows, alongside
existing synth hashes/costs; no sampling during playback. Compare sustained
missed-deadline counts, not just average us/sample. Stretch application targets:
prboom 60 fps, sdlquake 30 fps, QuakeSpasm 20 fps, through interception and
placement without modifying application code. These are aspirations, not
forecasts; use existing profiles to find coverage and bound plausible gains.

## Quiet QuakeSpasm comparison

Native timedemo result, real #401 board, fresh reset per arm, no polling,
profiling, screenshots or transfers during measurement. Same ac8e317f v3
candidate on SD, copy=1, clock=0, strings=0 versus 1. Stock demo1 at 320x240,
11025 Hz, zone 384, heapsize 12288. Passive runsh regex receives the game's
existing stdout FPS line; the harness does not sample a log file.

- Strings off: 969 frames, 108.9 seconds, 8.9 fps.
- Strings on: 969 frames, 103.4 seconds, 9.4 fps.

One pair is compatibility evidence and a promising direction, not proof of a
5.6% speedup above boot spread. Both completed without a recorded fault.
The off-arm game was left past measurement completion and reached its 240 s
watchdog before follow-up; exit 137 is intentional and not a timedemo crash.
The late stop attempt found no process, then its extra notification wait
expired. This follow-up error does not invalidate the already captured native
FPS line, but should not be described as prompt cleanup. The on arm was
stopped after FPS, and its harness exit notification was received. Post-run
maps confirmed /root/afp3/libs31fp.so. See gl-{off,on}-{console,result,stop}.txt.

Further handwritten arithmetic/hardware assessment: TYRIAN-ASM.md. Clock test
harness now stops/joins its affinity thread before freeing worker pthread
handles, uses atomic stop/violation fields, and checks thread creation. The
clock implementation remains disabled pending its separate correctness work.

## Focused comparison policy

cmp-result.txt: ac8e317f library, revised 46472bef test, three rounds with
reversed pair order in round two, CPU0; timer stops before printing. All
72 invocations exited zero. Sizes 8/64/1024/4096, equal/first-byte/last-byte
mismatch. At 64 bytes and above scalar comparison won across these cases.
Equal memcmp median ns: 64 B 663 -> 429, 1024 B 7239 -> 4594,
4096 B 28101 -> 17431. Equal strcmp: 64 B 2425 -> 709,
1024 B 10701 -> 7781, 4096 B 37699 -> 30638.

Use scalar directly for PIE-eligible memcmp/bcmp/strcmp/strcoll on either CPU;
keep the existing eligibility boundary and libc for unmeasured alignments or
small memcmp. This removes pthread_getspecific/rseq dispatch from those calls.
Large memcpy still benefits strongly from CPU0 PIE and retains its dispatch.
The 8-byte memcmp arms execute the same libc path and show warm-up/noise; do
not label their variation an implementation gain. Revised policy needs its
own correctness and application validation before shipping.

## Harness failure corrected after owner observation

The first arithmetic comparison DID NOT RUN: shipped BusyBox tar rejects -z.
The launcher exited 1, but runsh ignored its exit status and waited 200 seconds
for a detached completion that could never arrive. Calling that workload
'running' was wrong. mul-launch.txt preserves the error. runsh now captures
RS_EXIT, rejects nonzero/missing launcher status before any completion wait,
and streams passive incoming evidence to stderr. Real board controls:
launcher exit 7 -> immediate CLI 5; immediate completion token -> CLI 0.
See harness-{failure,success}-control.txt. Extraction uses checked gzip then
tar xf. The corrected launch is recorded separately as mul-launch-fixed.txt.

One revised-policy QEMU threaded invocation printed a zero-mismatch result,
then the shell reported SIGSEGV. It remains unexplained, not waived by passes.
Five diagnostic repeats each of dispatch/scalar and a later clean pair passed.
The disabled-string QEMU control hits unsupported vendor PIE instructions and
is not a scalar/library baseline. Retained-core reproduction is in progress.

The host string crash is now diagnosed and fixed in the cancellation test;
see STRING-CRASH.md for both core-derived PCs, the freed thread address,
caller identification, handshake fix and 60 successful corrected runs.

## Clock feasibility on the actual board

clock-board-result.txt: corrected clock test, original ac8e clock candidate,
strings/soft-double interception disabled. CSR time reads work on both CPUs;
measured frequency is approximately 320 MHz. CPU0 syscall clock cost about
2.7 us; experimental MONOTONIC 0.71 us and RAW 0.48 us. CPU1 about 2.8 us
versus 0.99/0.80 us. Three-thread 10-second tests: syscall 1,729,404 calls,
experimental 2,378,880 calls, zero observed monotonic violations in either.
These are isolated diagnostics, not application gains or proof against the
source races. Five-second drift windows showed about 6.5 us maximum midpoint
error for MONOTONIC; even baseline measurement brackets contribute error.

A real syscall-saving opportunity exists. The current estimated-clock design
still cannot be shipped as a transparent vDSO: owner-pid publication race,
plain-data seqlock races, signal/contended fallback potentially stepping
backward, and delayed REALTIME steps remain unresolved. A short board pass
cannot supersede these findings. Existing generic kernel vDSO is the route to
review before inventing another clock service: RISC-V currently selects
GENERIC_GETTIMEOFDAY and builds vgettimeofday only on 64BIT; its fallback
syscall numbers and CSR read also need RV32/time64 handling. No kernel or
shipping image has been changed for this investigation.

Corrected comparison candidate on board: libs31fp 5c791e53, strtest 04bc0733.
comparison-board-result.txt: CPU0/CPU1 single-thread cases pass, forced migration
22501 cases / 292513 calls pass with nine rseq registrations, all microbench
cases exit zero, V3_DONE failures=0. Dispatch-off v3work moves CPU1->CPU0;
dispatch-on stays CPU1. The detached-cancellation correction is exercised on
silicon as well as in the 60-run host stress.
