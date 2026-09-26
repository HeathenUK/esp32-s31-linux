/*
 * zpipe.c - the stages of the general fragment path. See zpipe.h for the
 * model; raster.c chooses the stages at glBegin. s31, MIT.
 *
 * Every stage is a straight loop over one chunk: which stage runs is
 * decided once per primitive, never per pixel. No double anywhere here
 * (F without D: a double is a libcall on this hart).
 */
#include <math.h>
#include <string.h>
#include "zgl.h"
#include "s31_ramtext.h"
#include "zpipe.h"

#include "zpipe_int.h"      /* MUL8, clamp255, UNPACK, PACK (phase 5: shared
                                with the fused fillers, zpipe_fused.c) */

/* ------------------------------------------------------------ depth test */

/* TinyGL's Z grows towards the viewer (clear.c), so GL's LESS is > */
#define ZP_DEPTH(name, CMP)                                             \
static int name(const ZSpan *s, ZFrag *f)                               \
{                                                                       \
  unsigned int z = s->z, zz;                                            \
  const unsigned short *pz = s->pz;                                     \
  int i, n = f->n, alive = 0, ok;                                       \
  for (i = 0; i < n; i++) {                                             \
    zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;                          \
    ok = CMP(zz, pz[i]);                                                \
    f->zz[i] = (unsigned short)zz;                                      \
    f->m[i] = (unsigned char)ok;                                        \
    alive += ok;                                                        \
    z += s->dzdx;                                                       \
  }                                                                     \
  return alive;                                                         \
}
/* the test and the write in one pass, when nothing after the depth test
   can kill a fragment (no alpha test): one stage fewer per chunk */
#define ZP_DEPTH_W(name, CMP)                                           \
static int name(const ZSpan *s, ZFrag *f)                               \
{                                                                       \
  unsigned int z = s->z, zz;                                            \
  unsigned short *pz = s->pz;                                           \
  int i, n = f->n, alive = 0, ok;                                       \
  for (i = 0; i < n; i++) {                                             \
    zz = (z >> ZB_POINT_Z_FRAC_BITS) & 0xffff;                          \
    ok = CMP(zz, pz[i]);                                                \
    if (ok) pz[i] = (unsigned short)zz;                                 \
    f->m[i] = (unsigned char)ok;                                        \
    alive += ok;                                                        \
    z += s->dzdx;                                                       \
  }                                                                     \
  return alive;                                                         \
}
#define C_LESS(a, b) ((a) > (b))
#define C_LEQUAL(a, b) ((a) >= (b))
#define C_GREATER(a, b) ((a) < (b))
#define C_GEQUAL(a, b) ((a) <= (b))
#define C_EQUAL(a, b) ((a) == (b))
#define C_NOTEQUAL(a, b) ((a) != (b))
#define C_ALWAYS(a, b) ((void)(b), 1)
ZP_DEPTH(zd_less, C_LESS)
ZP_DEPTH(zd_lequal, C_LEQUAL)
ZP_DEPTH(zd_greater, C_GREATER)
ZP_DEPTH(zd_gequal, C_GEQUAL)
ZP_DEPTH(zd_equal, C_EQUAL)
ZP_DEPTH(zd_notequal, C_NOTEQUAL)
ZP_DEPTH(zd_always, C_ALWAYS)
ZP_DEPTH_W(zdw_less, C_LESS)
ZP_DEPTH_W(zdw_lequal, C_LEQUAL)
ZP_DEPTH_W(zdw_greater, C_GREATER)
ZP_DEPTH_W(zdw_gequal, C_GEQUAL)
ZP_DEPTH_W(zdw_equal, C_EQUAL)
ZP_DEPTH_W(zdw_notequal, C_NOTEQUAL)
ZP_DEPTH_W(zdw_always, C_ALWAYS)

/* depth test disabled: everything passes, nothing is written */
static int zd_none(const ZSpan *s, ZFrag *f)
{
  (void)s;
  memset(f->m, 1, f->n);
  return f->n;
}

static int zd_never(const ZSpan *s, ZFrag *f)
{
  (void)s; (void)f;
  return 0;
}

ZDepthFn zp_depth_fn(int d, int write)
{
  if (write) {
    switch (d) {
    case ZP_DEPTH_NEVER: return zd_never;
    case ZP_DEPTH_LESS: return zdw_less;
    case ZP_DEPTH_EQUAL: return zdw_equal;
    case ZP_DEPTH_LEQUAL: return zdw_lequal;
    case ZP_DEPTH_GREATER: return zdw_greater;
    case ZP_DEPTH_NOTEQUAL: return zdw_notequal;
    case ZP_DEPTH_GEQUAL: return zdw_gequal;
    case ZP_DEPTH_ALWAYS: return zdw_always;
    default: return zd_none;
    }
  }
  switch (d) {
  case ZP_DEPTH_NEVER: return zd_never;
  case ZP_DEPTH_LESS: return zd_less;
  case ZP_DEPTH_EQUAL: return zd_equal;
  case ZP_DEPTH_LEQUAL: return zd_lequal;
  case ZP_DEPTH_GREATER: return zd_greater;
  case ZP_DEPTH_NOTEQUAL: return zd_notequal;
  case ZP_DEPTH_GEQUAL: return zd_gequal;
  case ZP_DEPTH_ALWAYS: return zd_always;
  default: return zd_none;
  }
}

/* ------------------------------------------------------------ colour */

static void zc_smooth(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  int r = s->r, g = s->g, b = s->b, a = s->a, i, n = f->n;
  (void)p;
  for (i = 0; i < n; i++) {
    /* clamped: the inclusive spans reach up to a pixel past the edge,
       where the colour is extrapolated */
    f->r[i] = (unsigned char)clamp255(r >> ZP_CSHIFT);
    f->g[i] = (unsigned char)clamp255(g >> ZP_CSHIFT);
    f->b[i] = (unsigned char)clamp255(b >> ZP_CSHIFT);
    f->a[i] = (unsigned char)clamp255(a >> ZP_CSHIFT);
    r += s->drdx; g += s->dgdx; b += s->dbdx; a += s->dadx;
  }
}

static void zc_flat(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  unsigned char r = p->flat[0], g = p->flat[1], b = p->flat[2], a = p->flat[3];
  int i, n = f->n;
  (void)s;
  for (i = 0; i < n; i++) {
    f->r[i] = r; f->g[i] = g; f->b[i] = b; f->a[i] = a;
  }
}

ZStageFn zp_color_fn(int flat)
{
  return flat ? zc_flat : S31_RT_RAM(zc_smooth);   /* phase 6 ramtext */
}

