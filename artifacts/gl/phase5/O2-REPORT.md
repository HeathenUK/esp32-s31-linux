# Phase 5, O2: fused fillers for every QuakeSpasm signature, and the filtered path

2026-09-26, host only: nothing here touched the board. Uncommitted, on top
of the phase 5 O1 working tree (snapshot `gl/bench/base5mt`, host lib md5
7e76983a). Design notes: gl/tinygl/README.s31, "Phase 5, O2". Raw outputs:
`artifacts/gl/phase5/o2/`.

No extension, GL state or pixel changes. Every new filler is gated
bit-identical to the general stage list it replaces.

## Headline (QuakeSpasm proxy, M RV32 instructions per counted frame, windows 0 / 1)

| trace (QuakeSpasm setting) | phase-4 final | O1 final | **O2 final** | O2 vs O1 |
|---|---|---|---|---|
| default, `gl_texturemode GL_LINEAR_MIPMAP_LINEAR` | 57.89 / 61.40 | 44.13 / 45.15 | **36.66 / 38.07** | -16.9% / -15.7% |
| `GL_LINEAR_MIPMAP_NEAREST` | - | 37.07 / 38.65 | **30.97 / 32.96** | -16.5% / -14.7% |
| `GL_LINEAR` | - | 34.87 / 35.97 | **29.57 / 31.20** | -15.2% / -13.3% |
| `GL_NEAREST_MIPMAP_LINEAR` | - | 32.22 / 32.16 | **25.76 / 25.89** | -20.1% / -19.5% |
| `GL_NEAREST_MIPMAP_NEAREST` | - | 29.02 / 29.11 | **23.11 / 23.32** | -20.4% / -19.9% |
| `GL_NEAREST` (lightmaps still `GL_LINEAR`) | - | 26.79 / 26.41 | **21.69 / 21.54** | -19.0% / -18.4% |
| library `S31GL_TEXFILTER=0` (all nearest) | 26.93 / 27.22 | 19.07 / 18.36 | **16.75 / 16.31** | -12.2% / -11.2% |
| no multitexture (phase-4 trace, QuakeSpasm "case 3"; the board's S31GL_MTEX=0 control) | 57.89 / 61.40 | 57.90 / 61.41 | **50.82 / 53.71** | -12.2% / -12.5% |
| same, `S31GL_TEXFILTER=0` | 26.93 / 27.22 | 26.85 / 27.11 | **22.94 / 22.92** | -14.6% / -15.5% |

- **Against the phase-4 final, the default frame is now -36.7% / -38.0%.**
- **Texture bytes are unchanged:** 4,892,320 B after load and 4,999,144 B at
  the end (default); 4,644,124 / 4,750,948 B with tf0.
- **Other heap +240 B.** That is the 192 B blend tables plus the malloc
  header, allocated at the first batch that needs them.
- **QuakeSpasm's texture modes.** Its default is trilinear
  (`gl_texmgr.c` 62-71, `glmode_idx = NUM_GLMODES - 1`). The six modes can
  only be set from the console (`gl_texturemode`, `gl_describetexturemodes`);
  the 0.96.3 menu has no filter entry. Lightmaps are always `GL_LINEAR`
  (TEXPREF_LINEAR, `r_brush.c` 598), and particles are `GL_LINEAR`. The HUD
  pictures follow the mode for their magnification filter.
- **The five other-mode traces** were captured with the app's own setting:
  `QS_CFG="gl_texturemode GL_..." capture-qs.sh p5a-<mode>`, which writes
  the setting to the autoexec.cfg of a copied game directory.

## Bars

| bar | result |
|---|---|
| 1. run-3a green, 0 suite apps worse than phase-4 final | **PASS**: `artifacts/gl/phase3a/p5-o2`. Suite 0 worse, and suitecmp is identical to O1's p5-b. Raster identical, pixels identical to p5-b, prims PASS, mtex 5/5. Host core_test 263/0, raster_gate 51/0, filt_test 32/0, fused_test 19/0. RV32 core_test 261/0, raster_gate, d2f, filt_test, fused_test 19/0. headless_gears md5 unchanged. qsr-fused 856/856 frames. |
| 2. frames using no new feature within +0.5% instructions, hash-identical | **PASS**: 74 bench lines against the phase-4 final (out-p5base), worst +0.422% (p4v8, 0.71 M; +0.352% at O1), 0 hash changes. The phase-4 QuakeSpasm trace guard (run-qsr.sh) is -12.2% / -12.5% default and -14.8% / -15.8% tf0, with 40/40 hashes per window. The filtered bench frames are 3-36% faster (filt2-5 -14 to -20%, filt9 -36%). |
| 3. every fused/fast filler bit-identical to the general path, with a gate | **PASS**: see the bar 3 detail below. |
| 4. every advertised extension to full spec, tested against Mesa | **Unchanged**: nothing new is advertised in O2. The O1 extensions keep their gates: mtex 5/5 against Mesa, core_test test_mtex. |
| 5. trace instructions per frame and texture bytes per lever | See the headline table and the per-lever table below. |

**Bar 3, the two gates.**

- **gl/tests/fused_test.c: 19 scenarios, host and RV32, 0 differing colour
  or depth values.** New scenarios:
  - the filtered world under every unit 0 filter;
  - one scenario per one-unit signature. Each covers all six filters,
    both wraps, both shadings (the other one is a neighbour signature
    that must stay general), and opaque or translucent colour, plus
    screen-aligned pictures at 1:1, 1.25:1 and a half-texel offset.
  - A mutation of each new shortcut and of the trilinear blend fails its
    scenario, so the gate is shown to cover them.
- **gl/tests/run-qsr-fused.sh, now in run-3a.** It replays every
  QuakeSpasm trace with `S31GL_FUSED=0` and with the default. On 10 traces
  and 856 full frames, every hash is identical between the arms and to
  live. The traces are: default, tf0, the five modes, the wide census
  capture, and the two phase-4 no-multitexture traces.

## What was done, per lever (default trace p5a, M instr/frame, windows 0 / 1)

| step | default | notes |
|---|---|---|
| O1 final | 44.13 / 45.15 | step 1: the trace re-captured with the O1 host lib (p5a, 84 frames). QuakeSpasm takes case 1. The profile matched O1's report exactly. |
| + filtered world fused (`zf_world_x`): depth first, inline sampling, square cache, one-multiply horizontal lerps | 40.33 / 41.61 | 31% of trilinear world pixels fail depth and are no longer sampled |
| + one-unit fused family (12 signatures) | 38.77 / 40.22 | every stage list QuakeSpasm draws is now fused (census) |
| + blend tables; level-0 bilinear shortcuts (weights 0 / 32: row, column, texel) | 37.65 / 39.24 | status bar: fv = 0 for 88% of pixels, 320 pixels from 256 texels across |
| + trilinear level code inline in the per-block path | 37.14 / 38.65 | also speeds the general path (bench filt*) |
| + nearest kinds one pass | 37.14 / 38.66 | NMN -3.3%, NML -3.1%, NEAREST -4.0% |
| + direct runners (no depth-stage call, no list walk) for both fused worlds | 36.69 / 38.11 | tf0 16.88 -> 16.75 |
| + size cleanup (the nearest kinds share one loop; cold selection code) | **36.66 / 38.07** | |

**Tried, measured and not kept** (the numbers are window 0 or 0 / 1):

- A one-multiply LERP in the general stages: +0.2%. It is one mul for a
  shift plus a sub, so it saves no instructions.
- The filtered world as one pass holding three levels: 40.96. Three
  passes: 41.35. Both lost to the two-pass version.
- A square cache on trilinear's finer level: 41.03 against 40.33 (86% of
  squares miss). The plain sample on a magnified level 0: 41.66.
