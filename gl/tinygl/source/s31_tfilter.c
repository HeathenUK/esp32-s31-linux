/*
 * s31_tfilter.c - phase 4: texture filtering (F-LIN) and perspective-
 * correct Gouraud colour (F-PERSP) for the general path. s31, MIT.
 *
 * Both are choices made per TRIANGLE (the mipmap level: per 8-pixel block
 * of a triangle that recedes in depth), never per pixel (plan 2.2,
 * "specialise, do not branch"). gl_build_pipe places the batch's stages
 * (zpipe.h) and records where the texel and colour stages sit
 * (ZPipeX.slot_tex / slot_col); for a triangle that needs a choice the
 * general filler calls zpx_tri, which writes the chosen stage into that
 * slot, the level(s) it reads into ZPipeX.cur, and the span attributes it
 * needs into ZPipe.need - or, for the level per block, hands the spans to
 * zp_run_lod, which does the same per chunk. A frame that uses neither
 * feature never reaches this file: its batches have xact = 0, the fillers
 * test that one word per triangle, and tier 1 is untouched.
 *
 * TEXTURE FILTERS (GL 1.3 3.8.7-3.8.8)
 *  - lambda from the GL scale factor rho = max(|d(u,v)/dx|, |d(u,v)/dy|)
 *    in level-0 texels, from the triangle's s/w, t/w and 1/w planes (so
 *    it is perspective-correct). A triangle whose 1/w differ by more than
 *    ~3% (zpx_qspread_lod) - a floor or wall receding in depth - takes it
 *    per 8-pixel block, at the block's centre (zp_run_lod: rho^2 =
 *    max(Kx, Ky(x)) / q^4 with a constant and a stepped numerator, one
 *    divide a block); any other triangle once, at its centroid, where
 *    lambda varies by less than 0.09 across it. Mesa computes it per 2x2
 *    pixel quad. (Review 4 R1: one lambda per triangle drew a one-quad
 *    floor 13-16% off Mesa - blurred near, sparkling far, a seam along the
 *    diagonal - against 0.01-0.34% per block; gl/tests/glx_floor.c.)
 *    The NEAREST levels come exactly from the float's bits; the LINEAR
 *    blends' frac(lambda) from a quadratic log2 (|error| < 0.005). The
 *    minification switch is Mesa's lambda > 0 (GL 1.3's c = 0.5 for LINEAR
 *    with NEAREST_MIPMAP_* is not used: review 4 R3). GL_TEXTURE_MIN_LOD /
 *    MAX_LOD clamp lambda; GL_TEXTURE_MAX_LEVEL ends the chain.
 *  - GL_LINEAR: bilinear, the four texels around (u - 1/2, v - 1/2),
 *    5-bit weights, done on RGB565 spread into one 32-bit word
 *    (0000 0ggg ggg0 0000 rrrr r000 000b bbbb: each field has the 5 spare
 *    bits a 5-bit weight needs), rounded, so one lerp is two multiplies
 *    for all three channels; the A8 plane is lerped alongside when the
 *    format has alpha. The texel it makes goes to the texenv stages as a
 *    texel (ZPipeX.ftex / falpha, idx = i): every texenv of zpipe.c
 *    serves filtered and nearest texels alike.
 *  - *_MIPMAP_NEAREST: level d = ceil(lambda + 1/2) - 1 (GL's), nearest or
 *    bilinear in it. LINEAR_MIPMAP_LINEAR: trilinear - bilinear in levels
 *    floor(lambda) and +1, blended by frac(lambda) (5 bits, per 8-pixel
 *    block: ZPipeX.twb); S31GL_TRILINEAR=0 draws it as
 *    LINEAR_MIPMAP_NEAREST (half the texel reads). NEAREST_MIPMAP_LINEAR:
 *    nearest in the same two levels, blended the same way (review 4 R2;
 *    it was nearest in the nearer level).
 *  - GL_CLAMP under a linear filter is drawn as GL_CLAMP_TO_EDGE (the
 *    border colour's half-texel fringe at the edge is not drawn;
 *    "approximated", once). The wraps are a stage variant: all-REPEAT
 *    masks, anything else masks-and-clamps per axis.
 *  - Lines and points take the magnification filter at level 0; the
 *    pixel paths (glBitmap / glDrawPixels texel) stay nearest at level 0.
 *
 * PERSPECTIVE COLOUR (GL 3.5.1 eq. 3.6's perspective-correct varying)
 *  - A smooth triangle's colour is interpolated linearly in screen space
 *    unless screen-affine interpolation could be off by more than four
 *    8-bit levels (one RGB565 green step) somewhere in it: that error is
 *    at most about
 *    crange * (wmax - wmin) / (2 (wmax + wmin)) for the triangle's largest
 *    vertex colour difference crange - so only a triangle whose w values
 *    differ enough AND whose colours differ is changed. That triangle is
 *    drawn with r/w, g/w, b/w (a/w) planes divided by 1/w every 8 pixels
 *    (the texture walk's subdivision): a divide per 8
 *    pixels, on those triangles only. On tier 1 (smooth untextured
 *    batches) gl_draw_triangle_fill_pq makes the test and sends such a
 *    triangle to ZB_fillTriangleSmoothPersp (ztriangle.c: TinyGL's smooth
 *    filler with the colour divided every 8 pixels, in each depth
 *    variant); every other triangle keeps ZB_fillTriangleSmooth. On the
 *    general path the colour stage becomes zc_smooth_pc for the triangle.
 *    S31GL_PERSPCOLOR=0 turns it off.
 *
 * No double anywhere (F without D).
 */
#include <stdlib.h>
#include "zgl.h"
#include "zpipe.h"
#include "ztri.h"

