# TinyGL on the S31: what the hardware can do for GL (assessment, 2026-09-25)

Scope: after TinyGL stage 1 (docs/tinygl-plan-2026-09-25.md), which of the
S31's engines (PIE SIMD, PPA, the DMA engines, the second hart) are worth
building for, and in what order. This was read-only research. Nothing touched
the board and nothing was built. TinyGL sources were cloned and compiled on
the host under /tmp/claude-501/tgl/ (erysdren@7f229c8, C-Chads HEAD 36a7987,
Bellard 0.4.1).

Labels used throughout:

- **MEASURED**: measured on this board. The source is cited.
- **COUNTED**: a deterministic count. It comes either from geometry (an
  instrumented TinyGL on the host, where counts are platform-independent) or
  from static inspection of RV32 objects built with esp-15.2 GCC using the
  board's user flags (Makefile:27-40).
- **HOST**: timed on an Apple M1. These give proportions only, never board
  speed. The host has hardware double and a memset far faster than the
  board's, so on the board the host under-states both the double work and
  the clear.
- **ESTIMATED**: arithmetic from the above.
- **VENDOR**: Espressif's own figures, taken under a different setup.

IDF citations are from `/opt/esp-idf` in the build container (v6.1-dev
048ec57f22, which has an esp32s31 target), not the host's v5.5. Memory notes
are in `~/.claude/projects/-Users-gadyke-esp32-s31-linux/memory/`.

Each opportunity was judged through three lenses: hardware facts, economics
(does a recorded cost eat the gain), and project rules. The lens corrections
are folded into section 4.

---

## 1. Summary

The biggest gains need no hardware at all. On a machine with F but no D,
TinyGL's per-vertex path is full of soft `double`. Once that is gone, the
frame is dominated by clears and rasterisation. There, the PPA helps with
clears and with scaling, and PIE helps only a little, because spans are
short. The second hart is a poor worker for this code.

| # | what | kind | expected effect (basis) | cost |
|---|---|---|---|---|
| 1 | **Remove the soft doubles** (G01): `-fsingle-precision-constant`, `sqrtf`/`fabsf`, and our own float sin/cos/pow for glRotate and spot lights | library | teapot 28-39 → 16-23 ms/frame, gears 13-20 → 10-16 ms (ESTIMATED from COUNTED call sites × MEASURED libcall cost) | a flag and about 5 call sites; zero RAM |
| 2 | **Depth-range ping-pong** (G03): no depth clear on most frames | library | −1.8-1.9 ms/frame at 320x240, −5.8-6.4 ms at 640x400 (ESTIMATED from MEASURED memset rate) | two filler variants or a bias/XOR; loses one depth bit |
| 3 | **Geometry cleanups** (G02, G14): skip dead specular, a cheaper normalise, a cached inverse-transpose, inlined dispatch, a vertex cache for glDrawElements, and the array bug fixes | library | about 0.4-0.9 ms teapot (specular) + 0.7-1.3 ms (dispatch); 5-10 ms of a mech frame (inverse); indexed meshes up to about 6x fewer vertex transforms (ESTIMATED) | small; the vertex cache is about 5 KB |
| 4 | **Native texture sizes** (G13): stop resampling everything to 256x256 | library | saves up to 128 KB of RAM per texture plus a 192 KB upload transient (COUNTED); speed roughly neutral to +1-3 ms | small |
| 5 | **Render small, PPA-scale on present** (G04) for panel-size fullscreen windows | library + xshim | 4x fewer pixels; saves about 13-15 ms/frame of clear alone against 800x480, and about 1.1 MB of RAM (ESTIMATED); the scale costs 0.6 ms of CPU (MEASURED) | a visible quality policy; must be labelled as such |
| 6 | **Zero-copy fullscreen present + fence + PPA clear-at-swap + lighter present** (G05, G06, G15, then G07) | xshim/lvdesk + our DRM driver | about 3-4 ms lvdesk CPU/present (MEASURED analogue) + about 1.4-1.7 ms client CPU per cleared plane + about 1-1.5 ms lvdesk CPU per present (ESTIMATED). **Wall-clock can get worse**: single-buffering serialises the PPA read | kernel patch behind module params; CMA residency; 320x240/320x200 only |
| 7 | PIE span/blend fillers (G08, G09) | hand-written asm | under 0.3 ms at 320x240 (noise), 1-2 ms at 640x400 on gears; G09 unknown until an app blends | high: no PIE ALU throughput measured, no PIE interrupt hammer, 7.1 validation missing |
| 8 | Second-hart band split or GL thread (G11, G12) | library | best case 1.07-1.25x (ESTIMATED), and possibly zero; adds CPU | high; gated by two unmeasured basics (M1, M5) |

In plain words: do 1-4 first. They are cheap, they carry no hardware risk,
and until they are done every hardware A/B is confounded by soft-double time.
Item 5 is the one hardware lever that already exists and is measured, and for
panel-size fullscreen it is also the only design that fits in RAM. Item 6 is
the real PPA work. It is worth doing for CPU, but it has to be judged on fps
as well, because it trades a copy for a wait. PIE and the second hart come
last, and only after a few minutes of board micro-measurement show they can
pay at all.

---

## 2. What the hardware can and cannot do for GL

### 2.1 Capability table

| engine | can do for GL | cannot do | key numbers |
|---|---|---|---|
| **FPU (both harts)** | all transform, lighting, clipping and setup in `float` | `double`: every op is a libgcc call | `__muldf3` 705-723 ns, `__adddf3` 420 ns, float mul 24 ns (MEASURED, memory s31-soft-double-cost.md); floor about 4.5 ns/insn (same) |
| **PIE SIMD (hart1 = Linux CPU0 only)** | integer per-pixel work on 8×u16 lanes: Z test and write, Gouraud stepping, RGB565 pack/unpack, blend, modulate, alpha test, writemask | float lanes (none exist), gather (none), 32-bit lane multiply (none); clears (memory-bound); anything on hart0 | PIE memset 1.00x, memcpy 1.10-1.11x (MEASURED, docs/xespv-libc.md:17-20); in-cache 128-bit store about 1.3-1.6 cycles (MEASURED, docs/current-state.md:6044-6062); **no ALU, compare or lane-move throughput measured** |
| **PPA SRM** | scale the rendered frame to the panel (RGB565 or ARGB8888 in, RGB565 out), 8.4 fixed-point ratio, 1/16 steps | in place; depth (it would interpolate Z) | CPU scale 17 ms vs 0.6 ms of PPA CPU at the 512 KB bucket (MEASURED, esp32s31-lcd.c:1307, docs/current-state.md:7847); async cut commit wall time from about 6 to about 0.3 ms (MEASURED, patches/0037 README) |
| **PPA fill** | glClear of colour, and of depth as a raw 16-bit pattern; x/y block for scissor | anything not in `lcd_reserved` CMA | 32x32 73 us, 128x128 349 us, 800x100 1,137 us, 800x480 4,024 us = 192 MB/s; 13 us to program (MEASURED, esp32s31-ppa.c:538-544) |
| **PPA blend** | source-over, non-premultiplied, fixed or per-pixel alpha; A8/A4 fg with fixed RGB | additive, multiply, any other Porter-Duff op; per-fragment work | 400x300 in 4,254-4,305 us vs 20,571-25,023 us for a cached CPU blend (MEASURED, docs/accel-plan.md:663-676, 720-726) |
| **AXI GDMA** | 1-D byte memset (depth value 0 works), 1-D copy | 2-D/strided; 16-bit colour patterns without a driver change; a single >~150 KB memset (descriptor pool) | memcpy 384,000 B in 3,654 us, about 450-480 us "fixed" (MEASURED, docs/accel-plan.md:100-116), probably a completion-wait artefact; **memset never measured** |
| **2D-DMA M2M, BitScrambler, JPEG, ETM** | nothing per frame | - | see section 5 |
| **hart0 (Linux CPU1)** | plain rv32imafc code incl. FPU | PIE (illegal; the task bounces to CPU0) | at most 0.82-0.84 of a core (docs/perf-review-2026-09-23.md:257); lvdesk takes about 30% of it in games (docs/smp-finish-plan.md:128-135); sleeping cross-hart hand-off 300-500 us (MEASURED, docs/worklog-2026-09-19.md:1775-1790, docs/perf-plan-2026-09-23.md:1550) |

