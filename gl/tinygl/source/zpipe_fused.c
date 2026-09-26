/*
 * zpipe_fused.c - phase 5 O1: fused fillers for the multitexture batches
 * of QuakeSpasm 0.96.3 (and any application that sets the same state).
 * s31, MIT.
 *
 * With texture units 0 and 1 on, the general path (zpipe.c) runs a list
 * of stages over each 32-pixel chunk: the colour, each unit's texel stage,
 * each unit's texture environment (unit 1's, and unit 0's under GL_COMBINE,
 * through the generic combiner zc_comb, which builds argument arrays per
 * source and operand), then the blend and store - every stage a pass over
 * the chunk's arrays. For the two signatures QuakeSpasm draws nearly
 * everything with, gl_build_pipe hands the chunk to one function here
 * instead (zpf_select):
 *
 *  ZF_WORLD  r_world.c "case 1": unit 0 GL_REPLACE (or DECAL) of an RGB
 *            texture, unit 1 GL_COMBINE with COMBINE_RGB MODULATE of
 *            (PREVIOUS, TEXTURE), any RGB_SCALE - or GL_MODULATE, its
 *            program - no blend: the lightmapped world in one pass.
 *  ZF_ALIAS  r_alias.c "case 1": the colour, unit 0 COMBINE_RGB MODULATE of
 *            (TEXTURE, PRIMARY_COLOR) with its scale, COMBINE_ALPHA
 *            MODULATE of (TEXTURE, PREVIOUS); unit 1 GL_ADD; blend
 *            SRC_ALPHA / ONE_MINUS_SRC_ALPHA (or none: ZF_ALIAS_STORE).
 *
 * Bit-identical by construction. The fused function runs the batch's own
 * colour and texel stages - whichever the batch placed and the triangle or
 * chunk chose: nearest, bilinear, trilinear, any wrap, perspective colour -
 * exactly as the general path runs them, and then ONE loop that computes,
 * per pixel, the same integer arithmetic the texenv, combiner and output
 * stages compute (zpipe_int.h's MUL8, clamp255, UNPACK, PACK: one
 * definition for both). What it removes is the combiner's argument arrays
 * and four passes over the chunk. The depth stage (ZPipe.depth) runs
 * before, as always. gl/tests/fused_test.c renders every signature both
 * ways (S31GL_FUSED=0 is the general path) and requires identical colour
 * and depth, pixel for pixel.
 *
 * The general list stays in ZPipeX.gen_st: the per-triangle choices
 * (s31_tfilter.c zpx_apply, the perspective colour) keep placing their
 * stages there through the slot pointers, and the pixel paths (s31_draw.c)
 * build their own list from it.
 */
#include <string.h>
#include "zgl.h"
#include "zpipe.h"
#include "zpipe_int.h"

static inline int clampi(int v, int hi)
{
  return v < 0 ? 0 : (v > hi ? hi : v);
}
#include "s31_tfilt_int.h"   /* the filtered stages' texel arithmetic */

enum { ZF_NONE, ZF_WORLD, ZF_ALIAS, ZF_ALIAS_STORE, ZF_WORLD_NN, ZF_ONE, ZF_WORLD_X,
       ZF_NKIND };

/* the depth stage of a world batch whose filler tests depth itself */
static int zf_pass(const ZSpan *s, ZFrag *f)
{
  (void)s;
  return f->n;
}

/* batches per kind (s31gl_fused_stats; cold: once per gl_build_pipe) */
static unsigned int zpf_count[8];

__attribute__((cold))
void tgl_fused_stats(unsigned int out[8])
{
  int k;
  for (k = 0; k < 8; k++) { out[k] = zpf_count[k]; zpf_count[k] = 0; }
}

/* ------------------------------------------------------------ fused chunks */

/* unit 0 REPLACE of an RGB texture, unit 1 MODULATE (PREVIOUS, TEXTURE) *
   2^sh, stored: ze_replace_rgb, zc_comb (unit 1), zo_store.
   Both texels are RGB565 and the result is stored as RGB565, and UNPACK
   expands a field exactly (5 or 6 bits, replicated), so each output field
   is a function of the two input fields alone: the stored field of
   MULS8(e(a), e(b), sh). ZPipeX.wtab holds it for every pair -
   32 x 32 bytes for red and blue, 64 x 64 for green (zf_world_tables,
   made from those very macros) - so a pixel is two texel loads, three
   table loads and the packing: the same bits as the stages' arithmetic,
   about a third of the instructions */
/* one output pixel from the two texels (ZPipeX.wtab) */
#define ZF_WPIX(a, b) ((PIXEL)(((unsigned int)trb[(((a) >> 6) & 0x3e0) | ((b) >> 11)] << 11) | \
    ((unsigned int)tg[(((a) << 1) & 0xfc0) | (((b) >> 5) & 63)] << 5) |                      \
    (unsigned int)trb[(((a) & 31) << 5) | ((b) & 31)]))

/* Both units nearest with GL_REPEAT (their texel stages are zt_rr and
   zt1_rr), the depth test inline: ZPipe.depth is zf_pass, this does what
   gen_depth (D: 0 zdw_lequal, 1 zd_lequal, 2 zdw_less), zt_rr, zt1_rr
   and the loop below do, in one pass - the walks with the stages' own
   expressions (zpipe.c ZP_TEXIDX, ZP_TEXIDX1: s/w, t/w divided by 1/w
   every 8 pixels, stepped between), the depth as ZP_DEPTH(_W) */
static inline __attribute__((always_inline))
void zf_world_nn_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int D)
{
  const ZPipeX *x = p->x;
  const ZTexGeo *g = &x->g1;
  const PIXEL *t0 = p->tex, *t1 = p->tex1;
  const unsigned char *trb = x->wtab, *tg = x->wtab + 1024;
  const unsigned int tm0 = p->tmask, sm0 = p->smask, tm1 = g->tmask, sm1 = g->smask;
  const int fb0 = p->fbits, fb1 = g->fbits;
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  float fz = s->fz, sz = s->sz, tz = s->tz, sz1 = s->sz1, tz1 = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  const float dsz1 = s->dszdx1, dtz1 = s->dtzdx1;
  int i = 0, e, n = f->n, ok;
  while (i < n) {
    float zinv = 1.0f / fz, ss = sz * zinv, tt = tz * zinv;
    int si = (int)ss, ti = (int)tt;
    int dsi = (int)((dsz - ss * dfz) * zinv);
    int dti = (int)((dtz - tt * dfz) * zinv);
    float zinv1 = 1.0f / fz, ss1 = sz1 * zinv1, tt1 = tz1 * zinv1;
    int si1 = (int)ss1, ti1 = (int)tt1;
    int dsi1 = (int)((dsz1 - ss1 * dfz) * zinv1);
    int dti1 = (int)((dtz1 - tt1 * dfz) * zinv1);
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;
      ok = D == 2 ? zz > pz[i] : zz >= pz[i];
      if (ok) {
        unsigned int a = t0[(((unsigned int)ti & tm0) | ((unsigned int)si & sm0)) >> fb0];
        unsigned int b = t1[(((unsigned int)ti1 & tm1) | ((unsigned int)si1 & sm1)) >> fb1];
        if (D != 1) pz[i] = (unsigned short)zz;
        pp[i] = ZF_WPIX(a, b);
      }
      z += s->dzdx;
      si += dsi; ti += dti; si1 += dsi1; ti1 += dti1;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
    sz1 += 8.0f * dsz1; tz1 += 8.0f * dtz1;
  }
}

