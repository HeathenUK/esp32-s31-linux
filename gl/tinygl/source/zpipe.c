/*
 * zpipe.c - the stages of the general fragment path. See zpipe.h for the
 * model; raster.c chooses the stages at glBegin. s31, MIT.
 *
 * Every stage is a straight loop over one chunk: which stage runs is
 * decided once per primitive, never per pixel. No double anywhere here
 * (F without D: a double is a libcall on this hart).
 */
#include <string.h>
#include "zgl.h"
#include "zpipe.h"

#define MUL8(x, y) (((x) * ((y) + 1)) >> 8)

static inline int clamp255(int v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* RGB565 -> 8 bits, replicating the high bits (31 -> 255, 63 -> 255) */
#define UNPACK(t, R, G, B) do { unsigned int t_ = (t); \
    (R) = ((t_ >> 8) & 0xf8) | (t_ >> 13); \
    (G) = ((t_ >> 3) & 0xfc) | ((t_ >> 9) & 3); \
    (B) = ((t_ << 3) & 0xf8) | ((t_ >> 2) & 7); } while (0)
/* 8 bits -> RGB565, truncating as RGB_TO_PIXEL does */
#define PACK(R, G, B) ((PIXEL)((((R) & 0xf8) << 8) | (((G) & 0xfc) << 3) | ((B) >> 3)))

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
  return flat ? zc_flat : zc_smooth;
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
  return clamp_t ? zt_rc : zt_rr;
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
#define BLENDK(c, t, k) (MUL8(c, 255 - (t)) + MUL8(k, t))

ZP_TEXENV(ze_replace_rgb, T_RGB SET_C(tr, tg, tb))
ZP_TEXENV(ze_replace_rgba, T_RGB T_A SET_C(tr, tg, tb) f->a[i] = (unsigned char)ta;)
ZP_TEXENV(ze_mod_rgb, T_RGB
  SET_C(MUL8(f->r[i], tr), MUL8(f->g[i], tg), MUL8(f->b[i], tb)))
ZP_TEXENV(ze_mod_rgba, T_RGB T_A
  SET_C(MUL8(f->r[i], tr), MUL8(f->g[i], tg), MUL8(f->b[i], tb))
  f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ze_decal_rgba, T_RGB T_A
  SET_C(MUL8(f->r[i], 255 - ta) + MUL8(tr, ta), MUL8(f->g[i], 255 - ta) + MUL8(tg, ta),
        MUL8(f->b[i], 255 - ta) + MUL8(tb, ta)))
ZP_TEXENV(ze_blend_rgb, T_RGB
  SET_C(BLENDK(f->r[i], tr, p->envc[0]), BLENDK(f->g[i], tg, p->envc[1]),
        BLENDK(f->b[i], tb, p->envc[2])))
ZP_TEXENV(ze_blend_rgba, T_RGB T_A
  SET_C(BLENDK(f->r[i], tr, p->envc[0]), BLENDK(f->g[i], tg, p->envc[1]),
        BLENDK(f->b[i], tb, p->envc[2]))
  f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ze_blend_i, T_RGB T_A
  SET_C(BLENDK(f->r[i], tr, p->envc[0]), BLENDK(f->g[i], tg, p->envc[1]),
        BLENDK(f->b[i], tb, p->envc[2]))
  f->a[i] = (unsigned char)BLENDK(f->a[i], ta, p->envc[3]);)
ZP_TEXENV(ze_alpha_replace, T_A f->a[i] = (unsigned char)ta;)
ZP_TEXENV(ze_alpha_mod, T_A f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ze_add_rgb, T_RGB
  SET_C(clamp255(f->r[i] + tr), clamp255(f->g[i] + tg), clamp255(f->b[i] + tb)))
ZP_TEXENV(ze_add_rgba, T_RGB T_A
  SET_C(clamp255(f->r[i] + tr), clamp255(f->g[i] + tg), clamp255(f->b[i] + tb))
  f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ze_add_i, T_RGB T_A
  SET_C(clamp255(f->r[i] + tr), clamp255(f->g[i] + tg), clamp255(f->b[i] + tb))
  f->a[i] = (unsigned char)clamp255(f->a[i] + ta);)