/* ------------------------------------------------------------ texel index */

/* TinyGL's perspective walk (ztriangle.c ZB_fillTriangleMappingPerspective):
   s/t divided by z every 8 pixels, stepped linearly in between. */
#define IDX_RR(si, ti) ((((unsigned int)(ti) & p->tmask) | ((unsigned int)(si) & p->smask)) >> p->fbits)
#define COL_R(si) (((si) >> p->fbits) & p->wmax)
#define COL_C(si) clampi((si) >> p->fbits, p->wmax)
#define ROW_R(ti) (((ti) >> (p->fbits + p->ws)) & p->hmax)
#define ROW_C(ti) clampi((ti) >> (p->fbits + p->ws), p->hmax)
#define IDX_2(C, R, si, ti) (((unsigned int)R(ti) << p->ws) | (unsigned int)C(si))
#define IDX_RC(si, ti) IDX_2(COL_R, ROW_C, si, ti)
#define IDX_CR(si, ti) IDX_2(COL_C, ROW_R, si, ti)
#define IDX_CC(si, ti) IDX_2(COL_C, ROW_C, si, ti)

static inline int clampi(int v, int hi)
{
  return v < 0 ? 0 : (v > hi ? hi : v);
}

#define ZP_TEXIDX(name, IDX)                                            \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  float fz = s->fz, sz = s->sz, tz = s->tz;                             \
  const float dfz = s->dfzdx, dsz = s->dszdx, dtz = s->dtzdx;           \
  int i = 0, e, n = f->n;                                               \
  while (i < n) {                                                       \
    float zinv = 1.0f / fz, ss = sz * zinv, tt = tz * zinv;             \
    int si = (int)ss, ti = (int)tt;                                     \
    int dsi = (int)((dsz - ss * dfz) * zinv);                           \
    int dti = (int)((dtz - tt * dfz) * zinv);                           \
    e = i + 8 < n ? i + 8 : n;                                          \
    for (; i < e; i++) {                                                \
      f->idx[i] = IDX(si, ti);                                          \
      si += dsi; ti += dti;                                             \
    }                                                                   \
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;               \
  }                                                                     \
}
ZP_TEXIDX(zt_rr, IDX_RR)
ZP_TEXIDX(zt_rc, IDX_RC)
ZP_TEXIDX(zt_cr, IDX_CR)
ZP_TEXIDX(zt_cc, IDX_CC)

ZStageFn zp_texidx_fn(int clamp_s, int clamp_t)
{
  if (clamp_s) return clamp_t ? zt_cc : zt_cr;
  return clamp_t ? zt_rc : S31_RT_RAM(zt_rr);        /* phase 6 ramtext */
}

/* phase 5 O1: texture unit 1's, from ZPipeX.g1 (the same fixed point and
   the same walk as unit 0's), into ZFrag.idx1 */
#define G_IDX_RR(si, ti) ((((unsigned int)(ti) & g->tmask) | ((unsigned int)(si) & g->smask)) >> g->fbits)
#define G_COL_R(si) (((si) >> g->fbits) & g->wmax)
#define G_COL_C(si) clampi((si) >> g->fbits, g->wmax)
#define G_ROW_R(ti) (((ti) >> (g->fbits + g->ws)) & g->hmax)
#define G_ROW_C(ti) clampi((ti) >> (g->fbits + g->ws), g->hmax)
#define G_IDX_2(C, R, si, ti) (((unsigned int)R(ti) << g->ws) | (unsigned int)C(si))
#define G_IDX_RC(si, ti) G_IDX_2(G_COL_R, G_ROW_C, si, ti)
#define G_IDX_CR(si, ti) G_IDX_2(G_COL_C, G_ROW_R, si, ti)
#define G_IDX_CC(si, ti) G_IDX_2(G_COL_C, G_ROW_C, si, ti)

#define ZP_TEXIDX1(name, IDX)                                           \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  const ZTexGeo *g = &p->x->g1;                                         \
  float fz = s->fz, sz = s->sz1, tz = s->tz1;                           \
  const float dfz = s->dfzdx, dsz = s->dszdx1, dtz = s->dtzdx1;         \
  int i = 0, e, n = f->n;                                               \
  while (i < n) {                                                       \
    float zinv = 1.0f / fz, ss = sz * zinv, tt = tz * zinv;             \
    int si = (int)ss, ti = (int)tt;                                     \
    int dsi = (int)((dsz - ss * dfz) * zinv);                           \
    int dti = (int)((dtz - tt * dfz) * zinv);                           \
    e = i + 8 < n ? i + 8 : n;                                          \
    for (; i < e; i++) {                                                \
      f->idx1[i] = IDX(si, ti);                                         \
      si += dsi; ti += dti;                                             \
    }                                                                   \
    fz += 8.0f * dfz; sz += 8.0f * dsz; tz += 8.0f * dtz;               \
  }                                                                     \
}
ZP_TEXIDX1(zt1_rr, G_IDX_RR)
ZP_TEXIDX1(zt1_rc, G_IDX_RC)
ZP_TEXIDX1(zt1_cr, G_IDX_CR)
ZP_TEXIDX1(zt1_cc, G_IDX_CC)

ZStageFn zp_texidx1_fn(int clamp_s, int clamp_t)
{
  if (clamp_s) return clamp_t ? zt1_cc : zt1_cr;
  return clamp_t ? zt1_rc : zt1_rr;
}

/* ------------------------------------------------------------ texenv */

/* GL 1.3 table 3.22, per base format class. C = fragment colour,
   T = texel, K = GL_TEXTURE_ENV_COLOR. */
#define ZP_TEXENV(name, BODY)                                           \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  int i, n = f->n, tr, tg, tb, ta;                                      \
  (void)s; (void)ta; (void)tr; (void)tg; (void)tb;                      \
  for (i = 0; i < n; i++) {                                             \
    BODY                                                                \
  }                                                                     \
}
#define T_RGB UNPACK(p->tex[f->idx[i]], tr, tg, tb);
#define T_A ta = p->talpha[f->idx[i]];
#define SET_C(R, G, B) f->r[i] = (unsigned char)(R); f->g[i] = (unsigned char)(G); \
  f->b[i] = (unsigned char)(B);
#define BLENDK(c, t, k) MIX8(c, 255 - (t), k, t)

#define ZE(n) ze_##n
#include "zpipe_env.h"
#undef ZE
#undef T_RGB
#undef T_A
/* phase 5: the same on a texel of 8 bits (s31_tex8.c), its RGBA8 word in
   ZTexF.ftex32 (unit 0: the texenv stages are unit 0's) */
