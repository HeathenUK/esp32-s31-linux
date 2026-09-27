# Audio first principles: implementation (2026-09-27)

This implements options 1-3 of `REPORT.md` in this directory. No application
was changed, and every app still gets exactly the rate and format it asks
for. Everything below the app (s31route, the codec rates, where conversion
happens) is ours.

**Shipped state after this work:**
- kernel **#401** (`images/xipImage`; #393 + `patches/0074`, same RAM-text list);
- XIP image 1 `images/rootfs-xip.cramfs` md5 `0f5028bc9142a9abaefac0b33f596a28`, carrying
  `/usr/lib/alsa-lib/libasound_module_pcm_s31route.so` md5 `b651fa15a84991663e81abb76040278e`
  (verified on the board);
- XIP image 2 unchanged (`6e5bb0948b7086f1923de8d8365c5c37`, not reflashed).

Rollback images: `images/ship-393-xipImage`, `images/pre-afp1-rootfs-xip.cramfs`
(before item 1) and `images/afp1-rootfs-xip.cramfs` (item 1 only).

**Not verified by ear.** Every check below is an instrument. The listening
script is in section 6, and none of this counts as good until someone has
run it.

## 1. s31route sink-buffer rounding (option 1): SHIPPED

The fix is `buf -= buf % want_per` before the `max(want_buf)` clamp, in
`rootfs/s31route.c`.

Board, #393, sdltone1 at light load, 4 s per case (`t1.sh`, same boot):

| app geometry | shipped plugin | fixed plugin |
|---|---|---|
| 44100 st, 512 | EINVAL, 248 underrun lines, 0 callbacks | 0 xruns, 347/345 callbacks, sink 3584/512 |
| 44100 mono, 512 | EINVAL, 252 lines, 0 callbacks | 0 xruns, 172/172 |
| 44100 st, 1024 | works (ring 4096 = sink) | unchanged |
| 32000 st, 512 | 0 xruns, but **203/250 callbacks** | 0 xruns, 206/250 |
| 48000 st, 512 | works | unchanged |

- **32 kHz did not hit the EINVAL.** Through plug the codec ran at "96000",
  and its own geometry hid the 2730 rounding.
- **But 32 kHz played at 81% speed.** 203-206 of 250 expected callbacks
  means 32 kHz content was ~19% slow (flat) on the shipped system. See
  item 2 for the cause.
- **Shipped as XIP image 1** by the proper path: overlay, `xip-fast`, image 1
  only, verified by a cramfs extract of old against new, where the ONLY
  differing file is the plugin.
- Board md5 matched after reboot, and the same 44.1 kHz cases passed again
  from `/usr/lib` without `.asoundrc`.

## 2. Native codec rates (option 2): SHIPPED in kernel #401

This is `patches/0074-audio-native-22050-24000-32000/`, with a README
carrying the full row derivation.

**I2S.** `ESP32S31_I2S_RATES` gains 22050, 24000 and 32000. MCLK = 256 x
rate divides the 40 MHz xtal exactly at all three.

**es8389.** These are 64x rows, and not vendor rows. They follow the
vendor's own pattern:
- **Registers 0x16/0x18/0x19 scale with the rate:** 8k `09/19/07`, 16k
  `12/31/0E`, 24k `1A/49/14`, 32k `23/61/1B`, 44.1/48k `35/91/28`.
- **The other registers are identical within a band at a fixed ratio.** The
  vendor's 24000/800 row equals 16000/1200, and 32000/600 equals 48000/400,
  apart from the pre-divider and the triple.
- **The band splits between 24 and 32 kHz.**

So the new rows are:
- **32000@64:** the 48000@64 row with the 32k triple.
- **24000@64:** the 16000@64 row with the 24k triple.
- **22050@64:** the 24000 row, as 44100 shares 48000's. This replaces
  0042's 44100-clone row, which had never played. 11025 is untouched.
- **`es8389_RATES` gains `SNDRV_PCM_RATE_24000`,** because 8000_96000
  predates that bit.

**96000 is removed from the I2S rates, which is a real bug fix.** 256 x 96000
needs an xtal divider of 1.63, and the integer part cannot go below 2, so it
ran at 40 MHz / 2 / 256 = 78125 Hz. That is exactly the 0.81 callback ratio
above. `hw_params` now refuses any rate whose divider would be below 2. A
96 kHz file through `plughw:0,0` now plays converted to 48000.

