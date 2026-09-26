# QuakeSpasm 0.96.3 "far too dark" on our libGL: cause

Host-only study, 2026-09-26. Stock QuakeSpasm 0.96.3 was built for the host
with the board's switches (`USE_SDL2=0` against the rig's sdl12-compat, no
MP3/Vorbis). It ran under Xvfb 800x480x16 in the s31-glref container with
the glref capture shim, at 320x240 windowed. Frames are deterministic:
`timedemo demo1` uses one demo message per frame, and the shim's virtual
clock runs at 1/60 s per swap. The same frame was captured under three arms:

| arm | what it is |
|---|---|
| **mesa-full** | llvmpipe with every extension: GLSL world and alias paths. This is the "reference look". |
| **mesa-nocomb** | llvmpipe with QuakeSpasm's own switches `-novbo -nomtex -nocombine -noadd -notexturenpot -noglsl -nowarpmipmaps`. This is the same code path the board takes (world "case 3", Fitz alias renderer, hardware gamma). |
| **ours** | `gl/out-host/libGL.so.1` (built 10:26, frozen copy in `host/ours/`). It prints exactly the board's warnings: no multitexture, combine, add or NPOT, GL 1.1 so no GLSL, "using hardware gamma". |

## The answer

1. **At the frame in the board screenshot, the board is not darker than
   the host rendering of the same frame.** It is also only 5-13% darker than
   Mesa's reference. That frame is the start of e1m3 (the Necropolis), a
   genuinely dark room, with the console still covering the top of the
   screen at `scr_conalpha 0.5`. Mesa draws it just as dark: the lower view
   averages 6.7/255 in luminance under Mesa and 5.7/255 on the board.
2. **Most of the darkness is the content.** QuakeSpasm's default look at
   `gamma 1` is dark, and the missing extensions barely change it: Mesa with
   full extensions is only 1.3-3.8% brighter than Mesa on the board's
   no-combiner path. The two-pass `glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)`
   lightmap path is fine in principle. QuakeSpasm's overbright lighting also
   matches software Quake's colormap response to within 2% (`host/colormap.py`,
   table below), so GLQuake here is not inherently darker than WinQuake.
3. **Our libGL removes a further 1-14% of luminance, and up to 23% in the
   darkest lightmaps.** The loss comes from two precision defects, both in
   our libGL:
   - **(A) Lightmap texels are stored as truncated RGB565.**
     `gl/tinygl/source/texture.c`, `T565` (line 326), used by
     `tex_store_img()`. Mesa keeps the lightmap at 8 bits. A lightmap value
     L loses `L & 7` in R and B and `L & 3` in G before the 2x modulate
     doubles the loss. Dark lightmaps are exactly where this hurts:
     L=20 renders 16 in R and B (-20%).
   - **(B) Blend arithmetic floors where Mesa rounds.**
     `gl/tinygl/source/zpipe.c` `MUL8` (line 13), used by `zo_sa_omsa`,
     `zo_mul2` and the generic blender. The floor only costs about one 8-bit
     unit. But a 565 target truncates, so that one unit drops a whole 565
     level whenever the exact result sits just above a level boundary. That
     happens constantly in dark scenes.
4. **`zo_mul2` is correct.** It computes `clamp255(2 * MUL8(src, dst))`,
   which is twice the product, clamped. Because `MUL8` multiplies by
   `(y+1)`, it runs slightly *bright*: at L=128 the 2x modulate is +3.8
   (ours) against +0.8 (Mesa) on R and B. It is not the darkening.
5. **The lightmap upload path is correct.** QuakeSpasm uses internal format
   4, `GL_RGBA`, one `glTexImage2D` followed by `glTexSubImage2D`, into
   256x256 `LMBLOCK`s. That path loses nothing beyond defect (A).