static inline int clamp255(int v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline int clampi(int v, int hi)
{
  return v < 0 ? 0 : (v > hi ? hi : v);
}

/* ------------------------------------------------------------ texel stages */

#include "s31_tfilt_int.h"   /* SPREAD, LERP, BILERP ... (shared with zpipe_fused.c) */

/* Each stage below is made for a unit: U its index (ZPipeX.tf[U]), S its
   suffix on the span's and the chunk's fields (idx / idx1). Unit 0's are
   exactly phase 4's stages. */

/* nearest in cur0 (NEAREST_MIPMAP_*): an index into that level, which
   zpx_tri made ZPipe.tex (tex1) */
#define ZX_NEAR(name, U, S, WR)                                         \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  const ZPipeX *x = p->x;                                               \
  const ZTexF *u = &x->tf[U];                                           \
  const ZLevel *L0 = u->cur0;                                           \
  LEVEL_VARS(L0)                                                        \
  TWALK(S, f->idx##S[i] = NEAR_IDX(L0, WR);)                            \
}
ZX_NEAR(zx_near_r, 0, , WR_R)
ZX_NEAR(zx_near_g, 0, , WR_G)
ZX_NEAR(zx1_near_r, 1, 1, WR_R)
ZX_NEAR(zx1_near_g, 1, 1, WR_G)

/* bilinear in cur0 */
#define ZX_BIL(name, U, S, WR, ALPHA)                                   \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0;                                           \
  PIXEL *ft = u->ftex;                                                  \
  unsigned char *fa = u->falpha;                                        \
  LEVEL_VARS(L0)                                                        \
  (void)fa;                                                             \
  TWALK(S,                                                              \
    unsigned int c; unsigned int a = 0;                                 \
    BILERP(L0, WR, c, a, ALPHA)                                         \
    ft[i] = UNSPREAD(c);                                                \
    if (ALPHA) fa[i] = (unsigned char)((a + 512u) >> 10);               \
    f->idx##S[i] = (unsigned int)i;                                     \
  )                                                                     \
}
ZX_BIL(zx_bil_r, 0, , WR_R, 0)
ZX_BIL(zx_bil_g, 0, , WR_G, 0)
ZX_BIL(zx_bil_ra, 0, , WR_R, 1)
ZX_BIL(zx_bil_ga, 0, , WR_G, 1)
ZX_BIL(zx1_bil_r, 1, 1, WR_R, 0)
ZX_BIL(zx1_bil_g, 1, 1, WR_G, 0)
ZX_BIL(zx1_bil_ra, 1, 1, WR_R, 1)
ZX_BIL(zx1_bil_ga, 1, 1, WR_G, 1)

/* trilinear: bilinear in cur0 and cur1, blended by twb[block]/32 (the
   weight is per 8-pixel block: zp_run_lod) */
#define ZX_TRI(name, U, S, WR, ALPHA)                                   \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0, *L1 = u->cur1;                            \
  unsigned int tw;                                                      \
  PIXEL *ft = u->ftex;                                                  \
  unsigned char *fa = u->falpha;                                        \
  LEVEL_VARS(L0)                                                        \
  LEVEL_VARS(L1)                                                        \
  (void)fa;                                                             \
  TWALKB(S, tw = ZPX_TW5(u->twb[i >> 3]);,                              \
    unsigned int c0; unsigned int c1; unsigned int a0 = 0; unsigned int a1 = 0; \
    BILERPX(L0, WR, c0, a0, ALPHA, LERP)                                \
    BILERPX(L1, WR, c1, a1, ALPHA, LERP)                                \
    c0 = LERPT(c0, c1, tw);                                             \
    ft[i] = UNSPREAD(c0);                                               \
    if (ALPHA) fa[i] = (unsigned char)((a0 * (32u - tw) + a1 * tw + 16384u) >> 15); \
    f->idx##S[i] = (unsigned int)i;                                     \
  )                                                                     \
}
ZX_TRI(zx_tri_r, 0, , WR_R, 0)
ZX_TRI(zx_tri_g, 0, , WR_G, 0)
ZX_TRI(zx_tri_ra, 0, , WR_R, 1)
ZX_TRI(zx_tri_ga, 0, , WR_G, 1)
ZX_TRI(zx1_tri_r, 1, 1, WR_R, 0)
ZX_TRI(zx1_tri_g, 1, 1, WR_G, 0)
ZX_TRI(zx1_tri_ra, 1, 1, WR_R, 1)
ZX_TRI(zx1_tri_ga, 1, 1, WR_G, 1)

/* NEAREST_MIPMAP_LINEAR (review 4 R2): nearest in cur0 and cur1, blended
   by tw/32 as trilinear blends its two bilinear samples */
#define ZX_NTRI(name, U, S, WR, ALPHA)                                  \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0, *L1 = u->cur1;                            \
  unsigned int tw;                                                      \
  PIXEL *ft = u->ftex;                                                  \
  unsigned char *fa = u->falpha;                                        \
  LEVEL_VARS(L0)                                                        \
  LEVEL_VARS(L1)                                                        \
  (void)fa;                                                             \
  TWALKB(S, tw = ZPX_TW5(u->twb[i >> 3]);,                              \
    unsigned int i0 = NEAR_IDX(L0, WR);                                 \
    unsigned int i1 = NEAR_IDX(L1, WR);                                 \
    unsigned int c0 = SPREAD((unsigned int)px_L0[i0]);                  \
    unsigned int c1 = SPREAD((unsigned int)px_L1[i1]);                  \
    ft[i] = UNSPREAD(LERPT(c0, c1, tw));                                \
    if (ALPHA) fa[i] = (unsigned char)(((unsigned int)pa_L0[i0] * (32u - tw) + \
                                        (unsigned int)pa_L1[i1] * tw + 16u) >> 5); \
    f->idx##S[i] = (unsigned int)i;                                     \
  )                                                                     \
}
ZX_NTRI(zx_ntri_r, 0, , WR_R, 0)
ZX_NTRI(zx_ntri_g, 0, , WR_G, 0)
ZX_NTRI(zx_ntri_ra, 0, , WR_R, 1)
ZX_NTRI(zx_ntri_ga, 0, , WR_G, 1)
ZX_NTRI(zx1_ntri_r, 1, 1, WR_R, 0)
ZX_NTRI(zx1_ntri_g, 1, 1, WR_G, 0)
ZX_NTRI(zx1_ntri_ra, 1, 1, WR_R, 1)
ZX_NTRI(zx1_ntri_ga, 1, 1, WR_G, 1)

/* [unit][kind][repeat][alpha] */
static const ZStageFn zx_tab[2][4][2][2] = {
  { { { zx_near_g, zx_near_g }, { zx_near_r, zx_near_r } },
    { { zx_ntri_g, zx_ntri_ga }, { zx_ntri_r, zx_ntri_ra } },
    { { zx_tri_g, zx_tri_ga }, { zx_tri_r, zx_tri_ra } },
    { { zx_bil_g, zx_bil_ga }, { zx_bil_r, zx_bil_ra } } },
  { { { zx1_near_g, zx1_near_g }, { zx1_near_r, zx1_near_r } },
    { { zx1_ntri_g, zx1_ntri_ga }, { zx1_ntri_r, zx1_ntri_ra } },
    { { zx1_tri_g, zx1_tri_ga }, { zx1_tri_r, zx1_tri_ra } },
    { { zx1_bil_g, zx1_bil_ga }, { zx1_bil_r, zx1_bil_ra } } },
};