ZStageFn zp_texenv_fn(int op)
{
  static const ZStageFn t[ZP_TE_N] = {
    ze_replace_rgb, ze_replace_rgba, ze_mod_rgb, ze_mod_rgba, ze_decal_rgba,
    ze_blend_rgb, ze_blend_rgba, ze_blend_i, ze_alpha_replace, ze_alpha_mod,
    ze_add_rgb, ze_add_rgba, ze_add_i,
  };
  return op >= 0 && op < ZP_TE_N ? t[op] : ze_replace_rgb;
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
      k = clamp255(fi >> 16);
      q = 255 - k;
      f->r[i] = (unsigned char)(MUL8(f->r[i], k) + MUL8(p->fogc[0], q));
      f->g[i] = (unsigned char)(MUL8(f->g[i], k) + MUL8(p->fogc[1], q));
      f->b[i] = (unsigned char)(MUL8(f->b[i], k) + MUL8(p->fogc[2], q));
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
  (void)p; (void)sa;                                                    \
  for (i = 0; i < n; i++) {                                             \
    if (!f->m[i]) continue;                                             \
    UNPACK(pp[i], dr, dg, db);                                          \
    sa = f->a[i];                                                       \
    BODY                                                                \
  }                                                                     \
}
#define PUT(R, G, B) pp[i] = PACK(R, G, B);
/* SRC_ALPHA, ONE_MINUS_SRC_ALPHA: the sum of two floors never exceeds 255 */
ZP_OUT(zo_sa_omsa, PUT(MUL8(f->r[i], sa) + MUL8(dr, 255 - sa),
                        MUL8(f->g[i], sa) + MUL8(dg, 255 - sa),
                        MUL8(f->b[i], sa) + MUL8(db, 255 - sa)))
ZP_OUT(zo_sa_one, PUT(clamp255(MUL8(f->r[i], sa) + dr),
                       clamp255(MUL8(f->g[i], sa) + dg),
                       clamp255(MUL8(f->b[i], sa) + db)))
ZP_OUT(zo_one_one, PUT(clamp255(f->r[i] + dr), clamp255(f->g[i] + dg),
                        clamp255(f->b[i] + db)))
/* ZERO, SRC_COLOR and DST_COLOR, ZERO: the product */
ZP_OUT(zo_mul, PUT(MUL8(f->r[i], dr), MUL8(f->g[i], dg), MUL8(f->b[i], db)))
/* DST_COLOR, SRC_COLOR: twice the product */
ZP_OUT(zo_mul2, PUT(clamp255(2 * MUL8(f->r[i], dr)), clamp255(2 * MUL8(f->g[i], dg)),
                     clamp255(2 * MUL8(f->b[i], db))))

/* one blend factor for a chunk (GL 1.3 table 4.1; the destination alpha
   factors are folded by raster.c: there is no alpha plane, so dst alpha
   is 1) */
static void factor(int fac, const ZFrag *f, const unsigned char *d,
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
  default: memset(out, 255, n); break;           /* GL_ONE */
  }
}

/* any pair, and the colour write mask */
static void zo_generic(const ZPipe *p, const ZSpan *s, ZFrag *f)
{
  PIXEL *pp = s->pp;
  unsigned char d[3][ZP_CHUNK], fs[ZP_CHUNK], fd[ZP_CHUNK], res[3][ZP_CHUNK];
  const unsigned char *src[3] = { f->r, f->g, f->b };
  unsigned int cm = p->cmask, v;
  int i, n = f->n, ch, dr, dg, db;

  for (i = 0; i < n; i++) {
    UNPACK(pp[i], dr, dg, db);
    d[0][i] = (unsigned char)dr; d[1][i] = (unsigned char)dg; d[2][i] = (unsigned char)db;
  }
  for (ch = 0; ch < 3; ch++) {
    factor(p->sfactor, f, d[ch], ch, fs);
    factor(p->dfactor, f, d[ch], ch, fd);
    for (i = 0; i < n; i++)
      res[ch][i] = (unsigned char)clamp255(MUL8(src[ch][i], fs[i]) + MUL8(d[ch][i], fd[i]));
  }
  for (i = 0; i < n; i++) {
    if (!f->m[i]) continue;
    v = PACK(res[0][i], res[1][i], res[2][i]);
    pp[i] = (PIXEL)((v & cm) | (pp[i] & ~cm));
  }
}

ZStageFn zp_out_fn(int sf, int df, int cmask)
{
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

void zp_run(ZBuffer *zb, ZSpan *s)
{
  const ZPipe *p = zb->pipe;
  const ZStageFn *st;
  ZFrag f;
  int n;

  for (;;) {
    n = s->n < ZP_CHUNK ? s->n : ZP_CHUNK;
    f.n = n;
    if (p->depth(s, &f))
      for (st = p->st; *st; st++)
        (*st)(p, s, &f);
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
  }
}
