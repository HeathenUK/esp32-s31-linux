/*
 * s31_tfilt_int.h - the texel arithmetic of the filtered texel stages
 * (s31_tfilter.c, phase 4 F-LIN), shared with the fused fillers
 * (zpipe_fused.c, phase 5): one definition, so a fused filler samples every
 * texel exactly as the stage it replaces. s31, MIT.
 *
 * Users define clampi() (and, for WR_G's LEVEL_VARS, a ZTexF *u) first.
 */
#ifndef S31_TFILT_INT_H
#define S31_TFILT_INT_H

/* RGB565 <-> the spread word; each field keeps 5 spare bits above it */
#define SPREAD(t) (((t) | ((t) << 16)) & 0x07E0F81Fu)
#define SMASK 0x07E0F81Fu
#define SRND 0x02008010u                /* 16 in each field: round the >> 5 */
#define LERP(a, b, w) ((((a) * (32u - (w)) + (b) * (w) + SRND) >> 5) & SMASK)
/* The LAST lerp of a filtered RGB565 texel: LERPF for a bilinear sample
   (its vertical lerp) rounds, as phase 4 did; LERPT, the blend of two
   mipmap levels (trilinear, NEAREST_MIPMAP_LINEAR), adds 12/32 of a level
   (11/32 in red) instead of 16/32 before its floor. Phase 5 P, measured:
   the QuakeSpasm trace vs Mesa (gl/bench/qslum.py, 80 frames: view
   luminance ratio, pixels exact) and the glref suite vs phase-4 final
   (gl/bench/suitecmp.py, a frame's tolerant-bad %, the phase's bar):
     LERPT bias 16 (round)  QuakeSpasm 1.046 57.1%; teapot f60 0.237 -> 0.249 WORSE
     bias 0 (floor: Mesa's  QuakeSpasm 1.000 61.3%; teapot 0.150;
       8-bit floor, band 4) fire f20 0.319 -> 0.440, ipers 0.001 -> 0.003 WORSE
     bias 10 / 11           fire f20 0.335 / 0.328 WORSE; teapot 0.203 / 0.215
     bias 12 / 13           fire 0.307 / 0.294; teapot 0.241 / 0.242 WORSE
     12, red 11 (this)      fire 0.309, teapot 0.237, ipers 0.001: none worse
   (teapot's and fire's worst frames are trilinear; the bias is a tuned
   compromise between Mesa's floor and phase 4's rounding, and it is
   fragile: one 32nd of a level moves a suite app across the bar). Only the
   8-bit filters (S31GL_FILT8=1: QuakeSpasm 1.011, 71.3% exact, +44%
   instructions) keep Mesa's 8 bits into the texture environment.
   -DS31GL_P4ARITH: both round */
#define LERPF(a, b, w) LERP(a, b, w)
#ifdef S31GL_P4ARITH
#define LERPT(a, b, w) LERP(a, b, w)
#else
#define SRNDT 0x0180580Cu               /* 12 in green and blue, 11 in red */
#define LERPT(a, b, w) ((((a) * (32u - (w)) + (b) * (w) + SRNDT) >> 5) & SMASK)
#endif
#define UNSPREAD(v) ((PIXEL)(((v) | ((v) >> 16)) & 0xffffu))
#define ALERP(a, b, w) ((a) * (32u - (w)) + (b) * (w))

/* the perspective walk of zpipe.c's ZP_TEXIDX: s/w, t/w divided by 1/w
   every 8 pixels, s and t stepped linearly in between; BODY sees si, ti
   (the texture's fixed point) and i. S is the unit's suffix on the span's
   fields: empty for unit 0 (sz, tz, dszdx, dtzdx), 1 for unit 1 (phase 5:
   sz1 ...); 1/w (fz) is the same for both */
#define TWALKB(S, BLOCK, ...)                                           \
  float fz = s->fz, sz = s->sz##S, tz = s->tz##S;                       \
  const float dfz = s->dfzdx, dsz = s->dszdx##S, dtz = s->dtzdx##S;     \
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
#define TWALK(S, ...) TWALKB(S, ;, __VA_ARGS__)

/* column and row wraps. R: GL_REPEAT (a mask). G: any wrap - the mask is
   the level's (REPEAT) or all ones (CLAMP*), then a clamp to the level */
#define WR_R(v, m, am) ((v) & (m))
#define WR_G(v, m, am) clampi((v) & (am), (m))

/* per-stage level constants; am*: the general wraps' masks (u: the unit's
   ZTexF) */
#define LEVEL_VARS(L)                                                   \
  const PIXEL *px_##L = L->pix;                                         \
  const unsigned char *pa_##L = L->alpha;                               \
  const int shs_##L = L->shs, sht_##L = L->sht, ws_##L = L->ws;         \
  const int wm_##L = L->wm, hm_##L = L->hm;                             \
  const int ams_##L = L->wm | ~u->rep_s, amt_##L = L->hm | ~u->rep_t;   \
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
#define BILERP(L, WR, C, A, ALPHA) BILERPX(L, WR, C, A, ALPHA, LERPF)
/* LAST: the vertical lerp's rounding - LERPF when this sample is the
   texel, LERP when a trilinear blend follows */
