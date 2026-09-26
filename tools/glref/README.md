# glref: host A/B correctness harness for our libGL

Runs **stock, unmodified** GL apps under Xvfb at the panel's depth
(800x480x16) twice - once on Mesa (llvmpipe), once on our `libGL.so.1` - and
compares the frames pixel by pixel. This is the correctness gate of
docs/gl-plan-2026-09-25.md section 6 ("the same stock app run under Xvfb and
Mesa on the host at the same size, compared by pixel runs with a
tolerance"). It is host-only: it never touches the board.

Everything runs in the `s31-glref` Docker image (Dockerfile here). Every
script re-execs itself in that container when started from the Mac, so
the commands below work from the repo root on the host as-is.

## Quick use

    tools/glref/suite.sh                        # all targets, our libGL vs Mesa
    tools/glref/suite.sh --apps "glxgears gears" --frames "3 60"
    tools/glref/suite.sh --impl mesa --run mesa-vs-mesa   # harness self-test
    open artifacts/gl/<run>/report.md

One app, one frame:

    tools/glref/run.sh mesa glxgears 30 /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears -geometry 320x240+0+0
    tools/glref/run.sh ours glxgears 30 /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears -geometry 320x240+0+0
    docker run --rm -v $PWD:/src -w /src s31-glref:latest python3 tools/glref/compare.py \
        artifacts/gl/adhoc/mesa/glxgears.f30.png artifacts/gl/adhoc/ours/glxgears.f30.png --diff /src/artifacts/gl/adhoc/d.png

Rebuild the image after editing the Dockerfile:
`docker build -t s31-glref:latest tools/glref`. Mount only paths under
/Users (Docker on this Mac silently discards writes to /private/tmp mounts).

## The pieces

| file | what it does |
|---|---|
| `build-apps.sh` | Builds GLU 9.0.3, freeglut 3.8.0 and mesa-demos 9.0.0 (the board's versions) into `gl/ref-apps/` (gitignored, ~90 MB). Idempotent; `--force` rebuilds. Ends by checking that every binary needs `libGL.so.1` and **nothing** from glvnd (`libOpenGL.so.0`/`libGLX.so.0`). |
| `build-games.sh` | The stage-5 corpus, from upstream tarballs in `gl/ref-apps/`, each by its own build system: SDL 1.2.15 `test/testgl`, SDL2 2.32.10 `test/testgl2`, rRootage 0.23a (against the rig's SDL: sdl12-compat over SDL2 2.32.4). The knobs used and why (and why GLtron 0.70 is not built) are in its header. suite.sh runs it; `$SB` in apps.txt is its build directory. |
| `pkgconfig/gl.pc`, `glx.pc` | Make meson/cmake link `-lGL` only. Debian's `glx.pc` says `-lGLX` and its libGLU links `libOpenGL.so.0`; either would bind GL calls to glvnd and make the Mesa/ours swap by `LD_LIBRARY_PATH` impossible. On the board, `s31-libgl`'s own `gl.pc`/`glx.pc` do the same job. |
| `capture.c` | The `LD_PRELOAD` shim (built on demand by run.sh into `gl/ref-apps/build/glref/capture.so`). See below. |
| `run.sh` | One app, one implementation, one frame: private Xvfb on `:90-:99`, the shim, a timeout, then a status file. |
| `compare.py` | Scores a frame against its reference; writes a diff image. |
| `apps.txt` | The target list: name, kind, capture mode, frames, command. |
| `suite.sh` | Every target at 2-3 frames, Mesa references cached, `report.md`. |
| `report.py` | Builds `report.md`/`report.json` from a run directory (re-runnable by hand). |
| `gltrace/` | Record a stock app's whole GL/GLX stream into a binary trace and replay it: on the host (Mesa or ours) and on the RV32 instruction counter (gl/bench/qsreplay.sh). The QuakeSpasm workload proxy; see "gltrace" below. |
| `xlite-load.sh` | The board's X stack at load time: every app and the libglut/libGLU it loads, `ldd -r` (all relocations, as musl binds) against our libGL + xlite libX11 + xstubs libXext + xlite's libXrandr/libXxf86vm, built for the host from the repo. A suite PASS uses the host's libX11 and says nothing about xlite gaps; this says which apps would abort at load on the board. suite.sh runs it for `--impl ours` and appends it to `report.md`. |

## How a frame is made deterministic (capture.c)

- **Time.** Every clock the *app* reads (`gettimeofday`, `clock_gettime`,
  `time`, `clock`) returns a fixed base plus `swaps x 1/60 s` plus virtual
  sleep. Sleeps and finite `select`/`poll`/`ppoll` waits from the app advance
  the virtual clock instead of waiting (capped at 10 s per wait: freeglut
  waits "INT_MAX ms" when it has no timers, and 24 virtual days overflow
  `glutGet(GLUT_ELAPSED_TIME)`). So a time-driven animation is at the same
  phase at swap N under any GL, and timer-driven apps still run. "The app"
  means any caller whose return address is outside the GL/X/libc libraries
  (`lib_is_real()`): Mesa's threads, libxcb's own blocking poll and our
  libGL (anything under `/gl/out-host/`) keep real time.
- **X events.** Whether an Expose/ConfigureNotify has arrived when the app
  next looks is a real-time race that decides which frame a resize lands
  in. When the app calls `XPending`/`XEventsQueued`, or waits on an X
  connection it opened, the shim `XSync`s that connection first, so the app
  sees an infinitely fast server - the same one under both
  implementations.
- **Apps that load GL themselves** (SDL: `dlopen("libGL.so.1")`, then
  `dlsym(handle, "glXGetProcAddressARB")` and everything else through it)
  bypass LD_PRELOAD. The shim therefore also interposes `dlsym`: an app
  asking any handle for a hooked GLX name gets the hook, and the handle is
  kept so the hook reaches the real function. Lookups from GL/X/libc
  libraries (glvnd's vendor lookups) are forwarded untouched. Adding this
  changed none of the 57 existing reference images or statuses.
- **sdl12-compat.** The rig's SDL 1.2 is sdl12-compat; on a GL with FBOs
  (Mesa, not ours) it renders SDL_OPENGL apps into an FBO and swaps once
  inside SDL_SetVideoMode, which put every Mesa frame one swap and 16 ms
  ahead. run.sh sets `SDL12COMPAT_OPENGL_SCALING=0` for both arms (the
  board's SDL 1.2.15 has no such path).
- **Capture.** `glXSwapBuffers` (also when fetched by `glXGetProcAddress`)
  calls the real swap, `XSync`s the app's display, and after the Nth swap
  reads the window's on-screen rectangle **from the root window over a
  private X connection** with `XGetImage`. No GL call is used, so the
  capture is identical for Mesa and ours. GLX 1.3 `GLXWindow`s are mapped
  back to their X window. `GLREF_CAPTURE=screen` grabs the whole 800x480
  (multi-window apps). The window is clipped to the screen (offset is 900
  wide). Pixels are expanded from the 565 visual by the visual's masks.
  Then `fflush` and `_exit(0)`: no GL teardown runs.
- **Stalls.** A watchdog thread captures the last completed frame if no swap
  happens for `GLREF_STALL_MS` (4 s) and exits 3 - that is how apps that draw
  once and wait for events (offset, isosurf) are captured, and it is recorded
  as `stalled at swap K`, so the report can see that ours stalled at a
  different swap than Mesa.
- **Load-time symbols.** Both implementations run with `LD_BIND_NOW=1`, so
  glibc resolves every import at load exactly as musl does on the board: a
  missing GL entry point is a `symbol lookup error` at start
  (MISSING-SYMBOL), not a crash at first call.
- **llvmpipe single-threaded** (`LP_NUM_THREADS=0`): with its rasteriser
  threads manywin (4 windows, 4 connections) differed in 5 of 30 Mesa runs;
  with none, 0 of 30.

Environment knobs (run.sh passes them through): `GLREF_TIMEOUT` (s, 60),
`GLREF_CAPTURE`, `GLREF_STALL_MS`, `GLREF_FRAME_NS`, `GLREF_TRACE=1` (log every
swap with its drawable and virtual time, and every virtual-time advance with
the library that slept), `GLREF_OURS` (directory holding our
`libGL.so.1`, default `/src/gl/out-host`), `GLREF_RUN_DIR`, `GLREF_CWD`
(default: the mesa-demos source `src/demos`, whose `../data/` the demos
load their textures from).

## Verdicts

Per frame, from `run.sh`'s status and `compare.py`:

| verdict | meaning |
|---|---|
| EXACT | bit-identical to the Mesa reference |
| PASS | tolerant-bad pixels <= 1.0% |
| FAIL | more than that, or a size mismatch |
| MISSING-SYMBOL | the loader refused the app (`symbol lookup error`, `undefined symbol`, musl `Error relocating`), **or** the log has any `libGL: unimplemented` line (the image verdict is kept in the notes) |
| CRASH | killed by a signal (the report shows it and the log tail) |
| TIMEOUT | not captured within `GLREF_TIMEOUT` |
| CAPTURE-FAILED | the window was not viewable/on screen |
| ERROR | exited non-zero without an image |
| NO-REF | Mesa itself produced no reference for that frame |
| NOT-BUILT | `$GLREF_OURS/libGL.so.1` does not exist |

For `ours`, run.sh also lists **every** `gl*`/`glX*` import of the app and of
the libglut/libGLU it loads that our `libGL.so.1` does not export (`nm -D`).
`LD_BIND_NOW` stops at the first one, and this list is the complete set.
report.md collects them into a table of symbol against apps.

`exit`-kind apps (glxinfo) have no image: PASS is a clean exit, and the report
lists the vendor/renderer/version strings the app printed.
An app's verdict is the worst of its frames.

### The metric and why these thresholds (compare.py)

- **strict**: pixel bad if any channel differs by more than `--tol`.
- **tolerant**: pixel bad only if **no** pixel in the 3x3 neighbourhood of
  the other image is within `--tol`, checked in both directions. It forgives
  a polygon edge landing one pixel over - Mesa and TinyGL have different
  fill conventions and sub-pixel precision - but not a wrong colour, a
  missing polygon, a wrong texture or a flipped image.
- `--tol 16`/255: RGB565 has 8.2 levels per 5-bit LSB. Round-to-nearest
  against truncating fixed-point Gouraud is one LSB, interpolation error one
  more.
- `--max-bad 1.0`%: what remains between two correct renderers after edge
  forgiveness is specular-rim and texture-phase noise. 1% of 320x240 is 768
  pixels; a missing gear tooth or a wrong texenv costs much more.

**What the tolerant score cannot see.** Calibration on Mesa's glxgears:
frame 30 against frame 31 (a 1.2 degree rotation) is **PASS**, with 3.25%
strict and 0.77% tolerant bad. A one-frame phase error is therefore invisible
to the verdict. It is prevented by the deterministic clock, not by the
metric, and it shows up in the strict column. Read both columns: a "PASS"
at a strict score far above its siblings means that something moved.

## Verified (2026-09-25)

- **Mesa against Mesa**: `suite.sh --impl mesa --run mesa-vs-mesa` gives 17 of
  17 image apps EXACT in every frame, and glxinfo PASS
  (`artifacts/gl/mesa-vs-mesa/report.md`).
- **The "ours" path, with Mesa behind it**: `GLREF_OURS` pointed at a directory
  holding a symlink to Mesa's `libGL.so.1`. Six consecutive suites, 10 runs
  at a time, all EXACT. This proves the `LD_LIBRARY_PATH` swap and the
  determinism under parallel load (`artifacts/gl/selftest-ours-is-mesa/`).
- **Negative tests**:
  - A `libGL.so.1` exporting only `glClear` gives MISSING-SYMBOL for
    glxgears (`glTranslated`), gears (`glEnd`) and glxinfo
    (`artifacts/gl/selftest-ours-fake/`).
  - One that prints `libGL: unimplemented glClear` and forwards to glvnd
    gives MISSING-SYMBOL with the image verdict noted
    (`artifacts/gl/selftest-unimpl/`).
- **Timing**: a full suite takes ~20 s for 96 runs at `--jobs 10`. The
  cached references are regenerated automatically when capture.c, run.sh,
  apps.txt, an app binary or the Mesa package changes (`ref-mesa/STAMP`).

## First run against our libGL (2026-09-25 21:25, `artifacts/gl/ours-first/`)

`gl/out-host/libGL.so.1` (aarch64, 316 kB, built 21:23) gets MISSING-SYMBOL
for all 18 apps.
- Every `gl*` import of every target resolves: F1 is complete for this
  corpus.
- 24 `glX*` entry points are missing. The GLX layer (gl/glx/) is not linked
  into that build yet. The report lists which app needs which:
  - `glXSwapBuffers`, `glXMakeCurrent`/`glXMakeContextCurrent`,
    `glXChooseVisual`/`glXChooseFBConfig`, `glXCreateContext`/
    `glXCreateNewContext`, `glXGetProcAddressARB`, and so on;
  - freeglut 3.8 alone needs the GLX 1.3 FBConfig set.
- No image comparison is possible until GLX is exported. Re-run
  `tools/glref/suite.sh --run <name>` when it is.

## Limits

- **Displays**: 10 at most (`:90-:99`), so `--jobs` is 10 at most per
  container.
- **Sizes**: stock sizes where an app has no size switch - glxheads and
  glxgears_fbconfig 300x300, manywin 4 x 90x90, offset 900x300 (clipped),
  isosurf 400x400 (it rejects `-geometry`). Everything else is 320x240 via
  `-geometry`.
- **Waits**: a wait on a non-X fd gets 1 ms of real time before virtual time
  jumps. An app that depends on a pipe from a helper thread arriving later
  than that could still be non-deterministic. None of the current targets
  does.
- **SDL**: testgl, testgl2 and rRootage are in `apps.txt` (2026-09-26,
  artifacts/gl/phase2/SUITE.md). The "SDL 1.2" arm is sdl12-compat, i.e.
  SDL2's GLX loader, not SDL 1.2.15's SDL_x11gl.c: the board's SDL 1.2.15
  (with Buildroot's patches) does not compile on this LP64 host
  (`_XData32` prototype conflict under LONG64).
- **xlite load arm and SDL**: the rig's libSDL2 links libX11 directly, the
  board's is `SDL_VIDEO_DRIVER_X11_DYNAMIC`, so the host SDL2's own imports
  (Xdbe*, Xutf8*) are listed but not counted.

## gltrace: the QuakeSpasm GL workload proxy (2026-09-26)

Records every GL and GLX call stock QuakeSpasm 0.96.3 makes during
`timedemo demo1` (320x240 windowed, the capture shim's virtual clock), with
texture and pixel data by value, into a compact binary trace. The trace then
replays three ways: on the host through GLX against our libGL (it must
reproduce the live frames bit for bit) or Mesa (image check), and on RV32
bare metal linked with our library objects under gl/bench's qemu
instruction counter (gl/bench/BASELINE.md, "QuakeSpasm proxy"). Host only:
nothing touches the board.

    tools/glref/gltrace/capture-qs.sh [NAME]                 # ~10 s, one command
    tools/glref/gltrace/replay-host.sh ours|mesa TRACE OUTDIR [OURS_DIR]
    tools/glref/gltrace/compare-qs.sh TRACE OUTDIR [OURS_DIR] # ours vs Mesa, every counted frame
    gl/bench/qsreplay.sh [GLDIR] [OUT] [TRACE]               # RV32 instructions/frame, texture bytes
    gl/bench/qsprof.sh OUT                                   # per-function profile of the counted frames
    gl/tests/run-qsr.sh [GLDIR]                              # guard: the phase-4 trace, hashes + <= +0.5%
    gl/tests/run-qsr-ab.sh "ENV_A" "ENV_B"                   # two toggle arms must draw identical frames
    gl/tests/run-qsr-fused.sh [GLDIR]                        # phase 5 O2: S31GL_FUSED=0 vs default, every trace
    gl/bench/qscensus.sh GLDIR OUT TRACE                     # which general-path stage lists draw how many pixels
    gl/bench/pcannot.py ELF OBJDUMP FUNC FRAMES PCPROF.LOG   # one function's disassembly with its counts

`QS_CFG="gl_texturemode GL_NEAREST_MIPMAP_NEAREST"` (console commands,
`;`-separated) captures with the app's own settings, as a player would set
them: the game directory is then a copy whose autoexec.cfg runs them before
the timedemo. Phase 5 O2's traces `p5a-nmn`, `-nml`, `-lmn`, `-lin`,
`-near` are QuakeSpasm's five other `gl_texturemode` values (it offers
them only on the console; the menu has none), `p5wide` is
`QS_WINDOWS=20-27,120-127,...,900-907` for the census.

TRACE and OUTDIR of the host scripts are container paths (`/src/...`).
**Re-capture after any library change that alters what the app sees** (the
extension string above all: QuakeSpasm picks its world and alias paths from
it, so after GL_ARB_multitexture/texture_env_combine land the trace must be
re-recorded). capture-qs.sh records against `QS_LIBGL`, default
`/src/gl/out-host/libGL.so.1`, i.e. whatever `gl/host-build.sh` last built.

### How it records (gltrace_rt.c, gen.py)

- **A wrapper libGL.so.1, not an LD_PRELOAD.** SDL (sdl12-compat -> SDL2)
  `dlopen()`s libGL.so.1 and fetches everything through `dlsym` and
  `glXGetProcAddressARB`, which a preload never sees. The tracer is built
  *as* libGL.so.1 and put first on `LD_LIBRARY_PATH`, so the app's direct
  imports and SDL's lookups all land in it. It `dlopen()`s the real library
  by path (`GLTRACE_REAL`) and forwards.
- **The same export surface as the real library.** gen.py reads `nm -D` of
  the real libGL and the prototypes of gl/include/GL/{gl,glext,glx}.h:
  - every exported gl* name gets a recording wrapper;
  - every glX* name gets a wrapper that notes the call;
  - MakeCurrent, SwapBuffers, Create/DestroyContext and GetProcAddress are
    hand-written;
  - everything else (s31gl_*, the two glXSwapInterval names without a
    prototype) is an aarch64 tail-jump trampoline.

  capture-qs.sh checks that the tracer's export list equals the real one's.
  An extra export would change what SDL finds; a missing one would fail
  LD_BIND_NOW. `glXGetProcAddressARB` returns the tracer's own wrapper for a
  name the real library resolves, and NULL where the real one returns NULL,
  so the app sees exactly the real feature set.
- **Depth 0 only.** The real library calls its own exports through the PLT,
  which lands in the wrappers again. A thread-local depth counter records
  only the application's calls.
- **Data by value.** A size rule per pointer parameter:
  - images: `glTex(Sub)Image*`, `glDrawPixels`, `glBitmap`. The rule applies
    the unpack state (row length, skips, alignment), read back from the real
    library at the call;
  - vectors by name (`glColor4fv`...);
  - pname-counted parameters (`glFogfv`, `glLightfv`...);
  - matrices, name arrays and index arrays.

  Outputs (`glGet*`, `glReadPixels`) record only their size: the replay
  passes scratch memory. `glGenTextures` records the names it returned, and
  the replay maps recorded names to its own. A call the rules cannot record
  faithfully is written as UNHANDLED, and the capture fails:
  - client-array pointers;
  - `glMap*`;
  - GLSL sources;
  - any draw call with client arrays enabled.

  QuakeSpasm's no-extension path uses none of them.
- **Frames.** Frame N is what the Nth `glXSwapBuffers` presents, the same
  numbering as glref's `f<N>` captures. Recording modes:
  - **Counted frames** (`GLTRACE_WINDOWS`, default `100-139,400-439`) and
    `GLTRACE_WARM` (2) warm-up frames before each window are recorded in
    full.
  - **State-only frames**: every other frame, the load phase included.
    Their draw calls and queries are dropped (`glBegin`/`glEnd` blocks,
    `glClear`, `glDraw*`, `glReadPixels`, `glCopyTex*`, `glGet*`). Each
    dropped block leaves only the last colour, texcoord and normal it set,
    so the state at the next frame is what it was live. Every texture
    upload and state change is kept.
  - The warm-up frames refill both colour buffers and the library's
    retained state (dirty boxes, depth epochs) before the first counted
    frame.
- **Live hashes.** After every full frame's swap the tracer `XSync`s. It
  then reads the window from the root window over a second connection,
  exactly as capture.c does, and stores the FNV-1a hash of the RGB565
  pixels in the SWAP record. With `GLTRACE_FRAMES` it also writes the raw
  frame.
- **Format** (gltrace.h). Each record is 32-bit words:
  - word 0 is `id | nwords << 12`;
  - then the scalars, then each pointer as a length word and its data.

  The header carries the name table, so replayers map names, not numbers.

### Verified (2026-09-26, trace `gl/bench/qstrace/work/p4final`, phase-4-final library)

- **The tracer does not change what is drawn.** Frame 439 captured by
  capture.c in a run without the tracer matches the tracer's live frame
  439: 0 of 76,800 pixels differ.
- **The capture is deterministic.** Two captures are byte-identical except
  for 2 of the 689 `glTexImage2D` calls, both in frame 13. Those two upload
  QuakeSpasm's warp-image placeholders, "dummy data from the hunk" (heap
  garbage, gl_model.c:585). `glCopyTexSubImage2D` overwrites them before
  they are used, and all 84 frame hashes are identical.
- **The host replay against ours is bit-exact.** All 84 full frames (80
  counted + 4 warm-up) match the live hashes. This holds for the default
  configuration and for a second capture made with `S31GL_TEXFILTER=0`
  (`QS_GLENV`).
- **The RV32 bare-metal replay is bit-exact.** All 84 frames match the live
  (host) capture in both configurations (gl/bench/BASELINE.md).
- **Against Mesa** (compare-qs.sh), 64 of 80 counted frames PASS and 16 FAIL
  (116-118, 122-127, 133-139). The worst is frame 137, with 2.79% tolerant
  and 6.3% strict bad pixels. Every failure is in the particle spray
  (`artifacts/gl/phase5/qsproxy/mesa-sbs-f137.png`): 1-2 pixel particle
  triangles land on different pixels. Nothing else differs; the walls,
  models and HUD pass.
- **The trace:**
  - 69.4 MB, 2,397,704 records, 439 frames: 84 full and 355 state-only;
  - load-phase uploads: `glTexImage2D` 20.4 MB (689 calls) and
    `glTexSubImage2D` (the lightmaps) 12.2 MB;
  - about 395 kB and 25-44 k calls per counted frame, mostly
    `glColor4fv`, `glTexCoord2f` and `glVertex3f[v]`.

  Traces are gitignored (`gl/bench/qstrace/work/`).
- **Time:** capture about 9 s, host replay about 2 s, RV32 replay about 8 s
  (both images), profile about 8 s.

### Limits

- **Replay ignores most of GLX.** It does not replay the glX calls other
  than MakeCurrent, SwapBuffers and Create/DestroyContext
  (`glXSwapInterval*`, `glXQuery*` and the like). It does not replay a
  separate read drawable either (the tracer warns).
- **The host SDL is not the board's.** The rig runs sdl12-compat, not SDL
  1.2.15, so the few GL calls SDL itself makes at start-up may differ from
  the board's. QuakeSpasm's own stream is the same.
- **A trace belongs to the library it was recorded against.** QuakeSpasm
  never reads pixels back on this path, so the calls do not depend on the
  library's rendering, apart from the two placeholder uploads. The live
  hashes do: replaying with other toggles is a valid measurement, but its
  hash check is only meaningful against a capture made with the same
  toggles (`QS_GLENV`).
