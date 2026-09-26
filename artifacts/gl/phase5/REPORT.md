# Phase 5, O1: GL_ARB_multitexture, texture_env_combine, texture_env_add

2026-09-26, host only: nothing here touched the board. Uncommitted working
tree on top of cedffd3. Design notes are in gl/tinygl/README.s31, in the
"Phase 5, O1" section.

## What is advertised

When S31GL_MTEX is not 0 (the default), the library advertises:

- GL_ARB_multitexture, with GL_MAX_TEXTURE_UNITS = 2;
- GL_ARB_texture_env_combine and GL_EXT_texture_env_combine;
- GL_ARB_texture_env_add and GL_EXT_texture_env_add.

GL_VERSION stays "1.1". VBO and GLSL are not advertised. GL_DOT3_* returns
GL_INVALID_ENUM, because DOT3 belongs to texture_env_dot3, which is not
advertised.

- **Multitexture, full spec for two units.** The multitexture work covers:
  - glActiveTexture and glClientActiveTexture, with their ARB names;
  - glMultiTexCoord{1,2,3,4}{s,i,f,d}[v], with their ARB names;
  - per-unit enables for 1D, 2D and texgen S/T/R/Q;
  - per-unit bindings, texenv (mode, colour and combine), texgen planes
    and modes;
  - a texture matrix stack per unit;
  - per-client-unit texcoord arrays;
  - glPushAttrib and glPopAttrib of both units, and of the active unit;
  - glPushClientAttrib of both units' arrays;
  - display lists;
  - queries: ACTIVE_TEXTURE, CLIENT_ACTIVE_TEXTURE, bindings, IsEnabled,
    GetTexEnv, GetTexGen, GetPointer and CURRENT_TEXTURE_COORDS;
  - errors: INVALID_ENUM for a unit past the last, and INVALID_OPERATION
    inside Begin/End.
  - Unit 1 textures triangles, lines, points, smooth primitives, glBitmap
    and glDrawPixels. The raster position carries unit 1's coordinates.
- **Combine, full spec.**
  - Functions: REPLACE, MODULATE, ADD, ADD_SIGNED, INTERPOLATE, SUBTRACT.
  - Sources 0-2: TEXTURE, CONSTANT, PRIMARY_COLOR, PREVIOUS.
  - Operands 0-2: SRC_COLOR, ONE_MINUS_SRC_COLOR, SRC_ALPHA,
    ONE_MINUS_SRC_ALPHA. The alpha operands accept only the alpha pair, as
    the spec says.
  - RGB_SCALE and ALPHA_SCALE take 1, 2 or 4. Any other value gives
    GL_INVALID_VALUE.
  - Every value can be set and queried per unit.
- **GL_ADD** works on both units, for every texture format.

## Bars

| bar | result |
|---|---|
| 1. run-3a p5-b green, 0 suite apps worse | **PASS**. See the run-3a detail below. |
| 2. frames using no new feature: at most +0.5% instructions and hash-identical | **PASS**. See the bar 2 detail below. |
| 3. every fused filler bit-identical to the general path, with a gate | **PASS**. See the bar 3 detail below. |
| 4. every advertised extension to full spec, tested against Mesa | **PASS**. See the bar 4 detail below. |
| 5. QuakeSpasm instructions per frame and texture bytes, per lever | See the tables below. |

**Bar 1, run-3a p5-b.** artifacts/gl/phase3a/p5-b holds the run.

- Host: core_test 263/0 (239 plus 24 new in test_mtex), raster_gate 51/0,
  filt_test 32/0, fused_test 4/4, fmath_test PASS and f2d 0 mismatches.
- suitecmp: 0 apps worse.
- raster: identical to f3f6.
- pixels: identical to p4-final and p5-a. p3's line differs from the old
  f7 base exactly as it already did at p4-final.
- prims: PASS.
- mtex: 5 of 5 pages PASS, with 5 logs showing 0 differences.
- RV32 qemu-user: core_test 261/0 (it was 237 at p4-final), raster_gate,
  d2f, filt_test and fused_test all pass.

**Bar 2, frames that use no new feature.** Measured on gl/bench:

- bench.sh against phase-4 final (out-p5base): 74 lines, worst +0.352%
  (p4v8), 0 hash changes (measure/bench-vs-p4final.txt).
- run-qsr on the phase-4 QuakeSpasm trace:
  - default: +0.01% and +0.01%;
  - tf0: -0.31% and -0.39%;
  - hashes: 40/40 per window.

**Bar 3, fused fillers against the general path.** gl/tests/fused_test.c
renders 560 variants in two contexts, one of them with S31GL_FUSED=0. It
runs on the host and on RV32, and finds 0 differing colour or depth
values. The variants are:

- world case 1 and its neighbours;
- world with both units nearest (the inline walk), in every depth state;
- alias blended;
- alias without blending.

Replaying the whole QuakeSpasm case-1 trace, fused and general-only give
the same hash on all 84 full frames.

**Bar 4, the extensions against Mesa.** Three tests cover the extensions:

- gl/tests/glx_mtex.c: 5 pages against Mesa llvmpipe, with images and
  printed state and errors.
- core_test test_mtex: exact RGB565 results, plus the semantics Mesa
  cannot be compared on.
- Host QuakeSpasm 0.96.3 in tools/glref. It prints `FOUND:
  ARB_multitexture`, `ARB_texture_env_combine` and `ARB_texture_env_add`,
  and takes r_world.c and r_alias.c case 1. Profile evidence: zf_world*
  and zf_alias_00 run.

