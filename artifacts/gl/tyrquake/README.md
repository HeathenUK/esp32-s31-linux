# TyrQuake 0.71 GL (stock, X11/GLX) on the board

2026-09-26. Kernel #393, lvdesk e83b9d67 and libXxf86vm 291419e7 (XIP,
commit 87fc9f6), libGL the shipped 927239ee. Status: runs, windowed and
fullscreen, with sound and hardware gamma; a first baseline.

Stock TyrQuake 0.71 `bin/tyr-glquake`, built by its own Makefile through
`rootfs/build-tyrglquake-x11.sh` (VID_TARGET=x11, IN_TARGET=x11, SDL2 sound,
no source changes). It links libX11 (xlite), libXext, libXxf86vm (xlite's
VidMode client), libGL (ours) and libSDL2 (sound only).

## 1. What it needed from the platform

- **XF86VidMode gamma ramps.** `vid_glx.c` Gamma_Init calls
  XF86VidModeGetGammaRampSize and XF86VidModeGetGammaRamp at start-up, and
  XF86VidModeSetGammaRamp whenever the gamma cvar changes (fullscreen only:
  NQ/view.c V_UpdatePalette) and at exit to put the saved ramp back. None of
  the three existed in xlite/vidmode, so the binary could not even be loaded
  (undefined symbols).
  - xlite/vidmode/xf86vm.c: the three client calls, laid out as
    xf86vmproto.h's xXF86VidMode{Get,Set}GammaRamp{,Size}Req and replies
    (minors 17, 18, 19).
  - lvdesk/xshim.c vidmode_request: GetGammaRampSize answers 256, GetGammaRamp
    returns the ramp in force (identity until someone sets one, or the
    ramp implied by a SetGamma), SetGammaRamp fills the same per-client 565
    tables SetGamma does (f64a466), so the ramp is applied to the client's
    16-bit ShmPutImage pixels, windowed and fullscreen, and goes back to
    identity when the client leaves. A size other than 256 is BadValue, as
    on a real server. XSHIM_NOGAMMA=1 reports size 0 (the app then has no
    hardware gamma, as before).
  - SDL 1.2 never asks for a VidMode ramp (its X11 ramps are DirectColor
    colormaps), so QuakeSpasm and the SDL games see no change.
  - Host check of the table maths (ramptest.c here): the ramp TyrQuake saves
    and restores (i*257) and its gamma-1 ramp (i<<8) both map to identity at
    5 and 6 bits (0 mismatches); its gamma 0.7 ramp lands within 1 level of
    SetGamma(1.43) on every red value.
  - On the board: `xshim: VidMode gamma ramp set (r[64] 24832 ...)` for
    `+gamma 0.7`, `identity` again when it exits. tyrgl-fs-gamma07.jpg
    against tyrgl-fs-demo1.jpg.
- **Nothing else was missing.** Every X11 call it imports is in xlite's
  export list, every GL call in libGL's; XF86VidMode 2.2 mode switching to
  320x240 works (PPA-scaled fullscreen, pillarboxed to 640x480).

## 2. Launch: the app's own switches only

`cd /root/tyrgl && HOME=/root/tyrgl ./tyr-glquake -basedir /root/tyrgl
-sndspeed 11025 -heapsize 14336 -width 320 -height 240 -fullscreen|-window`

| switch | why |
|---|---|
| `-basedir /root/tyrgl`, HOME=/root/tyrgl | its own tree (scripts/board/tyrgl-setup.sh: id1/pak0.pak links the shareware pak). Its writable game dir is $HOME/.tyrquake/id1 (config.cfg, video.cfg, qconsole.log), apart from QuakeSpasm's and the software Quakes' configs, which share cvar names. |
| `-sndspeed 11025` | its SDL2 sound's own rate switch (snd_sdl.c); the default is 48000. 11,025 Hz is the rate Quake's sounds are recorded at. |
| `-heapsize 14336` | TyrQuake's default is 256 MB. 12288 dies at map load (`Cache_TryAlloc: 1356896 is greater than free hunk`); 13312 plays but reloads models constantly (profile: GL_LoadAliasMeshData 2.5%, Mod_LoadAliasModel 1.4%, GL_FloodFillSkin 1.0%, CRC_Block 0.7%, with SD 14.8% and paging 12.7% of CPU0); 14336 shows no loader in the profile. 16384 also clean, but 2 MB more to page. |
| `-width 320 -height 240` | as QuakeSpasm; fullscreen is VidMode 320x240 on xshim, scaled by the PPA. |
| `+timedemo demo1` | TyrQuake stuffs "+commands" from its own argv (cmd.c), so unlike QuakeSpasm no autoexec basedir is needed. |