#define T_RGB { unsigned int w_ = p->x->tf[0].ftex32[f->idx[i]]; \
    tr = (int)(w_ & 255); tg = (int)((w_ >> 8) & 255); tb = (int)((w_ >> 16) & 255); }
#define T_A ta = (int)(p->x->tf[0].ftex32[f->idx[i]] >> 24);
#define ZE(n) ze8_##n
#include "zpipe_env.h"
#undef ZE

ZStageFn zp_texenv_fn(int op)
{
  static const ZStageFn t[ZP_TE_N] = {
    ze_replace_rgb, ze_replace_rgba, ze_mod_rgb, ze_mod_rgba, ze_decal_rgba,
    ze_blend_rgb, ze_blend_rgba, ze_blend_i, ze_alpha_replace, ze_alpha_mod,
    ze_add_rgb, ze_add_rgba, ze_add_i, ze_replace_rgb1,
  };
  return op >= 0 && op < ZP_TE_N ? t[op] : ze_replace_rgb;
}

ZStageFn zp_texenv8_fn(int op)
{
  static const ZStageFn t[ZP_TE_N] = {
    ze8_replace_rgb, ze8_replace_rgba, ze8_mod_rgb, ze8_mod_rgba, ze8_decal_rgba,
    ze8_blend_rgb, ze8_blend_rgba, ze8_blend_i, ze8_alpha_replace, ze8_alpha_mod,
    ze8_add_rgb, ze8_add_rgba, ze8_add_i, ze8_replace_rgb1,
  };
  return op >= 0 && op < ZP_TE_N ? t[op] : ze8_replace_rgb;
}

/* ------------------------------------------------------------ combiner */

/* phase 5 O1: GL_ARB_texture_env_combine (GL 1.3 3.8.13, table 3.20), and
   every texture environment of unit 1. One stage per unit runs the unit's
   program (ZPipeX.cb, raster_sel.c comb_prog):
     Arg_i = OPERAND_i(SOURCE_i), for the RGB and the alpha part apart;
     REPLACE a0, MODULATE a0 a1, ADD a0 + a1, ADD_SIGNED a0 + a1 - 1/2,
     INTERPOLATE a0 a2 + a1 (1 - a2), SUBTRACT a0 - a1;
     times RGB_SCALE / ALPHA_SCALE, clamped to [0, 1].
   The arithmetic is the texenv stages' own (8-bit, MUL8, clamp255; 1/2 is
   128), so a mode expressed as a program computes exactly what the
   corresponding unit-0 stage computes: MODULATE is MUL8(previous, texel),
   DECAL and BLEND the two-product sums of BLENDK. The choice of function,
   source and operand is made once per chunk; the loops are per channel.
   Sources: TEXTURE the unit's texel (as a combiner source an ALPHA base
   format has colour 0, GL 1.3 table 3.20's A = (0, 0, 0, At); RGB-class
   formats alpha 1), CONSTANT the unit's GL_TEXTURE_ENV_COLOR, PRIMARY_COLOR
   the fragment's colour before texturing (unit 1: kept by zp_saveprim when
   unit 0 changes it), PREVIOUS the running colour. */
#define ZC_N ZP_CHUNK

/* the source arrays of one chunk: [source][channel] */
typedef struct {
  const unsigned char *c[4][4];
} ZCSrc;

/* one argument: its three colour channels (or its alpha) after the
   operand, pointing into the source or into tmp */
static inline void zc_arg_rgb(const ZCSrc *S, int src, int op, int n,
                              unsigned char tmp[3][ZC_N], const unsigned char *out[3])
{
  int ch, i;
  switch (op) {
  case ZCO_COLOR:
    for (ch = 0; ch < 3; ch++) out[ch] = S->c[src][ch];
    break;
  case ZCO_OMCOLOR:
    for (ch = 0; ch < 3; ch++) {
      const unsigned char *a = S->c[src][ch];
      for (i = 0; i < n; i++) tmp[ch][i] = (unsigned char)(255 - a[i]);
      out[ch] = tmp[ch];
    }
    break;
  case ZCO_ALPHA:
    out[0] = out[1] = out[2] = S->c[src][3];
    break;
  default: {                              /* ZCO_OMALPHA */
    const unsigned char *a = S->c[src][3];
    for (i = 0; i < n; i++) tmp[0][i] = (unsigned char)(255 - a[i]);
    out[0] = out[1] = out[2] = tmp[0];
    break;
  }
  }
}

static inline const unsigned char *zc_arg_a(const ZCSrc *S, int src, int op, int n,
                                            unsigned char *tmp)
{
  int i;
  const unsigned char *a = S->c[src][3];
  if (op == ZCO_ALPHA) return a;
  for (i = 0; i < n; i++) tmp[i] = (unsigned char)(255 - a[i]);
  return tmp;
}

/* one channel: function f of the arguments, times 2^sh, clamped */
static inline void zc_func(int f, int sh, int n, const unsigned char *a0,
                           const unsigned char *a1, const unsigned char *a2,
                           unsigned char *out)
{
  int i;
  switch (f) {
  case ZCF_REPLACE:
    for (i = 0; i < n; i++) out[i] = (unsigned char)clamp255((int)a0[i] << sh);
    break;
  case ZCF_MODULATE:
    for (i = 0; i < n; i++) out[i] = (unsigned char)MULS8(a0[i], a1[i], sh);
    break;
  case ZCF_ADD:
    for (i = 0; i < n; i++) out[i] = (unsigned char)clamp255((a0[i] + a1[i]) << sh);
    break;
  case ZCF_ADD_SIGNED:
    for (i = 0; i < n; i++) out[i] = (unsigned char)ADDS8(a0[i], a1[i], sh);
    break;
  case ZCF_INTERPOLATE:
    for (i = 0; i < n; i++)
      out[i] = (unsigned char)MIXS8(a0[i], a2[i], a1[i], 255 - a2[i], sh);
    break;
  default:                                /* ZCF_SUBTRACT */
    for (i = 0; i < n; i++) out[i] = (unsigned char)clamp255((a0[i] - a1[i]) * (1 << sh));
    break;
  }
}

#define ZC_NARGS(f) ((f) == ZCF_REPLACE ? 1 : ((f) == ZCF_INTERPOLATE ? 3 : 2))

