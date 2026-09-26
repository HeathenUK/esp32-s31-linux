# GLQuake on SDL 1.2 (stock QuakeSpasm 0.96.3): launch, baseline, profile

2026-09-26. Kernel #391. libGL is the shipped XIP copy (md5 927239ee). The
game runs from SD (`/root/quake/quakespasm`), built by
`rootfs/build-quakespasm.sh`. Tooling:
- `scripts/board/glquake-run.sh`: one run on the board. It waits quietly and
  records RSS, swap, faults and SD reads every 10 s. It can also take h1s
  samples and smaps.
- `scripts/board/glquake-arm.sh`: a fresh boot, then N runs.
- `scripts/board/glquake-setup.sh`: the card-side timedemo basedir.
- `scripts/board/gq-prof.py`: the h1s breakdown by category.

## 1. Launch: the app's own switches only

| switch | why |
|---|---|
| `-mixspeed 11025` | QuakeSpasm's **output** rate: SDL and ALSA open at 11025 Hz with 512 samples. `-sndspeed` is the sfx cache rate and already defaults to 11025, the rate Quake's sounds are recorded at. At the default mixspeed of 44100 it mixes 4x the samples, runs a lowpass (`snd_mix.c` 415), and underran. The rate is chosen by the game, not forced by the platform. |
| `-zone 384` | QuakeSpasm's own old 32-bit zone size. The 4 MB default is `memset` inside the hunk at start (`Hunk_AllocName`), and here it holds only strings and the pak directory. |
| `-heapsize 12288` | The smallest hunk that plays demo1 **without the Cache thrashing**. The bisect is below. |
| `-width 320 -height 240 -window` / `-fullscreen` | Fullscreen is SDL 1.2 VidMode 320x240 on xshim, scaled by the PPA ("mode 320x240 scales to 640x480"). |

Heap bisect (`-zone 384 -mixspeed 11025`):

| -heapsize (kB) | result |
|---|---|
| 7936 | Below 8 MB the game loads its sounds as 8-bit, so SDL opens `AUDIO_U8`. The platform then refused it: "Couldn't set hardware audio parameters", and there is no sound at all (section 5). |
| 8192 | Fails at map load. |
| 10240 | Fails at map load. |
| 10752 | Loads and plays, but the Cache thrashes. The fullscreen profile shows TexMgr_LoadImage32, Mod_LoadModel, BuildTris and memset at 35% of CPU, with models and skins reloaded constantly, and it runs at about 0.8 fps. |
| **12288** | Plays demo1 with no loader functions in the profile. |

**The timedemo.** The shareware pak **ignores "+command" arguments**:
QuakeSpasm stuffs the command line only when `gfx/pop.lmp` exists
(`COM_CheckRegistered`, common.c). So `+timedemo demo1` silently ran the
attract loop, which is what the owner saw. The timedemo runs from
`/root/quake/td`, whose `id1/autoexec.cfg` holds `timedemo demo1` beside a
pak0.pak symlink. quake.rc execs autoexec.cfg before startdemos. Every run
below ends with QuakeSpasm's own `N frames X seconds Y fps` line.

## 2. Baseline, timedemo demo1, 320x240, sound on at DAC 110

**Before** (the owner's state: stock 44.1 kHz, `-heapsize 16384`, 4 MB
zone): 420 s of attract loop and no timedemo at all.
- VmSwap was 18.3-20.1 MB and VmRSS 3.5-4.7 MB.
- There were 30,811 major faults (73/s).
- ALSA underran.

**After:**

| arm | runs | fps | seconds | memory during the run |
|---|---|---|---|---|
| fullscreen, fresh boot (fs12a) | r1, r2 | 4.5, 4.6 | 215.7, 211.6 | VmRSS 4.7-5.1 MB, VmSwap 16.2-16.5 MB, majflt 6.1-6.5k (about 28/s) |
| fullscreen, second boot, unpinned (c1-a1, c1-a2) | 2 | 4.4, 4.5 | 222.1, 217.0 | VmSwap 16.1-17.2 MB, majflt 7.4-8.2k |
| windowed, fresh boot (win12a) | r1, r2 | 4.4, 4.4 | 221.1, 218.0 | VmRSS 4.0-4.5 MB, VmSwap 16.7-17.2 MB, majflt 7.2-7.9k |
| windowed, earlier boot (m11h12) | 1 | 4.3 | 226.1 | VmSwap 16.4-17.6 MB |

So fullscreen is 4.4-4.6 fps (median 4.5) and windowed 4.3-4.4.

