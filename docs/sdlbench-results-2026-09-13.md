# SDL baseline investigation — 2026-09-13

**Baseline status: closed and ready for targeted optimization A/Bs.**
This report accompanies the independent test clients and [running instructions](sdlbench.md).
No Doom, Tyrian, SDL library, or lvdesk implementation was modified by this work.

## Established configuration regression

Kernel #218, built September 13 at 10:50:57 UTC, returned ENOSYS from both
mismatching FUTEX_WAIT and FUTEX_WAIT_PRIVATE probes. The expected result is
EAGAIN: this tests syscall support without blocking.

Git history identifies the regression precisely. Commit `0ea51f8` enabled FUTEX
in the Makefile on September 9; `10821ca` removed the line roughly two hours
later while changing audio polling and adding lockup diagnostics. Its message
does not explain the removal. Every `make linux` regenerates the configuration
from a defconfig that disables futexes, so the missing override persists.

The Makefile now enables FUTEX and checks its effective value after olddefconfig.
Kernel #219 differs in configuration only by FUTEX and its automatically enabled
FUTEX_PI dependency. Its image is 5,955,617 bytes, versus 5,935,129 bytes before:
20,488 additional bytes, with 204,767 bytes remaining in the kernel partition.
It was flashed through the existing locked esptool path and hash verification.
Only the kernel partition was changed. The running kernel's probes confirm support.

## Initial controls and why they are not the final stability result

The initial isolated controls used identical benchmark binaries on both kernels:

- SDL1 MD5: `7132fbba515280e8937bb37c6ab79c0a`.
- SDL2 MD5: `a930d0b751bc1355a048646bfabd3e29`.
- lvdesk MD5: `d013d999d596f7c8ffeddf3f9b5327b5`.
- Installed SDL1 library MD5: `5339279768f42aa7fff8fa4e32ab6fab`.
- Installed SDL2 library MD5: `d50d5a2dfb894dfbb319a5e4be5486a7`.

Both initialized video plus the optional timer subsystem. Two runs of 24 measured
iterations followed warmup; the compositor restarted between invocations. No
remote commands or screenshots ran inside their timing intervals.

SDL2's indexed-frame workload was 100.62/104.65 ms before and 46.08/45.53 ms after.
The two-pass workload was 354.77/343.84 ms before and 142.79/144.87 ms after.
Chocolate Doom's source explicitly initializes SDL_INIT_TIMER, so that subsystem
is relevant to its path. These remain synthetic submission workloads, not Doom FPS.

SDL1's indexed-frame workload was 55.62/58.83 ms before and 114.42/55.24 ms after.
The first post-boot result is an outlier, not evidence of a persistent slowdown.
It had no major faults during that case, so blaming swap solely from its elapsed
time would be unjustified. Its cause was not established. This prompted a stronger
eligibility gate and targeted repeats instead of deleting the inconvenient sample.

Raw initial controls: `artifacts/sdlbench/before-isolated/` and
`artifacts/sdlbench/after-futex/`. Earlier smoke/visual runs are exploratory and
must not be pooled with these or the later gated controls.

## Methodological changes

The final harness requires completed association, a stable IP and three quiet CPU
windows before every invocation. It checks association and available carrier
change counters afterward. Warmup now lasts at least five iterations and 250 ms.
Future game-oriented profiles do not initialize the optional SDL timer subsystem
unless requested. SDL1's timer thread wakes every millisecond even when no timers
are registered; OpenTyrian does not request that subsystem during initialization.

Doom-style direct surface writes, SDL conversion/upload, and Tyrian-style
application-side palette expansion are distinct cases. Synthetic sprite work is
not presented as a measurement of Tyrian's actual RLE decoder or game simulation.

## Gated futex comparison: five runs per configuration

All frame cases used 60 measured iterations, the same executable/library hashes,
fresh compositor instances and the eligibility gate. Three runs on #219 were
followed by a fresh boot of #218 for five runs, then a fresh boot restoring #219
for two more runs. This brackets the control against an order effect; it is not
five independent cold boots per configuration. No timed case recorded a major
page fault. Kernel #219 was restored after the control.

| Indexed frame workload | Futex off: median mean ms (range) | Futex on: median mean ms (range) | Interpretation |
|---|---:|---:|---|
| SDL1, timers enabled | 59.072 (57.931–59.896) | 54.415 (53.000–55.614) | 7.9% lower time; no repeated regression |
| SDL1, timers disabled | 21.647 (21.441–22.045) | 20.833 (20.219–22.089) | Small median difference, overlapping ranges |
| SDL2, timers enabled | 101.962 (100.565–108.978) | 45.216 (44.791–45.789) | 2.25× synthetic workload throughput |

