# OpenGL for the shim: a TinyGL plan (2026-09-25)

Goal: simple 3D for stock X11 and SDL applications on this board - demos,
small tools, simple games - through the same kind of drop-in library that
made xlite work for libX11. Not a Quake renderer: sdlquake's own software
renderer already does 21 fps and a generic rasteriser would not beat it.

## Which TinyGL

| fork | state | why / why not |
|---|---|---|
| [erysdren/TinyGL](https://github.com/erysdren/TinyGL) | active, last commit 2026-02-09, 172 commits, MIT, CMake | **Base.** GL 1.2 subset, RGB565 mode (`ZB_MODE_5R6G5B`, `TGL_FEATURE_RENDER_BITS 16`), an offscreen API (`include/GL/ostinygl.h`) that is exactly the hook a GLX layer needs, glDrawElements, the classic demos (gears, teapot, texobj, morph3d, mech, spin, bounce). Its README lists GLX as a to-do: we write that part. |
| [C-Chads/tinygl](https://github.com/C-Chads/tinygl) | archived 2023-11-22 | **Mine, do not base on.** Tuned with valgrind/perf, OpenMP for pixel work and buffer copies, blending, glDepthMask, polygon stipple. Port individual optimisations into the erysdren base only where a board measurement shows they pay. |
| [bellard.org/TinyGL](https://bellard.org/TinyGL/) | 2002 original | Reference only. |
| [tinygles](https://github.com/lunixbochs/tinygles) | GLES 1.x variant | Only if a target app speaks GLES 1 rather than desktop GL. |

GL 1.1/1.2 fixed function, no shaders, no mipmapping, no stencil (per the
forks' READMEs) - that bounds which applications can run, and it is the right
bound for a 15 MB machine: anything that needs GLSL also needs far more RAM
than exists here.

## Architecture: our own libGL, the xlite pattern

```
 stock app ──GL 1.x──▶ libGL.so (ours) = TinyGL + a GLX layer on xlite
                          │  glXSwapBuffers
                          ▼
             XLITE-SHM zero-copy buffer ──▶ xshim/lvdesk present (existing)
```

- `libGL.so.1` (soname exactly as Mesa's, so stock binaries and SDL2's
  `dlopen("libGL.so.1")` find it), built by a new `xlite/gl/build.sh` like
  `xlite/vidmode/`; staged by `make x11-stage` into the first XIP image
  (zero RSS for its code), listed in `X11_REPLACEMENTS`.
- **GLX, client side only.** `glXQueryExtension`, `glXQueryVersion`,
  `glXChooseVisual`/`glXChooseFBConfig`/`glXGetFBConfigs`/
  `glXGetVisualFromFBConfig`, `glXCreateContext`/`glXCreateNewContext`,
  `glXMakeCurrent`/`glXMakeContextCurrent`, `glXSwapBuffers`,
  `glXDestroyContext`, `glXGetProcAddress(ARB)`, `glXQueryExtensionsString`,
  `glXSwapIntervalEXT`/`MESA` as no-ops. No indirect GLX, no GLX protocol:
  xshim only has to advertise the `GLX` extension so apps that check for it
  proceed, and answer `glXQueryServerString`-style queries from the client
  library itself.
- **Rendering.** One TinyGL context per GLX context; its colour buffer is a
  shared-memory segment xshim already knows how to present (the XLITE-SHM
  path, RGB565 matching the panel - no conversion); the depth buffer is
  private. `glXSwapBuffers` = damage the whole window and hand the segment
  over (xshim's ShmPutImage copy-or-alias, the fullscreen alias when the
  window is fullscreen). Resize follows ConfigureNotify.
- **Visuals.** xshim's depth-16 TrueColor visual (0x21) is the GL visual;
  advertise double-buffered RGB565 + 16-bit depth.
- **SDL2 apps.** SDL2's X11 GL path goes through GLX and `libGL.so.1`, so
  `SDL_GL_CreateContext` works once GLX does - SDL itself is not modified.

## Budget

| item | cost |
|---|---|
| library code | ~150-250 kB, in XIP flash (0 RSS) |
| per context, 320x240 | colour 150 kB (shared segment) + depth 150 kB |
| per context, 640x480 | 600 kB + 600 kB - allowed but tight; warn in docs |
| textures | whatever the app uploads, RGB565 internally |
| idle | zero: no timers, work only inside GL calls |

## Performance expectations and levers

A software rasteriser on a 320 MHz single-issue core with PSRAM behind a
cache is demo-grade: expect gears/teapot at 320x240 in the tens of fps,
textured scenes slower. What to measure, and the levers (in order of cost):

1. **Baseline:** gears, teapot, texobj at 320x240 and 640x400 through the
   real GLX path; fps, the h1s split (TinyGL / xlite / lvdesk / kernel), RSS.
2. **Compiler flags** for the library (-O2 vs -Os, Zba/Zbb, float constants
   single precision - this board has F without D: every `double` in TinyGL's
   transform code is a libgcc call; `-fsingle-precision-constant` and a
   grep for double arithmetic are the first optimisation).
3. **Clears and presents on the PPA** (the 2D engine already used for
   scaling): glClear of colour/depth as PPA fills, and the present is already
   zero-copy.
4. **Port C-Chads' measured wins** one at a time.
5. **The second hart:** TinyGL's per-scanline work split between the harts
   (C-Chads' OpenMP layout is the model; use a pthread worker pinned to CPU1,
   not OpenMP), measured against the cross-hart costs already on record.

## Staged plan with gates

| stage | deliverable | pass rule |
|---|---|---|
| 0 | cross-build erysdren TinyGL + its `ui_headless` demo on the board (no X) | renders a correct teapot PNG; fps printed |
| 1 | `libGL.so.1` + minimal GLX; `gears` from TinyGL's examples built against the system GL headers, run on the desktop | window appears, animates, closes via WM_DELETE_WINDOW; screenshot matches |
| 2 | stock `glxgears` (mesa-demos, via Buildroot) unmodified | runs, fps recorded |
| 3 | an SDL2 app that opens a GL context (a small stock one - pick in stage 2) | runs unmodified |
| 4 | performance levers 2-5 above, each A/B'd | recorded with numbers; keep what pays |
| 5 | ship: XIP image, menu entries for the demos, compat gate step | x11-compat-gate green, no desktop regression (lvdesk present cost, idle CPU) |

Kill rules: stage 1 fps below 5 for gears at 320x240 after stage-4 levers,
or RSS per context above 500 kB at 320x240, or any lvdesk regression.

## Candidate applications

glxgears and the mesa-demos GL 1.x set; the TinyGL example set; small GL 1.x
games that fit in memory (to be surveyed in stage 2 - e.g. simple puzzle or
arcade titles from Buildroot's package list that use fixed-function GL);
SDL2 apps with a GL renderer. Not: anything needing GLSL, GL 3+, or more than
a few MB of assets.

## Rules this respects

Stock applications, their own build systems. The GL library is ours - the
same standing as xlite. Nothing is placed in XIP for an app; only our library.