6. **Gamma is not involved.** At `gamma 1` QuakeSpasm's ramp is the
   identity (`VID_Gamma_f`: `(255*pow((i+0.5)/255.5, 1) + 0.5)` gives i). On
   the rig, sdl12-compat reports `gamma adjustment not available`. The board
   frame's luminance equals the host "ours" luminance, so nothing on the
   board side (X, SDL 1.2.15, scanout) darkens it. The consequence is the
   other way round: with no GLSL, QuakeSpasm's `gamma` cvar can only brighten
   the picture through hardware gamma. If the board's console also prints
   `gamma adjustment not available`, a user cannot brighten it from the
   menu. That was not checked on the board (host-only task).

## Evidence

### 1. The board frame against the same frame on the host

The board screenshot's window client area (origin 151,81) was aligned on the
HUD, with a mean absolute difference of 4.8 against host "ours" (JPEG noise).
The host frames come from the stock start with no autoexec (`host/plain`),
which is exactly what the board runs. Frames 44-48 match the console height
and the view. The table gives mean luminance (Rec.601, 0-255) per region.
`view_low` is rows 130-191, below the console.

| arm | full | view | view_low | hud |
|---|---:|---:|---:|---:|
| **board** (quakespasm-first.jpg) | **17.71** | **16.33** | **5.72** | **23.24** |
| ours f44 / f48 | 17.89 / 17.33 | 16.49 / 15.78 | 5.77 / 5.81 | 23.49 / 23.56 |
| mesa-nocomb f44 / f48 | 18.72 / 18.11 | 17.10 / 16.32 | 6.17 / 6.17 | 25.23 / 25.29 |
| mesa-full f44 / f48 | 19.12 / 18.52 | 17.53 / 16.77 | 6.68 / 6.68 | 25.47 / 25.53 |

Image: `sbs/plain.f48.png` (board | mesa-full | mesa-nocomb | ours).

### 2. Lit frames inside demo1 (timedemo), view luminance (rows 0-191)

| frame | mesa-full | mesa-nocomb | ours | ours / mesa-nocomb | mean dR, dG, dB (ours - mesa-nocomb) |
|---|---:|---:|---:|---:|---|
| 250 | 32.66 | 31.87 | 31.63 | 0.992 | -0.15 +0.67 **-5.19** (the flash, see 3c) |
| 400 | 21.55 | 21.27 | 20.31 | 0.955 | -1.38 -0.89 -0.24 |
| 550 | 8.60 | 8.39 | 7.22 | **0.860** | -0.29 -1.69 -0.84 |
| 700 | 20.15 | 19.43 | 18.99 | 0.977 | +0.04 -0.85 +0.36 |
| 850 | 7.69 | 7.40 | 7.15 | 0.966 | -0.29 -0.30 +0.08 |

- The whole frame, HUD included, shows the same pattern. The HUD is
  `scr_sbaralpha 0.75` blended over the view, so it inherits the view's
  loss: HUD 30.4 against 32.0 at f250.
- Every one of these frames still PASSes glref's tolerant metric (at most
  0.33% bad pixels), because each difference is at most one 565 level. That
  is why the harness never flagged it. The loss is systematic in sign and
  concentrated in dark pixels. Mean luminance sees it; the tolerant metric
  does not.
- Mesa-full against mesa-nocomb, which isolates the missing extensions: a
  ratio of 1.013-1.038. Diffs: `diff/path.f*.png` (tolerant 0.000-0.025%).

Tables: `compare-all.txt`, `variants.txt`. Images: `sbs/timedemo.f*.png`,
`diff/libgl.f*.png`.

### 3. The mechanisms, isolated (microtests, `rampt/`)

**(a) The lightmap pass, exactly as QuakeSpasm draws it** (`host/rampt5.c`).
This uses a 256x256 internal-format-4 lightmap, `TexImage2D` then
`TexSubImage2D`, `GL_LINEAR` magnified 64x, and `DST_COLOR,SRC_COLOR` over a
REPLACE texture pass. The table gives luminance as a fraction of the exact
`2*T*L/255`:

| lightmap L | Mesa | ours | ours, lightmap rounded (exp-lmround) | ours, all T565 rounded (exp-round) |
|---:|---:|---:|---:|---:|
| 20 | 0.905 | **0.772** | 0.948 | 0.963 |
| 39 | 0.966 | **0.830** | 0.977 | 0.997 |
| 55 | 0.979 | **0.884** | 0.993 | 1.015 |
| 71 | 0.986 | **0.929** | 1.005 | 1.025 |
| 100 | 0.994 | 0.991 | 1.025 | 1.042 |
| 135 | 1.000 | 0.978 | 1.022 | 1.042 |
| 199 | 1.000 | 0.997 | 1.018 | 1.031 |

With lightmap values that are multiples of 8 (`rampt.txt`) ours equals
Mesa. The loss is purely the storage truncation (`rampt-odd.txt`: L=20
gives R/B = 12.50 against Mesa 17.69).

**(b) Everything else, each an identity in exact arithmetic**
(`host/rampt2.c`, `rampt2.txt`). REPLACE, MODULATE by 1.0, alpha 0.75 over
itself, LINEAR at texel centres, trilinear-minified constant textures, and
GL_ONE,GL_ONE add of black are **identical between ours and Mesa**. Both
truncate to 565 on output (per-texel dumps in `rampt.txt`: x=39 gives 33 in
both). Untextured Gouraud is ours -1.2 R / -2.0 B.

**(c) The pickup flash (V_PolyBlend)** at timedemo frame 250. The flash
colour is `(215,186,69)/255` at alpha 28/255, logged from the running game
with `host/colorlog.c`. The same frame with `gl_polyblend 0` (`nopb/`) has
ours/mesa = 0.982 and blue +0.43, so the -5.19 blue belongs to the flash
alone. On a black pixel, the blue gets 69*28/255 = 7.58. Mesa rounds that in
8 bits to 8, which is 565 level 1. Ours computes `MUL8(69,28) = 7`, which is
level 0. So ours adds **no** blue from the flash anywhere on a dark screen:
the fitted flash alpha is 0.000 on B, against 0.09 on R and G. Mesa's view
blue with the flash is 8.85 and ours is 3.66. That is defect (B) exactly.
The polyblend primitive itself is fine in isolation (`rampt4*.txt`: an
identical quad gives identical pixels). The issue is the 1-unit floor
landing on a level boundary.

**(d) Fixing only (A) closes most of the in-game gap.** The diagnostic
libGLs were built from a copy of `gl/`; `gl/` itself was not touched
(`host/build-exp.sh`). exp-base is the unpatched copy and reproduces the
frozen library bit for bit.

| frame | ours / mesa-nocomb | exp-lmround | exp-round |
|---|---:|---:|---:|
| 250 | 0.992 | 1.009 | 1.084 |
| 400 | 0.955 | 0.987 | 1.050 |
| 550 | 0.860 | 0.896 | 1.043 |
| 700 | 0.977 | 1.006 | 1.074 |
| 850 | 0.966 | 1.030 | 1.178 |
| start f48 | 0.967 | 0.989 | 1.122 |

- exp-lmround rounds T565 only for 256-wide internal-format-4 textures, a
  diagnostic hack that picks out QuakeSpasm's lightmap blocks.
- exp-round rounds every texture. That overshoots Mesa, because Mesa keeps
  8-bit texels and only truncates once, on the final 565 write.
- The remainder at f550 (0.896) is defect (B) and floor rounding in
  filtering and Gouraud colour, which is not separately isolated here.

### 4. The missing extensions: what QuakeSpasm would use

Verified in the 0.96.3 source.

