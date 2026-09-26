/*
 * raster_sel.c - gl_update_raster and gl_build_pipe: which rasteriser path
 * draws, decided at glBegin (see raster.c for the model). Cold code - it
 * runs once per state change - so it is built -Os (gl/api/build-lib.sh).
 * s31, MIT.
 */
#include <stdlib.h>
#include <string.h>
#include "zgl.h"
#include "zpipe.h"
#include "raster_int.h"
#include "s31_ramtext.h"

/* texenv op for (mode, stored format class), GL 1.3 table 3.22 */
static int texenv_op(int mode, int fmt)
{
  switch (fmt) {
  case TGL_TEXF_RGB:
    switch (mode) {
    case GL_MODULATE: return ZP_TE_MOD_RGB;
    case GL_BLEND: return ZP_TE_BLEND_RGB;
    case GL_ADD: return ZP_TE_ADD_RGB;
    default: return ZP_TE_REPLACE_RGB;          /* REPLACE, DECAL */
    }
  case TGL_TEXF_RGBA:
    switch (mode) {
    case GL_MODULATE: return ZP_TE_MOD_RGBA;
    case GL_BLEND: return ZP_TE_BLEND_RGBA;
    case GL_ADD: return ZP_TE_ADD_RGBA;
    case GL_DECAL: return ZP_TE_DECAL_RGBA;
    default: return ZP_TE_REPLACE_RGBA;
    }
  case TGL_TEXF_ALPHA:
    /* the colour is the fragment's in every mode */
    return mode == GL_REPLACE ? ZP_TE_ALPHA_REPLACE : ZP_TE_ALPHA_MOD;
  default:                                     /* TGL_TEXF_INTENSITY */
    switch (mode) {
    case GL_MODULATE: return ZP_TE_MOD_RGBA;
    case GL_BLEND: return ZP_TE_BLEND_I;
    case GL_ADD: return ZP_TE_ADD_I;
    default: return ZP_TE_REPLACE_RGBA;         /* DECAL is undefined */
    }
  }
}

static int depth_sel(int func)
{
  switch (func) {
  case GL_NEVER: return ZP_DEPTH_NEVER;
  case GL_LESS: return ZP_DEPTH_LESS;
  case GL_EQUAL: return ZP_DEPTH_EQUAL;
  case GL_LEQUAL: return ZP_DEPTH_LEQUAL;
  case GL_GREATER: return ZP_DEPTH_GREATER;
  case GL_NOTEQUAL: return ZP_DEPTH_NOTEQUAL;
  case GL_GEQUAL: return ZP_DEPTH_GEQUAL;
  default: return ZP_DEPTH_ALWAYS;
  }
}

static void gl_draw_triangle_skip(GLContext *c, GLVertex *p0, GLVertex *p1,
                                  GLVertex *p2)
{
  (void)c; (void)p0; (void)p1; (void)p2;
}

/* ------------------------------------------------------------ phase 4 knobs */

static int knob(const char *name, int def)
{
  const char *e = getenv(name);
  return e ? atoi(e) : def;
}
static int tri_on = -1;

/* S31GL_SMOOTH_NOBLEND (phase 4 SMOOTH). 1, the default: smooth lines and
   points are drawn with their coverage footprint whether blending is on or
   off - GL makes no exception for blending (the coverage then only changes
   an alpha nothing reads, and every covered pixel is written at full
   colour), and Mesa, the correctness reference, does the same: measured on
   rRootage, which draws its shapes' outlines smooth with blending off,
   f300 is 0.59% tolerant-bad against Mesa this way and 1.10% (FAIL)
   aliased (artifacts/gl/phase4/sm1, sm3). 0: aliased when blending is
   off, the brief's rule, kept as the A/B arm */
static int aa_nb = -1;
static int aa_noblend(void)
{
  if (aa_nb < 0) aa_nb = knob("S31GL_SMOOTH_NOBLEND", 1) != 0;
  return aa_nb;
}

/* S31GL_TRILINEAR=0: GL_LINEAR_MIPMAP_LINEAR as LINEAR_MIPMAP_NEAREST */
static int zpx_tri_enabled(void)
{
  if (tri_on < 0) tri_on = knob("S31GL_TRILINEAR", 1) != 0;
  return tri_on;
}

/* phase 4 F-LIN: the texture's filter kinds and stored level chain into
   c->pipex. Returns 0 when the texture is drawn exactly as before phase 4
   (nearest in level 0, both filters), 1 when it is filtered. */