### 2.2 Rules that decide most questions

- **Only `lcd_reserved` CMA is reachable by the PPA** (esp32s31-ppa.c:504-515).
  Heap, SysV SHM and the XLITE-SHM memfd are invisible to every engine. No
  userspace fill ioctl exists; the fill is debugfs-only (esp32s31-ppa.c:812-849),
  and DIAG=0 compiles debugfs out. The exported ioctls are PPA_COPY,
  PPA_BLEND and PPA_CLUT (esp32s31-lcd.c:3994-4010). Under a fullscreen
  client, CmaFree was 1,096 kB of 4,096 (MEASURED, esp32s31.dtsi:126-131),
  and CmaFree is not headroom (memory s31-watermark-boost-oom.md).
- **Completion is the 2D-DMA RX SUC_EOF**, spun for `ppa_spin_us=300`, then
  a sleep (esp32s31-ppa.c:555-605; memory s31-ppa-completion-signal.md).
  Waking costs 0.7-1.1 ms (MEASURED, docs/accel-plan.md:896-903). So an
  engine op only frees the CPU if it runs well past 300 us, which means
  roughly 128 KB or more (memory s31-offload-not-purchasable.md). A 256x256
  blend left the CPU about 88% free.
- **Cache maintenance is CPU time.** rv32 has no memory-type PTE bits
  (`_PAGE_NOCACHE 0`, `_PAGE_MTMASK 0`, arch/riscv/include/asm/pgtable-32.h:27-29),
  so dumb-buffer "write-combine" mappings are ordinary cached mappings.
  Writing back 491,520 B cost 1,117-1,161 us when the CPU had dirtied it, and
  295-372 us when the engine had (MEASURED, docs/accel-plan.md:582-590).
- **The CPU memset rate is 79-88 MB/s** (MEASURED: fb 79, heap 81 MB/s,
  docs/accel-plan.md:883; 88 MB/s, docs/current-state.md:5537; a membw median
  of about 76, artifacts/smp-finish/canarymem-*). One 320x240 RGB565 plane
  (153,600 B) is 1.75-1.94 ms. Colour plus depth is 3.5-3.9 ms. At 640x400,
  the two planes (1,024,000 B) are 11.6-13 ms.
- **The D-cache is one 64 KB, 2-way, 64 B-line cache shared by both harts**
  (docs/s31_hardware/s31_cache.txt:2-7). **The I-cache is 16 KB per hart**
  (IDF Kconfig.cache `CACHE_L1_ICACHE_SIZE 0x4000`; esp32s31_cache.c:40-48).
  Several research reports said 32 KB; the TRM's 32 KB is the total of both
  harts' caches. libGL runs from XIP flash through that 16 KB, so filler code
  size costs time.
- **PIE state is already saved and restored for every user task.** The SBI
  hook is unconditional because musl's memcmp and strcmp are vectorised
  (esp32s31-ext.c:112-120). Using PIE in libGL adds no switch cost and needs
  no kernel change.
- **PIE on the lent CPU:** the instruction traps, the task migrates to CPU0,
  and the old mask is restored unless the task trips the bounce limit
  (`PIE_BOUNCE ?= 1000`, Makefile:444). **A task whose own mask excludes CPU0
  is pinned to CPU0 permanently** (esp32s31-ext.c:396-410). The plan's
  "pthread worker pinned to CPU1" (tinygl-plan:81-83) would therefore
  collapse onto one core without any error the first time it executed a PIE
  instruction, including one inside musl's memcmp or strcmp. The cost of one
  bounce has never been measured (ESTIMATED 0.3-1 ms).
- **HWLoop (`esp.lp.*`) corrupts userspace under Linux** (memory
  s31-hardware-loop-corruption.md), so no PIE loop may use it. **CFG is not
  in the PIE save area** (opensbi esp32s31_coproc.S:72-79), so CFG unaligned
  mode must not be used. The 353-of-354 PIE form validation predates Linux 7.1
  (docs/S31_extension_experiments.md:29-50) and has not been re-run.
- **No compiler support for PIE.** GCC 15.2 has no intrinsics and does not
  auto-vectorise, and q0-q7 are not a register class (HOST check). Every PIE
  loop is a hand-written `.S` assembled with `xespv2p2` spelled out.

### 2.3 The "write-combine 3-stream 18x" number is in doubt

Memory s31-write-combine-streams.md records 1-stream RMW at 1.0x and 3-stream
at 18x on dumb buffers, and blames write-combine fill buffers. Two things
contradict that mechanism:

- rv32 has no write-combine attribute (pgtable-32.h:27-29).
- docs/accel-plan.md:880-889 still calls the related 13.7x figure
  "unconfirmed" because lvdesk's own probe saw no penalty.

A better-fitting HYPOTHESIS is cache-set aliasing. CMA aligns each allocation
to `min(get_order(size), CONFIG_CMA_ALIGNMENT)` (kernel/dma/contiguous.c:394-399),
so equal-size buffers are congruent modulo the 32 KB way size, and three
streams then fight for two ways. This matters for any design that moves GL
buffers into CMA. Test M2 (section 4, prerequisites) decides it.

---

## 3. Where TinyGL spends its time

### 3.1 The code

erysdren's rasteriser, clipper, vertex, lighting, line, specbuf and clear
code is byte-identical to Bellard 0.4.1 (COUNTED: zero changed lines in
ztriangle.[ch], clip.c, vertex.c, light.c, zline.[ch], specbuf.c, clear.c).
Only zbuffer.c, zmath.c and zgl.h differ. It is 1997 code, and nobody has
tuned it.

