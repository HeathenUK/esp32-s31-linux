# GL phase 3a: levers, final state after review 3a (2026-09-26)

This file has three parts:

- **Part C (this section)** is the final state after the review-3a fix
  round. It has the final per-lever table, the review dispositions and the
  ranked list of what the board must A/B.
- **Part A** (geometry: G01, G02, G14) and **Part B** (pixels: the clears
  and the fillers) are the original lever reports. They are kept as they
  were written, with inline `[review 3a: ...]` corrections where a claim
  turned out wrong or incomplete.

Every number was measured on the host: qemu-system-riscv32 instruction
counts (gl/bench, with the board's Buildroot flags and the board's musl
libm), a new QEMU plugin that classifies each load and store by address,
the s31-glref rig (Xvfb + Mesa) and qemu-user. **Nothing ran on the
board.** Every "board ms" figure below comes from a model calibrated on
the one board profile we have. It is a projection, labelled as one, and not
a measurement.

Trees:

- **HEAD** is 7f822b6 (gl/bench/base3a). It is also the libGL the board
  profile was taken with.
- **base3p** is part A's final tree.
- **base3r** is the tree the review saw: part B's final, which is the
  review's `cur`.
- **final** is the working tree after this round.

## C1. Result: HEAD to final

The instruction column is qemu icount, in M instructions per frame, counted
over 10 frames after 3 warm-up frames. **Buffer kB** is the bytes stored
per frame to the colour and depth buffers only. On the board those are
PSRAM streams: stack and context stores are excluded, and are given
separately in the memory tables. **Lines** is the distinct 64-byte lines
of those buffers dirtied per frame, a write-back model (the line size is an
assumption). **Board ms** is the projected libGL CPU time per frame
(section C4). The frame hash is the same for every row except gears
640x400, where one pixel differs because of G01's rotation (part A).

| case | HEAD insn | final insn | change | buffer kB, HEAD -> final | lines kB | board ms (projection) |
|---|---|---|---|---|---|---|
| **glxgears-like 300x300** | 1.7046 | 0.9666 | **-43.3%** | 463 -> 249 (-46%) | 360 -> 218 | **10.7 -> 6.2 (-4.5)** |
| glxgears, two SHM buffers in turn | 1.7048 | 0.9669 | -43.3% | 463 -> 249 | 360 -> 218 | |
| gears 320x240 | 1.7656 | 1.0309 | -41.6% | 422 -> 269 (-36%) | 307 -> 232 | 10.6 -> 6.6 (-4.1) |
| gears 640x400 | 2.8700 | 1.8802 | -34.5% | 1438 -> 926 (-36%) | 1024 -> 762 | |
| teapotf 320x240 | 5.2775 | 2.5651 | -51.4% | 331 -> 75 (-77%) | 307 -> 68 | 23.0 -> 10.7 (-12.3; -7.6 is soft double at an assumed cost) |
| teapotf 640x400 | 6.0569 | 3.0516 | -49.6% | 1119 -> 301 (-73%) | 1024 -> 261 | |
| texobj 320x240 | 0.4165 | 0.3251 | -21.9% | 369 -> 202 (-45%) | 307 -> 141 | 3.9 -> 2.4 (-1.6) |
| texobj 640x400 | 1.2754 | 1.0080 | -21.0% | 1229 -> 676 (-45%) | 1024 -> 471 | |
| clear-heavy (geo1: clear, 4 small triangles) 320 | 0.1995 | 0.0939 | -52.9% | 326 -> 68 (-79%) | 307 -> 62 | |
| clear-heavy 640 | 0.6650 | 0.3343 | -49.7% | 1099 -> 270 (-75%) | 1024 -> 232 | |
| **full-coverage clear, far quad (geo9, new)** 320 | 1.1891 | 1.1437 | -3.8% | 614 -> 461 (-25%) | 307 -> 307 | |
| full-coverage far 640 | 3.8078 | 3.7020 | -2.8% | 2048 -> 1536 (-25%) | 1024 -> 1024 | |
| **full-coverage clear, near quad (geo10, new)** 320 | 1.1891 | 1.1870 | -0.2% | 614 -> 614 (0) | 307 -> 307 | |
| full-coverage near 640 | 3.8079 | 3.8460 | **+1.0%** | 2048 -> 2048 (0) | 1024 -> 1024 | |
| **approaching camera (geo11, new, 60 frames)** | 4.2420 | 2.2807 | -46.2% | 395 -> 295 (-25%) | 307 -> 218 | |
| **far-plane background (geo12, new, 60 frames)** | 5.3656 | 3.4002 | -36.6% | 405 -> 405 (0) | 307 -> 307 | |

What the new cases show:

- **Full-coverage frames.** geo9 and geo10 cover every row, so the dirty
  rows save nothing.
  - The far quad still gets the depth epochs: -25% bytes.
  - The near quad gets nothing. At 640x400 it is 1.0% *worse* in
    instructions. That comes from the F1 span loop, which does two pixels
    per turn where the old loop did four, and it costs on 640-pixel spans.
    Before this round nothing measured long spans.
- **Clear-heavy geo1.** Its -79% is mostly dirty-row sparsity: its four
  triangles touch about 40% of the rows (review m5). geo9 and geo10 give
  the full-coverage figures.

**The same comparison three other ways** (every number in fix/bench/):