__attribute__((noinline))
static int zpx_prepare(GLContext *c, const GLTexture *t)
{
  ZPipeX *x = &c->pipex;
  int mipf, mip = 0, l, n = t->ws > t->hs ? t->ws : t->hs, F = t->fbits;
  int ws = t->ws, hs = t->hs;

  mipf = t->min_filter != GL_NEAREST && t->min_filter != GL_LINEAR;
  /* GL_TEXTURE_MAX_LEVEL ends the chain early (review 4 R4) */
  if (n > t->max_level) n = t->max_level;
  /* the chain is usable when every level down to 1x1 (or MAX_LEVEL) is
     stored, at the halved size, in level 0's class (gl_texture_complete
     checked the sizes and internal formats) */
  if (mipf && c->mip_store && t->mip && n > 0 && n < ZPX_MAXLEV) {
    mip = 1;
    for (l = 1; l <= n; l++) {
      const GLMipLevel *m = &t->mip->l[l];
      int wl = ws > l ? ws - l : 0, hl = hs > l ? hs - l : 0;
      if (m->pix == NULL || m->cls != t->fmt || m->ws != wl || m->hs != hl) {
        mip = 0;
        break;
      }
    }
  }
  x->kmag = t->mag_filter == GL_LINEAR ? TF_LINEAR0 : TF_NEAREST0;
  switch (t->min_filter) {
  case GL_NEAREST: x->kmin = TF_NEAREST0; break;
  case GL_LINEAR: x->kmin = TF_LINEAR0; break;
  case GL_NEAREST_MIPMAP_NEAREST: x->kmin = mip ? TF_NMN : TF_NEAREST0; break;
  case GL_LINEAR_MIPMAP_NEAREST: x->kmin = mip ? TF_LMN : TF_LINEAR0; break;
  case GL_NEAREST_MIPMAP_LINEAR: x->kmin = mip ? TF_NML : TF_NEAREST0; break;
  default: x->kmin = mip ? (zpx_tri_enabled() ? TF_LML : TF_LMN) : TF_LINEAR0; break;
  }
  if (mipf && !mip && n > 0 && (c->mip_store || t->mip))
    gl_note_once("mipmap filter on an incomplete stored chain (level 0 sampled)");
  if (mipf && t->base_level != 0)
    gl_note_once("GL_TEXTURE_BASE_LEVEL > 0 (level 0 is the base)");
  if (x->kmag == TF_NEAREST0 && x->kmin == TF_NEAREST0) return 0;
  if (t->wrap_s == GL_CLAMP || t->wrap_t == GL_CLAMP)
    gl_note_once("GL_CLAMP with a linear texture filter (drawn as GL_CLAMP_TO_EDGE)");
  /* the switch to minification (review 4 R3): Mesa's lambda > 0 for
     every filter pair, which is also what D3D-class hardware does. GL 1.3
     3.8.8 has c = 0.5 for LINEAR magnification with NEAREST_MIPMAP_*; with
     it, 0 < lambda <= 0.5 drew LINEAR from level 0 where the reference
     draws NEAREST (99.2% of a glx_rev cell). The reference wins: the suite
     is scored against it */
  x->lod_c = 0.0f;
  x->lod_min = t->min_lod;
  x->lod_max = t->max_lod;
  x->lod_gen = t->min_lod != -1000.0f || t->max_lod != 1000.0f;
  x->nlev = mip ? n : 0;
  /* s, t fixed point -> level-0 texels, squared (rho^2) */
  x->lod_ks = 1.0f / (float)(1u << (2 * F));
  x->lod_kt = 1.0f / (float)(1u << F) / (float)(1u << F) /
              (float)(1u << ws) / (float)(1u << ws);
  for (l = 0; l <= x->nlev; l++) {
    ZLevel *L = &x->lvl[l];
    int wl = ws > l ? ws - l : 0, hl = hs > l ? hs - l : 0;
    if (l == 0) {
      L->pix = t->images[0].pixmap;
      L->alpha = t->alpha;
    } else {
      L->pix = t->mip->l[l].pix;
      L->alpha = t->mip->l[l].alpha;
    }
    L->ws = wl;
    L->wm = (1 << wl) - 1;
    L->hm = (1 << hl) - 1;
    L->shs = F + ws - wl;
    L->sht = F + ws + hs - hl;
  }
  return 1;
}

/* ------------------------------------------------------------ stencil */

/* one stencil op on stored value v (GL 1.4 table 4.1 of 4.1.5, with
   INCR_WRAP / DECR_WRAP), through the write mask */