These are API submission workloads, not physical panel FPS or game timedemos.
The earlier SDL1 114.42 ms sample remains in the exploratory data; its cause was
not established, and it did not recur in this comparison.

| Workload | Median per-run p95 ms, off → on | Worst individual frame ms, off → on |
|---|---:|---:|
| SDL1 with timers | 75.665 → 70.208 | 105.908 → 75.724 |
| SDL1 without timers | 37.046 → 33.186 | 47.792 → 48.097 |
| SDL2 with timers | 128.967 → 57.195 | 145.757 → 80.806 |

The separate semaphore/mutex test spends approximately 300 ms waiting. Median
process CPU time fell from **291.879 to 8.588 ms for SDL1**, and from
**287.594 to 8.876 ms for SDL2**. These probes omitted the optional timer subsystem
on both kernels and completed their worker successfully in every run.

The restored boot supplied a direct readiness counterexample: association was
already COMPLETED, but the first eight CPU windows reported 0% idle, from uptime
77 through 101 seconds. The gate waited through this and subsequent recovery;
only after three quiet windows did it launch the benchmark. That SDL1-with-timers
run measured 53.291 ms. This demonstrates why a login prompt or associated Wi-Fi
alone is insufficient; it does not identify the cause of the earlier outlier.

Current benchmark MD5s are `7c6714f486c50588151822dfa9db7b91` (SDL1) and
`ba8f003f416501926aeef142a6758a87` (SDL2). Source IDs are embedded in every run.
Raw logs are in `artifacts/sdlbench/gated-futex-{on,off,off-extra,restored}/`.
The retained [CLI records](benchmarks/sdlbench-2026-09-13.jsonl) preserve individual
runs, stage times, tails, checks and failure status without aggregating failed runs.

## Failures found by the tests

The 640x400 SDL2 exploratory run completed its client-side pixel checks and timing
loops, but lvdesk was absent afterward. The harness records exit status 90 and
rejects that run. Its raw log is
`artifacts/sdlbench/extended/sdl2-scale640-r1.log`. The cause has not been established;
no performance number from that failed run is accepted as a healthy baseline.

An earlier hardware screenshot showed overlapping/stale windows, and lvdesk was
subsequently found absent. It is retained as diagnostic evidence, not a successful
visual validation. This motivated fresh-compositor runs and the survival check.

Audio callback cadence also needs attention beyond futex support. In the exploratory
mixed SDL1 workload the requested 23.22 ms callback period averaged 43.80 ms, with
a 144.86 ms maximum gap. SDL2 averaged 23.41 ms, with a 58.04 ms maximum gap. These
are callback scheduling observations under different workload durations, not
speaker-quality or ALSA XRUN measurements. The full logs retain counts and formats.

## Harness findings

- `runsh.py` formerly killed its watchdog shell without terminating its sleep
  child. Short commands left sleeping processes resident until their original
  timeout. It now terminates and reaps the sleep and waits for watchdog cleanup.
  A deliberate five-second overrun emitted RS_TIMEKILL; the following command
  succeeded and showed only its own current watchdog sleep.
- `deploy.py` also returned console text as a CLI exit status, falsely reporting
  failure after successful small deployments. It now returns an integer and
  checks the destination MD5 on the console path.
- Hardware JPEG capture is not the entire screenshot cost. The wrapper records
  several frames into a 512 KB ring and transfers the result.
- The first Wi-Fi fetch failed in Python and triggered a second recording plus
  serial base64 fallback: 103,708 bytes transferred to obtain a 25,927-byte image.
  A curl fallback now attempts the same board URL before serial fallback.
- The recorder retains its vmalloc ring after STOP. Only a later START replaces
  it. A screenshot therefore changes subsequent memory availability, even after
  encoding and transfer end. Observer comparisons prime the same ring in every arm.
- The shipping kernel has DEBUG_FS disabled. No physical scanout completion or
  PPA-operation count is inferred from a fast SDL return or a “ZEROCOPY” log.

## Generic path coverage and game-shaped costs

The final gated smoke completed **81 measurements**, with successful client checks
and compositor survival: SDL1 at depths 8/16/32, SDL2 renderer, and SDL2 window
surface. These are one 24-iteration run per mode, so they establish coverage and
cost decomposition, not five-run performance claims. Timers and audio were off.

