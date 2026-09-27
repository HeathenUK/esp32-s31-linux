# s31fp next campaign

## Objective and baseline

Accelerate unchanged applications through the existing s31fp interception routes,
while preserving semantics and accounting for dispatch, launch, paging and memory
costs. Current shipped baseline: kernel #402, SD libs31fp MD5
9767181c1c32d35d8eabc09108f87eff, copy/strings/real time64 vDSO enabled.
See s31fp-handoff-2026-09-27.md for validation and rollback. Prboom's last quiet
acceptance was 47.1 fps; this is not a statistically established improvement.
Tyrian still underruns. QuakeSpasm 20 fps and the other stretch targets are goals,
not forecasts. Existing libGL improvements must not be recreated in s31fp.

## Active execution order (owner correction)

Start with the OPL-focused arithmetic experiment (item 4 below), then continue
memory operations, selective coverage, placement and wider candidates. The
inventory already completed is supporting evidence, not a reason to stop.
An active persistent task now tracks this campaign. Commit milestones and keep
working; a milestone is not a handoff unless the owner requests one.

## Work and decision gates

1. **Coverage inventory — started.** Inspect actual ELF bodies and dynamic imports
   for prboom, SDLQuake, QuakeSpasm, OpenTyrian and their important application
   libraries. Reuse scanbench, but replace its obsolete prefix signatures with the
   shipping full-body/masked signatures. Record input hashes, matching functions,
   executable segments and exclusions. Static matches establish possible coverage,
   not frequency or speedup. Check local artifacts against board hashes before
   claiming they represent installed bytes. Deliver a ranked shortlist before
   changing runtime scope.
2. **Memory operations.** Extend the existing v3 correctness/microbenchmark matrix
   for memset first; inspect the installed implementation before writing one.
   Include zero/nonzero fill, zero/tiny/cache/page-sized lengths, every relevant
   alignment, guard pages, CPU0/CPU1 and migration. Reuse existing routines where
   possible. Retain CPU0 PIE memcpy where it wins; include routing cost for short
   calls. No change ships merely because a large aligned microbenchmark wins.
3. **Selective shared-library interception, conditional on step 1.** If missed
   helpers are material, extend the existing matcher/cache/patcher to an explicit
   set of application libraries. Exclude libc, ld.so, libm and s31fp itself.
   Reverify full bodies on cache hits; handle load bias, executable segments,
   per-object identity, relocation masks and page permissions. Do not globally
   scan every library on every exec. Establish safe load-time timing before any
   patch; do not patch concurrently executing dlopen objects. Measure cold/warm
   startup, page copies, cache size and correctness before enabling globally.
4. **Bounded arithmetic experiments.** Keep the present exact normal/special-case
   split. First compare MULHU/MUL ordering and register allocation on real S31;
   fusion is unproven. Then examine common power-of-two/narrow-operand paths and
   code size. Reuse static oracle tests plus separate unpatched/patched process
   dumps (the body matcher can otherwise patch the oracle). Preserve rounding,
   NaNs and flags. Review the documented NX/signal caveat against kernel signal
   restoration. Division is not a Tyrian priority; its float-seed/exact-correction
   design is already sensible. No generic double-to-float substitution.
5. **Explicit placement experiment.** Kernel vDSO already occupies reserved RAM.
   Measure identical s31fp bytes under current SD/page-cache placement versus a
   narrowly bounded resident hot-page set; separately account for copied helpers
   and their page colouring. Include faults, footprint, launch and memory pressure
   in isolated diagnostics. Never pin the whole library by default. A residency
   benefit must outweigh displaced application/cache memory and be fail-safe if
   locking/allocation is unavailable. No claim of hard latency guarantees.
6. **Wider candidates, evidence-gated.** Survey 64-bit integer division/remainder
   and conversion helpers, then externally called floor/ceil/trunc/scalbn/ldexp.
   Existing instruction counts/profiles and reachable calls must justify each.
   Larger reusable audio/format kernels are possible if measured cost warrants;
   do not replace a whole application or invent a new offload service. BitScrambler
   is a bulk stream/LUT possibility, not a proposed per-double arithmetic engine.

## Validation, tools and stop conditions

