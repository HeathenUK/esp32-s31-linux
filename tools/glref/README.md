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
| `pkgconfig/gl.pc`, `glx.pc` | Make meson/cmake link `-lGL` only. Debian's `glx.pc` says `-lGLX` and its libGLU links `libOpenGL.so.0`; either would bind GL calls to glvnd and make the Mesa/ours swap by `LD_LIBRARY_PATH` impossible. On the board, `s31-libgl`'s own `gl.pc`/`glx.pc` do the same job. |
| `capture.c` | The `LD_PRELOAD` shim (built on demand by run.sh into `gl/ref-apps/build/glref/capture.so`). See below. |
| `run.sh` | One app, one implementation, one frame: private Xvfb on `:90-:99`, the shim, a timeout, then a status file. |
| `compare.py` | Scores a frame against its reference; writes a diff image. |
| `apps.txt` | The target list: name, kind, capture mode, frames, command. |
| `suite.sh` | Every target at 2-3 frames, Mesa references cached, `report.md`. |
| `report.py` | Builds `report.md`/`report.json` from a run directory (re-runnable by hand). |
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
swap with its drawable and virtual time), `GLREF_OURS` (directory holding our
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
- **SDL**: SDL apps (stage 5) are not in `apps.txt` yet. The mechanism covers
  them: SDL_Delay goes through `nanosleep` from libSDL, which is app-side.