- **60 counted frames** (review m1; `S31_BENCH_QDEFS=-DQN=60`): part B plus
  this round, measured on top of part A (base3p), gives:

  | case | change over 60 frames |
  |---|---|
  | glxgears | -13.8% (-14.0% over 10 frames) |
  | gears 320 | -12.3% |
  | gears 640 | -17.2% |
  | teapotf 640 | -11.6% |
  | texobj 320 | -15.7% (-18.7% over 10: texobj's scene grows) |

  Buffer bytes: glxgears 463 -> 259 kB (-44%), texobj 385 -> 225 kB
  (-42%). The 10-frame window misses the real depth clear that recurs
  about every 15 frames, which is why it reads slightly better.
- **Without the soft-double counting wrappers** (review m2;
  `S31_BENCH_NOWRAP=1`). The wrappers cost about 8 instructions a call
  inside the counted frames:

  | case | HEAD -> final, no wrappers | part A alone, no wrappers | part A alone, with wrappers |
  |---|---|---|---|
  | glxgears | 1.6685 -> 0.9665 (-42.1%) | -32.6% | -34.1% |
  | teapotf 320 | 5.1228 -> 2.5651 (-49.9%) | -47.7% | -49.2% |

  G01 alone on glxgears is about -19.1% once its wrapper instructions are
  subtracted, against -20.8% as recorded. This figure is derived: the G01-only
  tree was not rebuilt.
- **Frame-time tail** (the q_ui "frames" line; review M4):
  - glxgears frames span 0.9644 to 0.9678 M over 10 frames, and up to
    1.0086 M over 60, on the frame with a real depth clear. With epochs off
    the maximum is 0.9990 M.
  - geo12's dearest frame is **3.762 M against 3.409 M with epochs off
    (+10%)**. That is a materialisation, a whole-buffer read-modify-write.
    With this round's escalating backoff it happens 3 times in 60 frames
    (prof.py counts 3 zep_materialise calls).
  - geo11's dearest frame, 3.378 M, is the nearest pose. It is the same
    with epochs on or off.

## C2. Final per-lever table

This table merges parts A, B and C.

- **insn** is the change in qemu instructions when the lever was applied
  on top of the row before it (A and B as they reported; the details are in
  their sections).
- **buffer bytes** and **board ms** come from this round's separate arms,
  one per runtime or build toggle, on the final tree.
- A **-** means not separately measured.

| lever | what | insn (glxgears / gears320 / texobj320 / teapotf320) | buffer bytes/frame (glxgears) | board ms/frame, glxgears (projection) | decision |
|---|---|---|---|---|---|
| **Part A total** | G01, G02a/c/d/e/f, G14 (ctx, dispatch, strips, xform, vcache), fix-lights | 1.7046 -> 1.1240 (-34.1%), 1.7656 -> 1.1774, 0.4165 -> 0.3997, 5.2775 -> 2.6789 | 0 (463 -> 463) | **-2.2** (listed functions -0.7; soft double and libm -1.4 at an assumed 1.25 cycles per instruction) | keep |
| G01 | float sin/cos (degrees), powf, expf, sqrtf; bit-exact f2d/i2d | -20.8% (-19.1% without wrappers) / -20.3% / - / -28.6% | 0 | in part A | keep |
| G02a, c, d, e, f | lighting: zero specular skipped, cofactor normal matrix, per-state products, fmax/fmin clamps, inline normalise | -2.8%, -0.3%, -1.1%, -2.2%, -0.8% | 0 | in part A | keep |
| G02b | one reciprocal in normalise | +0.25% gears | 0 | - (fdiv.s latency is invisible here) | revert; board microbenchmark |
| G14 ctx1/ctx2, dispatch a/b/f/g/i, strips c, xform d, vcache e | layout, direct ops, copy-free strips, zero-skipping transform, vertex cache | see part A section 3 | 0 | in part A | keep (xform d: bit-exact since C-R3) |
| **L1** ZB_fill16 | 16 aligned words per loop turn | -3.0% / -2.4% / -7.2% / -1.1% | 0 (same stores) | **~0**: the clear is store-bound (11.5 cycles per store), and 1.50 -> 1.13 instructions per store buys nothing there (review m4) | keep (+36 B); expect nothing on the board |
| G03-h, G03-p | epochs with a depth bit lost | - | - | - | revert (suite precision) |
| **G03** depth epochs | integer base above zmax, exact | epochs off -> on: **-3.1% / -3.1% / 0 / +0.0%**; teapotf 640 -1.4%, gears 640 -6.7%, geo9 -3.6%, geo11 +0.1%, geo12 +0.6% | **395 -> 249 kB (-37%)**; gears 422 -> 269; teapotf 640 506 -> 301 | **-1.25** (gears -1.32, teapotf 640 -1.77, texobj 0) | keep; `S31GL_ZTRICK=0/1` A/B |
| F1, F2, F3, F4 fillers | span loops to an end pointer, two pixels a turn; row pointer; scalar sort | -7.9%, -0.4%, -0.6%, -0.4% (glxgears) | 0 (same pixels) | -0.23 to -0.40: 60k fewer filler instructions at 1.2 to 2.1 cycles each (the filler's 2.1 includes its memory stalls) | keep; **+1.0% on full-screen long spans (geo10 640)** |
| **DB** dirty rows | clear only the rows drawn since the last clear | rows off -> on: -0.6% / 0 / **-12.5%** / -0.9%; teapotf 640 -2.6% | 283 -> 249 kB (-12%); texobj 369 -> 202; teapotf 640 607 -> 301 | **-0.29** (texobj -1.49, teapotf 640 -2.72) | keep; `S31GL_DIRTYBOX=0/1` A/B |
| **DB-x** dirty box with x extent | per triangle, from the vertices | rows -> box: **+1.6% / +2.3% / -0.7% / +1.2%**; teapotf 640 +0.3%, prim7/8 +4.2%, geo3 +1.6% | **249 -> 212 kB (-15%)**; gears 269 -> 230; texobj 202 -> 159; teapotf 640 301 -> 178 | **-0.24** (gears -0.22, gears 640 -1.03, texobj -0.38, teapotf 320 -0.13, teapotf 640 -0.94, geo3 -0.43, **prim7 +0.38**) | **rebuilt behind `S31GL_DIRTYBOX=2`** (default stays rows); board A/B round 1 (review M2) |
| F5, F6 | paired 32-bit stores; textured remainder | +7.1%; +0.1% | 0 | - | revert |
| G03-g, G03-f | record keeping behind one flag; far check in the fillers | +0.2%; -0.4% | 0 | in G03 | keep |
| **C-R1** scissored depth clear | value fitted above the base (demote if not) | 0 (no bench case clears a box) | 0 | 0 | keep (correctness) |
| **C-R2** depth tail magic | an unknown tail makes the first clear real | 0 in counted frames; one real clear per bind | one clear per bind | 0 | keep (correctness) |
| **C-R3** transform FMA order | the zero-skipping projection written as explicit fmaf in the general branch's order | 0 (glxgears vertex op +2 instructions a frame) | 0 | 0 | keep (bit-exact again) |
| **C-R4** sliver bounds | a saturated-gradient triangle's true depth range, from its rows | +0.08% gears, +0.1% glxgears (register allocation in the flat filler); -0.4% teapotf 640, -0.1% geo3/6/7 (no more "zmax to the top") | teapotf 640 352 -> 301 kB, geo3 294 -> 256 | ~0 | keep (exact) |
| C-M4a | word-wise demote | per pixel ~6.5 instructions (unchanged), half the loads and stores, same bytes | - | - | keep |
| C-M4b | backoff 8/16/32/64; no zmax recording while the next clear is real anyway | geo12 +1.6% -> +0.6% against epochs off; its cheapest frame is now equal to epochs off; other cases 0 | 0 | 0 | keep |

**Part B plus this round, board-weighted** (base3p to final, projection),
in ms per frame:

| case | total | from the clear |
|---|---|---|
| glxgears | -2.3 | -1.9 |
| gears 320 | -1.9 | -1.4 |
| texobj 320 | -1.5 | -1.5 |
| teapotf 320 | -2.4 | -2.3 |
| teapotf 640 | -7.7 | -7.3 |

For glxgears that is about as much as part A's -2.2 ms. In instructions,
part A (-34%) looks more than twice as large as part B (-14%). The review's
point M1 stands: raw instruction counts rank the two parts in the wrong
order.

## C3. Review 3a findings: what was done

"Confirmed" means the finding was reproduced here before it was fixed.

| id | severity | status | change | verification |
|---|---|---|---|---|
| R1 scissored depth clear wraps | major | **confirmed, fixed** | glopClear's scissored/masked depth branch calls the new `zep_clear_rect_value()`. It demotes when the plain value exceeds ztop, then writes plain + base into the shared zmax (clear.c, s31_zepoch.c) | The review's rt_epoch went from 8 of 182 frames differing to 0, at 96x72 and at 321x239 with pitch +6. New zepoch_test `scissor` scenario: 10 frames differ on the pre-fix tree, 0 after. rt_fuzz: 0 of 2400 differ at both sizes, scissored clears included |
| R2 caller depth contents trusted | minor | **confirmed, fixed** | `ZDepthState.pad` became `magic`. zep_attach takes over any tail without it: zmax goes to the top, so the first full clear is real. s31gl.h now states the contract: any contents are fine; zero the tail when you reuse memory the core did not write at this size | rt_db `resize`: 4 of 122 frames differing -> 0 (96x72, and 97x71 with pitch +2). New zepoch_test `rebind` scenario: 3 frames differ before, 0 after |
| R3 lit transform not bit-identical | minor | **confirmed, fixed** (the review's second option) | vertex.c: PERSP and ORTHO pc = `fmaf(last kept term, first kept product)`, the association GCC gave the general 4-term sum | rt_geo, base3a against final, at 96x72, 320x240 and 641x401 with pitch +2: colormat_list, normals, projs, specspot and vcache now identical. Only rotate and twoside differ, which is G01's exact trig (90-degree and huge angles), as the review expected |
| R4 sliver exception visible | minor | **confirmed, fixed** (neither suggested option: better than both) | The sliver's true depth bounds are evaluated at both ends of every row's span, as the filler computes them (`zep_tri_sliver`). The bounds then feed the normal far and fit checks, and demotion happens only when a row wraps. Demoting at every sliver was measured and rejected: **teapotf +2.0% / +5.4%**, UV spheres +1.5-2.3%. Their pole triangles and teapot needles are slivers, and needles span many rows, so "a row or two" was wrong too | The review's 641x401 prims frame: 38 px differing -> 0. New zepoch_test `sliver` scenario: 2 frames differ before, 0 after. Pre-fix teapotf 640 also had a 3.215 M spike frame (a sliver setting zmax to the top); the dearest frame is now 3.073 M |
| R5 powf bound exceeded | minor | **confirmed, corrected** | Bound restated as about 1.2e-5 relative: 1.15e-5 = 174 ulp at x = 0.70, y = 127.5 is the worst of 2^28 random pairs. fmath_test adds 2^24 random pairs, half with y in [112, 128], limit 1.25e-5 | fmath_test PASS (1.08e-5 on its own sampling) |
| M1 no board weighting | major | **done** | gl/bench/boardw.py: per-function cycles per instruction from the board profile (C4). Board-weighted columns in C1 and C2, and the A/B order in C7 uses them | |
| M2 DB-x reverted on instructions alone | major | **done** | Rebuilt behind `S31GL_DIRTYBOX=2`, off the fillers' path: in that mode the inline row check is made to fail, so every triangle reaches `zdb_tri`, which adds the x extent from the vertices. Measured on every case in instructions, bytes (mem.sh) and board ms (C2). Projected net win on 8 of 9 cases and a loss on prim7 (2,400 small triangles) | zepoch_test arms `x` and `zx` identical to everything-off on host and RV32, 96x72 and 320x240. ASan/UBSan at 97x71 |
| M3 bytes mix stack and buffers | major | **done** | New QEMU plugin gl/bench/qemu/memclass.c with mem.sh and memtable.py. It classifies every access by address (colour buffers, depth, stack, other), counts dirty 64-byte lines, and gives the per-frame dearest. B1/B9 are restated here: glxgears HEAD -> final is 463 -> 249 kB of buffer stores. The "847 -> 563 kB written" figure included 149-341 kB of stack and 165-310 kB of context stores | Plugin totals equal prof.py's mnemonic counts (glxgears 166.6k stores, 563 kB) |
| M4 G03 tail unmeasured | major | **done** | geo11 (approaching camera) and geo12 (far-plane background), 60 frames, with per-frame min/max instructions and per-frame bytes. Demote made word-wise. From what geo12 showed: the backoff escalates (8, 16, 32, 64) and zmax is not recorded while the next clear is real anyway | C1 tail line; geo12 +1.6% -> +0.6% against epochs off. The board A/B must record the tail (C7) |
| m1 window misses the real clear | minor | **done** | 60-frame arms for base3p, final, DB-x and epochs off (C1) | |
| m2 dwrap inflates "before" | minor | **done** | `S31_BENCH_NOWRAP=1` (dwrap.c `DWRAP_NONE`) | C1 |
| m3 before-tree not reproducible | minor | **confirmed, fixed** | build_q.sh compiles each tree with its own build-lib.sh when that script supports S31GL_OBJONLY | base3a teapotf 320 is 5.2775 again (5.2103 through the current build-lib.sh) |
| m4 L1 invisible on the board | minor | **accepted** | Marked "expect ~0" (C2) and left out of the fps expectations | |
| m5 no full-coverage clear case | minor | **done** | geo9 (far) and geo10 (near) at 320 and 640 | This exposed F1's +1.0% on long spans |
| m6 size and footprint reporting | minor | **done** | One combined size row, GLX drift separated, hot 32-B lines, the textured filler's growth (C5). Gating zep_* per-frame work for texobj was not done: 14 hot lines, below the model's resolution | |
| m7 board A/B order | minor | **done**, adjusted with the measured projections | C7 | |

**Found while fixing, in the tests:**

- zepoch_test read glReadPixels(GL_UNSIGNED_SHORT) at an odd width into a
  W*H*2 buffer while the pack alignment was 4, which is a heap overflow.
- Its glBitmap of an 8x8 bitmap read 32 bytes of an 8-byte array while
  the unpack alignment was 4. **That out-of-bounds read was the unexplained
  host/RV32 difference in part B's "pixels" scenario**: the two builds
  drew different memory. Both are fixed, and host = RV32 now for every
  arm.
- run-san.sh now also runs zepoch_test with `S31GL_DIRTYBOX=2` at 97x71.

## C4. How it was measured (additions)

- **Address-classified memory** (review M3).
  - The tools:
    - gl/bench/qemu/memclass.c (a QEMU 11 TCG plugin, built by mem.sh
      against Homebrew's qemu-plugin.h);
    - gl/bench/mem.sh OUT IMAGE...;
    - gl/bench/memtable.py.
  - q_ui.c publishes the colour and depth buffer addresses in `q_bufs`
    and calls `q_fmark()` after each counted frame.
  - Output per class: loads, stores, bytes, distinct 64-byte lines. The
    classes are: colour buffers, depth (with its tail), stack
    (>= 0x8ff00000), and other.
  - Output per frame: the same, with a total and the dearest frame.
  - The plugin's totals match prof.py's mnemonic counts exactly.
  - "Lines" is a write-back model: the S31's cache line size and policy
    are not measured here.
- **q_ui.c now binds its own calloc'd depth buffer** (s31gl_bind_depth),
  as GLX gives every drawable one; `-DQ_NODEPTH` restores the old
  behaviour. It also prints a "frames" line with the cheapest and dearest
  frame. **base3p reproduces its recorded numbers to the last digit with
  this q_ui**: all 45 lines and every hash (out-fx-base3p against
  out-p0b).
- **build_q.sh** uses each tree's own build-lib.sh (review m3).
  - `S31_BENCH_NOWRAP=1` drops the soft-double wrappers.
  - `S31_BENCH_QDEFS` passes -D flags to q_ui.c, for example `-DQN=60`.
  - `S31_BENCH_DEFS=-DS31GL_DIRTYBOX_DEFAULT=2` selects the DB-x arm, and
    `=0` turns the dirty rows off.
- **geo.c / geo.sh** gain four cases: geo9 and geo10 (full coverage, far
  and near, at 320 and 640), and geo11 and geo12 (approach and far-plane
  background, 60 frames each).
- **boardw.py**, the board-weighted projection (review M1).
  - **Calibration.** It uses the task's board profile: stock glxgears
    300x300 at 31 fps, on the shipped libGL, which is HEAD. It is matched
    against prof.py of g_glxgears_300 built from HEAD.
  - **Cycles per instruction** = share x 32.3 ms x 320 MHz / instructions:
    - flat filler 2.11;
    - smooth filler 1.41;
    - vertex op 1.24;
    - shade 1.32;
    - ztri_setup 1.37;
    - viewport 1.19;
    - glopCallList 5.61;
    - gl_V3_Norm 0.81 (low sample count).
  - **The clear** is modelled per store: 10.0% of samples for 90.0k
    stores, which is **11.5 cycles per 4-byte store**.
  - **Everything the board profile does not list** takes 1.25 cycles per
    instruction ("unlisted"). That includes musl sqrt and the libgcc soft
    double. The board profile shows no samples for them, so part A's -1.4
    ms from them is the least certain number in this report.
  - **Assumptions:**
    - the same cycles per instruction hold for other images and
      resolutions;
    - only libGL CPU time is modelled, not the X server's copy, the
      present or anything else.
- **Snapshots** (gitignored; outside gl/, s31-libgl.mk's rsync should
  exclude them):
  - gl/bench/base3r is the tree the review saw, plus tinygl/examples.
  - gl/bench/base3pa, base3ra and finala are base3p, base3r and final with
    HEAD's gl/glx, for the size accounting in C5.

## C5. Code size and hot footprint (review m6)

Buildroot mode (brsize.sh, the musl toolchain, board flags):

| tree | .text | .rodata | stripped libGL.so.1 |
|---|---|---|---|
| HEAD (base3a) | 149,032 | 21,308 | 230,916 |
| part A final (base3p) | 147,260 | 21,544 | 226,820 |
| part B final (base3r) | 151,756 | 21,600 | 235,024 |
| **final** | **152,506** | 21,576 | **235,020** (= /src/images/libGL.so.1, md5 fe38512e...) |
| the same four with HEAD's gl/glx: base3a / base3pa / base3ra / finala | 149,032 / 146,674 / 150,644 / 151,686 | | 230,916 / 226,804 / 230,908 / 230,908 |

- **The combined phase 3a cost in the core** (HEAD's gl/glx in both
  trees) is **+2,654 B of .text**, and the stripped file is unchanged
  (-8 B).
  - Part A: -2,358.
  - Part B: +3,970.
  - This round: +1,042. That is zep_tri_sliver 206 B, zdb_tri 82 B,
    zep_clear_rect_value 126 B, the word-wise demote +138 B,
    zep_attach +46 B, and a few bytes in each filler.
- **Everything else in the full-tree rows is the concurrent GLX owner's**
  work in gl/glx (G04 render scale and others). It is about 1.1 kB of
  .text (152,506 - 151,686 = 820 B now, 586 B at base3p).
- **The unexplained +588 B** (review m6) is that drift: base3p with
  HEAD's gl/glx builds 146,674 B, against part A's recorded 146,672 B.
- **Filler sizes** (linked bench image), HEAD -> final:

  | filler | HEAD | final |
  |---|---|---|
  | flat | 498 | 498 |
  | smooth | 1,230 | 1,132 |
  | **textured (perspective)** | **1,350** | **1,500** |

  The textured filler grew in part B (+130) and in this round (+20).

**Hot code** (foot.sh: blocks run in every counted frame), in distinct
32-byte lines:

| image | HEAD: library + runtime | part B final | **final** |
|---|---|---|---|
| glxgears | 343 + 203 = **546** (17.5 kB) | 374 + 47 = 421 | 381 + 47 = **428** (13.7 kB) |
| gears 320 | 382 + 203 = 585 | 404 + 46 = 450 | 408 + 46 = 454 |
| texobj 320 | 292 + 178 = 470 | 354 + 2 = 356 | 368 + 1 = 369 |

- Library lines grew by 11% (glxgears) and 26% (texobj).
- Over the same span the soft-double and libm runtime lines fell from 203
  to 47, so the total per-frame footprint fell by 22%. For scale, the hart's
  I-cache is 16 KB and is shared with the kernel.
- texobj pays about 14 lines for the per-frame epoch and dirty-row
  bookkeeping, and never gets an epoch. Gating that work was not done.

## C6. Correctness (final tree; artifacts/gl/phase3a/fix-final)

| check | result |
|---|---|
| tools/glref suite, 22 apps | **0 worse than SUITE.md 10.2**. **All 22 identical to part B's final** (pix-final) to the last digit. glxgears and gears 0.000%. Verdicts as in 10.2 |
| raster pages 1-6 / pixel and GLU pages / prims | identical to f3f6 / identical to f7, logs 0 differ / PASS 0.003% |
| glx_geo (run-geo.sh) | 2.331% tolerant; our frame is byte-identical to part B's geo-pix |
| run-sysgears.sh (Debian glxgears) | PASS 0.000 / 0.000 / 0.000% |
| core_test / raster_gate | 185/0 and 51/0 on the host; 183/0 and 51/0 on RV32 qemu-user |
| fmath_test (now with 2^24 random powf pairs) / d2f / f2d | PASS / 0 mismatches / 0 of 2^32 |
| headless_gears 320x240x100 md5 | 38859f0c..., host and RV32, unchanged |
| **zepoch_test**: 304 frames, with the new scissor, sliver and rebind scenarios; arms epochs, rows, both, x boxes, epochs + x boxes | **every arm identical to everything-off**, host and RV32, 96x72 and 320x240, and on the host also 97x71. **host = RV32** for the reference and the default arms. The new scenarios fail on the pre-fix tree (scissor 10, rebind 3, sliver 2 frames) |
| the review's own harness (rt_epoch, rt_db, rt_geo on/off, rt_fuzz 2400 frames x 2 sizes) | 0 frames differ, at every size the review used |
| ASan + UBSan (run-san.sh, now with the x-box arm at 97x71; and an instrumented library with zepoch_test in 6 arms x 3 sizes) | clean |
| builds (host, RV32 via gl/build.sh, Buildroot mode, bench) | 0 warnings |
| limits.txt guard (newlib) | 44 within limits, 0 over |

## C7. What the board must A/B, ranked

The ranking follows the projected libGL ms per glxgears frame (32.3 ms at
31 fps), and every figure in this list is unmeasured. The protocol for every
arm:

- fresh boot;
- glxgears 300x300 windowed, with GLX's two SHM buffers;
- 5 or more runs, the first discarded;
- report fps median and spread **and the worst frame**;
- if possible, a PC-sample profile as well, so that boardw.py can be
  recalibrated.

The list:

1. **`S31GL_ZTRICK=0/1` and `S31GL_DIRTYBOX=0/1`.** These are runtime
   toggles on the final libGL, so no rebuild is needed.
   - Projected: epochs -1.25 ms, dirty rows -0.29 ms. That is 1.8 of the
     2.0 ms the clear projects to save.
   - Add a scene that moves in depth, for the demotion and
     materialisation tails (geo11 and geo12 patterns).
   - geo12-like content (a far-plane skybox under GL_LESS) is the known
     loss: projected +0.6% on average, and +10% on each of 3 frames in 60.
2. **`S31GL_DIRTYBOX=2`** (DB-x), also a runtime toggle.
   - Projected -0.24 ms on glxgears.
   - Also run a many-small-triangles app, where it is projected
     **+0.38 ms** (prim7).
   - Keep it only if both hold.
3. **Part A as a whole** (needs a libGL built from gl/bench/base3p).
   - Projected -2.2 ms, of which -1.4 ms is soft double and musl sqrt at
     an assumed cost.
   - **First check whether the board profile has libc/libgcc samples at
     all.** The 2026-09-26 evidence lists none, which caps what can be
     claimed.
4. **F1/F2 span loops.** This needs two builds.
   - Projected -0.23 to -0.40 ms on glxgears.
   - Include a full-screen long-span app: geo10 640 shows +1.0% in
     instructions.
5. **G02b** (fdiv.s latency). A 3 fdiv.s versus fdiv.s + 3 fmul.s
   microbenchmark is cheap. The board profile puts gl_V3_Norm at 0.3%,
   so any gain is small.

Not worth board time:

- L1, whose stores are the same;
- F5 and F6, which are reverted.

## C8. Open issues

1. **No board measurement** (the rules forbid it). Every ms figure is
   boardw.py's projection from one profile. It assumes the same cycles
   per instruction everywhere and puts a guessed cost on everything the
   board profile does not list.
2. **DB-x's default** waits for the board (C7 item 2).
3. **F1 costs +1.0% on full-screen long spans** (geo10 640). A
   four-pixel turn for long spans is untried.
4. **C-R4 costs the flat filler about 0.5 instruction a triangle**: gears
   +0.08%, glxgears +0.1%. GCC keeps &T in a callee-saved register once
   T is passed to the sliver call. Three alternatives were measured no
   better; they are recorded in ztri.h.
5. **G03 on far-plane backgrounds under GL_LESS** (geo12): +0.6% on
   average, and a +10% frame each time it materialises. That is 3 times
   in 60 frames with the escalating backoff.
6. **Demotion and materialisation** cost about 6.5 instructions per pixel
   (13 per word; the word-wise loop halved the loads and stores, not the
   instructions) and read and write the whole buffer.
7. **The bind contract** (s31gl.h): a caller that reuses depth memory
   across a size change, then comes back to the old size without zeroing
   the tail, can present a valid-looking stale tail. That use is
   documented as the caller's duty. GLX callocs a fresh buffer and never
   does it.
8. **texobj** never gets an epoch but pays about 14 hot lines of
   per-frame bookkeeping.
9. **Outside gl/**:
   - s31-libgl.mk's rsync exclusions should skip gl/bench/base3r,
     base3pa, base3ra, finala, out-*.log and qemu/*.dylib, as well as
     base3a/base3p.
   - run-prims.sh and run-sysgears.sh write their images into
     artifacts/gl/phase1. That is their existing behaviour.
10. **/src/images/libGL.so.1 is now the final tree** (gl/build.sh): 235,020
    B, md5 fe38512ee2a3bf6e8322dbdb234f412c. It includes the concurrent
    GLX owner's current gl/glx.

## C9. Commands (this round)

```
S31_BENCH_LIBM=musl BENCH_NO_LIMITS=1 gl/bench/bench.sh $PWD/gl gl/bench/out-fx-final        # final
S31_BENCH_DEFS=-DS31GL_ZTRICK_DEFAULT=0 ... | -DS31GL_DIRTYBOX_DEFAULT=0 | =2           # epochs off / rows off / DB-x
S31_BENCH_QDEFS=-DQN=60 ...   S31_BENCH_NOWRAP=1 ...                                   # 60 frames / no wrappers
bash -c 'cd gl/bench && ./mem.sh out-fx-final g_glxgears_300 q_gears_320 ...'           # buffer bytes by address
python3 gl/bench/memtable.py gl/bench/out-fx-base3a gl/bench/out-fx-final ...
gl/bench/prof.sh gl/bench/out-fx-final g_glxgears_300
python3 gl/bench/boardw.py gl/bench/out-fx-base3a/prof_g_glxgears_300.txt A/prof_X.txt B/prof_X.txt   # projection
./docker/build.sh 'BRSIZE_TREES="base3a:/src/gl/bench/base3a final:/src/gl ..." sh /src/gl/bench/brsize.sh'
gl/tests/run-3a.sh fix-final; gl/tests/run-zepoch.sh artifacts/gl/phase3a/fix-final/zepoch; gl/tests/run-san.sh
gl/build.sh                                                                               # /src/images/libGL.so.1
```

The tables are in artifacts/gl/phase3a/fix/bench/: insn-table*.md,
mem-table*.md and the per-arm out-*/ directories, which hold results,
frames, mem_*, prof_* and foot. The diff of this round against the tree the
review saw is artifacts/gl/phase3a/phase3a-fix.diff.

---


# Part A: geometry-side levers G01, G02, G14 (2026-09-26)

Everything here was measured on the host: qemu-system-riscv32 instruction
counts (gl/bench), the s31-glref rig (Xvfb + Mesa llvmpipe) and qemu-user
for the RV32 tests. **Nothing ran on the board.** There are no board fps,
PSRAM, I-cache, FPU-latency or XIP timings in this report; where a number
is a model or an estimate it says so.

The source changes are in gl/ (not committed). The before tree is git HEAD
7f822b6, snapshotted as gl/bench/base3a (gitignored).

## 1. Result

M instructions per frame, qemu icount, the board's Buildroot flags, **the
board's own musl libm** (section 2). "before" = HEAD 7f822b6, "after" =
every lever kept.

| case | before | after | change | frame hash |
|---|---|---|---|---|
| gears 320x240 | 1.7656 | 1.1774 | **-33.3%** | unchanged (6bf5e81c) |
| gears 640x400 | 2.8700 | 2.2813 | -20.5% | 1 px of 256,000 differs (G01 rotation, one edge pixel, one 5-bit step) |
| **glxgears-like 300x300** | 1.7046 | 1.1240 | **-34.1%** | unchanged (5f1ff8c8) |
| teapotf 320x240 | 5.2775 | 2.6789 | **-49.2%** | unchanged |
| teapotf 640x400 | 6.0569 | 3.4582 | -42.9% | unchanged |
| texobj 320x240 | 0.4165 | 0.3997 | -4.0% | unchanged |
| texobj 640x400 | 1.2753 | 1.2585 | -1.3% | unchanged |
| clear-heavy 320x240 (geo1) | 0.1995 | 0.1984 | -0.6% | unchanged |
| clear-heavy 640x400 (geo1) | 0.6650 | 0.6639 | -0.2% | unchanged |
| indexed mesh, glDrawElements (geo3) | 4.4570 | 2.5881 | -41.9% | unchanged |
| 512 glRotatef (geo4) | 5.2867 | 0.6354 | -88.0% | unchanged |
| mech-like: 200 x (rotate, 1 lit triangle) (geo5) | 3.2738 | 0.7824 | -76.1% | unchanged |
| specular + spot, 2 lights (geo6) | 13.4167 | 3.8182 | -71.5% | unchanged |
| colour material per vertex (geo7) | 5.1437 | 3.8236 | -25.7% | unchanged |
| strip/loop/fan/quad-strip probe (geo8) | 0.5520 | 0.5341 | -3.2% | unchanged |

- **Soft-double calls per frame** (every `__*df*` libcall, including the
  ones nested inside libm): gears 4577 -> 8, glxgears 4529 -> 8, teapotf
  19344 -> 0, geo6 59610 -> 0. The 8 left in gears/glxgears are the
  demo's own `-2.0 * angle - 9.0` (app code, stock glxgears does the same).
- **Where the glxgears frame went** (prof.sh, per function, instructions
  per frame): vertex 325k -> 220k, lighting 164k -> 91k, musl `sqrt` +
  soft double 290k -> 0, gl_V3_Norm 38k -> 0 (inlined), specular table
  lookups 13.5k -> 0, viewport 61k -> 56k, flat provoking colour 20k ->
  12k. The rasteriser and the clear are untouched: ZB_fillTriangleFlat
  411,353 -> 411,345, memset_16 135,043 -> 135,043. The geometry part of
  the frame is now about 0.40 M of 1.12 M (it was about 0.95 M of 1.70 M).
- **Memory operations** (prof.sh counts every load and store qemu
  executes, from the decompressed mnemonics): glxgears 326,843 loads /
  304,488 stores (1.11 MB written) -> 225,342 / 237,545 (0.85 MB written);
  teapotf 1.21 M / 683 k -> 771 k / 413 k. The clear-heavy case is
  unchanged: memset_16 issues 76,806 stores a frame at 320x240 (307 kB,
  4-byte stores: RV32 without D has no wider scalar store) and 256,023 at
  640x400, before and after. No lever here is a clear lever.
  [review 3a M3: these counts include stack spills and context stores,
  which stay in the D-cache. By address, glxgears' colour + depth stores
  are 463 kB per frame before AND after part A (C1, C2); what part A
  removed was stack and context traffic.]
- **Code size.** Library .text in Buildroot mode (the musl toolchain, the
  board TARGET_CFLAGS, gl/bench/brsize.sh): **149,032 -> 146,672 B
  (-2,360)**; the stripped libGL.so.1 230,916 -> 226,804 B (-4,112). The
  executed-code footprint per frame (foot.sh, blocks run every counted
  frame) for glxgears: library 8,556 -> 7,924 B plus runtime (libm,
  libgcc) 4,702 -> 1,024 B, so **13,258 -> 8,948 B (-4.3 kB) of hot code**
  for the 16 KB I-cache fed from XIP; gears 14,260 -> 9,920 B, teapotf
  10,466 -> 6,368 B.
- **Correctness:** the full glref suite has **0 apps worse** than
  phase2/SUITE.md 10.2 (19 identical to the last digit, 3 better: morph3d
  0.016-0.034% -> 0.000%, geartrain and rrootage strict counts lower);
  glxgears and gears stay 0.000%. raster pages, pixel/GLU pages, prims,
  core_test (185/0 host, 183/0 RV32), raster_gate (51/0 both), d2f_test,
  headless_gears md5 and ASan+UBSan are identical or clean (section 6).

## 2. How it was measured

- **gl/bench** (qemu-system-riscv32 `-icount shift=0`, bare metal, exact
  instruction counts, deterministic: no noise band applies). The library
  objects are compiled by gl/api/build-lib.sh with the board's Buildroot
  flags, as the library is.
- **New: `S31_BENCH_LIBM=musl`** (build_q.sh; the other scripts follow).
  It links the math objects of the board toolchain's musl libc.a (sin,
  cos, sqrt, pow, powf, expf and helpers, compiled rv32imafc/ilp32) ahead
  of newlib's soft-float libm. This makes a libm call cost what it costs on
  the board, and makes musl's float functions that compute in double
  visible to the soft-double counter. It changes the before numbers a lot:
  gears reads 1.7656 M with musl against 2.6152 M with newlib, because
  newlib's rv32imac `sqrt` costs about 790 instructions a call and musl's
  about 115. **All lever tables use musl.** The plan's +1% guard
  (limits.txt, newlib) was re-run on the final tree: 43 lines within
  limits, 0 over (gears 2.6152 -> 1.1774 there).
- **New cases** (gl/bench/geo.sh, geo.c, glxgears_q.c): glxgears-like is
  mesa-demos 9.0.0 glxgears' gear(), draw(), reshape() and init() verbatim
  (Brian Paul's MIT licence) at 300x300, 2 degrees a frame. geo1 is the
  clear-heavy case (glClear colour + depth, then 4 flat triangles). geo3-8
  are listed in the table above and in geo.c's header.
- **prof.sh / prof.py** (new): a flat per-function instruction profile of
  the counted frames only (between two q_mark() calls in q_ui.c), from
  qemu's `-d in_asm,exec,nochain` log, plus loads, stores and bytes per
  frame. `PROF_FN=name` lists that function's blocks.
- **Soft-double census** of the objects (dcensus.sh, objdump -dr): 67 call
  sites before, 14 after (section 3).
- **What the instrument does not see:** PSRAM stalls, the I-cache, XIP
  fetch, FPU latency (an fdiv.s counts as one instruction), stack traffic
  being cheaper than PSRAM traffic. Board fps after these levers is **not
  measured**.

## 3. Per lever

Each row is measured on top of the previous one (musl libm; M insn/frame,
change against the row above). Full table with every case:
bench/levers-table.md; raw outputs: bench/out-*/.

| # | lever | gears 320 | glxgears 300 | teapotf 320 | other case that moved | dcalls g/gx/t | lib obj .text | frames | decision |
|---|---|---|---|---|---|---|---|---|---|
| 0 | before (HEAD) | 1.7656 | 1.7046 | 5.2775 | | 4577/4529/19344 | 166,303 | | |
| G01 | float sin/cos (degrees), pow, exp; sqrtf; bit-exact f2d/i2d | 1.4079 (-20.3%) | 1.3504 (-20.8%) | 3.7683 (-28.6%) | rotates -87.8%, mech -65.2%, spot -64.2% | 8/8/0 | +956 | gears 640: 1 px | **keep** |
| G02a | skip zero specular; cached specular table | 1.3693 (-2.7%) | 1.3123 (-2.8%) | 3.5847 (-4.9%) | colormat -3.1% | 8/8/0 | +478 | identical | **keep** |
| G02c | normal matrix: 3x3 cofactors, recomputed only on a modelview change | 1.3660 (-0.2%) | 1.3090 (-0.3%) | 3.5836 (-0.0%) | mech -20.5% | 8/8/0 | +512 | identical | **keep** |
| G14-ctx1 | GLContext: the 16 lights moved to the end | 1.3328 (-2.4%) | 1.2773 (-2.4%) | 3.4277 (-4.4%) | all lit -1.5..-3.7% | 8/8/0 | -2,832 | identical | **keep** |
| G02d | emission+ambient and light ambient per state change; direct colour material | 1.3189 (-1.0%) | 1.2636 (-1.1%) | 3.3653 (-1.8%) | spot -2.1%, colormat -1.5% | 8/8/0 | +450 | identical | **keep** |
| G14a | immediate-mode ops run directly (no gl_add_op, no op table) | 1.3189 (0) | 1.2636 (0) | 3.1925 (-5.1%) | colormat -5.4%, spot -3.8% | 8/8/0 | +526 | identical | **keep** |
| G14b | gl_attrib.c (glVertex*, glNormal* ... wrappers) without frame pointer | 1.3189 (0) | 1.2636 (0) | 3.1253 (-2.1%) | colormat -2.2% | 8/8/0 | -2,446 | identical | **keep** |
| G14c | strips and fan without vertex copies | 1.2396 (-6.0%) | 1.1855 (-6.2%) | 3.1061 (-0.6%) | | 8/8/0 | +52 | identical (geo8 probe too) | **keep** |
| G14d | glFrustum/glOrtho zeros and affine w row skipped; w != 1 fixed | 1.2246 (-1.2%) | 1.1706 (-1.3%) | 3.0389 (-2.2%) | rotates +0.4% (1.6 insn per matrix op) | 8/8/0 | +730 | identical | **keep** |
| fix | GL_LIGHT1..7 default colours (GL 1.3 table 6.9) | 0 | 0 | 0 | | 8/8/0 | +12 | identical | **keep** (correctness) |
| G14e | glDrawElements post-transform vertex cache, 64 entries | 1.2235 (-0.1%) | 1.1696 (-0.1%) | 3.0341 (-0.2%) | **indexed -34.3%** | 8/8/0 | +2,852 | identical | **keep** |
| G14-ctx2 | GLContext: cold arrays to the end, hot fields below 2 kB | 1.2181 (-0.4%) | 1.1642 (-0.5%) | 3.0101 (-0.8%) | colormat -0.9% | 8/8/0 | -508 | identical | **keep** |
| G14f | vertex op takes its floats; glNormal/glTexCoord store directly; list replay calls the vertex op | 1.2173 (-0.1%) | 1.1634 (-0.1%) | 2.8757 (-4.5%) | spot -2.9%, colormat -2.8% | 8/8/0 | +1,266 | identical | **keep** |
| G14g | glColor stores directly | 0 | 0 | 0 | colormat -2.1% | 8/8/0 | +42 | identical | **keep** |
| G02e | lighting clamps as fmax.s/fmin.s | 1.1915 (-2.1%) | 1.1379 (-2.2%) | 2.7605 (-4.0%) | spot -2.8%, colormat -2.7% | 8/8/0 | -606 | identical | **keep** |
| G14h | provoking colour and glColor clamps as fmax.s/fmin.s | 1.1850 (-0.5%) | 1.1315 (-0.6%) | 2.7605 (0) | colormat -2.7%, prim7/8 -0.9% | 8/8/0 | -500 | identical | **keep** |
| G02f | normalisation inline in the vertex op | 1.1763 (-0.7%) | 1.1229 (-0.8%) | 2.7221 (-1.4%) | | 8/8/0 | +184 | identical | **keep** |
| G14i | gl_ctx hidden (PC-relative); context last in the vertex op | 1.1774 (+0.1%) | 1.1240 (+0.1%) | 2.6789 (-1.6%) | mech -0.9%, colormat -1.0% | 8/8/0 | -68 | identical | **keep** (immediate mode gains 43 k, list replay pays 1 k) |

The library-object .text sum (all objects, board flags, before the link)
rose 166,303 -> 167,403 B (+1,100), mostly the vertex cache's second
instantiation of the vertex op (2.3 kB, executed only by glDrawElements).
The linked library shrank (section 1): --gc-sections and G14b's 2.4 kB.

### G01. Soft doubles out

- **glRotate** (matrix.c): s31_sincos_deg(p[1].f) - the angle stays in
  degrees, is reduced exactly by the nearest multiple of 90 (deg - 90q is
  exact for |deg| < 2^23; fmodf above), converted with a two-part pi/180,
  and evaluated with float minimax polynomials (mpmath fits) in explicit
  fmaf, the cosine's 1 - z/2 compensated. It was `angle * M_PI / 180.0` in
  double, rounded to float, then libm sin and cos in double: about 60
  soft-double calls per rotation with musl. The axis normalisation uses
  1/sqrtf. gl_M4_Rotate became gl_M4_RotateSC(s, c).
- **Accuracy** (gl/tests/fmath_test.c, against a double reference that
  reduces the same exact way; every 7th float and every 1/4096 degree in
  [-720, 720], 2^24 random angles in [-1e6, 1e6], every integer to
  +-100000): **sin and cos <= 0.915 ulp**, max absolute error 5.5e-8;
  sincos(90) = (1, -0), (180) = (-0, -1), exactly. The old path's absolute
  error reached 9.8e-4 at large angles (the float angle in radians).
  **s31_powf <= 8.4e-6 relative** (134 ulp at y = 128: the float log2 is
  amplified by y; libm powf 0.51 ulp) over the specular table's x = i/1024
  and every 97th float in [0.001, 1], y from 0.5 to 128. [review 3a R5:
  that grid missed the worst case. 2^28 random pairs find 1.15e-5 = 174
  ulp at x = 0.70, y = 127.5; the bound is about 1.2e-5, and fmath_test now
  samples y near 128 densely with a 1.25e-5 limit.] **s31_expf <=
  1.077 ulp** over [-87.3, 88] (libm 0.50). The same numbers on the Mac
  and in the arm64 rig.
- **GL_SPOT_CUTOFF** cos, **spot exponent** powf (per vertex per spot
  light), **specular table** pow (1,025 calls per new shininess), **fog**
  expf (per vertex, and per glRasterPos) use the same functions. musl's
  powf and expf compute in double internally (objdump of the board
  toolchain's musl objects: powf has 14 __muldf3 and 10 __adddf3 call
  sites, expf 6 and 4, sinf goes through __sindf/__cosdf in double).
- **gl_V3_Norm** uses sqrtf (a bare fsqrt.s; sqrt was a soft-double call
  and two conversions on every lit vertex under GL_NORMALIZE).
- **GLU-facing double entry points** already used the bit-exact s31_d2f;
  the widening direction (glGetDoublev, glGetClipPlane, glGetTexGendv,
  glPopAttrib's glClearDepth/glDepthRange) now uses s31_f2d_bits /
  s31_i2d_bits (s31_float.h): exact for every input, **all 2^32 float
  patterns and every 1021st int checked** (gl/tests/f2d_test.c, 0
  mismatches). gluProject/gluUnProject call glGetDoublev every time.
- gl_resizeImage (NPOT upload resampling) used floor() on floats that are
  never negative: it uses the truncation it already had.
- **Call sites left** (objdump -dr of every object, dcensus.sh; 67 -> 14):
  the special-value fallbacks of s31_d2f_bits / s31_f2d_bits (zeros,
  denormals, inf, NaN) and glGetDoublev's; the GL_DOUBLE array fetch's
  fallback; gl_print_matrix/gl_print_op (debug printing); fmodf for
  |angle| >= 2^23 degrees; sqrtf in glopRasterPos (musl's sqrtf is a bare
  fsqrt.s). **None is on a per-frame or per-vertex path.**
- **-fsingle-precision-constant: not used.** After the explicit edits,
  every TinyGL object it compiles is byte-identical to the build without it
  (it would only narrow future double constants silently). Recorded in
  build-lib.sh.
- **-fno-math-errno: not used, measured inert**: GCC already emits a bare
  fsqrt.s for sqrtf here; instruction counts identical to the last digit,
  .text -16 B. Recorded in build-lib.sh.

### G02. Lighting

- **a. Zero specular skipped** (light.c): GLMaterial.do_specular and
  GLLight.has_specular, maintained where the colours are set; the whole
  specular block (local-viewer normalisation, half vector, sqrt, table
  lookup) is skipped when either is zero. The default material's specular
  is (0,0,0), so gears, glxgears and teapot computed it on every lit vertex
  and added 0. Bit-exact (every skipped term is x * 0). The material also
  caches its specular table (the LRU list was walked per vertex per
  light); a cached table is re-validated by its shininess key.
- **b. One reciprocal for normalisation: not kept.** 1/n and three
  products instead of three divides measured **+3 instructions a call**
  (gears +0.25%, teapot +0.43%; the 1.0f constant costs two) with
  identical frames. Its only possible gain is fdiv.s latency, which the
  instrument cannot see. A board A/B (a microbenchmark of 3 fdiv.s against
  fdiv.s + 3 fmul.s) decides it; recorded next to the code (vertex.c).
- **c. Cached inverse-transpose**: gl_normal_matrix (vertex.c) keeps the
  bit pattern of the modelview it inverted and returns at once when it has
  not changed (a projection or texture-matrix change, or a push/pop pair,
  also set the shared "matrices changed" flag). An affine modelview takes
  the 3x3 cofactor inverse (one divide) instead of the 4x4 Gauss-Jordan
  with pivoting; projective and singular ones keep the old path. mech-like
  -20.5%; gears -0.2% (3 inverses a frame). Frames identical.
- **d. Per-vertex work that repeats per light**: emission + ambient x
  scene ambient (per material side) and each light's ambient x material
  ambient are computed when a material, light or light model changes
  (light_dirty; only the front side unless two-sided), with the same
  expressions, so bit-identical. glColor under GL_COLOR_MATERIAL used to
  build a glMaterial op and recurse through glopMaterial; it writes the
  tracked fields directly (gl_color_material), which also paid for the
  re-computation the colour-material case now needs (geo7 -1.5% net; it
  was +6.9% before that).
  - **Rejected: the diffuse product too.** Keeping light diffuse x material
    diffuse measured gears -0.27%, teapot -0.55%, two lights -0.96%, but
    colour material **+0.7%** (recomputed per vertex), and it is not
    bit-exact (dot x (ld x md) against (dot x ld) x md). Recorded in
    light.c.
- **e. Clamps as fmax.s/fmin.s** (light.c clampf, zgl.h gl_zp_chan, api.c
  clamp01): the four colour channels of every lit vertex were compare-and-
  branch chains: gears -2.1%, teapot -4.0%. The same value for every
  number; a NaN now clamps to the bound instead of passing through, and a
  -0 becomes +0 (the integer colour is the same). Frames identical.
- **f. Normalisation inline** in the vertex op: the call clobbered every FP
  register (all caller-saved under ilp32), so the clip coordinates were
  reloaded after it. teapot -1.4%.

### G14. Dispatch, assembly and arrays

- **Context layout** (two steps, ctx1 and ctx2, -2.4% and -0.4% gears,
  -4.4% and -0.8% teapot, bit-exact): GLContext is about 4.8 kB and an
  RV32 load reaches +-2 kB from its base, so a field past 2 kB costs an
  addi at each use. The 16 lights (2.2 kB, reached through l pointers)
  sat at the front and pushed every hot field past 2 kB; they and the cold
  arrays (selection names, the guard's S x P, clip and texgen planes,
  polygon stipple) now sit at the end, and every field the vertex path
  reads is below 2 kB (checked with DWARF offsets). **Found the hard way:**
  a 68-byte field inserted in the middle cost glopVertex one instruction
  per vertex (+4.8 k per teapot frame) - zgl.h now says so where new
  fields go.
- **Immediate-mode dispatch**: glVertex, glNormal, glColor, glTexCoord,
  glEdgeFlag, glBegin and glEnd run their op directly while not compiling
  a list (exec_flag is 0 only while compiling): gl_add_op's context call,
  flag tests and op-table indirect call were ~29 instructions a call.
  glNormal, glTexCoord and glColor store into the context themselves; the
  vertex op takes its coordinates as float arguments (under ilp32 they
  arrive in integer registers) with the context last, so glVertex4f passes
  a0-a3 through; gl_ctx is hidden in its declaration (PC-relative, one
  load fewer); list replay calls the vertex op directly. The generated
  gl_attrib.c wrappers are built without a frame pointer: each built and
  tore down a frame before its tail call (4 of glVertex3f's 10
  instructions); the frame is gone at the tail call anyway, so backtraces
  lose nothing (-2.4 kB .text). teapot -5.1%, -2.1%, -4.5%, -1.6% over the
  four steps.
- **Copy-free strips** (vertex.c): GL_QUAD_STRIP, GL_TRIANGLE_FAN and
  GL_LINE_STRIP/LOOP copied one or two 148-byte vertices per vertex (~75
  instructions a copy); new vertices now alternate between slots and the
  primitive is drawn from where they are, with the same vertex order,
  provoking vertex and edge flags. glxgears and gears -6%. The geo8 probe
  (all four primitives, flat and smooth, colour per vertex) is
  byte-identical to the copying build; so is glx_prims.
- **Transform specialisation**: glFrustum's and glOrtho's zeros (and the
  viewport guard's S x P of them) are skipped in the lit path's projection
  (7 products instead of 16), and an affine modelview's w row gives w = 1.
  The projection's shape is found only when it changes (matrix.c marks a
  projection-mode change; the guard and the lighting switch mark it too).
  gears -1.2%, teapot -2.2%; geo4 (512 matrix operations, one triangle)
  +0.4%. A product by an exact zero adds exactly 0, so frames are
  identical. [review 3a R3: not as first written. `a*b + c*d` let GCC fuse
  the other product than the 4-term sum had, so lit geometry's depth moved
  by +-1 LSB on a few pixels (rt_geo, 81 of 232 frames at 320x240). Part C
  writes the kept terms as the explicit fmaf the general branch contracts
  to; rt_geo against HEAD is now identical except G01's trig.]
- **Post-transform vertex cache for glDrawElements** (arrays.c): 64
  entries direct-mapped by index, valid for one call (a generation
  counter), holding the transformed, lit and projected vertex; a hit
  copies it into the primitive. **geo3 (a 33x25 sphere grid, 1,536
  triangles) -34.3%** (kill rule: 20%). 32 entries gave only -6%: a grid
  row no longer fits and the next row misses (recorded in zgl.h). RAM:
  9,984 B per context, allocated at its first glDrawElements. The current
  colour/normal after the call are the last miss's, which GL 1.3 2.8
  leaves undefined. glx_geo's two glDrawElements cells (a colour-material
  lit grid; GL_QUADS in GL_LINE mode with an edge-flag array) are
  byte-identical to the uncached base tree.
- **Array-path audit.** The strides and the normal-array Z bug the
  assessment listed were already fixed in phase 2 (arrays.c s31 rewrite).
  Found now:
  - **w was ignored** by both vertex paths (TinyGL assumed w = 1): every
    glVertex4 with w != 1, and every vertex of a size-4 vertex array, was
    drawn at (x, y, z). Now the full product (an out-of-line path taken
    only when w != 1; the test is 2 instructions a vertex).
  - glopDisableClientState cleared the wrong bits (`&=` for `&= ~`;
    unreachable - the entry point sets the state itself).
- **Found outside arrays: GL_LIGHT1..7 defaulted to LIGHT0's white diffuse
  and specular** (GL 1.3 table 6.9: (0,0,0,1)). An app enabling LIGHT1
  with only its ambient and diffuse set got a white highlight Mesa does not
  draw: morph3d is now 0.000% from Mesa on all three frames.

## 4. Measured and not kept

| experiment | measured | decision |
|---|---|---|
| -fno-math-errno | identical instruction counts; .text -16 B | not used (inert) |
| -fsingle-precision-constant | TinyGL objects byte-identical | not used (inert, a hazard) |
| G02b one reciprocal in gl_V3_Norm / the inline normalise | +3 insns a call (gears +0.25%, teapot +0.43%), identical frames | not kept; needs a board fdiv.s latency A/B |
| G02d diffuse product | gears -0.27%, teapot -0.55%, geo6 -0.96%, geo7 **+0.7%**; not bit-exact | not kept |
| vertex cache with 32 entries | geo3 -6% (64: -34%) | 64 kept |
| G14d first cut: the matrices' shapes found at every glBegin after a matrix change, plus an unlit glOrtho path | geo5 (a glBegin per triangle) +0.66%: ~54 insns per glBegin against ~9 saved per vertex | replaced by the kept version (shape found only when the projection changes; unlit path unspecialised): geo5 -0.4% |

## 5. Code and RAM

- Buildroot-mode libGL.so.1 (gl/bench/brsize.sh): .text 149,032 ->
  146,672 B, .rodata 21,308 -> 21,428 B, stripped 230,916 -> 226,804 B.
  The before figure equals the shipped /src/images/libGL.so.1 (230,916 B,
  md5 6deff2cd..., untouched by this work).
- Hot code per frame (foot.sh; blocks run every counted frame), library +
  runtime: gears 9,558 + 4,702 -> 8,896 + 1,024 B; glxgears 8,556 + 4,702
  -> 7,924 + 1,024 B; teapotf 6,728 + 3,738 -> 6,320 + 48 B. The runtime
  left in gears/glxgears is the demo's own double code; teapotf's 48 B is
  memcmp (the normal-matrix cache check).
- RAM: GLContext 4,700 -> 5,280 B (RV32 sizeof): GLLight 136 -> 164 B
  (the specular flag and the two cached ambient products, x16 lights =
  +448 B) and 132 B of cached state (normal-matrix key, light bases,
  transform shapes, the vertex-cache pointers). GLVertex is unchanged
  (148 B). The vertex cache's 9,984 B exists only in a context that has
  called glDrawElements.

## 6. Correctness (all host or qemu)

| check | result |
|---|---|
| tools/glref suite, 22 apps (`phase3a/final/suite`, suitecmp.txt against `phase3a/suite-base`, which reproduces SUITE.md 10.2 exactly) | **0 worse**; 19 identical to the last digit; morph3d 0.016/0.034/0.025 -> 0.000/0.000/0.000 (LIGHT1 default); geartrain strict 0.212 -> 0.210 (tolerant the same); rrootage tolerant 8.182/7.960 -> 8.180/7.956. glxgears and gears 0.000%. Verdicts as 10.2 (fire, teapot, testgl FAIL; testgl2 ERROR; rrootage MISSING-SYMBOL - all for the reasons SUITE.md gives). xlite load arm 22/0 |
| run-raster.sh pages 1-6 | summary identical to artifacts/gl/f3f6/raster |
| run-pixels.sh p1-p4, g1-g3, b1-b2 | summary identical to artifacts/gl/f7/pixels; all logs "0 differ" |
| run-prims.sh | PASS, 0.003% tolerant (2 px), as before |
| core_test | 185/0 host, 183/0 RV32 qemu-user |
| raster_gate | 51/0 host and RV32 |
| d2f_test (RV32) / f2d_test (host, 2^32) / fmath_test | 0 mismatches / 0 / PASS |
| headless_gears 320x240x100 md5 | 38859f0c896efe6b12bc68e0fe3afe1f on the host and RV32, unchanged |
| ASan + UBSan (run-san.sh, now with glx_geo) | clean |
| builds | host, RV32 and Buildroot mode: 0 warnings |
| **gl/tests/glx_geo.c** (new, run-geo.sh): w != 1 unlit/lit/array/list/perspective, rotations 0..1e6 degrees, specular 1/20/128, two-sided, spot, attenuation, local viewer, colour material x7, lists, loops, polygons, a projective modelview, fog EXP/EXP2, glDrawElements | against Mesa: base tree 6.695% tolerant, final **2.331%**. Final against the base tree's own frame: identical everywhere except the w != 1 cells (the fix). What still differs from Mesa: Gouraud colour across a triangle whose vertices have different w (Mesa interpolates perspective-correctly; testgl's known issue), specular-table quantisation dots, line placement. Images: geo-base/, geo-final/ (sbs-vs-base.png) |

Per-lever gates: g01/ (after G01), g02/ (after G02a-d and ctx1), g14/
(after G14a-d and the light fix), final/. Each holds host.log, rv32.log,
suite/, suitecmp.txt, pixels/, raster/, prims.log.

## 7. Open, for owners

1. [review 3a: superseded by C7, the ranked board A/B list with
   board-weighted projections.] **Board A/B not done** (rule: no board). The instrument predicts the
   geometry part of a glxgears frame down ~58% and the whole frame ~34%;
   the board's glxgears profile had the libGL geometry items at about 8%
   of CPU0 samples plus musl sqrt and soft double, so the fps gain there is
   bounded by that share - **not estimated as a number**. Needs: fresh boot
   per arm, 5+ runs, the new libGL built through Buildroot.
2. G02b (one reciprocal) needs the fdiv.s latency measured on the board.
3. The clear is untouched and now the second-largest item in glxgears
   (135 k instructions, 90 k 4-byte stores of 1.12 M): G03 / P3 / PIE are
   the levers for it.
4. Perspective-correct colour interpolation (testgl f60, glx_geo row 0) is
   the one geometry-visible difference from Mesa left in the probe; it
   belongs to the rasteriser (a divide per span).
5. gl/out-rv32 and gl/out-host now hold this tree's builds (gitignored);
   gl/out-rv32/libGL.so.1.unstripped no longer matches /src/images.
6. glPopAttrib still widens through s31_f2d then narrows again (correct,
   bit-exact, ~20 instructions): an internal float setter would drop it.
7. Outside gl/: s31-libgl.mk's rsync exclusions should also skip
   gl/bench/base3a (the before snapshot) and gl/bench/out* (as SUITE.md 8.6
   noted for the older ones); the new sources (s31_fmath.c) are picked up
   by build-lib.sh's glob with no packaging change.

## 8. Commands

```
S31_BENCH_LIBM=musl BENCH_NO_LIMITS=1 gl/bench/bench.sh gl gl/bench/out-X   # the lever tables
gl/bench/bench.sh gl gl/bench/out-Y                  # the +1% guard and tripwires (newlib)
gl/bench/prof.sh gl/bench/out-X g_glxgears_300 q_teapotf_320   # per function, loads/stores
FOOT_IMAGES="q_gears_320 g_glxgears_300 q_teapotf_320" gl/bench/foot.sh gl/bench/out-X
python3 gl/bench/levers.py gl/bench/out-A gl/bench/out-B ...   # the per-lever table
gl/bench/dcensus.sh gl/bench/out-X/obj              # soft-double call sites
./docker/build.sh 'sh /src/gl/bench/brsize.sh'      # Buildroot-mode sizes, base3a vs gl
gl/tests/run-3a.sh LABEL                            # every correctness check -> phase3a/LABEL
gl/tests/run-geo.sh /src/artifacts/gl/phase3a/geo-X [LIBDIR]
gl/tests/run-san.sh
python3 gl/bench/suitecmp.py artifacts/gl/phase3a/suite-base/report.json artifacts/gl/phase3a/X/suite/report.json
```

---

# Part B: pixel-side levers - the clears (G03, clear speed, dirty rows) and the fillers (2026-09-26)

[review 3a: the byte columns below count every store, stack included,
over a 10-frame window that misses the recurring real depth clear. C1 and
C2 restate them by address (colour + depth only), over 10 and 60 frames,
with board-weighted projections. B4's two exceptions (scissored clears
below 1.0, slivers) were bugs and are fixed (C3 R1, R4); DB-x is back
behind S31GL_DIRTYBOX=2.]

Same rules as part A. Everything was measured on the host with
qemu-system-riscv32 instruction counts (gl/bench), the s31-glref rig (Xvfb +
Mesa) and qemu-user. **Nothing ran on the board.** This part has no board
fps, PSRAM, I-cache, XIP or FPU-latency timings. The clears are
memory-bound on the board, so for them the tables also give stores issued
and bytes written. prof.sh counts every load and store qemu executes,
stack traffic included. The "before" tree is the working tree as part A
left it, snapshotted as gl/bench/base3p (gitignored). It reproduces part A's
final numbers to the last digit (gl/bench/out-p0b against part A's
out-final).

## B1. Result

M instructions per frame (musl libm, board flags). Stores and kB are
everything the frame writes. "Clear stores" are the stores of the fill
routine.

| case | before | after | change | stores | kB written | clear stores | frame |
|---|---|---|---|---|---|---|---|
| **glxgears-like 300x300** | 1.1240 | 0.9652 | **-14.1%** | 237.5k -> 166.6k (-30%) | 847 -> 563 (-34%) | 90.0k -> 36.4k (-60%) | same (5f1ff8c8) |
| glxgears-like, two buffers in turn (as GLX now runs it) | 1.1242 | 0.9655 | -14.1% | 237.6k -> 166.7k | 847 -> 563 | 90.0k -> 36.4k | same |
| gears 320x240 | 1.1774 | 1.0300 | **-12.5%** | 235.5k -> 178.2k (-24%) | 827 -> 598 (-28%) | 76.8k -> 38.4k | same |
| gears 640x400 | 2.2813 | 1.8794 | -17.6% | 587.7k -> 418.5k | 1937 -> 1260 (-35%) | 256.0k -> 128.0k | same |
| texobj 320x240 | 0.3997 | 0.3251 | -18.7% | 110.5k -> 69.1k | 381 -> 215 (-44%) | 76.8k -> 35.3k | same |
| texobj 640x400 | 1.2585 | 1.0079 | -19.9% | 362.8k -> 224.7k | 1247 -> 694 (-44%) | 256.0k -> 117.8k | same |
| teapotf 320x240 | 2.6789 | 2.5676 | -4.2% | 413.3k -> 344.5k | 1629 -> 1354 (-17%) | 76.8k -> ~16.7k | same |
| teapotf 640x400 | 3.4582 | 3.0641 | -11.4% | 650.8k -> 435.0k | 2508 -> 1645 (-34%) | 256.0k -> 64.2k | same |
| **clear-heavy 320x240** (geo1) | 0.1984 | 0.0971 | **-51.1%** | 89.0k -> 25.9k | 337 -> 85 (-75%) | 76.8k -> 15.3k | same |
| clear-heavy 640x400 | 0.6639 | 0.3447 | -48.1% | 298.3k -> 97.2k | 1118 -> 314 (-72%) | 256.0k -> 58.2k | same |
| indexed mesh (geo3) / rotates (geo4) / mech-like (geo5) | 2.5881 / 0.6354 / 0.7824 | 2.4450 / 0.5262 / 0.6936 | -5.5% / -17.2% / -11.3% | | | | same |
| spec+spot (geo6) / colormat (geo7) / strips (geo8) | 3.8182 / 3.8236 / 0.5341 | 3.6752 / 3.6805 / 0.4467 | -3.7% / -3.7% / -16.4% | | | | same |

- **Every frame hash is unchanged**: all 45 bench lines, glxgears-2buf
  included. The full glref suite is identical to part A's final, all 22
  apps, to the last digit (B8).
- **Where glxgears' 0.16 M went** (prof.sh, per frame):
  - the clear, 135.0k -> 41.0k instructions and 90.0k -> 36.4k stores
    (0.36 MB -> 0.15 MB written);
  - ZB_fillTriangleFlat_lt, 411.3k -> 350.9k;
  - ZB_fillTriangleSmooth_lt, 51.3k -> 47.4k;
  - ztri_setup, 75.2k -> 71.1k (the scalar sort, and the epoch record
    keeping moved out of it).

  The clear bookkeeping costs about 4k: the depth-epoch and dirty-row
  record keeping, per new maximum and once per clear.
- **The board had the clear as its biggest libGL item.** memset_16 was
  10.0% of all CPU0 samples at 31 fps. The present owner's two-buffer
  profile had it at 24.5% of glxgears' samples, and that owner measured the
  clear and the fill slowing down while lvdesk copies the previous frame
  through PSRAM (gl/glx/glx_present.c want_bufs). The byte counts above are
  the quantity that matters there. The instruction counts under-state the
  gain for memory-bound code. **Board fps is not measured, and not
  estimated as a number.**
- **Code**:
  - the library objects' .text grew 167,405 -> 172,099 B (+4.7 kB);
  - s31_zepoch.c is 2.5 kB of that, all of it once per clear or rarer;
  - the flat and smooth fillers are the same size or smaller;
  - Buildroot mode: B7.
- **RAM**:
  - 20 bytes per depth buffer (S31GL_DEPTH_TAIL);
  - about 90 bytes per context (ZBuffer and ZPipe fields);
  - no new allocation.

## B2. How it was measured (additions to section 2)

- **`glxgears-2buf 300x300`** (geo.sh). glxgears_q.c draws into two colour
  buffers, bound in turn after every frame (q_ui.c `-DQ_BUFS=2`). This is
  how GLX now presents a double-buffered drawable of 200 kB or less: the
  present work changed want_bufs while this part was in progress. The case
  has a +2% tripwire in limits.txt. The other q_ui images keep one buffer.
- **q_ui.c marks its buffer retained** (s31gl_set_retained). The call is
  weak, so older trees still link. `-DQ_NORETAIN` measures without it.
- **`S31_BENCH_DEFS`** (build_q.sh) adds -D flags to the library objects
  only. The epoch lever was measured off with `-DS31GL_ZTRICK_DEFAULT=0`.
- **`BRSIZE_TREES`** (brsize.sh): which trees the Buildroot-mode size
  compares.
- **`S31GL_HOST_DEFS`** (host-build.sh): -D flags for a diagnostic host
  build. `-DZEP_TRACE` prints each depth clear's decision.
- **gl/tests/zepoch_test.c + run-zepoch.sh (new)**: the invisibility test
  (B4). It runs under ASan and UBSan in run-san.sh.
- Raw outputs:
  - artifacts/gl/phase3a/bench/pixel/out-*/ (results, feat, pix, prim,
    geo, prof_*.txt, foot);
  - levers-table.md (every case, every step);
  - stores-table.md (memory operations per step);
  - brsize.txt.
- Correctness runs (artifacts/gl/phase3a/, each from gl/tests/run-3a.sh):
  - p-g03: G03-h, fixed halves;
  - p-g03b: G03-p, adaptive precision;
  - p-g03c: G03, exact;
  - p-all1: after the dirty rows, before the per-buffer slots;
  - pix-final: the final tree;
  - zepoch/: run-zepoch.sh;
  - zep-trace-adaptive/: each depth clear's decision in geartrain, fire,
    gears and glxgears (a ZEP_TRACE host build of G03-p).

## B3. Per lever

Each row is measured on top of the row above: M instructions per frame,
with the change against the row above. The "stores" column is glxgears'
per-frame stores, from prof.sh.

| # | lever | gears 320 | glxgears 300 | texobj 320 | teapotf 320 | clear 320 | other | stores (glxgears) | frames | decision |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | before (base3p) | 1.1774 | 1.1240 | 0.3997 | 2.6789 | 0.1984 | | 237.5k | | |
| L1 | **ZB_fill16**: every clear is aligned 32-bit stores, 16 per loop turn, end pointer | 1.1488 (-2.4%) | 1.0905 (-3.0%) | 0.3711 (-7.2%) | 2.6502 (-1.1%) | 0.1698 (-14.4%) | texobj 640 -7.6%, clear 640 -14.4% | 237.6k (same stores; 1.50 -> 1.13 insn per store) | same | **keep** [review 3a m4: same stores, so expect ~0 on the board] |
| G03-h | depth epochs, fixed halves (2 epochs, 15 bits, exact) | 1.1298 (-1.7%) | 1.0676 (-2.1%) | 0.3495 (-5.8%) | 2.6333 (-0.6%) | 0.1485 (-12.5%) | | | same in bench; **suite: geartrain 0.030 -> 0.047%, fire 3.159 -> 3.168%** | **reverted** (precision) |
| G03-p | depth epochs, adaptive range, at least 15 bits | 1.1217 (-2.4%) | 1.0564 (-3.1%) | 0.3713 (0) | 2.6318 (-0.7%) | 0.1442 (-15.1%) | prim7/8 +0.5% | | same in bench; **suite: geartrain f20 0.030 -> 0.039%** | **reverted** (precision) |
| G03 | **depth epochs, exact** (integer base above everything stored: B4) | 1.1388 (-0.9%) | 1.0708 (-1.8%) | 0.3714 (+0.1%) | 2.6295 (-0.8%) | 0.1445 (-14.9%) | gears 640 -3.8%, teapot 640 -3.3% | 185.2k (-52.3k; clear 90.0k -> 45.0k) | same, suite identical | **keep** |
| F1 | flat span loop: two pixels a turn, odd first, end pointer | 1.0463 (-8.1%) | 0.9861 (-7.9%) | 0.3716 | 2.6344 (+0.2%) | 0.1447 | gears 640 -8.9% | | same | **keep** |
| F2 | smooth span loop: the same | 1.0414 (-0.5%) | 0.9819 (-0.4%) | 0.3716 | 2.6129 (-0.8%) | 0.1410 (-2.6%) | prim4 -10.1%, prim8 -5.3%, geo3 -3.6%, teapot 640 -2.5% | | same | **keep** |
| F3 | the rows end at a depth-row pointer | 1.0349 (-0.6%) | 0.9759 (-0.6%) | 0.3716 | 2.5990 (-0.5%) | 0.1404 | prim8 -1.7%, prim4 +0.7% | 176.1k | same | **keep** |
| G03-g | epoch record keeping behind one flag (zact) when no epoch can follow | 1.0370 (+0.2%) | 0.9780 (+0.2%) | 0.3716 | 2.6029 (+0.2%) | 0.1404 | **prim7/8 +1.3% -> +0.3%** | | same | **keep** |
| DB-x | dirty box with the x extent, per triangle at the fill dispatch | 1.0528 (+1.5%) | 0.9798 (+0.2%) | 0.3230 (-13.1%) | 2.5930 (-0.4%) | 0.0964 (-31.3%) | prim7/8 +2.0% | 159.7k (clear 45.0k -> 27.5k) | same | **not kept**: needs a board A/B (B5) [review 3a M2: rebuilt behind S31GL_DIRTYBOX=2 (C2): projected -0.24 ms per glxgears frame on the board] |
| DB | **dirty rows**: a full clear writes only the rows drawn since that buffer's last full clear; per colour buffer | 1.0384 (+0.1%) | 0.9732 (-0.5%) | 0.3252 (-12.5%) | 2.5810 (-0.8%) | 0.0974 (-30.6%) | texobj 640 -13.3%, teapot 640 -2.5%, geo4 -9.4% | 168.0k (clear 45.0k -> 36.4k) | same | **keep** |
| F4 | ztri_setup sorts scalars instead of indexing stack arrays | 1.0341 (-0.4%) | 0.9691 (-0.4%) | 0.3252 | 2.5735 (-0.3%) | 0.0974 | prim7/8 -0.6% | | same | **keep** |
| F5 | flat filler: aligned pairs as one 32-bit depth load and two 32-bit stores | 1.1093 (+7.3%) | 1.0382 (+7.1%) | | | | | filler stores 54.8k -> 36.5k (-33%) | same | **reverted** |
| F6 | textured filler: remainder loop to an end pointer | 1.0341 (0) | 0.9691 (0) | 0.3256 (+0.1%) | | | prim7 +0.4% | | same | **reverted** |
| G03-f | the far-plane check (GL_LESS) moved into the fillers, and the wrapper dropped; the toggle read cached | 1.0300 (-0.4%) | 0.9652 (-0.4%) | 0.3251 | 2.5676 (-0.2%) | 0.0971 (-0.3%) | prim4 +0.2% | 166.6k | same | **keep** |

**G03 alone, on the final fillers.** The fillers built without the epoch
code (`out-f3noz`) against with it (`out-f4`):
- glxgears -3.8%, gears 320 -2.9%, gears 640 -6.5%, teapotf 640 -3.3%;
- clear-heavy -15.3%;
- texobj +0.05%;
- prim7/prim8 +0.3/+0.4% (2,400 small triangles with no epoch room: record
  keeping behind the flag);
- geo3/geo6/geo7 +0.2-0.3%.

The depth-clear stores go to 0 on the frames an epoch starts: 12 of the 13
clears of a gears or glxgears bench run (the first is real). The room rule
allows about 15 epochs per real clear at glxgears' depth range (B4).

**The dirty rows alone** (out-f4 -> out-d4):
- clear stores: glxgears 45.0k -> 36.4k, texobj 320 76.8k -> 35.3k,
  teapotf 640 140.8k -> 64.2k, clear-heavy 53.8k -> 15.3k;
- gears 320 and 640 are unchanged: their gears reach every row;
- the recording costs about 8 instructions a triangle while it is on;
- a frame whose rows cover 90% or more stops recording for 16 frames.

## B4. G03: depth epochs, exactly

**Why not the ping-pong the assessment describes.** The classic trick
alternates the two halves of the range, flips the compare and never
clears. It is not invisible to the application. A pixel drawn two frames
ago and not drawn over last frame is back inside the current half, so it
still occludes. Quake's gl_ztrick gets away with this because its world
covers the whole screen every frame; glxgears does not.

Any exact scheme needs every stored value to say which frame since the last
real clear wrote it. So each epoch needs values above all earlier ones.
Fixed halves (G03-h) do that at a cost of one bit, with a real clear every
second frame. The suite rejected it: geartrain's error against Mesa grew
from 0.030% to 0.047% (strict 0.212 -> 0.359), and fire's from 3.159% to
3.168%. Both are z-fighting speckle on coincident surfaces. An adaptive
range with a 15-bit floor (G03-p) still moved geartrain (f20 0.030 ->
0.039%).

**What is kept (tinygl/source/s31_zepoch.c).**
- **The mechanism.**
  - A depth value is stored as TinyGL's own value plus an epoch base B:
    s = B + s_plain.
  - B is added as an integer to each triangle's depth plane, after the
    plane is computed (ztri.h ztri_zepoch). Lines, points and pixel
    rectangles add it to their end or raster depth.
  - So two fragments of an epoch compare exactly as they did, and stored
    values minus B are the plain ones bit for bit. **No precision or depth
    bit is lost.**
  - The rasterisers record zmax, the highest value stored since the last
    real clear: per triangle from its vertices, per line, point and pixel
    rectangle.
  - A full glClear of depth to 1.0 starts the next epoch at B = zmax + 2
    without writing anything. Everything left in the buffer is then below
    every value the new epoch can store, so it reads as 1.0.
- **Room.**
  - An epoch starts only if the last epoch's range, plus an eighth, fits
    above the new B. Perspective depth crowds towards the far plane, which
    is stored low: glxgears stores s_plain < 4,100 of 65,535 and geartrain
    < 4,800 (zep-trace-adaptive/ holds the trace).
  - So gears and glxgears clear for real about once every 15 frames: B
    grows by about 4,100 per epoch, until B + 1.125 x 4,100 no longer fits
    in 65,535. texobj's quads sit at d = 0.36, near the near plane, so it
    never has room.
  - If a primitive does not fit after all (the scene came nearer), the
    buffer is **demoted** first: s -> s - B, and stale -> 0. That is exactly
    the buffer a plain clear would have given by then.
- **The cases where a stale pixel does not act as 1.0.**
  - This happens only against a fragment whose own plain value is 0 (d
    within one step of 1.0), and only for LESS, GEQUAL, EQUAL and
    NOTEQUAL. The buffer is **materialised** first: stale -> B.
  - For GL_LESS the fillers check the triangle's lowest vertex depth
    (ztri_zepoch), and lines, points and pixel rectangles check their own.
    GEQUAL, EQUAL and NOTEQUAL materialise when they become current.
  - LEQUAL, GREATER, ALWAYS and NEVER are exact as they are.
  - A demotion or materialisation costs more than the clear it replaced,
    so the next 8 clears are real.
- **Everything that reads depth back decodes it** (glReadPixels,
  glCopyPixels of GL_DEPTH): stale -> 1.0, any other value -> the plain
  decode of s - B.
- **Other clears.**
  - glClearDepth other than 1.0: a real clear.
  - A scissored or masked depth clear writes the epoch's value inside the
    box. [review 3a R1: without a fit check, so a box cleared to less than
    1.0 late in the epochs wrapped below B and read back as 1.0. Fixed:
    zep_clear_rect_value demotes first when the value does not fit.]
  - glDrawPixels and glCopyPixels of GL_DEPTH_COMPONENT demote first.
- **Selection** uses the plain values (the base is added at rasterisation,
  never to zp.z).
- **Sharing.**
  - The state sits in 20 bytes after the depth values (S31GL_DEPTH_TAIL,
    s31gl.h).
  - The GLX layer's drawable depth buffer, shared by every context current
    on it, therefore carries the state (glx_present.c allocates the tail).
  - Each context re-reads it at gl_prepare_slow (make-current, bind,
    frame end).
- **The one output that can differ from the plain buffer.** A sliver
  whose depth gradient saturated (a triangle under 1/32768 px high) has
  meaningless depths in both builds. Here they are the plain garbage plus
  B, and zmax goes to the top, so the next clear is a real one. No such
  pixel showed up in any frame compared here. [review 3a R4: they do show
  up (the review's 641x401 prims frame: 38 px), and slivers are common:
  teapot needles and UV-sphere poles. Fixed exactly in part C:
  zep_tri_sliver bounds the sliver's depths from its rows.]
- **Toggle.** S31GL_ZTRICK=0 turns it off: every clear is real.

**The invisibility proof** (gl/tests/zepoch_test.c, run-zepoch.sh).
- **The scenarios:**
  - moving quads and triangles under perspective (pixels drawn two frames
    ago and not since);
  - geometry at the far plane under all eight depth functions, with a line
    and a point there;
  - a scene that comes nearer each frame until it crosses the near plane
    (demotion);
  - glClearDepth 0.5 and 0.0, a scissored depth clear mid-frame, the depth
    mask off, the depth test off;
  - polygon offset under outlines, wide lines, large points;
  - glBitmap, glDrawPixels (colour and GL_DEPTH_COMPONENT) and
    glCopyPixels(GL_DEPTH), near and far;
  - two contexts drawing into one caller-owned depth buffer;
  - two colour buffers bound in turn.
- **The comparison.** 234 frames per run (304 since part C added the
  scissor, sliver and rebind scenarios, and an x-box arm). Each frame's colour hash and
  glReadPixels depth hash (GL_FLOAT and GL_UNSIGNED_SHORT) are compared
  against the run with everything off (S31GL_ZTRICK=0 S31GL_DIRTYBOX=0).
  Three arms: epochs only, dirty rows only, both.
- **The result.** Every arm is **identical** on the host and on RV32
  qemu-user, at 96x72 and 320x240. With the epochs on, 120 of the 234
  clears left the depth memory untouched; with them off, 0.
- **One informational difference.** Host and RV32 differ from each other in
  12 frames of the pixels scenario, with every lever off as well. Each
  build's own pixel-path output is unchanged from the base (pix.sh hashes,
  the pixels pages). The cause is not established. [review 3a fix round:
  found by ASan - the test's glBitmap read 32 bytes of an 8-byte bitmap
  (unpack alignment 4), so the builds drew different memory. With the test
  fixed, host = RV32.]

## B5. Clear speed

- **Stores are already as wide as this core's scalar stores go.** RV32IMAFC
  has no D, so there is no fsd, and there is no Zilsd. TinyGL's memset_16
  already stored 32-bit words.
- **What ZB_fill16 changes is the loop.** It does 16 aligned words per
  turn to an end pointer, with a halfword at each end for an odd pixel:
  1.13 instructions per store against 1.50. The stores and bytes are the
  same.
- **PIE's 128-bit stores (esp.vst.128) were not used.** Using PIE in libGL
  would pin the client to hart 0 the first time it runs (memory
  s31-pie-pin-emptied-cpu1). And the store count is not what the board
  waits on: the fill is PSRAM line traffic.
- **Fewer bytes is the real clear lever.** The epochs remove the depth
  clear. The dirty rows remove the colour rows (and real-depth-clear rows)
  that nothing drew into. That exactness needs the buffer to be written by
  the rasteriser only, so it is opt-in per context (s31gl_set_retained).
  The GLX layer sets it: the X server only reads a ShmPutImage segment,
  and xshim copies out of it ("COPY. Always.", lvdesk/xshim.c).
- **Validity rules for skipping rows.** A clear skips rows only when:
  - the buffer's last full clear was to the same value;
  - that buffer is the one recorded in its slot (two slots per context,
    for GLX's two ping-pong segments);
  - nothing was made current or bound since (tgl_bind_serial).

  An unbind, a resize or another context invalidates the record.
- **The x extent (DB-x) was measured and is not kept.** It is exact too,
  and halves glxgears' clear again: 45.0k -> 27.5k stores, -67 kB per frame
  against the rows' -34 kB. But the per-triangle box costs about 30
  instructions (+0.2% glxgears, +1.5% gears, +2% prim7/8). It is the one
  clear lever whose decision is a board A/B: bytes against instructions.
- **Viewport-only clears are not done.** GL clears the whole buffer
  whatever the viewport. The dirty rows give the exact version of it:
  rows outside the viewport that nothing drew into are skipped.
- **"Skip pixels provably overwritten" was not attempted.** Proving
  coverage before the frame is drawn would need the frame's geometry
  ahead of the clear.
- **The glClear call path** (glClear -> gl_add_op -> glopClear) is about 150
  instructions. The epoch and row bookkeeping adds about 1-4k per frame in
  glxgears: new-zmax slow paths, row growth, one clear decision. That is
  small against the 94k instructions and 53.6k stores it removed.

## B6. Fillers

- **What the span loop was.** TinyGL's generic flat loop and its Gouraud
  DRAW_LINE did 4 pixels per turn plus a remainder loop, counted by n. GCC
  turned that into a trip count, spilled 4 registers per span, and
  recomputed the pointers for the remainder: about 35 instructions of
  overhead per span. gears' spans are 4.8 pixels.
- **What it is now.** Two pixels per turn, the odd one first, to an end
  pointer: about 21 instructions per span. The rows end at a depth-row
  pointer instead of a counter.
- **The flat filler** for glxgears went 411.3k -> 350.9k instructions per
  frame, and gears 640's 1.19 M -> 1.05 M. The smooth filler gained 0.5-10%
  (prim4 -10%, prim8 -5%, teapot 640 -2.5%).
- **ztri_setup** sorts the vertices as scalars: -0.4% on gears, the same
  arithmetic.
- **Rejected** (the numbers are in B3; the notes are next to the code in
  README.s31):
  - paired 32-bit colour and depth stores: +7% instructions, -33% of the
    filler's stores;
  - the textured filler's remainder loop to an end pointer: +0.1-0.4%.
- Zba and Zbb are used where GCC finds them (sh1add/sh2add/sh3add for
  pointers, min/max in the record keeping). Output: every bench frame hash
  and every suite frame is unchanged.

## B7. Code size and footprint

- **Library objects** (board flags, esp-elf, gl/bench): .text 167,405 ->
  172,099 B (+4,694).
  - s31_zepoch.c: +2,516, all cold.
  - clip.c: +316 (line and point guards).
  - s31_ctx.c: +384.
  - raster.c: +220.
  - ztriangle_gen.c: +264.
  - ztriangle_nt.c: +222.
  - ztriangle_lt.c / ztriangle.c: -6 each.
- [review 3a m6: C5 has the combined HEAD -> final row, the GLX drift
  separated out, and hot 32-B lines.]
- **Buildroot-mode libGL.so.1** (brsize.sh, musl toolchain), this part
  alone: .text 147,260 -> 151,242 B (+3,982), stripped 226,820 ->
  230,924 B (+4,104). "Alone" means the current core with base3p's gl/glx
  plus this part's two gl/glx hunks. The present owner kept editing
  gl/glx during this part; with that work included, gl/ as a whole reads
  .text 151,756 and stripped 235,024 B.
- **Hot code per frame** (foot.sh, blocks run at least once per counted
  frame): glxgears 7,924 -> 9,074 B, gears 8,896 -> 9,826 B.
  - The growth is the once-per-frame clear bookkeeping (zep_clear,
    zdb_clear_colour, glopClear): about 1.2 kB fetched once per frame.
  - The per-pixel and per-span code did not grow.
- **The q_gears_320 bench image** grew by 7.7 kB. That is mostly newlib's
  getenv and atoi, which libGL on the board takes from musl's libc.so.

## B8. Correctness (all host or qemu; artifacts/gl/phase3a/pix-final unless stated)

| check | result |
|---|---|
| tools/glref suite, 22 apps (`pix-final/suite`) | **0 worse than SUITE.md 10.2**. All 22 apps are identical to part A's final to the last digit (`suitecmp-vs-geometry-final.txt`). glxgears and gears are 0.000%. Verdicts are as 10.2. xlite load arm 22/0 |
| run-pixels.sh (p1-p4, g1-g3, b1-b2) | summary identical to artifacts/gl/f7/pixels; logs 0 differ |
| run-raster.sh pages 1-6 | summary identical to artifacts/gl/f3f6/raster |
| run-prims.sh | PASS, 0.003% (2 px), as before |
| glx_geo (run-geo.sh, `geo-pix`) | 2.331% tolerant against Mesa, frame byte-identical to part A's geo-final |
| run-sysgears.sh (Debian glxgears) | PASS 0.000/0.000/0.000% |
| core_test / raster_gate | 185/0, 51/0 host; 183/0, 51/0 RV32 qemu-user |
| d2f_test / f2d_test / fmath_test | 0 mismatches / 0 / PASS |
| headless_gears 320x240x100 md5 | 38859f0c896efe6b12bc68e0fe3afe1f, host and RV32, unchanged |
| **zepoch_test** (run-zepoch.sh, `zepoch/`) | epochs, dirty rows and both: **identical to everything-off**, host and RV32, 96x72 and 320x240, 234 frames; 120/234 clears left depth untouched |
| ASan + UBSan (run-san.sh, now with zepoch_test both ways) | clean |
| gl/glx/test/run-host.sh | NOSHM and SHMBUFS=2 ALL PASS. The default arm FAILs one check, "first draw allocates (SYSV maps 0 -> 2)". That comes from the present owner's want_bufs default (two segments for 200 kB or less) against the test's one-segment expectation. It passes with S31GL_SHMBUFS=1, and passes with the base library and the old GLX. Not this part's |
| limits.txt guard (newlib, `out-final-newlib-p`) | 44 lines within limits, 0 over (the new glxgears-2buf tripwire included) |
| builds | host, RV32 and Buildroot mode: 0 warnings |

## B9. Open, for owners

1. [review 3a: superseded by C7.] **Board A/B not done** (rule: no board). The clear is memory-bound there.
   - The instrument shows the bytes: glxgears -0.28 MB written per frame,
     texobj -0.17 MB, gears 640 -0.68 MB.
   - It cannot show what those bytes cost in PSRAM time next to lvdesk's
     copy.
   - It needs fresh-boot arms with a Buildroot-built libGL, and
     S31GL_ZTRICK=0 and S31GL_DIRTYBOX=0 as runtime toggles.
2. **DB-x (the dirty box's x extent)** is a board decision: +30
   instructions a triangle for a further -34 kB per glxgears frame.
3. **P3 (PPA clear at swap)** remains the lever for the colour clear of
   full-screen apps. The dirty rows give full-screen apps nothing, and the
   epochs give scenes near the near plane nothing (texobj).
4. **The present owner:**
   - glxtest's "first draw allocates" needs the two-segment default in its
     expectation;
   - this part added two things to gl/glx: the depth calloc's
     S31GL_DEPTH_TAIL and s31gl_set_retained in glxi_core_create. Any
     other writer of a drawable's colour buffer must drop the retained
     mark.
5. **zepoch_test's pixels scenario**: host and RV32 differ in 12 frames,
   with every lever off as well. The cause is not established. [review 3a:
   resolved, a test bug - see B4.]
6. **Outside gl/**: s31-libgl.mk's rsync exclusions should also skip
   gl/bench/base3p.

## B10. Commands

```
S31_BENCH_LIBM=musl BENCH_NO_LIMITS=1 gl/bench/bench.sh $PWD/gl gl/bench/out-X    # lever tables
gl/bench/bench.sh $PWD/gl gl/bench/out-Y                                          # guard + tripwires
S31_BENCH_DEFS=-DS31GL_ZTRICK_DEFAULT=0 ... bench.sh ...                           # epochs off
bash -c 'gl/bench/prof.sh gl/bench/out-X g_glxgears_300 q_gears_320 ...'          # stores, bytes
python3 gl/bench/levers.py gl/bench/out-p0b ... gl/bench/out-s5
./docker/build.sh 'BRSIZE_TREES="base3p:/src/gl/bench/base3p final:/src/gl" sh /src/gl/bench/brsize.sh'
gl/tests/run-3a.sh pix-final; gl/tests/run-zepoch.sh; gl/tests/run-san.sh
S31GL_HOST_DEFS=-DZEP_TRACE gl/host-build.sh                                       # per-clear decisions on stderr
```