static inline __attribute__((always_inline))
void zc_comb(const ZPipe *p, ZFrag *f, const int unit)
{
  const ZComb *cb = &p->x->cb[unit];
  const PIXEL *tex = unit ? p->tex1 : p->tex;
  const unsigned char *tal = unit ? p->talpha1 : p->talpha;
  const unsigned int *idx = unit ? (p->idx1_px ? p->idx1_px : f->idx1) : f->idx;
  unsigned char tr[ZC_N], tg[ZC_N], tb[ZC_N], ta[ZC_N];
  unsigned char kc[4][ZC_N];
  unsigned char tmp[3][3][ZC_N], atmp[3][ZC_N];
  unsigned char res[4][ZC_N];
  const unsigned char *arg[3][3], *aarg[3] = { NULL, NULL, NULL };
  ZCSrc S;
  int i, j, n = f->n, ch;

  /* the texel of every fragment (read whether or not an argument uses it:
     one pass, and the combiner is the general path) */
  if (p->x->tf[unit].t8) {
    /* phase 5: an 8-bit texel's word (its alpha is the texel's: 255 for
       the RGB class) */
    const unsigned int *w8 = p->x->tf[unit].ftex32;
    for (i = 0; i < n; i++) {
      unsigned int w = w8[idx[i]];
      tr[i] = (unsigned char)w; tg[i] = (unsigned char)(w >> 8);
      tb[i] = (unsigned char)(w >> 16); ta[i] = (unsigned char)(w >> 24);
    }
  } else {
    for (i = 0; i < n; i++) {
      unsigned int t = tex[idx[i]];
      int r, g, b;
      UNPACK(t, r, g, b);
      tr[i] = (unsigned char)r; tg[i] = (unsigned char)g; tb[i] = (unsigned char)b;
    }
    /* an RGB-class texture's alpha is 1: by its format, not the pointer -
       under a linear filter ZPipe.talpha is the filtered chunk's alpha
       array, which an RGB texture's filter stage does not write (found by
       gl/tests/fused_test.c) */
    if (tal && cb->fmt != TGL_TEXF_RGB) for (i = 0; i < n; i++) ta[i] = tal[idx[i]];
    else memset(ta, 255, n);
  }
  if (cb->zero_rgb) { memset(tr, 0, n); memset(tg, 0, n); memset(tb, 0, n); }
  for (ch = 0; ch < 4; ch++) memset(kc[ch], cb->k[ch], n);
  S.c[ZCS_TEXTURE][0] = tr; S.c[ZCS_TEXTURE][1] = tg;
  S.c[ZCS_TEXTURE][2] = tb; S.c[ZCS_TEXTURE][3] = ta;
  for (ch = 0; ch < 4; ch++) S.c[ZCS_CONSTANT][ch] = kc[ch];
  S.c[ZCS_PREVIOUS][0] = f->r; S.c[ZCS_PREVIOUS][1] = f->g;
  S.c[ZCS_PREVIOUS][2] = f->b; S.c[ZCS_PREVIOUS][3] = f->a;
  if (unit) {
    S.c[ZCS_PRIMARY][0] = f->pr; S.c[ZCS_PRIMARY][1] = f->pg;
    S.c[ZCS_PRIMARY][2] = f->pb; S.c[ZCS_PRIMARY][3] = f->pa;
  } else {
    /* unit 0: the previous colour is the primary one */
    for (ch = 0; ch < 4; ch++) S.c[ZCS_PRIMARY][ch] = S.c[ZCS_PREVIOUS][ch];
  }
  if (cb->f[0] != ZCF_NONE) {
    for (j = 0; j < ZC_NARGS(cb->f[0]); j++)
      zc_arg_rgb(&S, cb->src[0][j], cb->op[0][j], n, tmp[j], arg[j]);
    for (; j < 3; j++) arg[j][0] = arg[j][1] = arg[j][2] = NULL;
    for (ch = 0; ch < 3; ch++)
      zc_func(cb->f[0], cb->sh[0], n, arg[0][ch], arg[1][ch], arg[2][ch], res[ch]);
  }
  if (cb->f[1] != ZCF_NONE) {
    for (j = 0; j < ZC_NARGS(cb->f[1]); j++)
      aarg[j] = zc_arg_a(&S, cb->src[1][j], cb->op[1][j], n, atmp[j]);
    zc_func(cb->f[1], cb->sh[1], n, aarg[0], aarg[1], aarg[2], res[3]);
  }
  /* both parts read the previous colour: written back after both */
  if (cb->f[0] != ZCF_NONE) {
    memcpy(f->r, res[0], n); memcpy(f->g, res[1], n); memcpy(f->b, res[2], n);
  }
  if (cb->f[1] != ZCF_NONE) memcpy(f->a, res[3], n);
}

static void zc_comb0(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  (void)s;
  zc_comb(p, f, 0);
}

static void zc_comb1(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  (void)s;
  zc_comb(p, f, 1);
}

ZStageFn zp_comb_fn(int unit)
{
  return unit ? zc_comb1 : zc_comb0;
}

static void zc_saveprim(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  int n = f->n;
  (void)p; (void)s;
  memcpy(f->pr, f->r, n); memcpy(f->pg, f->g, n);
  memcpy(f->pb, f->b, n); memcpy(f->pa, f->a, n);
}

ZStageFn zp_saveprim_fn(void)
{
  return zc_saveprim;
}

/* ------------------------------------------------------------ fog */

/* C = f * C + (1 - f) * fog colour (GL 1.3 3.10). f is computed per
   vertex, which GL allows, and interpolated perspective-correctly (f/w
   over 1/w, divided every 8 pixels as the texture walk is), so fog on a
   large receding polygon follows depth, not screen position */
static void zf_fog(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  float fz = s->fz, fq = s->fq;
  const float dfz = s->dfzdx, dfq = s->dfqdx;
  int i = 0, e, n = f->n, k, q, fi, dfi;
  while (i < n) {
    float zinv = 1.0f / fz, fv = fq * zinv, dfv = (dfq - fv * dfz) * zinv;
    fi = (int)(fv * 65536.0f);
    dfi = (int)(dfv * 65536.0f);
    e = i + 8 < n ? i + 8 : n;
    for (; i < e; i++) {
#ifdef S31GL_P4ARITH
      k = clamp255(fi >> 16);
#else
      /* phase 5 P: the factor rounded, not floored (glx_prec band 39:
         Mesa mixes with the factor itself) */
      k = clamp255((fi + 32768) >> 16);
#endif
      q = 255 - k;
      f->r[i] = (unsigned char)MIX8(f->r[i], k, p->fogc[0], q);
      f->g[i] = (unsigned char)MIX8(f->g[i], k, p->fogc[1], q);
      f->b[i] = (unsigned char)MIX8(f->b[i], k, p->fogc[2], q);
      fi += dfi;
    }
    fz += 8.0f * dfz; fq += 8.0f * dfq;
  }
}

