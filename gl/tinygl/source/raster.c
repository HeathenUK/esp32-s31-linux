/*
 * raster.c - which rasteriser path draws a primitive (plan F3-F6), and
 * the general path's triangles, lines and points. s31, MIT.
 *
 * gl_update_raster() (raster_sel.c) runs from glBegin when any state that affects
 * rasterisation changed (c->raster_dirty). It decides, once per primitive
 * batch and never per pixel:
 *
 *  - tier 1, TinyGL's own fillers (ztriangle.c), byte for byte as they
 *    were: untextured flat/smooth, and textures whose environment is the
 *    texel itself (REPLACE or DECAL of an opaque texture, or MODULATE by a
 *    white vertex colour - checked per triangle), REPEAT wrap, depth
 *    LESS/LEQUAL or off, no blending, no alpha test, no fog, all colour
 *    channels written. A frame that uses no new feature takes only this.
 *  - otherwise the general path (zpipe.h): a fixed list of stages chosen
 *    here, run over each span.
 *
 * Lines and points take the general path also when the depth mask is off,
 * when they are textured, or wider than one pixel; TinyGL's line and plot
 * routines do not know those.
 *
 * glPolygonOffset (fill) is a per-triangle wrapper selected here too, so it
 * costs nothing while disabled.
 */
#include "zgl.h"
#include "s31_ramtext.h"
#include "zpipe.h"

#include "raster_int.h"

/* ------------------------------------------------------------ helpers */

/* a [0,1] float to 8.16, clamped */
static inline int f816(float v)
{
  if (v <= 0.0f) return 1 << (ZP_CSHIFT - 1);
  if (v >= 1.0f) return ZP_C1 + (1 << (ZP_CSHIFT - 1));
  return (int)(v * (float)ZP_C1) + (1 << (ZP_CSHIFT - 1));
}

/* the general filler's vertex: GL window position (zp.fx/fy, pixel
   centres), TinyGL's depth (zp.z, which the polygon offset wrapper may have
   moved) - both exactly what tier 1 scan-converts - 8.16 colours, the int
   s, t and q = 1/w */
static void to_vg(GLContext *c, ZVtxG *g, const GLVertex *v)
{
  float winv = v->zp.q;
  /* the same window position and depth tier 1 scan-converts (ztri.h) */
  g->x = v->zp.fx;
  g->y = v->zp.fy;
  g->z = v->zp.z;
  g->r = (float)c816(v->zp.r, ZB_POINT_RED_MIN, KR);
  g->g = (float)c816(v->zp.g, ZB_POINT_GREEN_MIN, KG);
  g->b = (float)c816(v->zp.b, ZB_POINT_BLUE_MIN, KB);
  g->a = (float)f816(v->color.v[3]);
  g->q = winv;
  g->f = c->raster_fog ? v->fog * 255.0f * winv : 0.0f;
  /* the filler forms s/w, t/w from these as tier 1 does */
  g->si = v->zp.s;
  g->ti = v->zp.t;
  if (c->raster_sepspec) {
    g->sr = (float)f816(v->spec.X);
    g->sg = (float)f816(v->spec.Y);
    g->sb = (float)f816(v->spec.Z);
  }
}

/* phase 5 O1: texture unit 1's s, t in its own fixed point, exactly as
   clip.c gl_transform_to_viewport forms unit 0's zp.s, zp.t (the q divide,
   the clamp into the int range) */
static inline void tc1_fixed(const GLContext *c, const GLVertex *v, int *si, int *ti)
{
  float fs = v->tex_coord1.X * c->tex1_sscale;
  float ft = v->tex_coord1.Y * c->tex1_tscale;
  if (v->tex_coord1.W != 1.0f) {
    float iq = v->tex_coord1.W > 1.0e-6f || v->tex_coord1.W < -1.0e-6f ?
               1.0f / v->tex_coord1.W : 1.0e6f;
    fs *= iq;
    ft *= iq;
  }
  fs = fminf(fmaxf(fs, -2.0e9f), 2.0e9f);
  ft = fminf(fmaxf(ft, -2.0e9f), 2.0e9f);
  *si = (int)fs;
  *ti = (int)ft;
}

/* GL_FLAT: the provoking vertex's colour and alpha (clip.c) */
static void set_flat(GLContext *c)
{
  S31_RT_ENTER_V(set_flat, c);   /* phase 6 ramtext */
  ZPipe *p = &c->pipe;
  p->flat[0] = (unsigned char)c8(c->flat_r, ZB_POINT_RED_MIN, KR);
  p->flat[1] = (unsigned char)c8(c->flat_g, ZB_POINT_GREEN_MIN, KG);
  p->flat[2] = (unsigned char)c8(c->flat_b, ZB_POINT_BLUE_MIN, KB);
  p->flat[3] = (unsigned char)(f816(c->flat_vtx ? c->flat_vtx->color.v[3] : 1.0f) >> ZP_CSHIFT);
  if (c->raster_sepspec && c->flat_vtx) {
    p->flatspec[0] = (unsigned char)(f816(c->flat_vtx->spec.X) >> ZP_CSHIFT);
    p->flatspec[1] = (unsigned char)(f816(c->flat_vtx->spec.Y) >> ZP_CSHIFT);
    p->flatspec[2] = (unsigned char)(f816(c->flat_vtx->spec.Z) >> ZP_CSHIFT);
  }
  c->flat_ok = 1;
}

