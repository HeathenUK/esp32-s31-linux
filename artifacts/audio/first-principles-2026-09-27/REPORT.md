# Audio from first principles (2026-09-27)

Two complaints:
- **(a)** OpenTyrian's OPL-synthesised music cannot keep up.
- **(b)** Apps asking for 22.05, 36, 44.1 or 48 kHz underrun.

This study re-derives both from the code and from arithmetic, then measures
them on the board. It does not take earlier conclusions as given, and says
where each one was right or wrong.

**Rules applied to every option:**
- **R1:** no change to an app's source, no relink, no build define.
- **R2:** no per-app config or environment steering. The app's own runtime
  switches are allowed.
- **R3:** never force the app's rate.
- **R4:** DAC 143 / lvdesk volume 40.
- **R5:** no capture.
- **R6:** Bluetooth and Wi-Fi stay on.

**Owner guidance (2026-09-27):** we own the whole stack beneath the app, so
s31route and everything under it are fair game: its output rate, where and
how rates get converted, buffer geometry, threading. The s31fp preload may
be measured, not shipped.

Board: kernel #393, DAC 143 throughout; nothing was flashed or shipped. Raw
data is in `raw/`, instruments in `rootfs/audiofp/`, scripts in `scripts/`.
The report was written by the coordinator from the study agent's hand-back,
because the harness refused the subagent's .md write.

## 1. The audio chain, as the code has it

    app -- SDL 1.2 / SDL 2 ALSA backend (blocking snd_pcm_writei)
      -- "default" = plug -- s31route (ioplug; S16_LE, 1-2 ch, 8-48 kHz, >= 4 periods)
         -- plug:'hw:0,0'  (route mono->stereo; alsa-lib "linear" rate converter)
            -- hw:0,0  ASoC dmaengine PCM (esp32s31-i2s.c)
                 rates 8000/11025/16000/44100/48000/96000; 2 ch; 8192-frame SRAM ring; period <= 1023 frames
               -- AHB GDMA cyclic TX; period-elapsed from HARD IRQ (not a kworker)
                  -- I2S -> ES8389 (64*fs BCLK-clocked)

The inner plug picks the codec rate with `snd_pcm_hw_param_refine_multiple`
(alsa-lib 1.2.15, pcm_params.c:1001): the first integer multiple of the
app's rate that the codec offers, otherwise the nearest rate.

| app asks | codec runs | measured | notes |
|---|---|---|---|
| 11025 / 44100 / 48000, stereo | same rate | RW_INTERLEAVED, direct | plug collapses to hw |
| 22050 | 44100 | MMAP, linear x2 | the ES8389 *has* a 64x 22050 row (patches/0042); only `ESP32S31_I2S_RATES` leaves it out |
| 36000 | 44100 (nearest) | MMAP, period 941, buffer 3764 | non-integer ratio 1.225 |
| 32000 | 96000 (3x) | not measured | a 64x 96000 row exists, but no 32000 row |
| any mono | same rate | route 1->2 | |

No rate is ever forced on the app (R3). The period IRQ runs in hard IRQ
context (esp32s31-ahb-gdma.c:582-655), so the ~8 ms deferred-work hand-off
seen elsewhere on this board is not on the audio path.

## 2. CPU budget per output frame (hart 1 = Linux CPU0, 320 MHz)

| rate | cycles/frame | us/frame |
|---|---|---|
| 11025 | 29,025 | 90.7 |
| 22050 | 14,512 | 45.4 |
| 36000 | 8,889 | 27.8 |
| 44100 | 7,256 | 22.7 |
| 48000 | 6,667 | 20.8 |

The lent hart (CPU1) ran OpenTyrian's synthesis loop at the same speed:
47.5 against 48.0 us/sample.

## 3. What each stage costs (measured)

**Platform path.** This is audio-thread CPU minus callback CPU per app frame,
measured with `sdltone` under light load (`raw/arms-a.txt`).

| geometry | path us/app-frame | share of a core | per callback |
|---|---|---|---|
| 48000 st, 2048-frame callbacks (direct) | 0.73-0.75 | 3.2-3.6% | ~1.5 ms |
| 44100 mono, 2048 (route only) | 1.09 | 4.8% | ~2.2 ms |
| 44100/48000 st, 512 (direct, with the fix) | 2.1 | 9.3-10.1% | ~1.1 ms |
| 36000 mono, 1024 (route + linear) | 2.8 | 10.1% | ~2.9 ms |
| 22050 mono, 1024 (route + linear x2) | 5.1-5.3 | 11.3-11.6% | ~5.3 ms |

- **A fixed cost of ~0.8-1 ms per app write dominates small-period apps.**
  That covers one SDL cycle, one kernel write and one sleep/wake.
