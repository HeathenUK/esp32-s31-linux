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

/* RGB565 <-> the spread word; each field keeps 5 spare bits above it */
#define SPREAD(t) (((t) | ((t) << 16)) & 0x07E0F81Fu)
#define SMASK 0x07E0F81Fu
#define SRND 0x02008010u                /* 16 in each field: round the >> 5 */
#define LERP(a, b, w) ((((a) * (32u - (w)) + (b) * (w) + SRND) >> 5) & SMASK)
#define UNSPREAD(v) ((PIXEL)(((v) | ((v) >> 16)) & 0xffffu))
#define ALERP(a, b, w) ((a) * (32u - (w)) + (b) * (w))

/* the perspective walk of zpipe.c's ZP_TEXIDX: s/w, t/w divided by 1/w
   every 8 pixels, s and t stepped linearly in between; BODY sees si, ti
   (the texture's fixed point) and i */
#define TWALKB(BLOCK, ...)                                              \
  float fz = s->fz, sz = s->sz, tz = s->tz;                             \
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;           \
  int i = 0, e, n = f->n;                                               \
  while (i < n) {                                                       \
    float zinv = 1.0f / fz, ss = sz * zinv, tt = tz * zinv;             \
    BLOCK                                                               \
    /* unsigned steps: a clamped axis keeps its huge coordinates, and    \
       signed overflow there would be undefined (UBSan, filt_test fuzz) */\
    unsigned int si = (unsigned int)(int)ss, ti = (unsigned int)(int)tt; \
    unsigned int dsi = (unsigned int)(int)((dsz - ss * dfz) * zinv);    \
    unsigned int dti = (unsigned int)(int)((dtz - tt * dfz) * zinv);    \
    e = i + 8 < n ? i + 8 : n;                                          \
    for (; i < e; i++) {                                                \
      __VA_ARGS__                                                       \
      si += dsi; ti += dti;                                             \
    }                                                                   \
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;               \
  }
#define TWALK(...) TWALKB(;, __VA_ARGS__)

/* column and row wraps. R: GL_REPEAT (a mask). G: any wrap - the mask is
   the level's (REPEAT) or all ones (CLAMP*), then a clamp to the level */
#define WR_R(v, m, am) ((v) & (m))
#define WR_G(v, m, am) clampi((v) & (am), (m))

/* per-stage level constants; am*: the general wraps' masks */
#define LEVEL_VARS(L)                                                   \
  const PIXEL *px_##L = L->pix;                                         \
  const unsigned char *pa_##L = L->alpha;                               \
  const int shs_##L = L->shs, sht_##L = L->sht, ws_##L = L->ws;         \
  const int wm_##L = L->wm, hm_##L = L->hm;                             \
  const int ams_##L = L->wm | ~x->rep_s, amt_##L = L->hm | ~x->rep_t;   \
  (void)px_##L; (void)pa_##L; (void)ams_##L; (void)amt_##L;

