# GLQuake (stock QuakeSpasm 0.96.3): libGL opportunities for the next library round

2026-09-26. Everything here was measured on the board (kernel #391, shipped
libGL md5 927239ee from XIP) or on the host glref rig. The launch was the
tuned one: `-mixspeed 11025 -heapsize 12288 -zone 384 -width 320 -height 240`,
with `timedemo demo1` run from `/root/quake/td` (see README.md in this
directory). **libGL is where the time goes.** In fullscreen it is 64% of CPU0.
The platform (kernel, desktop, sound) is under 20% combined, and it has no
single hotspot.

## 1. Where the frame goes

### 1a. h1s PC profile, 8,000 samples (8 s at 1 kHz)

The game and the desktop were pinned to CPU0, the core the sampler sees.
`scripts/board/gq-prof.py` produced the table from the raw files
`/root/gq/{prof1,f12}.pcs` and `.qmaps`, copied to
`artifacts/gl/glquake/prof/`.

| share of all CPU0 samples | windowed 320x240 | fullscreen 320x240 (VidMode + PPA) |
|---|---|---|
| **libGL total** | **46.4%** | **64.3%** |
| perspective textured filler (base texture pass) | 6.7 | 7.8 |
| general pipeline: filler + stage driver | 6.0 | 11.0 |
| general: depth-test stage (`zd_lequal`) | 3.9 | 4.8 |
| general: texture-fetch stage (`zt_rr`) | 3.3 | 3.7 |
| general: texenv stage (`ze_replace_rgba`, `ze_mod_rgb`) | 4.9 | 5.9 |
| general: blend and store (`zo_mul2`, `zo_sa_omsa`, `zo_one_one`) | 9.1 | 8.8 |
| general: colour and flat stages | 1.1 | 2.8 |
| immediate mode, transform, clip, setup | 8.1 | 14.9 |
| clear (`ZB_fill16`) | 1.3 | 1.7 |
| quakespasm (the game) | 5.6 | 7.8 |
| libc (memset/memcpy) | 5.3 | 4.1 |
| kernel (all) | 38 | 19 |
| lvdesk | about 2.7 | 3.3 |

The general per-pixel pipeline (depth, fetch, env, blend, each a stage loop
over a chunk) costs **37% of the whole CPU in fullscreen**. That is 4.7x the
base texture pass, even though both cover much the same pixels.

### 1b. What each QuakeSpasm pass costs

Fullscreen timedemo demo1, one boot, one run per arm, run in this order. Each
arm switches one pass off with QuakeSpasm's own cvar, set in an
`autoexec.cfg` for the measurement only. Later runs on a boot are slower, not
faster, so the savings below are conservative.

| arm (cvar) | seconds | fps | frame time removed |
|---|---|---|---|
| default | 276.0 | 3.5 | - |
| `r_drawentities 0` (no alias models: monsters, items, view weapon) | 148.9 | 6.5 | **46%** |
| `r_fullbright 1` (no world lightmap pass) | 165.1 | 5.9 | **40%** |
| `gl_fullbrights 0` (no fullbright glow pass) | 228.3 | 4.2 | **17%** |
| `gl_overbright 0` (lightmap blend ZERO/SRC_COLOR instead of DST/SRC) | 278.1 | 3.5 | 0% |
| libGL loaded from SD, so its text runs from RAM (page cache) instead of XIP | 268.4 | 3.6 | 2.8% |

The savings overlap, so they are not additive. But the ranking is clear.

**Why each pass costs what it does.** Without `GL_ARB_multitexture` and
`GL_ARB_texture_env_combine`, QuakeSpasm takes its no-combiner paths:
- **World: "case 3"** in `r_world.c` 1196-1225, three passes:
  - the texture, with `REPLACE`;
  - the lightmap, with blend `DST_COLOR, SRC_COLOR` (2x modulate) and depth
    mask off;
  - a glow pass for textures that have fullbright pixels, with blend
    `ONE, ONE`.