- The weight shortcut on the MODULATE signatures: +0.03.
- `-fno-code-hoisting`, `-fno-tree-pre` and `-fno-schedule-insns2` on
  zpipe_fused.c: within ±0.1%.
- A sliding 2x2 window for minified misses. A census shows about 45% of
  misses are one-texel steps, which would save about 0.4 M/frame. Not
  built, for the complexity.

## Where a default frame goes now (prof-final-default.txt; O1 in prof-o1-default.txt)

| function | M/frame | per pixel |
|---|---|---|
| trilinear world `zf_wx_tri` | 17.70 | 88.5k px (60.6k alive): about 30 per pixel plus about 163 per alive pixel plus misses. The alive cost is three exact bilinear samples, the trilinear blend and the product. |
| bilinear world `zf_wx_bil` | 5.77 | 38.9k px |
| per-block level choice `zp_run_lod_mt_direct` | 2.77 | O1: 3.19 plus zpx_code's 0.61 |
| status bar and pictures `zf_bil0_*`, `zf1_sbar`, `zf1_blend` | 3.2 | about 60 per status-bar pixel |
| triangle setup (general fillers) | 1.88 | |
| everything else (geometry, alias, API) | about 6 | |

**Filtered against nearest.** A world pixel now costs about 3.6x its
nearest-filtered cost (it was about 5.2x at O1), and a whole default frame
costs 2.2x a tf0 frame. What is left is the exact arithmetic:

- phase 4's bilinear (5-bit weights, three rounded lerps on spread words)
  is about 30 instructions when the 2x2 square is cached and about 55 when
  it is not;
- a trilinear world pixel is three such samples: two levels and the
  lightmap;
- bar 2 forbids changing that arithmetic, because filtered phase-4 frames
  must stay hash-identical.

Below this floor there are only two ways to go: a different filter
definition, which would change hashes and needs an owner decision, or the
app's own `gl_texturemode`. With `GL_NEAREST_MIPMAP_NEAREST` the frame is
23.1 against 36.7.

## Costs

- **Code, Buildroot flags (brsize.sh):**
  - .text grew from 209,436 to 223,790 B (+14.4 kB, of which 3 kB is cold
    selection code in .text.unlikely).
  - The stripped library grew from 317,056 to 333,440 B.
  - zpipe_fused.o grew from 6.2 to 18.2 kB. The per-kind world loops are
    0.9-1.6 kB each, and a chunk runs one of them.
  - s31_tfilter.o grew by 2.4 kB: the direct runner, and the inline level
    code in each runner.
- **RAM:**
  - ZPipeX grew by 24 B, off the hot 2 kB.
  - The blend tables are 192 B, allocated lazily.
- **Not measured: the I-cache and XIP effect of +14 kB.** The host counts
  instructions only. The board A/B uses `S31GL_FUSED=0` as the control
  (the general path) and the default as the arm.

## Instruments added (gl/bench, tools/glref)

- `qscensus.sh GLDIR OUT TRACE` and `census.py`: pixels per stage list over
  the counted frames. It uses a `-DS31GL_CENSUS` bench build (s31_census.c,
  empty otherwise). q_replay.c turns the census on and off through weak
  hooks. Censuses: `o2/census-o1-p5a.txt` and `o2/census-final-*.txt`.
- `pcannot.py`: one function's disassembly with executions per frame from
  pcprof. This is how the spills, the hoisting and the cache hit rates
  above were found (the lightmap square hits 79%, trilinear's finer level
  14%).
- `capture-qs.sh` gained `QS_CFG` for app settings.
- `run-qsr-fused.sh`: the bar 3 trace gate.

## Open

- **Board:** ship through Buildroot and XIP (not done here, by the task's
  rules), then A/B `S31GL_FUSED=0` against the default, fresh boots, for
  timedemo fps and the I-cache effect of +14 kB.
- **The remaining filtered cost is the exact phase-4 bilinear.** A cheaper
  filter (fewer weight bits, or the lightmap point-sampled in 2x2
  luxel-blocks) would change pixels, so it needs a decision against Mesa
  and against bar 2's frozen hashes.
- **The per-block level choice (2.8 M) and the general triangle setup
  (1.9 M)** are next on the profile.
- **Mesa comparison.** O1's findings stand (frame 127, DARKNESS defect A),
  since O2 changes no pixel.