- Routing mono to stereo adds ~0.34 us per output frame (~110 cycles).
- **alsa-lib's linear converter adds ~1.85 us per output frame (~590
  cycles).** At a 44.1 kHz output that is ~8% of a core.
- These figures include IRQ time charged to the thread while it runs on
  CPU0.
- Under CPU starvation, xrun-recovery churn inflates the path to 4-13 us
  per frame.

**The 2026-09-25 result** (h1s, TyrQuake at 48 kHz, 2048-frame periods,
1.81% of CPU0) is right for that geometry and **wrong as a general
statement.** Apps with 512-frame periods pay ~10% of a core, and 22.05 kHz
mono apps ~11%.

**Soft double.** There is no D extension. OpenTyrian's helpers are local
copies statically linked into its binary (`l F .text __muldf3`), so no
libc, libm or libgcc change can reach them.

**musl's libm float functions evaluate in double.** `sdltone` calling `sinf`
once per sample spent ~9-10 us per sample in its callback, which is 40-50%
of a core at 48 kHz. With a table lookup plus 300 iterations of synthetic
work it spent 5.4-6.4 us. So one `sinf` costs **~9 us, about 3000 cycles**.

## 4. OpenTyrian, derived then measured

**What it asks for** (src/loudness.c, Buildroot tarball cf5dbeb): 44100 Hz,
**mono**, S16, `samples = 2048`. The only 22 kHz setting, OUTPUT_QUALITY 2,
is a build define (R1). There is no runtime switch.

**What it gets:** SDL 1.2 hands the plugin an 8192-frame ring with
2048-frame periods. The codec runs 44100 Hz stereo, period 512, buffer
8192. Because the stream is mono, the route plugin is in the path.

**The emulator** is DOSBox's OPL2 (Ken Silverman's adlibemu), built with
`#define fltype double`. The shipped binary's `operator_output` makes these
calls per active operator per output sample:
- always: 4x `__muldf3`, 2x `__floatsidf`, 1x `__fixdfsi`;
- in decay or release: also `__gtdf2` and 1 multiply;
- in attack: also 3 multiplies and 3 adds.

**Active operators** (host run of OpenTyrian's own opl.c and lds_play.c,
driven the way its callback drives them; `host-op-activity.txt`):
- mean over 41 songs: 12.7 (range 0.9-17.9);
- title song 36: 11.8.

**Synthesis cost on the board.** `oplbench` uses the same sources, patch and
flags as the shipped binary: `adlib_getsample` has the same size and
`operator_output` the same instructions (`raw/opl.txt`).

| song | libgcc us/sample | x realtime | s31fp us/sample | x realtime |
|---|---|---|---|---|
| 36 (title) | 48.0 | 0.47 (2.1 cores) | 23.9 | 0.95 (1.05 cores) |
| 0 | 57.3 | 0.40 | 31.8 | 0.71 |
| 9 | 53.1 | 0.43 | 27.1 | 0.84 |
| 5 | 70.4 | 0.32 (3.1 cores) | 37.1 | 0.61 (1.6 cores) |
| 36 on CPU1 (lent hart) | 47.5 | 0.48 | 23.9 | 0.95 |

The phase-1 model predicted 49 and 22 us for the title song.

**The stock game at its title screen** (`raw/tyr-*.txt`, 15.3 s windows, one
boot):

| arm | audio thread CPU | game main | lvdesk | xruns/s |
|---|---|---|---|---|
| stock | 95.0% | 25.2% | 29.4% | 7.1 |
| s31fp (test-only LD_PRELOAD) | 92.8% | 25.5% | 29.3% | 12.7 |
| s31fp + audio thread on CPU0, the rest on CPU1 | 93.4% | 25.6% | 29.8% | 13.0 |

- The scheduler already gives the audio thread a whole CPU in every arm.
- The higher xrun rate with s31fp does not mean it is worse: the synth is
  faster, so there are more restart cycles.
- The demand is still above one core.

**The preload's cost:** ten launches of a libSDL client took 0.50 s plain
and 3.02 s preloaded, **+250 ms per exec.** The load-time scan of every text
segment makes a system-wide preload unacceptable, whatever the red line
says.

