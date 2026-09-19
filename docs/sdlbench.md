# Stock SDL baseline benchmarks

These are independent diagnostic clients, dynamically linked to the installed
SDL 1.2 and SDL2 libraries. No game source or library implementation is changed.
They are performance probes, not a complete SDL conformance suite.
The small probe executables run from `/root` on SD; the installed SDL libraries
and lvdesk retain their normal XIP mappings. Synthetic application loops therefore
do not reproduce a large game's instruction-fetch behavior or working set.

## Baseline status and when to rerun

The September 13 baseline is closed; see [measured results](sdlbench-results-2026-09-13.md).
Use the relevant cases to test an implementation hypothesis. Do not repeat every
suite before starting an optimization. Full coverage is a regression check after
an actual change, not a prerequisite for more research.

For example, one quiet indexed-frame case rather than a complete sweep:

```sh
python3 scripts/board/sdlbench.py --isolate --require-futex --sdl 1 --case indexed_frame --frames 60 --repeats 5
python3 scripts/board/sdlbench.py --isolate --require-futex --sdl 2 --case indexed_frame --timers --frames 60 --repeats 5
```

## Running

```sh
./docker/build.sh 'sh rootfs/build-sdlbench.sh'
python3 scripts/board/sdlbench.py --deploy --isolate --require-futex --suite quick --frames 24 --repeats 5
python3 scripts/board/sdlbench.py --isolate --require-futex --suite full --frames 24 --repeats 5
python3 scripts/board/sdlbench.py --isolate --require-futex --suite profiles --frames 60 --repeats 5
python3 scripts/board/sdlbench-summary.py artifacts/sdlbench/RUN/results.jsonl
```

`--isolate` restarts lvdesk before each invocation. Use it in a dedicated test
session: it closes desktop windows. It does not rewrite desktop configuration.
The runner requires a live compositor and checks that it survives client exit.
Without `--isolate`, it uses the current desktop, whose windows and memory state
are part of the result. Neither mode reboots or flashes the board.

The runner deploys and verifies binaries before testing. It transfers the command
script before starting the test, directs client output to a board file, sends no
commands during execution, and retrieves results afterward. No screenshots run
inside timing intervals. Before each invocation, the board-side settle gate
requires three consecutive two-second windows with completed Wi-Fi association,
an unchanged IPv4 address, and at least 85% Linux CPU idle. It fails after 60 seconds. This does not measure
hart0 CPU utilization; completed association is the available radio readiness check.
Association is checked again after the test, and a changed network carrier
counter rejects the run. This reduces startup and reconnection contamination;
it does not promise that arbitrary background work can never happen. A 180-second client alarm bounds hung APIs; the host
watchdog has a longer deadline. A timed-out or incomplete run is a failure.

CLI JSONL includes kernel identity, loaded library mappings, effective SDL
version/backend, actual surface geometry/format, futex probes, wall-time
percentiles, process user/system CPU time, faults, context switches and sampled
compositor CPU ticks. Startup and warmup (at least five iterations and 250 ms by default) are excluded. Records
are buffered; no per-frame printing or framebuffer hashing runs inside timings.
For kernel A/B comparisons, boot each kernel arm, keep benchmark and library
hashes identical, and repeat after settling; bracket the old kernel with the
restored new kernel to check for an order effect. Report the spread and frame
tails, retaining failed and exploratory runs separately. Do not compare a cold
startup with a primed loop.

The timer-only case exposes instrumentation overhead. CPU accounting around
very short cases includes the cost of the resource-accounting calls; do not
interpret sub-millisecond CPU differences as application work.

## Coverage

- SDL1: indexed-to-RGB565/XRGB conversion, fill, surface copy, alpha/color-key
  sprites, full/dirty updates, Flip, palette-only changes, and changing indexed
  frames at requested depths 8/16/32.
- SDL2: window surfaces, indexed-to-ARGB conversion, LockTexture + copy,
  LockTexture + indexed blit, UpdateTexture, clear/copy, nearest/linear scale,
  alpha/color modulation, presentation, target-texture two-pass scaling, and
  IYUV upload/conversion/presentation.
- Full suite: fullscreen, 640x400 SDL2 scaling, audio-enabled execution, and
  disabling MIT-SHM on the SDL1 control arm.
- Runtime: optional `--timers` subsystem, idle-wait cost, bounded mismatching private/shared futex waits (supported kernels
  return EAGAIN without blocking), SDL semaphore/mutex contention, audio callback
  cadence using S16 silence through the actual selected audio device.

Renderer primitives explicitly flush SDL's command queue, so their results
include execution rather than just appending commands. They still do not fence
physical panel scanout. Changing frames prevent an unchanged-image shortcut
from masquerading as presentation throughput. Audio gaps are scheduling
observations, **not** direct ALSA XRUN counts or proof of audible quality.

## Relating the probes to Doom and Tyrian

The target is general SDL compatibility. Game-shaped cases supplement the
primitive and runtime tests; they do not replace them or authorize client patches.

