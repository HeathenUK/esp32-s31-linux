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

## The fused kernel (`fused.c`): exact and measured

`fused.c` is a drop-in, exact replacement for line 355:

- **P is memoised** on the bits of both inputs, in 64 direct-mapped
  entries. About 10 operators interleave, so a single entry never hits.
  That was measured: 4,654 instructions per sample with one entry, against
  4,047 with 64.
- **`× w` and `× trem` are 53×16-bit integer products**, each rounded
  round-to-nearest-even exactly as a double multiply would round.
- **Power-of-two tremolo and the `/16` become exponent adjustments.**
- **Truncation to int is a shift.**
- **NX is accrued as the helper chain would accrue it.** A non-RNE rounding
  mode, a zero or subnormal P, or an out-of-range value falls back to the
  original chain.

Exactness:

- **Differential test** (`test.c`): 4,094,400 cases, covering every int16
  waveform value for sampled P and both tremolo regimes, with 0 value and 0
  fflags mismatches against the helper chain. A perturbed negative control
  gives 4 mismatches, as expected.
- **End-to-end:** swapping it into `opl.c` leaves the audio hashes of all
  42 songs identical.

## What else would help, ranked

1. **Hand-written assembly kernel.** The C kernel is ~136 instructions per
   call, because generic 64-bit shifts and rounding are expensive on rv32.
   The operand shapes are fixed (53-bit × 15-bit, then × 16-bit or a power
   of two), so assembly with fixed shift counts should be roughly half that.
   INFERRED: title to ~3,400 instructions per sample.
2. **One fcsr read per operator per sample instead of seven.** QEMU counts
   do not show this at all. On silicon a CSR access costs 25-30 ns and v2
   removing CSR traffic was worth 2 µs/sample (V2-REPORT §9). If the patch
   covers the whole block loop, a further step is one read per 512-sample
   block. These are the largest silicon-only gains and need the board.
3. **Fused envelope step.** `amp > level ? amp *= mul : amp` in one exact
   call saves one call and one CSR read per decaying or releasing operator
   per sample (~8.5 per sample on the title song). The multiplicative
   sequence itself must stay.
4. **Not worth it.** Double-to-float substitution (not exact), skipping
   envelope samples (changes output), and SIMD. PIE gives no double
   arithmetic, and ~10 operators with dependent chains do not vectorise
   exactly.

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
