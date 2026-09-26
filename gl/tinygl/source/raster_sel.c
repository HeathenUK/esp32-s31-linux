/*
 * raster_sel.c - gl_update_raster and gl_build_pipe: which rasteriser path
 * draws, decided at glBegin (see raster.c for the model). Cold code - it
 * runs once per state change - so it is built -Os (gl/api/build-lib.sh).
 * s31, MIT.
 */
#include "zgl.h"
#include "zpipe.h"
#include "raster_int.h"

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

/* ------------------------------------------------------------ selection */

void gl_update_raster(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZPipe *p = &c->pipe;
  /* s31 (plan F7): GL_TEXTURE_2D wins over GL_TEXTURE_1D (GL 1.3 3.8.15);
     a 1D texture is a W x 1 image, so the same paths draw it */
  GLTexture *t = (c->tex_enables & 1) ? c->current_texture : c->current_texture_1d;
  int gen = 0, tex, clamp_s = 0, clamp_t = 0, tex_tier1 = 0, modwhite = 0;
  int sf, df, cm, nocolor, afunc, skip = 0, dsel;

  c->raster_dirty = 0;
  zb->pipe = p;

  /* ---- texture (GL 1.3 3.8.10: an incomplete texture disables it) */
  tex = c->texture_2d_enabled && t != NULL && gl_texture_complete(t);
  c->tex_active = tex;
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
    if (!tex_tier1 && !modwhite) gen = 1;
  }

  /* ---- GL_SEPARATE_SPECULAR_COLOR: only differs from the single colour
     when there is a texture to put the specular part on */
  c->raster_sepspec = c->color_control == GL_SEPARATE_SPECULAR_COLOR &&
                      c->lighting_enabled && tex;
  p->st_spec = c->raster_sepspec;
  if (c->raster_sepspec) gen = 1;

  /* ---- blending: no alpha plane, so the destination alpha is 1 */
  sf = GL_ONE; df = GL_ZERO;
  if (c->blend_enabled) {
    sf = c->blend_src; df = c->blend_dst;
    if (sf == GL_DST_ALPHA) sf = GL_ONE;
    else if (sf == GL_ONE_MINUS_DST_ALPHA || sf == GL_SRC_ALPHA_SATURATE) sf = GL_ZERO;
    if (df == GL_DST_ALPHA) df = GL_ONE;
    else if (df == GL_ONE_MINUS_DST_ALPHA) df = GL_ZERO;
  }
  cm = (c->color_mask[0] ? 0xf800 : 0) | (c->color_mask[1] ? 0x07e0 : 0) |
       (c->color_mask[2] ? 0x001f : 0);
  nocolor = cm == 0 || (sf == GL_ZERO && df == GL_ONE);
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
  if (dsel == ZP_DEPTH_NEVER) skip = 1;
  else if (dsel != ZP_DEPTH_NONE && dsel != ZP_DEPTH_LESS && dsel != ZP_DEPTH_LEQUAL)
    gen = 1;
  else if (dsel == ZP_DEPTH_LESS && !c->depth_mask)
    gen = 1;
  if (dsel == ZP_DEPTH_NONE) {
    c->zb_flat = ZB_fillTriangleFlat_nt; c->zb_smooth = ZB_fillTriangleSmooth_nt;
    c->zb_map = ZB_fillTriangleMappingPerspective_nt;
    c->zb_line = ZB_line; c->zb_plot = ZB_plot_nz;
  } else if (!c->depth_mask) {
    c->zb_flat = ZB_fillTriangleFlat_nw; c->zb_smooth = ZB_fillTriangleSmooth_nw;
    c->zb_map = ZB_fillTriangleMappingPerspective_nw;
    c->zb_line = ZB_line_z; c->zb_plot = ZB_plot;   /* not used: general path */
  } else if (dsel == ZP_DEPTH_LESS) {
    c->zb_flat = ZB_fillTriangleFlat_lt; c->zb_smooth = ZB_fillTriangleSmooth_lt;
    c->zb_map = ZB_fillTriangleMappingPerspective_lt;
    c->zb_line = ZB_line_z_lt; c->zb_plot = ZB_plot_lt;
  } else {
    c->zb_flat = ZB_fillTriangleFlat; c->zb_smooth = ZB_fillTriangleSmooth;
    c->zb_map = ZB_fillTriangleMappingPerspective;
    c->zb_line = ZB_line_z; c->zb_plot = ZB_plot;
  }
  /* nothing to write at all */
  if (nocolor && (dsel == ZP_DEPTH_NONE || !c->depth_mask)) skip = 1;

  /* ---- polygon stipple (plan F7): a stage of the general path */
  if (c->poly_stipple_enabled && !skip) gen = 1;

  c->line_w = c->line_width < 1.5f ? 1 : (int)(c->line_width + 0.5f);
  c->point_w = c->point_size < 1.5f ? 1 : (int)(c->point_size + 0.5f);
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
  else c->draw_fill_inner = gl_draw_triangle_fill;
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
  GLTexture *t = (c->tex_enables & 1) ? c->current_texture : c->current_texture_1d;
  int i = 0, flat = c->current_shade_model != GL_SMOOTH, zw_late, stip;

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
  p->depth = zp_depth_fn(p->dsel, c->depth_mask && !zw_late);
  i = 0;
  if (stip) {
    p->stip = c->poly_stipple;
    p->zb = c->zb;
    p->st[i++] = zp_stipple_fn();
  }
  p->st[i++] = zp_color_fn(flat);
  if (c->tex_active) {
    p->st[i++] = zp_texidx_fn(p->clamp_s, p->clamp_t);
    p->st[i++] = zp_texenv_fn(texenv_op(c->texenv_mode, t->fmt));
  }
  if (c->raster_sepspec) p->st[i++] = zp_spec_fn(flat);
  if (c->fog_enabled) p->st[i++] = zp_fog_fn();
  if (p->afunc != GL_ALWAYS && p->afunc != GL_NEVER) p->st[i++] = zp_alpha_fn(p->afunc);
  if (p->dsel != ZP_DEPTH_NONE && c->depth_mask && zw_late) p->st[i++] = zp_zwrite_fn();
  if (!p->nocolor) p->st[i++] = zp_out_fn(p->sfactor, p->dfactor, p->cmask);
  p->st[i] = NULL;
  p->need = (p->dsel != ZP_DEPTH_NONE ? ZP_N_Z : 0) |
            (flat ? 0 : ZP_N_RGBA) |
            (c->tex_active ? ZP_N_ST | ZP_N_Q : 0) |
            (c->fog_enabled ? ZP_N_F | ZP_N_Q : 0) |
            (c->raster_sepspec && !flat ? ZP_N_SPEC : 0);
}