ZStageFn zp_fog_fn(void)
{
  return zf_fog;
}

/* ------------------------------------------------------------ colour sum */

/* GL_SEPARATE_SPECULAR_COLOR (GL 1.3 3.9): the specular part of lighting
   is added after texturing */
static void zs_smooth(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  int r = s->sr, g = s->sg, b = s->sb, i, n = f->n;
  (void)p;
  for (i = 0; i < n; i++) {
    f->r[i] = (unsigned char)clamp255(f->r[i] + clamp255(r >> ZP_CSHIFT));
    f->g[i] = (unsigned char)clamp255(f->g[i] + clamp255(g >> ZP_CSHIFT));
    f->b[i] = (unsigned char)clamp255(f->b[i] + clamp255(b >> ZP_CSHIFT));
    r += s->dsrdx; g += s->dsgdx; b += s->dsbdx;
  }
}

static void zs_flat(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  int i, n = f->n;
  (void)s;
  for (i = 0; i < n; i++) {
    f->r[i] = (unsigned char)clamp255(f->r[i] + p->flatspec[0]);
    f->g[i] = (unsigned char)clamp255(f->g[i] + p->flatspec[1]);
    f->b[i] = (unsigned char)clamp255(f->b[i] + p->flatspec[2]);
  }
}

ZStageFn zp_spec_fn(int flat)
{
  return flat ? zs_flat : zs_smooth;
}

/* ------------------------------------------------------------ alpha test */

#define ZP_ATEST(name, CMP)                                             \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  int i, n = f->n, ref = p->aref;                                       \
  (void)s;                                                              \
  for (i = 0; i < n; i++)                                               \
    f->m[i] &= (unsigned char)CMP(f->a[i], ref);                        \
}
/* here in GL's own sense: fragment alpha OP reference */
#define A_LESS(a, r) ((a) < (r))
#define A_LEQUAL(a, r) ((a) <= (r))
#define A_GREATER(a, r) ((a) > (r))
#define A_GEQUAL(a, r) ((a) >= (r))
ZP_ATEST(za_less, A_LESS)
ZP_ATEST(za_lequal, A_LEQUAL)
ZP_ATEST(za_greater, A_GREATER)
ZP_ATEST(za_gequal, A_GEQUAL)
ZP_ATEST(za_equal, C_EQUAL)
ZP_ATEST(za_notequal, C_NOTEQUAL)

ZStageFn zp_alpha_fn(int func)
{
  switch (func) {
  case GL_LESS: return za_less;
  case GL_LEQUAL: return za_lequal;
  case GL_GREATER: return za_greater;
  case GL_GEQUAL: return za_gequal;
  case GL_EQUAL: return za_equal;
  default: return za_notequal;
  }
}

/* ------------------------------------------------------------ depth write */

static void zw_write(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  unsigned short *pz = s->pz;
  int i, n = f->n;
  (void)p;
  for (i = 0; i < n; i++)
    if (f->m[i]) pz[i] = f->zz[i];
}

ZStageFn zp_zwrite_fn(void)
{
  return zw_write;
}

/* ------------------------------------------------------------ stencil */

/* phase 4 F8 (GL 1.3 4.1.5): after the alpha test, before the depth test.
   Each stored value s looks up ZPipeX.stab[s]: bit 24 is the stencil
   test's result, and the byte at shift 0 / 8 / 16 is the value to store
   when the stencil test fails / the depth test fails / both pass - so the
   store is always a shift of one table word by 8 * st << zpass, and no GL
   state is tested per fragment. The depth test itself is the batch's
   read-only one (x->st_zfn: f->m = depth pass, f->zz = depth), and the
   depth write is fused (DW). The stencil buffer's dirty range (ZStencilState
   lo/hi, clear.c) grows by the chunk when it is written (RW). A chunk with
   nothing left alive ends the stage list (f->n = 0: every later stage loops
   to f->n, and the runners step by their own count) */
static int zd_never_m(const ZSpan *s, ZFrag *f)
{
  (void)s;
  memset(f->m, 0, f->n);
  return 0;
}

ZDepthFn zp_depth_never_m(void)
{
  return zd_never_m;
}

static inline void zs_track(const ZPipe *p, const unsigned char *ps, int n)
{
  const ZBuffer *zb = p->zb;
  ZStencilState *ss = zb->sst;
  unsigned long off = (unsigned long)ps - (unsigned long)zb->sbuf;
  int lo;
  /* not in the buffer: a line's gathered copy (raster.c made the range
     the whole buffer already) */
  if (off >= (unsigned long)(zb->xsize * zb->ysize)) return;
  lo = (int)off;
  if (lo < ss->lo) ss->lo = lo;
  if (lo + n > ss->hi) ss->hi = lo + n;
}

#define ZS_STAGE(name, LATE, RW, DW)                                    \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  const ZPipeX *x = p->x;                                               \
  const unsigned int *tab = x->stab;                                    \
  unsigned char *ps = x->st_sb + (s->pz - x->st_zb), pre[ZP_CHUNK];     \
  unsigned short *pz = s->pz;                                           \
  int i, n = f->n, alive = 0;                                           \
  unsigned int e, st, m;                                                \
  (void)pz; (void)pre;                                                  \
  if (LATE) memcpy(pre, f->m, n);                                       \
  x->st_zfn(s, f);                    /* f->m: depth pass */            \
  if (RW) zs_track(p, ps, n);                                           \
  for (i = 0; i < n; i++) {                                             \
    if (LATE && !pre[i]) { f->m[i] = 0; continue; }                     \
    e = tab[ps[i]]; st = e >> 24; m = f->m[i];                          \
    if (RW) ps[i] = (unsigned char)(e >> ((st << 3) << m));             \
    m &= st;                                                            \
    if (DW && m) pz[i] = f->zz[i];                                      \
    f->m[i] = (unsigned char)m;                                         \
    alive += (int)m;                                                    \
  }                                                                     \
  if (!alive) f->n = 0;                                                 \
}
ZS_STAGE(zs_e_ro, 0, 0, 0)
ZS_STAGE(zs_e_ro_w, 0, 0, 1)
ZS_STAGE(zs_e_rw, 0, 1, 0)
ZS_STAGE(zs_e_rw_w, 0, 1, 1)
ZS_STAGE(zs_l_ro, 1, 0, 0)
ZS_STAGE(zs_l_ro_w, 1, 0, 1)
ZS_STAGE(zs_l_rw, 1, 1, 0)
ZS_STAGE(zs_l_rw_w, 1, 1, 1)