| Case | Total ms | Producer / application conversion ms | Flip or Present return ms | Event pump ms |
|---|---:|---:|---:|---:|
| SDL1 direct surface, 8-bit (PrBoom-shaped) | 17.796 | 3.374 | 12.848 | 1.475 |
| SDL1 direct surface, 16-bit | 23.321 | 6.327 | 16.005 | 0.832 |
| SDL1 direct surface, 32-bit | 38.038 | 8.169 | 28.382 | 1.436 |
| SDL1 synthetic sprites + application expansion, 8-bit | 21.905 | 3.556 + 2.728 | 14.044 | 1.550 |
| SDL1 synthetic sprites + application expansion, 32-bit | 40.653 | 3.636 + 8.360 | 27.542 | 1.089 |

SDL2's indexed texture workload measured 44.408 ms: 10.616 ms source-copy plus
indexed conversion/upload, 16.689 ms clear/copy/flush, 15.909 ms Present return,
and 1.160 ms event pump. Its two-pass scaling case measured 141.607 ms. These
measurements identify candidate stages for improvement; they do not measure the
Doom renderer or Tyrian simulation. Synthetic CPU code runs from SD-backed memory,
whereas installed games/libraries can use XIP. Do not transfer application-loop
costs directly to real games without calibration.

The CLI can select one case for the next change. Dedicated sprite-count curves,
extra game-profile repetitions and numerical observer sweeps were **not run**;
they were cancelled once the baseline was judged sufficient. Their commands remain
available when a concrete optimization calls for them. There is no new measured
Doom FPS, Tyrian FPS or screenshot-interference percentage in this report.

## Visual validation completed separately

Both independent visual clients displayed the expected red/green/blue bands,
white rules and yellow square. SDL1 used an indexed 8-bit video surface; SDL2
used indexed-to-ARGB conversion into a locked texture, then RenderCopy/Present.
Hardware DRM/JPEG capture returned three distinct frames per client over Wi-Fi.
Inspection of the first and last frames confirmed the square moved, with correct
colors and geometry. Both clients exited cleanly and lvdesk remained alive.

- SDL1: [first JPEG](benchmarks/sdl1-visual-0.jpg), [last JPEG](benchmarks/sdl1-visual-2.jpg), [MJPEG](benchmarks/sdl1-visual.mjpeg).
- SDL2: [first JPEG](benchmarks/sdl2-visual-0.jpg), [last JPEG](benchmarks/sdl2-visual-2.jpg), [MJPEG](benchmarks/sdl2-visual.mjpeg).

Transfers were 69,122 and 67,106 bytes respectively; first JPEGs were 23,046 and
22,367 bytes. These checks took place outside performance timings. They validate
these display paths, not every blend/scale/palette case or physical frame delivery
at maximum submission rate. The successful fetches used Python Wi-Fi directly;
the curl fallback was not exercised.

A failed orchestration attempt held the serial lock in its parent while launching
the screenshot subprocess. That is a host ownership error, not a board failure.
The retry used separate existing CLI invocations. The screenshot wrapper now
preserves a nonzero runsh error instead of hiding its stderr and trying unrelated
capture fallbacks.

## Next implementation boundary

The user's preference is standard Linux interfaces and existing library backends,
with another SDL ABI shim a last resort. The next bounded target is the permanent
scanout/GEM mapping proof through the existing DRM path, followed by a capability
audit tied to real consumers. The [revised architecture notes](performance-opportunities-2026-09-13.md#baseline-closed-revised-direction-september-13-evening)
record this direction and the distinction between kernel acceleration and a
userspace software renderer. Do not reopen a broad baseline campaign before
making an actual change.


## Final board and verification state

After visual validation, the board was rebooted through the existing esptool
reset path to clear the retained capture ring. Final CLI verification reports
kernel #219, a live lvdesk process, and VmallocUsed back at 400 KB. No screenshot
was taken after that reset. Benchmark/library hashes are retained in the manifest;
client applications, installed SDL libraries and the pre-existing lvdesk edits
were not changed by this work.

The final cross-build completed without compiler warnings. Python compilation,
shell syntax and git whitespace checks passed. Console deployment MD5 verification,
normal/forced watchdog cleanup, gated controls, the final path smoke and the two
hardware visual captures were exercised on the board. The observer script and
curl fallback remain unexercised; they are not presented as measured fixes.
