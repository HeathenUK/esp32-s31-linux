# s31fp generic acceleration plan (2026-09-28)

**Goal.** Make code that runs badly on the S31 fast in *every* unchanged
application, not just OpenTyrian. Each speedup should be exact and generic,
delivered by pattern rather than by per-app tables.

**Scope.** This plan supersedes nothing. It orders the work across:
- `docs/s31fp-arithmetic-opl-roadmap-2026-09-27.md` (arithmetic, patch engine);
- `artifacts/opl-anatomy/` (the measured OPL kernel);
- `artifacts/s31fp-wide-survey/` (the partial survey);
- `tools/cloud/` (the board-less environment).

## Evidence labels

Every figure below carries one of these labels.

- **VERIFIED**: measured in this repo and checked again in review, with a
  negative control.
- **PROBE**: one survey agent's QEMU measurement, not yet adversarially
  verified. The survey hit a usage limit after 29 probes
  (`artifacts/s31fp-wide-survey/README.md`).
- **INFERRED**: reasoning, not measurement.

QEMU gives exactness and instruction counts, never time. Every ship
decision needs the board.

## What we already know

- **Three delivery routes have proven exactness under our contract**
  (bits, errno and fflags identical to the original):
  - **Copy-patch.** Full-body match, then an in-place copy. This is v2's
    soft-double; VERIFIED on the board, shipped.
  - **Interposition.** Exported symbols overridden by the preload. This is
    v3's strings and clock; VERIFIED, shipped.
  - **Call-site kernel.** A fused, exact replacement for a helper chain.
    `s31opl_out` is VERIFIED in QEMU: 0 mismatches in 4.09 M differential
    cases plus 909 k directed edge cases, and all 42 songs bit-identical.
    It is not deployed.
- **Heat outside graphics is thin in the target apps.** QuakeSpasm
  (`artifacts/gl/glquake/prof9/PROFILE.txt`):
  - libGL (our own rasteriser) is 53%;
  - the kernel is ~25%;
  - QuakeSpasm itself is 8.3%, with nothing above 0.8%;
  - libc is 4.2%, of which soft-double 1.8%, `memset` 0.7% and
    `sincos` 0.2%.

  Software Quake's in-binary `__muldf3` was 2.57% of CPU0 before v2
  (`perf-review-2026-09-23.md:171`). Generic wins will therefore be many
  small percentages, plus large per-call savings wherever a function is
  hot. That makes the heat census (Phase 0) the gate for everything.
- **The release toolchain did not build the board's userspace**
  (`tools/cloud/README.md`). Exactness results transfer, because the libgcc
  bodies are byte-identical. Instruction counts of our own C do not.
- **Rules that constrain this plan:**
  - off-the-shelf software only, changed at runtime;
  - never disable features;
  - memory is the binding constraint;
  - fresh boot per arm, 5+ repeats, watch the tail;
  - libc, ld.so and libm are off-limits, while interception preloads are
    allowed (`status-and-todo-2026-09-27.md:171-172`);
  - existing libGL work is not recreated in s31fp.

## Phase 0: instruments and identity (the board; gates everything)

**0.1 Installed-bytes census.**
- Record MD5s of the installed libc (352e9417), libm (inside libc), SDL1,
  SDL2, zlib, libpng, opentyrian, prboom, sdlquake, quakespasm and libGL.
- The local copies are known to differ from the installed ones
  (`docs/s31fp-forward-plan-2026-09-27.md`).
- Every rule and interposition below is validated against installed bytes
  only.

**0.2 Counting build (`S31FP_COUNT=1`).**
- A diagnostic `libs31fp.so` that counts calls per interposed symbol and
  per matched helper site, with no per-call output (CLAUDE.md: per-frame
  logging costs ~1 ms per frame).
- A destructor writes one line per counter to `/tmp/s31fp-count.<pid>` at
  exit.
- It pre-registers every Phase 1 candidate symbol (libm, `strlen`,
  `select`/`poll`, mutexes, `crc32`) as pass-through wrappers, so it
  measures call rates before any replacement exists.
- **Cloud:** extend `make cloud-test` with a wrapper-transparency test. The
  counting build must produce byte-identical outputs to plain libc across
  the v3 string and clock suites.
- **Board:** one timedemo or song run per app, collected after completion.
  This is diagnostic; no quiet-run numbers are taken here.

**0.3 Output.** A table of calls per second per symbol per app. Every
Phase 1 item's ship gate cites a row of it.

## Phase 1: generic interpositions (the v3 route, no rewriter)

**Common rules for every item:**
- Each item is one exported symbol family in `libs31fp.so`.
- The preload delegates to libc through `RTLD_NEXT` on any case outside
  its proven domain.
- It writes no libc or libm bytes.
- Each item has its own env toggle, default off at first ship.

**Validation template for every item:**
- **Cloud:** exhaustive or large differential against the installed-version
  source (bits, errno and fflags), all five rounding modes, preset flags,
  a negative control, and instruction counts.
