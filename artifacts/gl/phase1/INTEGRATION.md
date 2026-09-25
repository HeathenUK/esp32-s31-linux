# GL phase 1 integration report (2026-09-25)

The integrator's report on four components built in parallel: the core (gl/api, gl/tinygl), GLX (gl/glx), the glref harness (tools/glref) and the Buildroot packaging (buildroot-external/package/s31-libgl). All of it was tested on the host. **Nothing ran on the board**, because the board was off-limits for this task, so there are no board timings, RSS or fps figures anywhere in this report.

## Result

| goal | status |
|---|---|
| 1. One libGL.so.1 from both builds, no warnings | **done.** `gl/host-build.sh` and `gl/build.sh` both build clean: `grep -ci warning` on the logs gives 0 (int/build-host.log, int/build-rv32.log). gl/glx now includes gl/api/s31gl.h, and nothing else, for the core API. |
| 2. Stock glxgears, both builds, matches Mesa at 3 frames; glxinfo sane | **done.** Details below. |
| 3. Suite over all targets, failures classified | **done.** 9 PASS and 9 feature gaps. No crash, no missing symbol, no GLX failure. |
| 4. RV32: /src/images/libGL.so.1, SONAME, NEEDED, exports, headless_gears, .text | **done.** |
| 5. libGL's X imports checked against xlite | **done.** All 19 exist in the board's `images/libX11.so.6.4.0`. The apps' own gaps are listed below. |

**Second pass:** the review's findings and what was done about each are in "Review fixes" directly below. Where a statement further down was wrong, it is marked *(corrected)*.

## Review fixes (second pass, 2026-09-25)

A review (probes in `~/.cache/s31-glreview/`: atk.c, abi.c, run-atk.sh) raised 19 findings. Each was checked in code or reproduced before it was changed. Everything below ran on the host rig: stock Xlib and xlite-for-host under Xvfb depth 16, the RV32 cross build, and qemu-user. **Nothing ran on the board.**

### Verdicts