#define BILERPX(L, WR, C, A, ALPHA, LAST)                               \
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
    C = LAST(a_, c_, fv_);                                              \
    if (ALPHA) {                                                        \
      unsigned int t_ = ALERP((unsigned int)pa_##L[o0_ + c0_], (unsigned int)pa_##L[o0_ + c1_], fu_); \
      unsigned int w_ = ALERP((unsigned int)pa_##L[o1_ + c0_], (unsigned int)pa_##L[o1_ + c1_], fu_); \
      A = t_ * (32u - fv_) + w_ * fv_;                                  \
    }                                                                   \
  }

/* ---- phase 5: 8-bit texels (P8, L8 and W32 levels, s31_tex8.c) ----

   A texel is its RGBA8 word, r | g << 8 | b << 16 | a << 24. The filters
   are Mesa llvmpipe's (gl/tests/glx_prec.c bands 22, 30 and 31, matched
   exactly by this arithmetic): 8-bit weights, w = round(frac 256) in
   0..256, every lerp truncated - a + ((b - a) w >> 8) - the two rows
   first, then between them. Each lerp does two channels at once: RB8 holds
   r and b, GA8 g and a, in 16-bit fields, and a (256 - w) + b w stays below
   2^16 in each, so the word's polynomial, (a << 8) + (b - a) w, is exact
   mod 2^32 whatever the fields' differences borrow. The phase-4 565
   filters above keep their own definition (5-bit weights, rounded) */
#define RB8(w) ((w) & 0x00FF00FFu)
#define GA8(w) (((w) >> 8) & 0x00FF00FFu)
#define JOIN8(rb, ga) ((rb) | ((ga) << 8))
#define LERP8(a, b, w) (((((a) << 8) + ((b) - (a)) * (w)) >> 8) & 0x00FF00FFu)
/* both halves of two words */
#define LERP8W(a, b, w) JOIN8(LERP8(RB8(a), RB8(b), (w)), LERP8(GA8(a), GA8(b), (w)))

/* the 8-bit weight of a fixed-point coordinate's fraction (sh fraction
   bits: the top 9 of them, rounded) */
#define W8(u, sh) (((((u) << (32 - (sh))) >> 23) + 1u) >> 1)

/* texel k of an 8-bit level as its word */
static inline unsigned int t8_fetch(const ZLevel *L, unsigned int k)
{
  unsigned int l;
  switch (L->k8) {
  case TGL_ST_P8: return L->pal[L->i8[k]];
  case TGL_ST_W32: return L->w32[k];
  case TGL_ST_565: {
    /* an RGB565 texel (+ A8) as 8 bits a channel, UNPACK's expansion */
    unsigned int t = L->pix[k];
    unsigned int r = ((t >> 8) & 0xf8) | (t >> 13), g = ((t >> 3) & 0xfc) | ((t >> 9) & 3);
    unsigned int b = ((t << 3) & 0xf8) | ((t >> 2) & 7);
    return r | g << 8 | b << 16 | (L->alpha ? (unsigned int)L->alpha[k] : 255u) << 24;
  }
  default: break;
  }
  l = L->i8[k];
  switch (L->am) {
  case TGL_AM_ONE: return l * 0x010101u | 0xff000000u;
  case TGL_AM_BITS: return l * 0x010101u | (0u - ((L->alpha[k >> 3] >> (k & 7)) & 1u)) << 24;
  case TGL_AM_A8: return l * 0x010101u | (unsigned int)L->alpha[k] << 24;
  case TGL_AM_I: return l * 0x01010101u;
  default: return 0x00ffffffu | l << 24;      /* TGL_AM_ALPHA */
  }
}

/* bilinear in 8-bit level L (a ZLevel *, with LEVEL_VARS(L)): the word
   into C */
#define BILERP8(L, WR, C)                                               \
  {                                                                     \
    unsigned int u_ = si - (1u << (shs_##L - 1));                       \
    unsigned int v_ = ti - (1u << (sht_##L - 1));                       \
    int c0_ = (int)u_ >> shs_##L, r0_ = (int)v_ >> sht_##L;             \
    int c1_ = WR(c0_ + 1, wm_##L, ams_##L), r1_ = WR(r0_ + 1, hm_##L, amt_##L); \
    unsigned int fu_ = W8(u_, shs_##L), fv_ = W8(v_, sht_##L);          \
    unsigned int o0_, o1_, a_, b_, c_, d_;                              \
    c0_ = WR(c0_, wm_##L, ams_##L); r0_ = WR(r0_, hm_##L, amt_##L);     \
    o0_ = (unsigned int)r0_ << ws_##L; o1_ = (unsigned int)r1_ << ws_##L; \
    a_ = t8_fetch(L, o0_ + (unsigned int)c0_);                          \
    b_ = t8_fetch(L, o0_ + (unsigned int)c1_);                          \
    c_ = t8_fetch(L, o1_ + (unsigned int)c0_);                          \
    d_ = t8_fetch(L, o1_ + (unsigned int)c1_);                          \
    a_ = LERP8W(a_, b_, fu_);                                           \
    c_ = LERP8W(c_, d_, fu_);                                           \
    C = LERP8W(a_, c_, fv_);                                            \
  }

#endif