ZStageFn zp_stencil_fn(int late, int rw, int dwrite)
{
  static const ZStageFn t[8] = {
    zs_e_ro, zs_e_ro_w, zs_e_rw, zs_e_rw_w, zs_l_ro, zs_l_ro_w, zs_l_rw, zs_l_rw_w,
  };
  return t[(late ? 4 : 0) | (rw ? 2 : 0) | (dwrite ? 1 : 0)];
}

/* ------------------------------------------------------------ coverage */

/* phase 4 SMOOTH (GL 1.3 3.4.2 / 3.3.1 antialiasing, 3.11 application):
   the fragment's alpha times its coverage. The coverage is that of the
   primitive's footprint over the pixel, evaluated at the pixel centre from
   its distance to the primitive - the same model Mesa's draw/llvmpipe uses
   for smooth lines (measured: artifacts/gl/phase4/smooth/mesa-probe.txt):
     line:  clamp(w/2 + 1/2 - |d|, 0, 1) * clamp(len/2 + 1/2 - |l|, 0, min(1, len))
            with d across and l along the line from its middle - exact for a
            pixel square against a straight edge, a box filter
     point: clamp(size/2 + 1/2 - r, 0, 1), r the distance to the centre
   Only the smooth rasterisers set cov_kind; for a triangle in the same
   batch the stage returns at once. The span carries the coordinates of its
   first pixel (ZSpan.cva/cvb), a chunk its offset from it */
static void zv_cover(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZPipeX *x = p->x;
  int i, n = f->n;
  float k, a, b;

  if (x->cov_kind == 0) return;
  k = (float)(s->pp - s->cvpp);
  a = s->cva + k * s->cvda;
  b = s->cvb + k * s->cvdb;
  if (x->cov_kind == 1) {
    const float hw = x->cv_hw, hl = x->cv_hl, lc = x->cv_lcap;
    const float da = s->cvda, db = s->cvdb;
    for (i = 0; i < n; i++) {
      float cw = fminf(fmaxf(hw - fabsf(a), 0.0f), 1.0f);
      float cl = fminf(fmaxf(hl - fabsf(b), 0.0f), lc);
      f->a[i] = (unsigned char)((float)f->a[i] * (cw * cl) + 0.5f);
      a += da; b += db;
    }
  } else {
    const float rr = x->cv_rr, b2 = b * b;
    for (i = 0; i < n; i++) {
      float c = fminf(fmaxf(rr - sqrtf(a * a + b2), 0.0f), 1.0f);
      f->a[i] = (unsigned char)((float)f->a[i] * c + 0.5f);
      a += 1.0f;
    }
  }
}

ZStageFn zp_cover_fn(void)
{
  return zv_cover;
}

/* ------------------------------------------------------------ polygon stipple */

/* GL 1.3 3.5.6: fragment (x_w, y_w) survives if bit x_w % 32 of row
   y_w % 32 is set. Only polygons are stippled (stip_on, set by raster.c
   per primitive); the window position comes from the span's address */
static void zst_stipple(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  const ZBuffer *zb = p->zb;
  int off, row, x, yw, i, n = f->n;
  unsigned int bits;
  (void)s;
  if (!p->stip_on) return;
  off = (int)((const char *)s->pp - (const char *)zb->pbuf);
  row = off / zb->linesize;
  x = (off - row * zb->linesize) >> 1;
  yw = zb->ysize - 1 - row;
  bits = p->stip[yw & 31];
  for (i = 0; i < n; i++)
    f->m[i] &= (unsigned char)((bits >> ((x + i) & 31)) & 1);
}

ZStageFn zp_stipple_fn(void)
{
  return zst_stipple;
}

/* ------------------------------------------------------------ blend + store */

static void zo_store(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  PIXEL *pp = s->pp;
  int i, n = f->n;
  (void)p;
  for (i = 0; i < n; i++)
    if (f->m[i]) pp[i] = PACK(f->r[i], f->g[i], f->b[i]);
}

/* the fixed-factor pairs, fused */
#define ZP_OUT(name, BODY)                                              \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  PIXEL *pp = s->pp;                                                    \
  int i, n = f->n, dr, dg, db, sa;                                      \
  unsigned int ks;                                                      \
  (void)p; (void)sa; (void)ks;                                          \
  for (i = 0; i < n; i++) {                                             \
    if (!f->m[i]) continue;                                             \
    UNPACK(pp[i], dr, dg, db);                                          \
    sa = f->a[i];                                                       \
    ks = MUL8K_PREP(sa);           /* (fix 2: once for three products) */ \
    BODY                                                                \
  }                                                                     \
}
#define PUT(R, G, B) pp[i] = PACK(R, G, B);
/* SRC_ALPHA, ONE_MINUS_SRC_ALPHA: two rounded products (Mesa blends in
   8-bit fixed point, each product rounded: glx_prec bands 11, 29) never
   exceed 255 together: round(s a) + round(d (255 - a)) <= round(255 a) +
   round(255 (255 - a)) = 255 */
const unsigned char *zf_btab(ZPipeX *x, int a);
_Static_assert(__builtin_offsetof(ZFrag, a) % 4 == 0, "ZFrag.a is at a word offset");
/* MUL8(v, a) of every 8-bit v (cold: at a chunk whose alpha differs from
   the table's); NULL without memory */
__attribute__((noinline))
static const unsigned char *zp_satab(ZPipeX *x, int a)
{
  int v;
  const unsigned int k = MUL8K_PREP(a);
  if (x->satab == NULL) {
    x->satab = gl_malloc(256);
    if (x->satab == NULL) return NULL;
  }
  for (v = 0; v < 256; v++) x->satab[v] = (unsigned char)MUL8K(v, k);
  x->satab_a = a;
  return x->satab;
}
/* (fix 2, bench P2: the rounded products made this stage +32% over phase
   4.) A chunk of one alpha (a flat colour, glBitmap, an opaque RGB
   texture's 255) takes the destination terms from zf_btab's tables - the
   same MUL8 of the same UNPACK expansions - and the source's with the
   factor prepared once; alpha 255 is the source itself. Otherwise each
   pixel prepares its two factors once for its three channels */
