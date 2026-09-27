# GL performance campaign, 2026-09-26/27: what I am doing and why

## The goal

Make stock GL applications fast on this board by improving the platform,
never the applications. The benchmark is GLQuake, meaning stock QuakeSpasm
0.96.3 on SDL 1.2 running `timedemo demo1`. glxgears is the regression check.

The rules that shape everything below:

- **The apps stay stock.** No source patch, relink, build define or per-app
  config. Only the app's own switches are used:
  `-mixspeed 11025 -zone 384 -heapsize 12288 -width 320 -height 240`.
- **Apps run from the SD card.** Only our platform code (libGL, xlite,
  lvdesk, the kernel) may go in XIP flash or RAM.
- **Nothing gets slower.** Every change that ships passes glxgears and the
  other regression checks.
- **Measure before claiming.** Every claim needs fresh boots, arms
  alternated, and the spread reported. A single run is not evidence,
  because the same build varies about ±5% from boot to boot.

## How each lever is chosen

1. **Profile the real thing on the board.** The hart0 PC sampler (`h1s`)
   takes 8,000 samples during the timedemo, and `scripts/board/gq-prof.py`
   resolves them by library and function.
2. **Model it on the host.** The QuakeSpasm GL trace is replayed on the
   RV32 instruction counter (`gl/bench/qsreplay.sh`), with cache models
   (`gl/bench/qemu/cachesim.c`, `icsim.c`). Every change must be
   bit-identical, gated by `gl/tests/run-3a.sh` and `qsr-fused`.
3. **A/B on the board** with fresh boots, 3-4 runs each, alternated.
   **Only a board win ships.**
4. **Record the numbers** beside the code or in `artifacts/gl/phase6/*`,
   and commit them, whether the change won or lost.

## Results so far (GLQuake fullscreen, fresh boots)

| Step | fps | Why it worked |
|---|---|---|
| Start: phase 3a library in XIP flash | 4.3-4.4 | |
| Phase 4+5 library: multitexture + combine, fused fillers, 8-bit texture storage, Mesa-style rounding | 6.1-6.3 | One pass per world surface instead of 2-3, and fewer instructions per pixel (about 35% fewer on the host replay) |
| libGL loaded from SD instead of XIP | 7.3-7.5 | libGL grew to about 400 kB and its hot code overflowed the I-cache. Every miss from XIP is an 80 MHz SPI-flash read; loaded from SD, the code runs from PSRAM |
| `vm.swappiness` 10 to 60, `vm.min_free_kbytes` 1024 to 512 | 8.0-8.1 | At swappiness 10 the kernel threw out code pages (libGL's and QuakeSpasm's, now on SD) before heap pages. Page faults fell from about 6.5k to 2.2k a run |
| World filler `zf8_wnn_p0` rewritten | 8.5-8.9 | It was 23% of all CPU. Now the depth test comes before the texel work, palette unpacking is cheaper and the loop is unrolled. 56 to 45 instructions per visible pixel |
| **Tier 2**: world filler once per triangle, clamp-free repeating texel fetch | **8.6-8.9 (mean about 8.8)** | Triangle setup went from 2,100 to 701 instructions |

**That is about 2x in total, and it is what the board runs now.** The
shipped state is:

- kernel #393;
- libGL tier 2, md5 91771d02, on the SD root;
- swappiness 60 and min_free 512;
- lvdesk e83b9d67.

## Measured and rejected (with the reason, so nobody retries them blind)

| Lever | Board result | Why it didn't pay |
|---|---|---|
| Hot libGL code copied to RAM and locked (RAMTEXT), library in XIP | 7.0, 6.5 against 7.3-7.5 | The 10% of time left in flash (API wrappers, PLT) costs more than the copy saves |
| Two-hart rasteriser (`S31GL_THREADS=1`) | No clear gain before the paging fix, about 3% (noise) after it | The game was waiting on paging, not on CPU. It stays in the library, off by default |
| Zero-copy fullscreen present (P2) | 42 against 47.9 fps (glxgears) | Switching the scanned-out buffer is a full display commit, about 2 ms, which costs more than the 192 kB copy. It would need a commit-free flip in our driver, and that is worth little for GLQuake |
| Kernel: HRTICK off, SD keepalive off | Inside the noise | The tick is already 100 Hz, and tickless idle hangs this board |
| `CONFIG_SCHED_MC` | Inside the noise | Nothing is idle for it to find, and it doesn't know the lent core is slower |
| zram | 5.4 fps | Its pool costs about 1.2 MB of the RAM the game is short of |
| Tier 3: lookup tables in place of arithmetic in the blend fillers | About 2% slower, despite 4.7% fewer instructions | The new tables are D-cache loads from PSRAM, and there is 2.7 kB more hot code |
| Tier 5: page-colouring libGL's hot data | D-refills flat to ±3% across boots | The lever isn't real on the board |
| Tier 6: ordering hot code, colour-controlled RAM text | 8.50 against 8.58, and 8.38 | libGL's page colours don't explain the slow boots |