- **Alias models: "case 3"** in `r_alias.c` 818-853, three passes over every
  model:
  - `MODULATE` with smooth colour;
  - the same geometry again with `ONE, ONE`, "to double the object colours";
  - a fullbright pass with `ONE, ONE`.

  Each pass is a full `GL_DrawAliasFrame` in immediate mode:
  `glBegin(GL_TRIANGLE_STRIP/FAN)` with `glColor`, `glTexCoord` and `glVertex`
  per vertex. The view weapon is close to the camera and covers a large part
  of the screen.

## 2. Ranked opportunities, with estimated gains

The estimates are fractions of the current fullscreen frame. They come from
1a and 1b and are not measured on new code.

### O1. GL_ARB_multitexture (2 units) + GL_ARB_texture_env_combine + GL_ARB_texture_env_add

**Estimate: -35 to -50% of frame time, about 1.5-2x fps.** This is the
largest lever, and it matches exactly what QuakeSpasm asks for:
- `gl_vidsdl.c` 1026-1046: `GL_ARB_multitexture` in the extension string;
  `glMultiTexCoord2fARB`, `glActiveTextureARB` and `glClientActiveTextureARB`
  from `SDL_GL_GetProcAddress`; `GL_MAX_TEXTURE_UNITS` of 2 or more (it tests
  3 or more only for the GLSL/VBO path).
- `gl_vidsdl.c` 1051-1086: `GL_ARB_texture_env_combine` (or EXT), then
  `GL_ARB_texture_env_add` (or EXT).
- **World** (`r_world.c` 1180-1195, "case 1") becomes one pass:
  - unit 0 texture;
  - unit 1 lightmap with `COMBINE_RGB = MODULATE`, `SOURCE0 = PREVIOUS`,
    `SOURCE1 = TEXTURE`, `RGB_SCALE = 2.0`.

  The glow pass stays.
