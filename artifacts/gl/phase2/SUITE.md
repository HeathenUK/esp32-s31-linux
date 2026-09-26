# GL phase 2 integration: suite report (2026-09-26)

This is the integrator's pass over three components that were built in parallel: xlite (item 3/4 fixes and the missing libX11 names), the F3-F6 rasteriser, and the F7 pixel paths. Everything ran on the host rig (`s31-glref`: Xvfb 800x480x16, Mesa 25.0.7 llvmpipe) or under qemu. **Nothing ran on the board.** There are no board fps, RSS, I-cache or XIP numbers anywhere in this report.

**The library the three components delivered is unchanged by this pass.** I made two experimental library changes, a blend-equation export and a different quad split, and reverted both (see 3.2 and 3.3). After the revert, a rebuild of `/src/images/libGL.so.1` is byte-identical to the first build of this pass (md5 `77199f7e2e0915061b012113b3d0b8d5`). Every change I kept is in the test harness (`tools/glref/`), plus this file.

## 1. Result

| goal | result |
|---|---|
| Rebuild host + RV32 libGL, host + RV32 xlite, in parallel, 0 warnings in our code | **done.** The four builds ran side by side in 2-4 s each. libGL: 0 warnings on the host, RV32 and RV32 Buildroot mode. xlite: 0 warnings in xlite sources. The 27 warnings in the RV32 xlite log all come from musl's `endian.h`, because xlite/build.sh includes the sysroot with `-I` instead of `-isystem`. That is xlite's to change (section 7). |
| Full glref suite, 3 frames each, ours vs Mesa | **22 apps: 17 PASS, 3 FAIL, 1 ERROR, 1 MISSING-SYMBOL (by the harness rule, image FAIL).** The 19 phase-1/F7 apps are unchanged (17 PASS; fire and teapot FAIL, GL_LINEAR). The new stage-5 apps: testgl FAIL (f3/f20 PASS, f60 1.9%), testgl2 ERROR (plan 4.2's blend guard), rRootage FAIL (GL_LINE_SMOOTH and more). |
| xlite load arm, 0 MISSING-SYMBOL | **22 load, 0 missing.** The RV32 link check (xlite/test/rv32-linkcheck.sh) also PASSes. |
| GLU test | **PASS** on pages g1/g2 and b1/b2, for both GLU builds (0.10% and 0.07%). g3 (NURBS) FAILs at 4.8%, as expected: the evaluators are F8 stubs. |
| SDL 1.2 testgl, SDL2 testgl2 | Built with SDL's own `test/configure` against the rig's SDL. testgl: **FAIL** at frame 60 only (cause in 3.3). testgl2: **ERROR** by design of plan 4.2. With the guard lifted in an experimental build it PASSes all three frames (3.2). |
| rRootage, GLtron | rRootage is built with its own makefile and runs (at `-O0`, 3.4). **GLtron is skipped**: the stock source has a conflicting-types error that no compiler flag turns off (3.5). |
| Perf guard (gl/bench) | Every guard demo is inside baseline + 1%. The frame hashes match the component reports (section 5). |

## 2. Per-app verdicts (`tools/glref/suite.sh --run phase2/suite`)

The numbers are tolerant-bad % per frame (strict % in brackets where it matters). The metric and thresholds are in tools/glref/README.md: a frame PASSes at 1.0% tolerant-bad or less.

| app | verdict | tolerant-bad % (frames) | cause of any non-PASS |
|---|---|---|---|
| glxgears | PASS | 0.020 / 0.034 / 0.030 (3/20/60) | |
| glxinfo | PASS | exit | vendor s31, renderer Software Rasterizer, 1.1 s31-tinygl, GLX 1.4 |
| glxheads | PASS | 0.000 / 0.000 / 0.000 | |
| manywin | PASS | 0.000 / 0.000 / 0.000 (4/20/60) | |
| multictx | PASS | 0.003 (20) | |
| offset | PASS | 0.638 (1) | |
| glxgears_fbconfig | PASS | 0.004 / 0.003 / 0.001 | |
| gears | PASS | 0.030 / 0.017 / 0.034 | |
| morph3d | PASS | 0.099 / 0.141 / 0.195 | |
| bounce | PASS | 0.030 / 0.013 / 0.048 | |
| spectex | PASS | 0.027 / 0.017 / 0.021 | |
| geartrain | PASS | 0.704 / 0.680 / 0.758 | |
| ipers | PASS | 0.186 / 0.190 / 0.177 | |
| terrain | PASS | 0.023 / 0.004 / 0.026 | |
| tunnel | PASS | 0.350 / 0.350 / 0.219 | |
| **fire** | FAIL | 3.078 / 3.158 / 3.039 | GL_LINEAR and the mipmap filters are drawn as nearest sampling of level 0 (one `libGL: approximated` line). The help text matches; this is the raster report's known gap. |
| **teapot** | FAIL | 5.900 / 6.079 / 4.949 | The same approximation, on the mipmapped teapot texture (raster/pixels reports). |
| texcyl | PASS | 0.000 / 0.004 / 0.000 | |
| isosurf | PASS | 0.035 (1) | |
| **testgl** (SDL 1.2) | FAIL | 0.077 / 0.237 / **1.895** | Frames 3 and 20 PASS. At frame 60 one face of the cube has a visible seam in ours and none in Mesa. The cause is **not established**; the evidence is in 3.3. |
| **testgl2** (SDL2) | ERROR | - | `INFO: Could not load GL functions`. testgl2 requires every function in SDL2's `SDL_glfuncs.h`, including glBlendEquation and glBlendFuncSeparate. Plan 4.2 withholds exactly those two as the SDL2 regression guard, so our glXGetProcAddress returns NULL for them (by design, and core_test checks it). See 3.2. |
| **rrootage** | MISSING-SYMBOL (image FAIL) | 8.522 / 8.164 / 8.507 (3/60/300) | The harness counts any `libGL: unimplemented` line as MISSING-SYMBOL; here it is `glEnable(GL_LINE_SMOOTH)`. The game draws almost everything as antialiased lines with additive blending. See 3.4. |

- **Determinism.** Mesa-vs-Mesa (`--impl mesa`) was run twice after the harness changes (`phase2/selftest-mesa`, `phase2/selftest-mesa-2`): all 21 image apps EXACT and glxinfo PASS, both times. Ours is also repeatable: a second run of testgl, testgl2, rrootage, glxgears and manywin gave identical percentages on every frame.
- **The harness changes did not move existing references.** When the reference stamp changed, all 57 existing Mesa reference images and statuses were regenerated, and every one came out byte-identical. Only the logs differ, and only in pids.
- **Unchanged outside the suite.** Every one of the 19 phase-1/F7 apps has the same verdict and the same percentages as `f7/suite`.

## 3. The stage-5 corpus (new: `tools/glref/build-games.sh`)

Sources are the research scratchpad's tarballs, copied to `gl/ref-apps/` (md5: SDL-1.2.15 `9d96df84...`, SDL2-2.32.10 `7b234751...`, rrootage-0.23a `c6247480...`, gltron-0.70 `300e5491...`). Each is built by its own build system with no source change. The script runs the three builds side by side, and suite.sh calls it. The knobs are all build-system inputs, and the script's header documents each one:
- `--build=aarch64-unknown-linux-gnu`, because the 2012 config.guess cannot guess aarch64.
- `ac_cv_lib_OpenGL_glBegin=no` for SDL2's test configure, which is what the board has (no glvnd).
- rRootage's make variables overridden on the command line, because its makefile.lin is the MinGW one: it has `-mwindows` and no GL libraries.
- `-fpermissive` and `-include cstring` for its 2003 C++.
- `make clean` first, because the tarball ships x86 objects.

### 3.1 SDL arms test sdl12-compat, not SDL 1.2.15

The rig's `libsdl1.2-dev` is **sdl12-compat 1.2.68 over SDL2 2.32.4**. The "SDL 1.2" arm therefore exercises SDL2's GLX loader (dlopen libGL.so.1, then dlsym and glXGetProcAddressARB), not SDL 1.2.15's `SDL_x11gl.c`, which is what the board will run. I tried building the board's SDL 1.2.15 in the rig, with Buildroot's four patches applied. It fails on this LP64 host: `_XData32` conflicts with Xlibint.h's LONG64 prototype. So **the SDL 1.2.15 GL path is untested on the host.**

Two harness fixes were needed before any SDL app could be compared (section 4):
- the capture shim interposes `dlsym`;
- run.sh sets `SDL12COMPAT_OPENGL_SCALING=0`.

Without the second one, sdl12-compat on Mesa (which has FBOs; ours does not) swapped once inside `SDL_SetVideoMode`. Every Mesa frame then ran one swap and 16 ms of virtual time ahead of ours. That alone had testgl at 1.6 / 1.9 / 4.5% and made rRootage's game state diverge.

### 3.2 testgl2 and the plan 4.2 blend guard (for the plan owner)

- **Cause.** testgl2 loads its GL functions through `SDL_GL_GetProcAddress` and quits if any one of SDL_glfuncs.h's 48 is NULL. With ours, exactly two are NULL: glBlendEquation and glBlendFuncSeparate. Plan 4.2 says both stay unexported "until the SDL2 gate has been re-run with them", and gl/api/mkstubs.py and core_test enforce that. **So testgl2 (a stage-5b target) cannot run while the guard stands.** I left the guard in place. The decision is the plan owner's.
- **Experiment** (`artifacts/gl/phase2/exp-blendeq/`; the source copy is in `gl/ref-apps/build/beq-gl`, gitignored). This is a variant libGL, not shipped:
  - it exports both functions;
  - glBlendEquation has the full ADD / SUBTRACT / REVERSE_SUBTRACT / MIN / MAX in the general-path blend stage;
  - glBlendFuncSeparate is glBlendFunc of the RGB pair. That is exact here: there is no alpha plane, so the alpha factors only ever produced a destination alpha that nothing stores.
- **Result.** testgl2 **PASSes: 0.005 / 0.048 / 0.000%**.
- **What this means for 4.2.** The gate this choice needs is plan 4.2's SDL2 regression set. The experiment shows only that testgl2 itself would be correct.

### 3.3 testgl (cause not established)

- **Symptom.** At frame 60 one face of the cube has a visible seam in ours and none in Mesa. Frames 3 and 20 PASS.
- **A different split makes it pass, and breaks two other tests.** With GL_QUADS split 0-1-3 / 1-2-3 instead of our 0-1-2 / 0-2-3, testgl PASSes all three frames (0.008 / 0.094 / 0.723%). The same change breaks glx_pixels page 3 (0.05% → **18.8%**) and glx_prims (0.013% → 1.04%) (`exp-quad013/regressions/`). I reverted it.
- **Mesa uses our split.** A direct probe (`gl/ref-apps/build/diag/quadsplit.c`) shows Mesa llvmpipe splitting every quad along the 0-2 diagonal, the same as ours. The probe covered:
  - a single quad, and the 1st, 2nd and 7th quad of one glBegin;
  - a 3D planar quad with varying z;
  - a clockwise quad;
  - depth test on.
- **So the split is not the cause.** Something else differs on that one face, and it needs a look from the raster owner. The next step: dump the per-vertex colours ours computes for testgl's cube at frame 60 (it rotates about the (1,1,1) axis, the path of the F7 glRotate fix) and compare them with Mesa's feedback-mode values.

### 3.4 rRootage 0.23a

- **Stack overflow in the stock source.** screen.c `loadGLTexture()` builds `"/usr/share/games/rRootage/" + "images/" + name` in `char name[32]` with strcpy and strcat. It is 33 or more bytes, and ASan reports a stack-buffer-overflow at the first texture. At -O1, -O2, -O3 and -Os the game dies with SIGBUS (a jump to an odd PC) **under Mesa and ours alike**. At -O0 the overflow lands on dead stack and the game runs. The harness uses the -O0 build: both libGLs run the identical binary, so the A/B stays fair.
- **For stage 5a.** The Buildroot package needs Debian's patch or an equivalent. Plan rule 0 allows "only compiler compatibility flags", and at the board's -Os this overflow is undefined behaviour that crashed every optimised build here. The data path is compiled in, so the rig image carries `/usr/share/games/rRootage -> /src/gl/ref-apps/rrootage-0.23a/rr_share`.
- **Image difference, attributed** (`exp-nosmooth/`). rRootage enables GL_LINE_SMOOTH with `glBlendFunc(GL_SRC_ALPHA, GL_ONE)` and draws nearly everything as lines. Smooth lines are unimplemented in ours (F7 report: "smooth points/lines/polygons" deferred). A diagnostic preload that makes Mesa ignore `glEnable(GL_LINE_SMOOTH)` cuts the difference from 8.5 / 8.2 / 8.5% to **3.6 / 3.5 / 3.8%**.
- **What remains after that** is known, documented raster behaviour:
  - line placement: TinyGL keeps the inclusive last pixel;
  - that doubled endpoint pixel, added twice under additive blending;
  - GL_LINEAR drawn as nearest on the title logo.
- **For the plan.** rRootage is the first real game, and the plan's feature matrix lists "0 functions missing". It needs **smooth lines (or at least wide, coverage-alpha lines)**, which is not in the F1-F7 table.

### 3.5 GLtron 0.70 (skipped)

Its own configure works, given `--disable-warn` (its default adds `-Werror`) and `CC="gcc -include stdint.h"` (its Lua copies `#define __USE_MISC` under `-ansi`, which breaks glibc's `unistd.h`). The build then stops at:

`nebu/scripting/scripting.c:172: error: conflicting types for 'scripting_RunFile'; have 'void(char *)'`

The header declares `const char *`. No flag turns that error off, so a source patch is needed, which this task rules out. **Stage 5a's GLtron package needs a patch.**

## 4. Harness changes (tools/glref, mine)

| file | change | why |
|---|---|---|
| `capture.c` | Interposes `dlsym`. An app asking any handle for a hooked GLX name (SwapBuffers, MakeCurrent, MakeContextCurrent, CreateWindow, GetProcAddress/ARB) gets the hook, and the handle is kept for the real lookup (`next_sym`). Lookups made by GL, X or libc libraries are forwarded unchanged. A foreign `RTLD_NEXT` walks the link map through `dlopen(RTLD_NOLOAD)` handles. | SDL loads GL itself, so the shim saw no swaps. The first version passed bare link_maps to glibc's dlsym and crashed on any second preload (reproduced with a probe; fixed and re-tested). |
| `capture.c` | `GLREF_TRACE` also logs every virtual-time advance with the library that slept. | That trace is how the sdl12-compat extra swap was found. |
| `run.sh` | The shim is built to `mktemp` names, not `$SHIM.$$`. | Two containers building the shim at once had the same pid and raced (`mv: cannot stat capture.so.1`). |
| `run.sh` | Sets `SDL12COMPAT_OPENGL_SCALING=0` for both arms. | 3.1. |
| `apps.txt`, `suite.sh`, `xlite-load.sh` | Add testgl, testgl2 and rrootage (`$SB` = gl/ref-apps/build). suite.sh builds and stamps them. | Stage 5a/5b targets. |
| `xlite-load.sh` | Imports that only the rig's own libSDL2 makes (Xdbe\*, Xutf8\*) are listed but not counted. | The rig's SDL2 links libX11 directly. The board's SDL2 2.32.10 is `SDL_VIDEO_DRIVER_X11_DYNAMIC` (checked in the Buildroot build's SDL_config.h): it dlopens libX11 and libXext at run time, so these are not load-time imports there. |
| `build-games.sh` (new), `Dockerfile` | The corpus build. A separate image layer adds libsdl-mixer1.2-dev and libpng-dev, plus the rRootage data link. The Mesa layer is unchanged (libgl1-mesa-dri still 25.0.7-2+deb13u1, and the reference stamp is still valid). | 3. |
| `README.md` | Documents all of the above. | |

## 5. Performance guard (`gl/bench/bench.sh`: qemu-system-riscv32 icount, the board's Buildroot flags, deterministic)

I ran it on the baseline tree (`gl/bench/base`, git f522dad, before F3) and on the current tree side by side.

| demo | baseline M insn/frame | now | delta | guard (baseline +1%) | fb hash now |
|---|---|---|---|---|---|
| gears 320x240 | 2.6077 | 2.6210 | +0.51% | <= 2.6338 ok | bc8b3ce6 |
| gears 640x400 | 3.7056 | 3.7179 | +0.33% | <= 3.7427 ok | cf6b6d75 |
| texobj 320x240 | 0.4107 | 0.4143 | +0.88% | <= 0.4148 ok | ed495265 |
| texobj 640x400 | 1.2688 | 1.2739 | +0.40% | <= 1.2814 ok | 1e49bce8 |
| teapotf 320x240 | 10.1608 | 10.2015 | +0.40% | (not guarded) | ccac18a5 |
| teapotf 640x400 | 10.9404 | 10.9800 | +0.36% | (not guarded) | 06347bc5 |

- **Soft-double calls per frame** are unchanged: 4624, 96 and 23888.
- **The frame hashes changed from the baseline** for the documented correctness fixes: strict GL_LESS, 1/w perspective, the glRotate transpose. The numbers are the pixels report's after-F7 values, to the last digit.
- **The benched binary.** The `q_gears_320.elf` image's `.text` is 113,506 → 147,474 B. That is the whole library linked with --gc-sections, which removes nothing reachable through the dispatch.
- **Where the extra cost sits.** Feature costs (`feat.sh`, gears / texobj at 320x240):
  - fog: 5.87 / 2.27 M
  - blend ONE,ONE: 5.11 / 2.04 M
  - alpha test: 5.09 / 1.97 M
  - MODULATE by white: 2.62 / 0.41 M (tier 1)
  - blend SA,1-SA: 5.45 / 2.23 M
- **Pixel paths** (`pix.sh`): the glyph and pixel-path numbers are identical to the F7 report, e.g. 1.892 M for 800 bitmap glyphs and 1.758 M for a 320x240 glReadPixels.
- **The general path costs 2-5x tier 1.** This is the known lever G09.

**These are instruction counts, not board time.** There is no PSRAM, I-cache or XIP model.

## 6. RV32 library: size, exports, soft double

The build is `gl/build.sh`, plus a simulated Buildroot mode: `S31GL_OUT`, TARGET_CFLAGS `-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base`, `--sysroot`, unstripped.

| item | value |
|---|---|
| /src/images/libGL.so.1 | 226,820 B stripped, md5 77199f7e2e0915061b012113b3d0b8d5 |
| .text | **144,066 B** in Buildroot mode, 144,120 B in plain mode. Before F3 it was 105,906, after F3-F6 127,240, and the F7 report gave 143,970; the 96 B gap to that figure was not traced. |
| .rodata / .data.rel.ro / .data / .bss | 21,292 / 4,556 / 40 / 6,080 |
| SONAME, NEEDED | libGL.so.1; libXext.so.6, libX11.so.6, libc.so |
| exports | 724 dynamic: **667 gl\*** (455 implemented, 212 stubs), **44 glX\***, **13 s31gl_\***. Nothing else leaks. glBlendEquation and glBlendFuncSeparate are absent (plan 4.2). |
| X imports | 19: XAddExtension, XCheckIfEvent, XCreateGC, XCreateImage, XESetCloseDisplay, XFlush, XFree, XFreeGC, XGetGeometry, XGetVisualInfo, XPutImage, XSetErrorHandler, XShm{Attach, CreateImage, Detach, GetEventBase, PutImage, QueryExtension}, XSync. All are defined in xlite's libX11. |
| soft-double call sites | 17 __extendsfdf2, 14 __truncdfsf2, 4 __divdf3, 3 __muldf3, 2 __floatsidf, plus pow x3, sincos x3, cos x2, sqrt x2. |

**Which functions call them:**
- glopRotate 12
- specbuf_get_buffer 8
- glopLight 5
- gl_M4_Rotate 4
- s31_d2f_bits 3 (the special-value fallback)
- glPopAttrib 3
- glGetDoublev 2
- one each in glGetClipPlane, glGetTexGendv, gl_print_op, and arrays.c `fetch`

`fetch` is the GL_DOUBLE vertex-array conversion's special-value fallback. **None is on a float per-vertex or per-pixel path.**

**xlite RV32** (from xlite/build.sh, unchanged by me):
- libX11.so.6.4.0: 116,264 B, .text 52,054, md5 f287f618...
- libXrandr.so.2.2.0: 9,480 B, md5 26a7d942...
- libXxf86vm.so.1.0.0: 9,472 B, md5 8ee0700e...

These match the xlite report.

## 7. Other checks (all host or qemu, all re-run after the final revert)

| check | result |
|---|---|
| core_test, host | 185 passed, 0 failed |
| core_test, RV32 qemu-user | 183 passed, 0 failed (2 dlsym checks are skipped when static) |
| d2f_test, RV32 qemu | 49,987,811 values, 0 mismatches |
| headless_gears 320x240x100 md5 | aeb114d6305ec92f487684aa2a761a0f, host and RV32 qemu (same as the raster/pixels reports) |
| ASan+UBSan (run-san.sh: raster 1-6, pixels 1-4, glu 1-3, prims, core_test, headless_gears) | clean |
| run-raster.sh (phase2/raster) | pages 1, 2, 3, 5, 6 PASS. Page 4 FAILs at 1.598%: per-vertex EXP fog and minified nearest textures, the same as f3f6. The page-6 glGet log differs in the 2 expected lines: 512x512 gives INVALID_VALUE, and an unspecified level's format is 1. |
| run-pixels.sh (phase2/pixels) | p1-p4 PASS (0.069 / 0.611 / 0.051 / 0.518%), logs identical. g1/b1 0.098%, g2/b2 0.072%, g3 NURBS FAIL (expected). |
| run-prims.sh | PASS 0.013% |
| run-sysgears.sh (Debian glxgears) | PASS 0.020 / 0.034 / 0.030% |
| gl/glx/test/run-host.sh | MIT-SHM, NOSHM and SHMBUFS=2: ALL PASS |
| glx-harness run-xlite.sh (xlite as libX11) | both arms ALL PASS |
| xlite/test/run-host.sh (seqtest) | 23/23 PASS, run as a cross-check |
| xlite/test/rv32-linkcheck.sh | PASS. libglut, libXi, libXfixes, glxgears, glxgears_fbconfig, glxheads, manywin, multictx, gears and glxinfo all resolve against /src/images. |

## 8. Open, for owners

1. **Plan owner, 4.2 against 5b:** testgl2 cannot run while glBlendEquation and glBlendFuncSeparate are withheld. Either re-run the SDL2 regression gate with them exported (the experiment says testgl2 is then correct), or drop testgl2 as the 5b correctness app.
2. **Stage 5a packaging:**
   - rRootage needs a source patch: the `name[32]` overflow crashes every optimised build.
   - GLtron needs a source patch: the conflicting `scripting_Run*` types.
   - Both contradict plan 3's "only compiler compatibility flags". Debian carries patches for both.
3. **Raster owner:**
   - GL_LINE_SMOOTH (rRootage), not in the F table.
   - The testgl frame-60 face (3.3; cause open).
   - GL_LINEAR and mipmap filtering (fire, teapot).
4. **xlite owner:** xlite/build.sh passes the sysroot as `-I`, so musl's `endian.h` produces 27 `-Wparentheses` warnings; `-isystem` would silence them. The owner's own note stands: host-only (LP64) XGetWMHints heap overflow.
5. **Not verified here:** SDL 1.2.15's own `SDL_x11gl.c` path (3.1); anything on the board.
6. **Out of my directories, still stale:** docs/gl-plan-2026-09-25.md (sections 2.2 and 6 still describe F3-F7 as missing) and artifacts/gl/phase1/INTEGRATION.md. s31-libgl.mk's rsync exclusions should add `/bench/base`, `/bench/out*`, `/out-san` and `/ref-apps`.

## 9. Commands

```
gl/host-build.sh & gl/build.sh & ./docker/build.sh 'sh /src/xlite/build.sh' &   # in parallel
tools/glref/build-games.sh                          # stage-5 corpus (suite.sh runs it)
tools/glref/suite.sh --run phase2/suite --jobs 8    # 22 apps, ~10 s with cached references
tools/glref/suite.sh --impl mesa --run phase2/selftest-mesa --jobs 8
gl/tests/run-pixels.sh /src/artifacts/gl/phase2/pixels   # + GLU g1-g3, b1-b2
gl/tests/run-raster.sh /src/artifacts/gl/phase2/raster
gl/bench/bench.sh gl gl/bench/out-p2; gl/bench/bench.sh gl/bench/base gl/bench/out-p2base
```

Artifacts in this directory:
- `suite/` (report.md with images and the load arm)
- `selftest-mesa{,-2}/`
- `pixels/`, `raster/`
- `xlite-load.md`
- `exp-blendeq/` (3.2), `exp-quad013/` (3.3, including `regressions/`), `exp-nosmooth/` (3.4)
- `sbs-testgl.f60.png` (Mesa | ours | diff)

## 10. Review fixes (2026-09-26)

The review's 17 findings (G1-G8, X1, N0, P1-P7) were each confirmed, then
fixed or scoped. Everything here ran on the host rig (s31-glref, Xvfb
800x480x16, Mesa 25.0.7 llvmpipe) or under qemu (system for the bench,
user for the RV32 tests). **Nothing ran on the board.** Probe sources and
outputs: the review's scratchpad `rev/` (re-run with its `go.sh`, which
builds our libGL from a fresh snapshot of /src/gl) and `perfrev/`; my
before/after logs are next to them in `fixrev/`.