## What we learned about this machine

- **On this board code size costs time as well as instruction count.**
  Hart 1's I-cache is 32 kB, 2-way and physically indexed; the vendor docs
  say 16 kB, which is wrong. GLQuake's fps tracks its refill count per frame
  at r = -0.98 over 14 boots. libGL's per-frame code working set is about
  49 kB, larger than the cache. So a change that saves instructions but adds
  code or tables can lose.
- **The boot-to-boot spread (about 8.4-9.0 fps) is I-cache refills**, not
  D-cache and not CPU placement. The remaining suspects are QuakeSpasm's
  own text and the kernel's flash-resident code sharing the cache.
- **Paging was the hidden limit.** Even at 7.5 fps CPU0 sat 28% idle while
  the game waited on the SD card.
- **Hazard:** code the CPU writes into RWX/anonymous pages has frozen the
  whole chip three times. Any new RAM-text range gets a short sanity run
  before an A/B.

## What is running now

**Tier 7**, host-side. Every change is judged on instructions *plus* the
I-cache and D-cache models, and kept only if the combined modelled cost
falls. The targets, in order:

1. A second pass on the world filler, which is still 20% of CPU.
2. QuakeSpasm's per-frame dynamic-lightmap re-uploads (`t8_l8_pass`, 1.2%),
   turned into a straight copy.
3. Triangle setup, the vertex path and the alias path, only if the models
   agree.
4. Shrinking the shipped code: removing dead variants and test arms, and
   moving cold paths out of hot functions.

After that the build gets a board A/B, 4+ fresh boots each, and ships only
if it wins with no glxgears regression.

## What comes after

- **Where the time goes now (tier-2 profile, `artifacts/gl/glquake/prof9`):**

  | Component | Share of CPU |
  |---|---|
  | libGL | 53% |
  | ... of which the world filler | 20% |
  | ... of which the vertex path | 3.5% |
  | kernel scheduler/tick/timer | 13% |
  | QuakeSpasm itself | 8% |
  | lvdesk's frame copy | 3% |

- **The I-cache lead:** QuakeSpasm's text and the kernel's flash code
  sharing hart 1's I-cache, which is what makes slow boots slow.
- **Quality:** fast filtered fillers would make QuakeSpasm's own trilinear
  default affordable. Today it costs about 2x, so the library defaults to
  nearest.

## Where the detail lives

- `docs/gl-plan-2026-09-25.md`: the plan, its status sections, and the
  to-do queue.
- `artifacts/gl/phase5/`: the library round (multitexture, fillers,
  precision, memory).
- `artifacts/gl/phase6/`, one directory per lever:
  - `zf8/`, `tier2/`, `tier3/`, `tier4/`, `tier5/`, `tier6/`: filler rounds
    and cache work;
  - `ramtext/`, `THREADS.txt`, `ZEROCOPY.txt`, `GEOMETRY.txt`: the
    individual levers.

  Each holds its board result.
- `artifacts/gl/glquake/`:
  - `arms/`: raw per-run data;
  - `prof8/`, `prof9/`: profiles;
  - `tick/`, `paging/`: the kernel and memory levers.
- `docs/current-state.md`: the GLQuake paging and tick sections.
- `buildroot-external/board/esp32-s31/overlay/etc/sysctl.d/99-s31-memory.conf`:
  the swap settings, with their numbers.

## Git note

Commit a97b9851 (this document's first version) also carries the revert of
tier 3's eight tinygl source files to their tier-2 state (9ecf5f86). The
tier-7 agent had staged that revert, and the documentation commit picked it
up. So the tier-3 revert is recorded under that commit, not under tier 7's
e567cb01. From here on, documentation commits name their paths
explicitly (`git commit -- <path>`).