- Z is 16-bit, interpolated with 14 fraction bits (zbuffer.h:10-12). The test
  is `zz >= zpix` (ztriangle.c:4). The clear value is hard-coded to 0 (clear.c:17-28).
- Triangles always test and write Z. `glDepthMask` and `glBlendFunc` are
  no-ops (include/GL/gl.h:842, 845). There is no blending and no GL_MODULATE
  (texture.c:197).
- Every texture is resampled to 256x256 RGB565: 128 KB each, plus a 192 KB
  RGB888 transient at upload (texture.c:146-175).
- The colour buffer can be supplied externally: `ZB_open(..., frame_buffer)`
  (zbuffer.c:66-76). The present needs no copy inside TinyGL (ostinygl.c:145-146).

### 3.2 The double problem on F-without-D

GCC narrows `float = 1.0/float` to `fdiv.s`, so **the triangle and span code
has no double calls** (COUNTED: ztriangle.o has no df libcalls). What remains:

| site | per | double work (COUNTED) |
|---|---|---|
| `gl_clipcode`, zgl.h:351 (`w1*(1.0+CLIP_EPSILON)`) | every vertex, and 4x per clipped triangle | `__extendsfdf2` + `__muldf3` + `__truncdfsf2` |
| `gl_V3_Norm`, zmath.c:248 | every lit vertex under GL_NORMALIZE | `sqrt` (146 insns of integer code + `__adddf3`) |
| `gl_shade_vertex`, light.c:220, 250, 276, 277 | every lit vertex, per light | `sqrt` ×2, `pow`, `__gtdf2`, `__muldf3`, 6 extends, 3 truncs |
| `glopRotate`, matrix.c:119-157; zmath.c:212-213 | per call | `sin`, `cos`, `sqrt`, 2 `__divdf3`, `__muldf3`; about 20 us each (ESTIMATED); mech makes 640 calls per frame |
| glFrustum, glLight, viewport, specbuf setup (`pow` ×1025 per new shininess), glu.c | per call | various |

`-fsingle-precision-constant` alone removes the clipcode, `__gtdf2` and
viewport doubles. Changing the call sites to `sqrtf`/`fabsf` does the rest,
and `sqrtf` becomes a bare `fsqrt.s` (COUNTED). **musl's `sinf`, `cosf` and
`powf` still compute in soft double internally** (COUNTED: `__sindf` has
8 `__muldf3`; `powf` has 14 `__muldf3` and 10 `__adddf3` call sites). So
glRotate and spot lighting need our own float polynomials.

Caveat from verification: these libm counts come from the toolchain sysroot's
`libc.a`. That is not byte-identical to the shipped Buildroot `libc.so`,
whose memcmp and strcmp are vectorised (esp32s31-ext.c:112-120). libm is
probably the same, but that is unverified. Re-count against the shipped
library before quoting these as board facts.

A fallback already exists for any double work that remains: rootfs/s31fp,
a bit-exact `__muldf3` at 295-296 ns instead of 705-736 (MEASURED, memory
s31-soft-double-cost.md). Because libGL is ours, it can be linked directly
into the library.

### 3.3 Per-frame counts (COUNTED, host-instrumented, platform-independent)

| workload | verts (lit) | spec sqrt | tris submitted/culled/rasterised | span px | px/line |
|---|---|---|---|---|---|
| gears 320x240 | 1076 (1076) | 505 | 969/414/397 | 32,486 | 4.6 (flat 4.8, smooth 2.5) |
| gears 640x400 | 1076 | 505 | 1030/376/374 | 113,381 | 9.0 |
| texobj 320x240 | 8 (0) | 0 | 4/0/4 | 17,811 | 45 |
| teapot 320x240 | 4800 (4800) | 2293 | 1600/692/675 | 7,852 | 1.8 |
| mech 320x240 | 45188 | 16284 | 38854/0/14171 | 217,958 | 3.7 |
| morph3d 320x240 | 2300 | 4600 | 2116/0/1090 | 3,680 | 1.4 |

Spans are very short. The scanline overhead is 44 instructions (flat) or 75
(smooth), against 6.75-12.75 instructions per pixel in the inner loop
(COUNTED, RV32 objects). So on every demo except texobj, per-scanline and
per-triangle overhead exceeds per-pixel work. That is what limits SIMD.

### 3.4 RV32 cost model (ESTIMATED; /tmp/claude-501/tgl/model.py)

Inputs: the counts above, COUNTED loop instruction counts, 4.5-6.5 ns per
instruction, clear at 88 MB/s, a 275-316 ns PSRAM miss (MEASURED,
docs/perf-review-2026-09-23.md:460), and assumed miss rates. The model
excludes lvdesk, X and the app itself.

| workload | clear | vertex core | double libcalls | tri setup | scanline | pixel ALU | mem stall | total ms (fps) |
|---|---|---|---|---|---|---|---|---|
| gears 320x240 | 3.5 [21%] | 2.1-3.1 | 2.7-3.7 [19%] | 0.6-0.9 | 1.4-2.1 | 1.1-1.6 [8%] | 1.3-5.1 | 13-20 (50-78) |
| gears 640x400 | 11.6 [35%] | 2.3-3.3 | 2.8-3.8 | 0.6-0.9 | 2.6-3.7 | 4.0-5.7 [15%] | 2.7-10 | 27-39 |
| texobj 320x240 | 3.5 [46%] | ~0 | ~0 | ~0 | 0.2 | 1.5-2.1 | 0.7-3.4 | 6-9 |
| teapot 320x240 | 3.5 [10%] | 8.6-12 | 11.6-16 [41%] | 1.1-1.6 | 1.5-2.1 | 0.5-0.7 | 0.8-2.9 | 28-39 |
| mech 320x240 | 3.5 | 82-118 | 104-142 [38%] | 24-34 | 20-29 | 13-19 | 11-41 | 257-387 |
| morph3d 320x240 | 3.5 | 4.1-6.0 | 9.4-13 [46%] | 1.7-2.4 | 0.9-1.3 | 0.2-0.3 | 0.5-1.8 | 20-28 |

HOST self-time profiles agree on shape (host has hardware double and a fast
memset, so its clear and double shares are lower bounds). gears: Flat filler
55.9%, glopVertex 14.2%, shade 9.6%. teapot: shade 28.9%, glopVertex 21.5%,
Smooth 19.2%, gl_add_op 7.3%. texobj: MappingPerspective 77%, clear 22-25%.

Reading: once the doubles are gone, a lit demo at 320x240 is roughly one
third clear, one third geometry and one third raster. The only per-pixel
ALU share large enough to matter for SIMD appears at 640x400 or on
long-span scenes, and the long-span demo (texobj) is textured. Every
textured triangle takes the perspective filler (clip.c:403-408), which PIE
cannot vectorise because there is no gather.

**The whole model needs a board profile (M4) before any number is quoted as
fact.**

---

## 4. Surviving opportunities, ranked

