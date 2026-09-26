/*
 * zpipe_fused8.h - phase 5: the fused fillers of zpipe_fused.c for units
 * whose texels reach the texture environment as 8-bit words (ZTexF.t8:
 * P8 and L8 textures, s31_tex8.c, and every filtered RGB565 one - their
 * filters are Mesa's 8-bit ones). Included by zpipe_fused.c, whose
 * selection (zpf_select) calls zpf8_pick for such a batch. s31, MIT.
 *
 * The same signatures as the 565 fillers, and the same rule: each filler
 * computes exactly what the general stages it replaces compute - the T8
 * texel stages (s31_tfilter.c zx8_*: t8_fetch, BILERP8, LERP8W), the
 * texenv8 stages and zc_comb reading ZTexF.ftex32 - with their macros
 * (s31_tfilt_int.h, zpipe_int.h), so gl/tests/fused_test.c and
 * gl/tests/run-qsr-fused.sh (S31GL_FUSED=0 against the default) find no
 * differing pixel. What differs is how: one pass, no argument arrays, the
 * lightmap sampled from its grey plane alone (an L8 texel's r, g and b are
 * its grey; the combiner here reads no alpha), and the combiner's
 * product with its scale, MULS8, as one multiply a channel:
 *   min(255, div255r(x y 2^sh)) = min(255, (x m + 32894) >> 16), m = (y << sh) 257
 * for every x, y in 0..255 and sh in 0..2 (checked exhaustively by
 * gl/tests/fused_test.c; x m < 2^27).
 */

/* the combiner's product, as above */
#define ZF8_M(y, sh) (((unsigned int)(y) << (sh)) * 257u)
static inline unsigned int zf8_p(unsigned int x, unsigned int m)
{
  unsigned int v = (x * m + 32894u) >> 16;
  return v < 255u ? v : 255u;
}

/* an RGB565 texel's UNPACK as an RGBA8 word (alpha a) */
static inline unsigned int zf8_w565(unsigned int t, unsigned int a)
{
  unsigned int r = ((t >> 8) & 0xf8) | (t >> 13), g = ((t >> 3) & 0xfc) | ((t >> 9) & 3);
  unsigned int b = ((t << 3) & 0xf8) | ((t >> 2) & 7);
  return r | g << 8 | b << 16 | a << 24;
}

#define PACK8(w) PACK((w) & 255, ((w) >> 8) & 255, ((w) >> 16) & 255)

/* ------------------------------------------------------------ world, generic */

/* ZF_WORLD with any texel stages: the batch's own for both units (the
   levels chosen per triangle or block), then the combination. W0 / W1:
   the unit's texels are words (ftex32, idx = i) or 565 (ZPipe.tex / tex1) */
static inline __attribute__((always_inline))
void zf8_world_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int W0, const int W1)
{
  const ZPipeX *x = p->x;
  const PIXEL *t0 = p->tex, *t1 = p->tex1;
  const unsigned int *w0 = x->tf[0].ftex32, *w1 = x->tf[1].ftex32;
  const unsigned int *i0 = f->idx, *i1 = f->idx1;
  const int sh = x->cb[1].sh[0];
  PIXEL *pp = s->pp;
  int i, n = f->n;

  (*x->tf[0].slot)(p, s, f);
  (*x->tf[1].slot)(p, s, f);
  for (i = 0; i < n; i++) {
    unsigned int a, b;
    if (!f->m[i]) continue;
    a = W0 ? w0[i0[i]] : zf8_w565(t0[i0[i]], 255);
    b = W1 ? w1[i1[i]] : zf8_w565(t1[i1[i]], 255);
    /* zc_comb's MODULATE (PREVIOUS, TEXTURE) x 2^sh: the previous colour
       (unit 0's REPLACE: its texel) first */
    pp[i] = PACK(MULS8(a & 255, b & 255, sh), MULS8((a >> 8) & 255, (b >> 8) & 255, sh),
                 MULS8((a >> 16) & 255, (b >> 16) & 255, sh));
  }
}
static void zf8_world_11(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_t(p, s, f, 1, 1); }
static void zf8_world_10(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_t(p, s, f, 1, 0); }
static void zf8_world_01(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_t(p, s, f, 0, 1); }

/* ------------------------------------------------------------ world, inline */

/* the texture of a world batch (unit 0) as the inline loops read it: K0
   KP8 (an RGB-class P8 texture: its words' alpha is 255) or K565 (an
   RGB565 one: filtered, so its texels are words to the general path too) */
enum { KP8, K565 };

/* one level's constants, REPEAT only (the batch's wraps are REPEAT on both
   axes) */
#define LV8(L, Z)                                                       \
  const unsigned char *i8_##L = (Z)->i8;                                \
  const unsigned int *pal_##L = (Z)->pal;                               \
  const PIXEL *px_##L = (Z)->pix;                                       \
  const int shs_##L = (Z)->shs, sht_##L = (Z)->sht, ws_##L = (Z)->ws;   \
  const int wm_##L = (Z)->wm, hm_##L = (Z)->hm;                         \
  const unsigned int hs_##L = 1u << (shs_##L - 1), ht_##L = 1u << (sht_##L - 1); \
  (void)i8_##L; (void)pal_##L; (void)px_##L; (void)hs_##L; (void)ht_##L; \
  (void)ws_##L;

/* texel k of level L of kind K0 as its word */
#define FT8(L, k) (K0 == KP8 ? pal_##L[i8_##L[k]] : zf8_w565(px_##L[k], 255))