ZStageFn zpx_stage_u(int unit, int kind, int repeat, int alpha)
{
  int k;
  switch (kind) {
  case TF_NMN: k = 0; break;
  case TF_NML: k = 1; break;                   /* the blend of two nearest */
  case TF_LML: k = 2; break;
  default: k = 3; break;                       /* bilinear */
  }
  return zx_tab[unit != 0][k][repeat != 0][alpha != 0];
}

ZStageFn zpx_stage(int kind, int repeat, int alpha)
{
  return zpx_stage_u(0, kind, repeat, alpha);
}

/* ------------------------------------------------------------ 8-bit texels */

/* Phase 5: the texel stages of a unit whose texture holds 8-bit texels
   (P8, L8, W32: s31_tex8.c). Each writes the fragments' texels into
   ZTexF.ftex32 as RGBA8 words, idx[i] = i, for the texenv stages
   (zp_texenv8_fn) and the combiner - nearest in level 0 as well (the
   base stage, in place of the 565 path's texel index stage). One stage a
   filter and unit, every wrap (WR_G): the texel read is t8_fetch's, one
   switch a texel on the level's kind, and the filters are Mesa's 8-bit
   ones (s31_tfilt_int.h BILERP8, LERP8W). The fused fillers
   (zpipe_fused.c) replace these for the signatures they know; these are
   what they are gated against. */
#define ZX8_BASE(name, U, S)                                            \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = &u->lvl[0];                                        \
  unsigned int *ft = u->ftex32;                                         \
  LEVEL_VARS(L0)                                                        \
  TWALK(S, ft[i] = t8_fetch(L0, NEAR_IDX(L0, WR_G));                    \
           f->idx##S[i] = (unsigned int)i;)                             \
}
#define ZX8_NEAR(name, U, S)                                            \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0;                                           \
  unsigned int *ft = u->ftex32;                                         \
  LEVEL_VARS(L0)                                                        \
  TWALK(S, ft[i] = t8_fetch(L0, NEAR_IDX(L0, WR_G));                    \
           f->idx##S[i] = (unsigned int)i;)                             \
}
#define ZX8_BIL(name, U, S)                                             \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0;                                           \
  unsigned int *ft = u->ftex32;                                         \
  LEVEL_VARS(L0)                                                        \
  TWALK(S, unsigned int c; BILERP8(L0, WR_G, c) ft[i] = c;              \
           f->idx##S[i] = (unsigned int)i;)                             \
}
/* trilinear: the two levels' bilinear words, blended by the block's
   weight twb (0..32) as 8 twb in 8 bits */
#define ZX8_TRI(name, U, S)                                             \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0, *L1 = u->cur1;                            \
  unsigned int tw, *ft = u->ftex32;                                     \
  LEVEL_VARS(L0)                                                        \
  LEVEL_VARS(L1)                                                        \
  TWALKB(S, tw = ZPX_TW8(u->twb[i >> 3]);,                              \
    unsigned int c0; unsigned int c1;                                   \
    BILERP8(L0, WR_G, c0)                                               \
    BILERP8(L1, WR_G, c1)                                               \
    ft[i] = LERP8W(c0, c1, tw);                                         \
    f->idx##S[i] = (unsigned int)i;                                     \
  )                                                                     \
}
#define ZX8_NTRI(name, U, S)                                            \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  ZTexF *u = &x->tf[U];                                                 \
  const ZLevel *L0 = u->cur0, *L1 = u->cur1;                            \
  unsigned int tw, *ft = u->ftex32;                                     \
  LEVEL_VARS(L0)                                                        \
  LEVEL_VARS(L1)                                                        \
  TWALKB(S, tw = ZPX_TW8(u->twb[i >> 3]);,                              \
    unsigned int c0 = t8_fetch(L0, NEAR_IDX(L0, WR_G));                 \
    unsigned int c1 = t8_fetch(L1, NEAR_IDX(L1, WR_G));                 \
    ft[i] = LERP8W(c0, c1, tw);                                         \
    f->idx##S[i] = (unsigned int)i;                                     \
  )                                                                     \
}
ZX8_BASE(zx8_base, 0, )
ZX8_BASE(zx8_1_base, 1, 1)
/* unit 0's level 0 when it is P8 (every non-reference P8 texture): the
   palette word without t8_fetch's switch */
static void zx8_base_p8(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  ZPipeX *x = p->x;
  ZTexF *u = &x->tf[0];
  const ZLevel *L0 = &u->lvl[0];
  const unsigned char *i8 = L0->i8;
  const unsigned int *pal = L0->pal;
  unsigned int *ft = u->ftex32;
  LEVEL_VARS(L0)
  TWALK(, ft[i] = pal[i8[NEAR_IDX(L0, WR_G)]];
          f->idx[i] = (unsigned int)i;)
}
ZX8_NEAR(zx8_near, 0, )
ZX8_NEAR(zx8_1_near, 1, 1)
ZX8_BIL(zx8_bil, 0, )
ZX8_BIL(zx8_1_bil, 1, 1)
ZX8_TRI(zx8_tri, 0, )
ZX8_TRI(zx8_1_tri, 1, 1)
ZX8_NTRI(zx8_ntri, 0, )
ZX8_NTRI(zx8_1_ntri, 1, 1)

/* [unit][base, NMN, NML, LML, bilinear] */
static const ZStageFn zx8_tab[2][5] = {
  { zx8_base, zx8_near, zx8_ntri, zx8_tri, zx8_bil },
  { zx8_1_base, zx8_1_near, zx8_1_ntri, zx8_1_tri, zx8_1_bil },
};

ZStageFn zpx_stage8_u(int unit, int kind)
{
  int k;
  switch (kind) {
  case TF_NEAREST0: k = 0; break;
  case TF_NMN: k = 1; break;
  case TF_NML: k = 2; break;
  case TF_LML: k = 3; break;
  default: k = 4; break;                       /* bilinear */
  }
  return zx8_tab[unit != 0][k];
}

/* the nearest-in-level-0 stage for a unit whose level 0 is L (set up by
   zpx_level0) */
ZStageFn zpx_base8(const ZLevel *L, int unit)
{
  return unit == 0 && L->k8 == TGL_ST_P8 ? zx8_base_p8 : zx8_tab[unit != 0][0];
}

/* whether f is one of zpx_base8's stages for the unit */
int zpx_is_base8(ZStageFn f, int unit)
{
  return f == zx8_tab[unit != 0][0] || (unit == 0 && f == zx8_base_p8);
}