static void zo_sa_omsa(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  PIXEL *pp = s->pp;
  const unsigned char *fr = f->r, *fg = f->g, *fb = f->b, *fa = f->a, *m = f->m;
  const unsigned char *bt, *st;
  int i, n = f->n, a0 = fa[0];
  unsigned int d = 0;
  {
    /* one alpha? a word at a time (ZFrag.a is at a word offset) */
    typedef unsigned int __attribute__((may_alias)) zp_w32a;
    const unsigned int a4 = (unsigned int)a0 * 0x01010101u;
    for (i = 0; i + 4 <= n; i += 4) d |= *(const zp_w32a *)(fa + i) ^ a4;
    for (; i < n; i++) d |= (unsigned int)(fa[i] ^ a0);
  }
  if (d == 0) {
    ZPipeX *x = p->x;
    if (a0 == 255) {
      for (i = 0; i < n; i++)
        if (m[i]) pp[i] = PACK(fr[i], fg[i], fb[i]);
      return;
    }
    bt = x->btab && x->btab_a == a0 ? x->btab : zf_btab(x, a0);
    st = x->satab && x->satab_a == a0 ? x->satab : zp_satab(x, a0);
    if (bt && st) {
      for (i = 0; i < n; i++) {
        unsigned int t = pp[i];
        if (!m[i]) continue;
        pp[i] = PACK(st[fr[i]] + bt[32 + (t >> 11)], st[fg[i]] + bt[128 + ((t >> 5) & 63)],
                     st[fb[i]] + bt[32 + (t & 31)]);
      }
      return;
    }
  }
  for (i = 0; i < n; i++) {
    unsigned int ks, kd;
    int dr, dg, db;
    if (!m[i]) continue;
    UNPACK(pp[i], dr, dg, db);
    ks = MUL8K_PREP(fa[i]);
    kd = MUL8K_PREP(255 - fa[i]);
    pp[i] = PACK(MUL8K(fr[i], ks) + MUL8K(dr, kd), MUL8K(fg[i], ks) + MUL8K(dg, kd),
                 MUL8K(fb[i], ks) + MUL8K(db, kd));
  }
}
ZP_OUT(zo_sa_one, PUT(clamp255(MUL8K(f->r[i], ks) + dr),
                       clamp255(MUL8K(f->g[i], ks) + dg),
                       clamp255(MUL8K(f->b[i], ks) + db)))
ZP_OUT(zo_one_one, PUT(clamp255(f->r[i] + dr), clamp255(f->g[i] + dg),
                        clamp255(f->b[i] + db)))
/* ZERO, SRC_COLOR and DST_COLOR, ZERO: the product */
ZP_OUT(zo_mul, PUT(MUL8(f->r[i], dr), MUL8(f->g[i], dg), MUL8(f->b[i], db)))
/* DST_COLOR, SRC_COLOR: twice the product */
ZP_OUT(zo_mul2, PUT(clamp255(2 * MUL8(f->r[i], dr)), clamp255(2 * MUL8(f->g[i], dg)),
                     clamp255(2 * MUL8(f->b[i], db))))

/* phase 4 BLEND-EQ: the (reverse) subtract of the two commonest pairs,
   fused as the ADD pairs above (clamped at 0: GL 1.4 4.1.7) */
ZP_OUT(zo_sub_one_one, PUT(clamp255(f->r[i] - dr), clamp255(f->g[i] - dg),
                            clamp255(f->b[i] - db)))
ZP_OUT(zo_rsub_one_one, PUT(clamp255(dr - f->r[i]), clamp255(dg - f->g[i]),
                             clamp255(db - f->b[i])))
ZP_OUT(zo_sub_sa_one, PUT(clamp255((int)MUL8K(f->r[i], ks) - dr),
                           clamp255((int)MUL8K(f->g[i], ks) - dg),
                           clamp255((int)MUL8K(f->b[i], ks) - db)))
ZP_OUT(zo_rsub_sa_one, PUT(clamp255(dr - (int)MUL8K(f->r[i], ks)),
                            clamp255(dg - (int)MUL8K(f->g[i], ks)),
                            clamp255(db - (int)MUL8K(f->b[i], ks))))

/* one blend factor for a chunk (GL 1.4 table 4.1; the destination alpha
   factors are folded by raster_sel.c: there is no alpha plane, so dst
   alpha is 1). The constant factors read GL_BLEND_COLOR (ZPipeX.bcol) */
static void factor(const ZPipe *p, int fac, const ZFrag *f, const unsigned char *d,
                   int ch, unsigned char *out)
{
  const unsigned char *src = ch == 0 ? f->r : (ch == 1 ? f->g : f->b);
  int i, n = f->n;
  switch (fac) {
  case GL_ZERO: memset(out, 0, n); break;
  case GL_SRC_COLOR: memcpy(out, src, n); break;
  case GL_ONE_MINUS_SRC_COLOR: for (i = 0; i < n; i++) out[i] = (unsigned char)(255 - src[i]); break;
  case GL_DST_COLOR: memcpy(out, d, n); break;
  case GL_ONE_MINUS_DST_COLOR: for (i = 0; i < n; i++) out[i] = (unsigned char)(255 - d[i]); break;
  case GL_SRC_ALPHA: memcpy(out, f->a, n); break;
  case GL_ONE_MINUS_SRC_ALPHA: for (i = 0; i < n; i++) out[i] = (unsigned char)(255 - f->a[i]); break;
  case GL_CONSTANT_COLOR: memset(out, p->x->bcol[ch], n); break;
  case GL_ONE_MINUS_CONSTANT_COLOR: memset(out, 255 - p->x->bcol[ch], n); break;
  case GL_CONSTANT_ALPHA: memset(out, p->x->bcol[3], n); break;
  case GL_ONE_MINUS_CONSTANT_ALPHA: memset(out, 255 - p->x->bcol[3], n); break;
  default: memset(out, 255, n); break;           /* GL_ONE */
  }
}

/* any pair, the three factor equations and the colour write mask (the
   equation is chosen once per channel of a chunk, never per pixel: this is
   the path of the uncommon pairs, one function for all of them) */