/* nearest in level L (NEAR_IDX with WR_R) */
#define NEAR8(L, SI, TI)                                                \
  (((unsigned int)(((int)(TI) >> sht_##L) & hm_##L) << ws_##L) |       \
   (unsigned int)(((int)(SI) >> shs_##L) & wm_##L))

/* BILERP8(L, WR_R, C) at (SI, TI), the texture's kind K0 */
#define BIL8(L, SI, TI, C)                                              \
  {                                                                     \
    unsigned int u_ = (SI) - hs_##L, v_ = (TI) - ht_##L;                \
    int c0_ = (int)u_ >> shs_##L, r0_ = (int)v_ >> sht_##L;             \
    unsigned int fu_ = W8(u_, shs_##L), fv_ = W8(v_, sht_##L);          \
    unsigned int o0_ = (unsigned int)(r0_ & hm_##L) << ws_##L;          \
    unsigned int o1_ = (unsigned int)((r0_ + 1) & hm_##L) << ws_##L;    \
    unsigned int ca_ = (unsigned int)(c0_ & wm_##L), cb_ = (unsigned int)((c0_ + 1) & wm_##L); \
    unsigned int a_ = FT8(L, o0_ + ca_), b_ = FT8(L, o0_ + cb_);        \
    unsigned int c_ = FT8(L, o1_ + ca_), d_ = FT8(L, o1_ + cb_);        \
    a_ = LERP8W(a_, b_, fu_);                                           \
    c_ = LERP8W(c_, d_, fu_);                                           \
    C = LERP8W(a_, c_, fv_);                                            \
  }

/* the lightmap (unit 1: an L8 level, bilinear in level 0, REPEAT) at
   (SI, TI): its grey. The last 2x2 square is kept as two words of two
   16-bit fields (column 0 of both rows, and the columns' difference), so
   a hit is one multiply for both rows' horizontal lerp and one for the
   vertical: the general path's LERP8W on each of the grey's channels,
   field for field */
#define LM8_VARS(Z)                                                     \
  const unsigned char *lm_ = (Z)->i8;                                   \
  const int lshs_ = (Z)->shs, lsht_ = (Z)->sht, lws_ = (Z)->ws;         \
  const int lwm_ = (Z)->wm, lhm_ = (Z)->hm;                             \
  const unsigned int lhs_ = 1u << (lshs_ - 1), lht_ = 1u << (lsht_ - 1); \
  int lqc_ = 0, lqr_ = 0x7fffffff;                                      \
  unsigned int lqa_ = 0, lqd_ = 0;
#define LM8(SI, TI, L)                                                  \
  {                                                                     \
    unsigned int u_ = (SI) - lhs_, v_ = (TI) - lht_;                    \
    int c0_ = (int)u_ >> lshs_, r0_ = (int)v_ >> lsht_;                 \
    unsigned int fu_ = W8(u_, lshs_), fv_ = W8(v_, lsht_), h_, t_, b_;  \
    if (c0_ != lqc_ || r0_ != lqr_) {                                   \
      const unsigned char *w0_ = lm_ + ((r0_ & lhm_) << lws_);          \
      const unsigned char *w1_ = lm_ + (((r0_ + 1) & lhm_) << lws_);    \
      int ca_ = c0_ & lwm_, cb_ = (c0_ + 1) & lwm_;                     \
      unsigned int a_ = w0_[ca_] | (unsigned int)w1_[ca_] << 16;        \
      unsigned int e_ = w0_[cb_] | (unsigned int)w1_[cb_] << 16;        \
      lqa_ = a_ << 8; lqd_ = e_ - a_;                                   \
      lqc_ = c0_; lqr_ = r0_;                                           \
    }                                                                   \
    h_ = lqa_ + lqd_ * fu_;                                             \
    t_ = (h_ >> 8) & 255u; b_ = h_ >> 24;                               \
    L = ((t_ << 8) + (b_ - t_) * fv_) >> 8 & 255u;                      \
  }

/* the product and the store: pp = PACK of MULS8(texel, L, sh) a channel */
/* PACK(zf8_p(r, m), zf8_p(g, m), zf8_p(b, m)) with each field's
   truncation folded into the product's shift: (min(255, v) & 0xf8) << 8
   = min(31, v >> 3) << 11, v >> 3 = (x m + 32894) >> 19 (and >> 18, 63 for
   green) - the same bits, one instruction a field fewer */
static inline unsigned int zf8_pack(unsigned int r, unsigned int g, unsigned int b,
                                    unsigned int m)
{
  unsigned int R = (r * m + 32894u) >> 19, G = (g * m + 32894u) >> 18;
  unsigned int B = (b * m + 32894u) >> 19;
  R = R < 31u ? R : 31u; G = G < 63u ? G : 63u; B = B < 31u ? B : 31u;
  return R << 11 | G << 5 | B;
}
#define WPIX8(T, M) ((PIXEL)zf8_pack((T) & 255u, ((T) >> 8) & 255u, ((T) >> 16) & 255u, M))

/* Both units nearest, REPEAT, depth inline (D: 0 zdw_lequal, 1 zd_lequal,
   2 zdw_less): the walks of zt_rr / zx8_base (unit 0: 565 index or P8
   word) and zx8_1_base (unit 1, the L8 lightmap: its grey) */
static inline __attribute__((always_inline))
void zf8_world_nn_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int K0, const int D)
{
  const ZPipeX *x = p->x;
  const ZLevel *L0 = &x->tf[0].lvl[0], *LM = &x->tf[1].lvl[0];
  const PIXEL *t0 = p->tex;
  const unsigned char *ix0 = L0->i8, *lm = LM->i8;
  const unsigned int *pal0 = L0->pal;
  const int sh = x->cb[1].sh[0];
  const unsigned int tm0 = p->tmask, sm0 = p->smask;
  const unsigned int tm1 = x->g1.tmask, sm1 = x->g1.smask;
  const int fb0 = p->fbits, fb1 = x->g1.fbits;
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  float fz = s->fz, sz = s->sz, tz = s->tz, sz1 = s->sz1, tz1 = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  const float dsz1 = s->dszdx1, dtz1 = s->dtzdx1;
  int i = 0, e, n = f->n, ok;
  /* (memoising the last texel's product measured worse: 18.13 -> 18.37 M
     a frame on the TEXFILTER=0 trace - few texels repeat at 320 x 240) */
  (void)t0; (void)ix0; (void)pal0; (void)LM;
  while (i < n) {
    float zinv = 1.0f / fz;
    /* unit 0: zt_rr's walk (int) for 565, TWALK's (unsigned) for P8: the
       same values */
    float ss = sz * zinv, tt = tz * zinv;
    unsigned int si = (unsigned int)(int)ss, ti = (unsigned int)(int)tt;
    unsigned int dsi = (unsigned int)(int)((dsz - ss * dfz) * zinv);
    unsigned int dti = (unsigned int)(int)((dtz - tt * dfz) * zinv);
    float ss1 = sz1 * zinv, tt1 = tz1 * zinv;
    unsigned int si1 = (unsigned int)(int)ss1, ti1 = (unsigned int)(int)tt1;
    unsigned int dsi1 = (unsigned int)(int)((dsz1 - ss1 * dfz) * zinv);
    unsigned int dti1 = (unsigned int)(int)((dtz1 - tt1 * dfz) * zinv);
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;
      ok = D == 2 ? zz > pz[i] : zz >= pz[i];
      if (ok) {
        unsigned int a, l;
        /* (level 0: NEAR8's index is zt_rr's, ((t & tmask) | (s & smask))
           >> F - the same bits, fewer live constants) */
        if (K0 == K565)
          a = zf8_w565(t0[((ti & tm0) | (si & sm0)) >> fb0], 255);
        else
          a = pal0[ix0[((ti & tm0) | (si & sm0)) >> fb0]];
        l = lm[((ti1 & tm1) | (si1 & sm1)) >> fb1];
        if (D != 1) pz[i] = (unsigned short)zz;
        {
          const unsigned int m = ZF8_M(l, sh);
          pp[i] = WPIX8(a, m);
        }
      }
      z += s->dzdx;
      si += dsi; ti += dti; si1 += dsi1; ti1 += dti1;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
    sz1 += 8.0f * dsz1; tz1 += 8.0f * dtz1;
  }
}
static void zf8_wnn_p0(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_nn_t(p, s, f, KP8, 0); }
static void zf8_wnn_p1(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_nn_t(p, s, f, KP8, 1); }
static void zf8_wnn_p2(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_nn_t(p, s, f, KP8, 2); }
static void zf8_wnn_c0(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_nn_t(p, s, f, K565, 0); }
static void zf8_wnn_c1(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_nn_t(p, s, f, K565, 1); }
static void zf8_wnn_c2(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_world_nn_t(p, s, f, K565, 2); }

/* The filtered world (unit 1 the L8 lightmap, bilinear in level 0; unit 0
   any filter, its kind K0), depth LEQUAL with the write inline: pass 1 the
   depth and the lightmap (its grey into ZFrag.idx1, ZF_DEAD for a dead
   fragment), pass 2 unit 0's sample and the product - as zf_wx_* for 565,
   so no loop holds more than two levels' constants. Unit 0 nearest (level
   0, d, or two levels blended) takes one pass */
static inline __attribute__((always_inline))
void zf8_wx_p1(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZPipeX *x = p->x;
  unsigned int *bb = f->idx1;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  const int dzdx = s->dzdx;
  float fz = s->fz, sz = s->sz1, tz = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx1, dtz = s->dtzdx1;
  int i = 0, e, n = f->n;
  LM8_VARS(x->tf[1].cur0)
  while (i < n) {
    float zinv = 1.0f / fz;
    ZF_WALK_HEAD(sz, tz, dsz, dtz, si, ti, dsi, dti)
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      unsigned int b = ZF_DEAD;
      zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;
      if (zz >= pz[i]) {
        LM8(si, ti, b)
        pz[i] = (unsigned short)zz;
      }
      bb[i] = b;
      z += dzdx;
      si += dsi; ti += dti;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
  }
}

static inline __attribute__((always_inline))
void zf8_wx_p2(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA,
               const int K, const int K0)
{
  const ZPipeX *x = p->x;
  const ZTexF *u0 = &x->tf[0];
  const unsigned int *bb = f->idx1;
  const int sh = x->cb[1].sh[0];
  PIXEL *pp = s->pp;
  float fz = s->fz, sz = s->sz, tz = s->tz;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  int i = 0, e, n = f->n;
  LV8(A, LA)
  LV8(B, K == ZK_TRI ? u0->cur1 : LA)
  while (i < n) {
    float zinv = 1.0f / fz;
    ZF_WALK_HEAD(sz, tz, dsz, dtz, si, ti, dsi, dti)
    unsigned int tw = K == ZK_TRI ? ZPX_TW8(u0->twb[i >> 3]) : 0;
    (void)tw;
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      unsigned int b = bb[i], a;
      if (b != ZF_DEAD) {
        const unsigned int m = ZF8_M(b, sh);
        if (K == ZK_BIL) {
          BIL8(A, si, ti, a)
        } else {
          unsigned int a1;
          BIL8(A, si, ti, a)
          BIL8(B, si, ti, a1)
          a = LERP8W(a, a1, tw);
        }
        pp[i] = WPIX8(a, m);
      }
      si += dsi; ti += dti;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
  }
}

/* unit 0 nearest (level 0 or d, or NEAREST_MIPMAP_LINEAR's two): one pass */
static inline __attribute__((always_inline))
void zf8_wx_1p(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA, const int K,
               const int K0)
{
  const ZPipeX *x = p->x;
  const int sh = x->cb[1].sh[0];
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  const int dzdx = s->dzdx;
  float fz = s->fz, sz = s->sz, tz = s->tz, sz1 = s->sz1, tz1 = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  const float dsz1 = s->dszdx1, dtz1 = s->dtzdx1;
  int i = 0, e, n = f->n;
  LM8_VARS(x->tf[1].cur0)
  LV8(A, LA)
  LV8(B, K == ZK_NTRI ? x->tf[0].cur1 : LA)
  while (i < n) {
    float zinv = 1.0f / fz;
    ZF_WALK_HEAD(sz, tz, dsz, dtz, si, ti, dsi, dti)
    ZF_WALK_HEAD(sz1, tz1, dsz1, dtz1, si1, ti1, dsi1, dti1)
    unsigned int tw = K == ZK_NTRI ? ZPX_TW8(x->tf[0].twb[i >> 3]) : 0;
    (void)tw;
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;
      if (zz >= pz[i]) {
        unsigned int a, l, m;
        LM8(si1, ti1, l)
        pz[i] = (unsigned short)zz;
        m = ZF8_M(l, sh);
        a = FT8(A, NEAR8(A, si, ti));
        if (K == ZK_NTRI) a = LERP8W(a, FT8(B, NEAR8(B, si, ti)), tw);
        pp[i] = WPIX8(a, m);
      }
      z += dzdx;
      si += dsi; ti += dti; si1 += dsi1; ti1 += dti1;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
    sz1 += 8.0f * dsz1; tz1 += 8.0f * dtz1;
  }
}

#define ZF8_WX(K0N, K0)                                                 \
__attribute__((noinline))                                               \
static void zf8_wx_near_##K0N(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA) \
{ zf8_wx_1p(p, s, f, LA, ZK_NEAR, K0); }                                \
__attribute__((noinline))                                               \
static void zf8_wx_ntri_##K0N(const ZPipe *p, const ZSpan *s, ZFrag *f) \
{ zf8_wx_1p(p, s, f, p->x->tf[0].cur0, ZK_NTRI, K0); }                  \
__attribute__((noinline))                                               \
static void zf8_wx_bil_##K0N(const ZPipe *p, const ZSpan *s, ZFrag *f)  \
{ zf8_wx_p1(p, s, f); zf8_wx_p2(p, s, f, p->x->tf[0].cur0, ZK_BIL, K0); } \
__attribute__((noinline))                                               \
static void zf8_wx_tri_##K0N(const ZPipe *p, const ZSpan *s, ZFrag *f)  \
{ zf8_wx_p1(p, s, f); zf8_wx_p2(p, s, f, p->x->tf[0].cur0, ZK_TRI, K0); } \
/* the chunk's unit 0 texel stage picks the loop */                     \
static void zf8_world_x_##K0N(const ZPipe *p, const ZSpan *s, ZFrag *f) \
{                                                                       \
  const ZPipeX *x = p->x;                                               \
  const ZTexF *u0 = &x->tf[0];                                          \
  ZStageFn t = *u0->slot;                                               \
  if (t == u0->f_tri) zf8_wx_tri_##K0N(p, s, f);                        \
  else if (t == u0->f_bil) zf8_wx_bil_##K0N(p, s, f);                   \
  else if (t == u0->f_near) zf8_wx_near_##K0N(p, s, f, u0->cur0);       \
  else if (t == u0->f_ntri) zf8_wx_ntri_##K0N(p, s, f);                 \
  else if (zpx_is_base8(t, 0))                                          \
    zf8_wx_near_##K0N(p, s, f, &u0->lvl[0]);                            \
  else if (x->gen_depth(s, f)) zf8_world_11(p, s, f);                   \
}
ZF8_WX(p8, KP8)

/* The filtered world with an RGB565 texture sampled by phase 4's 565
   filter (unit 0 not t8: S31GL_FILT8=0, the default) and the L8 lightmap:
   unit 0's sample exactly as zf_wx_* take it (s31_tfilt_int.h BILERP, the
   cached BILQ, LERP of the spread words), UNPACK of the result - what
   ze_replace_rgb makes of it - and the lightmap's grey (LM8) for the
   combiner's product */
#define WPIXS(T565, M)                                                  \
  ({ unsigned int t_ = (T565), r_, g_, b_;                              \
     r_ = ((t_ >> 8) & 0xf8) | (t_ >> 13); g_ = ((t_ >> 3) & 0xfc) | ((t_ >> 9) & 3); \
     b_ = ((t_ << 3) & 0xf8) | ((t_ >> 2) & 7);                         \
     (PIXEL)zf8_pack(r_, g_, b_, M); })

static inline __attribute__((always_inline))
void zf8s_wx_p2(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA, const int K)
{
  const ZPipeX *x = p->x;
  const ZTexF *u0 = &x->tf[0];
  const unsigned int *bb = f->idx1;
  const int sh = x->cb[1].sh[0];
  PIXEL *pp = s->pp;
  float fz = s->fz, sz = s->sz, tz = s->tz;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  int i = 0, e, n = f->n;
  LVR(A, LA)
  LVR(B, K == ZK_TRI ? u0->cur1 : LA)
  while (i < n) {
    float zinv = 1.0f / fz;
    ZF_WALK_HEAD(sz, tz, dsz, dtz, si, ti, dsi, dti)
    unsigned int tw = K == ZK_TRI ? ZPX_TW5(u0->twb[i >> 3]) : 0;
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      unsigned int b = bb[i], a;
      if (b != ZF_DEAD) {
        const unsigned int m = ZF8_M(b, sh);
        if (K == ZK_BIL) {
          BILQ(A, si, ti, a)
        } else {
          unsigned int a1;
          BILERPX(A, WR_R, a, a, 0, LERP)
          BILQX(B, si, ti, a1, LERP)
          a = LERPT(a, a1, tw);
        }
        pp[i] = WPIXS(UNSPREAD(a), m);
      }
      si += dsi; ti += dti;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
  }
}

static inline __attribute__((always_inline))
void zf8s_wx_1p(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA, const int K)
{
  const ZPipeX *x = p->x;
  const int sh = x->cb[1].sh[0];
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  const int dzdx = s->dzdx;
  float fz = s->fz, sz = s->sz, tz = s->tz, sz1 = s->sz1, tz1 = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  const float dsz1 = s->dszdx1, dtz1 = s->dtzdx1;
  int i = 0, e, n = f->n;
  LM8_VARS(x->tf[1].cur0)
  LVR(A, LA)
  LVR(B, K == ZK_NTRI ? x->tf[0].cur1 : LA)
  while (i < n) {
    float zinv = 1.0f / fz;
    ZF_WALK_HEAD(sz, tz, dsz, dtz, si, ti, dsi, dti)
    ZF_WALK_HEAD(sz1, tz1, dsz1, dtz1, si1, ti1, dsi1, dti1)
    unsigned int tw = K == ZK_NTRI ? ZPX_TW5(x->tf[0].twb[i >> 3]) : 0;
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;
      if (zz >= pz[i]) {
        unsigned int a, l, m;
        LM8(si1, ti1, l)
        pz[i] = (unsigned short)zz;
        m = ZF8_M(l, sh);
        if (K == ZK_NTRI)
          a = UNSPREAD(LERPT(SPREAD((unsigned int)NEARR(A, si, ti)),
                            SPREAD((unsigned int)NEARR(B, si, ti)), tw));
        else
          a = NEARR(A, si, ti);
        pp[i] = WPIXS(a, m);
      }
      z += dzdx;
      si += dsi; ti += dti; si1 += dsi1; ti1 += dti1;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
    sz1 += 8.0f * dsz1; tz1 += 8.0f * dtz1;
  }
}

__attribute__((noinline))
static void zf8s_wx_near(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA)
{ zf8s_wx_1p(p, s, f, LA, ZK_NEAR); }
__attribute__((noinline))
static void zf8s_wx_ntri(const ZPipe *p, const ZSpan *s, ZFrag *f)
{ zf8s_wx_1p(p, s, f, p->x->tf[0].cur0, ZK_NTRI); }
__attribute__((noinline))
static void zf8s_wx_bil(const ZPipe *p, const ZSpan *s, ZFrag *f)
{ zf8_wx_p1(p, s, f); zf8s_wx_p2(p, s, f, p->x->tf[0].cur0, ZK_BIL); }
__attribute__((noinline))
static void zf8s_wx_tri(const ZPipe *p, const ZSpan *s, ZFrag *f)
{ zf8_wx_p1(p, s, f); zf8s_wx_p2(p, s, f, p->x->tf[0].cur0, ZK_TRI); }

static void zf8_world_xs(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZPipeX *x = p->x;
  const ZTexF *u0 = &x->tf[0];
  ZStageFn t = *u0->slot;
  if (t == u0->f_tri) zf8s_wx_tri(p, s, f);
  else if (t == u0->f_bil) zf8s_wx_bil(p, s, f);
  else if (t == u0->f_near) zf8s_wx_near(p, s, f, u0->cur0);
  else if (t == u0->f_ntri) zf8s_wx_ntri(p, s, f);
  else if (t == zp_texidx_fn(0, 0)) {
    /* level 0 as the texidx stage addresses it (as zf_world_x) */
    ZLevel lb;
    lb.pix = p->tex;
    lb.shs = p->fbits; lb.sht = p->fbits + p->ws; lb.ws = p->ws;
    lb.wm = p->wmax; lb.hm = p->hmax;
    zf8s_wx_near(p, s, f, &lb);
  } else if (x->gen_depth(s, f)) zf8_world_01(p, s, f);
}

/* ------------------------------------------------------------ alias */

/* ZF_ALIAS with a unit (or both) of 8-bit words: the colour stage and both
   texel stages, then each unit's texels as words (a 565 unit's UNPACK, its
   A8 plane or 255), then the loop of zf_alias_t on words */
static inline __attribute__((always_inline))
void zf8_alias_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int BL)
{
  const ZPipeX *x = p->x;
  PIXEL *pp = s->pp;
  const int sh = x->cb[0].sh[0], sha = x->cb[0].sh[1];
  const int a1on = BL && x->cb[1].f[1] != ZCF_NONE;
  unsigned int w0[ZP_CHUNK], w1[ZP_CHUNK];
  int i, k, n = f->n;

  (*x->slot_col)(p, s, f);
  (*x->tf[0].slot)(p, s, f);
  (*x->tf[1].slot)(p, s, f);
  for (k = 0; k < 2; k++) {
    const unsigned int *idx = k ? f->idx1 : f->idx;
    unsigned int *w = k ? w1 : w0;
    if (x->tf[k].t8) {
      const unsigned int *src = x->tf[k].ftex32;
      for (i = 0; i < n; i++) w[i] = src[idx[i]];
    } else {
      const PIXEL *t = k ? p->tex1 : p->tex;
      const unsigned char *al = k ? p->talpha1 : p->talpha;
      const int hasa = al != NULL && x->cb[k].fmt != TGL_TEXF_RGB;
      for (i = 0; i < n; i++) w[i] = zf8_w565(t[idx[i]], hasa ? al[idx[i]] : 255);
    }
  }
  for (i = 0; i < n; i++) {
    int r, g, b, a;
    unsigned int t0, t1;
    if (!f->m[i]) continue;
    t0 = w0[i]; t1 = w1[i];
    /* unit 0: MODULATE (TEXTURE, PRIMARY_COLOR), the alpha MODULATE
       (TEXTURE, PREVIOUS) - the texel first, as zc_comb orders them */
    r = MULS8(t0 & 255, f->r[i], sh);
    g = MULS8((t0 >> 8) & 255, f->g[i], sh);
    b = MULS8((t0 >> 16) & 255, f->b[i], sh);
    a = BL ? MULS8(t0 >> 24, f->a[i], sha) : 0;
    /* unit 1: ADD (PREVIOUS, TEXTURE); its alpha MODULATE (PREVIOUS,
       TEXTURE) when its program has one */
    r = clamp255(r + (int)(t1 & 255));
    g = clamp255(g + (int)((t1 >> 8) & 255));
    b = clamp255(b + (int)((t1 >> 16) & 255));
    if (a1on) a = clamp255(MUL8(a, (int)(t1 >> 24)));
    if (BL) {
      int dr, dg, db;
      UNPACK(pp[i], dr, dg, db);
      pp[i] = PACK(MUL8(r, a) + MUL8(dr, 255 - a), MUL8(g, a) + MUL8(dg, 255 - a),
                   MUL8(b, a) + MUL8(db, 255 - a));
    } else {
      pp[i] = PACK(r, g, b);
    }
  }
}
static void zf8_alias_bl(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_alias_t(p, s, f, 1); }
static void zf8_alias_st(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_alias_t(p, s, f, 0); }

/* ------------------------------------------------------------ one unit */

/* zx8_bil (level 0, REPEAT here) with the exact shortcuts of zf_bil0_t:
   LERP8W(x, y, 0) is x and LERP8W(x, y, 256) is y, so with fv 0 or 256
   the sample is one row's lerp, with fu 0 or 256 one column's (a picture
   drawn texel for texel). Writes what the stage writes (ftex32, idx = i).
   K8: the level's kind as the loop reads it - KB_P8 its palette, KB_GEN
   t8_fetch (any kind) */
enum { KB_P8, KB_GEN };
#define FB8(k) (K8 == KB_P8 ? pal_[i8_[k]] : t8_fetch(L0, (k)))
static inline __attribute__((always_inline))
void zf8_bil0_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int K8)
{
  ZPipeX *x = p->x;
  ZTexF *u = &x->tf[0];
  const ZLevel *L0 = u->cur0;
  const unsigned char *i8_ = L0->i8;
  const unsigned int *pal_ = L0->pal;
  unsigned int *ft = u->ftex32;
  LEVEL_VARS(L0)
  (void)i8_; (void)pal_;
  TWALK(,
    unsigned int c;
    unsigned int uq_ = si - (1u << (shs_L0 - 1)), vq_ = ti - (1u << (sht_L0 - 1));
    unsigned int fu0_ = W8(uq_, shs_L0), fv0_ = W8(vq_, sht_L0);
    int c0q_ = (int)uq_ >> shs_L0, r0q_ = (int)vq_ >> sht_L0;
    if (!(fv0_ & 255)) {
      const unsigned int oq_ = (unsigned int)((r0q_ + (int)(fv0_ >> 8)) & hm_L0) << ws_L0;
      c = LERP8W(FB8(oq_ + (unsigned int)(c0q_ & wm_L0)),
                 FB8(oq_ + (unsigned int)((c0q_ + 1) & wm_L0)), fu0_);
    } else if (!(fu0_ & 255)) {
      const unsigned int cq_ = (unsigned int)((c0q_ + (int)(fu0_ >> 8)) & wm_L0);
      c = LERP8W(FB8(((unsigned int)(r0q_ & hm_L0) << ws_L0) + cq_),
                 FB8(((unsigned int)((r0q_ + 1) & hm_L0) << ws_L0) + cq_), fv0_);
    } else {
      const unsigned int o0_ = (unsigned int)(r0q_ & hm_L0) << ws_L0;
      const unsigned int o1_ = (unsigned int)((r0q_ + 1) & hm_L0) << ws_L0;
      const unsigned int ca_ = (unsigned int)(c0q_ & wm_L0), cb_ = (unsigned int)((c0q_ + 1) & wm_L0);
      unsigned int a_ = LERP8W(FB8(o0_ + ca_), FB8(o0_ + cb_), fu0_);
      unsigned int b_ = LERP8W(FB8(o1_ + ca_), FB8(o1_ + cb_), fu0_);
      c = LERP8W(a_, b_, fv0_);
    }
    ft[i] = c;
    f->idx[i] = (unsigned int)i;
  )
}
__attribute__((noinline))
static void zf8_bil0_p8(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_bil0_t(p, s, f, KB_P8); }

/* the same for an L8 level (alpha none, bits or A8): the grey by one lerp
   of the cached 2x2 square's two rows at once (LM8's arithmetic), and the
   alpha - when the signature reads it (AL) - by LERP8 of the texels' */
static inline __attribute__((always_inline))
void zf8_bil0_l8_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int AL)
{
  ZPipeX *x = p->x;
  ZTexF *u = &x->tf[0];
  const ZLevel *L0 = u->cur0;
  const unsigned char *al = L0->alpha;
  const int am = L0->am;
  unsigned int *ft = u->ftex32;
  LM8_VARS(L0)
  (void)al; (void)am;
  TWALK(,
    unsigned int g, a = 255;
    LM8(si, ti, g)
    if (AL && am != TGL_AM_ONE) {
      /* the alpha of the square BILERP8 reads, lerped as its A field */
      unsigned int u_ = si - lhs_, v_ = ti - lht_;
      int c0_ = (int)u_ >> lshs_, r0_ = (int)v_ >> lsht_;
      unsigned int fu_ = W8(u_, lshs_), fv_ = W8(v_, lsht_);
      unsigned int o0_ = (unsigned int)(r0_ & lhm_) << lws_, o1_ = (unsigned int)((r0_ + 1) & lhm_) << lws_;
      unsigned int ca_ = (unsigned int)(c0_ & lwm_), cb_ = (unsigned int)((c0_ + 1) & lwm_);
      unsigned int k0 = o0_ + ca_, k1 = o0_ + cb_, k2 = o1_ + ca_, k3 = o1_ + cb_;
      unsigned int a0, a1, a2, a3, t_, b_;
      if (am == TGL_AM_A8) {
        a0 = al[k0]; a1 = al[k1]; a2 = al[k2]; a3 = al[k3];
      } else {
        a0 = (0u - ((al[k0 >> 3] >> (k0 & 7)) & 1u)) & 255u;
        a1 = (0u - ((al[k1 >> 3] >> (k1 & 7)) & 1u)) & 255u;
        a2 = (0u - ((al[k2 >> 3] >> (k2 & 7)) & 1u)) & 255u;
        a3 = (0u - ((al[k3 >> 3] >> (k3 & 7)) & 1u)) & 255u;
      }
      t_ = ((a0 << 8) + (a1 - a0) * fu_) >> 8 & 255u;
      b_ = ((a2 << 8) + (a3 - a2) * fu_) >> 8 & 255u;
      a = ((t_ << 8) + (b_ - t_) * fv_) >> 8 & 255u;
    }
    ft[i] = g * 0x010101u | a << 24;
    f->idx[i] = (unsigned int)i;
  )
}
__attribute__((noinline))
static void zf8_bil0_l8(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_bil0_l8_t(p, s, f, 0); }
__attribute__((noinline))
static void zf8_bil0_l8a(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_bil0_l8_t(p, s, f, 1); }

/* the batch's bilinear sampler (ZPipeX.bil0): 1 P8, 3 L8 without the
   alpha, 4 L8 with it (2, any kind, is gone: fix 2) */
static inline void zf8_bil0(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  switch (p->x->bil0) {
  case 1: zf8_bil0_p8(p, s, f); break;
  case 3: zf8_bil0_l8(p, s, f); break;
  case 4: zf8_bil0_l8a(p, s, f); break;
  default: zf8_bil0_l8a(p, s, f); break;
  }
}

/* zf_one_t for a unit-0 texel of 8 bits (ZTexF.ftex32, idx = i): the same
   signatures, the texenv8 stages' arithmetic */
static inline __attribute__((always_inline))
void zf8_one_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int COL, const int ENV,
               const int AT, const int ZW, const int OUT)
{
  const ZPipeX *x = p->x;
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  const unsigned int *idx = f->idx;
  const unsigned int *tw = x->tf[0].ftex32;
  const int n = f->n, aref = p->aref, sh = x->cb[0].sh[0];
  const int fr = p->flat[0], fg = p->flat[1], fb = p->flat[2], fa = p->flat[3];
  int i;
  (void)pz; (void)aref; (void)sh; (void)fr; (void)fg; (void)fb; (void)fa; (void)tw;
  if (COL == C_SMOOTH) (*x->slot_col)(p, s, f);
  if (ENV != E_NONE) {
    if (x->bil0) zf8_bil0(p, s, f);
    else (*x->tf[0].slot)(p, s, f);
  }
  if (COL == C_FLAT && (ENV == E_REP_RGB || ENV == E_NONE) && !AT && !ZW && OUT == O_SAOMSA) {
    /* a flat alpha: opaque, the texel is stored as it is; else each field
       MUL8(src, a) + MUL8(e(dst), 255 - a), the destination's half from the
       tables (zf_btab) */
    const unsigned char *bt;
    if (ENV == E_REP_RGB && fa == 255) {
      for (i = 0; i < n; i++)
        if (f->m[i]) pp[i] = PACK8(tw[idx[i]]);
      return;
    }
    bt = zf_btab((ZPipeX *)x, fa);
    if (bt) {
      if (ENV == E_NONE) {
        const int sr = MUL8(fr, fa), sg = MUL8(fg, fa), sb = MUL8(fb, fa);
        for (i = 0; i < n; i++) {
          unsigned int d = pp[i];
          if (!f->m[i]) continue;
          pp[i] = PACK(sr + bt[32 + (d >> 11)], sg + bt[128 + ((d >> 5) & 63)],
                       sb + bt[32 + (d & 31)]);
        }
      } else {
        for (i = 0; i < n; i++) {
          unsigned int d = pp[i], t;
          if (!f->m[i]) continue;
          t = tw[idx[i]];
          pp[i] = PACK(MUL8((int)(t & 255), fa) + bt[32 + (d >> 11)],
                       MUL8((int)((t >> 8) & 255), fa) + bt[128 + ((d >> 5) & 63)],
                       MUL8((int)((t >> 16) & 255), fa) + bt[32 + (d & 31)]);
        }
      }
      return;
    }
  }
  for (i = 0; i < n; i++) {
    int r = 0, g = 0, b = 0, a = 0, tr, tg, tb;
    unsigned int t = 0;
    if (!f->m[i]) continue;
    if (COL == C_FLAT) { r = fr; g = fg; b = fb; a = fa; }
    else if (COL == C_SMOOTH) { r = f->r[i]; g = f->g[i]; b = f->b[i]; a = f->a[i]; }
    if (ENV != E_NONE) t = tw[idx[i]];
    tr = (int)(t & 255); tg = (int)((t >> 8) & 255); tb = (int)((t >> 16) & 255);
    switch (ENV) {
    case E_REP_RGB:
      r = tr; g = tg; b = tb;
      break;
    case E_REP_RGBA:
      r = tr; g = tg; b = tb;
      a = (int)(t >> 24);
      break;
    case E_MOD_RGB:
      r = MUL8(r, tr); g = MUL8(g, tg); b = MUL8(b, tb);
      break;
    case E_MOD_RGBA:
      r = MUL8(r, tr); g = MUL8(g, tg); b = MUL8(b, tb);
      a = MUL8(a, (int)(t >> 24));
      break;
    case E_COMB_MT:
      /* zc_comb's MODULATE of (TEXTURE, PRIMARY_COLOR): the texel first */
      r = MULS8(tr, r, sh); g = MULS8(tg, g, sh); b = MULS8(tb, b, sh);
      break;
    default:
      break;
    }
    if (AT && !(a > aref)) continue;           /* za_greater */
    if (ZW) pz[i] = f->zz[i];                  /* zw_write */
    if (OUT == O_STORE) {
      pp[i] = PACK(r, g, b);
    } else {
      int dr, dg, db;
      UNPACK(pp[i], dr, dg, db);
      if (OUT == O_SAOMSA)
        pp[i] = PACK(MUL8(r, a) + MUL8(dr, 255 - a), MUL8(g, a) + MUL8(dg, 255 - a),
                     MUL8(b, a) + MUL8(db, 255 - a));
      else if (OUT == O_ONEONE)
        pp[i] = PACK(clamp255(r + dr), clamp255(g + dg), clamp255(b + db));
      else if (OUT == O_MUL)
        pp[i] = PACK(MUL8(r, dr), MUL8(g, dg), MUL8(b, db));
      else
        pp[i] = PACK(clamp255(2 * MUL8(r, dr)), clamp255(2 * MUL8(g, dg)),
                     clamp255(2 * MUL8(b, db)));
    }
  }
}

#define ZF8_ONE(name, COL, ENV, AT, ZW, OUT) \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf8_one_t(p, s, f, COL, ENV, AT, ZW, OUT); }
ZF8_ONE(zf8_sbar, C_FLAT, E_REP_RGB, 0, 0, O_SAOMSA)
ZF8_ONE(zf8_pic, C_FLAT, E_REP_RGB, 1, 0, O_STORE)
ZF8_ONE(zf8_pica, C_NONE, E_REP_RGBA, 1, 0, O_STORE)
ZF8_ONE(zf8_fence, C_NONE, E_REP_RGBA, 1, 1, O_STORE)
ZF8_ONE(zf8_lmap, C_NONE, E_REP_RGBA, 0, 0, O_MUL2)
ZF8_ONE(zf8_lmap1, C_NONE, E_REP_RGBA, 0, 0, O_MUL)
ZF8_ONE(zf8_amod, C_SMOOTH, E_MOD_RGB, 0, 0, O_STORE)
ZF8_ONE(zf8_amod2, C_SMOOTH, E_MOD_RGB, 0, 0, O_ONEONE)
ZF8_ONE(zf8_con, C_FLAT, E_MOD_RGB, 0, 0, O_SAOMSA)
ZF8_ONE(zf8_water, C_NONE, E_REP_RGB, 0, 0, O_STORE)
ZF8_ONE(zf8_alias1, C_SMOOTH, E_COMB_MT, 0, 0, O_STORE)
ZF8_ONE(zf8_part, C_FLAT, E_MOD_RGBA, 0, 0, O_SAOMSA)
ZF8_ONE(zf8_glow, C_FLAT, E_MOD_RGB, 0, 0, O_ONEONE)

/* zf1_pick's signatures on the texenv8 stages (a view blend has no
   texture: it is zf1_pick's own) */
__attribute__((cold))
static ZStageFn zf1_pick8(const ZPipeX *x, const ZStageFn *st)
{
  int k = 0, col = C_NONE, env = E_NONE, at = 0, zw = 0, out;
  ZStageFn e;
  if (x->slot_col == &st[0]) {
    if (st[0] == zp_color_fn(1)) col = C_FLAT;
    else if (st[0] == zp_color_fn(0)) col = C_SMOOTH;
    else return NULL;
    k++;
  }
  if (x->tf[0].slot != &st[k]) return NULL;
  e = st[k + 1];
  if (e == zp_texenv8_fn(ZP_TE_REPLACE_RGB)) env = E_REP_RGB;
  else if (e == zp_texenv8_fn(ZP_TE_REPLACE_RGBA)) env = E_REP_RGBA;
  else if (e == zp_texenv8_fn(ZP_TE_MOD_RGB)) env = E_MOD_RGB;
  else if (e == zp_texenv8_fn(ZP_TE_MOD_RGBA)) env = E_MOD_RGBA;
  else if (e == zp_comb_fn(0)) {
    const ZComb *c0 = &x->cb[0];
    if (c0->zero_rgb || c0->f[1] != ZCF_NONE ||
        !prog(c0, 0, ZCF_MODULATE, ZCS_TEXTURE, ZCO_COLOR, ZCS_PRIMARY, ZCO_COLOR))
      return NULL;
    env = E_COMB_MT;
  } else {
    return NULL;
  }
  k += 2;
  if (st[k] == zp_alpha_fn(GL_GREATER)) { at = 1; k++; }
  if (st[k] == zp_zwrite_fn()) { zw = 1; k++; }
  if (st[k] == zp_out_fn(GL_ONE, GL_ZERO, 0xffff, GL_FUNC_ADD)) out = O_STORE;
  else if (st[k] == zp_out_fn(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0xffff, GL_FUNC_ADD)) out = O_SAOMSA;
  else if (st[k] == zp_out_fn(GL_ONE, GL_ONE, 0xffff, GL_FUNC_ADD)) out = O_ONEONE;
  else if (st[k] == zp_out_fn(GL_ZERO, GL_SRC_COLOR, 0xffff, GL_FUNC_ADD)) out = O_MUL;
  else if (st[k] == zp_out_fn(GL_DST_COLOR, GL_SRC_COLOR, 0xffff, GL_FUNC_ADD)) out = O_MUL2;
  else return NULL;
  if (st[k + 1]) return NULL;
  if (env == E_REP_RGBA) col = C_NONE;
#define ZF1(C, E, A, Z, O) (col == (C) && env == (E) && at == (A) && zw == (Z) && out == (O))
  if (ZF1(C_FLAT, E_REP_RGB, 0, 0, O_SAOMSA)) return zf8_sbar;
  if (ZF1(C_FLAT, E_REP_RGB, 1, 0, O_STORE)) return zf8_pic;
  if (ZF1(C_NONE, E_REP_RGBA, 1, 0, O_STORE)) return zf8_pica;
  if (ZF1(C_NONE, E_REP_RGBA, 1, 1, O_STORE)) return zf8_fence;
  if (ZF1(C_NONE, E_REP_RGBA, 0, 0, O_MUL2)) return zf8_lmap;
  if (ZF1(C_NONE, E_REP_RGBA, 0, 0, O_MUL)) return zf8_lmap1;
  if (ZF1(C_SMOOTH, E_MOD_RGB, 0, 0, O_STORE)) return zf8_amod;
  if (ZF1(C_SMOOTH, E_MOD_RGB, 0, 0, O_ONEONE)) return zf8_amod2;
  if (ZF1(C_FLAT, E_MOD_RGB, 0, 0, O_SAOMSA)) return zf8_con;
  if (ZF1(C_NONE, E_REP_RGB, 0, 0, O_STORE)) return zf8_water;
  if (ZF1(C_SMOOTH, E_COMB_MT, 0, 0, O_STORE)) return zf8_alias1;
  if (ZF1(C_FLAT, E_MOD_RGBA, 0, 0, O_SAOMSA)) return zf8_part;
  if (ZF1(C_FLAT, E_MOD_RGB, 0, 0, O_ONEONE)) return zf8_glow;
#undef ZF1
  return NULL;
}

/* ------------------------------------------------------------ selection */

/* zpf_select for a batch with a unit of 8-bit words: the filler (NULL:
   none), its kind (ZF_*) and how it runs - *direct 0 from the stage list,
   1 called by zp_run_mt_direct (both units nearest), 2 by it and by
   zp_run_lod_mt_direct (the filtered world); both mean it tests depth
   itself (ZPipe.depth passes everything) */
__attribute__((cold))
static ZStageFn zpf8_pick(GLContext *c, int *kind, int *direct)
{
  ZPipe *p = &c->pipe;
  ZPipeX *x = &c->pipex;
  ZStageFn *st = p->st;
  const ZComb *c0 = &x->cb[0], *c1 = &x->cb[1];
  const ZTexF *u0 = &x->tf[0], *u1 = &x->tf[1];
  ZStageFn store = zp_out_fn(GL_ONE, GL_ZERO, 0xffff, GL_FUNC_ADD);
  ZStageFn saomsa = zp_out_fn(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0xffff, GL_FUNC_ADD);
  int n;

  *kind = ZF_NONE;
  *direct = 0;
  for (n = 0; st[n]; n++) ;
  if (!c->tu1_on) {
    ZStageFn fn = zf1_pick8(x, st);
    if (fn == NULL) return NULL;
    *kind = ZF_ONE;
    /* the level-0 REPEAT bilinear stage, placed once for the batch: its
       sampler by the level's kind (the alpha of an L8 level only when the
       signature reads it: an alpha test, a REPLACE-alpha blend, MODULATE
       of an RGBA texture) */
    x->bil0 = 0;
    if (c->tex_filtered && !(p->xact & ZPX_TEX) && !p->clamp_s && !p->clamp_t &&
        u0->cur0 == &u0->lvl[0] && *u0->slot == zpx_stage8_u(0, TF_LINEAR0)) {
      const ZLevel *L0 = &u0->lvl[0];
      int al = fn == zf8_pica || fn == zf8_fence || fn == zf8_part;
      if (L0->k8 == TGL_ST_P8) x->bil0 = 1;
      else if (L0->k8 == TGL_ST_L8 && L0->am <= TGL_AM_A8) x->bil0 = al ? 4 : 3;
      /* (fix 2: any other kind - the W32 reference, S31GL_FILT8's 565, an
         INTENSITY / ALPHA L8 - keeps the stage: the any-kind shortcut was
         3.5 kB that no QuakeSpasm replay ran) */
    }
    return fn;
  }
  /* ZF_WORLD: [texel 0] [REPLACE RGB] [texel 1] [combiner 1] [store] */
  if (n == 5 && u0->slot == &st[0] &&
      st[1] == (u0->t8 ? zp_texenv8_fn(ZP_TE_REPLACE_RGB) : zp_texenv_fn(ZP_TE_REPLACE_RGB)) &&
      u1->slot == &st[2] && st[3] == zp_comb_fn(1) && st[4] == store &&
      !c1->zero_rgb && c1->f[1] == ZCF_NONE &&
      prog(c1, 0, ZCF_MODULATE, ZCS_PREVIOUS, ZCO_COLOR, ZCS_TEXTURE, ZCO_COLOR)) {
    const GLTexture *t0 = (c->tex_enables & 1) ? c->current_texture : c->current_texture_1d;
    const GLTexture *t1 = c->tu1_tex;
    const ZTexGeo *g = &x->g1;
    /* the inline loops: unit 1 the L8 lightmap, both units REPEAT, unit 0
       an RGB-class P8 or 565 texture (never the W32 reference) */
    int lm = u1->t8 && t1->st == TGL_ST_L8 && t1->amode != TGL_AM_ALPHA &&
             !g->clamp_s && !g->clamp_t;
    int k0 = !p->clamp_s && !p->clamp_t && t0->fmt == TGL_TEXF_RGB ?
             (t0->st == TGL_ST_P8 ? KP8 : (t0->st == TGL_ST_565 ? K565 : -1)) : -1;
    *kind = ZF_WORLD;
    if (lm && k0 >= 0 && !(p->xact & (ZPX_TEX | ZPX_TEX1)) &&
        (k0 == K565 ? *u0->slot == zp_texidx_fn(0, 0) : zpx_is_base8(*u0->slot, 0)) &&
        *u1->slot == zpx_stage8_u(1, TF_NEAREST0)) {
      /* both units nearest in level 0 for every triangle of the batch
         (the nearest stages themselves: a texture with one filter has
         that filter's stage as its tex_base) */
      int d = p->depth == zp_depth_fn(ZP_DEPTH_LEQUAL, 1) ? 0 :
              p->depth == zp_depth_fn(ZP_DEPTH_LEQUAL, 0) ? 1 :
              p->depth == zp_depth_fn(ZP_DEPTH_LESS, 1) ? 2 : -1;
      if (d >= 0) {
        static const ZStageFn nn[2][3] = {
          { zf8_wnn_p0, zf8_wnn_p1, zf8_wnn_p2 }, { zf8_wnn_c0, zf8_wnn_c1, zf8_wnn_c2 } };
        *direct = 1;
        zpf_count[ZF_WORLD_NN]++;
        return nn[k0][d];
      }
    }
    if (lm && k0 >= 0 && p->depth == zp_depth_fn(ZP_DEPTH_LEQUAL, 1) &&
        c->tu1_filtered && !(p->xact & ZPX_TEX1) && u1->kmag == u1->kmin &&
        *u1->slot == zpx_stage8_u(1, TF_LINEAR0) && u1->cur0 == &u1->lvl[0]) {
      /* (fix 2: the RGB565-on-8-bit-filter loops, S31GL_FILT8=1 only, were
         removed - 5.9 kB of .text for a non-default arm; that arm takes
         zf8_world_11 below, the same pixels) */
      if (u0->t8 && k0 == KP8) {
        *direct = 2;
        zpf_count[ZF_WORLD_X]++;
        return zf8_world_x_p8;
      }
      /* an RGB565 texture on phase 4's filter (S31GL_FILT8=0): its REPEAT,
         no-alpha stages, or nearest in level 0 */
      if (k0 == K565 && (!c->tex_filtered ? *u0->slot == zp_texidx_fn(0, 0) :
                         u0->f_bil == zpx_stage_u(0, TF_LINEAR0, 1, 0))) {
        *direct = 2;
        zpf_count[ZF_WORLD_X]++;
        return zf8_world_xs;
      }
    }
    return u0->t8 ? (u1->t8 ? zf8_world_11 : zf8_world_10) : zf8_world_01;
  }
  /* ZF_ALIAS: [colour] [texel 0] [combiner 0] [texel 1] [combiner 1]
     [SRC_ALPHA, ONE_MINUS_SRC_ALPHA / store] */
  if (n == 6 && x->slot_col == &st[0] && u0->slot == &st[1] &&
      st[2] == zp_comb_fn(0) && u1->slot == &st[3] && st[4] == zp_comb_fn(1) &&
      (st[5] == saomsa || st[5] == store) && !c0->zero_rgb && !c1->zero_rgb &&
      prog(c0, 0, ZCF_MODULATE, ZCS_TEXTURE, ZCO_COLOR, ZCS_PRIMARY, ZCO_COLOR) &&
      prog(c1, 0, ZCF_ADD, ZCS_PREVIOUS, ZCO_COLOR, ZCS_TEXTURE, ZCO_COLOR) &&
      c1->sh[0] == 0) {
    if (st[5] == store) {
      if (c0->f[1] == ZCF_NONE && c1->f[1] == ZCF_NONE) {
        *kind = ZF_ALIAS_STORE;
        return zf8_alias_st;
      }
    } else if (prog(c0, 1, ZCF_MODULATE, ZCS_TEXTURE, ZCO_ALPHA, ZCS_PREVIOUS, ZCO_ALPHA) &&
               (c1->f[1] == ZCF_NONE ||
                (prog(c1, 1, ZCF_MODULATE, ZCS_PREVIOUS, ZCO_ALPHA, ZCS_TEXTURE, ZCO_ALPHA) &&
                 c1->sh[1] == 0))) {
      *kind = ZF_ALIAS;
      return zf8_alias_bl;
    }
  }
  return NULL;
}