/* a stored level of t as the texel stages read it: its planes */
void zpx_level_ptrs(const GLTexture *t, int l, ZLevel *L)
{
  const void *pix;
  const unsigned char *al;
  const GLTexPal *pal;
  if (l == 0) {
    pix = t->images[0].pixmap; al = t->alpha; pal = t->pal0;
  } else {
    const GLMipLevel *m = &t->mip->l[l];
    pix = m->pix; al = m->alpha; pal = m->pal;
  }
  L->alpha = al;
  L->am = t->amode;
  L->pix = NULL; L->i8 = NULL; L->pal = NULL; L->w32 = NULL;
  switch (t->st) {
  case TGL_ST_P8: L->i8 = pix; L->pal = pal->w; L->k8 = TGL_ST_P8; break;
  case TGL_ST_L8: L->i8 = pix; L->k8 = TGL_ST_L8; break;
  case TGL_ST_W32: L->w32 = pix; L->k8 = TGL_ST_W32; break;
  default: L->pix = pix; L->k8 = 0; break;
  }
}

unsigned int zpx_t8_texel(const ZLevel *L, unsigned int k)
{
  return t8_fetch(L, k);
}

void zpx_level0(const GLTexture *t, ZLevel *L)
{
  int F = t->fbits;
  zpx_level_ptrs(t, 0, L);
  L->ws = t->ws;
  L->wm = (1 << t->ws) - 1;
  L->hm = (1 << t->hs) - 1;
  L->shs = F;
  L->sht = F + t->ws;
}

int zpx_is_tex_stage(ZStageFn f)
{
  const ZStageFn *t = &zx_tab[0][0][0][0];
  int i;
  for (i = 0; i < (int)(sizeof zx_tab / sizeof zx_tab[0][0][0][0]); i++)
    if (t[i] == f) return 1;
  for (i = 0; i < 10; i++)
    if ((&zx8_tab[0][0])[i] == f) return 1;
  return f == zx8_base_p8;
}

/* ------------------------------------------------------------ perspective colour */

/* r/w ... a/w over 1/w, divided every 8 pixels and stepped linearly
   between along the chord (exact at every 8th pixel; ztriangle.c's
   ZB_fillTriangleSmoothPersp does the same); clamped as zc_smooth clamps */
static void zc_smooth_pc(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  float fz = s->fz, rq = s->rq, gq = s->gq, bq = s->bq, aq = s->aq;
  const float dfz = s->dfzdx, drq = s->drqdx, dgq = s->dgqdx, dbq = s->dbqdx,
              daq = s->daqdx;
  float zinv = 1.0f / fz;
  float r = rq * zinv, g = gq * zinv, b = bq * zinv, a = aq * zinv;
  int i = 0, e, n = f->n;
  (void)p;
  while (i < n) {
    int m = n - i < 8 ? n - i : 8;
    float e1 = 1.0f / (fz + (float)m * dfz), k = 1.0f / (float)m;
    float r1 = (rq + (float)m * drq) * e1, g1 = (gq + (float)m * dgq) * e1;
    float b1 = (bq + (float)m * dbq) * e1, a1 = (aq + (float)m * daq) * e1;
    int ri = (int)r, gi = (int)g, bi = (int)b, ai = (int)a;
    int dri = (int)((r1 - r) * k), dgi = (int)((g1 - g) * k);
    int dbi = (int)((b1 - b) * k), dai = (int)((a1 - a) * k);
    e = i + m;
    for (; i < e; i++) {
      f->r[i] = (unsigned char)clamp255(ri >> ZP_CSHIFT);
      f->g[i] = (unsigned char)clamp255(gi >> ZP_CSHIFT);
      f->b[i] = (unsigned char)clamp255(bi >> ZP_CSHIFT);
      f->a[i] = (unsigned char)clamp255(ai >> ZP_CSHIFT);
      /* (wrapping, as the target's adds do: a colour far out of range at
         a vertex near the eye saturates the conversion above) */
      ri = (int)((unsigned int)ri + (unsigned int)dri);
      gi = (int)((unsigned int)gi + (unsigned int)dgi);
      bi = (int)((unsigned int)bi + (unsigned int)dbi);
      ai = (int)((unsigned int)ai + (unsigned int)dai);
    }
    fz += (float)m * dfz; rq += (float)m * drq; gq += (float)m * dgq;
    bq += (float)m * dbq; aq += (float)m * daq;
    r = r1; g = g1; b = b1; a = a1;
  }
}

ZStageFn zpx_color_pc(void)
{
  return zc_smooth_pc;
}

/* ------------------------------------------------------------ per triangle */

/* log2 of a positive normal float: exponent + a quadratic in the mantissa
   (|error| < 0.005). Phase 5 P: Mesa llvmpipe's own, exponent + mantissa
   - 1 (lp_build_fast_log2: exact at the powers of two, linear between),
   on rho^2 - the trilinear blend of gl/tests/glx_prec.c band 32 is then
   Mesa's for every lambda (256 of 256 values; with the quadratic, 44) */
static inline float zpx_log2(float v)
{
  union { float f; unsigned int u; } b;
  float e, m;
  b.f = v;
  e = (float)((int)((b.u >> 23) & 255) - 127);
  b.u = (b.u & 0x007fffffu) | 0x3f800000u;
  m = b.f;
#ifdef S31GL_P4ARITH
  return e + fmaf(fmaf(-0.34484843f, m, 2.02466578f), m, -1.67487759f);
#else
  return e + (m - 1.0f);
#endif
}

/* The level choice, as one code: what the texel slot holds (ZC_*) in
   bits 16 and up, the (first) level in bits 8-15, and for ZC_TRI / ZC_NTRI
   the weight of the second level (0..32, of 32) in bits 0-7. Codes whose
   bits 8 and up agree place the same stage and levels; the weight is per
   8-pixel block (ZTexF.twb), so zp_run_lod keeps blocks whose weights
   differ in one chunk. GL 1.3 3.8.8, the switch to minification at lambda
   > 0 (review 4 R3: Mesa's; GL 1.3 has c = 0.5 for LINEAR with
   NEAREST_MIPMAP_*, zpx_prepare sets lod_c). */
/* the trilinear weight of frac(lambda) f, the low byte of a code
   (ZTexF.twb). Phase 5 P: Mesa's, floor(256 f) in 0..255 - the 8-bit
   stages lerp by it, the 565 ones by ZPX_TW5 of it, (w + 4) >> 3 in 0..32.
   -DS31GL_P4ARITH: phase 4's, round(32 f), a 32 being level d + 1 */