**Verdict on the 2026-09-20 claim** ("~1.8 cores; ~0.9 with s31fp;
unfixable platform-side"):
- **The numbers are right:** the title song measured 2.1 cores, and 1.05
  with s31fp.
- **Phase 1's "wrong since SMP" is retracted.** With two CPUs the thread
  gets ~95% of one, and still needs 1.05-1.6 cores even with s31fp.
- **Priority and placement cannot create cycles.**
- **Even best-case helpers would not be enough.** Bit-exact helpers are
  estimated, not measured, at ~15 us for the title song (~0.66 core) and
  ~25 us for song 5 (~1.1 cores). They are red-lined anyway.

**Conclusion.** OpenTyrian's 44.1 kHz FM music cannot be made real-time on
this core within the rules. The routes that would work all lie outside
them:
- its own 22 kHz build (R1);
- a core with a D extension;
- a preload plus faster helpers plus an overclock, which would cover most
  songs but not all.

## 5. Ordinary apps at 22.05-48 kHz: the mechanisms, measured

Instrument: `sdltone` (SDL 1.2), 12 s per arm, with two CPU-bound spinner
threads beside it standing in for a game and a desktop.

1. **The s31route EINVAL bug gives no sound at all at 44.1 kHz for rings
   under 3763 frames.**
   - **Cause:** `buf = 4096*44100/48000 = 3763`. No integer period between
     256 and 1023 divides it, so hw_params fails with "Invalid argument".
   - **Who is hit:** every SDL 1.2 app asking for 44100 with
     `samples <= 1024`, because SDL's ring is 2 x samples.
   - **Shipped plugin:** 404-418 underrun messages in 12 s, 0 callbacks,
     silence.
   - **Prototype fix** (round the buffer down to whole app periods):
     **0 xruns**, and sound plays.
   - **Other rates:** at 48 kHz the value is 4096, which works. By the same
     arithmetic 32 kHz (2730) probably fails too; that was not measured.
2. **Under contention, CPU share decides it.** Once the audio thread
   (callback plus path) needs more than ~2/3 of a CPU beside two hogs:
   - every 44.1/48 kHz geometry with 512-frame periods underruns, 160-270
     xruns in 12 s;
   - nothing rescued it: RR priority 10, a 1 ms EEVDF slice, pinning to
     CPU0 and extra headroom (D) were all tried;
   - 2048-frame periods at the same load had 0-42 xruns;
   - at light load (callback ~6 us) there were 0 xruns everywhere with the
     fix.
3. **Period size matters twice:** short periods pay the ~1 ms fixed cost
   more often, and they leave a shorter deadline.
4. **Conversion path.** 22.05 kHz arms never underran, because their
   deadline is 46 ms or more, but they cost ~11% of a core. 36 kHz with
   512-sample periods underran only under the heavy load (13-158).
5. **Not on the path, or unimportant:**
   - deferred work: period-elapsed runs in hard IRQ;
   - scheduling latency: the C knobs had no measurable effect;
   - PIE bounces: 1-53 per 12 s arm, and pinning did not change outcomes;
   - codec clocking: there is no hiss mechanism for 22.05, 36, 44.1 or
     48 kHz, since every one has a codec row or is resampled.

## 6. Earlier conclusions checked

| claim | verdict |
|---|---|
| audio path < 2% of CPU0 at 48 kHz, no resampler (09-25) | right for 48 kHz / 2048-frame / stereo; wrong in general (~10% at 512-frame periods, ~11% for 22.05 kHz mono through linear) |
| OpenTyrian ~1.8 cores, ~0.9 with s31fp, unfixable (09-20) | numbers right (2.1 / 1.05); the conclusion holds even with SMP |
| S31ROUTE_NICE / priority "did not help" | still right: RR, slice and pinning did not rescue a starved thread, and a thread that fits does not need them |
| "growing the codec buffer changes nothing for a game" (09-10) | effectively right: extra headroom (D) had no effect in either regime |
| s31route comment "adds underrun headroom" (4096*rate/48000) | wrong, and harmful at 44.1 kHz: this formula creates the EINVAL bug |
| "the codec cannot run at 22050" | the codec can (a 64x row exists); only the I2S rate list does not offer it |
| deferred work on the audio path | not on it |

## 7. Options, ranked

For each option: gain, cost, risk and compliance. None touches an app (R1),
forces a rate (R3) or steers one app (R2) unless it says so.

1. **Fix s31route's sink-buffer rounding. Ship.**
   - **Gain:** 44.1 kHz SDL 1.2 apps with `samples <= 1024` go from silence
     and an underrun storm to sound (418 messages to 0). Probably fixes
     32 kHz as well.
   - **Cost:** one line, `buf -= buf % want_per`
     (rootfs/audiofp/s31route-proto.c).
   - **Risk:** low. It needs an ear check before shipping; then rebuild XIP
     image 1, since the plugin is in XIP_ROOTS.
2. **Run the codec at the app's native rate wherever a clock row exists.**
   - **Gain:** removes alsa-lib's linear converter, ~1.85 us per output
     frame (~8% of a core at a 44.1 kHz output), and its roughly -23 dB
     images near the top of the band.
   - **How:**
     - add `SNDRV_PCM_RATE_22050` to `ESP32S31_I2S_RATES`; the codec row
       already exists;
     - clone 64x rows for 32000 and 24000 from the 48 kHz family, as 11025
       was cloned;
     - 36 kHz has no codec family, so it is left to option 3.
   - **Cost:** a kernel build, plus an ear check per rate.
   - **Compliance:** within the rules.
3. **s31route does its own resampling and channel routing, replacing the
   inner plug.**
   - **Cost:** a tight float polyphase filter (16 taps) is estimated at
     <= 150 cycles per frame, against ~590 measured for linear. Hardware
     float is fast.
   - **Gains:**
     - better quality (images around -60 dB);
     - one layer less;
     - a later path to writing straight into the DMA ring.
   - **Covers:** 36 kHz and any other rate outside the codec's families
     (36 kHz to 48 kHz is a clean 4:3).
   - **Rule:** keep 44.1-family content on 44.1 kHz rather than one fixed
     48 kHz, so native 44.1 kHz apps are never resampled.
   - **Effort:** medium; needs ear and sweep checks.
4. **Cut the ~1 ms fixed cost per app write**, by batching kernel writes or
   copying straight into the hardware ring.
   - **Gain:** up to ~7% of a core for 512-sample apps at 44.1/48 kHz, and
     more margin before they starve.
   - **The naive prototype failed:** it underran on every batch
     (`raw/arms-d.txt`), because staged frames never reached the kernel
     while it drained.
   - **Constraint learned:** the kernel buffer must hold the batch plus the
     deadline, so the sink has to be sized independently of the app's ring.
     Combine this with option 3.
   - **Status:** unvalidated.
5. **A float-only libm inside our own musl build** (sinf, cosf, powf, expf
   and logf computed in float).
   - **Gain:** any app calling float maths per sample. `sinf` measured
     ~9 us, about 3000 cycles; a float version would take about 50-100.
   - **Cost:** results differ from musl's by rounding. That is tolerable for
     audio, but it is a behaviour change.
   - **Compliance:** platform libc. The owner's red line on custom
     libraries means this needs an explicit decision.
6. **Overclock to 360 MHz.**
   - **Gain:** +12.5% for everything.
   - **Cost:** the radios and Wi-Fi must be re-checked
     (overclock-verify-radios).
   - **Limit:** it does not rescue OpenTyrian (2.1 to 1.87 cores).
7. **Not recommended on this evidence:** C (RR, EEVDF slice, nice), D
   (headroom) and B (pinning). All were measured and none rescued a starved
   thread. The best result, one single-run reduction at 36 kHz (35 to 13
   xruns), is within noise.
8. **Excluded or not viable:**
   - the s31fp preload: red line, +250 ms per exec, and OpenTyrian still
     needs 1.05-1.6 cores with it;
   - OUTPUT_QUALITY 2: R1;
   - capped rates: R3;
   - AUDIODEV or SDL_* environment steering: R2;
   - synthesis on hart0: it cannot run the app's code;
   - GDMA/I2S features: DMA is 0.04%, and the position already comes from
     hardware.

## 8. Instruments

- **`rootfs/audiofp/oplbench`:** OpenTyrian's opl.c and lds_play.c,
  extracted with Buildroot's 0001 patch and Makefile flags. `-r` prints
  per-helper ns; that figure is noisy, and the song figures are the
  reliable ones.
- **`rootfs/audiofp/sdltone1` / `sdltone2`:** SDL 1.2 / SDL 2 test clients.
  - they report callback CPU, path CPU, the worst gap, and the CPUs used
    and moves between them;
  - they take a spinner count and a load.
- **`rootfs/audiofp/s31route-proto.c`:** test only.
  - With no environment set it behaves like the shipped plugin except for
    the buffer fix.
  - Knobs: HEADROOM, SLICE_US, RR, CPU, BATCH, OLDBUF.
  - It prints an xrun count at close, and is loaded only through a
    temporary `/root/.asoundrc`.
- **`scripts/`:** opl.sh, tone.sh, arms.sh, tyr.sh, collect.sh.
- **`raw/`:** opl.txt, tone-h2-sinf.txt (a callback using `sinf`),
  arms-a/b/c/d.txt, tyr-stock/s31fp/s31fp-place.txt.

## 9. Limits of this data

- **Single runs:** every arm is one 12 s run on one boot, and the OpenTyrian
  arms shared a boot. So only large effects are claimed: silence against
  sound, and 0 against hundreds of xruns. Single-digit differences are
  noise.
- **Counting xruns:** alsa-lib prints two lines per recovery, so the
  prototype plugin's own counter gives the true count. The "underruns" in
  tone-h2 are message lines, about 2 per xrun.
- **Nothing was verified by ear:** capture is excluded (R5) and no one was
  listening. Option 1, and anything else that changes the sample path, needs
  an ear check before it ships.