static unsigned int zs_op(int op, unsigned int v, unsigned int ref, unsigned int wm)
{
  unsigned int r;
  switch (op) {
  case GL_ZERO: r = 0; break;
  case GL_REPLACE: r = ref; break;
  case GL_INCR: r = v < 255 ? v + 1 : 255; break;
  case GL_DECR: r = v > 0 ? v - 1 : 0; break;
  case GL_INVERT: r = ~v & 255; break;
  case GL_INCR_WRAP: r = (v + 1) & 255; break;
  case GL_DECR_WRAP: r = (v - 1) & 255; break;
  default: r = v; break;                          /* GL_KEEP */
  }
  return (v & ~wm) | (r & wm);
}

/* phase 4 F8: the context's stencil table, allocated at its first stencil
   batch (review 4 R6-size: 1 kB that sat in every context). 0 with no
   memory: GL_OUT_OF_MEMORY, and the batch draws as without a stencil
   buffer */
static int zs_table(GLContext *c)
{
  ZPipeX *x = &c->pipex;
  if (x->stab != NULL) return 1;
  x->stab = gl_malloc(256 * sizeof(unsigned int));
  x->st_valid = 0;
  if (x->stab == NULL) {
    gl_set_error(c, GL_OUT_OF_MEMORY);
    return 0;
  }
  return 1;
}

/* phase 4 F8: ZPipeX.stab for the stencil state (cold: rebuilt only when
   the state it was made for changed; 256 entries) */
__attribute__((noinline))
static void zs_build(GLContext *c)
{
  ZPipeX *x = &c->pipex;
  int key[7], s;
  unsigned int ref, vm, wm, a;

  key[0] = c->stencil_func; key[1] = c->stencil_ref;
  key[2] = c->stencil_value_mask; key[3] = c->stencil_writemask;
  key[4] = c->stencil_fail; key[5] = c->stencil_zfail; key[6] = c->stencil_zpass;
  if (x->st_valid && memcmp(key, x->st_key, sizeof key) == 0) return;
  memcpy(x->st_key, key, sizeof key);
  x->st_valid = 1;
  /* GL: the reference is clamped to [0, 2^s - 1]; the masks use s bits */
  ref = c->stencil_ref < 0 ? 0 : (c->stencil_ref > 255 ? 255 : (unsigned int)c->stencil_ref);
  vm = (unsigned int)c->stencil_value_mask & 255;
  wm = (unsigned int)c->stencil_writemask & 255;
  a = ref & vm;
  for (s = 0; s < 256; s++) {
    unsigned int b = (unsigned int)s & vm, pass;
    switch (c->stencil_func) {
    case GL_NEVER: pass = 0; break;
    case GL_LESS: pass = a < b; break;
    case GL_LEQUAL: pass = a <= b; break;
    case GL_GREATER: pass = a > b; break;
    case GL_GEQUAL: pass = a >= b; break;
    case GL_EQUAL: pass = a == b; break;
    case GL_NOTEQUAL: pass = a != b; break;
    default: pass = 1; break;                     /* GL_ALWAYS */
    }
    x->stab[s] = zs_op(c->stencil_fail, (unsigned int)s, ref, wm) |
                 zs_op(c->stencil_zfail, (unsigned int)s, ref, wm) << 8 |
                 zs_op(c->stencil_zpass, (unsigned int)s, ref, wm) << 16 |
                 pass << 24;
  }
}

/* ------------------------------------------------------------ selection */

/* phase 4: the P4_R_* decisions for the phase 4 enables (called only when
   one is on: out of line, so gl_update_raster keeps its registers for the
   frames that use none - inline it cost 27 instructions a call).
   Stencil (F8): a stage of the general path, only with a stencil buffer
   (GL 1.3 4.1.5: without one the test passes and nothing is modified -
   exactly what not running it does); _W: it can write, so a primitive whose
   every fragment fails the depth test, or that writes no colour and no
   depth, still has an effect. Smooth lines and points (SMOOTH) are drawn
   with coverage - it reaches the pixels through alpha, so through blending;
   with blending off the footprint is written opaque, as GL and Mesa do
   (S31GL_SMOOTH_NOBLEND=0 keeps them aliased then) - at their real width,
   at least 1 (the smooth ranges start there, get.c) */