- All builds through repository Make recipes and docker/build.sh/$S31_MAKE.
  Extend an existing tool/recipe when necessary; do not source historical scripts
  as a substitute for the current build contract. No parallel agents requested.
- Use scripts/board tools and a single serial owner. Preflight actual dependencies:
  taskset and timeout are build-enabled but not installed on the current board;
  use the checked oncpu helper and established watchdog when needed.
- Diagnostics and quiet acceptance are separate. During application measurements:
  no probes, profiling, affinity changes, screenshots or transfers. Await native
  completion/events through the passive harness, then collect logs immediately.
- Initial comparisons: three interleaved pairs, reverse order between pairs;
  use fresh boots for application comparisons where memory state matters. Increase
  repeats only for a promising, ambiguous result, not an obviously neutral change.
- Correctness: exact bits/flags for arithmetic; guarded memory and thread lifecycle
  for strings; fault injection for patching changes; relevant application exits,
  native demo completion, audio output hashes and sustained underrun counts.
- Ship only when the useful-case gain exceeds observed spread, or a concrete
  correctness fix is established, without material launch/memory regressions.
  Negative experiments are recorded and closed. Keep each speculative experiment
  bounded to its matrix; do not chase an unreproduced historical crash indefinitely.
- Commit reviewed milestones and measured release notes. Keep #402/current SD
  library as rollback until a replacement passes; deploy accepted changes through
  the existing harness. Never sweep unrelated workspace changes into commits.

## First inspection

The current preload3.c explicitly scans only the main executable. The existing
scanbench instead uses scan2.h/sigs2.h prefix signatures, so using its counts as
shipping full-body coverage would be misleading. Updating that existing read-only
inventory tool is the first implementation task. No board process is being run
or observed for this step.

## First milestone completed

The existing scanbench now reads shipping sigs3 full bodies and relocation masks,
validates RV32 ELF segments, reports matching addresses/counts and fails malformed
inputs. Build/check through:

```
./docker/build.sh '$S31_MAKE s31fp-scanbench-check'
```

Nine fixture checks pass: exact match at segment end, rejection of a changed late
byte despite a matching prefix, permitted relocation changes, multiple executable
segments, exclusion of non-executable data, truncated body/header/segment and
wrong ELF class. No board activity was needed. Reports are in
`artifacts/s31fp-next/{inventory,imports,inventory-check}.txt`.

| Local artifact | Full-body matches | Implication |
| --- | ---: | --- |
| prboom capture | 6 | Main-executable route already targets this category |
| SDLQuake | 11 | Same |
| QuakeSpasm | 11 | Same |
| OpenTyrian buildroot target | 12 | Same |
| SDL 1.2 buildroot target | 13 | Potential missed shared-library coverage |
| SDL2 buildroot target | 12 | Potential missed shared-library coverage |
| SDL_mixer 1.2 buildroot target | 8 | Potential missed shared-library coverage |
| libGL buildroot / tier-7 local variants | 2 each | Only float/double conversions, not a broad arithmetic opportunity |

Every inspected game imports memset; so do SDL/SDL2 and libGL. SDLQuake and
QuakeSpasm import floor and sqrt; SDL2 imports additional rounding/scaling math.
Imports establish reachability, not hotness. Both SDL versions import time APIs
already covered by the shipped clock adapter.

**Identity caveat:** local prboom hash 618565c1 differs from the recorded installed
9aa9beaa, and the inspected libGL variants 4a6bbc22/b643055a differ from recorded
installed 01aa340f. Do not treat this as an inventory of installed bytes. The next
read-only board action, after confirming no workload owns the board, is to record
installed hashes and resolve these mismatches before any candidate deployment.
The offline tool examines every executable file segment and exact end-boundary
matches; the current runtime scanner considers the main executable's selected
segment and has its own scan bounds. Counts are potential matching bodies, not a
prediction of the number of runtime patches. Printed scan/report times are host
measurements and include reporting; they are not board startup-cost evidence.

**Next action:** verify installed artifact identities, inspect that libc's memset
implementation and extend the existing v3 matrix. SDL is the leading conditional
DSO-coverage candidate; do not implement broad shared-library scanning from match
counts alone. The board remains on the tested #402/v3 release unchanged.