- **Board:** exactness replay on silicon, then a quiet A/B with 5+
  interleaved pairs on fresh boots.
- **Ship gate:** a gain beyond the measured spread in an app where 0.3
  shows heat.
- **Kill:** no heat in 0.3, or any silicon mismatch.

| # | Item | Mechanism | Evidence | Notes |
|---|---|---|---|---|
| 1.1 | Double libm: `sin cos sincos tan atan atan2 exp log pow sqrt` | musl 1.2.5 source rebuilt with the v2 helpers, interposed | PROBE: 37-48% fewer instructions per call; 300 k cases per function, random frm and flags, 0 mismatches | Reaches QuakeSpasm's `sincos` and libc soft-double. Allowed (D1). |
| 1.2 | Float libm: `sinf cosf sincosf tanf expf logf powf hypotf` | Single-precision FPU evaluation, exact against musl, with a fallback | PROBE: 9-23× fewer instructions; unary functions exhaustive over 2^32 with 0 mismatches | The largest per-call win found. Allowed (D1). |
| 1.3 | `floor ceil trunc` | Integer bit manipulation | VERIFIED in QEMU: 0 / 20 M; floor 427→38 instructions | Arithmetic roadmap W6 |
| 1.4 | `strlen` (and any `str*` not already in `s31str.c`) | Zbb `orc.b` word loop | PROBE: 0.60-0.75× instructions; 200 k cases, 0 mismatches | Check the exact overlap with v3's list first |
| 1.5 | Zero-timeout `select`/`poll` | Answer from the fd state without a syscall | PROBE: 1 syscall → 0 per call. **Not exact:** 42 of 790,918 decisions differed (classified benign) | Must be redesigned to be exact, or fall back to the syscall; otherwise kill. D3. |
| 1.6 | Recursive/normal `pthread_mutex_lock/unlock/trylock` | Shorter owner path, one AMO | PROBE: 133 → 66 instructions per recursive pair; 3 M steps, 0 mismatches | High risk: it must stay coherent with every libc function that touches mutex internals (condvars, robust lists). Design review before any code. |
| 1.7 | zlib `crc32` | Zbc `clmul` folding | PROBE: 3.5× fewer instructions at ≥ 256 B; 24 k cases, 0 mismatches | Only zlib consumers (libpng in prboom). clmul latency on the S31 is unknown. |
| 1.8 | `printf %f/%g`, and app-static `__addtf3/__multf3/__subtf3` (map load) | Soft-quad replacements. Interposition for libc's; copy-patch for app-static copies (v2 route) | PROBE: 2.6× fewer instructions on Quake's per-vertex chain; 1.8 M cases, 0 mismatches | A load-time win, not a frame-rate win |

## Phase 2: generic helper improvements (the v2 route)

**2.1 `S31V2_LATEFRM` + `S31V2_LAZYLO`.**
- These are already in `rootfs/s31fp/v2/v2.S`, opt-in. VERIFIED in QEMU:
  - the full v2 suite (3 M multiplies plus all other ops) has 0
    mismatches;
  - a broken LAZYLO is caught with 26,328 mismatches;
  - the default build's `.text` is byte-identical to the shipped one.
- On OPL they cut CSR reads per sample from 66.8 to 42.0 (title) and from
  103.9 to 50.1 (heavy).
- INFERRED board gain: 3-4% (title) and 5-6% (heavy) at 25-30 ns per CSR
  access.
- **Board:** a quiet A/B on songs 36 and 5, using the existing b10
  arithmetic harness (`B10_OLD`/`B10_NEW`).
- **Ship gate:** a gain beyond the b10 spread (~1.7% title, ~3.5% heavy).
  Then make it the default for every patched executable.

**2.2 Rounding-mode probe (`OPL_FPROBE` pattern).**
- VERIFIED exact in QEMU; the probe itself is verified under all five
  modes.
- It trades CSR reads for FPU instructions, so it is a board A/B only.
- **Board:** also confirm that silicon `fadd.s` rounds as QEMU does
  (`artifacts/opl-anatomy/probe.c`).

## Phase 3: the call-site rewriter

**Purpose.** Handle slow code compiled *into* an application, where
neither interposition nor whole-function copy-patch reaches.

**3.1 Engine** (arithmetic roadmap W9, prerequisite).
- Transactional patching: all or nothing per object, with rollback.
- Byte re-verification of every window on cache hits.
- A per-rule kill switch.
- The cloud fault-injection and coherence hammer runs under `make
  cloud-test`.

**3.2 Window machinery.**
- Decode RV32IMAFC + Zba/Zbb/Zbs windows around matched helper call sites
  only.
- Track register data flow between the calls.
- Prove liveness, so the thunk clobbers nothing the original kept.
  Inlined sites need this, because the patch adds a call where the
  compiler had none.
- Place thunks in caves on pages that are already copied. Refuse the site
  when there is no cave.