- **World** (`r_world.c` 1180-1192, case 1). This needs `GL_ARB_multitexture`
  (2 units) plus `GL_ARB_texture_env_combine` or the EXT version:
  - unit 0 holds the texture;
  - unit 1 holds the lightmap with `GL_TEXTURE_ENV_MODE = GL_COMBINE`,
    `GL_COMBINE_RGB = GL_MODULATE`, `GL_SOURCE0_RGB = GL_PREVIOUS`,
    `GL_SOURCE1_RGB = GL_TEXTURE`, `GL_RGB_SCALE = 2.0`, and the default
    operands (`SRC_COLOR`).
  - Without combine, QuakeSpasm uses case 3 (1200-1224): a texture pass,
    then the lightmap with `glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)`, depth
    writes off. The lightmaps are built at half intensity (`r_brush.c`
    856-860, `>> 8` when `gl_overbright`).
- **Alias models** (`r_alias.c` 768-817):
  - Case 1 does everything in one pass. It needs combine + multitexture +
    `GL_ARB_texture_env_add`, with `COMBINE_RGB = MODULATE`,
    `SOURCE0 = TEXTURE`, `SOURCE1 = PRIMARY_COLOR`, `RGB_SCALE = 2.0`, then
    `GL_ADD` on unit 1 for fullbrights.
  - Case 2 is the same combiner without add, followed by a fullbright pass.
  - Correction to the brief: **opaque models keep overbright without
    combiners.** Case 3 (818-850) draws MODULATE, then the same frame again
    with `GL_ONE, GL_ONE`, which is 2x. Only translucent entities
    (`entalpha < 1`, line 687) lose overbright.
- **Sky** (`gl_sky.c` 916-936). With multitexture it draws in one pass,
  with `GL_DECAL` on unit 1 for the alpha layer, and no combiners. Without
  multitexture it uses two blended passes.
- **GLSL** needs GL >= 2.0. That also disables "GLSL gamma", which is why
  the game falls back to hardware gamma.

Adding combine + multitexture would make QuakeSpasm take case 1 (and alias
cases 1/2). That saves one full world pass per frame, which matters for
speed on this board, and one 565 round trip between passes, worth roughly
the 1.3-3.8% gap between mesa-full and mesa-nocomb. **It does not fix the
darkness by itself.** The lightmap would still be sampled from truncated
565 texels (defect A).

### 5. Software Quake response against GL overbright (`host/colormap.py`, from pak0's colormap.lmp)

| lightmap | software gain (median over the palette) | GL overbright 2L/255 |
|---:|---:|---:|
| 16 | 0.129 | 0.125 |
| 64 | 0.516 | 0.502 |
| 128 | 1.000 | 1.004 |
| 192 | 1.498 | 1.506 |
| 255 | 1.952 | 2.000 |

QuakeSpasm's lighting maths is the software renderer's to within 2-3%, so
the reference look is not a GL-only darkening.

## Fix recommendations (libGL side; QuakeSpasm stays stock)

1. **Lightmap precision (A), the largest effect.** Use one of these:
   - **Zero memory:** round to nearest in `T565` (`min(255, v + 4) >> 3`
     for R and B, `+2` for G) instead of truncating. In the microtest this
     is the most accurate against exact maths (0.96-1.04). It is 4-18%
     brighter than Mesa in game, because Mesa's texels are exact.
   - **Exact:** store 8 bits per channel for internal formats that ask for
     them (3/4, `GL_RGB8`, `GL_RGBA8`).

   Today a QuakeSpasm lightmap block (256x256, format 4) costs 192 KB as
   565+A8. RGBA8888 would be 256 KB, and RGB888+A8 would be 256 KB too.
   Rounding costs nothing, and is the first thing to try given the 15.4 MB
   budget.
2. **Blend rounding (B).** Replace `MUL8`'s floor with the rounded
   `x*y/255`, `t = x*y + 128; (t + (t >> 8)) >> 8`, in `zo_sa_omsa`,
   `zo_mul`, `zo_mul2` and `zo_generic`, so that a blend lands where Mesa's
   does before the 565 truncation. Frame 250's flash is the test case: the
   view's blue should go from 3.66 to about 8.9.
