# gl/bench baseline (2026-09-25, before F3-F6)

The performance guard for the rasteriser work (plan section 2.2: "a frame
that uses no new feature runs today's filler"). Measured on the tree at
git HEAD f522dad (gl/api, gl/tinygl, gl/include), before any F3-F6 change.

## How

    gl/bench/bench.sh [GLDIR] [OUT]      # build + run all six images, ~3 s
    gl/bench/bench.sh gl/bench/base gl/bench/out-base   # the baseline tree

gl/bench/base is the baseline source snapshot (gitignored); recreate it with
`git archive f522dad gl/api gl/tinygl gl/include gl/build.sh gl/host-build.sh
gl/glx gl/tests | tar -x -C gl/bench/base --strip-components=1`. The
baseline tree has no S31GL_OBJONLY, so bench.sh compiles it with this
tree's gl/api/build-lib.sh (same flags; only the build script differs).

- The library objects are compiled by `gl/api/build-lib.sh` itself
  (`S31GL_OBJONLY=1`) with the board's Buildroot flags
  (`-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
  -mabi=ilp32 -mtune=esp-base`, then build-lib's own `-O2` for the
  rasteriser and `-Os` for the ABI layer, `-fPIC`), so these are the
  objects libGL.so.1 is linked from. No GLX: q_ui.c binds a calloc'd
  RGB565 buffer through `s31gl_bind_color`, as the GLX layer does.
- Demos: gl/tinygl/examples gears.c and texobj.c unchanged; teapotf is
  examples/teapot.c with float constants (the survey's variant).
- qemu-system-riscv32 `-icount shift=0`, bare metal, semihosting; minstret
  is exact. 3 warm-up frames, then 10 counted. Soft-double calls are
  counted with `ld --wrap` (dwrap.c). The run is deterministic: repeated
  runs give identical counts, so no noise band applies to this instrument
  (it is NOT a board timing: no PSRAM stalls, no I-cache model).
- Toolchain: ESP-IDF riscv32-esp-elf 15.2.0 (same GCC as the board's musl
  toolchain); libc/libm from the rv32imac/ilp32 newlib multilib.

## Numbers (M instructions per frame, soft-double calls per frame, frame hash)

| demo | 320x240 | 640x400 |
|---|---|---|
| gears | **2.6077** M, 4624 dcalls, fb d1bae36c | **3.7056** M, 4624, fb 4351e2f4 |
| texobj (GL_DECAL, RGB 8x8, nearest) | **0.4107** M, 96, fb 135e83c8 | **1.2687** M, 96, fb 9a3fac89 |
| teapotf | **10.1608** M, 23888, fb e2447185 | **10.9404** M, 23888, fb 25dc9565 |

texobj sets `glTexEnvi(GL_TEXTURE_ENV_MODE, GL_DECAL)` itself (it is not
the GL_MODULATE default), with an RGB texture: DECAL of an RGB texture is
the texel, which is what TinyGL's only textured filler draws.

## Other baseline facts

- RV32 libGL.so.1 (Buildroot mode of gl/build.sh, the flags above,
  unstripped): **.text 105,906 B**, .rodata 22,420, .data.rel.ro 3,568,
  .data 40, .bss 6,516. Soft-double call sites in the library: 15
  __extendsfdf2, 13 __truncdfsf2, 4 __divdf3, 3 __muldf3, 2 __floatsidf.
- Filler sizes (bytes): ZB_fillTriangleFlat 960, Smooth 1,502,
  MappingPerspective 1,564; the _nt/_nw variants 614-1,516.
- headless_gears 320x240x100 on the host rig: md5
  **181ba4b2c0020e2eccac1006a1ea1459**; core_test 125 passed, 0 failed.

## The guard

After a change, a frame that uses no new feature must cost at most
baseline + 1% instructions: gears <= 2.6338 M (320) / 3.7427 M (640),
texobj <= 0.4148 M / 1.2814 M. A changed `fb` hash on gears or texobj
means the pixels changed and must be a documented correctness fix.

(Superseded 2026-09-26: the limits are in limits.txt and bench.sh checks
them, re-measured with the current harness - see "After the review fixes".)

## After F3-F6 (2026-09-26)

Same bench, same flags, the final tree (`gl/bench/bench.sh`):

| demo | baseline | after | delta | frame |
|---|---|---|---|---|
| gears 320x240 | 2.6077 M | 2.6215 M | **+0.53%** | 287 px (0.37%) differ: strict GL_LESS |
| gears 640x400 | 3.7056 M | 3.7170 M | **+0.31%** | 312 px (0.12%) |
| texobj 320x240 | 0.4107 M | 0.4142 M | **+0.85%** | 501 px (0.65%): 1/w perspective |
| texobj 640x400 | 1.2687 M | 1.2738 M | **+0.40%** | 1,705 px (0.67%) |
| teapotf 320x240 | 10.1608 M | 10.2111 M | +0.50% | 5 px |
| teapotf 640x400 | 10.9404 M | 10.9896 M | +0.45% | 14 px |

Soft-double calls per frame are unchanged (4624 / 96 / 23888), and the
library's soft-double call sites are the baseline's exactly (15
__extendsfdf2, 13 __truncdfsf2, 4 __divdf3, 3 __muldf3, 2 __floatsidf);
none of the new objects has one (objdump -dr of every tgl_*.o touched).

Where the instructions went, measured by removing each piece:
- texobj: the perspective divide by 1/w instead of window z, +1.9 k per
  frame at 320 (4-5 instructions per scanline); the per-pixel loop is the
  same instruction count (`srl` by the texture's shift for `srli 13`).
- gears: the depth-state filler pointers made the per-triangle dispatch
  cheaper (gears was 2.6110 M before GL_SEPARATE_SPECULAR_COLOR); the
  separate-specular test in gl_shade_vertex costs ~11 k (lit vertices);
  the fog test per vertex ~4 k.

The frame changes are both correctness fixes, each proved to be the only
change: with GL_LESS mapped back to TinyGL's `>=` fillers, gears and
headless_gears are bit-identical to the baseline (headless_gears md5
181ba4b2c0020e2eccac1006a1ea1459 again); the new md5 is
**aeb114d6305ec92f487684aa2a761a0f**, identical on the host and on RV32
under qemu-user. Against Mesa, strict LESS moved glxgears f60 0.049% ->
0.030% tolerant-bad, gears f60 0.05 -> 0.03, geartrain f60 0.88 -> 0.76.

### What the general path costs (`gl/bench/feat.sh`, 320x240)

One feature switched on after the demo's init; tier 1 cannot draw these:

| feature | gears | texobj |
|---|---|---|
| none (tier 1) | 2.62 M | 0.41 M |
| GL_MODULATE (by white: tier 1 still draws it) | 2.62 M | 0.41 M |
| alpha test GREATER 0.1 | 5.09 M | 1.97 M |
| blend ONE, ONE | 5.11 M | 2.04 M |
| blend SRC_ALPHA, ONE_MINUS_SRC_ALPHA | 5.45 M | 2.23 M |
| fog LINEAR | 5.84 M | 2.27 M |

gears is ~6,000 spans of ~5 pixels a frame, so per-span work dominates
there; texobj's large spans cost ~100 instructions a pixel through the
stages against ~10 in TinyGL's fused filler. Fusing the common recipes
(plan G09) is the next lever; these numbers are its baseline.

### Code size (RV32 libGL.so.1, Buildroot mode, unstripped)

.text 105,906 -> **127,240 B** (+21,334, +20%); stripped file 181,756 ->
202,240 B. The new code: the strict-LESS triangle fillers 5.5 kB, strict
lines 1.5 kB, the fragment stages (zpipe.c) 7.3 kB, the general
triangle/line/point paths (raster.c, ztriangle_gen.c) 5.3 kB, selection
(raster_sel.c, built -Os) 2.0 kB, texture store and unpack 2-3 kB. A frame
that uses no new feature executes none of the new stages.

### Texture storage: RGB565 + A8 against ARGB4444 (`gl/bench/texfmt.c`)

The general path's MODULATE-of-RGBA texel fetch, 64,000 fragments at
random texels of a 256x256 texture: RGB565 + A8 **42.47** instructions a
fragment, ARGB4444 **40.62** (4% fewer). Chosen: RGB565 + A8. ARGB4444
halves colour precision (4 bits against 5/6/5) and cuts alpha from 8 to 4
bits (visible banding in particles, smoke and fonts' edges); A8 costs one
byte a texel, and only for formats with alpha (opaque textures stay 2).

## After F7 (2026-09-26: pixel paths, texgen, clip planes, stipple, 1D)

Same bench, same flags (`gl/bench/bench.sh`). Before = this tree at the
start of F7 (the "after F3-F6" numbers above, re-measured: identical).

| demo | before F7 | after F7 | delta | frame |
|---|---|---|---|---|
| gears 320x240 | 2.6215 M | 2.6210 M | -0.02% | fb bc8b3ce6, unchanged |
| gears 640x400 | 3.7170 M | 3.7179 M | +0.02% | cf6b6d75, unchanged |
| texobj 320x240 | 0.4142 M | 0.4143 M | +0.02% | ed495265, unchanged |
| texobj 640x400 | 1.2738 M | 1.2739 M | +0.01% | 1e49bce8, unchanged |
| teapotf 320x240 | 10.2111 M | 10.2015 M | -0.09% | ccac18a5, unchanged |
| teapotf 640x400 | 10.9896 M | 10.9800 M | -0.09% | 06347bc5, unchanged |

Soft-double calls per frame unchanged (4624 / 96 / 23888). headless_gears
md5 aeb114d6305ec92f487684aa2a761a0f on the host and under qemu-user
RV32, unchanged. The glRotate fix (README.s31) does not touch these demos:
they rotate about x, y and z only.

What the new paths cost (`gl/bench/pix.sh`, 320x240, M instructions a
frame; every frame starts with a 0.058 M glClear):

| workload | M/frame | per unit |
|---|---|---|
| 800 glBitmap 8x13 glyphs (20 lines, freeglut's way), nothing enabled | 1.892 | ~2.3 k a glyph |
| the same, blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA | 5.339 | ~6.6 k a glyph |
| 800 glBitmap(0, 0) moves only | 0.202 | ~180 a call |
| glDrawPixels 128x128 RGBA bytes | 0.378 | ~19.5 a pixel |
| glDrawPixels 64x64 RGB zoomed 2x2 | 1.024 | ~60 a window pixel |
| glReadPixels 320x240 RGB bytes | 1.758 | ~22 a pixel |
| glCopyTexSubImage2D 128x128 | 0.805 | ~46 a texel |

The first cut of glBitmap cost 6.1 k a glyph (a bit at a time, and the
stage list rebuilt per call); reading 32 bits a word with ctz for the runs
and caching the pixel stage list brought it to 2.3 k.

Code size: the library objects (esp-elf, board flags, `-shared
--gc-sections` of core.list, no GLX) .text 113,560 -> 130,472 B
(+16.9 kB); RV32 libGL.so.1 in Buildroot mode (with GLX, unstripped)
.text 143,970 B, stripped file 226,820 B (the F3-F6 report gave 127,240 /
202,240). New objects: s31_draw 8.5 kB and s31_rpos 2.3 kB (both -Os),
s31_xform 1.6 kB (-O2, per vertex), gl_pixels (the ~70 entry points)
4.7 kB, s31_pixels +3.6 kB (packing); 7.5 kB of stubs went away.

## After the review fixes (2026-09-26: findings P1-P7, G1-G8)

**The harness changed, so every number below is re-measured, baseline
included** (review P5):
- dwrap.c counts every soft-double libcall now (wraps.txt: the
  comparisons and the int conversions too), so "soft-double calls/frame"
  reads 7929 / 96 / 42851 where it read 4624 / 96 / 23888, and each wrapped
  call costs a few instructions more. Both trees are measured with it.
- The f522dad baseline tree is compiled with every TinyGL file -O2, as its
  own build-lib.sh did (build_q.sh sets S31GL_TGLCOLD=" " for a tree with
  no S31GL_OBJONLY). Before, it took the current -Os list; texobj 640
  reproduces the original 1.2687 again (the review saw 1.2688).
- bench.sh now also runs feat.sh, pix.sh and the new prim.sh, and checks
  every line against limits.txt (the plan's +1% guard for gears and
  texobj; tripwires at +2% for the rest). foot.sh reports the
  executed-code footprint (not gated).
- libm in these images is rv32imac soft-float newlib (no F multilib), so
  expf/powf/sqrtf are not what the board runs; none is per pixel.

| demo | baseline f522dad | after the fixes | delta | guard (limits.txt) |
|---|---|---|---|---|
| gears 320x240 | 2.6341 M | 2.6152 M | **-0.72%** | <= 2.6604 ok |
| gears 640x400 | 3.7321 M | 3.7196 M | **-0.33%** | <= 3.7694 ok |
| texobj 320x240 | 0.4107 M | 0.4145 M | **+0.93%** | <= 0.4148 ok (0.07% headroom) |
| texobj 640x400 | 1.2687 M | 1.2733 M | **+0.36%** | <= 1.2814 ok |
| teapotf 320x240 | 10.3125 M | 10.3917 M | +0.77% | not guarded (tripwire +3%) |
| teapotf 640x400 | 11.0921 M | 11.1711 M | +0.71% | not guarded |

Frame hashes: gears 6bf5e81c / 8021513b, texobj bf39f57c / d0419553,
teapot e4febfa5 / 37f84f25, all changed by the coverage rule (below).
headless_gears 320x240x100 md5 **38859f0c896efe6b12bc68e0fe3afe1f**, the
same on the host and on RV32 under qemu-user (so the board's fcvt-rounding
inline asm and the host's C fallback in ztri.h agree).

What changed in the fast path, and what it cost:
- **One scan conversion for both paths** (ztri.h, P1/G1). Tier 1's per-
  pixel loops are the same instruction count; setup is new. Per triangle it
  costs ~100 instructions more than TinyGL's (prim4: 400 flat 10x20 ortho
  triangles, 1.1893 -> 1.2169 M, +2.3%). gears pays it back: the flat
  colour now reaches the filler through the ZBuffer instead of being
  copied into all three vertices (18 loads/stores a triangle, -0.9%), and
  the triangles TinyGL's snapped facing test called degenerate and dropped
  (1,869 of 17,949 in 20 frames of headless_gears) are drawn, as GL
  requires, since they can cover a pixel centre the neighbours no longer
  cover.
- **Textured tier-1 triangles** (P2): q = 1/w once per vertex, no copies,
  no squeeze: prim7 (2,400 textured ~8x8 triangles) 3.8509 -> 3.4541 M
  (-10.3%; -2.9% against the f522dad baseline's 3.5556). prim8 (the same
  untextured, smooth) 3.6778 -> 3.6497 M (-0.8%; baseline 3.6847).
- **General-path lines** (P3), gathered into spans: 200 lines of ~200 px
  blended ONE,ONE 12.4655 -> 5.1281 M (~310 -> ~125 instructions a pixel);
  width 2 19.6612 -> 8.2769 M; y-major blended 13.7252 -> 5.6651 M. The
  stipple divide per pixel is a counter. TinyGL's own line (prim1) is
  0.5111 M against the baseline's 0.5048 (+1.2%: the end-point box check).
- **Texture uploads** (P7), qemu instructions a texel (ts.c of the review,
  128x128, old harness for both): RGBA 24.6 -> 17.2, LUMINANCE 29.7 ->
  12.2, RGB re-upload 30.7 -> 14.2.
- The general path (feat.sh, pre-fix -> now, same harness): fog 5.8932 ->
  5.6997 M (gears) / 2.2718 -> 2.2561 (texobj); blend ONE,ONE 5.1332 ->
  4.9378 / 2.0426 -> 2.0269; alpha test 5.1203 -> 4.9252 / 1.9693 ->
  1.9536; SRC_ALPHA blend 5.4782 -> 5.2830 / 2.2267 -> 2.2110. The pixel
  paths (pix.sh) are unchanged except glCopyTexSubImage2D, 0.8045 ->
  0.7233 (the texture store).

Soft-double: the call-site census of every library object (objdump -dr,
R_RISCV_CALL to __*df*/sqrt/pow/sin/cos/floor) is identical before and
after the fixes; none of the touched objects gained one.

Code footprint (foot.sh; blocks run at least once a frame, 32 B lines):

| | baseline f522dad | before the fixes (review) | after |
|---|---|---|---|
| gears 320: hot library bytes / lines | 9,652 B / 387 | 10,070 B / 404 | 10,208 B / 410 |
| texobj 320 | 6,242 B / 265 | 7,422 B / - | 7,400 B / 314 |

Object .text (the library objects, board flags) 166,257 -> 166,303 B
(+46): the five TinyGL filler objects shrank by 5.3 kB (one shared setup,
the affine filler and the squeeze gone), light.c grew 2.4 kB (a second
inlined body for two-sided lighting: one out-of-line body, or one inlined
body looped over the sides, cost gears 1.2%, measured), clip.c 1.7 kB
(two-sided swap, end-point checks). RV32 libGL.so.1 in Buildroot mode
(unstripped) .text 149,086 B (the phase-2 report: 144,066).

RAM (RV32 sizeof): GLTexture 288 -> 148 B (one stored image instead of
MAX_TEXTURE_LEVELS, lfmt 16-bit; review P6), GLVertex 144 -> 148 B
(ZBufferPoint gained fx, fy, q and lost sz, tz; the two-sided back colours
share dead storage), GLContext 4,688 -> 4,700 B.

## Phase 3a: geometry levers G01, G02, G14 (2026-09-26)

Full report: artifacts/gl/phase3a/LEVERS.md. New in the harness:
- `S31_BENCH_LIBM=musl` links the board toolchain's musl math objects
  (build_q.sh; every other script follows): a libm call costs what it does
  on the board, and musl's double-inside float functions are counted. The
  lever tables use it; limits.txt stays newlib (the default).
- geo.sh + geo.c (clear-heavy geo1 at 320x240 and 640x400, indexed mesh
  geo3, rotations geo4, mech-like geo5, specular + spot geo6, colour
  material geo7, strip assembly probe geo8) and glxgears_q.c (stock
  glxgears' drawing at 300x300); bench.sh runs geo.sh and limits.txt has
  tripwires for them.
- prof.sh/prof.py (per-function instructions, loads, stores and bytes of
  the counted frames, from q_ui.c's q_mark() calls; PROF_FN= lists a
  function's blocks), foot.sh FOOT_IMAGES= and a runtime (libm/libgcc)
  footprint, dcensus.sh (soft-double call sites per object), levers.py
  (per-lever table), rawdiff.py/raw2png.py (frames), suitecmp.py (two
  glref suite runs), brsize.sh (Buildroot-mode sizes of base3a and gl/).
- dwrap.c prints the per-name soft-double breakdown (dcalls.txt).

Result (musl libm, M insn/frame): gears 320 1.7656 -> 1.1774 (-33%),
glxgears-like 300 1.7046 -> 1.1240 (-34%), teapotf 320 5.2775 -> 2.6789
(-49%), texobj 320 0.4165 -> 0.3997; frames identical except one pixel of
gears 640. With newlib (limits.txt): gears 320 2.6152 -> 1.1774, 43 lines
within limits, 0 over. Soft-double calls per frame: gears 4577 -> 8 (the
demo's own), teapotf 19344 -> 0.

## Phase 3a, pixel side: the clears and the fillers (2026-09-26)

Full report: artifacts/gl/phase3a/LEVERS.md part B. Before = the tree
geometry left (gl/bench/base3p; it reproduces part A's final exactly).

New in the harness:
- **glxgears-2buf 300x300** (geo.sh, q_ui.c -DQ_BUFS=2): two colour
  buffers bound in turn, as GLX now presents a small drawable. It has a
  tripwire in limits.txt.
- q_ui.c marks its buffer retained (s31gl_set_retained, weak).
- **S31_BENCH_DEFS** (build_q.sh): -D flags for the library objects.
- **BRSIZE_TREES** (brsize.sh): which trees the size check compares.
- **gl/tests/zepoch_test.c + run-zepoch.sh**: the epochs and dirty rows
  against everything off, frame for frame.

Result (musl libm, M insn per frame):

| case | before | after | change |
|---|---|---|---|
| gears 320 | 1.1774 | 1.0300 | -12.5% |
| gears 640 | 2.2813 | 1.8794 | -17.6% |
| glxgears-like 300 | 1.1240 | 0.9652 | -14.1% |
| teapotf 320 | 2.6789 | 2.5676 | -4.2% |
| texobj 320 | 0.3997 | 0.3251 | -18.7% |
| clear-heavy 320 (geo1) | 0.1984 | 0.0971 | -51.1% |

- Every frame hash is unchanged.
- Bytes written per frame: glxgears 847 -> 563 kB, clear-heavy 337 ->
  85 kB (prof.sh; the clears are memory-bound on the board).
- With newlib: 44 lines within limits.txt, 0 over.
- The levers:
  - ZB_fill16, the 32-bit clear loop;
  - G03 exact depth epochs;
  - dirty rows;
  - two-pixel flat and Gouraud span loops;
  - row end pointers;
  - ztri_setup's scalar sort.

## QuakeSpasm proxy: a replayed GL trace on the instruction counter (2026-09-26)

A trace of stock QuakeSpasm 0.96.3's whole GL stream during `timedemo demo1`
(tools/glref/gltrace, recorded on the host rig against our libGL, which
advertises what the board's does) replayed on RV32 bare metal. The
replayer is linked with this tree's library objects, the board flags and
exact minstret, like every other image here.

    gl/bench/qsreplay.sh [GLDIR] [OUT] [TRACE]    # ~8 s; QSR_ENV="S31GL_X=0 ..." runtime toggles
    gl/bench/qsprof.sh OUT                        # per-function / per-object profile of the same run
    gl/tests/run-qsr.sh [GLDIR]                   # guard: phase-4 trace, hashes and <= +0.5%
    gl/tests/run-qsr-ab.sh "ENV_A" "ENV_B"        # bit-identity of two toggle arms (fused vs general)

- **The trace.** `gl/bench/qstrace/work/p4final/qs.gltr` (gitignored, 69 MB)
  covers:
  - the load phase: every texture and lightmap upload, recorded by value;
  - frames 100-139 and 400-439 in full, each window after 2 warm-up
    frames;
  - every other frame "state only" (tools/glref/README.md, "gltrace").

  The launch is the board's: `-heapsize 12288 -zone 384`, 320x240,
  windowed, no extensions (world and alias "case 3", Fitz renderer).
  Recreate the trace with `gl/bench/qstrace/mkbase.sh` (the 246e832
  snapshot in gl/bench/base5 and its host libGL, which builds bit-identical,
  md5 d0566593...), then
  `QS_LIBGL=/src/gl/bench/base5/out-host/libGL.so.1 tools/glref/gltrace/capture-qs.sh p4final`.
- **The replayer** (q_replay.c, tools/glref/gltrace/replay.c and the dispatch
  that gen.py generates from the trace's names):
  - every record becomes a direct call with the recorded arguments;
  - the platform half does what gl/glx/glx_core.c does: `set_doublebuffer`,
    `set_stencil_bits` and `set_retained` per context; two RGB565 colour
    buffers bound in turn (GLX's two SHM segments, 200 kB or less) and a
    caller-owned depth buffer; `s31gl_frame_end` at every swap.

  Frame N counts from the end of swap N-1 to swap N. The count includes
  swap N-1's `frame_end`, as q_ui.c's frames do, and excludes the
  replayer's hashing.
- **Harness floor.** `qsr_null.elf` replays the same trace into empty
  functions. It costs 0.35 / 0.24 M instructions per frame (0.6% and 0.4%
  of the totals): the decode and call overhead the proxy adds to the
  library's own cost.
- **Texture bytes.** `malloc`, `calloc`, `realloc` and `free` are
  `--wrap`'ed. A block allocated inside a texture call (`glTex*`,
  `glBindTexture`, `glGen/DeleteTextures`, `glCopyTex*`) counts as texture
  memory; everything else is "other" (the context, about 34 kB). An
  independent estimate from the uploads (`tools/glref/gltrace/trace.py
  texmem`: RGB565 plus A8 unless RGB) gives:
  - level 0: 4,620,508 B;
  - stored levels > 0: 236,844 B;
  - 145 textures after the load phase;
  - three 192 kB lightmap blocks (589,824 B).

  The measured values below are those plus about 24 kB of texture objects.
  The two agree.

### Phase-4 final (246e832, gl/bench/base5; `gl/bench/qstrace/baseline-p4final.txt`)

M instructions per counted frame (mean, min, max over 40 frames each):

| configuration | frames 100-139 | frames 400-439 | texture B after load / at end | frames = live |
|---|---|---|---|---|
| **default** (phase 4: QuakeSpasm's GL_LINEAR_MIPMAP_LINEAR = trilinear, mips stored) | **57.8946** (47.30-67.87) | **61.3988** (48.99-77.25) | **4,892,320** / 4,999,144 | 84/84 |
| `S31GL_TEXFILTER=0` (the pre-phase-4 nearest-of-level-0: the board's shipped behaviour) | **26.9329** (19.27-33.91) | **27.2208** (21.79-34.38) | **4,644,124** / 4,750,948 | 84/84 (own capture) |
| `S31GL_MIPMAPS=0` (bilinear from level 0) | 48.9153 | 52.5580 | 4,644,124 / 4,750,948 | - |
| `S31GL_TRILINEAR=0` (mip levels, no blend between them) | 50.7164 | 54.7733 | 4,892,320 / 4,999,144 | - |
| `S31GL_PERSPCOLOR=0` | 57.8449 | 61.3553 | 4,892,320 / 4,999,144 | - |
| harness floor (null) | 0.3465 | 0.2370 | - | - |

- **The load phase.** The 97 frames before the first full one replay in
  100.58 M instructions (97.58 M with `TEXFILTER=0`), texture uploads
  included. Peak library heap is 6,897,240 B.
- **Soft-double calls:** 0 per frame. The board's musl libm (`S31_BENCH_LIBM=musl`)
  gives the same counts to 4 decimals: no libm call is on this path.
- **Phase 4's filtering doubles QuakeSpasm's frame:** 57.9 M against 26.9 M
  with the old nearest sampling, +115%. Stored mip levels cost only +5.3%
  texture memory here (248 kB), not the +33% of a world-texture chain,
  because lightmaps, skins and the 2D art dominate the texture bytes.
- **Determinism.** Two runs, and newlib against musl libm, give identical
  counts. The frame hashes of every replayed frame equal the live host
  capture's, so the RV32 build and the host build of the library draw
  QuakeSpasm identically.

Where the default frame goes (`qsprof.sh`, all 80 counted frames, 59.65 M a
frame; artifacts/gl/phase5/qsproxy/prof-default.txt):

| object | share | top functions |
|---|---|---|
| s31_tfilter.o (the filters) | **51.7%** | `zx_tri_r` 18.6%, `zx_bil_ra` 17.5%, `zx_bil_r` 9.4%, `zp_run_lod` 5.0% |
| zpipe.o (the general stages) | **34.5%** | `zo_mul2` 6.9%, `ze_replace_rgb` 4.6%, `ze_replace_rgba` 4.5%, `zo_sa_omsa` 3.9%, `zd_lequal` 3.6%, `zdw_lequal` 3.3%, `zo_store` 3.0% |
| ztriangle_gen.o | 5.1% | `ZB_fillTriangleGeneral` |
| vertex, clip, raster, api (geometry) | 5.3% | `gl_vertex4f` 1.9%, `gl_transform_to_viewport` 0.7% |
| memset | 1.6% | |
| replayer | 0.6% | |

With `S31GL_TEXFILTER=0` (27.08 M a frame; prof-tf0.txt):

| object | share | top functions |
|---|---|---|
| zpipe.o | **63.5%** | `zo_mul2` 15.3%, `ze_replace_rgba` 9.8%, `zt_rr` 8.7%, `zo_sa_omsa` 8.6%, `zd_lequal` 7.9% |
| ztriangle.o | 9.3% | `ZB_fillTriangleMappingPerspective` (the base texture pass) |
| ztriangle_gen.o | 8.1% | `ZB_fillTriangleGeneral` |
| geometry | 11.1% | |
| memset | 3.5% | |

This is the shape the board profile found (artifacts/gl/glquake/LIBGL-OPPORTUNITIES.md
1a): the general stage chain several times the base texture pass, with
`zo_mul2`, `ze_replace_rgba`, `zt_rr` and `zd_lequal` on top.

What the proxy does not model:
- **It counts instructions, not time.** There is no PSRAM, D-cache or XIP
  I-cache model. On the board the geometry path is fetch-bound from flash,
  so its share of time there (immediate mode about 23% of libGL) is well
  above its share of instructions here (about 11%).
- **It does not include:**
  - the GLX present (XShmPutImage, lvdesk);
  - QuakeSpasm's own CPU;
  - the board's SDL 1.2.15, whose start-up GL calls differ slightly from
    sdl12-compat's.

**For a lever (plan bar 5):**
- **A lever that changes no app-visible feature:** replay the p4final trace
  on the new tree and compare with the table. `gl/tests/run-qsr.sh` does
  that and requires hash-identical frames.
- **A lever that changes what QuakeSpasm sees** (GL_ARB_multitexture,
  texture_env_combine): re-capture against the new host library
  (`capture-qs.sh`, default `QS_LIBGL` = gl/out-host) and report the new
  trace on the new tree against the p4final trace on base5. The texture
  bytes come from the same `qsr ... heap:` line.
- **A fused filler:** `gl/tests/run-qsr-ab.sh` renders every frame with the
  general path forced and with the fused one (once the library has such a
  switch) and requires 0 differing frames.

### Phase-5 final (2026-09-26, after the review fixes; artifacts/gl/phase5/REPORT.txt)

Trace p5a (QuakeSpasm with GL_ARB_multitexture, case 1), `qsreplay.sh`,
M instructions per counted frame, windows 0 / 1; texture bytes after load /
at end:

| configuration | phase-4 final | O2 | P5c (review tree) | **final** | texture B |
|---|---|---|---|---|---|
| default (trilinear) | 57.89 / 61.40 | 36.66 / 38.07 | 37.97 / 38.94 | **37.82 / 38.67** | 4,892,320 -> **3,635,120** / 3,657,000 |
| `S31GL_TEXFILTER=0` | 26.93 / 27.22 | 16.75 / 16.31 | 17.61 / 17.32 | **17.47 / 17.05** | 4,644,124 -> **3,259,237** / 3,281,117 |

Peak library heap 6,901,096 -> 5,681,952 B. The final column is the review
tree plus the O7 word loops and the s31_cap_index memo: all 84 frame
hashes identical to the review tree's in both configurations. Per-mode
numbers, the phase-4 (no multitexture) trace and the default-build bench
table against limits.txt: artifacts/gl/phase5/REPORT.txt.

Code size (Buildroot flags, `BRSIZE_TREES=... brsize.sh`): .text
p4final 170,034 -> O1 209,436 -> O2 223,790 -> final 300,130 B; stripped
276,096 -> 411,264 B.