All 15 candidates survived. None collected two refutations; G08 collected
one (economics). The ranking is by value per unit of risk. Every experiment
follows the house rules: fresh boot per arm, at least 5 runs, first run
discarded, spread reported, worst frame reported, each result under
10 minutes (memory board-test-10-minute-rule.md). Screenshots and mjpegrec
recordings go in separate runs from timing arms, because mjpegrec shares the
2D-DMA with the PPA.

### Board prerequisites (each a few minutes, no GL code)

| id | measurement | decides |
|---|---|---|
| M4 | stage-0/1 board profile of gears, teapot and texobj with the h1s hart0 PC sampler (docs/smp-finish-plan.md:143-149) plus thread-CPU timers around ZB_clear, the fillers and the present | replaces the model in 3.4; gates everything |
| M2 | blendbench 3-stream with the third dumb buffer offset by 16 KB | whether "18x" is set aliasing; gates any GL buffer in CMA (G05, G06, G09) |
| P1 | in-cache PIE micro-benchmark on CPU0 (4 KB buffers: `vadd.u16.ld.incp`, `vmul.u16` + SAR, `vcmp` + select, `vmax.u16`, `vunzip.16`, `movi` lane moves) plus a PIE corruption hammer (vector vs scalar, quiet and under background SD reads), plus a re-run of `s31_pie_cases` on 7.1 | gates G08 and G09 |
| M1 | two-hart streaming: memset and memcpy of 2 MB, one hart vs both, libc-free workers, placement verified | gates G11; never measured (docs/smp-plan.md:206-209) |
| M5 | cross-hart user-space spin hand-off (pingpong with a flag instead of a futex), median and p99 | gates G11 and G12 |
| M3 | GDMA memset and memcpy with a raw RX_SUC_EOF poll vs the cookie wait | only if the GDMA is ever considered for a clear |

### G01. Remove the per-vertex soft doubles (rank 1)

- **Mechanism.** Build libGL with `-fsingle-precision-constant`. Change `sqrt`
  to `sqrtf` (zmath.c:248, light.c:220/276) and `fabs` to `fabsf`. Write our
  own float sin/cos/pow for glRotate (matrix.c:119-157) and spot lighting
  (light.c:250). Audit literals that need double range (glu, large glOrtho).
  Keep erysdren's specbuf. C-Chads' `SPECULAR_BUFFERS=0` calls `pow()` per
  vertex, which is harmful here.
- **Gain.** Teapot 28-39 → 16-23 ms, morph3d 20-28 → 11-15, gears 13-20 →
  10-16, mech 257-387 → 153-245, plus about 13 ms for mech's rotates
  (ESTIMATED from COUNTED sites × MEASURED libcall cost). The economics lens
  re-derived gears at 2.6-3.3 ms of double work and teapot at 11-14 ms. This
  also takes about 1,300 static instructions of libgcc df routines (COUNTED: muldf3 453, adddf3 644, truncdfsf2 180, extendsfdf2 56) out of the XIP I-cache path.
- **RAM.** None.
- **Risk.** Float sin/cos accuracy. Compare screenshots against the double
  build. The libm counts come from the toolchain libc.a, not the shipped one;
  re-count them.
- **Experiment.** Two library builds. gears and teapot at 320x240 through GLX.
- **Kill rule.** If teapot improves by less than 15%, or the result sits
  inside the base arm's spread, the model's double share is wrong. Profile
  (M4) before any hardware work.

### G03. Depth-range ping-pong: no depth clear (rank 2)

- **Mechanism.** Alternate frames between the two halves of the 16-bit range
  and flip the compare, so last frame's values always lose. Clear only on
  wrap, on a glClearDepth change, or when depth is read back. Implement as a
  per-frame bias/XOR folded into `zz` if it costs no instructions, otherwise
  as two filler variants (this doubles filler I-cache footprint, which
  matters with a 16 KB I-cache). Lines too (zline.c:4).
- **Gain.** 1.75-1.94 ms/frame at 320x240; 5.8-6.5 ms at 640x400 (ESTIMATED
  from MEASURED 79-88 MB/s). It also stops a 150-500 KB sweep through the
  shared D-cache. It beats every engine depth fill: G06 would still pay about
  1.1 ms of engine time plus an invalidate, and would need depth in CMA.
- **RAM.** None.
- **Risk.** 15-bit depth can z-fight in deep scenes. Needs real
  glDepthFunc/glDepthMask plumbing first (erysdren ignores both), so the
  fallback has somewhere to go.
- **Experiment.** One build, `TGL_ZTRICK=0/1`. gears at 320x240 and 640x400,
  with ZB_clear thread time.
- **Kill rule.** Drop it if the saving is under 1 ms at 320x240, or any
  z-fighting appears in gears, teapot or morph3d.

### G02. Dead specular, one-divide normalise, cached inverse-transpose (rank 3)

- **Mechanism.** (a) Skip the specular term when the material or light
  specular is zero. The default material specular is 0 (init.c:99) and LIGHT0
  specular is 1 (init.c:75), so gears computes and discards specular on 505
  of 1,076 vertices per frame. This is spec-exact, unlike C-Chads'
  non-standard `glSetEnableSpecular`. (b) C-Chads' `gl_V3_Norm_Fast` (one
  divide, three multiplies; cchads src/zmath.h:71-86), using `sqrtf`.
  (c) Cache the normal matrix on a matrix serial number and compute a 3x3
  inverse-transpose instead of glopBegin's general 4x4 Gauss-Jordan
  (vertex.c:84-100, zmath.c:138-193). mech does that 2,670 times per frame.
- **Gain (after the economics correction).** Once G01 has made sqrt cheap,
  the specular skip is worth about 0.4-0.9 ms/frame on teapot (2,293 vertices
  × 0.2-0.4 us), not 0.5-1.5 ms. The inverse cache is worth about 5-10 ms of
  a 150-250 ms mech frame (3-5%). All ESTIMATED.
- **RAM.** None. **Risk.** Low.
- **Experiment.** A/B against the G01 build: teapot and mech.
- **Kill rule.** Keep only if the median improves by more than the spread
  **and** the pixel diff on gears/teapot is zero.

### G14. Immediate-mode and array overhead (rank 4)

- **Mechanism.** Inline `gl_add_op` and use a static context (C-Chads commit
  d27050d; cchads src/zgl.h:337-338). Drop the per-vertex realloc check and
  the SELECT/FEEDBACK branches. Add a small post-transform vertex cache for
  glDrawElements (arrays.c:214-267 re-transforms each shared vertex about
  6 times). Fix bugs stock apps will hit: arrays.c:32 writes `.Z` instead of
  `.W`, and strides are applied in floats, not bytes (arrays.c:19/28/35/45).
  **Do not port C-Chads' inner loops.** On RV32 they are 20-190% more
  instructions per pixel, and the ztriangle text is 3.6x larger (19.5 KB vs
  5.4 KB), on a 16 KB I-cache fed from XIP (COUNTED).
- **Gain.** Dispatch inlining about 0.7-1.3 ms/frame on teapot (economics
  correction; about 14,400 calls × 10-20 insns). The vertex cache is worth a
  lot, but only for indexed-mesh apps. ESTIMATED.