/* ------------------------------------------------------------ triangles */

void gl_draw_triangle_general(GLContext *c, GLVertex *p0, GLVertex *p1,
                              GLVertex *p2)
{
  S31_RT_ENTER_V(gl_draw_triangle_general, c, p0, p1, p2);   /* phase 6 ramtext */
  ZVtxG g[3];

  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 1;                /* polygon stipple applies */
  /* phase 6 V5: once per provoking vertex (clip.c gl_set_provoking_flat
     clears flat_ok), not per triangle: a GL_POLYGON's n - 2 share it */
  if (c->current_shade_model != GL_SMOOTH && !c->flat_ok) set_flat(c);
  to_vg(c, &g[0], p0);
  to_vg(c, &g[1], p1);
  to_vg(c, &g[2], p2);
  ZB_fillTriangleGeneral(c->zb, &g[0], &g[1], &g[2], c->tex_active);
}

/* phase 5 O1: the general path with texture unit 1 on */
void gl_draw_triangle_mt(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
  ZVtxG g[3];

  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 1;                /* polygon stipple applies */
  /* phase 6 V5: once per provoking vertex (clip.c gl_set_provoking_flat
     clears flat_ok), not per triangle: a GL_POLYGON's n - 2 share it */
  if (c->current_shade_model != GL_SMOOTH && !c->flat_ok) set_flat(c);
  to_vg(c, &g[0], p0);
  to_vg(c, &g[1], p1);
  to_vg(c, &g[2], p2);
  tc1_fixed(c, p0, &g[0].si1, &g[0].ti1);
  tc1_fixed(c, p1, &g[1].si1, &g[1].ti1);
  tc1_fixed(c, p2, &g[2].si1, &g[2].ti1);
  ZB_fillTriangleGeneralMT(c->zb, &g[0], &g[1], &g[2], 1);
}

/* GL_MODULATE of an opaque texture: by a white colour it is the texel, so
   TinyGL's own textured filler draws it; any other colour the general
   path */
void gl_draw_triangle_modwhite(GLContext *c, GLVertex *p0, GLVertex *p1,
                               GLVertex *p2)
{
  int white;
  if (c->current_shade_model != GL_SMOOTH) {
    white = c->flat_r == ZB_POINT_RED_MAX && c->flat_g == ZB_POINT_GREEN_MAX &&
            c->flat_b == ZB_POINT_BLUE_MAX;
  } else {
    white = p0->zp.r == ZB_POINT_RED_MAX && p0->zp.g == ZB_POINT_GREEN_MAX &&
            p0->zp.b == ZB_POINT_BLUE_MAX &&
            p1->zp.r == ZB_POINT_RED_MAX && p1->zp.g == ZB_POINT_GREEN_MAX &&
            p1->zp.b == ZB_POINT_BLUE_MAX &&
            p2->zp.r == ZB_POINT_RED_MAX && p2->zp.g == ZB_POINT_GREEN_MAX &&
            p2->zp.b == ZB_POINT_BLUE_MAX;
  }
  if (white) gl_draw_triangle_fill(c, p0, p1, p2);
  else gl_draw_triangle_general(c, p0, p1, p2);
}

/* glPolygonOffset for GL_FILL (GL 1.3 3.5.5): o = factor * m + units * r,
   m the larger depth slope in x or y, r one step of the 16-bit buffer.
   TinyGL's Z grows towards the viewer, so a positive offset subtracts. */
void gl_draw_triangle_offset(GLContext *c, GLVertex *p0, GLVertex *p1,
                             GLVertex *p2)
{
  float dx1 = p1->zp.fx - p0->zp.fx, dy1 = p1->zp.fy - p0->zp.fy;
  float dx2 = p2->zp.fx - p0->zp.fx, dy2 = p2->zp.fy - p0->zp.fy;
  float dz1 = (float)(p1->zp.z - p0->zp.z), dz2 = (float)(p2->zp.z - p0->zp.z);
  float det = dx1 * dy2 - dx2 * dy1, m = 0.0f, o;
  int z[3], i, v;
  GLVertex *pv[3] = { p0, p1, p2 };

  if (det != 0.0f) {
    float inv = 1.0f / det;
    float dzdx = fabsf((dz1 * dy2 - dz2 * dy1) * inv);
    float dzdy = fabsf((dx1 * dz2 - dx2 * dz1) * inv);
    m = dzdx > dzdy ? dzdx : dzdy;
  }
  o = c->offset_factor * m + c->offset_units * (float)(1 << ZB_POINT_Z_FRAC_BITS);
  if (o > 1e9f) o = 1e9f;
  if (o < -1e9f) o = -1e9f;
  for (i = 0; i < 3; i++) {
    z[i] = pv[i]->zp.z;
    v = (int)((float)z[i] - o);
    if (v < 0) v = 0;
    if (v > (1 << 30) - 1) v = (1 << 30) - 1;
    pv[i]->zp.z = v;
  }
  c->draw_fill_inner(c, p0, p1, p2);
  for (i = 0; i < 3; i++) pv[i]->zp.z = z[i];
}