#ifdef S31GL_P4ARITH
static inline int zpx_tw_q(float f, int *d)
{
  int w = ztri_floor(f * 32.0f + 0.5f);
  if (w < 0) w = 0;
  if (w > 32) w = 32;
  if (w == 32) { (*d)++; w = 0; }
  return w;
}
#define ZPX_TW_QUANT(f) zpx_tw_q((f), &d)
#else
static inline int zpx_tw_q(float f)
{
  int w = ztri_floor(f * 256.0f);
  return w < 0 ? 0 : (w > 255 ? 255 : w);
}
#define ZPX_TW_QUANT(f) zpx_tw_q(f)
#endif

enum { ZC_BASE,        /* nearest, level 0: the batch's own texidx stage */
       ZC_NEAR,        /* nearest in level d (> 0) */
       ZC_BIL,         /* bilinear in level d */
       ZC_TRI,         /* bilinear in d and d + 1, blended */
       ZC_NTRI };      /* nearest in d and d + 1, blended (NEAREST_MIPMAP_LINEAR) */
#define ZCODE(k, d, w) ((k) << 16 | (d) << 8 | (w))

/* the code for lambda lam (GL_TEXTURE_MIN_LOD / MAX_LOD applied here) */
static int zpx_code(const ZTexF *x, float lam)
{
  int n = x->nlev, d, w, k;
  lam = fminf(fmaxf(lam, x->lod_min), x->lod_max);
  k = lam <= x->lod_c ? x->kmag : x->kmin;
  switch (k) {
  case TF_NEAREST0:
    return ZCODE(ZC_BASE, 0, 0);
  case TF_LINEAR0:
    return ZCODE(ZC_BIL, 0, 0);
  case TF_NMN:
  case TF_LMN:
    /* GL's d = ceil(lambda + 1/2) - 1 */
    d = lam <= 0.5f ? 0 : clampi(ztri_ceil(lam + 0.5f) - 1, n);
    if (k == TF_LMN) return ZCODE(ZC_BIL, d, 0);
    return d ? ZCODE(ZC_NEAR, d, 0) : ZCODE(ZC_BASE, 0, 0);
  default:                                     /* TF_LML, TF_NML */
    d = ztri_floor(lam);
    if (d < 0) d = 0;
    w = 0;
    if (d >= n) d = n;
    else w = ZPX_TW_QUANT(lam - (float)d);
    if (k == TF_LML) return w ? ZCODE(ZC_TRI, d, w) : ZCODE(ZC_BIL, d, 0);
    if (w) return ZCODE(ZC_NTRI, d, w);
    return d ? ZCODE(ZC_NEAR, d, 0) : ZCODE(ZC_BASE, 0, 0);
  }
}

/* place code's stage and level(s) in unit `unit`'s texel slot (the
   trilinear weight is the caller's: ZTexF.twb) */
__attribute__((noinline))
static void zpx_apply(ZPipe *p, ZTexF *x, int unit, int code)
{
  int d = (code >> 8) & 255;
  const PIXEL *tx;
  const unsigned char *ta;
  x->lod_cur = code & ~255;
  switch (code >> 16) {
  case ZC_BASE:
    *x->slot = x->tex_base;
    tx = x->tex0; ta = x->talpha0;
    break;
  case ZC_NEAR:
    x->cur0 = &x->lvl[d];
    *x->slot = x->f_near;
    tx = x->cur0->pix; ta = x->cur0->alpha;
    break;
  case ZC_BIL:
    x->cur0 = &x->lvl[d];
    *x->slot = x->f_bil;
    tx = x->ftex; ta = x->falpha;
    break;
  default:                                     /* ZC_TRI, ZC_NTRI */
    x->cur0 = &x->lvl[d]; x->cur1 = &x->lvl[d + 1];
    *x->slot = (code >> 16) == ZC_TRI ? x->f_tri : x->f_ntri;
    tx = x->ftex; ta = x->falpha;
    break;
  }
  if (unit) {
    p->tex1 = tx; p->talpha1 = ta;
  } else {
    p->tex = tx; p->talpha = ta;
  }
}

/* the code for rho^2 (level-0 texels): lambda = log2(rho^2) / 2. The
   NEAREST levels are exact from the float's bits - GL's d = ceil(lambda +
   1/2) - 1 is ceil(log2(rho^2)) >> 1, and lambda > 0 is rho^2 > 1 - so the
   common filters take no log2; the LINEAR blends need frac(lambda) and take
   a quadratic log2 (|error| < 0.005). MIN_LOD / MAX_LOD other than GL's
   defaults (lod_gen) go through zpx_code. */
static inline __attribute__((always_inline)) int zpx_code2(const ZTexF *x, float rho2)
{
  union { float f; unsigned int u; } b;
  int k, d;
  if (x->lod_gen) {
    float lam;
    if (!(rho2 > 1.0e-30f)) lam = -64.0f;      /* also NaN: magnified */
    else if (!(rho2 < 1.0e30f)) lam = 64.0f;
    else lam = 0.5f * zpx_log2(rho2);
    return zpx_code(x, lam);
  }
  if (!(rho2 > 1.0f))                          /* lambda <= 0 (or NaN): magnified */
    return x->kmag == TF_LINEAR0 ? ZCODE(ZC_BIL, 0, 0) : ZCODE(ZC_BASE, 0, 0);
  k = x->kmin;
  if (k == TF_NEAREST0) return ZCODE(ZC_BASE, 0, 0);
  if (k == TF_LINEAR0) return ZCODE(ZC_BIL, 0, 0);
  if (!(rho2 < 1.0e30f)) rho2 = 1.0e30f;
  if (k == TF_NMN || k == TF_LMN) {
    b.f = rho2;
    d = ((int)((b.u + 0x7fffffu) >> 23) - 127) >> 1;
    if (d > x->nlev) d = x->nlev;
    if (k == TF_LMN) return ZCODE(ZC_BIL, d, 0);
    return d ? ZCODE(ZC_NEAR, d, 0) : ZCODE(ZC_BASE, 0, 0);
  }
  {
    /* (phase 5 O2) TF_LML / TF_NML: zpx_code's own steps inline, which
       saves the call and its float spills once per 8-pixel block. With
       GL's default MIN_LOD / MAX_LOD (lod_gen 0) its clamp is the
       identity - lambda is finite here, |lambda| < 50 - and the rest is
       its arithmetic, so the codes are the same */
    float lam = 0.5f * zpx_log2(rho2);
    int n = x->nlev, w;
    if (lam <= x->lod_c)
      return x->kmag == TF_LINEAR0 ? ZCODE(ZC_BIL, 0, 0) : ZCODE(ZC_BASE, 0, 0);
    d = ztri_floor(lam);
    if (d < 0) d = 0;
    w = 0;
    if (d >= n) d = n;
    else w = ZPX_TW_QUANT(lam - (float)d);
    if (k == TF_LML) return w ? ZCODE(ZC_TRI, d, w) : ZCODE(ZC_BIL, d, 0);
    if (w) return ZCODE(ZC_NTRI, d, w);
    return d ? ZCODE(ZC_NEAR, d, 0) : ZCODE(ZC_BASE, 0, 0);
  }
}