## QuakeSpasm per frame (M RV32 instructions, qsreplay.sh; windows 0 / 1)

The phase-4 final uses trace work/p4final. QuakeSpasm there has no
multitexture, so it takes its two-pass case 3. Phase 5 uses trace
work/p5mtex, the same demo frames, where QuakeSpasm takes case 1. The -tf0
traces are the same captures with S31GL_TEXFILTER=0.

| lever | default (trilinear, mips) | S31GL_TEXFILTER=0 |
|---|---|---|
| phase-4 final (case 3, two passes) | 57.89 / 61.40 | 26.93 / 27.22 |
| + multitexture and combine, general stages only (S31GL_FUSED=0) | 56.28 / 58.86 (-2.8% / -4.1%) | 32.76 / 33.31 (+21.6% / +22.4%) |
| + fused world and alias fillers (the default) | **44.13 / 45.15** (-21.6% / -23.3% against general) | **19.07 / 18.36** (-41.8% / -44.9%) |
| net against phase-4 final | **-23.8% / -26.5%** | **-29.2% / -32.6%** |

- **Without the fused fillers, multitexture is a loss for nearest
  filtering.** Phase 4's case 3 drew both passes with tier-1 fillers,
  while the general two-unit stage list costs more per pixel. The fused
  fillers are what make case 1 pay.
- **The worst frame improved too.** It fell from 67.9 to 54.8 (window 0)
  and from 77.2 to 57.6 (window 1). With tf0 it fell from 33.9 to 26.2
  and from 34.4 to 24.2.
- **Texture bytes did not change**, because the lever does not touch
  texture storage:

  | arm | after load (B) | at end (B) |
  |---|---|---|
  | default | 4,892,320 | 4,999,144 |
  | tf0 | 4,644,124 | 4,750,948 |

  Other heap grew by 5,120 B, the world product tables (38,056 to
  43,176 B at start). The peak is 6,901,048 B, unchanged.
- **The profiles** are in measure/qs-prof-*.txt.
  - Default: the trilinear and bilinear samplers still dominate:
    - zx_tri_r 11.1 M, unit 1's lightmap zx1_bil_r 8.4 M and zx_bil_r
      4.8 M;
    - the fused zf_world 3.8 M;
    - blended passes zo_sa_omsa 2.3 M.
  - tf0: zf_world_nn0 (the inline walk plus depth) 7.3 M, zo_sa_omsa
    2.3 M and the two setup fillers 1.9 M.

## Against Mesa, host QuakeSpasm

- **Case-1 trace (compare-qs.sh):** 79 of 80 frames PASS. The phase-4
  trace passed 64 of 80. Frame 127 FAILs with a tolerant diff of 1.266%,
  and it also FAILs at phase-4 final (1.079%).
  - That frame's diffs are ours being darker in a brightly dynamic-lit
    floor. Where the difference exceeds 16, the mean signed diff (R,G,B)
    is -11.7, -2.0, -14.3.
  - This is DARKNESS.md defect A, the RGB565 lightmap truncation. On case
    1 it is doubled by RGB_SCALE 2.
- **Live frames:** 120, 250, 400 and 700 all PASS, with tolerant diffs of
  0.109%, 0.152%, 0.042% and 0.051% (qs-live/).

## Costs

- **Code size, Buildroot flags (brsize.sh):**
  - .text grew from 170,034 to 209,436 B (+39.4 kB), and the stripped
    library from 276,096 to 317,056 B.
  - The largest items are unit 1's filtered samplers in s31_tfilter.o
    (+11.6 kB), zpipe_fused.o (6.2 kB), zpipe.o (+4.3 kB), the two-unit
    general filler ztriangle_genmt.o (3.8 kB) and vertex.o (+3.7 kB).
- **RAM:**
  - GLContext grew from 5,996 to 7,132 B, and GLVertex from 148 to 164 B.
  - The hot fields still end below the 2 kB load reach: mtex_used is at
    offset 1692.
  - The world product tables take 5 kB per context. They are allocated on
    first use.

## Mesa behaviour seen while testing (probe/)

- llvmpipe accepts GL_ONE as a combiner source.
- It accepts glActiveTexture and glMultiTexCoord of units past
  MAX_TEXTURE_UNITS, and raises no error. Ours raises INVALID_ENUM as the
  spec says, and these cases were taken out of the Mesa comparison.
- With unit 1 enabled, glBitmap only shows after a glFlush (glx_mtex page
  5 flushes), and its glDrawPixels does not match GL. Exact glDrawPixels
  through unit 1 is therefore checked in core_test.

## Knobs

- S31GL_MTEX=0: one unit, and the phase-4 extension string, for a board
  A/B.
- S31GL_FUSED=0: general stages only, for the gate's reference arm.
- s31gl_fused_stats(): counts of fused batches.

## Open

- **Frame 127 lightmap precision** (DARKNESS.md defect A, doubled by
  RGB_SCALE 2). The fix belongs to the rounding lever (O3, stored-lightmap
  precision).
- **Default QuakeSpasm filtering is still expensive.** The trilinear and
  bilinear samplers are 55% of a default frame. The next levers are fast
  filtered fillers and the MIPMAPS and TRILINEAR A/Bs on the board.
- **Blended alias and HUD passes (zo_sa_omsa, 2.3 M) are not fused.**
- **The I-cache and XIP effect of +39 kB of .text is not measured.** The
  host counter counts instructions, not fetch stalls. It needs a board A/B
  with S31GL_MTEX=0 as the control, done by the board owner.
- **Not built through Buildroot or shipped to the board.** That is the
  board agent's queue item 1.