### 10.1 What was confirmed and what was done

| id | severity | confirmed | fix | evidence after |
|---|---|---|---|---|
| P1 / G1 | blocker / major | yes: probe6 EQUAL-after-LESS 30 of 2879 px drawn, probe5 depth mean -39, perfrev/mp.c 37.9% / 98.3% missing | **One scan conversion for both paths** (`gl/tinygl/source/ztri.h`): GL window coordinates (new per-vertex zp.fx/fy), pixel centres, top-left rule, 16.16 edges stepped from each edge's own first row, one integer depth plane per triangle; the float setup uses explicit fmaf so no inlined copy can contract differently. Tier 1's per-pixel loops are unchanged; the general filler takes its spans and depth from the same setup. Also: facing and polygon offset from the float position (the snapped facing test dropped sub-pixel triangles, which now would be cracks), lines and points from the pixel holding the GL position. | probe6 24/24 same as Mesa; probe5 depth B-A 0 everywhere (6880 vs 6882 covered: 2 tie pixels); mp.c and mpt.c 0 missing, 0 outside in all 8 arms; E2-E6 0 cracks, H0/H1 polygon-offset bands all same as Mesa; new gate `gl/tests/raster_gate.c` 51/51 on the host and on RV32 (qemu-user) |
| G2 | major | yes: probe8 8064 wrong texels on tier 1 | squeeze removed, pixel-centre sampling on tier 1 (falls out of ztri.h), and a triangle with a negative REPEAT coordinate is moved by whole periods so the int conversion floors (both paths; not in the review, the same one-texel class for Quake's negative texcoords) | probe8 36/36 same (0 wrong texels, 1:1, atlas cells, 2x); raster_gate texel phase incl. negative coordinates 0 wrong on both paths |
| G3 | major | yes: probe2 page J, gluBuild2DMipmaps 384/512 -> no level 0 | level L limited to 256 >> L for proxies and uploads; proxy remembers level and border | probe2 J: level-1 256 proxy refused, real level-1 256 GL_INVALID_VALUE, 2max/1.5max mipmaps level 0 = 256, border-1 proxy reports 1; glu_check g2/b2 new 512 and 384 lines identical to Mesa (23 lines, 0 differ) |
| G4 | minor | yes | threshold per function from r = ref*255 (GREATER/LEQUAL floor, LESS/GEQUAL ceil, EQUAL/NOTEQUAL only on whole r) | texel cases (TyrQuake GREATER 0.666 with 170, GREATER 0.5 with 128) and vertex a=128/255 cases now match. **Partial:** 4 cells differ the other way - a vertex alpha of exactly 0.5 against ref 0.5 - because fragment alpha is 8 bits (0.5 and 128/255 are both 128). Before: 10 cells wrong; after: 4. Recorded in README.s31. |
| G5 | minor | yes | back material and -n for back-facing polygons (light.c computes both colours; clip.c swaps the back ones in per back-facing triangle, including a flat provoking colour); lighting now runs last in glopVertex so the back colours can share the dead object/eye-coordinate storage (GLVertex does not grow) | probe7 8/8 same; probe4 back-material cell same |
| G6 | minor | yes | even point sizes centred on floor(xw + 1/2) | probe4 size-2 cells same |
| G7 | minor | yes | s and t divided by q per vertex (and q interpolated by both clippers); exact for constant q, approximate for varying q (recorded) | probe4 texcoord4 cell same |
| G8 | minor | yes | TEXTURE_BIT saves/restores min/mag filter, wraps, border colour and priority of the 1D and 2D bindings | probe3: only Mesa's own two deviations remain (1080 same) |
| X1 | minor (host only) | yes | format-32 properties packed/widened as Xlib's Data32/_XRead32 under LP64 (dead code on ILP32); WM_NORMAL_HINTS / WM_HINTS set and read field by field; XSetTransientForHint passes a long | review xtest.c 24/24 same as stock Xlib; new `xlite/test/run-props.sh` (proptest.c: hints, WM_HINTS, transient-for, protocols, a raw format-32 array, read back by xlite and by stock xprop): output identical to stock Xlib. RV32 libX11 builds with **0 warnings** now (build.sh includes the sysroot with -isystem; the three .so files are byte-identical to the -I build) |
| N0 | - | - | the suggested gates were added: probe6/E3/probe8 as raster_gate.c (ours only, exact answers), the 512 mipmap case in glu_check | |
| P2 | major | yes: tg.c 3.5552 -> 3.8502 M (+8.3%) | q = 1/w once per vertex, gl_tex_points and its copies/divides/squeeze gone | prim7 (the review's tg.c) 3.4541 M: -10.3% against the pre-fix tree, -2.9% against the f522dad baseline |
| P3 | major | yes: ~310 instructions a blended line pixel | line pixels gathered into spans through zp_run and scattered back; stipple as a counter; box test hoisted | blended lines 12.47 -> 5.13 M (~125 a pixel), width 2 19.66 -> 8.28 M, y-major 13.73 -> 5.67 M. Still ~12x TinyGL's plain line: a fused line loop is the next step (not done). |
| P4 | minor | yes | foot.sh/foot.py report the executed-code footprint (not gated); headroom recorded | gears 10,208 B / 410 lines, texobj 7,400 B / 314 (baseline 9,652 / 387 and 6,242 / 265). texobj guard headroom 0.07% |
| P5 | minor | yes, all four | (a) wraps.txt + dwrap.c count comparisons and conversions; (b) the baseline tree is compiled with its own flags; (c) noted in BASELINE.md; (d) bench.sh runs feat/pix/prim and fails on limits.txt | baseline texobj 640 1.2687 again; "bench: 34 lines within limits.txt, 0 over" |
| P6 | minor | yes | GLTexture 288 -> 148 B (one stored image, 16-bit lfmt) | **Not done:** opaque-RGBA detection (dropping the A8 plane and letting tier 1 draw it needs a storage-class change when a later glTexSubImage2D brings non-255 alpha) and row-block streaming of the depth/copy pixel paths |
| P7 | minor | yes | one store loop per class, direct UNSIGNED_BYTE RGB/RGBA/LUMINANCE converters, built -O2 | 24.6 / 29.7 / 30.7 -> 17.2 / 12.2 / 14.2 instructions a texel (RGBA / L / RGB) |

### 10.2 Suite (`tools/glref/suite.sh --run phase2/suite-fix`, 22 apps)

Same verdicts as section 2 - 17 PASS; fire, teapot, testgl FAIL; testgl2
ERROR; rrootage MISSING-SYMBOL - with the error lower almost everywhere
(tolerant-bad %, frame 3 / 20 / 60 or the one frame):

| app | before (phase2/suite) | after (phase2/suite-fix) |
|---|---|---|
| glxgears | 0.020 / 0.034 / 0.030 | 0.000 / 0.000 / 0.000 |
| gears | 0.030 / 0.017 / 0.034 | 0.000 / 0.000 / 0.000 |
| geartrain | 0.704 / 0.680 / 0.758 | 0.029 / 0.030 / 0.022 |
| morph3d | 0.099 / 0.141 / 0.195 | 0.034 / 0.016 / 0.025 |
| offset | 0.638 | 0.152 |
| terrain | 0.023 / 0.004 / 0.026 | 0.000 / 0.000 / 0.000 |
| tunnel | 0.350 / 0.350 / 0.219 | 0.288 / 0.324 / 0.188 |
| ipers | 0.186 / 0.190 / 0.177 | 0.173 / 0.164 / 0.156 |
| isosurf | 0.035 | 0.001 |
| teapot | 5.900 / 6.079 / 4.949 | 5.737 / 5.997 / 4.818 |
| rrootage | 8.522 / 8.164 / 8.507 | 8.182 / 7.960 / 8.246 |
| testgl | 0.077 / 0.237 / **1.895** | 0.093 / 0.225 / **1.990** |
| fire | 3.078 / 3.158 / 3.039 | 3.081 / 3.159 / 3.039 |

All others are within 0.01 of before. The xlite load arm: 22 load, 0
missing.

**testgl frame 60, cause now identified (not verified by an experiment):**
the cube's corner colours are its positions, so each face's colour is
affine in object space. Mesa interpolates colour perspective-correctly and
the face is seamless; ours interpolates Gouraud colour linearly in screen
space (TinyGL's, on both paths), and under perspective each triangle's
screen-affine ramp differs, which shows a crease along the split diagonal
of the faces (`review-fixes/tg_mesa.png` against `tg_ours.png`: visible
on the top and right faces). The quad split itself is not it (3.3). A fix
is perspective-correct colour, which costs a divide per span on the
general path and per 8 pixels on tier 1's smooth filler; not done.

### 10.3 Other checks after the fixes

- Host and RV32 builds, 0 warnings in our code (RV32 xlite: 0 at all).
  RV32 Buildroot mode: 0 warnings, .text 149,086 B.
- core_test 185/0 (host), 183/0 (RV32 qemu-user); d2f_test 0 mismatches;
  raster_gate 51/0 on both.
- headless_gears md5 38859f0c896efe6b12bc68e0fe3afe1f on the host and RV32
  (was aeb114d6: every frame changes with the coverage rule).
- ASan+UBSan (run-san.sh, now with raster_gate): clean. It first found a
  signed overflow in ztri.h's host floor on core_test's fuzz (a sliver's
  saturated depth gradient); the host fallback saturates now and the
  depth plane arithmetic is unsigned.
- run-raster.sh (`review-fixes/raster/`): pages 1, 2, 3, 5, 6 PASS; strict
  error 0.42 / 2.23 / 1.52 / 3.36 / 6.46% before -> 0.00 / 0.04 / 0.00 /
  4.55 / 1.66% after. Page 4 FAILs at 1.724% tolerant-bad (was 1.598):
  per-vertex EXP/EXP2 fog, as before, now mostly on the fogged-line cells;
  its polygon-offset and depth-range rows are clean where they had
  tolerant-level diffs. Page 5 (lines, points) is worse in the strict
  column (3.36 -> 4.55%) and the same tolerant (0.059 -> 0.066%): line ends
  now come from the GL position (Bresenham, not Mesa's diamond rule).
- run-pixels.sh (`review-fixes/pixels/`): p1-p4, g1-g2, b1-b2 PASS, all
  logs identical to Mesa; g3 (NURBS) FAILs as before.
- glx_prims PASS; GLX over xlite (phase1 run-xlite.sh) ALL PASS in both
  buffer modes; xlite seqtest 23/23; run-clients PASS; rv32-linkcheck PASS.
- `gl/tests/run-raster.sh` and `run-pixels.sh` write to
  `artifacts/gl/f3f6/raster` and `f7/pixels` by default, so those two
  (untracked) directories now hold the post-fix runs; the pre-fix runs
  are this directory's `raster/` and `pixels/`.

### 10.4 Performance guard (gl/bench, qemu instructions; details in gl/bench/BASELINE.md)

The harness changed (wider soft-double counter, pinned baseline flags), so
the baseline was re-measured with it.

| demo | baseline f522dad | after | delta | guard |
|---|---|---|---|---|
| gears 320x240 | 2.6341 M | 2.6152 M | -0.72% | ok |
| gears 640x400 | 3.7321 M | 3.7196 M | -0.33% | ok |
| texobj 320x240 | 0.4107 M | 0.4145 M | +0.93% | ok (0.07% headroom) |
| texobj 640x400 | 1.2687 M | 1.2733 M | +0.36% | ok |
| teapotf 320x240 | 10.3125 M | 10.3917 M | +0.77% | not guarded |

Soft-double call sites: identical to the pre-fix tree in every object.

### 10.5 Still open

- Not on the board: no fps, RSS, I-cache or XIP numbers. The fcvt
  rounding-mode asm in ztri.h ran under qemu-user and qemu-system only.
- Tier-1 per-triangle setup is ~100 instructions dearer than TinyGL's
  (prim4 +2.3% for 10x20 px ortho quads); gears and textured meshes came
  out cheaper overall for other reasons (above).
- G4's exact-0.5 vertex alpha, G7's varying q, P3's fused line loop, P6's
  opaque RGBA and pixel-path streaming, testgl's perspective colour.
- Tie-breaking: a pixel centre exactly on a diagonal edge can go to the
  other triangle than Mesa's (probe4 "flat trifan", a flat-shaded fan
  whose triangles differ in colour); both of our paths agree.
- docs/gl-plan-2026-09-25.md (outside the fixer's directories) still
  describes F3-F7 as missing. artifacts/gl/phase1/INTEGRATION.md is left
  as the phase-1 record; this section and gl/bench/BASELINE.md supersede
  its numbers.