Menu: Games > Quake > GLQuake (TyrQuake 0.71, X11 GLX): Window,
Fullscreen, Timedemo (overlay and card /etc/lvdesk/menu.conf).

## 3. timedemo demo1, fresh boot per run (glquake-arm.sh, sound on at DAC 110)

| mode | runs | fps | seconds | memory at the end |
|---|---|---|---|---|
| fullscreen | 2 (tyrfs1, tyrfs2) | **3.5, 3.6** | 280.5, 265.9 | VmSwap 21.3-22.4 MB, VmRSS 3.7-4.7 MB, majflt 9.2-13.4k (35-48/s) |
| windowed | 1 (tyrwin1) | **3.7** | 261.3 | VmSwap 21.2 MB, majflt 8.3k (32/s) |

A first baseline, n=2 and n=1 (trimmed on purpose; phase 5 will move it).
Against QuakeSpasm on the same libGL: 4.5 fullscreen / 4.3 windowed (#391)
and 4.7-4.8 fullscreen (#393). TyrQuake GL is ~20% slower and pages more
(VmSwap ~21.5 against 16-17 MB; its hunk has to be 14 MB against 12 MB).
Fullscreen present gaps (tyrfs1, lvdesk's histogram): 100-200 ms 251,
200-400 ms 615, >= 400 ms 152 of 1,021.

## 4. Profile (h1s, 8,000 samples, fullscreen, on the tyrfs2 boot after its timedemo)

Game and desktop pinned to CPU0 for the window (raw/tgpfs2.*,
raw/tgpfs2.prof.txt; `scripts/board/gq-prof.py` with nm/).

| | share of CPU0 |
|---|---|
| libGL | **61.4%** (ze_mod_rgba 9.6, ZB_fillTriangleMappingPerspective 6.8, zo_mul 4.9, gl_vertex_indexed 4.3, ZB_fillTriangleGeneral 4.2, zt_rr 3.7, zdw_equal 3.0, zo_sa_omsa 2.1, tex_store 2.0, zo_store 1.9, zp_run 1.9) |
| kernel | 24.2 (sched/tick 8.6, other 5.8, SD 3.8, paging 3.0, IRQ entry 1.3, syscalls 1.1, sound 0.9, idle 0.4) |
| libc | 5.0 (memset 3.3) |
| tyr-glquake (game logic) | 4.4 |
| lvdesk (present) | 2.1 |
| sound userspace (ALSA, s31route, SDL2) | 1.1 |
| OpenSBI | 1.1 |

The same shape as QuakeSpasm fullscreen (libGL 64, kernel 19, game 8,
lvdesk 3): the library is the lever.

## 5. Found on the way, for the library round (gl/ is not the board agent's)

- **GL_MAX_TEXTURE_SIZE 256 leaves TyrQuake's larger textures white.**
  libGL logs `unimplemented texture larger than GL_MAX_TEXTURE_SIZE 256
  (GL_INVALID_VALUE)` once. TyrQuake reads GL_MAX_TEXTURE_SIZE only into its
  gl_max_size cvar (gl_textures.c:1035) and does not clamp uploads to it, so
  anything wider than 256 is rejected: the 320x24 status bar draws as a
  white strip. The torch flames and a monster in tyrgl-fs-gamma07.jpg are
  white too; skins resampled to a power of two above 256 would explain it,
  but that part is inferred, not traced. QuakeSpasm's frames never showed
  this (it scales to the reported maximum). Fix: accept at least 512 (TyrQuake
  and GLQuake assume 1024 is common), or 1024 with the memory cost stated.
- `approximated GL_LINEAR / mipmap texture filters by nearest sampling of
  level 0`: TyrQuake asks for linear and mipmapped filtering; phase 4's
  filters (not shipped) would honour them - measure the cost first
  (LIBGL-OPPORTUNITIES).
- O7 (the glBegin 64-byte memcpy + memcmp on the lent CPU) applies here too.

## Files

- tyrgl-fs-demo1.jpg (gamma 1), tyrgl-fs-gamma07.jpg (+gamma 0.7):
  fullscreen demo1 on the panel, hardware JPEG.
- arms/ (the timedemo runs), raw/ (bisect, profiles, smaps), nm/ (symbols).