- **Alias models** (`r_alias.c` 770-786, "case 1") become one pass instead of
  three:
  - unit 0 with `COMBINE_RGB = MODULATE` (`TEXTURE` x `PRIMARY_COLOR`),
    `RGB_SCALE = 2.0`;
  - unit 1 fullbright texture with `GL_ADD`;
  - blending on with the default `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.
- **Sky** (`gl_sky.c` 916) uses multitexture with `GL_DECAL` when
  `r_skyalpha >= 1`.
- **What it removes, per frame:**
  - one full world pass: its geometry (part of the 14.9% immediate mode)
    and its general-pipeline fill;
  - two of three alias passes;
  - a framebuffer read-modify-write per world pixel.
- **Filler shape.** The combined world pixel is two nearest-neighbour
  fetches (the lightmap is 1/16 texel density, so it is cheap to fetch), a
  multiply, x2, a clamp and a store. With depth test and write it is the
  base filler plus a few instructions. It must be a dedicated filler, not
  the general stage chain.
- **Also needed:** `GL_TEXTURE1` state for enable, bind, texenv and
  texcoords, and `glClientActiveTextureARB` for vertex arrays (QuakeSpasm
  draws the world with `glBegin(GL_POLYGON)` and `GL_MTexCoord2fFunc`, but
  still requires the entry point).
- **Do not advertise** `GL_ARB_vertex_buffer_object` or GL 1.5+ with it.
  QuakeSpasm would then want 3 units plus GLSL for its "fast path"
  (`r_brush.c` 623 and 647, `gl_vidsdl.c` 1265), which we cannot give.

### O2. A fused filler for the patterns the general pipeline sees from Quake

**Estimate: -20 to -25% of frame time, about 1.3x fps, if O1 is not done;
it still helps after O1.** Today every blended pixel runs five stage loops
over a chunk, with arrays in between. The signatures QuakeSpasm produces
(from the profile's stage symbols) are:
- **Lightmap pass:** nearest `REPEAT` fetch, `REPLACE` of an RGBA texture,
  `DST_COLOR/SRC_COLOR` blend, depth `LEQUAL` with no write. Symbols:
  `zd_lequal`, `zt_rr`, `ze_replace_rgba`, `zo_mul2`.
- **Glow and alias second passes:** `MODULATE` (smooth colour) or `REPLACE`,
  `ONE, ONE`, `LEQUAL` with no write. Symbol: `zo_one_one`.
- **Particles, water and translucent surfaces:** `MODULATE`,
  `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`. Symbol: `zo_sa_omsa`.

One specialised span loop per signature, chosen at triangle setup as the
plan's "specialise, do not branch" rule says, would bring these close to
`ZB_fillTriangleMappingPerspective`'s cost. That filler is 7.8% of CPU for a
full pass today.

### O3. Lightmaps: RGB storage when every alpha is 255, and precision

This is part correctness and part memory.
- QuakeSpasm uploads lightmaps as `GL_RGBA` (internal format 4). Alpha is
  always 255 (`r_brush.c` 876-879). libGL therefore stores RGB565 plus an A8
  plane, 192 kB per 256x256 block, and samples them with
  `ze_replace_rgba` instead of the RGB path.
- An upload whose alpha is all 255 can be stored as the RGB class, which
  saves 64 kB per block. It needs promoting back if a later
  `glTexSubImage2D` brings real alpha.
- **Darkness** (`dark/DARKNESS.md`, host rig against Mesa): our frames lose
  1-14% of luminance, up to 23% in the darkest lightmaps, from two defects:
  - (A) `T565` truncation of lightmap texels (`texture.c`:326);
  - (B) flooring `MUL8` in the blenders (`zpipe.c`:13).
- Rounding `T565` restores 0.96-1.04 of exact in the microtest; a
  diagnostic build lifted in-game luminance to 0.987-1.030 of Mesa. Rounded
  blends (`t = x*y + 128; (t + (t >> 8)) >> 8`) fix the lost pickup flash,
  where blue 69 at alpha 28 gives 0 in ours and 1 level in Mesa.
- Storing 8-bit lightmaps would be exact, but costs 256 kB instead of
  192 kB per block, so rounding is the better fit for this board's RAM.

### O4. Immediate-mode geometry (14.9% of CPU in fullscreen)

The top symbols are `gl_vertex4f`, `gl_draw_triangle`, `set_flat`,
`gl_transform_to_viewport`, `glopBegin` and `glopEnd`, `tgl_glVertex4f`,
`glTexCoord2f` and `tgl_glColor4f`.
- QuakeSpasm sends every world polygon and every alias vertex through
  `glBegin`/`glEnd`, per pass.
- O1 halves the world passes and thirds the alias passes. After that the
  per-vertex path is the next cost: one transform, one clip-code pass and a
  triangle setup per vertex.
- `GL_POLYGON` fans should reuse transformed vertices (check that nothing is
  transformed twice).
- The alias frames are strips and fans with `glColor` per vertex, so the
  colour path should not re-derive lighting state per vertex.

### O5. Hot libGL text in RAM (the S31GL_RAMTEXT lever)

**Estimate: 3-8%.** The measurement arm was the same libGL bytes loaded
from SD (`LD_LIBRARY_PATH` to a copy, so the text sits in the page cache):
268.4 s against 276.0 s from XIP, +2.8%. That is one run, and it came last
in the boot's sequence, so the gain may be larger.
- Stage 3 measured about 8% on glxgears.
- QuakeSpasm touches far more of libGL per frame than gears: five stage
  families, several fillers and the whole immediate-mode path. That is well
  past the 16 KB I-cache.
- The fillers and stage loops are the candidates. Under memory pressure the
  RAM copy must be resident (tens of kB), or it will be evicted and
  refetched from SD, which is worse than XIP.

### O6. Clear

`ZB_fill16` plus the depth clear are 1.7-3% of CPU. Quake clears depth every
frame, and colour only when `gl_clear 1` (off by default). The depth
ping-pong (G03) or PPA clear-at-swap (P3) removes this.

## 3. Warnings for the next library (measure QuakeSpasm before shipping)

- **Trilinear by default.** QuakeSpasm's default `gl_texturemode` is
  `GL_LINEAR_MIPMAP_LINEAR` (`gl_texmgr.c` 62-71). Its lightmaps are
  `GL_LINEAR`.
  - The shipped library samples nearest from level 0. The console says:
    "approximated GL_LINEAR / mipmap texture filters by nearest sampling of
    level 0".
  - The phase-4 work in the tree (F-LIN, `s31_tfilter.c`) draws real
    bilinear and trilinear. That is correct, but it will make every world
    pixel several times dearer, and it applies to QuakeSpasm by default.
  - Measure `timedemo demo1` with it. The owner can choose the app's own
    `gl_texturemode GL_NEAREST` (the classic software-Quake look) in
    QuakeSpasm's menu or config. It is a visual choice.
- **Mipmap storage** (phase 4 stores levels above 0 by default;
  `S31GL_MIPMAPS=0` turns it off). QuakeSpasm uploads a full mip chain for
  every world texture (`TexMgr_LoadImage32`), which is up to +33% texture
  memory.
  - This app already has 16-17 MB in swap at a 12 MB hunk, and VmRSS is
    about 4 MB.
  - Measure VmSwap and major faults during the timedemo with the new
    library.
- **`GL_MAX_TEXTURE_SIZE` 256.** QuakeSpasm resamples anything bigger (the
  conback) to fit. That is fine and saves memory; keep it.

## 4. Not libGL, for completeness

The platform levers measured in the same work are in README.md:
- the timer interrupt and scheduler overhead;
- the SD/swap cost;
- lvdesk at 3.3%;
- sound at about 1%;
- kernel flash text: the candidate pool for `.text..fast` is 2% of CPU0,
  about 1.5% at best.

None of them compares with O1 and O2.

### O7. No libc PIE routine on a per-frame path (found by the glxgears dips, 2026-09-26)

musl's libc.so here uses PIE (hart 1's SIMD) in strcmp, memcmp, memchr,
memrchr and in **memcpy from 64 bytes up** (memcpy+0x4e calls a 128-bit
esp.vld/esp.vst copier). Linux CPU1 is the lent hart 0, which has no PIE: a
client that reaches one of these there traps, and the kernel moves it to
CPU0 (esp32s31_pie_bounce), onto the desktop's or its own partner's CPU.
Whenever the scheduler places the GL client on CPU1, that is what sends it
back and starts the client/desktop ping-pong behind the fullscreen dips
(artifacts/gl/dips/README.md).

- **Where, measured.** rootfs/nopie.so in stock glxgears -fullscreen:
  **6,159 memcmp calls in ~40 s (about 4 per frame) from one site**, libGL
  +0x2839c (the shipped 927239ee), which is `glopBegin` ->
  gl/tinygl/source/vertex.c:203-205:
  `memcpy(b, a, sizeof b); if (c->mvinv_valid && memcmp(b, c->mvinv_src, sizeof b) == 0)`
  - both 64 bytes, i.e. both on the PIE path. The #394 trap log shows
  glxgears trapping at libc+0x107b0 from libc+0x5894a (that memcpy).
  raster_sel.c:225 (`memcmp(key, x->st_key, sizeof key)`) is the other
  libGL memcmp; it did not show in the count.
- **Fix (library round, gl/ is not the board agent's to edit).** Either an
  open-coded 16-word compare/copy for the modelview cache (it is a fixed
  64-byte, 4-aligned block, so a word loop is as fast as anything and never
  traps), or the same dispatch lvdesk now uses (lvdesk/lentcpu.c): read the
  thread's rseq cpu_id (one load; register the rseq area once per thread,
  fall back to scalar if the syscall fails) and call libc's PIE routine,
  resolved once with dlsym(RTLD_NEXT), only when on CPU0; scalar otherwise.
  For a 64-byte block the word loop is the simpler and sufficient choice;
  dispatch is the pattern for large copies (texture uploads, glReadPixels).
- **Gain.** Not a throughput lever: glxgears on CPU1 without a trap is a
  stable placement instead of a bounce, which is what shortens the clusters.
  Measure with scripts/board/swt-arm.sh (long frames and PIE bounces per
  window) before and after.