/* ------------------------------------------------------------ lines, points */

/* a line or point end: TinyGL's integer position and depth, 8.16 colours,
   and s/w, t/w, fog/w over q = 1/w, so textures and fog along a line are
   perspective-correct as they are on triangles (the fog and texture stages
   divide by q) */
typedef struct { int x, y, z, r, g, b, a; float s, t, f, q, s1, t1; } LinePt;

static void to_lp(GLContext *c, LinePt *l, const GLVertex *v)
{
  float q = 1.0f / v->pc.W;
  /* the stored depth: plus the depth epoch's base (s31_zepoch.c) */
  l->x = v->zp.x; l->y = v->zp.y; l->z = v->zp.z + (int)c->zb->zoff;
  l->r = c816(v->zp.r, ZB_POINT_RED_MIN, KR);
  l->g = c816(v->zp.g, ZB_POINT_GREEN_MIN, KG);
  l->b = c816(v->zp.b, ZB_POINT_BLUE_MIN, KB);
  l->a = f816(v->color.v[3]);
  l->q = q;
  l->f = c->raster_fog ? v->fog * 255.0f * q : 0.0f;
  if (c->tex_active) {
    /* zp.s, zp.t: already divided by the texture q (clip.c) */
    l->s = (float)v->zp.s * q;
    l->t = (float)v->zp.t * q;
  } else {
    l->s = l->t = 0.0f;
  }
  if (c->vtx_extra & 4) {
    /* phase 5 O1: texture unit 1's (vtx_extra bit 2 is tu1_on, and near
       the context pointer), likewise over q */
    int si, ti;
    tc1_fixed(c, v, &si, &ti);
    l->s1 = (float)si * q;
    l->t1 = (float)ti * q;
  }
  /* (else s1, t1 are not read: ZP_N_ST1 and tu1_on are the same state) */
}

/* ------------------------------------------------------------ smooth */

/* floor and ceil without the libm calls (musl's floorf is a function) */
static inline int ifloorf(float v)
{
  int i = (int)v;
  return i - (v < (float)i);
}
static inline int iceilf(float v)
{
  int i = (int)v;
  return i + (v > (float)i);
}

/* phase 4 SMOOTH: a smooth primitive's attributes as functions of the line
   parameter t (0 at the first end, 1 at the second; a point has one end),
   made once per primitive; aa_span evaluates them per row, only those the
   stages read (ZPipe.need) */
typedef struct {
  float z0, dz, zlo, zhi;
  float r0, dr, g0, dg, b0, db, a0, da;
  float s0, ds, t0, dt_, f0, df, q0, dq;
  float s10, ds1, t10, dt1;     /* phase 5 O1: texture unit 1's */
  float dtdx;                   /* t per pixel along a row */
  int need;
} AAPrim;

static void aa_prim(const ZPipe *p, AAPrim *a, const LinePt *p1, const LinePt *p2,
                    float dtdx)
{
  a->need = p->need;
  a->z0 = (float)p1->z; a->dz = (float)(p2->z - p1->z);
  a->zlo = (float)(p1->z < p2->z ? p1->z : p2->z);
  a->zhi = (float)(p1->z > p2->z ? p1->z : p2->z);
  a->r0 = (float)p1->r; a->dr = (float)(p2->r - p1->r);
  a->g0 = (float)p1->g; a->dg = (float)(p2->g - p1->g);
  a->b0 = (float)p1->b; a->db = (float)(p2->b - p1->b);
  a->a0 = (float)p1->a; a->da = (float)(p2->a - p1->a);
  a->s0 = p1->s; a->ds = p2->s - p1->s;
  a->t0 = p1->t; a->dt_ = p2->t - p1->t;
  a->f0 = p1->f; a->df = p2->f - p1->f;
  a->q0 = p1->q; a->dq = p2->q - p1->q;
  if (a->need & ZP_N_ST1) {                  /* phase 5 O1 */
    a->s10 = p1->s1; a->ds1 = p2->s1 - p1->s1;
    a->t10 = p1->t1; a->dt1 = p2->t1 - p1->t1;
  }
  a->dtdx = dtdx;
}

