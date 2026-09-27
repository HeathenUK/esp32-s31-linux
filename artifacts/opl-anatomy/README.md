# OpenTyrian OPL engine: what it is made of, and what helps (2026-09-27)

QEMU-only (no board): correctness and instruction counts, never timing.

## Provenance

- **Source.** OpenTyrian `cf5dbeb` (Buildroot's pin) plus Buildroot's
  `0001-Move-definitions...` patch. `src/opl.c` is DOSBox/Ken Silverman's
  adlibemu with `#define fltype double` (`opl.c:33`).
- **Data.** `music.mus` from the freely distributed `tyrian21.zip`.
- **Rebuild is bit-identical to the board.** It reproduces the board's own
  audio hashes, title song 36 = `9979b782` and heavy song 5 = `23d6ac74`
  (`artifacts/s31fp-opl-order/result.txt`), and matches across all 42 songs.
- **Two kinds of binary.**
  - Profiles of the committed `rootfs/audiofp/oplbench` (board toolchain,
    libgcc helpers) are `profile-libgcc-*`.
  - The v2 and fused variants were built here with the release toolchain.
    CLAUDE.md explains why its counts can differ slightly from board builds.
- **Instruments.** `bbdump.c` is a QEMU plugin that counts executions of
  every translated block. `attr.py` attributes the counts to functions and
  counts helper calls per call site.

## What the engine spends its time on

Instructions per output sample, 2 s of each song, 44.1 kHz.

| Build | Title (36) | Heavy (5) |
|---|---:|---:|
| libgcc helpers (the app as shipped, no s31fp) | 12,049 | 18,502 |
| s31fp v2 (what the board runs) | 5,102 | 7,313 |
| v2 + fused `operator_output` (prototype here) | **4,047 (-20.7%)** | **6,028 (-17.6%)** |

With v2, `s31v2_muldf3` is still 52% of all instructions. The calls come
from two places.

**1. `operator_output` (`opl.c:345-357`), inlined twice into `adlib_getsample`.**

- It runs for every active operator on every sample: 9.57 times per sample
  on the title song.
- It computes `cval = (Bit32s)(step_amp*vol*w*trem/16.0)`, compiled as:
  - `P = step_amp*vol`. This is **loop-invariant**: `step_amp` changes only
    at envelope-step boundaries (`opl.c:390-395`), and `vol` only on a
    register write.
  - `(double)w * P`, where `w` is an int16 waveform sample.
  - `* (double)trem`, where `trem` is **exactly 65,536 when tremolo is off**
    (`tremval_const`, `opl.c:710`). That makes it an exact power-of-two
    scaling.
  - `* 0.0625`. This is `/16.0`, also an exact power of two.
  - truncation to int.
- That is 7 helper calls and 7 fcsr reads per operator per sample: about
  38 of the ~50 multiplies per sample.

**2. The envelope** (`operator_decay`/`release`/`attack`).

- Decay and release do one compare and one multiply per operator per sample
  (5.17 and 3.32 per sample on the title song).
- Attack does three multiplies and three adds (0.97 per sample).
- Every step must keep the exact multiplicative sequence, so none of it can
  be skipped.

The loop itself (`adlib_getsample` minus helpers, which also builds the
vib/trem tables) is about 450-700 instructions per sample. It is app code.

## Combined result: every idea, measured together

Per output sample, 2 s per song. "CSR" counts executed CSR instructions.
QEMU has no timing model, so the CSR column is the silicon-only lever: each
access costs 25-30 ns on the board (V2-REPORT §5), about 8-10 cycles at
320 MHz.

| Build | Title (36) instr / CSR | Heavy (5) instr / CSR |
|---|---:|---:|
| A: shipped v2 | 5,105 / 66.8 | 7,314 / 103.9 |
| B: v2 `S31V2_LATEFRM=1 S31V2_LAZYLO=1` | 4,963 / 42.0 | 7,060 / 50.1 |
| C: v2 + `s31opl_out` (frcsr) | 3,362 / 29.6 | 4,492 / 39.1 |
| **D: B + `s31opl_out` (frcsr)** | **3,327 / 26.0 (-34.8% / -61%)** | **4,442 / 31.2 (-39.3% / -70%)** |
| E: B + `s31opl_out -DOPL_FPROBE` | 3,427 / 16.5, +66.8 FPU ops | 4,841 / 13.9, +106.9 FPU ops |

For reference, libgcc without s31fp is 12,049 (title) and 18,502 (heavy).

**Every build reproduces all 42 songs' audio hashes bit-for-bit** against A,
and A matches the board.

**D is the recommendation.** E trades ~10-17 CSR reads per sample for
100-400 more instructions per sample. At 8-10 cycles per CSR access it
probably loses on the heavy song and roughly breaks even on the title song,
so it is a board A/B, not a default.

`operator_output.part.0` (239 instr/sample) is an artefact of swapping the
kernel in at the source level: the harness keeps a non-inlined wrapper. A
patch of the installed binary would call the kernel from the inlined site
directly.

### 1. `s31opl_out` (`opl_out.S`): exact fused `operator_output`, hand-written RV32

- **The algorithm is not a double-rounding emulation.**
  - With NX already set and frm = RNE, the chain can change no flag.
  - So compute `V = P*|w|*trem/16` exactly: `k = |w|*trem < 2^32`, then one
    84-bit product `m*k` (3 `mul`/`mulhu`) and a shift.
  - The two roundings move V by at most 2^-51 relative, which is under
    2^-20 absolute for |V| < 2^31.
  - So `trunc(exact) == trunc(chain)` unless V lies within 2^-20 above an
    integer n >= 1, or within 2^-20 below the next integer. Those cases run
    the original chain.
- **Other cases that also run the original chain:** NX clear, frm not RNE,
  P not normal or negative, `trem <= 0`, and |V| possibly >= 2^20.
- **Short cuts:** `w == 0` returns 0, and V < 1/2 returns 0 (NX is already
  set).
- **P = step_amp*vol** is memoised in 64 × 32-byte direct-mapped entries,
  keyed on all four input words. The hit rate is > 97%. A miss calls the
  exact v2 multiply.
- **Cost:** a 62-instruction fast path, frame-free. The arguments stay
  intact until every fallback decision is made.
- **Exactness:**
  - `test2.c`: 4,094,400 cases, 0 value and 0 flag mismatches.
    2,352,756 of them took the fast path (NX preset on 3/4 of calls).
    The negative control gives 4 mismatches.
  - `edge.c`: 909,351 directed cases within ±3 ulp of an integer V (all
    correctly routed to the chain) and at 2^-19..2^-12 off an integer
    (632,592 on the fast path). 0 value and 0 flag mismatches.
  - `test-rounding-modes.c`: RUP, RDN and RTZ, 0 mismatches.
- **`fused.c` and `fused2.c`** are the C forms. `fused.c` emulates each
  rounding step and costs ~136 instructions per call; `fused2.c` is the
  algorithm above and costs ~73. Both are exact.

### 2. `S31V2_LATEFRM` and `S31V2_LAZYLO` in `rootfs/s31fp/v2/v2.S`

Both are opt-in. The default build's `.text` is byte-identical to the
shipped v2.

- **LATEFRM** reads fcsr only on the multiply paths that can round. Zero,
  power-of-two and 21×21-bit (`both0`) products are exact in every mode and
  raise nothing. That removes one CSR read from ~49% of OPL's multiplies
  (A→B: title CSR 66.8 → 42.0 per sample).
- **LAZYLO** computes the general path's `L00` only when the bits of w1
  below the round bit are zero. It is worth ~1 instruction. `H00` cannot be
  deferred: it lands in w1, the round word, and can move it by up to a full
  word. This is smaller than first estimated.
- **Exactness:** the full v2 suite passes with both flags (mul 3,000,000 +
  749,391 directed-mode sets, and all 13 other ops), with 0 bits+fflags
  mismatches. A deliberately broken LAZYLO (L00 never computed) is caught
  with **26,328 mismatches**, so the suite exercises the ties that matter.

### 3. CSR-free rounding-mode probe (`OPL_FPROBE`, `probe.c`)

- **How it works.** `fadd.s` with dynamic rounding on 1+2^-24 and 1+3·2^-24.
  The two results differ by exactly 2 only under RNE; RTZ, RDN, RUP and RMM
  give 1.
- **Verified in `probe.c`** under all five modes, including RMM via `fsrm`.
  The probe raises only NX.
- **When it is used.** Only where the chain is provably inexact (nonzero
  fraction), so the NX it sets is the chain's own.
- **`w == 0` needs care.** The chain's `sa*vol` may be inexact even when
  `w == 0`, and a first version got this wrong (18 flag mismatches). The
  memo entry now records whether `sa*vol` was exact, from the mantissas'
  trailing zeros; unknown cases take the chain.
- **Board questions:**
  - whether 2 FPU adds and 4 moves are cheaper than one CSR read;
  - whether S31 silicon rounds `fadd.s` as QEMU does (IEEE-specified, but
    unverified);
  - reserved frm values trap on `fadd.s` rather than being read. That is
    only reachable by a program writing invalid frm itself.

## Hazards before any of this ships

- **App-specific.** Delivery means matching the installed opentyrian's
  inlined `operator_output` sequence and redirecting it. That is the item on
  owner hold (`status-and-todo` lines 167-171).
- **Registers.** A patch of an inlined site adds a call where none existed.
  Every caller-saved register live across it must be saved or proven dead,
  including the F registers for `OPL_FPROBE`.
- **Threads.** The memo is one table per process. Entries are written value
  first, then keys, which is not safe against a concurrent reader. OPL runs
  only in SDL's audio thread, but a general delivery needs a per-thread
  table or a sequence check.
- **Instruction counts are not board timings.** Measure on the board.

## How it would ship

This is an app-specific replacement of code inlined into
`adlib_getsample`. That is exactly the item the owner has on hold ("a fused
soft-double operator_output for OpenTyrian's OPL", `status-and-todo`
lines 167-171). Delivering it needs:

- s31fp to match the installed opentyrian's inlined sequence by full body
  (installed-bytes identity first; see
  `docs/s31fp-arithmetic-opl-roadmap-2026-09-27.md` W0d);
- a cave or copy for the kernel;
- the memo table in s31fp's own memory: 64 × 24 B = 1.5 kB per process.