/* (out of line for zpx_tri, once per triangle: zp_run_lod inlines it) */
__attribute__((noinline))
static int zpx_code2_call(const ZTexF *x, float rho2)
{
  return zpx_code2(x, rho2);
}

/* one unit's level for a triangle whose 1/w are (nearly) constant: lambda
   at its centroid, where each plane is the mean of its vertices (the same
   as Mesa's per 2x2 quad on an affine triangle). s*, t*: the unit's int
   coordinates at the sorted vertices */
static void zpx_tri_unit(ZPipe *p, ZTexF *x, int unit, const ZTri *T, float q0,
                         float q1, float q2, int sa, int sb, int sc, int ta,
                         int tb, int tc)
{
  float s0 = (float)sa * q0, s1 = (float)sb * q1, s2 = (float)sc * q2;
  float t0 = (float)ta * q0, t1 = (float)tb * q1, t2 = (float)tc * q2;
  float gsx, gsy, gtx, gty, gqx, gqy, r, u, v, iq, ux, uy, vx, vy, rx, ry;
  int code;
  ZTRI_GRAD(T, s0, s1, s2, gsx, gsy);
  ZTRI_GRAD(T, t0, t1, t2, gtx, gty);
  ZTRI_GRAD(T, q0, q1, q2, gqx, gqy);
  r = 1.0f / (q0 + q1 + q2);
  u = (s0 + s1 + s2) * r;
  v = (t0 + t1 + t2) * r;
  iq = 3.0f * r;
  ux = (gsx - u * gqx) * iq; uy = (gsy - u * gqy) * iq;
  vx = (gtx - v * gqx) * iq; vy = (gty - v * gqy) * iq;
  rx = ux * ux * x->lod_ks + vx * vx * x->lod_kt;
  ry = uy * uy * x->lod_ks + vy * vy * x->lod_kt;
  code = zpx_code2_call(x, rx > ry ? rx : ry);
  if ((code & ~255) != x->lod_cur) zpx_apply(p, x, unit, code);
  x->twb[0] = x->twb[1] = x->twb[2] = x->twb[3] = (unsigned char)code;
}

int zpx_tri(ZPipe *p, const ZTri *T, const ZVtxG *v0, const ZVtxG *v1,
            const ZVtxG *v2)
{
  ZPipeX *x = p->x;
  int lodb = 0;

  if (p->xact & (ZPX_TEX | ZPX_TEX1)) {
    float q0 = v0->q, q1 = v1->q, q2 = v2->q;
    if (zpx_qspread_lod(q0, q1, q2)) {
      /* (review 4 R1) its 1/w differ by more than ~3%: lambda varies
         across it (a floor or wall receding in depth), so each 8-pixel
         block picks its own level (zp_run_lod) - one lambda for the whole
         triangle drew the near end too coarse, the far end too fine and a
         seam along a quad's diagonal (15.6% against Mesa on one quad) */
      lodb = 1;
    } else {
      /* 1/w (nearly) constant: lambda is too, one per triangle */
      if (p->xact & ZPX_TEX)
        zpx_tri_unit(p, &x->tf[0], 0, T, q0, q1, q2, v0->si, v1->si, v2->si,
                     v0->ti, v1->ti, v2->ti);
      if (p->xact & ZPX_TEX1)
        zpx_tri_unit(p, &x->tf[1], 1, T, q0, q1, q2, v0->si1, v1->si1, v2->si1,
                     v0->ti1, v1->ti1, v2->ti1);
    }
  }
  if (p->xact & ZPX_PC) {
    float qa = v0->q, qb = v1->q, qc = v2->q;
    float mx = fmaxf(fmaxf(qa, qb), qc), mn = fminf(fminf(qa, qb), qc);
    int pc = 0;
    if ((mx - mn) * ZPX_PC_PRE > mx + mn) {
      /* the largest vertex colour difference, 8.16 */
      float cr = fmaxf(fmaxf(v0->r, v1->r), v2->r) - fminf(fminf(v0->r, v1->r), v2->r);
      float cg = fmaxf(fmaxf(v0->g, v1->g), v2->g) - fminf(fminf(v0->g, v1->g), v2->g);
      float cb = fmaxf(fmaxf(v0->b, v1->b), v2->b) - fminf(fminf(v0->b, v1->b), v2->b);
      float ca = fmaxf(fmaxf(v0->a, v1->a), v2->a) - fminf(fminf(v0->a, v1->a), v2->a);
      float cm = fmaxf(fmaxf(cr, cg), fmaxf(cb, ca));
      pc = cm * (mx - mn) > ZPX_PC_MIN * (mx + mn);
    }
    x->pc_cur = pc;
    if (pc) {
      *x->slot_col = zc_smooth_pc;
      p->need = (x->need0 & ~ZP_N_RGBA) | ZP_N_PC | ZP_N_Q;
    } else {
      *x->slot_col = x->col_affine;
      p->need = x->need0;
    }
  }
  return lodb;
}

/* phase 4 (review 4 R1): zp_run for a triangle whose level is chosen per
   8-pixel block. rho^2 at a block's centre, without a divide per term:
   along a span the x derivatives of u = (s/w)/(1/w) and v are
   (d(s/w)/dx q - (s/w) dq/dx) / q^2, whose numerators are constant along
   the span (the terms in x cancel), and the y derivatives' numerators are
   linear in x; so per block rho^2 = max(Kx, ks Ny^2 + kt My^2) / q^4 with
   q, Ny, My stepped - one divide a block. A chunk is the run of blocks with
   the same stage and levels, at most ZP_CHUNK pixels; each block's
   trilinear weight goes to ZTexF.twb, and the texel slot is re-placed only
   when a chunk's stage or levels differ from what it holds. The per-pixel
   stages are the ones zp_run runs. Phase 5: the same for texture unit 1
   (ZPX_TEX1), each unit its own code; a block where either unit's differs
   starts the next chunk. */