/* a span of a smooth primitive: row `row`, columns x0..x1-1, the line
   parameter t0 at the first pixel centre, and the coverage coordinates
   (ZSpan.cva/cvb, zpipe.c zv_cover). Depth is kept inside the two ends'
   range: the footprint reaches half a pixel past them */
static inline __attribute__((always_inline))
void aa_span_t(ZBuffer *zb, const AAPrim *a, int row, int x0, int x1,
               float t0, float ca, float cda, float cb, float cdb, const int mt)
{
  ZSpan sp;
  int n = x1 - x0, need = a->need;
  float dt = a->dtdx;

  sp.pp = (PIXEL *)((char *)zb->pbuf + row * zb->linesize) + x0;
  sp.pz = zb->zbuf + row * zb->xsize + x0;
  sp.n = n;
  if (need & ZP_N_Z) {
    float za = fminf(fmaxf(a->z0 + t0 * a->dz, a->zlo), a->zhi);
    float zb1 = fminf(fmaxf(a->z0 + (t0 + dt * (float)(n - 1)) * a->dz, a->zlo), a->zhi);
    sp.z = (unsigned int)za;
    sp.dzdx = n > 1 ? (int)((zb1 - za) / (float)(n - 1)) : 0;
  }
  if (need & ZP_N_RGBA) {
    sp.r = (int)(a->r0 + t0 * a->dr); sp.drdx = (int)(dt * a->dr);
    sp.g = (int)(a->g0 + t0 * a->dg); sp.dgdx = (int)(dt * a->dg);
    sp.b = (int)(a->b0 + t0 * a->db); sp.dbdx = (int)(dt * a->db);
    sp.a = (int)(a->a0 + t0 * a->da); sp.dadx = (int)(dt * a->da);
  }
  if (need & ZP_N_ST) {
    sp.sz = a->s0 + t0 * a->ds; sp.dszdx = dt * a->ds;
    sp.tz = a->t0 + t0 * a->dt_; sp.dtzdx = dt * a->dt_;
  }
  if (need & ZP_N_F) { sp.fq = a->f0 + t0 * a->df; sp.dfqdx = dt * a->df; }
  if (need & ZP_N_Q) { sp.fz = a->q0 + t0 * a->dq; sp.dfzdx = dt * a->dq; }
  sp.cva = ca; sp.cvda = cda; sp.cvb = cb; sp.cvdb = cdb;
  sp.cvpp = sp.pp;
  if (mt) {                                 /* phase 5 O1 */
    sp.sz1 = a->s10 + t0 * a->ds1; sp.dszdx1 = dt * a->ds1;
    sp.tz1 = a->t10 + t0 * a->dt1; sp.dtzdx1 = dt * a->dt1;
    zp_run_mt(zb, &sp);
  } else {
    zp_run(zb, &sp);
  }
}

typedef void (*AASpanFn)(ZBuffer *, const AAPrim *, int, int, int, float, float,
                         float, float, float);
static void aa_span(ZBuffer *zb, const AAPrim *a, int row, int x0, int x1,
                    float t0, float ca, float cda, float cb, float cdb)
{
  aa_span_t(zb, a, row, x0, x1, t0, ca, cda, cb, cdb, 0);
}

/* phase 5 O1: with texture unit 1 on (the primitive picks one of the two) */
static void aa_span_mt(ZBuffer *zb, const AAPrim *a, int row, int x0, int x1,
                       float t0, float ca, float cda, float cb, float cdb)
{
  aa_span_t(zb, a, row, x0, x1, t0, ca, cda, cb, cdb, 1);
}

/* GL_LINE_SMOOTH (GL 1.3 3.4.2): every pixel whose centre is within
   w/2 + 1/2 of the segment across and len/2 + 1/2 of its middle along, its
   alpha times the coverage (zv_cover). The rectangle is walked row by row,
   each row's pixels found by solving the two distances for x; attributes
   are interpolated along the line from the projection of each pixel centre
   onto it, as the aliased walk interpolates them per pixel. Floats only.
   Line stipple is not applied to smooth lines */
