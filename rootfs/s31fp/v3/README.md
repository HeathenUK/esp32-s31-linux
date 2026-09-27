# s31fp v3 candidate

Not shipped. Builds on v2's body-matched copy/trampoline interceptor. The
additional string functions currently use dynamic symbol interposition;
they do not patch internal calls in libc or replace static binaries.

Build through the repository Makefile:

```
./docker/build.sh '$S31_MAKE s31fp-v3'
```

This first builds and checks v2, then links its fresh reference objects into
v3. Outputs are in the build volume's `s31fp-v3/`, with no overlay staging.

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

The experimental clock path (`S31CLK`) stays **off and unaccepted**. Review
found a lock-publication race and fallback monotonicity issues. Do not enable
it as part of string validation or claim transparent clock behavior.

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