/* nearest in level L */
#define NEAR_IDX(L, WR)                                                 \
  (((unsigned int)WR((int)ti >> sht_##L, hm_##L, amt_##L) << ws_##L) |  \
   (unsigned int)WR((int)si >> shs_##L, wm_##L, ams_##L))

/* bilinear in level L: the spread colour into C, the alpha (x 1024) into A.
   The weights are the fraction rounded to 1/32 (0..32: a weight of 32 is
   the second texel alone, which the spread fields hold: 31 x 32 fits their
   5 spare bits); truncated they were biased by up to 1/32 texel, which
   filt_test saw as 1.5 steps of red on steep texel edges */
#define BILERP(L, WR, C, A, ALPHA)                                      \
  {                                                                     \
    unsigned int u_ = si - (1u << (shs_##L - 1));                       \
    unsigned int v_ = ti - (1u << (sht_##L - 1));                       \
    int c0_ = (int)u_ >> shs_##L, r0_ = (int)v_ >> sht_##L;             \
    int c1_ = WR(c0_ + 1, wm_##L, ams_##L), r1_ = WR(r0_ + 1, hm_##L, amt_##L); \
    unsigned int fu_ = (((u_ << (32 - shs_##L)) >> 26) + 1) >> 1;       \
    unsigned int fv_ = (((v_ << (32 - sht_##L)) >> 26) + 1) >> 1;       \
    int o0_, o1_;                                                       \
    unsigned int a_, b_, c_, d_;                                        \
    c0_ = WR(c0_, wm_##L, ams_##L); r0_ = WR(r0_, hm_##L, amt_##L);     \
    o0_ = r0_ << ws_##L; o1_ = r1_ << ws_##L;                           \
    a_ = SPREAD((unsigned int)px_##L[o0_ + c0_]);                       \
    b_ = SPREAD((unsigned int)px_##L[o0_ + c1_]);                       \
    c_ = SPREAD((unsigned int)px_##L[o1_ + c0_]);                       \
    d_ = SPREAD((unsigned int)px_##L[o1_ + c1_]);                       \
    a_ = LERP(a_, b_, fu_);                                             \
    c_ = LERP(c_, d_, fu_);                                             \
    C = LERP(a_, c_, fv_);                                              \
    if (ALPHA) {                                                        \
      unsigned int t_ = ALERP((unsigned int)pa_##L[o0_ + c0_], (unsigned int)pa_##L[o0_ + c1_], fu_); \
      unsigned int w_ = ALERP((unsigned int)pa_##L[o1_ + c0_], (unsigned int)pa_##L[o1_ + c1_], fu_); \
      A = t_ * (32u - fv_) + w_ * fv_;                                  \
    }                                                                   \
  }

/* nearest in cur0 (NEAREST_MIPMAP_*): an index into that level, which
   zpx_tri made ZPipe.tex */
#define ZX_NEAR(name, WR)                                               \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  const ZPipeX *x = p->x;                                               \
  const ZLevel *L0 = x->cur0;                                           \
  LEVEL_VARS(L0)                                                        \
  TWALK(f->idx[i] = NEAR_IDX(L0, WR);)                                  \
}
ZX_NEAR(zx_near_r, WR_R)
ZX_NEAR(zx_near_g, WR_G)

/* bilinear in cur0 */
#define ZX_BIL(name, WR, ALPHA)                                         \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  const ZLevel *L0 = x->cur0;                                           \
  PIXEL *ft = x->ftex;                                                  \
  unsigned char *fa = x->falpha;                                        \
  LEVEL_VARS(L0)                                                        \
  (void)fa;                                                             \
  TWALK(                                                                \
    unsigned int c; unsigned int a = 0;                                 \
    BILERP(L0, WR, c, a, ALPHA)                                         \
    ft[i] = UNSPREAD(c);                                                \
    if (ALPHA) fa[i] = (unsigned char)((a + 512u) >> 10);               \
    f->idx[i] = (unsigned int)i;                                        \
  )                                                                     \
}
ZX_BIL(zx_bil_r, WR_R, 0)
ZX_BIL(zx_bil_g, WR_G, 0)
ZX_BIL(zx_bil_ra, WR_R, 1)
ZX_BIL(zx_bil_ga, WR_G, 1)

/* trilinear: bilinear in cur0 and cur1, blended by twb[block]/32 (the
   weight is per 8-pixel block: zp_run_lod) */
#define ZX_TRI(name, WR, ALPHA)                                         \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  const ZLevel *L0 = x->cur0, *L1 = x->cur1;                            \
  unsigned int tw;                                                      \
  PIXEL *ft = x->ftex;                                                  \
  unsigned char *fa = x->falpha;                                        \
  LEVEL_VARS(L0)                                                        \
  LEVEL_VARS(L1)                                                        \
  (void)fa;                                                             \
  TWALKB(tw = x->twb[i >> 3];,                                          \
    unsigned int c0; unsigned int c1; unsigned int a0 = 0; unsigned int a1 = 0; \
    BILERP(L0, WR, c0, a0, ALPHA)                                       \
    BILERP(L1, WR, c1, a1, ALPHA)                                       \
    c0 = LERP(c0, c1, tw);                                              \
    ft[i] = UNSPREAD(c0);                                               \
    if (ALPHA) fa[i] = (unsigned char)((a0 * (32u - tw) + a1 * tw + 16384u) >> 15); \
    f->idx[i] = (unsigned int)i;                                        \
  )                                                                     \
}
ZX_TRI(zx_tri_r, WR_R, 0)
ZX_TRI(zx_tri_g, WR_G, 0)
ZX_TRI(zx_tri_ra, WR_R, 1)
ZX_TRI(zx_tri_ga, WR_G, 1)

/* NEAREST_MIPMAP_LINEAR (review 4 R2): nearest in cur0 and cur1, blended
   by tw/32 as trilinear blends its two bilinear samples */
#define ZX_NTRI(name, WR, ALPHA)                                        \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  ZPipeX *x = p->x;                                                     \
  const ZLevel *L0 = x->cur0, *L1 = x->cur1;                            \
  unsigned int tw;                                                      \
  PIXEL *ft = x->ftex;                                                  \
  unsigned char *fa = x->falpha;                                        \
  LEVEL_VARS(L0)                                                        \
  LEVEL_VARS(L1)                                                        \
  (void)fa;                                                             \
  TWALKB(tw = x->twb[i >> 3];,                                          \
    unsigned int i0 = NEAR_IDX(L0, WR);                                 \
    unsigned int i1 = NEAR_IDX(L1, WR);                                 \
    unsigned int c0 = SPREAD((unsigned int)px_L0[i0]);                  \
    unsigned int c1 = SPREAD((unsigned int)px_L1[i1]);                  \
    ft[i] = UNSPREAD(LERP(c0, c1, tw));                                 \
    if (ALPHA) fa[i] = (unsigned char)(((unsigned int)pa_L0[i0] * (32u - tw) + \
                                        (unsigned int)pa_L1[i1] * tw + 16u) >> 5); \
    f->idx[i] = (unsigned int)i;                                        \
  )                                                                     \
}
ZX_NTRI(zx_ntri_r, WR_R, 0)
ZX_NTRI(zx_ntri_g, WR_G, 0)
ZX_NTRI(zx_ntri_ra, WR_R, 1)
ZX_NTRI(zx_ntri_ga, WR_G, 1)

ZStageFn zpx_stage(int kind, int repeat, int alpha)
{
  switch (kind) {
  case TF_NMN:
    return repeat ? zx_near_r : zx_near_g;
  case TF_NML:                                 /* the blend of two nearest */
    if (alpha) return repeat ? zx_ntri_ra : zx_ntri_ga;
    return repeat ? zx_ntri_r : zx_ntri_g;
  case TF_LML:
    if (alpha) return repeat ? zx_tri_ra : zx_tri_ga;
    return repeat ? zx_tri_r : zx_tri_g;
  default:                                     /* bilinear */
    if (alpha) return repeat ? zx_bil_ra : zx_bil_ga;
    return repeat ? zx_bil_r : zx_bil_g;
  }
}

int zpx_is_tex_stage(ZStageFn f)
{
  return f == zx_near_r || f == zx_near_g || f == zx_bil_r || f == zx_bil_g ||
         f == zx_bil_ra || f == zx_bil_ga || f == zx_tri_r || f == zx_tri_g ||
         f == zx_tri_ra || f == zx_tri_ga || f == zx_ntri_r || f == zx_ntri_g ||
         f == zx_ntri_ra || f == zx_ntri_ga;
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
      ri += dri; gi += dgi; bi += dbi; ai += dai;
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
   (|error| < 0.005) */
static inline float zpx_log2(float v)
{
  union { float f; unsigned int u; } b;
  float e, m;
  b.f = v;
  e = (float)((int)((b.u >> 23) & 255) - 127);
  b.u = (b.u & 0x007fffffu) | 0x3f800000u;
  m = b.f;
  return e + fmaf(fmaf(-0.34484843f, m, 2.02466578f), m, -1.67487759f);
}

/* The level choice, as one code: what the texel slot holds (ZC_*) in
   bits 16 and up, the (first) level in bits 8-15, and for ZC_TRI / ZC_NTRI
   the weight of the second level (0..32, of 32) in bits 0-7. Codes whose
   bits 8 and up agree place the same stage and levels; the weight is per
   8-pixel block (ZPipeX.twb), so zp_run_lod keeps blocks whose weights
   differ in one chunk. GL 1.3 3.8.8, the switch to minification at lambda
   > 0 (review 4 R3: Mesa's; GL 1.3 has c = 0.5 for LINEAR with
   NEAREST_MIPMAP_*, zpx_prepare sets lod_c). */
enum { ZC_BASE,        /* nearest, level 0: the batch's own texidx stage */
       ZC_NEAR,        /* nearest in level d (> 0) */
       ZC_BIL,         /* bilinear in level d */
       ZC_TRI,         /* bilinear in d and d + 1, blended */
       ZC_NTRI };      /* nearest in d and d + 1, blended (NEAREST_MIPMAP_LINEAR) */
#define ZCODE(k, d, w) ((k) << 16 | (d) << 8 | (w))

/* the code for lambda lam (GL_TEXTURE_MIN_LOD / MAX_LOD applied here) */
static int zpx_code(const ZPipeX *x, float lam)
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
    else {
      w = ztri_floor((lam - (float)d) * 32.0f + 0.5f);
      if (w < 0) w = 0;
      if (w > 32) w = 32;
    }
    if (w == 32) { d++; w = 0; }
    if (k == TF_LML) return w ? ZCODE(ZC_TRI, d, w) : ZCODE(ZC_BIL, d, 0);
    if (w) return ZCODE(ZC_NTRI, d, w);
    return d ? ZCODE(ZC_NEAR, d, 0) : ZCODE(ZC_BASE, 0, 0);
  }
}

/* place code's stage and level(s) in the texel slot (the trilinear weight
   is the caller's: ZPipeX.twb) */
__attribute__((noinline))
static void zpx_apply(ZPipe *p, ZPipeX *x, int code)
{
  int d = (code >> 8) & 255;
  x->lod_cur = code & ~255;
  switch (code >> 16) {
  case ZC_BASE:
    p->st[x->slot_tex] = x->tex_base;
    p->tex = x->tex0; p->talpha = x->talpha0;
    return;
  case ZC_NEAR:
    x->cur0 = &x->lvl[d];
    p->st[x->slot_tex] = x->f_near;
    p->tex = x->cur0->pix; p->talpha = x->cur0->alpha;
    return;
  case ZC_BIL:
    x->cur0 = &x->lvl[d];
    p->st[x->slot_tex] = x->f_bil;
    break;
  default:                                     /* ZC_TRI, ZC_NTRI */
    x->cur0 = &x->lvl[d]; x->cur1 = &x->lvl[d + 1];
    p->st[x->slot_tex] = (code >> 16) == ZC_TRI ? x->f_tri : x->f_ntri;
    break;
  }
  p->tex = x->ftex;
  p->talpha = x->falpha;
}

/* the code for rho^2 (level-0 texels): lambda = log2(rho^2) / 2. The
   NEAREST levels are exact from the float's bits - GL's d = ceil(lambda +
   1/2) - 1 is ceil(log2(rho^2)) >> 1, and lambda > 0 is rho^2 > 1 - so the
   common filters take no log2; the LINEAR blends need frac(lambda) and take
   a quadratic log2 (|error| < 0.005). MIN_LOD / MAX_LOD other than GL's
   defaults (lod_gen) go through zpx_code. */
static inline __attribute__((always_inline)) int zpx_code2(const ZPipeX *x, float rho2)
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
  return zpx_code(x, 0.5f * zpx_log2(rho2));
}

/* (out of line for zpx_tri, once per triangle: zp_run_lod inlines it) */
__attribute__((noinline))
static int zpx_code2_call(const ZPipeX *x, float rho2)
{
  return zpx_code2(x, rho2);
}

int zpx_tri(ZPipe *p, const ZTri *T, const ZVtxG *v0, const ZVtxG *v1,
            const ZVtxG *v2)
{
  ZPipeX *x = p->x;
  int lodb = 0;

  if (p->xact & ZPX_TEX) {
    float q0 = v0->q, q1 = v1->q, q2 = v2->q;
    if (zpx_qspread_lod(q0, q1, q2)) {
      /* (review 4 R1) its 1/w differ by more than ~3%: lambda varies
         across it (a floor or wall receding in depth), so each 8-pixel
         block picks its own level (zp_run_lod) - one lambda for the whole
         triangle drew the near end too coarse, the far end too fine and a
         seam along a quad's diagonal (15.6% against Mesa on one quad) */
      lodb = 1;
    } else {
      /* 1/w (nearly) constant: lambda is too, one per triangle at its
         centroid (the same as Mesa's per 2x2 quad on an affine triangle) */
      float s0 = (float)v0->si * q0, s1 = (float)v1->si * q1, s2 = (float)v2->si * q2;
      float t0 = (float)v0->ti * q0, t1 = (float)v1->ti * q1, t2 = (float)v2->ti * q2;
      float gsx, gsy, gtx, gty, gqx, gqy, r, u, v, iq, ux, uy, vx, vy, rx, ry;
      int code;
      ZTRI_GRAD(T, s0, s1, s2, gsx, gsy);
      ZTRI_GRAD(T, t0, t1, t2, gtx, gty);
      ZTRI_GRAD(T, q0, q1, q2, gqx, gqy);
      /* at the centroid, where each plane is the mean of its vertices */
      r = 1.0f / (q0 + q1 + q2);
      u = (s0 + s1 + s2) * r;
      v = (t0 + t1 + t2) * r;
      iq = 3.0f * r;
      ux = (gsx - u * gqx) * iq; uy = (gsy - u * gqy) * iq;
      vx = (gtx - v * gqx) * iq; vy = (gty - v * gqy) * iq;
      rx = ux * ux * x->lod_ks + vx * vx * x->lod_kt;
      ry = uy * uy * x->lod_ks + vy * vy * x->lod_kt;
      code = zpx_code2_call(x, rx > ry ? rx : ry);
      if ((code & ~255) != x->lod_cur) zpx_apply(p, x, code);
      x->twb[0] = x->twb[1] = x->twb[2] = x->twb[3] = (unsigned char)code;
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
      p->st[x->slot_col] = zc_smooth_pc;
      p->need = (x->need0 & ~ZP_N_RGBA) | ZP_N_PC | ZP_N_Q;
    } else {
      p->st[x->slot_col] = x->col_affine;
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
   trilinear weight goes to ZPipeX.twb, and the texel slot is re-placed only
   when a chunk's stage or levels differ from what it holds. The per-pixel
   stages are the ones zp_run runs. */
void zp_run_lod(ZBuffer *zb, ZSpan *s)
{
  ZPipe *p = (ZPipe *)zb->pipe;
  ZPipeX *x = p->x;
  const ZStageFn *st;
  ZFrag f;
  int n, m, nb, code, next = 0, have = 0;
  const float ks = x->lod_ks, kt = x->lod_kt;

  for (;;) {
    /* this span position's planes (x steps: d*dx; y: d*dy) */
    const float q0 = s->fz, dq = s->dfzdx;
    const float cx = s->dszdx * q0 - s->sz * dq, dx = s->dtzdx * q0 - s->tz * dq;
    const float kx = ks * cx * cx + kt * dx * dx;
    const float ny0 = s->dszdy * q0 - s->sz * s->dfzdy;
    const float dny = s->dszdy * dq - s->dszdx * s->dfzdy;
    const float my0 = s->dtzdy * q0 - s->tz * s->dfzdy;
    const int sn = s->n;
    const float dmy = s->dtzdy * dq - s->dtzdx * s->dfzdy;
#define ZBLK(o) ({ float o_ = (o), q_ = q0 + o_ * dq, ny_ = ny0 + o_ * dny, \
      my_ = my0 + o_ * dmy, ky_ = ks * ny_ * ny_ + kt * my_ * my_;          \
      q_ *= q_;                                                             \
      zpx_code2(x, (kx > ky_ ? kx : ky_) / (q_ * q_)); })
    /* the blocks of this chunk (one ZBLK site: it is the bulk of the
       code); a block whose levels differ starts the next chunk */
    n = nb = 0;
    code = 0;
    do {
      int c;
      m = sn - n < 8 ? sn - n : 8;
      c = have ? next : ZBLK((float)n + (float)(m - 1) * 0.5f);
      have = 0;
      /* (the weight bits of code are not read: the blocks' are in twb) */
      if (nb && ((c ^ code) & ~255)) { next = c; have = 1; break; }
      code = c;
      x->twb[nb++] = (unsigned char)c;
      n += m;
    } while (n < sn && n < ZP_CHUNK);
#undef ZBLK
    if ((code & ~255) != x->lod_cur) zpx_apply(p, x, code);
    f.n = n;
    if (p->depth(s, &f))
      for (st = p->st; *st; st++)
        (*st)(p, s, &f);
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
    s->sz += (float)n * s->dszdx; s->tz += (float)n * s->dtzdx;
    if (p->need & ZP_N_F) s->fq += (float)n * s->dfqdx;
    s->fz += (float)n * s->dfzdx;
    if (p->need & ZP_N_PC) {
      s->rq += (float)n * s->drqdx; s->gq += (float)n * s->dgqdx;
      s->bq += (float)n * s->dbqdx; s->aq += (float)n * s->daqdx;
    }
  }
}

void zpx_reset(ZPipe *p)
{
  ZPipeX *x = p->x;
  if (p->xact & ZPX_TEX) {
    int code = zpx_code(x, -64.0f);
    zpx_apply(p, x, code);
    x->twb[0] = x->twb[1] = x->twb[2] = x->twb[3] = (unsigned char)code;
  }
  if (p->xact & ZPX_PC) {
    p->st[x->slot_col] = x->col_affine;
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