- **RAM.** About 5 KB (32 × 156 B GLVertex). Check the XIP text growth from
  inlining against the image slack.
- **Kill rule.** Drop the inlining if teapot improves by less than 3%. Drop
  the vertex cache if an indexed test improves by less than 20%.

### G13. Native texture sizes and a cache-friendly texel layout (rank 5)

- **Mechanism.** Store textures at their native power-of-two size with a
  runtime mask and shift (like C-Chads' `TGL_FEATURE_TEXTURE_POW2`, but not
  compile-time). Optionally use 4x4-texel tiles, so a rotated quad does not
  take a new 64 B line per T step (the stride is 512 B today). PIE has no
  gather, so fewer misses is the only texture lever (275-316 ns per miss,
  MEASURED).
- **Gain.** RAM (COUNTED): 128 KB → w×h×2 per texture (8x8 = 128 B), and the
  192 KB upload transient disappears. texobj's two 8x8 textures cost 256 KB
  today. Speed: about +2 insns/px ≈ +0.16 ms on texobj, against a miss
  ceiling of 0.7-3.4 ms (ESTIMATED).
- **Risk.** Low. NPOT sources still need resampling (GL 1.x requires POT
  anyway). Tiling complicates glTexSubImage.
- **Kill rule.** Keep native sizes for the RAM saving unless they cost more
  than 5% fps. Keep tiling only if it gives at least 10% on a rotated-texture
  scene.

### G04. Render small, scale with PPA SRM on present (rank 6)

- **Mechanism.** xshim already picks the smallest mode that holds a
  fullscreen window (xshim.c:515-529), and the driver scales by the largest
  exact 1/16 factor on the PPA (esp32s31-lcd.c:1487-1515), async by default
  (`ppa_async=true`, esp32s31-lcd.c:1238). The gap is panel-size fullscreen
  (SDL2 FULLSCREEN_DESKTOP, the vm_native path at xshim.c:530-555). There,
  TinyGL would render at 800x480: 1.5 MB of colour plus depth, three times
  the plan's 500 kB kill rule, and more than the roughly 1.1-1.6 MB
  MemAvailable under a fullscreen game (xshim.c:1611). Instead, libGL renders
  at 400x240 (in vm_modes, xshim.c:500-503; an exact 2.0 factor), maps
  glViewport/glScissor/glReadPixels by the factor, and presents as a 400x240
  mode.
- **Gain.** 4x fewer pixels. The economics lens found the claim undercounted:
  the 800x480 CPU clear alone is about 17-19 ms per frame, against 4.4-4.9 ms
  at 400x240. Against that, the frame gains about 6-7 ms of async PPA engine
  time sharing PSRAM with the rasteriser. The scale is 0.6 ms of CPU
  (MEASURED). RAM: about 375 KB instead of 1.5 MB+.
- **Rules correction.** This is **a visible quality policy set by the
  platform, not an app speedup**. It is close to the "steering to a cheaper
  config" redline (memory s31-never-rebuild-client-apps.md). Do not report
  its fps as a like-for-like win. The env override (`TGL_RENDER_SCALE`)
  defaults to what the owner chooses. Keep 640x400 fullscreen out of the
  experiment: it killed the board on 2026-09-10, before the CLIC fix, and has
  not been re-run (docs/current-state.md:7905-7910).
- **Risk.** Softness. Exact, invertible viewport mapping for apps that read
  back or query GL_VIEWPORT. The latent async race (a CPU write into the mode
  buffer during the previous scale, docs/current-state.md:7884-7887) is
  closed by G06's fence.
- **Kill rule.** If fps gains less than 2x, the frame is not pixel-bound. Go
  back to G01/G02. Check the scanout geometry from the `scanout started` line.

### G05. Render straight into the presented GEM buffer (rank 7)

- **Mechanism.** The plan's "present is already zero-copy" (tinygl-plan:78-79)
  is wrong. MIT-SHM costs one CPU copy per frame in fullscreen (ShmPutImage
  copy + row hash, xshim.c:6138-6250) and two when windowed (plus the LVGL
  blit). At glXMakeCurrent, libGL asks xshim (a new XLITE-SHM op) to re-home
  the window as a 16-bit GEM dumb buffer (a bpp-2 analogue of
  `win8_gem_alloc`, xshim.c:1269, 1339-1346), exported through px_share's
  PRIME branch (xshim.c:9368-9380). TinyGL renders into that mapping. In
  fullscreen, lvdesk ADDFBs the same handle as the mode fb (the kms_fs_enter
  ioctls, kms.c:662-745, minus CREATE_DUMB). This is also what makes a PPA
  clear possible at all.
- **Gain.** MEASURED analogue: dropping a same-size 153.6 kB copy cut lvdesk
  user CPU per present from 8.52-9.15 to 4.90-6.09 ms, with a median present
  of 4.0 vs 7.6 ms (xshim.c:1596-1604). Note that this removed lvdesk's
  expand copy, not the ShmPutImage copy, and was measured with both
  processes on CPU0. It also saves the SHM segment (150 KB at 320x240).
- **Economics correction: wall-clock can get worse.** Today the client waits
  about 3 ms in XSync for the copy, and the async scale then overlaps its
  next frame. Rendering into the presented buffer means the client cannot
  write until the scale (about 5-6 ms of engine time, docs/current-state.md:7874)
  has read it. TinyGL's first write comes almost immediately after swap, so
  expect **about 2-3 ms more wall-clock per frame** unless a second colour
  buffer (+150 KB) is added.
- **Rules correction.** Restrict it to 320x240/320x200 until CmaFree under a
  fullscreen GL client is measured. 640x400 is memfd-only. Keep a memfd +
  MIT-SHM fallback for CMA allocation failure.
- **Prerequisites.** M2. A one-frame micro-check: TinyGL rendering into a
  dumb buffer vs the heap.
- **Experiment.** Arm A: MIT-SHM. Arm B: GEM target with `ppa_async=0` (a
  runtime module param) as a crude fence. gears fullscreen at 320x240.
- **Kill rule.** Abandon B if the dumb-buffer render is more than 10% slower
  than the heap render, if lvdesk CPU per present drops by less than 1.5 ms,
  **or if fps falls**.

### G06. Consumed-sequence fence + PPA clear chained after the scale (rank 8)

- **Mechanism.** The scanout buffer is already the front buffer, so one
  render buffer suffices if the client never writes it while its reader is
  active. glXSwapBuffers sends Damaged(seq N). lvdesk publishes `consumed=N`
  in a spare ring-header word (xring.h:79) once the reader is done. libGL
  waits at the next frame's first write, not at swap. The GLX back buffer is
  undefined after a swap, so a new ioctl in our driver can start a PPA fill
  of the render buffer right after the SRM's RX SUC_EOF. Kernel pieces: a
  fill ioctl on a GEM handle with an x/y block (RX descriptor pic = buffer
  geometry plus offsets, as IDF's ppa_fill.c:39-46 does; ours writes
  pic = block and W2 = 0 today, esp32s31-ppa.c:692-704); invalidate only
  (DMA_FROM_DEVICE, not the TO_DEVICE writeback the existing ioctls do,
  esp32s31-lcd.c:3531-3538); a single 50 ms timeout as in the CLUT path
  (esp32s31-ppa.c:1495-1497); fix the `> SZ_16K` bound that admits 16384
  into a 14-bit field (esp32s31-ppa.c:678).