__attribute__((noinline))
static int p4_select(GLContext *c)
{
  int x4 = 0;
  if ((c->p4_en & P4_EN_STENCIL) && c->stencil_bits && zs_table(c)) {
    x4 = P4_R_STENCIL;
    if ((c->stencil_writemask & 0xff) != 0 &&
        (c->stencil_fail != GL_KEEP || c->stencil_zfail != GL_KEEP ||
         c->stencil_zpass != GL_KEEP))
      x4 |= P4_R_STENCIL_W;
  }
  if ((c->p4_en & (P4_EN_LSMOOTH | P4_EN_PSMOOTH)) && (c->blend_enabled || aa_noblend())) {
    /* widths in window pixels; a render-scaled buffer's are 2^rscale of them */
    float rf = c->rscale ? 1.0f / (float)(1 << c->rscale) : 1.0f;
    float lw = c->line_width * rf, ps = c->point_size * rf;
    if (c->p4_en & P4_EN_LSMOOTH) {
      x4 |= P4_R_AA_LINES;
      c->aa_lw = lw < 1.0f ? 1.0f : lw;
    }
    if (c->p4_en & P4_EN_PSMOOTH) {
      x4 |= P4_R_AA_POINTS;
      c->aa_ps = ps < 1.0f ? 1.0f : ps;
    }
  }
  return x4;
}