__attribute__((noinline))
static void gl_aa_line(GLContext *c, GLVertex *va, GLVertex *vb, int flat)
{
  ZBuffer *zb = c->zb;
  ZPipeX *x = &c->pipex;
  LinePt p1, p2;
  AAPrim ap;
  AASpanFn span;
  float x0 = va->zp.fx, y0 = va->zp.fy, dx = vb->zp.fx - x0, dy = vb->zp.fy - y0;
  float len2 = dx * dx + dy * dy, len, inv, ux, uy, mx, my, hw1, hl, ey, cy0;
  float lo1, hi1, lo2, hi2, slo1, shi1, slo2, shi2;
  int row, r0, r1, bx0 = c->rast_box[0], by0 = c->rast_box[1];
  int bx1 = c->rast_box[2], by1 = c->rast_box[3];

  if (len2 < 1e-12f) return;             /* a zero-length line draws nothing */
  if (c->line_stipple_enabled) gl_note_once("GL_LINE_STIPPLE on smooth lines (not stippled)");
  to_lp(c, &p1, va);
  to_lp(c, &p2, vb);
  if (flat) set_flat(c);
  len = sqrtf(len2);
  inv = 1.0f / len;
  ux = dx * inv; uy = dy * inv;
  mx = x0 + 0.5f * dx; my = y0 + 0.5f * dy;
  hw1 = 0.5f * c->aa_lw + 0.5f;
  hl = 0.5f * len + 0.5f;
  x->cv_hw = hw1; x->cv_hl = hl;
  x->cv_lcap = len < 1.0f ? len : 1.0f;
  x->cov_kind = 1;
  aa_prim(&c->pipe, &ap, &p1, &p2, ux * inv);
  span = ap.need & ZP_N_ST1 ? aa_span_mt : aa_span;
  /* rows whose centre is inside the rectangle's y extent */
  ey = fabsf(uy) * hl + fabsf(ux) * hw1;
  r0 = ifloorf(my - ey - 0.5f) + 1;
  r1 = iceilf(my + ey - 0.5f) - 1;
  if (r0 < by0) r0 = by0;
  if (r1 > by1 - 1) r1 = by1 - 1;
  /* In a row at centre offset cy = row + 1/2 - my, a pixel centre at offset
     X = x - mx is along l = cy uy + X ux and across d = cy ux - X uy. The
     x where |l| < hl and where |d| < w/2 + 1/2 are two intervals whose ends
     are linear in cy: kept as (lo, hi) plus the step per row, in pixel
     column units (+ mx - 1/2), so a row costs four adds. A direction with
     no x component (a vertical line's along, a horizontal one's across)
     bounds only the rows, and the row range already holds it */
  cy0 = (float)r0 + 0.5f - my;
  lo1 = lo2 = -1e30f; hi1 = hi2 = 1e30f;
  slo1 = shi1 = slo2 = shi2 = 0.0f;
  if (fabsf(ux) > 1e-6f) {
    float iu = 1.0f / ux, a = (-hl - cy0 * uy) * iu, b = (hl - cy0 * uy) * iu, st = -uy * iu;
    lo1 = (a < b ? a : b) + mx - 0.5f; hi1 = (a < b ? b : a) + mx - 0.5f;
    slo1 = shi1 = st;
  }
  if (fabsf(uy) > 1e-6f) {
    float iu = 1.0f / uy, a = (cy0 * ux - hw1) * iu, b = (cy0 * ux + hw1) * iu, st = ux * iu;
    lo2 = (a < b ? a : b) + mx - 0.5f; hi2 = (a < b ? b : a) + mx - 0.5f;
    slo2 = shi2 = st;
  }
  for (row = r0; row <= r1; row++, lo1 += slo1, hi1 += shi1, lo2 += slo2, hi2 += shi2) {
    float lo = lo1 > lo2 ? lo1 : lo2, hi = hi1 < hi2 ? hi1 : hi2, cy, cx0, l0;
    int c0, c1;
    if (hi <= lo) continue;
    /* columns whose centre is strictly inside */
    c0 = ifloorf(lo) + 1;
    c1 = iceilf(hi);
    if (c0 < bx0) c0 = bx0;
    if (c1 > bx1) c1 = bx1;
    if (c1 <= c0) continue;
    cy = (float)row + 0.5f - my;
    cx0 = (float)c0 + 0.5f - mx;
    l0 = cy * uy + ux * cx0;
    span(zb, &ap, row, c0, c1, l0 * inv + 0.5f, cy * ux - uy * cx0, -uy, l0, ux);
  }
  x->cov_kind = 0;
}

/* GL_POINT_SMOOTH (GL 1.3 3.3.1): the disk of radius size/2 + 1/2 around
   the point, each pixel's alpha times its coverage clamp(size/2 + 1/2 - r,
   0, 1) at its centre */