static void zf_world_nn0(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_world_nn_t(p, s, f, 0); }
static void zf_world_nn1(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_world_nn_t(p, s, f, 1); }
static void zf_world_nn2(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_world_nn_t(p, s, f, 2); }

/* any other texel stages (filters, wraps; the level may change per
   triangle or block): the stages, then the texels' combination */
static void zf_world(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZPipeX *x = p->x;
  const PIXEL *t0 = p->tex, *t1 = p->tex1;
  const unsigned int *i0 = f->idx, *i1 = f->idx1;
  const unsigned char *trb = x->wtab, *tg = x->wtab + 1024;
  PIXEL *pp = s->pp;
  int i, n = f->n;

  (*x->tf[0].slot)(p, s, f);
  (*x->tf[1].slot)(p, s, f);
  for (i = 0; i < n; i++) {
    unsigned int a, b;
    if (!f->m[i]) continue;
    a = t0[i0[i]];
    b = t1[i1[i]];
    pp[i] = ZF_WPIX(a, b);
  }
}

/* ------------------------------------------------------------ filtered world */

/* Phase 5 O2: ZF_WORLD with filtered textures, the depth test and both
   units' sampling in one pass. The per-triangle and per-block level
   choices (s31_tfilter.c zpx_tri / zp_run_lod_mt) still place unit 0's
   texel stage in its slot (ZPipeX.gen_st) and its level(s) in ZTexF.cur0 /
   cur1 / twb; zf_world_x reads which stage that is, once per chunk, and
   runs the loop made for it: unit 0 nearest in level 0 (its texidx stage)
   or in level d (zx_near), bilinear in d (zx_bil), trilinear (zx_tri) or
   nearest-in-two-levels (zx_ntri), all GL_REPEAT; unit 1 bilinear in level
   0 (the lightmap: GL_LINEAR, zx1_bil). Each texel is computed with the
   stages' own macros (s31_tfilt_int.h) and walk (TWALKB's anchors every 8
   pixels), and only for the fragments that pass the depth test, which is
   zdw_lequal's own arithmetic (the only depth state this runs in).
   Two exact rewrites of the stages' arithmetic:
   - a bilinear sample whose 2x2 texel square is the previous pixel's (a
     magnified texture: the 1/16-density lightmap, a near wall) reuses the
     square's spread texels and, for the first (horizontal) lerps, 32 a +
     SRND and b - a: a (32 - w) + b w + SRND == 32 a + SRND + (b - a) w in
     unsigned 32-bit arithmetic (the same polynomial mod 2^32, and the true
     value is below 2^32 - s31_tfilt_int.h), one multiply for two;
   - the product tables are indexed from the spread word's fields
     directly (they are the 565 fields UNSPREAD would pack).
   Anything else (another wrap, unit 1 not bilinear, a stage this does not
   know) runs the general zf_world after the batch's own depth stage. */
enum { ZK_NONE, ZK_BASE, ZK_NEAR, ZK_BIL, ZK_TRI, ZK_NTRI };

/* one level's constants for the loops, REPEAT only (WR_R) */
#define LVR(L, Z)                                                       \
  const PIXEL *px_##L = (Z)->pix;                                       \
  const int shs_##L = (Z)->shs, sht_##L = (Z)->sht, ws_##L = (Z)->ws;   \
  const int wm_##L = (Z)->wm, hm_##L = (Z)->hm;                         \
  const unsigned int hs_##L = 1u << (shs_##L - 1), ht_##L = 1u << (sht_##L - 1); \
  const int ls_##L = 32 - shs_##L, lt_##L = 32 - sht_##L;               \
  const unsigned char *pa_##L = NULL;       /* (BILERP's, unread) */    \
  int qc_##L = 0, qr_##L = 0x7fffffff;                                  \
  unsigned int qa_##L = 0, qb_##L = 0, qe_##L = 0, qd_##L = 0;          \
  (void)px_##L; (void)ws_##L; (void)hs_##L; (void)ht_##L; (void)ls_##L; \
  (void)lt_##L; (void)qc_##L; (void)qr_##L; (void)qa_##L; (void)qb_##L; \
  (void)qe_##L; (void)qd_##L; (void)pa_##L;

/* BILERP(L, WR_R, C, -, 0) at (SI, TI), exactly, with the square cached
   (BILQX: BILERPX's LAST) */