void gl_update_raster(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZPipe *p = &c->pipe;
  /* s31 (plan F7): GL_TEXTURE_2D wins over GL_TEXTURE_1D (GL 1.3 3.8.15);
     a 1D texture is a W x 1 image, so the same paths draw it */
  GLTexture *t = (c->tex_enables & 1) ? c->current_texture : c->current_texture_1d;
  int gen = 0, tex, clamp_s = 0, clamp_t = 0, tex_tier1 = 0, modwhite = 0;
  int sf, df, cm, nocolor, afunc, skip = 0, dsel, beq, sw, x4;

  c->raster_dirty = 0;
  zb->pipe = p;

  /* ---- texture (GL 1.3 3.8.10: an incomplete texture disables it) */
  tex = c->texture_2d_enabled && t != NULL && gl_texture_complete(t);
  c->tex_active = tex;
  c->tex_filtered = 0;
  if (tex) {
    int TW = 1 << t->ws, F = t->fbits;
    zb->current_texture = t->images[0].pixmap;
    zb->tex_smask = (TW - 1) << F;
    zb->tex_tmask = ((1 << t->hs) - 1) << (F + t->ws);
    zb->tex_shift = F - 1;                        /* byte offset */
    c->tex_smax = TW << F;
    c->tex_tmax = 1 << (F + t->ws + t->hs);
    c->tex_sscale = (float)c->tex_smax;
    c->tex_tscale = (float)c->tex_tmax;
    zb->tex_speriod = c->tex_smax;
    zb->tex_tperiod = c->tex_tmax;
    /* nearest sampling: GL_CLAMP and GL_CLAMP_TO_EDGE pick the same texel */
    clamp_s = t->wrap_s != GL_REPEAT;
    clamp_t = t->wrap_t != GL_REPEAT && t->hs > 0;   /* one row: no t */
    if (t->fmt == TGL_TEXF_RGB && !clamp_s && !clamp_t) {
      if (c->texenv_mode == GL_REPLACE || c->texenv_mode == GL_DECAL)
        tex_tier1 = 1;
      else if (c->texenv_mode == GL_MODULATE)
        modwhite = 1;
    }
    /* phase 4 F-LIN: a linear or mipmap filter is a stage of the general
       path (tier 1 is nearest in level 0 only) */
    /* (out of line, and not called for NEAREST / NEAREST: texobj binds
       two textures a frame, and inlined it put 244 B into its hot code) */
    /* S31GL_TEXFILTER=0 (c->tex_filter): every filter is nearest in
       level 0 on tier 1 and no level is stored, as before phase 4 (review
       4: the QuakeSpasm A/B arm) */
    c->tex_filtered = (t->mag_filter != GL_NEAREST || t->min_filter != GL_NEAREST) &&
                      c->tex_filter && zpx_prepare(c, t);
    if (c->tex_filtered) tex_tier1 = modwhite = 0;
    if (!tex_tier1 && !modwhite) gen = 1;
  }

  /* ---- GL_SEPARATE_SPECULAR_COLOR: only differs from the single colour
     when there is a texture to put the specular part on */
  c->raster_sepspec = c->color_control == GL_SEPARATE_SPECULAR_COLOR &&
                      c->lighting_enabled && tex;
  p->st_spec = c->raster_sepspec;
  if (c->raster_sepspec) gen = 1;

  /* ---- blending: no alpha plane, so the destination alpha is 1.
     (phase 4 BLEND-EQ) That also makes glBlendFuncSeparate's alpha
     factors and glBlendEquationSeparate's alpha equation exact to ignore:
     they only compute the alpha that would be stored, and there is no
     alpha plane to store it in (GL_ALPHA_BITS 0) - the RGB result never
     reads it, since GL_DST_ALPHA is 1 here whatever was blended */
  sf = GL_ONE; df = GL_ZERO;
  nocolor = 0;
  if (c->blend_enabled) {
    sf = c->blend_src; df = c->blend_dst;
    if (sf == GL_DST_ALPHA) sf = GL_ONE;
    else if (sf == GL_ONE_MINUS_DST_ALPHA || sf == GL_SRC_ALPHA_SATURATE) sf = GL_ZERO;
    if (df == GL_DST_ALPHA) df = GL_ONE;
    else if (df == GL_ONE_MINUS_DST_ALPHA) df = GL_ZERO;
    beq = c->blend_eq;
    if (beq != GL_FUNC_ADD) {
      if (beq == GL_MIN || beq == GL_MAX) sf = df = GL_ONE;   /* factors unused */
      else if (beq == GL_FUNC_SUBTRACT && df == GL_ZERO) beq = GL_FUNC_ADD;  /* S*sf - 0 */
      if (beq != GL_FUNC_ADD) gen = 1;
    }
    /* D * 1 +- S * 0 = D: nothing changes (FUNC_SUBTRACT: 0 - D does) */
    nocolor = sf == GL_ZERO && df == GL_ONE && beq != GL_FUNC_SUBTRACT;
  }
  cm = (c->color_mask[0] ? 0xf800 : 0) | (c->color_mask[1] ? 0x07e0 : 0) |
       (c->color_mask[2] ? 0x001f : 0);
  nocolor |= cm == 0;
  if (sf != GL_ONE || df != GL_ZERO || cm != 0xffff) gen = 1;
  p->sfactor = sf; p->dfactor = df; p->cmask = (unsigned short)cm;
  p->clamp_s = clamp_s; p->clamp_t = clamp_t;

  /* ---- alpha test */
  afunc = c->alpha_test_enabled ? c->alpha_func : GL_ALWAYS;
  if (afunc == GL_NEVER) skip = 1;
  else if (afunc != GL_ALWAYS) gen = 1;
  p->afunc = afunc;

  /* ---- fog */
  if (c->fog_enabled) {
    gen = 1;
    c->fog_scale = c->fog_end != c->fog_start ? 1.0f / (c->fog_end - c->fog_start) : 0.0f;
  }

  /* ---- depth: TinyGL's fillers exist for LEQUAL (>=, with and without
     the write), strict LESS with the write (ztriangle_lt.c) and no test;
     anything else is the general path */
  dsel = c->depth_test ? depth_sel(c->depth_func) : ZP_DEPTH_NONE;
  /* ---- phase 4 (p4_select): the stencil stage makes the general path;
     sw: it can write, so what fails depth or writes nothing still counts */
  x4 = c->p4_en ? p4_select(c) : 0;
  if (x4 & P4_R_STENCIL) gen = 1;
  sw = x4 & P4_R_STENCIL_W;
  if (dsel == ZP_DEPTH_NEVER) skip = !sw;
  else if (dsel != ZP_DEPTH_NONE && dsel != ZP_DEPTH_LESS && dsel != ZP_DEPTH_LEQUAL)
    gen = 1;
  else if (dsel == ZP_DEPTH_LESS && !c->depth_mask)
    gen = 1;
  if (dsel == ZP_DEPTH_NONE) {
    c->zb_flat = ZB_fillTriangleFlat_nt; c->zb_smooth = ZB_fillTriangleSmooth_nt;
    c->zb_smooth_pc = NULL;               /* phase 4: the general path */
    c->zb_smooth_long = ZB_fillTriangleSmoothLong_nt;
    c->zb_map = ZB_fillTriangleMappingPerspective_nt;
    c->zb_line = ZB_line; c->zb_plot = ZB_plot_nz;
  } else if (!c->depth_mask) {
    c->zb_flat = ZB_fillTriangleFlat_nw; c->zb_smooth = ZB_fillTriangleSmooth_nw;
    c->zb_smooth_pc = c->zb_smooth_long = NULL;   /* phase 4: the general path */
    c->zb_map = ZB_fillTriangleMappingPerspective_nw;
    c->zb_line = ZB_line_z; c->zb_plot = ZB_plot;   /* not used: general path */
  } else if (dsel == ZP_DEPTH_LESS) {
    /* phase 4 L1: s31_rt holds the RAM copies when S31GL_RAMTEXT made them */
    c->zb_flat = s31_rt.flat_lt; c->zb_smooth = s31_rt.smooth_lt;
    c->zb_smooth_pc = ZB_fillTriangleSmoothPersp_lt;
    c->zb_smooth_long = ZB_fillTriangleSmoothLong_lt;
    c->zb_map = s31_rt.map_lt;
    c->zb_line = ZB_line_z_lt; c->zb_plot = ZB_plot_lt;
  } else {
    c->zb_flat = s31_rt.flat; c->zb_smooth = s31_rt.smooth;
    c->zb_smooth_pc = ZB_fillTriangleSmoothPersp;
    c->zb_smooth_long = ZB_fillTriangleSmoothLong;
    c->zb_map = s31_rt.map;
    c->zb_line = ZB_line_z; c->zb_plot = ZB_plot;
  }
  /* nothing to write at all */
  if (nocolor && (dsel == ZP_DEPTH_NONE || !c->depth_mask) && !sw) skip = 1;

  /* ---- polygon stipple (plan F7): a stage of the general path */
  if (c->poly_stipple_enabled && !skip) gen = 1;

  {
    /* widths are in window pixels; a render-scaled buffer's are 2^rscale
       of them (GLContext.rscale) */
    float lw = c->line_width, ps = c->point_size;
    if (c->rscale) {
      float rf = 1.0f / (float)(1 << c->rscale);
      lw *= rf;
      ps *= rf;
    }
    c->line_w = lw < 1.5f ? 1 : (int)(lw + 0.5f);
    c->point_w = ps < 1.5f ? 1 : (int)(ps + 0.5f);
    /* phase 4 SMOOTH (p4_select): the footprint reaches w/2 + 1/2 past
       the centre; line_w / point_w, which size the dirty boxes, cover it */
    if (x4 & P4_R_AA_LINES) c->line_w = (int)c->aa_lw + 2;
    if (x4 & P4_R_AA_POINTS) c->point_w = (int)c->aa_ps + 2;
  }
  c->p4_raster = x4;
  c->raster_general = gen;
  c->raster_gen_lines = gen || tex || (c->depth_test && !c->depth_mask) || c->line_w > 1 ||
                       c->line_stipple_enabled;
  c->raster_gen_points = gen || tex || (c->depth_test && !c->depth_mask) || c->point_w > 1;
  c->raster_need_attr = gen || modwhite || c->raster_gen_lines || c->raster_gen_points;
  c->raster_fog = c->raster_need_attr && c->fog_enabled;
  /* glopVertex's one test for the per-vertex extras (s31_xform.c) */
  c->vtx_extra = (c->raster_fog ? 1 : 0) | (c->clip_plane_mask ? 2 : 0);
  c->raster_skip = skip;

  /* the stage list is built when a general-path primitive is first drawn
     (gl_build_pipe): a batch that only uses TinyGL's fillers never pays */
  p->dsel = dsel; p->nocolor = nocolor;
  c->pipe_dirty = 1;

  /* ---- GL_FILL triangles */
  if (skip) c->draw_fill_inner = gl_draw_triangle_skip;
  else if (gen) c->draw_fill_inner = gl_draw_triangle_general;
  else if (modwhite) c->draw_fill_inner = gl_draw_triangle_modwhite;
  /* phase 4 F-PERSP: smooth untextured tier 1 - one test per triangle
     for perspective-correct colour (s31_tfilter.c) */
  else if (!tex && c->current_shade_model == GL_SMOOTH && c->pc_enable)
    c->draw_fill_inner = s31_rt.draw_fill_pq;
  else c->draw_fill_inner = s31_rt.draw_fill;
  c->draw_fill = (c->offset_states & TGL_OFFSET_FILL) && !skip ?
                 gl_draw_triangle_offset : c->draw_fill_inner;

  /* phase 3a G03 (s31_zepoch.c): stale depth epochs under this depth
     state - GL_LESS gets the far-step check (the fillers, ztri_zepoch),
     GEQUAL / EQUAL / NOTEQUAL materialise now */
  zep_guard(c);
  zep_track_target(c);
}