/* one unit's per-span constants of the block lambda */
typedef struct {
  float kx, ny0, dny, my0, dmy, ks, kt;
} ZLodSpan;

static inline __attribute__((always_inline))
void zpx_lod_span(ZLodSpan *l, const ZTexF *u, float q0, float dq, float sz,
                  float tz, float dsx, float dtx, float dsy, float dty,
                  float dqy)
{
  const float cx = dsx * q0 - sz * dq, dx = dtx * q0 - tz * dq;
  l->ks = u->lod_ks; l->kt = u->lod_kt;
  l->kx = l->ks * cx * cx + l->kt * dx * dx;
  l->ny0 = dsy * q0 - sz * dqy;
  l->dny = dsy * dq - dsx * dqy;
  l->my0 = dty * q0 - tz * dqy;
  l->dmy = dty * dq - dtx * dqy;
}

static inline __attribute__((always_inline))
int zpx_lod_blk(const ZLodSpan *l, const ZTexF *u, float q0, float dq, float o)
{
  float q_ = q0 + o * dq, ny_ = l->ny0 + o * l->dny, my_ = l->my0 + o * l->dmy;
  float ky_ = l->ks * ny_ * ny_ + l->kt * my_ * my_;
  q_ *= q_;
  return zpx_code2(u, (l->kx > ky_ ? l->kx : ky_) / (q_ * q_));
}

static inline __attribute__((always_inline))
void zp_run_lod_t(ZBuffer *zb, ZSpan *s, const int two, const int direct)
{
  ZPipe *p = (ZPipe *)zb->pipe;
  ZPipeX *x = p->x;
  ZTexF *u0 = &x->tf[0], *u1 = &x->tf[1];
  const ZStageFn *st;
  ZFrag f;
  int n, m, nb, code, next = 0, have = 0, code1 = 0, next1 = 0;
  /* (direct: a fused filler's batch - unit 1 has no per-block choice) */
  const int t0 = !two || (p->xact & ZPX_TEX), t1 = two && !direct && (p->xact & ZPX_TEX1);

  for (;;) {
    /* this span position's planes (x steps: d*dx; y: d*dy) */
    const float q0 = s->fz, dq = s->dfzdx;
    const int sn = s->n;
    /* (zeroed: GCC cannot see that t0 / t1 guard every use; for unit 0
       alone the stores are dead and go) */
    ZLodSpan l0 = { 0 }, l1 = { 0 };
    if (t0)
      zpx_lod_span(&l0, u0, q0, dq, s->sz, s->tz, s->dszdx, s->dtzdx,
                   s->dszdy, s->dtzdy, s->dfzdy);
    if (t1)
      zpx_lod_span(&l1, u1, q0, dq, s->sz1, s->tz1, s->dszdx1, s->dtzdx1,
                   s->dszdy1, s->dtzdy1, s->dfzdy);
    /* the blocks of this chunk; a block whose levels differ starts the
       next chunk */
    n = nb = 0;
    code = code1 = 0;
    do {
      int c = 0, c1 = 0;
      m = sn - n < 8 ? sn - n : 8;
      if (have) {
        c = next; c1 = next1;
      } else {
        float o = (float)n + (float)(m - 1) * 0.5f;
        if (t0) c = zpx_lod_blk(&l0, u0, q0, dq, o);
        if (t1) c1 = zpx_lod_blk(&l1, u1, q0, dq, o);
      }
      have = 0;
      /* (the weight bits of a code are not read: the blocks' are in twb) */
      if (nb && (((c ^ code) | (c1 ^ code1)) & ~255)) {
        next = c; next1 = c1; have = 1;
        break;
      }
      code = c; code1 = c1;
      if (t0) u0->twb[nb] = (unsigned char)c;
      if (t1) u1->twb[nb] = (unsigned char)c1;
      nb++;
      n += m;
    } while (n < sn && n < ZP_CHUNK);
    if (t0 && (code & ~255) != u0->lod_cur) zpx_apply(p, u0, 0, code);
    if (t1 && (code1 & ~255) != u1->lod_cur) zpx_apply(p, u1, 1, code1);
    f.n = n;
#ifndef S31GL_CENSUS
    if (direct)
      x->chunk(p, s, &f);
    else if (p->depth(s, &f))
      for (st = p->st; *st; st++)
        (*st)(p, s, &f);
#else
    if (direct) {
      zp_census_chunk(p, n, n);
      x->chunk(p, s, &f);
    } else {
      int al_ = p->depth(s, &f);
      zp_census_chunk(p, n, al_);
      if (al_)
        for (st = p->st; *st; st++)
          (*st)(p, s, &f);
    }
#endif
    s->n -= n;
    if (s->n <= 0)
      return;
    s->pp += n;
    s->pz += n;
    /* what a textured span's stages read (ZPipe.need) */
    if (p->need & ZP_N_Z) s->z += (unsigned int)(n * s->dzdx);
    if (p->need & ZP_N_RGBA) {
      s->r += n * s->drdx; s->g += n * s->dgdx; s->b += n * s->dbdx;
      s->a += n * s->dadx;
    }
    if (p->need & ZP_N_SPEC) {
      s->sr += n * s->dsrdx; s->sg += n * s->dsgdx; s->sb += n * s->dsbdx;
    }
    if (!two || (p->need & ZP_N_ST)) {
      s->sz += (float)n * s->dszdx; s->tz += (float)n * s->dtzdx;
    }
    if (two && (p->need & ZP_N_ST1)) {
      s->sz1 += (float)n * s->dszdx1; s->tz1 += (float)n * s->dtzdx1;
    }
    if (p->need & ZP_N_F) s->fq += (float)n * s->dfqdx;
    s->fz += (float)n * s->dfzdx;
    if (p->need & ZP_N_PC) {
      s->rq += (float)n * s->drqdx; s->gq += (float)n * s->dgqdx;
      s->bq += (float)n * s->dbqdx; s->aq += (float)n * s->daqdx;
    }
  }
}

void zp_run_lod(ZBuffer *zb, ZSpan *s)
{
  zp_run_lod_t(zb, s, 0, 0);
}

/* phase 5 O1: a batch with texture unit 1 on */
void zp_run_lod_mt(ZBuffer *zb, ZSpan *s)
{
  zp_run_lod_t(zb, s, 1, 0);
}

/* phase 5 O2 */
void zp_run_lod_mt_direct(ZBuffer *zb, ZSpan *s)
{
  zp_run_lod_t(zb, s, 1, 1);
}