#define BILQ(L, SI, TI, C) BILQX(L, SI, TI, C, LERPF)
#define BILQX(L, SI, TI, C, LAST)                                       \
  {                                                                     \
    unsigned int u_ = (SI) - hs_##L, v_ = (TI) - ht_##L;                \
    int c0_ = (int)u_ >> shs_##L, r0_ = (int)v_ >> sht_##L;             \
    unsigned int fu_ = (((u_ << ls_##L) >> 26) + 1) >> 1;               \
    unsigned int fv_ = (((v_ << lt_##L) >> 26) + 1) >> 1;               \
    unsigned int t_, b_;                                                \
    if (c0_ != qc_##L || r0_ != qr_##L) {                               \
      int c1_ = (c0_ + 1) & wm_##L, r1_ = (r0_ + 1) & hm_##L;           \
      const PIXEL *w0_ = px_##L + ((r0_ & hm_##L) << ws_##L);           \
      const PIXEL *w1_ = px_##L + (r1_ << ws_##L);                      \
      unsigned int a_ = SPREAD((unsigned int)w0_[c0_ & wm_##L]);        \
      unsigned int e_ = SPREAD((unsigned int)w1_[c0_ & wm_##L]);        \
      qb_##L = SPREAD((unsigned int)w0_[c1_]) - a_;                     \
      qd_##L = SPREAD((unsigned int)w1_[c1_]) - e_;                     \
      qa_##L = (a_ << 5) + SRND;                                        \
      qe_##L = (e_ << 5) + SRND;                                        \
      qc_##L = c0_; qr_##L = r0_;                                       \
    }                                                                   \
    t_ = ((qa_##L + qb_##L * fu_) >> 5) & SMASK;                        \
    b_ = ((qe_##L + qd_##L * fu_) >> 5) & SMASK;                        \
    C = LAST(t_, b_, fv_);                                              \
  }

/* nearest in level L (NEAR_IDX with WR_R) */
#define NEARR(L, SI, TI)                                                \
  px_##L[((unsigned int)(((int)(TI) >> sht_##L) & hm_##L) << ws_##L) |  \
         (unsigned int)(((int)(SI) >> shs_##L) & wm_##L)]

/* the product-table indices of a spread word (unit 0: red << 5, green << 6,
   blue << 5; unit 1: red, green, blue) - ZF_WPIX's of its UNSPREAD */
#define ZF_WSPIX(a, b) ((PIXEL)(((unsigned int)trb[(((a) >> 6) & 0x3e0) | (((b) >> 11) & 31)] << 11) | \
    ((unsigned int)tg[(((a) >> 15) & 0xfc0) | ((b) >> 21)] << 5) |                              \
    (unsigned int)trb[(((a) & 31) << 5) | ((b) & 31)]))

/* the same with unit 0's texel as RGB565 (a nearest sample) */
#define ZF_WMIX(a, b) ((PIXEL)(((unsigned int)trb[(((a) >> 6) & 0x3e0) | (((b) >> 11) & 31)] << 11) | \
    ((unsigned int)tg[(((a) << 1) & 0xfc0) | ((b) >> 21)] << 5) |                               \
    (unsigned int)trb[(((a) & 31) << 5) | ((b) & 31)]))

/* Unit 0 bilinear or trilinear: two passes over the chunk - pass 1 the
   depth test and write and unit 1's sample (into ZFrag.idx1; ~0 for a
   fragment that failed), pass 2 unit 0's sample(s), the product and the
   store - so no loop holds more than two levels' constants (on the
   QuakeSpasm proxy one pass for everything was 1.5% worse, three passes
   2.5%). Unit 0 nearest (level 0, d, or two levels blended) takes one
   pass (zf_wx_1p: 3-4% better than two for those modes). The walks are
   TWALKB's. */
#define ZF_DEAD 0xffffffffu

#define ZF_WALK_HEAD(SZ, TZ, DSZ, DTZ, SI, TI, DSI, DTI)                \
  float ss##SI = SZ * zinv, tt##SI = TZ * zinv;                         \
  unsigned int SI = (unsigned int)(int)ss##SI, TI = (unsigned int)(int)tt##SI; \
  unsigned int DSI = (unsigned int)(int)((DSZ - ss##SI * dfz) * zinv);  \
  unsigned int DTI = (unsigned int)(int)((DTZ - tt##SI * dfz) * zinv);

static inline __attribute__((always_inline))
void zf_wx_p1(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZPipeX *x = p->x;
  unsigned int *bb = f->idx1;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  const int dzdx = s->dzdx;
  float fz = s->fz, sz = s->sz1, tz = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx1, dtz = s->dtzdx1;
  int i = 0, e, n = f->n;
  LVR(M, x->tf[1].cur0)
  while (i < n) {
    float zinv = 1.0f / fz;
    ZF_WALK_HEAD(sz, tz, dsz, dtz, si, ti, dsi, dti)
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
      unsigned int b = ZF_DEAD;
      zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;
      if (zz >= pz[i]) {
        BILQ(M, si, ti, b)
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
void zf_wx_p2(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA,
              const int K)
{
  const ZPipeX *x = p->x;
  const ZTexF *u0 = &x->tf[0];
  const unsigned char *trb = x->wtab, *tg = x->wtab + 1024;
  const unsigned int *bb = f->idx1;
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
        if (K == ZK_BIL) {
          BILQ(A, si, ti, a)
        } else {
          unsigned int a1;
          /* level d is minified (a texel a pixel or less): its square is
             seldom the last pixel's (86% missed), so it is sampled
             uncached - measured 1.6% better than cached */
          BILERPX(A, WR_R, a, a, 0, LERP)
          BILQX(B, si, ti, a1, LERP)
          a = LERPT(a, a1, tw);
        }
        pp[i] = ZF_WSPIX(a, b);
      }
      si += dsi; ti += dti;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
  }
}

/* unit 0 nearest (level 0 or d): one pass - its seven constants fit
   beside unit 1's without spilling */
static inline __attribute__((always_inline))
void zf_wx_1p(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA, const int K)
{
  const ZPipeX *x = p->x;
  const unsigned char *trb = x->wtab, *tg = x->wtab + 1024;
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  unsigned int z = s->z, zz;
  const int dzdx = s->dzdx;
  float fz = s->fz, sz = s->sz, tz = s->tz, sz1 = s->sz1, tz1 = s->tz1;
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;
  const float dsz1 = s->dszdx1, dtz1 = s->dtzdx1;
  int i = 0, e, n = f->n;
  LVR(M, x->tf[1].cur0)
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
        unsigned int a, b;
        BILQ(M, si1, ti1, b)
        pz[i] = (unsigned short)zz;
        if (K == ZK_NTRI) {
          a = LERPT(SPREAD((unsigned int)NEARR(A, si, ti)),
                   SPREAD((unsigned int)NEARR(B, si, ti)), tw);
          pp[i] = ZF_WSPIX(a, b);
        } else {
          a = NEARR(A, si, ti);
          pp[i] = ZF_WMIX(a, b);
        }
      }
      z += dzdx;
      si += dsi; ti += dti; si1 += dsi1; ti1 += dti1;
    }
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;
    sz1 += 8.0f * dsz1; tz1 += 8.0f * dtz1;
  }
}

static inline __attribute__((always_inline))
void zf_wx_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int K)
{
  const ZLevel *LA = p->x->tf[0].cur0;
  if (K == ZK_NTRI) {
    zf_wx_1p(p, s, f, LA, K);
    return;
  }
  zf_wx_p1(p, s, f);
  zf_wx_p2(p, s, f, LA, K);
}

/* (each kind out of line: a chunk runs one of them) */
__attribute__((noinline))
static void zf_wx_near(const ZPipe *p, const ZSpan *s, ZFrag *f, const ZLevel *LA)
{
  zf_wx_1p(p, s, f, LA, ZK_NEAR);
}
__attribute__((noinline))
static void zf_wx_bil(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_wx_t(p, s, f, ZK_BIL); }
__attribute__((noinline))
static void zf_wx_tri(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_wx_t(p, s, f, ZK_TRI); }
__attribute__((noinline))
static void zf_wx_ntri(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_wx_t(p, s, f, ZK_NTRI); }
static void zf_world(const ZPipe *p, const ZSpan *s, ZFrag *f);

/* the chunk's unit 0 texel stage picks the loop */
static void zf_world_x(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZPipeX *x = p->x;
  const ZTexF *u0 = &x->tf[0];
  ZStageFn t = *u0->slot;
  if (t == u0->f_tri) zf_wx_tri(p, s, f);
  else if (t == u0->f_bil) zf_wx_bil(p, s, f);
  else if (t == u0->f_near) zf_wx_near(p, s, f, u0->cur0);
  else if (t == u0->f_ntri) zf_wx_ntri(p, s, f);
  else if (t == zp_texidx_fn(0, 0)) {
    /* level 0 as the texidx stage addresses it (IDX_RR is NEAR_IDX of
       these: ((t & tmask) | (s & smask)) >> F): the same loop as a level
       d's nearest */
    ZLevel lb;
    lb.pix = p->tex;
    lb.shs = p->fbits; lb.sht = p->fbits + p->ws; lb.ws = p->ws;
    lb.wm = p->wmax; lb.hm = p->hmax;
    zf_wx_near(p, s, f, &lb);
  } else if (x->gen_depth(s, f)) zf_world(p, s, f);
}

/* ZF_WORLD's tables for scale 2^sh (cold: at a batch whose scale differs
   from the tables'); 0 without memory (the batch keeps the general list) */
__attribute__((cold))
static int zf_world_tables(ZPipeX *x, int sh)
{
  int a, b;
  if (x->wtab && x->wtab_sh == sh) return 1;
  if (x->wtab == NULL) {
    x->wtab = gl_malloc(1024 + 4096);
    if (x->wtab == NULL) return 0;
  }
  for (a = 0; a < 32; a++)
    for (b = 0; b < 32; b++) {
      /* the red (and blue) field: UNPACK's expansion, the combiner, PACK */
      int e0 = (a << 3) | (a >> 2), e1 = (b << 3) | (b >> 2);
      x->wtab[a * 32 + b] = (unsigned char)(PACK(MULS8(e0, e1, sh), 0, 0) >> 11);
    }
  for (a = 0; a < 64; a++)
    for (b = 0; b < 64; b++) {
      int e0 = (a << 2) | (a >> 4), e1 = (b << 2) | (b >> 4);
      x->wtab[1024 + a * 64 + b] = (unsigned char)(PACK(0, MULS8(e0, e1, sh), 0) >> 5);
    }
  x->wtab_sh = sh;
  return 1;
}

/* the alias chunk: A0 unit 0's texture has an alpha plane, A1 unit 1's
   alpha modulates (GL_ADD of an alpha format), BL blend SRC_ALPHA /
   ONE_MINUS_SRC_ALPHA (else stored). The colour stage, zc_comb (unit 0),
   zc_comb (unit 1) with its ADD program, zo_sa_omsa / zo_store */
static inline __attribute__((always_inline))
void zf_alias_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int A0,
                const int A1, const int BL)
{
  const ZPipeX *x = p->x;
  const PIXEL *t0 = p->tex, *t1 = p->tex1;
  const unsigned char *a0p = p->talpha, *a1p = p->talpha1;
  PIXEL *pp = s->pp;
  const int sh = x->cb[0].sh[0], sha = x->cb[0].sh[1];
  int i, n = f->n;

  (*x->slot_col)(p, s, f);
  (*x->tf[0].slot)(p, s, f);
  (*x->tf[1].slot)(p, s, f);
  for (i = 0; i < n; i++) {
    int tr, tg, tb, r, g, b, a;
    unsigned int k0, k1;
    if (!f->m[i]) continue;
    k0 = f->idx[i]; k1 = f->idx1[i];
    /* unit 0: MODULATE (TEXTURE, PRIMARY_COLOR) and, for the alpha,
       MODULATE (TEXTURE, PREVIOUS): the texel first, as zc_comb orders the
       arguments */
    UNPACK(t0[k0], tr, tg, tb);
    r = MULS8(tr, f->r[i], sh);
    g = MULS8(tg, f->g[i], sh);
    b = MULS8(tb, f->b[i], sh);
    if (BL) a = MULS8(A0 ? a0p[k0] : 255, f->a[i], sha);
    else a = 0;
    /* unit 1: ADD (PREVIOUS, TEXTURE); the alpha MODULATE (PREVIOUS,
       TEXTURE) when its texture has one */
    UNPACK(t1[k1], tr, tg, tb);
    r = clamp255(r + tr);
    g = clamp255(g + tg);
    b = clamp255(b + tb);
    if (BL && A1) a = clamp255(MUL8(a, a1p[k1]));
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

static void zf_alias_00(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_alias_t(p, s, f, 0, 0, 1); }
static void zf_alias_10(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_alias_t(p, s, f, 1, 0, 1); }
static void zf_alias_01(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_alias_t(p, s, f, 0, 1, 1); }
static void zf_alias_11(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_alias_t(p, s, f, 1, 1, 1); }
static void zf_alias_st(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_alias_t(p, s, f, 0, 0, 0); }

/* part `part` of a combiner program is f of (s0, o0), (s1, o1) */
__attribute__((cold))
static int prog(const ZComb *cb, int part, int f, int s0, int o0, int s1, int o1)
{
  return cb->f[part] == f && cb->src[part][0] == s0 && cb->op[part][0] == o0 &&
         cb->src[part][1] == s1 && cb->op[part][1] == o1;
}

/* ------------------------------------------------------------ one unit */

/* Phase 5 O2: the one-texture (or untextured) batches QuakeSpasm still
   draws through the general stage list, fused like the world and alias
   ones: the batch's own colour stage (smooth or perspective-corrected; a
   flat colour is read from ZPipe.flat, as zc_flat copies it) and texel
   stage (any filter and wrap) run as before, and then ONE loop computes
   what the texenv, alpha test, depth write and blend stages compute, with
   their arithmetic (zpipe_int.h). The signatures (the stage list after
   the colour and texel stages):
     REPLACE RGB  + SRC_ALPHA/ONE_MINUS_SRC_ALPHA   the status bar
     (untextured) + SRC_ALPHA/ONE_MINUS_SRC_ALPHA   the view blend
     REPLACE RGB  + alpha GREATER + store           HUD pictures
     REPLACE RGBA + alpha GREATER (+ depth write) + store
                                                   HUD pictures, characters,
                                                   fence textures
     MODULATE RGB + SRC_ALPHA/ONE_MINUS_SRC_ALPHA   the console, menus
     REPLACE RGB  + store                           water (no colour stage)
     COMBINE MODULATE (TEXTURE, PRIMARY) x 2^s + store
                                                   alias models, no fullbright
     MODULATE RGBA + SRC_ALPHA/ONE_MINUS_SRC_ALPHA  particles
     MODULATE RGB + ONE/ONE                         the fullbright glow pass
   and, for a Quake without multitexture (QuakeSpasm's "case 3" passes,
   S31GL_MTEX=0, or another GL Quake):
     REPLACE RGBA + DST_COLOR/SRC_COLOR (or ZERO/SRC_COLOR)  the lightmaps
     MODULATE RGB, smooth + store / ONE/ONE         the alias passes
   With blending SRC_ALPHA / ONE_MINUS_SRC_ALPHA and a flat alpha of 255,
   MUL8(c, 255) + MUL8(d, 0) is c and PACK(UNPACK(t)) is t, so an opaque
   status bar is the texel stored. */
enum { C_NONE, C_FLAT, C_SMOOTH };
enum { E_NONE, E_REP_RGB, E_REP_RGBA, E_MOD_RGB, E_MOD_RGBA, E_COMB_MT };
enum { O_STORE, O_SAOMSA, O_ONEONE, O_MUL, O_MUL2 };

/* zx_bil_r / zx_bil_ra (s31_tfilter.c: BILERP in level 0, REPEAT) with
   exact shortcuts for a weight of 0 or 32. BILERP lerps the two rows by
   fu, then the rows by fv; LERP(x, y, 0) is x and LERP(x, y, 32) is y for
   any spread x, y ((32 v + 16) >> 5 is v in every field), and the alpha's
   t (32 - fv) + w fv is 32 t or 32 w. So with fv 0 or 32 the sample is one
   row's lerp, with fu 0 or 32 one column's, with both one texel - the
   same bits, from half or a quarter of the texel reads. Pictures drawn
   near texel for texel hit them: QuakeSpasm's status bar at scale 1 is
   1:1 vertically (fv is 0 for 88% of its pixels) and 320 pixels from 256
   texels across (GL_MAX_TEXTURE_SIZE: fu is 0 or 16 for a third). Writes
   what the stage writes (ZTexF.ftex, falpha, idx = i) */
static inline __attribute__((always_inline))
void zf_bil0_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int ALPHA)
{
  ZPipeX *x = p->x;
  ZTexF *u = &x->tf[0];
  const ZLevel *L0 = u->cur0;
  PIXEL *ft = u->ftex;
  unsigned char *fa = u->falpha;
  LEVEL_VARS(L0)
  (void)fa;
  TWALK(,
    unsigned int c; unsigned int a = 0;
    unsigned int uq_ = si - (1u << (shs_L0 - 1)), vq_ = ti - (1u << (sht_L0 - 1));
    unsigned int fu0_ = (((uq_ << (32 - shs_L0)) >> 26) + 1) >> 1;
    unsigned int fv0_ = (((vq_ << (32 - sht_L0)) >> 26) + 1) >> 1;
    if (!(fv0_ & 31) || !(fu0_ & 31)) {
      int c0q_ = (int)uq_ >> shs_L0, r0q_ = (int)vq_ >> sht_L0;
      if (!(fv0_ & 31)) {
        /* one row (r0, or r0 + 1 for fv 32), lerped by fu */
        const int oq_ = ((r0q_ + (int)(fv0_ >> 5)) & hm_L0) << ws_L0;
        const int ca_ = c0q_ & wm_L0, cb_ = (c0q_ + 1) & wm_L0;
        c = LERP(SPREAD((unsigned int)px_L0[oq_ + ca_]),
                 SPREAD((unsigned int)px_L0[oq_ + cb_]), fu0_);
        if (ALPHA)
          a = ALERP((unsigned int)pa_L0[oq_ + ca_], (unsigned int)pa_L0[oq_ + cb_], fu0_) * 32u;
      } else {
        /* one column (c0, or c0 + 1 for fu 32), lerped by fv */
        const int cq_ = (c0q_ + (int)(fu0_ >> 5)) & wm_L0;
        const int oa_ = (r0q_ & hm_L0) << ws_L0, ob_ = ((r0q_ + 1) & hm_L0) << ws_L0;
        c = LERPF(SPREAD((unsigned int)px_L0[oa_ + cq_]),
                 SPREAD((unsigned int)px_L0[ob_ + cq_]), fv0_);
        if (ALPHA)
          a = ALERP((unsigned int)pa_L0[oa_ + cq_] * 32u, (unsigned int)pa_L0[ob_ + cq_] * 32u, fv0_);
      }
    } else {
      BILERP(L0, WR_R, c, a, ALPHA)
    }
    ft[i] = UNSPREAD(c);
    if (ALPHA) fa[i] = (unsigned char)((a + 512u) >> 10);
    f->idx[i] = (unsigned int)i;
  )
}
static void zf_bil0_r(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_bil0_t(p, s, f, 0); }
static void zf_bil0_ra(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_bil0_t(p, s, f, 1); }

/* the blend tables for alpha a: [0..31] MUL8(e5(v), a), [32..63]
   MUL8(e5(v), 255 - a), [64..127] MUL8(e6(v), a), [128..191]
   MUL8(e6(v), 255 - a), e5 / e6 UNPACK's expansions (cold: at a chunk
   whose alpha differs from the tables'); NULL without memory */
__attribute__((noinline))
static const unsigned char *zf_btab(ZPipeX *x, int a)
{
  int v;
  if (x->btab == NULL) {
    x->btab = gl_malloc(192);
    if (x->btab == NULL) return NULL;
  } else if (x->btab_a == a) {
    return x->btab;
  }
  for (v = 0; v < 32; v++) {
    int e = (v << 3) | (v >> 2);
    x->btab[v] = (unsigned char)MUL8(e, a);
    x->btab[32 + v] = (unsigned char)MUL8(e, 255 - a);
  }
  for (v = 0; v < 64; v++) {
    int e = (v << 2) | (v >> 4);
    x->btab[64 + v] = (unsigned char)MUL8(e, a);
    x->btab[128 + v] = (unsigned char)MUL8(e, 255 - a);
  }
  x->btab_a = a;
  return x->btab;
}

static inline __attribute__((always_inline))
void zf_one_t(const ZPipe *p, const ZSpan *s, ZFrag *f, const int COL, const int ENV,
              const int AT, const int ZW, const int OUT)
{
  const ZPipeX *x = p->x;
  PIXEL *pp = s->pp;
  unsigned short *pz = s->pz;
  const unsigned int *idx = f->idx;
  const int n = f->n, aref = p->aref, sh = x->cb[0].sh[0];
  const int fr = p->flat[0], fg = p->flat[1], fb = p->flat[2], fa = p->flat[3];
  const PIXEL *tex;
  const unsigned char *tal;
  int i;
  (void)pz; (void)aref; (void)sh; (void)fr; (void)fg; (void)fb; (void)fa;
  if (COL == C_SMOOTH) (*x->slot_col)(p, s, f);
  if (ENV != E_NONE) {
    /* the texels the stage would make, for the REPLACE signatures (the
       pictures; bil0 2: the stage filters the alpha too, which REPLACE of
       RGBA reads). The MODULATE ones (console, particles, glow) are
       seldom drawn texel for texel: the shortcut's test cost them more
       than it saved (measured) */
    if (ENV == E_REP_RGBA && x->bil0 == 2) zf_bil0_ra(p, s, f);
    else if (ENV == E_REP_RGB && x->bil0) zf_bil0_r(p, s, f);
    else (*x->tf[0].slot)(p, s, f);
  }
  /* (after the texel stage: a filtered one points these at its chunk) */
  tex = p->tex;
  tal = p->talpha;
  (void)tex; (void)tal;
  if (COL == C_FLAT && (ENV == E_REP_RGB || ENV == E_NONE) && !AT && !ZW && OUT == O_SAOMSA) {
    /* a flat alpha, a texel or flat colour and SRC_ALPHA /
       ONE_MINUS_SRC_ALPHA: opaque, the texel is stored as it is; else
       each field is MUL8(e(src), a) + MUL8(e(dst), 255 - a), from the
       tables (zf_btab: the same MUL8 of the same expansions) */
    const unsigned char *bt;
    if (ENV == E_REP_RGB && fa == 255) {
      for (i = 0; i < n; i++)
        if (f->m[i]) pp[i] = tex[idx[i]];
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
          t = tex[idx[i]];
          pp[i] = PACK(bt[t >> 11] + bt[32 + (d >> 11)],
                       bt[64 + ((t >> 5) & 63)] + bt[128 + ((d >> 5) & 63)],
                       bt[t & 31] + bt[32 + (d & 31)]);
        }
      }
      return;
    }
  }
  for (i = 0; i < n; i++) {
    int r = 0, g = 0, b = 0, a = 0, tr, tg, tb;
    if (!f->m[i]) continue;
    if (COL == C_FLAT) { r = fr; g = fg; b = fb; a = fa; }
    else if (COL == C_SMOOTH) { r = f->r[i]; g = f->g[i]; b = f->b[i]; a = f->a[i]; }
    switch (ENV) {
    case E_REP_RGB:
      UNPACK(tex[idx[i]], r, g, b);
      break;
    case E_REP_RGBA:
      UNPACK(tex[idx[i]], r, g, b);
      a = tal[idx[i]];
      break;
    case E_MOD_RGB:
      UNPACK(tex[idx[i]], tr, tg, tb);
      r = MUL8(r, tr); g = MUL8(g, tg); b = MUL8(b, tb);
      break;
    case E_MOD_RGBA:
      UNPACK(tex[idx[i]], tr, tg, tb);
      r = MUL8(r, tr); g = MUL8(g, tg); b = MUL8(b, tb);
      a = MUL8(a, tal[idx[i]]);
      break;
    case E_COMB_MT:
      /* zc_comb's MODULATE of (TEXTURE, PRIMARY_COLOR): the texel first */
      UNPACK(tex[idx[i]], tr, tg, tb);
      r = MULS8(tr, r, sh); g = MULS8(tg, g, sh);
      b = MULS8(tb, b, sh);
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
      else if (OUT == O_MUL)                   /* zo_mul: the source first */
        pp[i] = PACK(MUL8(r, dr), MUL8(g, dg), MUL8(b, db));
      else                                     /* zo_mul2 */
        pp[i] = PACK(clamp255(2 * MUL8(r, dr)), clamp255(2 * MUL8(g, dg)),
                     clamp255(2 * MUL8(b, db)));
    }
  }
}

#define ZF_ONE(name, COL, ENV, AT, ZW, OUT) \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f) { zf_one_t(p, s, f, COL, ENV, AT, ZW, OUT); }
ZF_ONE(zf1_sbar, C_FLAT, E_REP_RGB, 0, 0, O_SAOMSA)
ZF_ONE(zf1_blend, C_FLAT, E_NONE, 0, 0, O_SAOMSA)
ZF_ONE(zf1_pic, C_FLAT, E_REP_RGB, 1, 0, O_STORE)
ZF_ONE(zf1_pica, C_NONE, E_REP_RGBA, 1, 0, O_STORE)
ZF_ONE(zf1_fence, C_NONE, E_REP_RGBA, 1, 1, O_STORE)
ZF_ONE(zf1_lmap, C_NONE, E_REP_RGBA, 0, 0, O_MUL2)
ZF_ONE(zf1_lmap1, C_NONE, E_REP_RGBA, 0, 0, O_MUL)
ZF_ONE(zf1_amod, C_SMOOTH, E_MOD_RGB, 0, 0, O_STORE)
ZF_ONE(zf1_amod2, C_SMOOTH, E_MOD_RGB, 0, 0, O_ONEONE)
ZF_ONE(zf1_con, C_FLAT, E_MOD_RGB, 0, 0, O_SAOMSA)
ZF_ONE(zf1_water, C_NONE, E_REP_RGB, 0, 0, O_STORE)
ZF_ONE(zf1_alias, C_SMOOTH, E_COMB_MT, 0, 0, O_STORE)
ZF_ONE(zf1_part, C_FLAT, E_MOD_RGBA, 0, 0, O_SAOMSA)
ZF_ONE(zf1_glow, C_FLAT, E_MOD_RGB, 0, 0, O_ONEONE)

/* the one-unit signatures: gl_build_pipe's list is
   [colour] [texel] [texenv] [alpha GREATER] [depth write] [blend/store] */
__attribute__((cold))
static ZStageFn zf1_pick(const ZPipe *p, const ZPipeX *x, const ZStageFn *st)
{
  int k = 0, col = C_NONE, env = E_NONE, at = 0, zw = 0, out;
  ZStageFn e;
  if (x->slot_col == &st[0]) {
    if (st[0] == zp_color_fn(1)) col = C_FLAT;
    else if (st[0] == zp_color_fn(0)) col = C_SMOOTH;
    else return NULL;
    k++;
  }
  if (x->tf[0].slot == &st[k]) {
    e = st[k + 1];
    if (e == zp_texenv_fn(ZP_TE_REPLACE_RGB)) env = E_REP_RGB;
    else if (e == zp_texenv_fn(ZP_TE_REPLACE_RGBA)) env = E_REP_RGBA;
    else if (e == zp_texenv_fn(ZP_TE_MOD_RGB)) env = E_MOD_RGB;
    else if (e == zp_texenv_fn(ZP_TE_MOD_RGBA)) env = E_MOD_RGBA;
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
  }
  if (st[k] == zp_alpha_fn(GL_GREATER)) { at = 1; k++; }
  if (st[k] == zp_zwrite_fn()) { zw = 1; k++; }
  if (st[k] == zp_out_fn(GL_ONE, GL_ZERO, 0xffff, GL_FUNC_ADD)) out = O_STORE;
  else if (st[k] == zp_out_fn(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0xffff, GL_FUNC_ADD)) out = O_SAOMSA;
  else if (st[k] == zp_out_fn(GL_ONE, GL_ONE, 0xffff, GL_FUNC_ADD)) out = O_ONEONE;
  else if (st[k] == zp_out_fn(GL_ZERO, GL_SRC_COLOR, 0xffff, GL_FUNC_ADD)) out = O_MUL;
  else if (st[k] == zp_out_fn(GL_DST_COLOR, GL_SRC_COLOR, 0xffff, GL_FUNC_ADD)) out = O_MUL2;
  else return NULL;
  if (st[k + 1]) return NULL;
  (void)p;
  /* REPLACE of an RGBA texel sets the colour and the alpha: the colour
     stage computes what nothing reads (the list keeps it because the
     alpha is read), so those signatures do not run it */
  if (env == E_REP_RGBA) col = C_NONE;
#define ZF1(C, E, A, Z, O) (col == (C) && env == (E) && at == (A) && zw == (Z) && out == (O))
  if (ZF1(C_FLAT, E_REP_RGB, 0, 0, O_SAOMSA)) return zf1_sbar;
  if (ZF1(C_FLAT, E_NONE, 0, 0, O_SAOMSA)) return zf1_blend;
  if (ZF1(C_FLAT, E_REP_RGB, 1, 0, O_STORE)) return zf1_pic;
  if (ZF1(C_NONE, E_REP_RGBA, 1, 0, O_STORE)) return zf1_pica;
  if (ZF1(C_NONE, E_REP_RGBA, 1, 1, O_STORE)) return zf1_fence;
  if (ZF1(C_NONE, E_REP_RGBA, 0, 0, O_MUL2)) return zf1_lmap;
  if (ZF1(C_NONE, E_REP_RGBA, 0, 0, O_MUL)) return zf1_lmap1;
  if (ZF1(C_SMOOTH, E_MOD_RGB, 0, 0, O_STORE)) return zf1_amod;
  if (ZF1(C_SMOOTH, E_MOD_RGB, 0, 0, O_ONEONE)) return zf1_amod2;
  if (ZF1(C_FLAT, E_MOD_RGB, 0, 0, O_SAOMSA)) return zf1_con;
  if (ZF1(C_NONE, E_REP_RGB, 0, 0, O_STORE)) return zf1_water;
  if (ZF1(C_SMOOTH, E_COMB_MT, 0, 0, O_STORE)) return zf1_alias;
  if (ZF1(C_FLAT, E_MOD_RGBA, 0, 0, O_SAOMSA)) return zf1_part;
  if (ZF1(C_FLAT, E_MOD_RGB, 0, 0, O_ONEONE)) return zf1_glow;
#undef ZF1
  return NULL;
}

/* phase 5: the fillers for units of 8-bit words */
#include "zpipe_fused8.h"

/* ------------------------------------------------------------ selection */

__attribute__((cold))
void zpf_select(GLContext *c)
{
  ZPipe *p = &c->pipe;
  ZPipeX *x = &c->pipex;
  ZStageFn *st = p->st, fn = NULL;
  const ZComb *c0 = &x->cb[0], *c1 = &x->cb[1];
  ZStageFn store = zp_out_fn(GL_ONE, GL_ZERO, 0xffff, GL_FUNC_ADD);
  ZStageFn saomsa = zp_out_fn(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0xffff, GL_FUNC_ADD);
  int n, kind = ZF_NONE, k;

  x->fused = ZF_NONE;
  for (n = 0; st[n]; n++) ;
  x->bil0 = 0;
  /* phase 5: a unit whose texels are 8-bit words (ZTexF.t8) has its own
     fillers (zpipe_fused8.h); the 565 ones read ZPipe.tex */
  if ((c->tex_active && x->tf[0].t8) || (c->tu1_on && x->tf[1].t8)) {
    int direct;
    fn = zpf8_pick(c, &kind, &direct);
    zpf_count[kind]++;
    if (fn == NULL) { x->bil0 = 0; return; }
    for (k = 0; k <= n; k++) x->gen_st[k] = st[k];
    if (x->slot_col) x->slot_col = &x->gen_st[x->slot_col - st];
    for (k = 0; k < 2; k++)
      if (x->tf[k].slot) x->tf[k].slot = &x->gen_st[x->tf[k].slot - st];
    x->gen_depth = p->depth;
    st[0] = fn;
    st[1] = NULL;
    x->fused = kind;
    if (direct) {
      /* the filler tests depth itself and the runners call it directly */
      p->depth = zf_pass;
      x->chunk = fn;
      x->run_mt = zp_run_mt_direct;
      if (direct == 2) x->run_lod_mt = zp_run_lod_mt_direct;
    }
    return;
  }
  if (!c->tu1_on) {
    /* phase 5 O2: one texture unit (or none) */
    fn = zf1_pick(p, x, st);
    if (fn) kind = ZF_ONE;
    /* the level-0 REPEAT bilinear stage, placed once for the batch */
    if (fn && c->tex_active && c->tex_filtered && !(p->xact & ZPX_TEX) &&
        x->tf[0].cur0 == &x->tf[0].lvl[0]) {
      if (*x->tf[0].slot == zpx_stage_u(0, TF_LINEAR0, 1, 0)) x->bil0 = 1;
      else if (*x->tf[0].slot == zpx_stage_u(0, TF_LINEAR0, 1, 1)) x->bil0 = 2;
    }
  }
  /* ZF_WORLD: [texel 0] [REPLACE RGB] [texel 1] [combiner 1] [store] */
  if (c->tu1_on && n == 5 && x->tf[0].slot == &st[0] && st[1] == zp_texenv_fn(ZP_TE_REPLACE_RGB) &&
      x->tf[1].slot == &st[2] && st[3] == zp_comb_fn(1) && st[4] == store &&
      !c1->zero_rgb && c1->f[1] == ZCF_NONE &&
      prog(c1, 0, ZCF_MODULATE, ZCS_PREVIOUS, ZCO_COLOR, ZCS_TEXTURE, ZCO_COLOR) &&
      zf_world_tables(x, c1->sh[0])) {
    kind = ZF_WORLD;
    fn = zf_world;
  }
  /* ZF_ALIAS: [colour] [texel 0] [combiner 0] [texel 1] [combiner 1]
     [SRC_ALPHA, ONE_MINUS_SRC_ALPHA / store] */
  if (c->tu1_on && n == 6 && x->slot_col == &st[0] && x->tf[0].slot == &st[1] &&
      st[2] == zp_comb_fn(0) && x->tf[1].slot == &st[3] && st[4] == zp_comb_fn(1) &&
      (st[5] == saomsa || st[5] == store) && !c0->zero_rgb && !c1->zero_rgb &&
      prog(c0, 0, ZCF_MODULATE, ZCS_TEXTURE, ZCO_COLOR, ZCS_PRIMARY, ZCO_COLOR) &&
      prog(c1, 0, ZCF_ADD, ZCS_PREVIOUS, ZCO_COLOR, ZCS_TEXTURE, ZCO_COLOR) &&
      c1->sh[0] == 0) {
    if (st[5] == store) {
      /* the alpha is read by nothing (the parts are ZCF_NONE) */
      if (c0->f[1] == ZCF_NONE && c1->f[1] == ZCF_NONE) {
        kind = ZF_ALIAS_STORE;
        fn = zf_alias_st;
      }
    } else if (prog(c0, 1, ZCF_MODULATE, ZCS_TEXTURE, ZCO_ALPHA, ZCS_PREVIOUS, ZCO_ALPHA)) {
      /* unit 1's alpha: none (an RGB texture's ADD program) or MODULATE
         (PREVIOUS, TEXTURE) of a texture with an alpha plane */
      int a1 = -1;
      if (c1->f[1] == ZCF_NONE) a1 = 0;
      else if (prog(c1, 1, ZCF_MODULATE, ZCS_PREVIOUS, ZCO_ALPHA, ZCS_TEXTURE, ZCO_ALPHA) &&
               c1->sh[1] == 0 && x->tf[1].talpha0 != NULL)
        a1 = 1;
      if (a1 >= 0) {
        int a0 = x->tf[0].talpha0 != NULL;
        kind = ZF_ALIAS;
        fn = a0 ? (a1 ? zf_alias_11 : zf_alias_10) : (a1 ? zf_alias_01 : zf_alias_00);
      }
    }
  }
  zpf_count[kind]++;
  if (kind == ZF_NONE) return;
  /* the general list moves to gen_st, and the slots with it */
  for (k = 0; k <= n; k++) x->gen_st[k] = st[k];
  if (x->slot_col) x->slot_col = &x->gen_st[x->slot_col - st];
  for (k = 0; k < 2; k++)
    if (x->tf[k].slot) x->tf[k].slot = &x->gen_st[x->tf[k].slot - st];
  x->gen_depth = p->depth;
  st[0] = fn;
  st[1] = NULL;
  x->fused = kind;
  if (kind == ZF_WORLD && !(p->xact & (ZPX_TEX | ZPX_TEX1)) &&
      *x->tf[0].slot == zp_texidx_fn(0, 0) && *x->tf[1].slot == zp_texidx1_fn(0, 0)) {
    /* both units nearest, REPEAT, for every triangle of the batch (no
       per-triangle level choice can place another stage): the walks and,
       for the common depth states, the depth test move into the filler,
       and ZPipe.depth passes everything (the pixel paths take gen_depth) */
    int d = p->depth == zp_depth_fn(ZP_DEPTH_LEQUAL, 1) ? 0 :
            p->depth == zp_depth_fn(ZP_DEPTH_LEQUAL, 0) ? 1 :
            p->depth == zp_depth_fn(ZP_DEPTH_LESS, 1) ? 2 : -1;
    if (d >= 0) {
      st[0] = d == 0 ? zf_world_nn0 : (d == 1 ? zf_world_nn1 : zf_world_nn2);
      p->depth = zf_pass;
      /* (phase 5 O2) called directly by the runner */
      x->chunk = st[0];
      x->run_mt = zp_run_mt_direct;
      zpf_count[ZF_WORLD_NN]++;
    }
  } else if (kind == ZF_WORLD && p->depth == zp_depth_fn(ZP_DEPTH_LEQUAL, 1) &&
             c->tu1_filtered && !(p->xact & ZPX_TEX1) &&
             *x->tf[1].slot == zpx_stage_u(1, TF_LINEAR0, 1, 0) &&
             (!c->tex_filtered ? *x->tf[0].slot == zp_texidx_fn(0, 0) :
              x->tf[0].f_bil == zpx_stage_u(0, TF_LINEAR0, 1, 0))) {
    /* phase 5 O2: a filtered world batch - unit 1 bilinear in level 0,
       REPEAT (the lightmap), unit 0 any filter with REPEAT, depth LEQUAL
       with the write: zf_world_x (the f_* stages of a filtered unit 0 are
       its REPEAT, no-alpha ones; an unfiltered unit 0 is always zt_rr) */
    st[0] = zf_world_x;
    p->depth = zf_pass;
    /* the runners call it directly: the depth stage passes everything
       and the list is this one filler */
    x->chunk = zf_world_x;
    x->run_mt = zp_run_mt_direct;
    x->run_lod_mt = zp_run_lod_mt_direct;
    zpf_count[ZF_WORLD_X]++;
  }
}

__attribute__((cold))
int zpf_is_fused_stage(ZStageFn f)
{
  if (f == zf1_sbar || f == zf1_blend || f == zf1_pic || f == zf1_pica ||
      f == zf1_fence || f == zf1_con || f == zf1_water || f == zf1_alias ||
      f == zf1_part || f == zf1_glow || f == zf1_lmap || f == zf1_lmap1 ||
      f == zf1_amod || f == zf1_amod2)
    return 1;
  return f == zf_world || f == zf_world_x || f == zf_world_nn0 || f == zf_world_nn1 || f == zf_world_nn2 ||
         f == zf_alias_00 || f == zf_alias_10 || f == zf_alias_01 ||
         f == zf_alias_11 || f == zf_alias_st ||
         f == zf8_world_11 || f == zf8_world_10 || f == zf8_world_01 ||
         f == zf8_world_x_p8 || f == zf8_world_x_c565 || f == zf8_alias_bl || f == zf8_alias_st;
}