- Heat comes from 0.2, never from an in-process sampler (killed in the
  roadmap).

**3.3 Rule verification harness (cloud).**
- For each rule: an offline proof sketch, plus an exhaustive or randomized
  differential of the original window against the rewritten window under
  QEMU, with negative controls.
- A rule is admissible only once its harness passes.

**3.4 Rules**, in order. Each rule needs its harness (3.3) and a heat row
(0.3).

| Rule | Pattern | Exact replacement | Evidence |
|---|---|---|---|
| R1 | Straight-line soft-double chain of multiplies and int conversions ending in `__fixdfsi`/`__fixunsdfsi` | Exact integer evaluation, falling back within 2^-20 of an integer boundary, when NX is clear or frm ≠ RNE (the `s31opl_out` algorithm, generalised) | VERIFIED on OPL: title 5,105 → 3,327 instructions per sample; 42 songs identical |
| R2 | `ext → op → trunc` float idiom | One F instruction, frm-guarded | VERIFIED in QEMU: 0 / 32 M outside RMM (roadmap W8) |
| R3 | `__divdi3`/`__udivdi3` by a constant (`li` operands) | Multiply-high sequence | INFERRED; roadmap C6 found generic 64-bit division slower, so only the constant-divisor sites |
| R4 | Repeated product at a site | Per-site memo keyed on operand bits, adaptively disabled on a low hit rate | VERIFIED on OPL (> 97% hits); generic payoff unknown |
| R5 | Chains with adds, ending in a conversion | Wider exact evaluator (operand alignment) | Not started. Part of the standard rule set under D2; built after R1 and ordered by 0.3 heat. Quake has 120 static chains ending in a conversion. |

**Deployment policy (D2).**
- Every rule is generic: it is matched by pattern in any binary, with no
  per-app tables and no per-app code.
- The aim is maximum coverage, applying the OPL result's effect to as many
  sites as possible.
- OpenTyrian's inlined `operator_output` is the first validation target,
  because its gain is already proven: 5,105 → 3,327 instructions per
  sample. It is reached through R1, not through an OPL-specific kernel.
- `s31opl_out` remains the reference implementation and oracle for R1.
- Coverage then grows rule by rule (R1 → R2 → R5 → R3), ordered by the heat
  rows from 0.3.
- A static chain census runs over every installed binary:
  `tools/s31fp/chainscan.py`, extended to report matching windows per
  rule.

## Phase 4: finish the survey (after the usage limit resets)

- Re-run verification (heat, exactness+rules, mechanism) on the 196 raw
  candidates, using the committed partial results as input. Do not re-run
  the 29 completed probes.
- Then run the synthesis and completeness critique, and commit
  `docs/s31fp-wide-survey-<date>.md`.
- **Kernel-side leads go to a separate kernel plan.** s31fp cannot reach
  them. Examples:
  - the sched/tick/timer path at 12.8% of QuakeSpasm CPU;
  - SBI timer ecalls;
  - generic atomic64 in the cache-range statistics.

## Owner decisions needed

- **D1: libm by interposition. RULED 2026-09-28.** The rule is that the
  *source* of off-the-shelf libraries (musl libc, libm, ld.so) is not
  edited. Interception is allowed. Phase 1 therefore proceeds:
  - Replacements live in `libs31fp.so`.
  - The installed libraries are never modified.
  - Reusing unmodified upstream source as the reference or basis for our
    own implementation (1.1: musl's libm source built against the v2
    helpers inside the preload) is our code, not an edit of theirs.
- **D2: OPL hold. RULED 2026-09-28.** Achieve the same effect as the
  fused OPL kernel, applied to as many cases as possible from now on. This
  means generic rules, matched by pattern, with OPL as the first
  validation target. See "Deployment policy" in Phase 3.
- **D3: `select`/`poll`.** Accept only an exact redesign (the current probe
  is 42 / 790,918 decisions off), or drop the item. Open.
- **D4: shared libraries.** Phase 1 items are exported symbols
  (interposition). Copy-patching app-static helpers inside DSOs (roadmap
  W4) stays default-off until 0.3 shows heat. Open.

## Order of work, and what can happen without the board

| Step | Where | Blocked on |
|---|---|---|
| 0.2 counting build + transparency test | Cloud | — |
| 1.1-1.4 implementations + differentials | Cloud | — |
| 3.1-3.3 engine and harness | Cloud | — |
| R1 generalisation, tested against OPL and synthetic chains; chain census tool | Cloud | — |
| 0.1 and 0.3 census | Board | — |
| 2.1 A/B, and the 2.2 silicon check | Board | — |
| Phase 1 and R1 ship A/Bs | Board | 0.3 heat rows |
| Phase 4 survey verification | Cloud | Usage limit reset |

**Stop conditions.**
- An item with no heat row in 0.3 is closed, and recorded with its numbers.
- An item whose quiet A/B stays inside the spread after 5 pairs is closed.
- Any silicon exactness mismatch stops that item and is recorded next to
  its code.
