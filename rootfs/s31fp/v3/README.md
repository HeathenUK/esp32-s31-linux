# s31fp v3

Copy/string policy is deployed from SD. Builds on v2's body-matched copy/trampoline interceptor. The
additional string functions currently use dynamic symbol interposition;
they do not patch internal calls in libc or replace static binaries.

Build through the repository Makefile:

```
./docker/build.sh '$S31_MAKE s31fp-v3'
```

This first builds and checks v2, then links its fresh reference objects into
v3. Outputs are in the build volume's `s31fp-v3/`. The optional
`s31fp-v3-stage` target installs the library into the SD rootfs overlay.

`S31STR=1` uses scalar code for eligible memcmp/bcmp/strcmp/strcoll on both
CPUs (measured faster on CPU0 too). Other eligible calls select libc's PIE
routines on CPU0 and scalar routines on the lent CPU using per-thread rseq state. Unregistered threads use scalar.
`S31STR=scalar` forces the scalar route; unset/0 retains libc dispatch.
`S31FP_DEBUG=1` enables routing counters (atomic and absent from normal hot
paths). A migration after the CPU check can still trigger the kernel's existing
PIE trap handler; this is not an atomic rseq critical section.

The scalar search uses musl's existing two-way algorithm, adapted only to call
our scalar primitives. See `sc_memmem.h` and `MUSL-COPYRIGHT`. The system libc
and dynamic loader are untouched.

The clock path now uses the kernel's standard timekeeper-backed vDSO instead
of the rejected approximate-clock implementation. Patch 0075 exposes RV32
`__vdso_clock_gettime64` and `__vdso_clock_getres_time64`; a small adapter uses
musl's existing ELF symbol lookup and leaves musl itself unchanged. REALTIME,
MONOTONIC, RAW and coarse clocks use kernel-maintained data. Unsupported clocks
fall back to syscalls. `S31CLK=0`, or an older kernel without these symbols,
uses libc. There are no user-space anchors, writer locks, calibration knobs or
resynchronization timers. Debug mode prints availability once at construction.

`clktest abi` checks exact syscall brackets, resolutions, errno, fork and signal
re-entry; `mono 35 3` covers counter rollover and concurrent CPU migration.
`clktest step` is root-only: it advances REALTIME by two seconds, checks it,
then restores the original time plus elapsed MONOTONIC time and checks again.
`board/clocks-run.sh` runs the diagnostic suite through runsh with a deadline;
it requires the existing `/root/afp2/oncpu` helper, not `taskset` or `timeout`.
Run diagnostics separately from applications. `cost` measures hot call overhead,
not game frame rate or worst-case page-fault latency.

The kernel vDSO code occupies one reserved RAM page and cannot be evicted to
SD. The small preload adapter and other v3 functions execute from the SD-backed
page cache and can be reclaimed; they are not explicitly pinned. Hot residency
is likely, not guaranteed. A cold fault can cost SD I/O, so clock-call microbench
results do not imply bounded latency under memory pressure.

`strtest` checks reference semantics, guard-page boundaries, repetitive search
inputs, joinable/detached threads, exit and cancellation. Detached completion
is synchronized, not guessed from a sleep. Detached cancellation targets keep
cancellation disabled until the parent's pthread_cancel has returned; otherwise
the target can free its pthread storage underneath the pending request. Its optional affinity hammer is a
correctness stress workload, never performance evidence. QEMU exercises the
scalar side only; real board routing is a separate requirement.

Use `scripts/board/deploy.py` for a candidate bundle, then the narrowly scoped
`board/strings-run.sh` through `scripts/board/runsh.py`. The latter accepts
`V3_DIR`, `V3_OUT`, and `V3_DONE_TOKEN`, launches detached and logs four
bounded cases. Use runsh `--done TOKEN --done-timeout 370` for passive serial
notification after exit, then collect output; require `V3_DONE failures=0`,
all completion lines and routing evidence. A launcher is not a
result. No probes, screenshots, transfers or sampling during runs.

Placement is part of the design: copied v2 bodies execute in application RAM,
but fallbacks/division and these interposed string routines remain in the
library. XIP saves RAM but may increase instruction-miss cost. Compare identical
bytes on SD/XIP, with colouring independently controlled, before choosing
placement or adding dedicated RAM text. Evidence and review notes currently
live under `artifacts/s31fp-copy-fix/` and `artifacts/s31fp-v3-review/`.