| id | verdict | what was done / why not |
|---|---|---|
| interval0-shows-unswapped-frame (major) | **fixed** | The interval no longer changes pacing; every put asks for its completion and the next frame's first write waits for it (glx_present.c). There is no vblank to skip, and interval 0 never licenses showing unswapped pixels. `S31GL_NOWAIT=1` keeps the old no-wait behaviour as a debug toggle only. glxtest `interval0`: 0 of 50 bad frames. Negative control with `S31GL_NOWAIT=1`: 48 of 50 bad, so the test catches it. The review's own probe (IV=0, 200 trials) passes. |
| viewport-memory-misses-resize-back (major) | **fixed** | A size change now empties the rectangle memory, at glViewport and at MakeCurrent. Found and fixed in the same code: a rectangle arriving after the frame's one query was remembered *unchecked*, so a resize first reported through it was never seen. Now only rectangles that were really checked are remembered. glxtest `vpmem` (160x120 -> 320x240 -> 160x120): all 3 steps pass on both libX11s. |
| error-trap-steals-app-errors (major) | **fixed** | The trap records `NextRequest` at `glxi_trap_begin(dpy)` and claims only errors on that display with serial >= it (16-bit wrap compare: xlite's serials are raw wire values). Older errors are chained to the handler the app installed. It covers glxi_query_geometry and alloc_shm. glxtest `trap`: MakeCurrent succeeds, and the resize is seen through glViewport, with an app BadMatch in flight. The app's handler gets each error exactly once, on both libX11s. atk `trap` and `trapshm` pass. |
| prboom-plus-gl-never-gets-a-visual (major, plan) | **deferred: plan owner** | Confirmed by the review's `visuals` probe. This is a plan decision, and docs/ is not mine. Recommendation: record next to the F8 row that prboom-plus GL needs stencil 8 (SDL/i_video.c:1289, unconditional) and depth 24 by default (m_misc.c). Either implement F8 stencil before stage 6 or drop prboom-plus GL from stage 6. Reporting depth 24 or stencil 8 without the buffers would break the plan's "honest" rule. |
| slow-caveat-with-visual-rating (minor) | **fixed** | `GLX_EXT_visual_rating` is no longer advertised (glx.c). Configs still report `GLX_CONFIG_CAVEAT = GLX_SLOW_CONFIG`, which is honest. With the extension gone, neither SDL adds `GLX_VISUAL_CAVEAT_EXT`, so SDL_GL_ACCELERATED_VISUAL=0 and =1 both get a visual. Reporting NONE, the review's first option, would have broken SDL2's accelerated=0, which asks for SLOW. glxtest checks the string. **Plan 4.1's extension list needs the same edit** (docs/ not mine). |
| depth-buffer-per-context (minor) | **fixed** | The depth buffer is the drawable's now. `glxi_surf.depth` is calloc'd with the colour buffer, and every context current on the window binds it through the new core call `s31gl_bind_depth` (gl/api/s31gl.h). The core never frees caller-owned depth (`zbuf_ext`). glxtest `depthshare`: 0x07e0 (Mesa's answer; it was 0xf800). This also saves w*h*2 per extra context on one window. |
| **new, found while fixing the above** | **fixed** | glx_core.c called `s31gl_set_doublebuffer` and `s31gl_release_depth` only under `#ifdef S31GL_IFACE_FROM_CORE`. The integration's s31gl_iface.h change stopped defining that macro, so **both calls were silently compiled out**. GL_DOUBLEBUFFER read 1 and GL_DRAW_BUFFER read GL_BACK for single-buffered contexts, and a context's depth buffer was never released at unbind. The #ifdefs are gone. glxtest now checks GL_DOUBLEBUFFER 0 / GL_DRAW_BUFFER GL_FRONT on a single-buffered context. |
| context-hop-reallocates-every-switch (minor) | **deferred** | Reproduced on the host (atk `hop`): 14.0 X requests and 0.525 ms per two-window frame, against 4.0 and 0.201 ms for `nohop`. Not fixed, for two reasons. (1) Keeping buffers past the last unbind means keeping a GC, a SHM attach and a Visual of a Display that xlite's no-op `XESetCloseDisplay` cannot tell us has closed. That is exactly the use-after-free the integration removed (glx_reopen), and a new Display can reuse the old address. (2) It costs RAM for a pattern no target uses: manywin, glxheads and freeglut keep one context per window. Revisit if an app on the target list hops. |
| multictx-not-gated (minor) | **fixed** | meson does define multictx; tools/glref/build-apps.sh simply did not list it. It is added to XDEMOS and to apps.txt (window, frame 20), commented as the F6 scissor gate. Result: 58.96% tolerant-bad, FAIL as an image. The expected gap: GL_SCISSOR_TEST, GL_BLEND and glPolygonStipple are unimplemented. Mesa-vs-Mesa gives EXACT. |
| tyrquake-sdl-needs-create-context (minor) | **deferred: not my tree** | glXGetProcAddress("glXCreateContextAttribsARB") is NULL by design (plan 4.1), so the SDL2 video target (vid_sgl.c:126 sets the COMPATIBILITY profile mask) cannot create a context. The board's x11 target (vid_glx.c) is unaffected. The plan's stage-6 row should say "build tyr-glquake with VID_TARGET=x11". |
| silent-stubs-in-gate-apps (minor) | **fixed for Push/PopAttrib** | New gl/api/gl_pushattrib.c implements glPushAttrib/glPopAttrib and glPushClientAttrib/glPopClientAttrib from the library's own glGet* and setters. It covers every group the core records: lighting with light positions restored in eye coordinates through an identity modelview, current colour without a GL_COLOR_MATERIAL side effect, and so on. GL_ATTRIB_STACK_DEPTH and GL_CLIENT_ATTRIB_STACK_DEPTH report the depth, and overflow and underflow set the GL errors. Inside glNewList it is ignored with one "unimplemented" line, because gets and sets cannot be compiled into a list. Callers: SDL 1.2 SDL_GL_Lock (GL_ALL_ATTRIB_BITS), SDL testgl, freeglut menus. core_test gains 7 checks (125 pass). glRasterPos*, glTexGen*, glPixelTransfer*, glTexImage1D and the evaluators are still stubs (F7/F8). |
| ubsan-signed-shifts (minor) | **fixed, and more** | Both reported sites are fixed: zbuffer.c memset_16 is unsigned, and ztriangle.h edge slopes use `* 65536`. A fresh UBSan run found more, all fixed the same way: zline.h colour steps; the ztriangle.c smooth filler's packed-colour shifts; and **signed overflow** in ztriangle.h's z edge steps (`dzdy + dzdx * dxdy_min`, `z1 += dzdl_*`) on steep edges, now wrapping unsigned arithmetic. The wrapped sums are the true values, so the bits do not change. UBSan is now clean on headless_gears, core_test, glxtest, glx_prims and 7 atk probes. ASan is clean on those plus share, destroywin, manyctx and hop. headless_gears output is bit-identical before and after (md5 181ba4b2...). |
| review-coverage (minor) | **fixed** | interval0, vpmem, trap and depthshare are folded into gl/glx/test/glxtest.c (`review_tests()`). All four run in every arm: stock Xlib x3 and xlite x2. interval0 is skipped on xlite because its pixel reads need XGetImage. |
| R1-xlite-xsync-per-frame (major) | **partly confirmed; the fix is xlite's** | Confirmed in code: xlite sets `last_request_read` only in `_XReply` (xlite_int.c:265). Its `XSync` (xlite.c:1119) and its event reads never advance it, so `LastKnownRequestProcessed` cannot prove a put when the app ate the completion. **The fix belongs to the xlite owner:** advance `last_request_read` in XSync and from every event and error serial read off the wire. **Not reproduced:** the claim that under stock Xlib glxgears needs 0 XSync. It is timing-dependent. Debian glxgears at 300x300 on stock Xlib here logs, per 41,996 presents at SHMBUFS=1: `0 event, 0 serial, 41,995 XSync`. At SHMBUFS=2, per 75,790 presents: `1,402 event, 65,856 serial, 4,265 XSync`. At 5k fps the app draws before the server's copy has finished. The review's figure came from a strace'd, slower run. libGL now also remembers the serial its own round trips proved (`glxi_dpy.proven`, a new "earlier-round-trip" count in the S31GL_TRACE line). Measured honestly, it fired 0 times in these loops, because the drain after an XSync already clears both SHMBUFS=2 buffers. It is a safety net for a completion eaten after one of our round trips, not a win. SHMBUFS=2 stays the board A/B. |
| R2-clipcode-soft-double (major) | **fixed, and more** | `CLIP_EPSILON` is `1E-5f` (zgl.h). objdump of the RV32 library confirms it: glopVertex and gl_draw_triangle_clip have **no** df libcall now. gl_transform_to_viewport's `1.0/W` and the fillers' `1.0/fz` were already `fdiv.s`, so the integration named the wrong sites. Also fixed, being trivial and on the per-vertex **lighting** path that glxgears uses: gl_shade_vertex's `sqrt` x2 -> `sqrtf`, `1.0` -> `1.0f`, `1E-3` -> `1E-3f` and `pow` -> `powf` (spot lights only). It had 6 df calls and now has none. headless_gears is bit-identical, host and RV32 (qemu). Left for G01, none of them per vertex: glopRotate/gl_M4_Rotate (per glRotate call), glopLight (per glLight call), specbuf_get_buffer (per shininess cache miss), glPopAttrib (glClearDepth/glDepthRange take GLdouble), glGetDoublev (API). |
| R3-warn-once-hot-scan (minor) | **fixed** | error.c first_time() makes a pointer-equality pass over every slot before any strcmp. glopBegin's MODULATE warning and s31_cap_record's per-capability warning now test a static flag or bitmask first, so a warned call site never scans again. |
| R4-viewport-cache-2 (minor) | **fixed** | Viewport memory is 8 rectangles (GLXI_NVP). With "remember only checked rectangles", a 3-viewport app converges to zero queries in 3 frames. The alternative, skipping any rectangle inside the known window, was **rejected**: a shrink arrives as a sub-rectangle. An app with more than 8 distinct rectangles a frame (glx_prims has 30) still pays one query per frame, capped as before. |
| R5-repo-hygiene (minor) | **partly** | New `gl/.gitignore` ignores `/out-host/` and `/out-rv32/` (checked with `git check-ignore`). **Not done, outside my directories:** the 188 MB aarch64 core dump at the repo root (`core`, from /tmp/t, 21:24) is still there and not ignored, and I did not delete it. The root .gitignore and s31-libgl.mk's rsync excludes are the packaging owner's. |
| R6-frame-pointer-hot-fillers (minor) | **fixed (rasteriser only)** | `-fno-omit-frame-pointer` now applies only to the API and GLX objects; the tgl_* rasteriser objects drop it. `S31GL_TGL_FRAMEPTR=1` restores it for a board A/B. RV32, same source: .text 110,504 -> **103,946 B (-5.9%)**. Per function: ZB_fillTriangleSmooth 1,828 -> 1,502, ZB_fillTriangleFlat 1,124 -> 958, ZB_fillTriangleMappingPerspective 1,692 -> 1,554, gl_shade_vertex 1,278 -> 1,070, glopVertex 2,028 -> 1,916, gl_draw_triangle_clip 2,584 -> 2,286. The board A/B (5 fresh boots) was not run: no board. |
| R7-harness-scope-and-report-accuracy (minor) | **fixed** | New tools/glref/xlite-load.sh, run by suite.sh for `--impl ours` and appended to report.md. It resolves every app, and the libglut/libGLU it loads, with `ldd -r` (every relocation, as musl binds) against our libGL + xlite libX11 + xstubs libXext + xlite's libXrandr/libXxf86vm, all built for the host from the repo. Result: **2 load (glxinfo, offset), 17 MISSING-SYMBOL**, every one an xlite or stub gap, none in libGL (table below). The two INTEGRATION statements are corrected here: see R1 and R2. |

### New suite results (`tools/glref/suite.sh --run phase1/suite-review`)

- **19 apps** (multictx added): **9 PASS**, the same 9 as before (glxgears, glxinfo, glxheads, offset, glxgears_fbconfig, gears, morph3d, bounce, geartrain).
- 10 feature gaps, shown as MISSING-SYMBOL because the harness counts `libGL: unimplemented` lines that way (`unresolved=` is empty for all): manywin, multictx, spectex, ipers, terrain, tunnel, fire, teapot, texcyl (image PASS), isosurf (image PASS).
- **Every one of the 48 frames shared with the previous run has the identical verdict, strict % and tolerant %.** The only difference is the new multictx f20 (58.962%). The review changes altered no pixel of any suite app.
- Mesa-vs-Mesa self-test after the tools/glref changes (`--impl mesa --run phase1/selftest-mesa-review`): 18 EXACT and glxinfo PASS.
- Rules test (gl/tests/run-prims.sh): PASS, tolerant 0.013% (unchanged). Debian glxgears (run-sysgears.sh): PASS at frames 3/20/60, tolerant 0.021/0.047/0.049%.
- xlite load arm, `suite-review/xlite-load.md`:
  - glxgears, glxheads, manywin, multictx, glxgears_fbconfig: XSetNormalHints, XSetStandardProperties.
  - every GLUT demo, via libglut.so.3: XGetEventData, XFreeEventData, XGetPointerMapping, XGetWMName, XStoreColor, XRRConfigTimes, XRRSetScreenConfig.
  - via the host's libXi, standing in for the board's: _XData32, _XRead32, _XUnknownNativeEvent.

### Other checks after the fixes

- core_test: host 125/0; qemu RV32 123/0 (the 2 dlsym checks are skipped in a static build). d2f_test: 49,987,811 values, 0 mismatches.
- glxtest (`gl/glx/test/run-host.sh`): MIT-SHM, NOSHM and SHMBUFS=2 all pass (297 PASS lines). Over xlite (`run-xlite.sh`, rebuilt from /src/xlite): both arms pass.
- glx_reopen: 5/5 rounds read 0xf800 on stock Xlib (with `Xvfb -noreset`; without it Xvfb resets between the rounds' connections and round 1 cannot connect). Over xlite it runs without a crash (no pixel check possible).
- The review's own probes against the fixed library (`run-atk.sh`): interval0 (IV=0), vpmem, trap, trapshm, depthshare, twowin, sharedwin, share, events, destroywin, manyctx, hop and nohop all pass.
- RV32 library (`review-fixes/rv32-lib.txt`):
  - 177,660 B stripped (was 181,748); .text 103,940; RW LOAD memsz 0x28c8.
  - SONAME libGL.so.1; NEEDED libXext.so.6, libX11.so.6, libc.so.
  - Exports: 671 gl*, 44 glX*, 13 s31gl_* (s31gl_bind_depth added).
  - Both build logs have 0 warnings (`review-fixes/build-rv32.log`).
- headless_gears 320x240x100: md5 181ba4b2c0020e2eccac1006a1ea1459 on the host before the fixes, the host after them and RV32 under qemu.

### Interface changes

- `s31gl_bind_depth(ctx, depth)` (gl/api/s31gl.h): caller-owned depth; the core never frees it, and a size-changing bind_color forgets it. `tgl_ctx_bind_depth` and `tgl_attrib_slots` are new in tgl_bridge.h.
- `glxi_trap_begin(Display *)` takes the display.
- The S31GL_TRACE line gains an `earlier-round-trip` count.
- New toggles:
  - `S31GL_NOWAIT=1`: debug only, the old unpaced interval-0 behaviour;
  - `S31GL_TGL_FRAMEPTR=1`: build time, rasteriser frame pointers back on.
- `glXQueryExtensionsString` no longer lists `GLX_EXT_visual_rating`.

### Still not tested

- Anything on the board: pacing cost of the interval-0 fix, the SHMBUFS=2 A/B, the frame-pointer A/B, RSS with drawable-owned depth.
- Real SDL apps (testgl, testgl2) and the SDL_GL_Lock PushAttrib path.
- musl eager binding on RV32 (the xlite load arm is glibc `ldd -r` against host builds of the board libraries).
- xshim's own error and ShmCompletion behaviour.

## Bugs found and fixed during integration

The harness passed glxgears from the first run, but a rasterisation-rule test written for this integration (`gl/tests/glx_prims.c`, below) exposed five upstream TinyGL bugs and one GLX lifetime bug. Every fix is marked `s31:` at the code.

1. **glOrtho was transposed** (`tinygl/source/matrix.c`, glopOrtho). Upstream copied Mesa's column-major `M(row,col) m[col*4+row]` into TinyGL's row-major M4. The translation therefore landed in the w row.
   - A symmetric glOrtho has zero translation, so it hid the bug: bounce and the gears demos were unaffected.
   - **Every asymmetric glOrtho drew in the wrong place.** That includes the `glOrtho(0,w,h,0)` of every 2D game and freeglut's text overlays.
   - It is upstream erysdren's bug (same code at /tmp/claude-501/tgl/erysdren/source/matrix.c:262).
2. **GL_FLAT used the wrong provoking vertex.**
   - The flat fillers read `p2` **after sorting the vertices by y**, so a flat triangle took its lowest vertex's colour.
   - The quad and quad-strip decompositions and the triangle-strip slot rotation did not keep GL's provoking vertex in `p2` either. GL 1.5 table 2.12 says: the last vertex for lines, triangles, strips, fans and quads, and the first for polygons.
   - Fix: the primitive assembler records the provoking vertex's colour (`gl_set_provoking` in vertex.c; out of line in clip.c for code size), and the flat fill and line paths apply it to all three vertices of that one triangle.
   - glxgears_fbconfig f60 went from FAIL to PASS.
3. **Flat lines were interpolated.** ZB_line interpolated whenever the two ends differed, ignoring GL_FLAT. Fixed with the same provoking colour (`gl_zb_line`, clip.c).
4. **Clipped vertices took the current colour.** With lighting off, `gl_transform_to_viewport` fills zp.r/g/b from `longcurrent_color`, the last glColor, not from the vertex. Clipping creates vertices after the fact, so every clipped triangle and line had wrong colours at its clip edge. Fixed in `updateTmp` and the clipped line path (`gl_zp_color`, zgl.h). A clipped line in select mode also used to be drawn instead of selected; fixed.
5. **Smooth lines started at the wrong end** (`tinygl/source/zline.h`). The walk began at p2's colour and stepped by (p2-p1)/n, so a red-to-blue line drew cyan-to-red.
6. **GLX use-after-free after XCloseDisplay under xlite** (`gl/glx/glx.c`, unbind_draw).
   - xlite's `XESetCloseDisplay` is a no-op, like all its XESet* hooks, so GLX's close hook never runs on the board.
   - A drawable record then keeps a GC and a Visual of the freed Display, and the next MakeCurrent's sweep calls XFreeGC through it.
   - Fix: at the last unbind, while the display is certainly open, free the GC and forget the Visual. Both are remade at the next first draw.
   - Test: `gl/tests/glx_reopen.c`, 5 open/draw/close rounds. Stock Xlib: all 5 read back 0xf800. xlite: all 5 complete without a crash; xlite's XGetImage reads zeros, so there is no pixel check there.

Also changed:
- **An XGetGeometry from glViewport at most once per presented frame** (glx.c, glx_int.h, glx_present.c). glx_prims sets 30 viewports a frame and paid 30 round trips. A game with 3D, HUD and map viewports would pay one per viewport per frame, and on the board each is a ring round trip.
- **Honest diagnostics for the default texenv.** GL_MODULATE is GL's default, so an app that never calls glTexEnv (manywin) used to be drawn as REPLACE with no `libGL: unimplemented` line. vertex.c's glopBegin now warns once at the first textured draw.
- **Code size, which is time from XIP:**
  - `-ffunction-sections -fdata-sections -Wl,--gc-sections`;
  - the dead 8/24/32-bit output formats dropped (zfeatures.h). Their 8-bit dither also carried an `exit(1)`, now gone from the library.
  - Result: .text 120,436 -> 106,490 B, and the stripped file 194,048 -> 181,748 B.
- **Warnings tightened.** TinyGL was built with `-Wno-maybe-uninitialized -Wno-misleading-indentation -Wno-unused-variable -Wno-unused-function`. Built without them, host gcc and the RV32 gcc warn only about `zz`/`sx` set-but-unused in the templates. Only `-Wno-unused-but-set-variable` is kept, so an uninitialised read now fails the zero-warning gate.
- **gl/build.sh implements the Buildroot S31GL_* contract** (docs/gl-packaging.md section 2).
  - With `S31GL_OUT` set it builds from its own tree using S31GL_CC, S31GL_CFLAGS (first), S31GL_LDFLAGS and S31GL_STRIP=0.
  - It writes only `$S31GL_OUT/libGL.so.1.2.0` and `$S31GL_OUT/obj`, and links without `-z defs`.
  - Simulated in the build container: a copy of gl/, the defconfig's TARGET_CFLAGS (`-Os -march=rv32imafbc_..._zaamo_zalrsc_... -mtune=esp-base`), `--sysroot` to staging. The result has SONAME libGL.so.1 and NEEDED libXext, libX11, libc, with .text 106,608 and 0 warnings.
  - With the variables unset, the host defaults are unchanged.
- `gl/glx/s31gl_iface.h` now only includes `s31gl.h`. Its fallback copy of the API is gone, so drift is a compile error.

A frame that uses none of the changed paths costs one extra compare per primitive: `current_shade_model != GL_SMOOTH`. Clipped vertices pay three clamped conversions, and flat primitives pay one conversion. No double was added. The RV32 and host renderers are **bit-identical** (headless_gears 320x240 frame 20 under qemu-user against the host build, `cmp`).

## Goal 2: glxgears and glxinfo

Deterministic comparison through tools/glref: virtual time at 1/60 s per swap, Xvfb 800x480x16, LD_BIND_NOW=1, and the metric from tools/glref/compare.py (tolerance 16/255, at most 1% tolerant-bad).

| build | frame 3 | frame 20 | frame 60 |
|---|---|---|---|
| mesa-demos 9.0.0 (harness build) `glxgears -geometry 320x240` | PASS 0.021% | PASS 0.047% | PASS 0.049% |
| Debian mesa-utils 9.0.0-2+b2 `/usr/bin/glxgears` | PASS 0.021% | PASS 0.047% | PASS 0.049% |

(Tolerant-bad %. Strict is 1.05-1.07%, all edge pixels: see "Known differences".)

**Live, real time** (`artifacts/gl/phase1/glx-harness/run-gears.sh`):
- It maps and animates: 906 pixels differ between frames 0.5 s apart.
- It exits 0 on an injected Escape.
- With xlite as libX11 plus the test-only shim for xlite's two missing symbols, it animates (2,325 pixels differ). It is killed rather than exiting on Escape; the GLX agent traced that to xlite and Xvfb disagreeing on keycodes.

**glxinfo** (`int/glxinfo.txt`):
- `direct rendering: Yes`, GLX 1.4, and exactly the plan's five GLX extensions.
- `OpenGL vendor string: s31`, `renderer: Software Rasterizer`, `version: 1.1 s31-tinygl`.
- GL_EXTENSIONS `GL_EXT_bgra GL_EXT_texture_object GL_EXT_vertex_array`.
- Only the RGB565 TrueColor visual family, with depth 16, stencil 0 and caveat Slow.
- The limits are the GL minimums, with MAX_TEXTURE_SIZE 256 and MAX_LIGHTS 16.

**glx_prims**, the rules test (gl/tests/glx_prims.c, 48 cells covering culling, provoking vertex per primitive, clipping, lines, points, polygon modes, lists and depth):
- It started at **34% tolerant-bad** against Mesa.
- It ends at **0.013%** (10 px); strict is 2.96%, edges only.
- Side by side: `prims/sbs.png` (Mesa | ours | diff).

## Goal 3: the suite

`tools/glref/suite.sh --run phase1/suite`. The full report with images is `suite/report.md`. The harness counts any `libGL: unimplemented` line as MISSING-SYMBOL. In every such row the `unresolved=` field is empty: **no app has a missing symbol**, so each is a feature gap.

| app | verdict | tolerant-bad (f3/f20/f60) | cause |
|---|---|---|---|
| glxgears | PASS | 0.02 / 0.05 / 0.05 | |
| glxinfo | PASS | exit | |
| glxheads | PASS | 0.00 / 0.00 / 0.00 | |
| offset | PASS | 0.42 (f1) | |
| glxgears_fbconfig | PASS | 0.00 / 0.00 / 0.00 | was FAIL at f60 before fix 2 |
| gears | PASS | 0.04 / 0.03 / 0.05 | |
| morph3d | PASS | 0.18 / 0.18 / 0.50 | |
| bounce | PASS | 0.03 / 0.01 / 0.05 | was FAIL 3.6% before fixes 1-2 |
| geartrain | PASS | 0.85 / 0.79 / 0.88 | was FAIL 2.9% before fixes 1-2 |
| manywin | gap | 2.26 | **F4**: GL_MODULATE (2x2 texture times green) drawn as REPLACE |
| spectex | gap | 13.2 | **F4** (lit texture needs MODULATE); GL_SEPARATE_SPECULAR_COLOR (lighting, not in the F list) |
| texcyl | gap (image PASS, 0.00%) | 0.00 | **F7** glTexGen |
| isosurf | gap (image PASS, 0.04%) | 0.04 | **F7** glTexGen; **F8** glClipPlane; **F5/F6** glPolygonStipple. "Compiled vertex arrays not supported" is isosurf's own message: EXT_compiled_vertex_array is exported but not advertised |
| ipers | gap | 75.6 | **F5** blend (help panel), **F6** fog, **F4** MODULATE, **F7** glBitmap/glRasterPos/glPush/PopClientAttrib; GL_RESCALE_NORMAL |
| terrain | gap | 86 | F5 blend, F6 fog, F4, F7 (same set) |
| tunnel | gap | 80 | F5, F6, F4, F7 |
| fire | gap | 89 | F5 blend and alpha test, F6, F4, F7 |
| teapot | gap | 86 | F5, F6, F4, F7, glPixelTransfer |

In the last five, most of the bad pixels are the translucent help panel. Without blending (F5) it is opaque and hides the scene, and its text is glBitmap (F7). Classification: **crash 0, missing symbol 0, GLX problem 0; feature gaps: F4 x8, F5 x6, F6 x5, F7 x7, F8 x1, plus GL_SEPARATE_SPECULAR_COLOR and GL_RESCALE_NORMAL**, which are not in the plan's F table. The harness's own self-test (`--impl mesa`) was not re-run by me.

## Goal 4: RV32

`gl/build.sh` (Mac; it re-runs itself in `./docker/build.sh`):

- `/src/images/libGL.so.1`: **181,748 B stripped**, md5 e7d633cf5a3d31ee8e5a99d711acf454.
- `readelf -d`: **SONAME libGL.so.1; NEEDED libXext.so.6, libX11.so.6, libc.so**.
- Sections: **.text 106,490 B**, .rodata 20,916, .data.rel.ro 3,536, .data 36, .bss 6,352.
- **683 exported symbols**: 627 GL (343 implemented, 284 stubs), 44 glX*, 12 s31gl_*. No TinyGL or internal symbol leaks.
- `gl/out-rv32/headless_gears`: 198,616 B, static and stripped. Plus core_test and the qemu variants.
- Under qemu-user: core_test 116 passed, 0 failed (the 2 dlsym checks are skipped in a static build); headless_gears is bit-identical to the host.
- Soft-double call sites left for plan stage G01: 20 __extendsfdf2, 18 __truncdfsf2, 8 __muldf3, 3 __divdf3, 1 __floatsidf, 1 __gedf2, plus pow x4, sincos x3, cos x2 and sqrt x2.
  - *(corrected)* This first pass named `winv=1.0/v->pc.W` (clip.c) and `fz = 1.0 / fz` (ztriangle.h) as the hottest sites. GCC already narrows both to `fdiv.s`.
  - The real per-vertex sites were gl_clipcode's `1.0 + CLIP_EPSILON` (inlined into glopVertex) and gl_shade_vertex's sqrt, pow and constants. Both are fixed in the second pass; see Review fixes, R2.

## Goal 5: X imports against xlite

libGL imports 19 X symbols:
- **all 19 are defined in the board's `images/libX11.so.6.4.0`** (xlite): XAddExtension, XCheckIfEvent, XCreateGC, XCreateImage, XESetCloseDisplay, XFlush, XFree, XFreeGC, XGetGeometry, XGetVisualInfo, XPutImage, XSetErrorHandler, XSync, and XShm Attach / CreateImage / Detach / GetEventBase / PutImage / QueryExtension;
- five of the XShm ones are also in the xstubs `images/libXext.so.6.4.0`. XShmGetEventBase is only in xlite's libX11, which is always loaded, so it resolves.
- `xlite/libX11-exports.txt` (Aug 30) is stale: it lists no XShm*, though the built library has them.

**Gaps outside libGL.** The X imports of the suite's binaries and of libglut/libGLU were checked against every X library in images/ (`int/app-ximports.txt`). musl binds eagerly, so each gap aborts that app at load on the board:

| missing in board libs | needed by |
|---|---|
| XSetNormalHints, XSetStandardProperties | glxgears (mesa-demos 9.0.0), glxgears_fbconfig, glxheads, manywin |
| XGetEventData, XFreeEventData, XGetPointerMapping, XGetWMName, XStoreColor | libglut.so.3 (every GLUT demo) |
| XRRConfigTimes, XRRSetScreenConfig | libglut.so.3 (xlite's libXrandr) |

These gaps agree with the packaging report's. The packaging agent also found that stock libXi imports `_XUnknownNativeEvent`.
- Caveat: the libglut checked is the host rig's freeglut 3.8.0. The Buildroot build's feature set (XInput2, XRandR) may differ.
- The Debian `/usr/bin/glxgears` needs none of these, so it is a different build from the one the board will run.

## Packaging check (buildroot-external: read, not edited)

| item | finding |
|---|---|
| `S31_LIBGL_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../gl` | correct (external.desc name ESP32_S31) |
| build contract | **was a mismatch; fixed on the gl/ side.** build.sh now honours S31GL_* (simulated, see above) |
| rsync exclusions `/ref-apps /out /build` | **mismatch:** the tree's outputs are `gl/out-host/` (572 kB) and `gl/out-rv32/` (2.1 MB), which are not excluded. They are harmless (build.sh writes to S31GL_OUT), but they are copied on every rsync. Suggest `--exclude=/out-host --exclude=/out-rv32` |
| `XIP_ROOTS_GL ?= usr/lib/libGL.so.1.2.0` (Makefile:934) | matches the installed name |
| LICENSE_FILES | still empty: gl/ has no top-level LICENSE (gl/tinygl/LICENSE exists). Adding one needs the owner's copyright line |
| headers | our lib compiles against gl/include (libglvnd's Khronos copy); consumers get mesa3d-headers. Both are Khronos, so the ABI is the same. enumcmp.py checks TinyGL's enums against gl/include only |
| host python3 | build-lib.sh runs enumcmp.py and mkstubs.py. Buildroot already requires python3 on the build host |
| .gitignore | `gl/out-host/` and `gl/out-rv32/` are not ignored, so build outputs would be committed. Not my file |

## Known differences, not fixed

- **Edge placement (the strict column).** TinyGL maps NDC with a `(size - 0.5) / 2` scale, and its spans include both end pixels. Geometry therefore shrinks by half a pixel towards the origin, and exact-integer lines land one row higher than Mesa's (bounce grid: rows 19, 39 ... against 20, 40 ...). The tolerant metric counts this as edge-only: glxgears strict 1.05%, tolerant 0.02%. An exact transform needs a top-left fill rule in every filler, which is rasteriser work with a speed cost.
- **Presents wait on almost every frame.** With one SHM buffer, glxgears waits by XSync on 40,385 of 40,386 presents under stock Xlib, and 70 of 74 in glxtest under xlite. It draws the moment after it swaps, so the server's copy has to finish first.
  - *(corrected, second pass)* On stock Xlib this depends on timing. The review saw 0 XSync from a slower, strace'd run; this pass re-measured 41,995 of 41,996 at 5k fps. On xlite it happens whatever the timing, because xlite never advances `last_request_read` (R1).
  - `S31GL_SHMBUFS=2` overlaps the two (host: 9.7k against 5.4k fps; GLX agent, 3+ runs). It costs one more colour buffer: **153,600 B at 320x240**. The GLX report's "about 600 kB" is the 640x480 figure.
  - This needs a board A/B.
  - Separately, xlite never advances `last_request_read` from events, so a wait whose ShmCompletion the app ate always needs an XSync there. That is xlite owner's fix, per the GLX report.
- Pixmaps and pbuffers are unsupported. Visuals are RGB565 only; apps asking for 24-bit depth or 8-bit colour get NULL. Push/PopAttrib were no-ops *(implemented in the second pass)*. For these and the other open core and GLX items, see the component reports.

## Commands

```
gl/host-build.sh                        # host rig: gl/out-host/{libGL.so.1,core_test,headless_gears,glx_prims,glx_reopen}
gl/build.sh                             # RV32: /src/images/libGL.so.1, gl/out-rv32/*
S31GL_OUT=... S31GL_CC=... sh gl/build.sh   # Buildroot mode (s31-libgl.mk)
tools/glref/suite.sh --run phase1/suite # all 18 targets -> artifacts/gl/phase1/suite/report.md
docker run --rm -v $PWD:/src -w /src s31-glref:latest sh gl/tests/run-sysgears.sh   # Debian glxgears, 3 frames
docker run --rm -v $PWD:/src -w /src s31-glref:latest sh gl/tests/run-prims.sh      # rules test -> prims/sbs.png
docker run --rm -v $PWD:/src -w /src s31-glref:latest gl/out-host/core_test         # 125/0 after the review fixes
tools/glref/xlite-load.sh [out.md]      # load-time arm against xlite + xstubs (also run by suite.sh)
docker run --rm -v $PWD:/src -w /src s31-glref:latest sh gl/glx/test/run-host.sh /src/gl/out-host   # 3 arms, all pass
docker run --rm -v $PWD:/src -w /src s31-glref:latest sh artifacts/gl/phase1/glx-harness/run-xlite.sh /src/gl/out-host  # xlite, 2 arms, all pass
docker run --rm -v $PWD:/src s31-glref-qemu:latest qemu-riscv32 -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true /src/gl/out-rv32/core_test.qemu  # 123/0 after the review fixes
```

(`s31-glref-qemu` is a local image the core agent made with docker commit. It is not in the repo.)

## Not tested

- Anything on the board: fps, CPU, RSS, xshim's present path and fullscreen alias, eager binding under musl, and the SHMBUFS=2 A/B.
- A real Buildroot build of s31-libgl, libglu, freeglut or mesa3d-demos. The contract was exercised only in simulation.
- SDL apps: the harness does not include testgl or testgl2 yet.
- The harness's Mesa-vs-Mesa self-test after today's changes. I changed nothing in tools/glref, so the cached references stand. *(Second pass: re-run after the tools/glref changes, with 18 EXACT and glxinfo PASS.)*
