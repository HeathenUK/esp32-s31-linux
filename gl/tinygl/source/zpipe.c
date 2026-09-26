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

/* phase 4 BLEND-EQ: the (reverse) subtract of the two commonest pairs,
   fused as the ADD pairs above (clamped at 0: GL 1.4 4.1.7) */
ZP_OUT(zo_sub_one_one, PUT(clamp255(f->r[i] - dr), clamp255(f->g[i] - dg),
                            clamp255(f->b[i] - db)))
ZP_OUT(zo_rsub_one_one, PUT(clamp255(dr - f->r[i]), clamp255(dg - f->g[i]),
                             clamp255(db - f->b[i])))
ZP_OUT(zo_sub_sa_one, PUT(clamp255(MUL8(f->r[i], sa) - dr), clamp255(MUL8(f->g[i], sa) - dg),
                           clamp255(MUL8(f->b[i], sa) - db)))
ZP_OUT(zo_rsub_sa_one, PUT(clamp255(dr - MUL8(f->r[i], sa)), clamp255(dg - MUL8(f->g[i], sa)),
                            clamp255(db - MUL8(f->b[i], sa))))

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
    if (p->need & ZP_N_PC) {                /* phase 4 F-PERSP */
      s->rq += (float)n * s->drqdx; s->gq += (float)n * s->dgqdx;
      s->bq += (float)n * s->dbqdx; s->aq += (float)n * s->daqdx;
    }
  }
}