| Client behavior | Probe | Where a compatible implementation can help |
|---|---|---|
| PrBoom-style direct writes to an SDL1 video surface | `doom_frame`, depths 8/16/32 | Surface storage, palette handling and Flip/presentation; application rendering remains application work |
| SDL1 offscreen surfaces and sprite blits | conversion, alpha, color key, `sprite_frame` | Compatible SDL blits, fills and presentation |
| SDL1 dirty rectangles and palette animation | `present_dirty32`, `palette_only` | Damage handling and palette-only invalidation |
| SDL2 streaming textures | LockTexture, UpdateTexture, `indexed_frame` | Upload, format conversion, render commands and presentation |
| SDL2 render targets and scaled output | nearest/linear, `target_two_pass` | Compatible scaling and render-target operations |
| SDL2 window-surface applications | `surface_frame`, `surface_dirty32` | Surface conversion and dirty presentation |
| Multimedia / threaded applications | YUV, audio, semaphore/mutex, idle/timers | Format conversion, blocking waits and callback scheduling |

By default the test initializes video without SDL's optional timer subsystem.
Use `--timers` explicitly to measure it. SDL1's Unix timer thread wakes every
millisecond even without registered timers; OpenTyrian's initialization does not
request it. Chocolate Doom does request SDL_INIT_TIMER. The initial futex A/B
results used video plus timers on both kernels; later game profiles use their
stated subsystem flags.

`doom_frame` writes synthetic changing pixels directly to the video surface and
separates that producer cost from SDL_Flip and the event pump. This avoids charging
an extra SDL_BlitSurface to a direct-rendering game such as PrBoom. The PrBoom
2.5.0 source in the build initializes SDL video without SDL_INIT_TIMER; its audio
subsystem is separately enabled. All subsystem choices must match the target
application when interpreting a result.

`indexed_frame` supplies changing precomputed pixels rather than rendering a 3D
scene. SDL1 measures source copy, surface blit, update return and event pump as
separate stages within the same frame. SDL2 measures source-copy/index-conversion
upload, clear/copy, Present return and event pump. This is the platform-facing
shape of an indexed game, not a replacement timedemo.

`tyrian_frame` combines a synthetic indexed background and configurable 16x16
transparent sprites with an application-side palette loop and SDL_Flip. It is
separate from `sprite_frame`, which uses SDL_BlitSurface for conversion. The
actual OpenTyrian source draws sprites directly into indexed pixels and calls
its own selected scaler before SDL_Flip. Therefore a library shim cannot claim
to accelerate all of that CPU work. The synthetic code is independently written;
it is not extracted or modified game code and does not reproduce Tyrian's RLE
sprite decoder, simulation or advanced scalers.

Set `SDLBENCH_SPRITES=0`, `16`, `64` or `128` to build a cost curve. The profiles
suite does this automatically. Individual runs can be a few seconds:

```sh
DISPLAY=:0 SDLBENCH_DEPTH=8 /root/sdlbench1 --case indexed_frame --frames 60
DISPLAY=:0 SDLBENCH_DEPTH=32 SDLBENCH_SPRITES=64 /root/sdlbench1 --case tyrian_frame --frames 60
DISPLAY=:0 /root/sdlbench2 --case indexed_frame --frames 60
DISPLAY=:0 /root/sdlbench2 --case target_two_pass --frames 24 --width 640 --height 400
```

Use measured stage costs to estimate the platform portion of a frame budget
(28.57 ms at 35 Hz; 16.67 ms at 60 Hz). For a measured real-game baseline T,
`T_new ≈ T - platform_old + platform_new` is only a conditional estimate with
matching format, resolution, damage and synchronization. It assumes those costs
are additive and other work remains unchanged. Present can return before the
panel changes, so `1000 / mean_ms` is submitted-workload throughput, not verified
displayed FPS. A real unchanged Doom timedemo remains the calibration point;
Tyrian estimates must retain uncertainty about its application CPU work.
In particular, do not substitute the synthetic sprite or palette-loop time for
the unchanged game's CPU time without calibration: code placement, algorithm,
compiler choices and memory pressure differ.

## Visual validation and observer effect

Build also produces `sdlbench-visual1.bin` and `sdlbench-visual2.bin`. These draw
red/green/blue bands, white horizontal rules and a moving yellow square for
60 seconds by default. SDL1 accepts a second argument for depth (8, 16 or 32);
SDL2 converts indexed pixels into a locked ARGB texture and renders it. Capture these in separate validation runs using the
existing hardware JPEG tool, never the raw framebuffer/serial path while timing.
Set `SCREENSHOT_KEEP_MJPEG=1` to retain the already-transferred frames beside the
JPEG for motion checks, with no additional board capture or transfer.

Hardware JPEG is cheaper, not free. The screenshot wrapper records multiple
frames into a 512 KB ring, drains them, launches a file server and transfers
output. Network failure can trigger another recording and base64 serial transfer.
The encoder latency alone is not the cost of the whole screenshot command.
The current recorder retains its ring after STOP; a later START replaces it.
Restarting only lvdesk does not remove that allocation. The observer experiment
primes the same 512 KB ring before every arm to control this persistent cost.

```sh
python3 scripts/board/sdlbench-observer.py --frames 360 --repeats 2
```

**Status: the observer experiment is implemented but has not been run.** Its
queued trials were cancelled when the baseline was closed; no interference
percentage is claimed. When explicitly needed, it compares quiet execution, a fixed burst of CLI commands/output, hardware
JPEG capture, and capture followed by serial base64 output. These are deliberate
interference experiments, separate from baseline timing. They do not measure
Wi-Fi transfer, every wrapper startup cost, or prove a passive serial reader has
zero overhead.

The current shipping kernel has DEBUG_FS disabled. These clients consequently
do not claim PPA-operation or physical display-completion counters. Do not enable
a larger diagnostic kernel merely to make those metrics appear in a baseline.
Visual checks and renderer readback complement timings; neither alone proves
that every submitted frame appeared on the panel.