- **Fill encoding (hardware-lens correction).** The S31 LL writes the raw
  word to `blend_tx_fix_pixel` with no colour-space conversion
  (esp_hal_ppa/esp32s31/include/hal/ppa_ll.h:1061-1096). Our driver's raw
  RGB565 write (esp32s31-ppa.c:757) is correct, and any 16-bit Z pattern is
  reachable. A board pattern check is still wise.
- **Gain (economics correction).** A 153.6 KB fill is about 1.1 ms of engine
  time (MEASURED 800x100 = 160,000 B in 1,137 us), not 0.8 ms. After the
  invalidate and the ioctl, the **net CPU saving is about 1.4-1.7 ms per
  plane**, just above the kill threshold. The fence wait grows by the fill
  time, so wall-clock per frame gets longer unless double-buffered.
- **RAM.** Zero with G05. Depth in CMA adds 150 KB of CMA residency. **Keep
  depth in the heap and take G03 instead**, unless CmaFree under load is
  measured with room to spare.
- **Risk.** The fence blocks the GL thread for about the SRM engine time. A
  sleep costs a 0.7-1.1 ms wake. The fill contends with mjpegrec/JPEG on
  `ppa->lock`. Scissored clears must use the x/y block.
- **Experiment.** Step 1, no build: the fence alone, via `ppa_async=0`,
  measuring the wait. Step 2: the fill ioctl behind a module param
  (`ppa_gl_clear=0/1`).
- **Kill rule.** Drop the fill if client clear time falls by less than
  1.5 ms/frame or the wait grows by more than the saving. Drop the fence (for
  two buffers) if the wait exceeds 25% of the frame.

### G15. Lighter fullscreen present: PRESENT for the mode fb (rank 9)

- **Mechanism.** Fullscreen presents use DIRTYFB (kms.c:757-777): a full
  atomic commit (2.2 ms per 320x200 frame on the direct path, kms.c:421) that
  can block up to a 23.7 ms frame on the previous flip (lvdesk.c:6888-6895).
  PRESENT skips the commit but refuses any fb other than `scan_gem`
  (esp32s31-lcd.c:3964-3975). Extend it, in our driver, to the fb on the
  CRTC: writeback + `esp32s31_ppa_scale_rect(_async)` + optionally G06's
  fence and clear. This helps every fullscreen game.
- **Gain (economics correction).** About 1-1.5 ms of lvdesk CPU per present,
  from dropping the commit. The "one range, not per row" writeback idea saves
  nothing for mode fbs, because their pitch equals their width, and PRESENT
  already uses one range at half a pitch or more (esp32s31-lcd.c:3978-3987).
  Removing the flip-wait tail is a latency gain, not a CPU saving.
- **Rules.** It needs a kernel build and flash; confirm with `uname` #N. It
  also needs regression checks of Doom, Quake and the desktop (memory
  regression-check-other-apps.md). Split into batches of 10 minutes or less,
  one app per batch, flipping `present_modefb` at runtime.
- **Kill rule.** Drop it if the present median falls by less than 1 ms, or
  any game regresses beyond the spread.

### G07. Damage from the rasteriser's bounding box (rank 10)

- **Mechanism.** libGL unions the screen bounding boxes of this frame's and
  the previous frame's primitives and sends that as the damage, replacing the
  hash that G05 removes. The driver's writeback and scale already take a
  damage rectangle (esp32s31-lcd.c:2035-2051, 2872-2882). Clips are honoured
  only when old fb == new fb (esp32s31-lcd.c:2049), which G05's
  single-buffer design satisfies. At an exact 2.0 factor any rectangle is
  exact (esp32s31-ppa.c:1255-1283).
- **Gain.** HOST-deterministic changed rows per frame: gears 240/240, teapot
  86/240, texobj 118/240, spin 66/240. The bounding box matched true rows
  within 3%. **Economics correction:** in fullscreen the CPU saving is only
  the writeback, about 0.15-0.2 ms, which is below its own 0.5 ms kill rule.
  The real payoffs are a shorter PPA engine time (about 3 ms shorter G05/G06
  fence wait on teapot) and, windowed, about 1-1.5 ms less LVGL blit on 3 of
  4 demos (ESTIMATED).
- **Kill rule (revised).** In fullscreen, judge it by fence wait and fps.
  Windowed, by lvdesk CPU per present (≥ 0.5 ms). Any stale-pixel frame (a
  separate mjpegrec run) kills it. Keep `TGL_DAMAGE=full`.

### G09. PIE for blend, modulate, fog, alpha test (conditional, rank 11)

- **Mechanism.** Stock apps need blending and GL_MODULATE for correctness,
  and erysdren has neither. Write them first as **scalar** specialised
  fillers (the C-Chads NOBLEND pattern, cchads clip.c:416-429). This is the
  heaviest per-pixel arithmetic in GL: a cached CPU blend measured about
  170-210 ns/px (20.6-25.0 ms for 400x300), well above the roughly 78 ns/px
  memory floor, so there is real compute to cut. The PIE version fetches
  texels scalar, inserts them with `movi.16.q`, then uses `vmul.u16`,
  `vadd.u16` and a pack.
- **Gain.** 1.5-2.5x per blended span (ESTIMATED). Unknowable per frame until
  an app is chosen.
- **Risk.** All of P1's unknowns. `vadd` saturation and `vmul` SAR semantics
  are unverified on S31. The destination read makes three streams, so
  blending into a CMA/GEM target depends on M2. Rounding must match the
  scalar oracle exactly.
- **Rules correction.** The scalar fillers are a correctness requirement.
  Defer PIE until a chosen stock app is shown to blend.
- **Kill rule.** Filler time improves less than 1.5x, or any pixel differs
  from the scalar oracle.

### G08. PIE Z-test and Gouraud spans for long spans (1 refutation; rank 12)

- **Mechanism.** Hand-written `.S` span functions. Per 8 pixels: aligned
  `esp.vld.128` of Z, z stepped in 32-bit lanes, `vunzip.16` for `zz`, and a
  select. Gouraud in three u16 channel vectors. Masked read-modify-write at
  span ends. Scalar Bellard code below about 16 px. Assemble with
  `xespv2p2`. Never `esp.lp.*`, never CFG.
- **Hardware corrections.** Z has 14 fraction bits (zbuffer.h:12), not 16:
  pre-shift z and dz left by 2 at span setup (0x3FFFC000<<2 fits u32), then
  take the high halves. Because ZCMP is `>=`, the new Z is simply
  `esp.vmax.u16(zz, zpix)`, with no select. The colour mask is NOT(`vcmp.lt.u16`).
  The I-cache is 16 KB.
