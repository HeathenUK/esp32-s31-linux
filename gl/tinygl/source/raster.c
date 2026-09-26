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

/* GL_FLAT: the provoking vertex's colour and alpha (clip.c) */
static void set_flat(GLContext *c)
{
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
}

/* ------------------------------------------------------------ triangles */

void gl_draw_triangle_general(GLContext *c, GLVertex *p0, GLVertex *p1,
                              GLVertex *p2)
{
  ZVtxG g[3];

  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 1;                /* polygon stipple applies */
  if (c->current_shade_model != GL_SMOOTH) set_flat(c);
  to_vg(c, &g[0], p0);
  to_vg(c, &g[1], p1);
  to_vg(c, &g[2], p2);
  ZB_fillTriangleGeneral(c->zb, &g[0], &g[1], &g[2], c->tex_active);
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
typedef struct { int x, y, z, r, g, b, a; float s, t, f, q; } LinePt;

static void to_lp(GLContext *c, LinePt *l, const GLVertex *v)
{
  float q = 1.0f / v->pc.W;
  l->x = v->zp.x; l->y = v->zp.y; l->z = v->zp.z;
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
  float dpf[4], inv;
  int dz, stip = c->line_stipple_enabled, sfac = c->line_stipple_factor, rev = 0, s0;
  unsigned int spat = (unsigned int)c->line_stipple_pattern;
  int bx0 = c->rast_box[0], by0 = c->rast_box[1], bx1 = c->rast_box[2], by1 = c->rast_box[3];
  PIXEL pbuf[LN_BUF];
  unsigned short zbuf[LN_BUF];
  PIXEL *pa[LN_BUF];          /* where each came from, or NULL: discard */
  unsigned short *za[LN_BUF];

  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 0;
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
        zp_run(zb, &sp);
        for (jj = 0; jj < j; jj++)
          if (pa[jj]) { *pa[jj] = pbuf[jj]; *za[jj] = zbuf[jj]; }
        i0 += j;
        j = 0;
      }
    }
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

  if (c->pipe_dirty) gl_build_pipe(c);
  c->pipe.stip_on = 0;
  to_lp(c, &q, v);
  if (c->current_shade_model != GL_SMOOTH) {
    ZPipe *p = &c->pipe;
    p->flat[0] = (unsigned char)(q.r >> ZP_CSHIFT);
    p->flat[1] = (unsigned char)(q.g >> ZP_CSHIFT);
    p->flat[2] = (unsigned char)(q.b >> ZP_CSHIFT);
    p->flat[3] = (unsigned char)(q.a >> ZP_CSHIFT);
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
    zp_run(zb, &sp);
  }
}