3. **For speed, and a last 1-4%:** `GL_ARB_multitexture` (2 units) plus
   `GL_ARB_texture_env_combine` limited to the modes listed in section 4
   (MODULATE with sources PREVIOUS/TEXTURE and TEXTURE/PRIMARY_COLOR,
   RGB_SCALE 1 and 2). `GL_ARB_texture_env_add` would complete alias
   case 1.
4. **Verification on the rig:** re-run `host/compare-all.sh` and
   `host/variants.py` against a rebuilt libGL. ours/mesa-nocomb should reach
   about 1.00 in every frame, and `rampt5` should reach Mesa's column.

## Anything else wrong in our frames against Mesa (same path)

- **Nothing structural.** Textures, water warp (demo2 f400, underwater),
  torches, alias models (zombies, fish, ogre, view weapons), particles
  (demo3 f500 teleporter field), centerprints and the HUD all match.
  Positions are identical, so the game state is deterministic across arms.
- **demo3 f500 is the one FAIL** (tolerant 3.0%). It comes from a
  high-frequency speckled wall texture where ours and llvmpipe sample
  different texels or mip levels, which is LOD and filter phase. Particles
  are in the same places. There is no mean shift: ours 30.94 against Mesa
  30.03.
- **Sky was not in any captured frame** (demo1-3 at the frames used). It
  was not checked.
- **Blue is hit hardest in dark scenes.** R and B have 5 bits against G's
  6, so every truncation loses about twice as much in R and B. Dark areas
  shift slightly green, as in the f250 flash.

## Files

- `DARKNESS.md`: this file. `compare-all.txt` and `variants.txt` hold the
  tables.
- Captures, as `<arm>/<name>.f<frame>.png` plus `.log`/`.status`:
  - `mesa/` holds `mesa-full.*`, `mesa-nocomb.*` and `plain-mesa-*`;
  - `ours/` holds `ours.*` and `plain-ours.*`;
  - `exp-{base,round,lmround}/ours/` hold the diagnostic libGLs' captures;
  - `nopb/` is f250 with `gl_polyblend 0`;
  - `demo2/` and `demo3/` hold the other demos.
- `diff/`: glref `compare.py` diff images. `libgl.f*` is mesa-nocomb vs
  ours; `path.f*` is mesa-full vs mesa-nocomb; `plain-libgl.f44/48` is the
  board frame.
- `sbs/`: side by side. `plain.f44/48` shows board, mesa-full, mesa-nocomb
  and ours; `timedemo.f*` shows the three host arms.
- `rampt/`: microtest captures and scores (`rampt*.txt`).
- `../host/`, the rig:
  - `build-host.sh` builds QuakeSpasm into `host/quakespasm` from a source
    copy in `host/src`;
  - `run-arm.sh <mesa|ours> <name> <frame> [switches]` runs one arm, with
    `BD=` for another basedir;
  - `compare-all.sh`, `variants.py` and `analyze.py` make the tables;
  - `build-exp.sh` builds the diagnostic libGLs from a copy of `gl/`;
  - `rampt*.c` / `rampt*.py` are the microtests and their scorers;
  - `colorlog.c` is the polyblend logger and `colormap.py` the software
    response table.
- Game data: `host/id1/pak0.pak`, hard-linked into `plain/`, `nopb/`, `d2/`
  and `d3/`. It is gitignored. `id1/autoexec.cfg` drives
  `timedemo demo1`, because shareware ignores `+commands`.

Caveats:
- The board frame is a JPEG, so region means carry about ±1 unit of noise.
- The frame-44-48 match is on console height and view, not an exact swap.
- A `config.cfg` that QuakeSpasm writes into `host/id1` was shown not to
  change any capture: every demo1 arm was re-run with it present and gave
  identical numbers.
- The board's own libGL was not inspected. The match in table 1 is the
  evidence that it behaves like this host build.