**Tail** (fullscreen fs12a r2; lvdesk's present-gap histogram over the
run's 1,000 presents):
- <100 ms: 5;
- 100-200 ms: 522;
- 200-400 ms: 428;
- ≥400 ms: 45, which is 4.5% of frames.

It pages throughout, though it does not thrash: faults are about 28/s at a
cost of about 1 ms each.

Screenshots (board panel, hardware JPEG):
- `fs-timedemo-menu.jpg`: the menu's timedemo entry, fullscreen;
- `g10-fs-demo1.jpg`: gamma 1;
- `g07-fs-demo1.jpg`: gamma 0.7 through the new VidMode gamma, section 4;
- `menu-glquake.jpg`: Games > Quake > GLQuake;
- `../stage6/quakespasm-first.jpg`: the first light.

## 3. Profile: where the CPU goes

h1s, 8,000 samples. The game and lvdesk were pinned to CPU0 for the window
only. Raw data is in `prof/`.

| | windowed | fullscreen |
|---|---|---|
| libGL | 46.4% | **64.3%** |
| kernel | 38% (scheduler, tick and IRQ about 20; SD and paging 9) | 19% (scheduler and tick 8.3; SD and paging 2.9) |
| quakespasm (game logic) | 5.6% | 7.8% |
| libc | 5.3% | 4.1% |
| lvdesk (present) | about 2.7% | 3.3% |
| sound (ALSA, s31route, SDL, the kernel's snd) | about 1.5% | about 1.5% |
| OpenSBI | 1.1% | 1.4% |
| idle | 1.5% | 0.6% |

The libGL functions, and what each rendering pass costs (one boot, each pass
switched off with QuakeSpasm's own cvar): alias models 46% of frame time,
the world lightmap pass 40%, the fullbright glow pass 17%. The details are
in **LIBGL-OPPORTUNITIES.md**.

## 4. "Far too dark": the cause and the fix

The host rig result is in `dark/DARKNESS.md`: Mesa against our host libGL,
same frames.
1. **Most of it is the content at gamma 1.** The first-light frame is the
   start of e1m3 with the console half down. Mesa draws it only 5-13%
   brighter.
2. **Our libGL loses a further 1-14% of luminance, up to 23% in dark
   lightmaps.** The causes are `T565` truncation of lightmap texels and
   flooring `MUL8` blends. That is for the library round: LIBGL-OPPORTUNITIES
   O3.
3. **The game's own brightness control did nothing on this platform. Fixed.**
   - With no GLSL, QuakeSpasm's `gamma` cvar (the menu's Brightness slider)
     is hardware gamma: SDL 1.2 `SDL_SetGamma`, then (no ramp on a TrueColor
     visual) `XF86VidModeSetGamma`.
   - xshim swallowed that request, so the game could not be brightened at
     all.
   - xshim now applies the VidMode gamma to X clients' 16-bit ShmPutImage
     pixels: every GL frame, windowed and fullscreen. The ramp belongs to
     the client that set it, and it is reset when that client leaves.
   - The shipped lvdesk (md5 a39ec077, XIP) logs
     `xshim: VidMode gamma 1.43 1.43 1.43` for `gamma 0.7`. See
     `g07-fs-demo1.jpg` against `g10-fs-demo1.jpg`.
   - Identity costs one test per row; a ramp costs about 2 ms per 320x240
     frame. `XSHIM_NOGAMMA=1` restores the old behaviour.

## 5. Platform levers measured and rejected, or left open

| lever | result |
|---|---|
| **lvdesk pinned to CPU1** (the game keeps 0-1) | **Pathological.** The run did not finish in 330 s. CPU0 system time rose from 45 to 220 s and CPU1 user time from 47 to 290 s. Rejected. |
| the game pinned to CPU0 | 4.2 fps against 4.4-4.6 unpinned. With default affinity the game uses CPU1 for about 47 s of a 220 s demo, and that helps. |
| libGL text from RAM (an SD copy in the page cache) against XIP | 268.4 s against 276.0 s, +2.8%, one run on one boot. This is lever O5 for the library round. |
| kernel `.text..fast` for GLQuake | Flash-resident kernel text is 6.1% of CPU0 fullscreen. After idle, entry asm and memset (which cannot move), the candidates are about 2%, so at best about 1.5%. Not pursued. |
| timer IRQs | riscv-timer runs at about 450/s on CPU0 under the game against 188/s idle: the 100 Hz tick, the LCD vblank hrtimer, the 10 ms SD keepalive while paging, and hrtimer wakeups. The scheduler, tick and IRQ total is 8.3% fullscreen, spread flat with no symbol above 0.5%. |
| **U8 audio** (open) | `plug` over s31route cannot convert formats: alsa-lib's linear, rate and route plugins need MMAP access on their slave, and s31route offers RW only. So an `AUDIO_U8` open fails ("no configurations available"). Adding `MMAP_INTERLEAVED` fixes the negotiation, but aplay then hangs in playback. That needs work in ioplug's mmap emulation and pointer handling, and it was not shipped. GLQuake needs 12 MB of heap, so it never takes the 8-bit path. |
| the Sys_Error message is lost (open) | When QuakeSpasm errors out (for example a hunk overflow at 8 MB), the process exits silently after "Shutting down SDL sound", inside Host_Shutdown's video teardown, before it prints the error text. Nothing appears in dmesg. The suspect is xlite's IO-error `_exit(1)` during SDL 1.2's two-connection teardown. |