__attribute__((noinline))
static void gl_aa_point(GLContext *c, GLVertex *v)
{
  ZBuffer *zb = c->zb;
  ZPipeX *x = &c->pipex;
  LinePt q;
  AAPrim ap;
  AASpanFn span;
  float cx = v->zp.fx, cy = v->zp.fy, rr = 0.5f * c->aa_ps + 0.5f;
  int row, r0, r1, bx0 = c->rast_box[0], by0 = c->rast_box[1];
  int bx1 = c->rast_box[2], by1 = c->rast_box[3];

  to_lp(c, &q, v);
  if (c->current_shade_model != GL_SMOOTH) {
    ZPipe *p = &c->pipe;
    p->flat[0] = (unsigned char)(q.r >> ZP_CSHIFT);
    p->flat[1] = (unsigned char)(q.g >> ZP_CSHIFT);
    p->flat[2] = (unsigned char)(q.b >> ZP_CSHIFT);
    p->flat[3] = (unsigned char)(q.a >> ZP_CSHIFT);
    c->flat_ok = 0;   /* phase 6 V5: not flat_vtx's any more */
  }
  x->cv_rr = rr;
  x->cov_kind = 2;
  aa_prim(&c->pipe, &ap, &q, &q, 0.0f);
  span = ap.need & ZP_N_ST1 ? aa_span_mt : aa_span;
  r0 = ifloorf(cy - rr - 0.5f) + 1;
  r1 = iceilf(cy + rr - 0.5f) - 1;
  if (r0 < by0) r0 = by0;
  if (r1 > by1 - 1) r1 = by1 - 1;
  for (row = r0; row <= r1; row++) {
    float dy = (float)row + 0.5f - cy, h2 = rr * rr - dy * dy, h;
    int c0, c1;
    if (h2 <= 0.0f) continue;
    h = sqrtf(h2);
    c0 = ifloorf(cx - h - 0.5f) + 1;
    c1 = iceilf(cx + h - 0.5f);
    if (c0 < bx0) c0 = bx0;
    if (c1 > bx1) c1 = bx1;
    if (c1 <= c0) continue;
    span(zb, &ap, row, c0, c1, 0.0f, (float)c0 + 0.5f - cx, 1.0f, dy, 0.0f);
  }
  x->cov_kind = 0;
}

/* GL lines through the general path: TinyGL's Bresenham walk (zline.h),
   and for width w > 1 the w pixels of the minor axis GL 1.3 3.4.2 asks
   for, clipped to the buffer and scissor box.

   The pixels go through the stages as spans, not one by one (review P3:
   one zp_run per pixel cost ~300 instructions a pixel, 31x TinyGL's
   line). A line's pixels are not contiguous in the frame buffer, but every
   attribute is linear along it, so each of the w parallel copies is
   gathered - its colour and depth pixels copied into a contiguous buffer -
   run through zp_run as one span whose per-"x" steps are the per-pixel
   steps, and scattered back. A line visits each pixel once, so nothing is
   read after it is written. Pixels outside the box, or off in the stipple,
   keep their place in the span (the attributes stay linear) but are not
   scattered: their fragments are discarded. */
