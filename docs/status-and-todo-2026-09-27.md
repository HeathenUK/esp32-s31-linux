# Status and to-do, 2026-09-27 (~15:10)

This is a single place for what is on the board now, what is in progress
and what is queued. The detail lives in the documents linked from each
line.

## On the board now (shipped, verified)

| Component | Version | Notes |
|---|---|---|
| Kernel | **#401** | #393 + patches/0074 (native 22050/24000/32000 codec rates; 96000 removed, since it actually ran at 78125). `images/ship-401-xipImage`. **The build volume's `build/xipImage` is a rejected test kernel: never run a bare `make sync-images`.** |
| XIP image 1 | fa39f3b2 | lvdesk 4b06dbec, s31route b651fa15, libs31fp 077a697a |
| XIP image 2 | 6e5bb094 | unchanged |
| libGL | 01aa340f (tier 7) | on the SD root `/usr/lib`, not in XIP |
| Memory settings | swappiness 60, min_free_kbytes 512 | 99-s31-memory.conf |
| Menu | 225 entries (card md5 fbcf23fe) | full size x mode x rate variants per app; lvdesk MENU_MAX 512 |
| s31fp v2 | system-wide LD_PRELOAD | via /etc/profile and lvdesk. Trampoline by default; copy-in-place opt-in (`S31FP_COPY=1`); kill switch `S31FP=0` |

## Results so far (fresh boots, quiet runs)

**GLQuake (QuakeSpasm)** has gone from 4.3 to about 8.8 fps. Each step, with
its gain:

| Step | fps |
|---|---|
| Starting point | 4.3 |
| Phase 4+5 libGL | 6.2 |
| libGL loaded from SD instead of XIP | 7.5 |
| Swappiness and min_free tuning | 8.0 |
| World filler | 8.7 |
| Tier 2 | 8.8 |
| Tier 7 | about equal to tier 2, with less code |

Details: docs/gl-performance-campaign-2026-09-27.md.

**Audio**, all of it ear-checked by the owner ("All sound good"):

- **44.1 kHz SDL 1.2 apps with small rings:** they were silent (the buffer
  rounding broke the ALSA setup with EINVAL); they now play.
- **32 kHz:** it was 19% slow; it is now correct and native on the codec.
- **22.05/24/32 kHz:** these now run natively on the codec.
- **Other rates:** they go through s31route's own polyphase resampler,
  with images 72-85 dB down against 6-62 dB before.
- **Cost:** mono and converted streams take 30-50% less CPU than before.

Details: artifacts/audio/first-principles-2026-09-27/{REPORT,IMPLEMENT}.md.

**sdlquake: 21.5-21.9 fps** on quiet runs, against a recorded band of
19.2-20.1. The earlier "dips" were our own harness polling the board.

**s31fp v2** is bit-exact everywhere: results, fflags, frm and NaN
behaviour all match libgcc.

- **OpenTyrian title song:**
  - libgcc: 44 µs/sample;
  - trampoline (shipped default): about 22 µs (0.97 core);
  - colour-placed copy-in-place: about 20 µs (0.88 core).
- **Lent hart:** CPU1 is 20-25% slower for every arm.
- **Heaviest song:** it still needs about 1.3 cores.

Details: rootfs/s31fp/v2/V2-REPORT.txt.

**Measured and rejected, each recorded with its numbers:**

- RAMTEXT for libGL;
- a two-hart rasteriser;
- zero-copy present (lvdesk now has it off by default, `LVDESK_ZC=1`
  opts in);
- HRTICK;
- the SD keepalive;
- CONFIG_SCHED_MC;
- zram;
- tier 3, tier 5 and tier 6;
- kernel FASTFN for GLQuake;
- s31route priority, pinning and headroom.

## In progress (agents running)

1. **s31fp regression check and copy-in-place crash hunt** (board, about
   1 h).
   - **Regression check:** in the shipped state, prboom fullscreen read
     39.9 fps (band 41.0-46.4) and sdlquake 20.5. The agent runs quiet
     fresh-boot A/Bs with and without `S31FP=0`.
   - **Crash hunt:** one prboom SIGSEGV happened at level load under
     `S31FP_COPY=1` (1 in 4 runs). The agent reproduces it, maps the
     faulting PC to the patched code, fixes it, and then makes
     copy-in-place the default again.