static void zo_generic(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  PIXEL *pp = s->pp;
  unsigned char d[3][ZP_CHUNK], fs[ZP_CHUNK], fd[ZP_CHUNK], res[3][ZP_CHUNK];
  const unsigned char *src[3] = { f->r, f->g, f->b };
  unsigned int cm = p->cmask, v;
  int i, n = f->n, ch, dr, dg, db, eq = p->x->beq;

  for (i = 0; i < n; i++) {
    UNPACK(pp[i], dr, dg, db);
    d[0][i] = (unsigned char)dr; d[1][i] = (unsigned char)dg; d[2][i] = (unsigned char)db;
  }
  for (ch = 0; ch < 3; ch++) {
    const unsigned char *sc = src[ch], *dc = d[ch];
    unsigned char *rc = res[ch];
    factor(p, p->sfactor, f, dc, ch, fs);
    factor(p, p->dfactor, f, dc, ch, fd);
    /* GL 1.4 4.1.7, clamped to [0, 1] */
    if (eq == GL_FUNC_SUBTRACT)
      for (i = 0; i < n; i++) rc[i] = (unsigned char)clamp255(MUL8(sc[i], fs[i]) - MUL8(dc[i], fd[i]));
    else if (eq == GL_FUNC_REVERSE_SUBTRACT)
      for (i = 0; i < n; i++) rc[i] = (unsigned char)clamp255(MUL8(dc[i], fd[i]) - MUL8(sc[i], fs[i]));
    else
      for (i = 0; i < n; i++) rc[i] = (unsigned char)clamp255(MUL8(sc[i], fs[i]) + MUL8(dc[i], fd[i]));
  }
  for (i = 0; i < n; i++) {
    if (!f->m[i]) continue;
    v = PACK(res[0][i], res[1][i], res[2][i]);
    pp[i] = (PIXEL)((v & cm) | (pp[i] & ~cm));
  }
}

/* GL_MIN / GL_MAX: per channel, the factors are not used (GL 1.4 4.1.7);
   on the stored 8-bit expansion of the destination */
#define ZO_MINMAX(name, OP)                                             \
static void name(const ZPipe *p, const ZSpan *s, ZFrag *f)              \
{                                                                       \
  PIXEL *pp = s->pp;                                                    \
  unsigned int cm = p->cmask, v;                                        \
  int i, n = f->n, dr, dg, db;                                          \
  for (i = 0; i < n; i++) {                                             \
    if (!f->m[i]) continue;                                             \
    UNPACK(pp[i], dr, dg, db);                                          \
    v = PACK(OP(f->r[i], dr), OP(f->g[i], dg), OP(f->b[i], db));        \
    pp[i] = (PIXEL)((v & cm) | (pp[i] & ~cm));                          \
  }                                                                     \
}
#define OP_MIN(a_, b_) ((int)(a_) < (b_) ? (int)(a_) : (b_))
#define OP_MAX(a_, b_) ((int)(a_) > (b_) ? (int)(a_) : (b_))
ZO_MINMAX(zo_min, OP_MIN)
ZO_MINMAX(zo_max, OP_MAX)

ZStageFn zp_out_fn(int sf, int df, int cmask, int eq)
{
  switch (eq) {
  case GL_FUNC_SUBTRACT:
    if (cmask == 0xffff && df == GL_ONE && sf == GL_ONE) return zo_sub_one_one;
    if (cmask == 0xffff && df == GL_ONE && sf == GL_SRC_ALPHA) return zo_sub_sa_one;
    return zo_generic;
  case GL_FUNC_REVERSE_SUBTRACT:
    if (cmask == 0xffff && df == GL_ONE && sf == GL_ONE) return zo_rsub_one_one;
    if (cmask == 0xffff && df == GL_ONE && sf == GL_SRC_ALPHA) return zo_rsub_sa_one;
    return zo_generic;
  case GL_MIN: return zo_min;
  case GL_MAX: return zo_max;
  default: break;
  }
  if (cmask == 0xffff) {
    if (sf == GL_ONE && df == GL_ZERO) return zo_store;
    if (sf == GL_SRC_ALPHA && df == GL_ONE_MINUS_SRC_ALPHA) return zo_sa_omsa;
    if (sf == GL_SRC_ALPHA && df == GL_ONE) return zo_sa_one;
    if (sf == GL_ONE && df == GL_ONE) return zo_one_one;
    if ((sf == GL_ZERO && df == GL_SRC_COLOR) || (sf == GL_DST_COLOR && df == GL_ZERO))
      return zo_mul;
    if (sf == GL_DST_COLOR && df == GL_SRC_COLOR) return zo_mul2;
  }
  return zo_generic;
}

/* ------------------------------------------------------------ the runner */

static inline __attribute__((always_inline))
void zp_run_t(ZBuffer *zb, ZSpan *s, const int mt, const int direct)
{
  const ZPipe *p = zb->pipe;
  const ZStageFn *st;
  ZFrag f;
  int n;

  for (;;) {
    n = s->n < ZP_CHUNK ? s->n : ZP_CHUNK;
    f.n = n;
#ifndef S31GL_CENSUS
    if (direct)
      p->x->chunk(p, s, &f);
    else if (p->depth(s, &f))
      for (st = p->st; *st; st++)
        (*st)(p, s, &f);
#else
    if (direct) {
      zp_census_chunk(p, n, n);
      p->x->chunk(p, s, &f);
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
    /* only what the stages read (ZPipe.need) */
    if (p->need & ZP_N_Z) s->z += (unsigned int)(n * s->dzdx);
    if (p->need & ZP_N_RGBA) {
      s->r += n * s->drdx; s->g += n * s->dgdx; s->b += n * s->dbdx;
      s->a += n * s->dadx;
    }
    if (p->need & ZP_N_SPEC) {
      s->sr += n * s->dsrdx; s->sg += n * s->dsgdx; s->sb += n * s->dsbdx;
    }
    if (p->need & ZP_N_ST) {
      s->sz += (float)n * s->dszdx; s->tz += (float)n * s->dtzdx;
    }
    if (p->need & ZP_N_F) s->fq += (float)n * s->dfqdx;
    if (p->need & ZP_N_Q) s->fz += (float)n * s->dfzdx;
    if (p->need & ZP_N_PC) {                /* phase 4 F-PERSP */
      s->rq += (float)n * s->drqdx; s->gq += (float)n * s->dgqdx;
      s->bq += (float)n * s->dbqdx; s->aq += (float)n * s->daqdx;
    }
    if (mt && (p->need & ZP_N_ST1)) {       /* phase 5 O1 */
      s->sz1 += (float)n * s->dszdx1; s->tz1 += (float)n * s->dtzdx1;
    }
  }
}

void zp_run(ZBuffer *zb, ZSpan *s)
{
  S31_RT_ENTER_V(zp_run, zb, s);
  zp_run_t(zb, s, 0, 0);
}

/* phase 5 O1: a batch with texture unit 1 on */
void zp_run_mt(ZBuffer *zb, ZSpan *s)
{
  S31_RT_ENTER_V(zp_run_mt, zb, s);
  zp_run_t(zb, s, 1, 0);
}

/* phase 5 O2 */
void zp_run_mt_direct(ZBuffer *zb, ZSpan *s)
{
  zp_run_t(zb, s, 1, 1);
}