- **Economics refutation.** At 320x240 the whole pixel-ALU line of gears is
  1.1-1.6 ms. With 2.5-4.8 px/line and a 16 px threshold, few pixels reach
  PIE, and the loop is memory-bound (about 78 ns/px PSRAM vs about 50 ns
  compute). Realistic: **0-0.3 ms at 320x240 (noise), about 1-2 ms (3-6%)
  on gears at 640x400**, borderline against its own 5% rule. The one
  long-span demo is textured, and G08 does not touch the perspective filler.
  One migration bounce (0.3-1 ms, ESTIMATED) can cancel a frame's gain.
- **Rules correction.** No GL thread may ever set a CPU1-only affinity.
- **Verdict.** Kept only as a conditional. Do it only if G04 is rejected and
  flat or Gouraud geometry at high resolution dominates the board profile.
  Kill rule 1 (P1): `vcmp`/`vmax` + `vst` on 8 u16 costs more than about
  12 cycles per block in cache, or any hammer mismatch appears. Kill rule 2:
  gears at 640x400 improves less than 5% or sits inside the spread.

### G11. Second-hart band split with deferred binning (rank 13)

- **Mechanism.** TinyGL is immediate-mode (clip.c:383-412), so per-triangle
  sync at 300-500 us is impossible. Bin each triangle (about 136 B) during
  the frame. At swap or a forced flush, both threads rasterise the identical
  ordered list clipped to their own band, which preserves Z order. Spin, then
  sleep, on a shared-D-cache flag (cross-hart AMO is sound, memory
  s31-amo-atomic-across-harts.md). The worker is scalar, unpinned, and stays
  out of musl's vectorised string functions. **The plan's "C-Chads' OpenMP
  layout is the model" is wrong:** C-Chads never parallelised rasterisation
  (cchads src/ztriangle.h:176-180).
- **Gain.** Amdahl with a 0.5-core helper: gears 1.25x at best, teapot 1.07x.
  After binning, a sleeping hand-off, the shared D-cache (2.1 M conflicts/s
  under Quake, memory s31-cache-counters.md) and TLB-shootdown IPIs, the
  economics lens puts it at 1.0-1.2x, or zero if two-hart PSRAM streaming
  does not scale. It adds CPU and takes hart0 time from the radios and
  lvdesk's present.
- **RAM (rules correction).** Cap the bin at 64 KB or less with flush-on-full.
  Uncapped, mech needs about 1.9 MB, more than MemAvailable. Each flush adds
  a sync.
- **Kill rules.** M1 must show two-hart store/copy ≥ 1.4x one hart. M5 p99
  must be under 100 us. The prototype must give ≥ 15% fps with no lvdesk
  regression, and A2DP/Wi-Fi checked during the arm.

### G12. GL command stream on a second thread (rank 14)

- **Mechanism.** Run all of TinyGL on a GL thread fed by a ring of `GLParam`
  ops (api.c:5-71 already packs every call). The app's own logic overlaps.
- **Gain.** Zero, and in fact net negative, on the demo set: teapot's
  roughly 14,000 calls per frame add about 1.3-2.5 ms of ring writes with
  nothing to overlap. It helps only an app with ≥ 25% non-GL CPU.
- **RAM.** A 64-128 KB ring plus a stack. Drain on pointer-argument calls
  (glTexImage, vertex arrays) instead of copying them.
- **Kill rule.** Don't build it unless a profiled stage-3 app shows ≥ 25%
  non-GL CPU. Then keep it only for ≥ 15% fps.

### G10. PPA blend for screen-aligned constant-alpha quads (rank 15)

- **Mechanism.** Detect a full-screen fade or HUD dim (ortho, axis-aligned,
  ≥ 128 KB, constant colour, no texture, no depth test) and run it as a PPA
  blend in place on the GEM buffer (out == bg is safe, esp32s31_drm.h:33-38).
- **Hardware correction.** Even a constant-colour fade needs a CMA foreground
  buffer (an A8 plane, or an RGB565 surface filled once). The A8/fixed-RGB
  fg mode (ppa_ll.h:1114-1125) needs new driver programming, not just offsets
  on the existing PPA_BLEND ioctl (esp32s31_drm.h:40-49).
- **Gain (economics correction).** At 320x240: about 10-12 ms of CPU saved
  per fade frame against a scalar blend, about 4-8 ms against a PIE blend.
  The 15-20 ms figure was for 400x300.
- **Kill rule.** Skip it entirely unless an app in the target set issues a
  qualifying quad at least once a second (count it with a libGL counter).

---

## 5. Rejected ideas (do not retry without new evidence)

No judged opportunity reached two refutations; G08's one refutation is
recorded above and it is kept only as a conditional. The following were
rejected in the research itself, with their reasons:

| idea | reason | source |
|---|---|---|
| PIE memset for glClear | measured 1.00x; fill is PSRAM-bound | docs/xespv-libc.md:17-20 |
| PIE flat (no-Z) span fill | a 16-bit memset; same bound | same |
| PIE texture fetch | no gather; each texel leaves and re-enters a q register; misses (275-316 ns) dominate | IDF xesppie.S corpus; perf-review-2026-09-23.md:460 |
| PIE for transform, lighting, clipping, setup | no float lanes; 16-bit fixed point lacks range | xesppie.S (zero float forms) |
| PIE/wide stores in present or copy paths | a 128-bit PIE store measured slower than paired 32-bit stores into scanout (4,550 and 6,149 us vs 3,062 us) | lvdesk.c:6722-6745 |
| esp-dsp `*_arp4.S` routines as-is | 26 hardware loops; no pixel primitives; its "f32" code is scalar | esp-dsp modules/*/ |
| esp_lvgl_port SIMD blenders | Xtensa-only (esp32, esp32s3) | esp-bsp esp_lvgl_port/src/lvgl9/simd/ |
| CFG unaligned-access mode | CFG is not in the PIE save area; cross-task hazard | esp32s31_coproc.S:72-79 |
| A worker thread pinned to CPU1 (plan step 5 as written) | its first PIE instruction, including inside musl memcmp/strcmp, pins it to CPU0 forever | esp32s31-ext.c:396-410 |
| Second hart for clears | two sleeping hand-offs (0.6-1.0 ms) against at most about 2 ms; a DMA clear uses no core | worklog-2026-09-19.md:1775-1790 |
| Synchronous PPA clear at 320x240 without the async design | roughly a wash on wall clock once the fixed cost and invalidate are paid; below the true-offload size | esp32s31-ppa.c:538-544; accel-plan.md:896-917 |
| GDMA memset as the first clear engine | byte values only, 1-D, a 300 KB memset exhausts the 12 KiB descriptor pool, never measured; revisit only after M3 | esp32s31-axi-gdma.c:480-487, 873-914 |
| GDMA copy of the ShmPutImage | SysV segment not contiguous; no scatter-gather memcpy | accel-plan.md:128-131 |
| PPA/2D-DMA/BitScrambler conversion for glTexImage or glDrawPixels | one-off at upload; the source is user memory and would need a copy into CMA first; SRM's 1/16 ratios cannot hit every size | ppa_ll.h:456-478; texture.c:140-152 |
| PPA rotate/mirror, colour key, CLUT for GL | no GL operation maps to them | ppa.c:1388, 1065-1072 |
| JPEG codec, ETM | no raster work; JPEG only matters as 2D-DMA contention | soc_caps.h:207, 632-634 |
| D-cache preload engine, cache lock | preload needs a syscall per call; locking I-cache lines measured worse (pingpong 432 → 718 us); a 150 KB depth buffer cannot be locked in 64 KB | perf-review-2026-09-23.md:275; esp32s31_cache.c:66-71 |
| Rendering straight into the scanout buffer | no back buffer (glClear visible every frame); gives a client write access to the desktop | frame-path-plan.md:108-110 |
| Adopting the client's MIT-SHM segment as the window | protocol-illegal; measured flicker in Doom | xshim.c:6055-6080 |
| XLITE-SHM memfd "zero-copy" without a fence | windowed, the late LVGL blit races the next clear; fullscreen, px_shared disables the alias and the copy returns | xshim.c:1647-1648, 6652-6778 |
| C-Chads rasteriser inner loops | 20-190% more insns/px on RV32; 3.6x the text | COUNTED, /tmp/claude-501/tgl/rv/loops.py |
| C-Chads `SPECULAR_BUFFERS=0` | per-vertex soft-double `pow()` | cchads src/light.c:369 |
| C-Chads `glSetEnableSpecular` | non-standard; stock apps silently lose specular; G02's spec-exact skip replaces it | cchads src/init.c:336 |
| Fast inverse square root (`TGL_FEATURE_FISR`) | `fsqrt.s` via `sqrtf` is the right answer here | COUNTED |
| `TGL_OPTIMIZATION_HINT_BRANCH_COST`, `ALIGNAS` "SIMD" math, OpenMP | tuned for out-of-order x86 and auto-vectorised float; nothing parallelised is on our hot path | cchads README, zfeatures.h |
| Windowed PPA_SCALE ioctl / windowed PPA blit (present report O3-windowed, O7) | parked, not rejected: 153.6 kB sits on the crossover, and the pointer over the window forces the CPU fallback; revisit only if a windowed profile shows the LVGL blit on top | esp32s31-lcd.c:1156; lvdesk.c:7649-7650 |
| Quoting `linux-dma-memcpy-results.json` 526 KB/s as AHB GDMA bandwidth | it is dmatest iops with CPU verification, not bandwidth | - |

---

## 6. How this changes docs/tinygl-plan-2026-09-25.md stage 4

### 6.1 Corrections to the plan's text

- **Levers item 3 ("clears ... as PPA fills, and the present is already
  zero-copy", :77-79).** The present is one CPU copy fullscreen and two
  windowed. A PPA fill needs GL buffers in GEM/CMA plus a new fill ioctl; the
  XLITE-SHM memfd is unreachable. The depth clear is better removed by G03.
- **Levers item 5 (:81-83).** C-Chads has no scanline parallelism, and a
  CPU1-pinned worker is pinned to CPU0 the first time it executes PIE,
  including inside musl.
- **Budget (:57-62).** The server-side copy target is missing: the real cost
  is 450 kB per 320x240 context, not 300. Every texture is 128 kB regardless
  of source size until G13. Panel-size fullscreen at native resolution is
  about 2.25 MB and cannot be supported without G04.
- **Stage 1.** Pass the SHM segment to `ZB_open` (no ZB_copyFrameBuffer).
  Advertise only the 16-bit visual. Round xsize to a multiple of 8, so the
  pitch is 16-byte aligned (this keeps PIE possible later). Add thread-CPU
  timers around clear, fillers and present from day one.

### 6.2 Proposed stage-4 lever list (ordered; each A/B'd with its own kill rule)

| step | lever | build needed | gate before it |
|---|---|---|---|
| 4.0 | board profile of gears/teapot/texobj at 320x240 and 640x400 (M4); replace section 3.4 | none | stage 1 |
| 4.1 | G01 float cleanup (+ s31fp linked for any residue) | libGL | 4.0 |
| 4.2 | G02 + G14 geometry cleanups and array fixes | libGL | 4.1 landed |
| 4.3 | G03 depth ping-pong (`TGL_ZTRICK`) | libGL | depth func/mask plumbing |
| 4.4 | G13 native texture sizes (`TGL_TEXNATIVE`) | libGL | none |
| 4.5 | G04 render scale for panel-size fullscreen (`TGL_RENDER_SCALE`), owner decides the default | libGL + xshim | stage 3 fullscreen works |
| 4.6 | G05 + G06 fence prototype with `ppa_async=0` | xshim/lvdesk | M2; dumb-buffer render check |
| 4.7 | one driver patch: G15 `present_modefb`, G06 fill ioctl `ppa_gl_clear`, fill bound fix; then G07 bbox damage | kernel + libGL | 4.6 shows fps not worse |
| 4.8 | PIE: P1 micro-benchmark + hammer + 7.1 case re-run; then G09 for a blending app, G08 only if 4.0/4.5 leave flat/Gouraud high-res dominant | asm in libGL | P1 passes; scalar oracle exists |
| 4.9 | second hart: M1 + M5; then G11 (64 KB bin cap), or G12 for an app with ≥ 25% non-GL CPU | libGL | M1 ≥ 1.4x, M5 p99 < 100 us |
| 4.10 | G10 PPA fade blend | kernel + libGL | an app issues qualifying quads |

Stage 4's pass rule stays as written ("recorded with numbers; keep what
pays"). Add one rule: **a present-path lever must not lower fps, whatever it
saves in CPU** (G05, G06).

### 6.3 Notes elsewhere in the repo that are now wrong (not edited here)

- docs/accel-plan.md:207-214 says PIE is disabled for userspace because the
  kernel does not save vendor vector state. The kernel does save it,
  unconditionally, and musl uses it (esp32s31-ext.c:47-174).
- Memory s31-soft-double-cost.md: "no usable SIMD in userspace". Superseded
  by the same.
- Memory s31-hardware-loop-corruption.md: "xespv2p2 is the same trap". Only
  HWLoop is known-unsafe; PIE has no hammer result yet either way.
- docs/xespv-libc.md:5 vs the kernel comment (esp32s31-ext.c:117-118): which
  musl routines are PIE on the shipped libc.so (is memcpy?) is unresolved.
  Disassemble the shipped library, because it decides what a CPU1 worker may
  call.
- docs/accel-plan.md:15, 113 calls the GDMA the "fastest engine". It is the
  slowest measured damage-copy engine (DIRTYFB 10.6 ms vs 7.9 CPU and 7.8
  PPA, esp32s31-lcd.c:2512-2541), probably because of its completion wait.
- The dma research report gave `PIE_BOUNCE=20`; it is 1000 (Makefile:444).
  The research reports' "32 KB I-cache" is 16 KB per hart.