2. **Generic S31-aware interception in libs31fp** (host, then board):
   - **A1:** CPU-safe memcpy/memset/str* for every process (the lentcpu
     rseq dispatch);
   - **A2:** a user-space clock from the `time` CSR (there is no vDSO);
   - **A3:** a survey of the SDL fast paths.
3. **Bluetooth sink-switch stall fix in s31route** (host, then board).
   Switching `/run/s31-sink` to the loopback mid-stream, with no A2DP
   reader, stalls the app. s31route must never block on a sink that no
   one drains.

## Agents stopped by the weekly usage limit (~15:25; it resets 1 Oct 04:00)

1. **s31fp regression check.** Quiet fresh-boot A/B, preload on (default)
   against S31FP=0:
   - prboom 320x240 fullscreen: 40.3 fps on, 39.7 fps off. **No s31fp
     cost.** The low prboom number reads the same with the preload off, so
     it comes from the boot or harness state, not s31fp.
   - prboom's 5-6 patched helpers have only 18 call sites and none of them
     is in a per-pixel loop.
   - sdlquake: 20.2 fps on. The coordinator is running the off/on pairs
     (sqab).

   **Resume:** the copy-in-place crash hunt (a prboom SIGSEGV at level
   load under S31FP_COPY=1, 1 in 4 runs).

   **Hypothesis to test:** in trampoline mode the v2 code runs from
   libs31fp in XIP flash, so try the library on the SD root.
2. **Generic interceptions, v3 WIP** (commit 7da299bf, rootfs/s31fp/v3):
   - A1 (string routines) and A2 (user-space clock) are built and pass on
     qemu. The agent was replacing TLS with a pthread key when it stopped.
   - **Resume:** the board tests (exec cost, correctness under migration,
     clock drift against the syscall, PIE bounce counts).
3. **Bluetooth sink-switch fix.** Committed (682776d8) and staged, not
   flashed.
   - **Resume:** build the overlay; gate with scripts/xipdiff.sh (image 1
     may change only in the s31route plugin, image 2 only in s31-bt); flash
     both images; run the arms that do not need headphones; leave sinkear.sh
     ready for the owner.
   - Baselines: images/sinkfix-base/.

## Queued (in order)

1. **sdlquake frame-dip investigation.**
   - Step 0 is to rule out our own harness.
   - The target is to make its long-stretch ~25 fps the average.
2. **Warm `reboot` hang.**
   - After deploying the menu, `reboot` stayed SILENT for more than
     3 min; a hard reset booted fine.
   - It has not been reproduced yet.
3. **Next GLQuake levers.**
   - The current profile is artifacts/gl/glquake/prof9.
   - Where the time goes:
     - the world filler, 20%;
     - sched/tick, 13%;
     - the game, 8%;
     - lvdesk's copy, 3%.
   - Code size counts as much as instruction count: hart 1's I-cache is
     32 kB, 2-way and physically indexed.
4. **Fast filtered GL fillers**, so that QuakeSpasm's trilinear default
   becomes affordable. This is a quality item.

## For the owner to test later

**Bluetooth sink-switch fix.** It is committed as 682776d8 and ships to the
board with XIP images 1 and 2 once the s31fp regression check finishes.

To test it:
1. Connect the soundcore Liberty 4 NC.
2. Run `sh /root/afp/sinkear.sh` on the board console. It plays a 24 s
   quiet tone that switches speaker -> headphones -> speaker -> headphones
   at DAC 143.
3. Listen for a stall, a click storm or dropouts at each switch.

## Held for the owner's ruling

- **App-specific bit-exact rewrites.** The first candidate is a fused
  soft-double `operator_output` for OpenTyrian's OPL, which the heavier
  songs need.
- **musl libc, ld.so and libm are off-limits** (owner ruling). The
  interception preloads are allowed.

## Standing rules learned today (also in memory)

- Quiet runs only for performance numbers: harness polling cost 3-5 fps.
- Never roll back an improvement over a suspected regression. Explain it
  fully first, try to fix it while keeping the gain, and report before any
  revert.
- No repeat measurements without a code change.
- GL performance is benchmarked on QuakeSpasm, with glxgears as the
  regression check; other apps get compatibility checks only.
- Never run a bare `make sync-images` while the volume holds a test
  kernel. Confirm every kernel flash with `uname`.
- `git commit -- <paths>` only, so one agent never sweeps up another's
  staged files.