#define LN_BUF 128
void gl_general_line(GLContext *c, GLVertex *va, GLVertex *vb, int flat)
{
  ZBuffer *zb = c->zb;
  LinePt p1, p2, tmp;
  int dx, dy, sx, n, i, k, e, xmaj, x, y, w = c->line_w, k0;
  int dcol[4];
  float dpf[4], inv, dpf1[2] = { 0.0f, 0.0f };
  int dz, stip = c->line_stipple_enabled, sfac = c->line_stipple_factor, rev = 0, s0;
  unsigned int spat = (unsigned int)c->line_stipple_pattern;
  int bx0 = c->rast_box[0], by0 = c->rast_box[1], bx1 = c->rast_box[2], by1 = c->rast_box[3];
  PIXEL pbuf[LN_BUF];
  unsigned short zbuf[LN_BUF];
  PIXEL *pa[LN_BUF];          /* where each came from, or NULL: discard */
  unsigned short *za[LN_BUF];
  /* phase 4 F8: with the stencil test, its values are gathered and
     scattered the same way, in a pass of their own per block (sten: the
     test runs), so the pixel walk is the same loop with or without it */
  unsigned char sbuf[LN_BUF];
  int sten = RASTER_STENCIL(c);

  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 0;
  if (c->pipe.xact) zpx_reset(&c->pipe);   /* phase 4: level 0, affine colour */
  if (RASTER_AA_LINES(c)) {                /* phase 4 SMOOTH */
    gl_aa_line(c, va, vb, flat);
    return;
  }
  /* phase 4 F8: the stages see the gathered copies (ZPipeX.st_zb/st_sb
     point at them while the line runs), not the buffer, so the stencil's
     dirty range cannot follow them: it becomes the whole buffer */
  if (sten) {
    if (RASTER_STENCIL_W(c)) zst_touch_all(zb);
    c->pipex.st_zb = zbuf;
    c->pipex.st_sb = sbuf;
  }
  /* line stipple (GL 1.3 3.4.2, plan F7): the counter restarts at each
     independent segment and at the first segment of a strip or loop,
     and runs on along the strip */
  if (stip && (c->begin_type == GL_LINES || c->vertex_cnt <= 2))
    c->line_stipple_counter = 0;
  to_lp(c, &p1, va);
  to_lp(c, &p2, vb);
  if (flat) set_flat(c);
  if (p1.y > p2.y || (p1.y == p2.y && p1.x > p2.x)) {
    tmp = p1; p1 = p2; p2 = tmp;
    rev = 1;                          /* the stipple counts from va */
  }
  dx = p2.x - p1.x; dy = p2.y - p1.y;
  sx = dx < 0 ? -1 : 1;
  if (dx < 0) dx = -dx;
  xmaj = dx >= dy;
  n = xmaj ? dx : dy;
  inv = n ? 1.0f / (float)n : 0.0f;
  s0 = c->line_stipple_counter;
  if (stip) c->line_stipple_counter += n;   /* a strip runs on by its length */
  if (sfac < 1) sfac = 1;
  dcol[0] = n ? (p2.r - p1.r) / n : 0;
  dcol[1] = n ? (p2.g - p1.g) / n : 0;
  dcol[2] = n ? (p2.b - p1.b) / n : 0;
  dcol[3] = n ? (p2.a - p1.a) / n : 0;
  dpf[0] = (p2.s - p1.s) * inv; dpf[1] = (p2.t - p1.t) * inv;
  dpf[2] = (p2.f - p1.f) * inv; dpf[3] = (p2.q - p1.q) * inv;
  if (c->tu1_on) {                    /* phase 5 O1 */
    dpf1[0] = (p2.s1 - p1.s1) * inv; dpf1[1] = (p2.t1 - p1.t1) * inv;
  }
  dz = n ? (p2.z - p1.z) / n : 0;
  k0 = -(w - 1) / 2;

  for (k = k0; k < k0 + w; k++) {
    /* this copy's pixels are (x, y + k) (x-major) or (x + k, y); all of
       them inside the box needs no per-pixel test (the common width 1:
       the clipper bounded the line) */
    int inside, q = 0, r = 0, dir = 1, i0 = 0, j = 0, jj;
    ZSpan sp;
    if (xmaj)
      inside = p1.y + k >= by0 && p2.y + k < by1 && p1.x >= bx0 && p1.x < bx1 &&
               p2.x >= bx0 && p2.x < bx1;
    else
      inside = p1.y >= by0 && p2.y < by1 && p1.x + k >= bx0 && p1.x + k < bx1 &&
               p2.x + k >= bx0 && p2.x + k < bx1;
    if (stip) {
      /* the stipple position of pixel i is s0 + i (s0 + n - i reversed):
         one divide per line, then a counter (the divide per pixel was the
         other half of P3) */
      int p0s = s0 + (rev ? n : 0);
      q = p0s / sfac; r = p0s % sfac;
      dir = rev ? -1 : 1;
    }
    x = p1.x; y = p1.y;
    e = xmaj ? 2 * dy - dx : 2 * dx - dy;
    sp.pp = pbuf; sp.pz = zbuf;
    for (i = 0; i <= n; i++) {
      int px = xmaj ? x : x + k, py = xmaj ? y + k : y, on = 1;
      if (stip) {
        on = (spat >> (q & 15)) & 1;
        r += dir;
        if (r == sfac) { r = 0; q++; } else if (r < 0) { r = sfac - 1; q--; }
      }
      if (on && (inside || (px >= bx0 && px < bx1 && py >= by0 && py < by1))) {
        PIXEL *pp = (PIXEL *)((char *)zb->pbuf + py * zb->linesize) + px;
        unsigned short *pz = zb->zbuf + py * zb->xsize + px;
        pbuf[j] = *pp; zbuf[j] = *pz;
        pa[j] = pp; za[j] = pz;
      } else {
        pa[j] = NULL;
      }
      j++;
      if (xmaj) {
        x += sx;
        if (e > 0) { y++; e -= 2 * (dx - dy); } else e += 2 * dy;
      } else {
        y++;
        if (e > 0) { x += sx; e -= 2 * (dy - dx); } else e += 2 * dx;
      }
      if (j == LN_BUF || i == n) {
        /* pixels i0 .. i0 + j - 1 of the line, as one span */
        if (sten)
          for (jj = 0; jj < j; jj++)
            sbuf[jj] = pa[jj] ? zb->sbuf[za[jj] - zb->zbuf] : 0;
        sp.pp = pbuf; sp.pz = zbuf; sp.n = j;
        sp.z = (unsigned int)p1.z + (unsigned int)i0 * (unsigned int)dz;
        sp.dzdx = dz;
        sp.r = p1.r + i0 * dcol[0]; sp.g = p1.g + i0 * dcol[1];
        sp.b = p1.b + i0 * dcol[2]; sp.a = p1.a + i0 * dcol[3];
        sp.drdx = dcol[0]; sp.dgdx = dcol[1]; sp.dbdx = dcol[2]; sp.dadx = dcol[3];
        /* lines and points carry no secondary colour (lit, textured lines
           with GL_SEPARATE_SPECULAR_COLOR lose the highlight) */
        sp.sr = sp.sg = sp.sb = sp.dsrdx = sp.dsgdx = sp.dsbdx = 0;
        sp.sz = p1.s + (float)i0 * dpf[0]; sp.tz = p1.t + (float)i0 * dpf[1];
        sp.fq = p1.f + (float)i0 * dpf[2]; sp.fz = p1.q + (float)i0 * dpf[3];
        sp.dszdx = dpf[0]; sp.dtzdx = dpf[1]; sp.dfqdx = dpf[2]; sp.dfzdx = dpf[3];
        if (c->tu1_on) {
          /* phase 5 O1 */
          sp.sz1 = p1.s1 + (float)i0 * dpf1[0]; sp.tz1 = p1.t1 + (float)i0 * dpf1[1];
          sp.dszdx1 = dpf1[0]; sp.dtzdx1 = dpf1[1];
          zp_run_mt(zb, &sp);
        } else {
          zp_run(zb, &sp);
        }
        for (jj = 0; jj < j; jj++)
          if (pa[jj]) { *pa[jj] = pbuf[jj]; *za[jj] = zbuf[jj]; }
        if (sten)
          for (jj = 0; jj < j; jj++)
            if (pa[jj]) zb->sbuf[za[jj] - zb->zbuf] = sbuf[jj];
        i0 += j;
        j = 0;
      }
    }
  }
  if (sten) {                         /* the buffers' own again */
    c->pipex.st_zb = zb->zbuf;
    c->pipex.st_sb = zb->sbuf;
  }
}