**Machine checks** (#400/#401, sdltone1 through "default"; `hw_params` read
during the stream):

| app | codec hw_params | callbacks | xruns |
|---|---|---|---|
| 22050 st 1024 | RW_INTERLEAVED 22050 (plug collapsed) | 129/129 | 0 |
| 24000 st 1024 | RW 24000 | 141/141 | 0 |
| 32000 st 1024 | RW 32000 | 188/188 | 0 |
| 32000 st 512 (#401) | RW 32000 | 251/250 | 0 |
| 11025 / 16000 / 44100 / 48000 | native, unchanged | 130/129, 188/188, 260/258, 283/281 | 0 |

- **No "Clock coefficients do not match" line in dmesg,** on either boot.
- **The kernel builds clean,** with no `warning:`.
- **The RAM-text function set and order match #393's System.map,** name for
  name. The volume had last built #399 with another list, so this was
  checked rather than assumed.

## 3. s31route's own converter and channel routing (option 3): SHIPPED

**What it is.** `rootfs/s31resample.h` is used by `rootfs/s31route.c`. When
the sink is the codec (`hw:0,0`), s31route now opens it directly instead of
`plug:'hw:0,0'`.
- **Native rate, stereo:** pass-through (RW_INTERLEAVED).
- **Native rate, mono:** its own dup to stereo.
- **Any other rate:** a float polyphase FIR to the smallest codec rate at or
  above it **in the same family**:
  - multiples of 11025 go to the 44.1 kHz family;
  - everything else goes to the 8/48 kHz family.
  - So 36000 goes to 48000 (4:3), 12000 to 16000, 33075 to 44100 and 37800
    to 48000 (80:63).
- **Rates plug had been getting wrong.** With 32000 now native, alsa-lib's
  nearest-rate choice had started sending 36000 DOWN to 32000, which
  band-limits it to 16 kHz (seen on #400 before item 3).
- **The loopback / A2DP sink still goes through plug:, unchanged.**
- **Any failure of the direct open falls back to plug:.** That covers an
  odd ratio needing more than 160 phases, or hw_params refusing.
- **Runtime knob:** `S31ROUTE_PLUG=1` forces the old path, for A/B tests.

**Filter.** 24 taps per phase, Kaiser beta 7, -6 dB at half the input rate,
and every phase normalised to unity DC gain. Positions are exact rational
(k*M/L), so there is no drift.

**Single precision only.** The table is built with float polynomial sin(pi x)
and a float I0 series; musl's sinf evaluates in double. The build uses
`-Wdouble-promotion -fsingle-precision-constant`, and `build-s31route.sh`
**fails** if the .so references any `__*df*` helper.

**Geometry fix needed for direct open.** A converted app period can exceed
the codec's 1023-frame maximum: 36 kHz x 1024 is 1365 at 48 kHz. The
period is split into equal parts that fit, and the buffer is the largest
multiple of that period within the SRAM ring (8192). Without this, the
first direct build failed hw_params with EINVAL and silently fell back to
plug.

**Quality** (host, `rootfs/audiofp/rstest.c`, the same header). A tone at
0.3 FS is fed in 512-frame chunks, and a least-squares fit gives gain and
residual (everything that is not the tone: images, aliasing, rounding).

| 36000 to 48000 | T=24 gain / resid | alsa-style linear gain / resid |
|---|---|---|
| 0.02 fs_in (720 Hz) | 0.00 / -85.2 dB | -0.01 / -62.5 dB |
| 0.20 (7.2 kHz) | 0.00 / -83.8 | -1.09 / -21.5 |
| 0.30 (10.8 kHz) | 0.00 / -80.5 | -2.49 / -13.1 |
| 0.40 (14.4 kHz) | 0.00 / -72.3 | -4.55 / -6.2 |
| 0.45 (16.2 kHz, transition band) | -0.68 / -21.8 | -5.87 / -3.0 |

- **The other ratios are within 2 dB of these:** 12000 to 16000, 37800 to
  48000, 33075 to 44100, 40000 to 48000 and 30000 to 32000.
- **T=16 loses the top of the band:** -32 dB residual at 0.40 fs_in. T=32
  gains only ~4 dB there.

**Cost** (board, `rootfs/audiofp/rsbench.c`, same header, thread CPU time,
320 MHz; two runs):

| path | cycles / output frame |
|---|---|
| 36000 to 48000 mono, T=24 | 203 / 218 |
| 36000 to 48000 stereo, T=24 | 302 / 295 |
| 37800 to 48000 stereo, T=24 | 294 / 316 |
| (T=16 mono / stereo) | 184-190 / 272-302 |
| (T=32 mono / stereo) | 232-251 / 354-355 |
| mono-to-stereo dup | 42 |
| alsa-lib linear, REPORT.md (measured end to end) | ~590, plus ~110 for route |

- **Stereo L=R output is bit-identical to mono** in every case.
- **The first build ran a single fmadd.s chain** at 270 cycles mono; four
  accumulators brought it to 203.

**End to end** (sdltone1, path = audio thread CPU minus callback CPU, same
boot #400, interleaved, 8 s per arm, 2 reps; `impl/pathab-b.txt`):

| app | new (s31route converts) | old (plug:) |
|---|---|---|
| 36000 mono 1024 | 5.80 / 5.96% of a core, codec 48000 | 9.33 / 9.40%, codec 32000 (down) |
| 36000 st 1024 | 7.67 / 7.49%, 48000 | 7.27 / 7.49%, 32000 (down; 2/3 the output frames) |
| 22050 mono 1024 | 2.03 / 3.06% (dup) | 3.89 / 3.74% |
| 44100 mono 2048 | 2.50 / 2.74% (dup) | 4.83 / 4.06% |
| 12000 mono 512 | 2.26 / 2.31%, 16000 | 4.76 / 5.00%, 24000 |
| 48000 st 2048 | 2.73 / 3.23% | 3.45 / 2.82% (pass-through both; noise) |

Every arm had 0 xruns, and callbacks matched expectations (e.g. 94/94 at
12 kHz). Direct pass-through at native rates uses equal parts of the app
period, for example 683-frame codec periods for a 2048-frame 48 kHz app.

**Sink switching,** with `/run/s31-sink` moved to `hw:1,0` mid-stream and
the Bluetooth daemon not streaming:
- the stream stalls, **identically with `S31ROUTE_PLUG=1`**, so this is
  pre-existing and not introduced here;
- the loopback write blocks with no reader;
- s31route prints the app ring as half its size on the reopen (for example
  1024/256 for a 2048/512 app), in both arms. That is not investigated
  here.

The follow check is throttled to one per 128 transfers, so a switch is
seen 1.5-3.6 s later, depending on the period.

## 4. Option 4 (the ~1 ms fixed cost per write): not attempted

Out of time box.

## 5. Regression checks

(filled in below from `impl/regress/`)

## 6. Ear check: REQUIRED, and the owner's to do

The board holds the script at `/root/audiocheck.sh`, with source in
`impl/audiocheck.sh`. The signal generator is `/root/afp/wavgen`, source
`rootfs/audiofp/wavgen.c`.

    sh /root/audiocheck.sh            # all 10 cases, ~50 s
    sh /root/audiocheck.sh 36000 2    # one case

- It plays through the normal "default" path at DAC 143, the owner's level,
  covering 22050, 32000, 36000, 44100 and 48000, each mono then stereo.
- Before each case it prints `=== now playing: <rate> Hz <mono|stereo> ===`,
  and afterwards the rate the codec actually ran at.
- **Each case is ~4.5 s:**
  - 1 s pure A440;
  - a plucked C-E-G-C arpeggio, LEFT speaker only (stereo);
  - the same arpeggio, faster, RIGHT only (stereo);
  - a C major chord.

**Listen for:**
- **Pitch:** the A must be the same concert A in every case. Flat means a
  wrong clock, as 32 kHz was before #401.
- **Hiss** or noise under the notes. That is a wrong codec row, as 11025 was
  before 2026-09-19. It matters most for 22050 (new row) and 32000 (new
  row).
- **A rough or metallic edge on the bright arpeggio.** That would be
  converter images, which matters most for 36000.
- **Clicks, dropouts or stutter.**
- **Channel placement:** the left-only and right-only sections must come
  from one side.