void zpx_reset(ZPipe *p)
{
  ZPipeX *x = p->x;
  int k;
  for (k = 0; k < 2; k++) {
    ZTexF *u = &x->tf[k];
    if (p->xact & (k ? ZPX_TEX1 : ZPX_TEX)) {
      int code = zpx_code(u, -64.0f);
      zpx_apply(p, u, k, code);
      u->twb[0] = u->twb[1] = u->twb[2] = u->twb[3] = (unsigned char)code;
    }
  }
  if (p->xact & ZPX_PC) {
    *x->slot_col = x->col_affine;
    p->need = x->need0;
    x->pc_cur = 0;
  }
}

/* ------------------------------------------------------------ tier 1 */

/* GL_SMOOTH untextured batches on tier 1 (raster_sel.c installs this in
   place of gl_draw_triangle_fill, whose smooth untextured branch it
   replaces). A triangle stays on TinyGL's smooth filler unless one of two
   things makes that filler's colour visibly wrong:

   - perspective: its w values and colours differ enough for screen-affine
     colour to be off by E levels (ZPX_PC_MIN, zpipe.h; zpx_tri's test in
     the fillers' colour scale: one level is (MAX - MIN) / 255 ~ 249 zp
     units, 256 is used);
   - long spans: TinyGL's filler steps its packed colour with the
     gradient truncated to 1/32 of a 5-bit step for red and green's 6-bit
     step, 1/16 for blue, so the error grows along a span - 2 steps of blue
     (16/255) 32 pixels from the span's start. SDL testgl's cube (glOrtho,
     w = 1 everywhere) shows it as creases along the quads' diagonals,
     where one triangle's spans restart: 1.99% of frame 60 against Mesa.
     A triangle ZPX_LONG_W (64) or more pixels wide whose colour varies
     is drawn re-anchored every 8 pixels from its planes instead
     (ZB_fillTriangleSmoothLong when its w are equal: the integer planes,
     no divide; otherwise the perspective filler), exact to 8 steps of the
     truncated gradient, half a blue step - which is what makes testgl
     match (0.000%). 64, not 16 or 32: at 16 the smooth cylinders of gears,
     glxgears and teapot 640x400 qualified (their frame hashes changed,
     gears 640 +1.6%), at 32 gears 640x400 still did; at 64 no bench frame
     changes.

   Perspective goes to ZB_fillTriangleSmoothPersp (ztriangle.c: TinyGL's
   smooth filler with the colour re-derived from r/w, g/w, b/w every 8
   pixels), long spans at equal w to ZB_fillTriangleSmoothLong, each in the
   batch's depth variant.

   The common case must cost next to nothing (teapot: ~730 smooth
   triangles a frame, a 0.5% budget of ~17 instructions each), so the
   first tests are one shift each: the bit patterns of the three q = 1/w
   (zpx_qspread, zpipe.h: passes every triangle whose q ratio could
   qualify) and the width of the snapped x. The rest is out of line, and
   the common case calls the smooth filler itself (TinyGL's
   gl_draw_triangle_fill spent ~9 instructions finding it). */
__attribute__((noinline))
static void fill_pq_slow(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
  float qa = p0->zp.q, qb = p1->zp.q, qc = p2->zp.q;
  float mx = fmaxf(fmaxf(qa, qb), qc), mn = fminf(fminf(qa, qb), qc);
  int x0 = p0->zp.x, x1 = p1->zp.x, x2 = p2->zp.x;
  int w = (x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) -
          (x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
  int qpre = (mx - mn) * ZPX_PC_PRE > mx + mn, cm;
  ZB_fillTriangleFunc f;
  /* the bit test is coarse (a q ratio of 1.03 passes it): most triangles
     that get here leave now, before the colour ranges */
  if (!qpre && w < ZPX_LONG_W) {
    c->zb_smooth(c->zb, &p0->zp, &p1->zp, &p2->zp);
    return;
  }
  {
    int r0 = p0->zp.r, r1 = p1->zp.r, r2 = p2->zp.r;
    int g0 = p0->zp.g, g1 = p1->zp.g, g2 = p2->zp.g;
    int b0 = p0->zp.b, b1 = p1->zp.b, b2 = p2->zp.b;
    int cr = (r0 > r1 ? (r0 > r2 ? r0 : r2) : (r1 > r2 ? r1 : r2)) -
             (r0 < r1 ? (r0 < r2 ? r0 : r2) : (r1 < r2 ? r1 : r2));
    int cg = (g0 > g1 ? (g0 > g2 ? g0 : g2) : (g1 > g2 ? g1 : g2)) -
             (g0 < g1 ? (g0 < g2 ? g0 : g2) : (g1 < g2 ? g1 : g2));
    int cb = (b0 > b1 ? (b0 > b2 ? b0 : b2) : (b1 > b2 ? b1 : b2)) -
             (b0 < b1 ? (b0 < b2 ? b0 : b2) : (b1 < b2 ? b1 : b2));
    cm = cr > cg ? cr : cg;
    if (cb > cm) cm = cb;
  }
  if (qpre && (float)cm * 256.0f * (mx - mn) > ZPX_PC_MIN * (mx + mn))
    f = c->zb_smooth_pc;
  else if (w >= ZPX_LONG_W && cm >= ZPX_LONG_C)
    /* w (nearly) equal: re-anchored from the integer planes, no divide */
    f = mx != mn ? c->zb_smooth_pc : c->zb_smooth_long;
  else {
    c->zb_smooth(c->zb, &p0->zp, &p1->zp, &p2->zp);
    return;
  }
  /* the depth states without such a filler (no depth test: perspective;
     depth mask off: both) take the general path, whose colour stages are
     exact per pixel (and perspective-corrected by zpx_tri) */
  if (f) f(c->zb, &p0->zp, &p1->zp, &p2->zp);
  else gl_draw_triangle_general(c, p0, p1, p2);
}

void gl_draw_triangle_fill_pq(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
  int x0 = p0->zp.x, x1 = p1->zp.x, x2 = p2->zp.x;
  int mx = x0 > x1 ? x0 : x1, mn = x0 < x1 ? x0 : x1;
  mx = x2 > mx ? x2 : mx;
  mn = x2 < mn ? x2 : mn;
  if (__builtin_expect(zpx_qspread(p0->zp.q, p1->zp.q, p2->zp.q) |
                       ((unsigned int)(mx - mn) >= ZPX_LONG_W), 0))
    fill_pq_slow(c, p0, p1, p2);
  else
    c->zb_smooth(c->zb, &p0->zp, &p1->zp, &p2->zp);
}