/* a point of integer size w: a w x w square, one span per row; a point is
   its own provoking vertex */
void gl_general_point(GLContext *c, GLVertex *v)
{
  ZBuffer *zb = c->zb;
  LinePt q;
  int w = c->point_w, x0, x1, y0, y1, y;
  ZSpan sp;

  /* phase 3a G03 (s31_zepoch.c); to_lp adds the epoch's base */
  if (v->zp.z < zb->zguard) zep_materialise(c);
  zep_prim(c, (unsigned int)v->zp.z);
  if (c->pipe.bact)            /* the dirty box: the square either side */
    zdb_grow(c, v->zp.x - w, v->zp.y - w, v->zp.x + w + 1, v->zp.y + w + 1);
  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 0;
  if (c->pipe.xact) zpx_reset(&c->pipe);   /* phase 4: level 0, affine colour */
  if (RASTER_AA_POINTS(c)) {               /* phase 4 SMOOTH */
    gl_aa_point(c, v);
    return;
  }
  to_lp(c, &q, v);
  if (c->current_shade_model != GL_SMOOTH) {
    ZPipe *p = &c->pipe;
    p->flat[0] = (unsigned char)(q.r >> ZP_CSHIFT);
    p->flat[1] = (unsigned char)(q.g >> ZP_CSHIFT);
    p->flat[2] = (unsigned char)(q.b >> ZP_CSHIFT);
    p->flat[3] = (unsigned char)(q.a >> ZP_CSHIFT);
    c->flat_ok = 0;   /* phase 6 V5: not flat_vtx's any more */
  }
  if (w & 1) {
    /* odd: centred on the pixel holding the point - TinyGL's snapped
       position, which size 1 shares with tier 1's ZB_plot, so a point does
       not move when a state change switches its path */
    x0 = q.x - (w - 1) / 2;
    y0 = q.y - (w - 1) / 2;
  } else {
    /* even (GL 1.3 3.3): centred on the pixel corner nearest the point,
       (floor(xw + 1/2), floor(yw + 1/2)). In rows from the top, with
       fy = H - yw, the first row is ceil(fy - 1/2) - w/2 (review G6: it
       was TinyGL's row minus (w-1)/2, one row low) */
    x0 = (int)floorf(v->zp.fx + 0.5f) - w / 2;
    y0 = (int)ceilf(v->zp.fy - 0.5f) - w / 2;
  }
  x1 = x0 + w;
  y1 = y0 + w;
  if (x0 < c->rast_box[0]) x0 = c->rast_box[0];
  if (y0 < c->rast_box[1]) y0 = c->rast_box[1];
  if (x1 > c->rast_box[2]) x1 = c->rast_box[2];
  if (y1 > c->rast_box[3]) y1 = c->rast_box[3];
  for (y = y0; y < y1; y++) {
    if (x1 <= x0) break;
    sp.pp = (PIXEL *)((char *)zb->pbuf + y * zb->linesize) + x0;
    sp.pz = zb->zbuf + y * zb->xsize + x0;
    sp.n = x1 - x0;
    sp.z = (unsigned int)q.z; sp.dzdx = 0;
    sp.r = q.r; sp.g = q.g; sp.b = q.b; sp.a = q.a;
    sp.drdx = sp.dgdx = sp.dbdx = sp.dadx = 0;
    sp.sr = sp.sg = sp.sb = sp.dsrdx = sp.dsgdx = sp.dsbdx = 0;
    sp.sz = q.s; sp.tz = q.t; sp.fq = q.f; sp.fz = q.q;
    sp.dszdx = sp.dtzdx = sp.dfqdx = sp.dfzdx = 0.0f;
    if (c->tu1_on) {
      /* phase 5 O1 */
      sp.sz1 = q.s1; sp.tz1 = q.t1; sp.dszdx1 = sp.dtzdx1 = 0.0f;
      zp_run_mt(zb, &sp);
    } else {
      zp_run(zb, &sp);
    }
  }
}