/* the general path's stage list and constants for the state
   gl_update_raster saw (zpipe.h) */
void gl_build_pipe(GLContext *c)
{
  ZPipe *p = &c->pipe;
  ZPipeX *x = &c->pipex;
  GLTexture *t = (c->tex_enables & 1) ? c->current_texture : c->current_texture_1d;
  int i = 0, flat = c->current_shade_model != GL_SMOOTH, zw_late, stip, nocol;

  c->pipe_dirty = 0;
  c->pipe_serial++;             /* s31_draw.c rebuilds its copy */
  if (c->tex_active) {
    p->tex = t->images[0].pixmap;
    p->talpha = t->alpha;
    p->ws = t->ws; p->hs = t->hs; p->fbits = t->fbits;
    p->smask = c->zb->tex_smask; p->tmask = c->zb->tex_tmask;
    p->wmax = (1 << t->ws) - 1; p->hmax = (1 << t->hs) - 1;
    for (i = 0; i < 4; i++) p->envc[i] = f8(c->texenv_color.v[i]);
  }
  for (i = 0; i < 3; i++) p->fogc[i] = f8(c->fog_color.v[i]);
  /* the alpha test compares a/255 with the reference exactly (review G4):
     fragment alpha a is an 8-bit integer, so with r = ref * 255
       GREATER a > floor r, LEQUAL a <= floor r,
       LESS a < ceil r, GEQUAL a >= ceil r,
       EQUAL / NOTEQUAL against r only when r is a whole number (to within
       float rounding: 128/255 is 128); otherwise EQUAL never passes and
       NOTEQUAL always does (a reference of -1 no alpha equals).
     f8's rounding had TyrQuake's GREATER 0.666 (r = 169.8) reject a texel
     alpha of 170. */
  {
    float r = c->alpha_ref <= 0.0f ? 0.0f : (c->alpha_ref >= 1.0f ? 255.0f : c->alpha_ref * 255.0f);
    int ri = (int)(r + 0.5f), fl, ce;
    float d = r - (float)ri;
    if (d < 0.0f) d = -d;
    if (d < 1.0e-3f) {
      fl = ce = ri;
    } else {
      fl = (int)r;
      ce = fl + 1;
    }
    switch (p->afunc) {
    case GL_GREATER: case GL_LEQUAL: p->aref = fl; break;
    case GL_LESS: case GL_GEQUAL: p->aref = ce; break;
    default: p->aref = fl == ce ? fl : -1; break;   /* EQUAL, NOTEQUAL */
    }
  }

  /* the depth write goes after the alpha test (GL 4.1); without one it
     is fused into the test */
  /* polygon stipple kills fragments after the depth test too */
  stip = c->poly_stipple_enabled;
  zw_late = (p->afunc != GL_ALWAYS && p->afunc != GL_NEVER) || stip;
  i = 0;
  p->zb = c->zb;
  if (RASTER_STENCIL(c)) {
    /* phase 4 F8: the stencil stage runs the depth test and writes depth
       itself (zpipe.c zp_stencil_fn); first in the list when nothing can
       kill a fragment before it, else after the alpha test (below) */
    int dw = p->dsel != ZP_DEPTH_NONE && c->depth_mask;
    zs_build(c);
    x->st_zb = c->zb->zbuf;
    x->st_sb = c->zb->sbuf;
    x->st_zfn = p->dsel == ZP_DEPTH_NEVER ? zp_depth_never_m() : zp_depth_fn(p->dsel, 0);
    p->depth = zp_depth_fn(ZP_DEPTH_NONE, 0);
    if (!zw_late) p->st[i++] = zp_stencil_fn(0, RASTER_STENCIL_W(c), dw);
  } else {
    p->depth = zp_depth_fn(p->dsel, c->depth_mask && !zw_late);
  }
  if (stip) {
    p->stip = c->poly_stipple;
    p->zb = c->zb;
    p->st[i++] = zp_stipple_fn();
  }
  /* phase 4: the fragment colour is not computed when nothing reads it -
     an RGB texture under REPLACE (or DECAL) replaces it, and without an
     alpha test or an alpha blend factor its alpha is never read (it cost
     ~26 instructions a pixel on a REPLACE-textured general-path span) */
  {
    int te = c->tex_active ? texenv_op(c->texenv_mode, t->fmt) : -1;
    int sa = p->sfactor, da = p->dfactor;
    int alpha_read = (p->afunc != GL_ALWAYS && p->afunc != GL_NEVER) ||
                     sa == GL_SRC_ALPHA || sa == GL_ONE_MINUS_SRC_ALPHA ||
                     da == GL_SRC_ALPHA || da == GL_ONE_MINUS_SRC_ALPHA ||
                     RASTER_AA_LINES(c) || RASTER_AA_POINTS(c);   /* the coverage stage */
    nocol = te == ZP_TE_REPLACE_RGB && !alpha_read;
  }
  x->col_affine = zp_color_fn(flat);
  x->slot_col = -1;
  if (!nocol) {
    x->slot_col = i;
    p->st[i++] = x->col_affine;
  }
  x->slot_tex = -1;
  x->tex_base = NULL;
  x->tex0 = NULL; x->talpha0 = NULL;
  if (c->tex_active) {
    x->slot_tex = i;
    x->tex_base = zp_texidx_fn(p->clamp_s, p->clamp_t);
    x->tex0 = p->tex; x->talpha0 = p->talpha;
    p->st[i++] = x->tex_base;
    p->st[i++] = zp_texenv_fn(texenv_op(c->texenv_mode, t->fmt));
  }
  if (c->raster_sepspec) p->st[i++] = zp_spec_fn(flat);
  if (c->fog_enabled) p->st[i++] = zp_fog_fn();
  /* phase 4 SMOOTH: the coverage, after texturing and fog, before the
     alpha test (GL 1.3 3.11); a no-op for the batch's triangles */
  x->cov_kind = 0;
  if (RASTER_AA_LINES(c) || RASTER_AA_POINTS(c)) p->st[i++] = zp_cover_fn();
  if (p->afunc != GL_ALWAYS && p->afunc != GL_NEVER) p->st[i++] = zp_alpha_fn(p->afunc);
  if (RASTER_STENCIL(c)) {
    if (zw_late)
      p->st[i++] = zp_stencil_fn(1, RASTER_STENCIL_W(c),
                                 p->dsel != ZP_DEPTH_NONE && c->depth_mask);
  } else if (p->dsel != ZP_DEPTH_NONE && c->depth_mask && zw_late) {
    p->st[i++] = zp_zwrite_fn();
  }
  /* phase 4 BLEND-EQ: the equation (factors folded by gl_update_raster:
     S*sf - D*0 is S*sf) */
  x->beq = GL_FUNC_ADD;
  if (c->blend_enabled) {
    x->beq = c->blend_eq;
    if (x->beq == GL_FUNC_SUBTRACT && p->dfactor == GL_ZERO) x->beq = GL_FUNC_ADD;
  }
  if (!p->nocolor) p->st[i++] = zp_out_fn(p->sfactor, p->dfactor, p->cmask, x->beq);
  if (c->blend_enabled) {                  /* the GL_CONSTANT_* factors */
    int k;
    for (k = 0; k < 4; k++) x->bcol[k] = f8(c->blend_color[k]);
  }
  p->st[i] = NULL;
  p->need = (p->dsel != ZP_DEPTH_NONE ? ZP_N_Z : 0) |
            (flat || nocol ? 0 : ZP_N_RGBA) |
            (c->tex_active ? ZP_N_ST | ZP_N_Q : 0) |
            (c->fog_enabled ? ZP_N_F | ZP_N_Q : 0) |
            (c->raster_sepspec && !flat ? ZP_N_SPEC : 0);
  x->need0 = p->need;

  /* phase 4 (s31_tfilter.c): which choices the triangles make */
  p->xact = 0;
  if (!flat && !nocol && c->pc_enable) {
    p->xact |= ZPX_PC;
    x->pc_min = ZPX_PC_MIN;
    x->pc_cur = 0;
  }
  if (c->tex_active && c->tex_filtered) {
    int rep_all = !p->clamp_s && !p->clamp_t, al = t->alpha != NULL;
    x->rep_s = p->clamp_s ? 0 : -1;
    x->rep_t = p->clamp_t ? 0 : -1;
    x->f_near = zpx_stage(TF_NMN, rep_all, al);
    x->f_bil = zpx_stage(TF_LINEAR0, rep_all, al);
    x->f_tri = zpx_stage(TF_LML, rep_all, al);
    x->f_ntri = zpx_stage(TF_NML, rep_all, al);
    x->lod_cur = -1;             /* the slot holds tex_base, no code yet */
    if (x->kmag == x->kmin) {
      /* one filter for every triangle (LINEAR / LINEAR without mipmaps):
         placed once, no per-triangle choice */
      x->cur0 = &x->lvl[0];
      x->tex_base = x->f_bil;
      p->st[x->slot_tex] = x->f_bil;
      p->tex = x->ftex; p->talpha = x->falpha;
    } else {
      p->xact |= ZPX_TEX;
    }
  }
}

