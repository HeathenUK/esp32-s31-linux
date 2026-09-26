/*
 * ztri.h - the one rule that turns a triangle into fragments. s31, MIT.
 *
 * Both rasteriser paths include this: TinyGL's fillers (ztriangle.h, tier 1)
 * and the general filler (ztriangle_gen.c). GL 1.3 Appendix A rule 2 asks
 * that changing blend, alpha test, fog, the depth function or the depth
 * mask never changes which fragments a primitive generates nor their depth,
 * and those are exactly the state changes that move a triangle from one
 * path to the other. So coverage and depth are decided here, once, the same
 * way for both (review finding P1/G1):
 *
 *  - a pixel is covered when its centre (x + 1/2, y + 1/2) is inside the
 *    triangle; a centre exactly on an edge belongs to the triangle on its
 *    right / below (top-left rule), so a mesh covers each pixel once;
 *  - the vertices are GL window coordinates, not snapped (zp.fx / zp.fy);
 *  - each edge's x is 16.16 fixed point, computed from its two end points
 *    at its first row and stepped by a fixed-point slope, so every triangle
 *    sharing that edge gets the same x on every row, bit for bit;
 *  - depth is one integer plane per triangle, z(x, y) = zc + dzdx x + dzdy y
 *    (wrapping unsigned arithmetic), so a fragment's depth does not depend
 *    on where its span starts or which path drew it.
 *
 * The float code that feeds these integers is written with explicit fmaf()
 * wherever a product meets a sum, so the compiler has nothing left to
 * contract (-ffp-contract): every inlined copy computes the same bits.
 * Float only; no double.
 */
#ifndef ZTRI_H
#define ZTRI_H

#include <math.h>

typedef struct ZTri {
  int o[3];                  /* the vertices sorted by y (o[0] the top) */
  float x0, y0;              /* the top vertex */
  float dx1, dy1, dx2, dy2;  /* v1 - v0, v2 - v0 */
  float ia;                  /* 1 / (dx1 dy2 - dx2 dy1) */
  int px, py;                /* the pixel the planes are referenced to */
  float ox, oy;              /* its centre minus the top vertex */
  /* the two parts: rows [ya, yb), left and right edge x at row ya (16.16)
     and their per-row steps */
  struct { int ya, yb, xl, dxl, xr, dxr; } part[2];
  unsigned int zc;           /* depth plane at pixel (0, 0) */
  int dzdx, dzdy;
} ZTri;

/* float -> int: truncate, floor, ceil. On the board each is the one
   F-extension instruction with its rounding mode (fcvt.w.s rtz / rdn /
   rup), which saturates out-of-range values in hardware - a degenerate
   (sliver) triangle's gradients stay in range instead of undefined. The C
   fallback (host builds) saturates the same way. */
#if defined(__riscv) && defined(__riscv_flen)
static inline int ztri_f2i(float v)
{
  int r;
  __asm__("fcvt.w.s %0, %1, rtz" : "=r"(r) : "f"(v));
  return r;
}
static inline int ztri_floor(float v)
{
  int r;
  __asm__("fcvt.w.s %0, %1, rdn" : "=r"(r) : "f"(v));
  return r;
}
static inline int ztri_ceil(float v)
{
  int r;
  __asm__("fcvt.w.s %0, %1, rup" : "=r"(r) : "f"(v));
  return r;
}
#else
static inline int ztri_f2i(float v)
{
  return v >= 2147483520.0f ? 2147483647 : (v <= -2147483648.0f ? (-2147483647 - 1) :
         (v == v ? (int)v : 2147483647));
}
static inline int ztri_floor(float v)
{
  int i = ztri_f2i(v);
  return v < (float)i && i != (-2147483647 - 1) ? i - 1 : i;
}
static inline int ztri_ceil(float v)
{
  int i = ztri_f2i(v);
  return v > (float)i && i != 2147483647 ? i + 1 : i;
}
#endif

/* edge a->b (a above b, ya < yb): x at the centre of row r, and the
   per-row step, both 16.16 */
static inline void ztri_edge(float xa, float ya, float xb, float yb, int r,
                             int *x, int *dx)
{
  float m = (xb - xa) / (yb - ya);
  float xr = fmaf((float)r + 0.5f - ya, m, xa);
  /* a slope too steep for 16.16 saturates: it is an edge with less than
     1/32768 px of height, which has at most one row and is never stepped */
  *x = ztri_f2i(xr * 65536.0f);
  *dx = ztri_f2i(m * 65536.0f);
}

/* Set T up for the triangle (xi, yi, zi) inside the box (x0 y0 x1 y1, rows
   from the top). Returns 0 when it covers no row. */
static inline int ztri_setup(ZTri *T, float xa, float ya, int za,
                             float xb, float yb, int zb, float xc, float yc,
                             int zc, const ZPipe *pipe)
{
  const int *box = pipe->box;
  /* every vertex is inside the box: the clipper cut the triangle to the
     clip volume, which maps to the box (vertex.c, the viewport guard), and
     clamps the vertices it makes (clip.c updateTmp), so no covered pixel
     centre can lie outside and the rows need no per-span clip */
  /* s31 (phase 3a): the sort moves scalars (x, y, z and the index)
     instead of indexing stack arrays - the same values, so the same
     arithmetic below, without the arrays' stores and reloads */
  float x0 = xa, y0 = ya, x1 = xb, y1 = yb, x2 = xc, y2 = yc, tf;
  int z0 = za, z1 = zb, z2 = zc, i0 = 0, i1 = 1, i2 = 2, t, r0, r1, r2, k;
  float area, d1, d2, gzx, gzy;
  int el, del, es0, des0, es1, des1, v1left;
  unsigned int zr;

#define ZTRI_SWAP(a, b) do { tf = x##a; x##a = x##b; x##b = tf; \
    tf = y##a; y##a = y##b; y##b = tf; t = z##a; z##a = z##b; z##b = t; \
    t = i##a; i##a = i##b; i##b = t; } while (0)
  /* sort by y; ties keep the argument order */
  if (y1 < y0) ZTRI_SWAP(0, 1);
  if (y2 < y1) ZTRI_SWAP(1, 2);
  if (y1 < y0) ZTRI_SWAP(0, 1);
#undef ZTRI_SWAP
  T->o[0] = i0; T->o[1] = i1; T->o[2] = i2;
  T->x0 = x0; T->y0 = y0;
  T->dx1 = x1 - x0; T->dy1 = y1 - y0;
  T->dx2 = x2 - x0; T->dy2 = y2 - y0;
  area = fmaf(T->dx1, T->dy2, -(T->dx2 * T->dy1));
  if (!(area != 0.0f) || !(T->dy2 > 0.0f)) return 0;   /* also NaN */
  T->ia = 1.0f / area;

  /* rows whose centre y + 1/2 is in [y0, y2); the upper part's end at y1 */
  r0 = ztri_ceil(y0 - 0.5f);
  r1 = ztri_ceil(y1 - 0.5f);
  r2 = ztri_ceil(y2 - 0.5f);
  if (r0 >= r2 || r2 <= box[1] || r0 >= box[3]) return 0;


  /* the edges, each at its own first row */
  ztri_edge(x0, y0, x2, y2, r0, &el, &del);
  es0 = des0 = es1 = des1 = 0;
  if (r1 > r0) ztri_edge(x0, y0, x1, y1, r0, &es0, &des0);
  if (r2 > r1) ztri_edge(x1, y1, x2, y2, r1, &es1, &des1);
  /* v1 is left of the long edge when the area is negative (y grows down) */
  v1left = area < 0.0f;

  for (k = 0; k < 2; k++) {
    int ya = k ? r1 : r0, yb = k ? r2 : r1, xs = k ? es1 : es0, dxs = k ? des1 : des0;
    /* the long edge runs on; wrapping arithmetic, as only a sliver's
       saturated slope can overflow and a sliver has at most one row */
    int xl = (int)((unsigned int)el + (unsigned int)(ya - r0) * (unsigned int)del);
    if (ya < box[1]) {
      xl = (int)((unsigned int)xl + (unsigned int)(box[1] - ya) * (unsigned int)del);
      xs = (int)((unsigned int)xs + (unsigned int)(box[1] - ya) * (unsigned int)dxs);
      ya = box[1];
    }
    if (yb > box[3]) yb = box[3];
    T->part[k].ya = ya;
    T->part[k].yb = yb;
    if (v1left) {
      T->part[k].xl = xs; T->part[k].dxl = dxs;
      T->part[k].xr = xl; T->part[k].dxr = del;
    } else {
      T->part[k].xl = xl; T->part[k].dxl = del;
      T->part[k].xr = xs; T->part[k].dxr = dxs;
    }
  }

  /* the depth plane, referenced to the centre of the top vertex's pixel */
  T->px = ztri_floor(x0);
  T->py = ztri_floor(y0);
  T->ox = (float)T->px + 0.5f - x0;
  T->oy = (float)T->py + 0.5f - y0;
  d1 = (float)(z1 - z0);
  d2 = (float)(z2 - z0);
  gzx = fmaf(d1, T->dy2, -(d2 * T->dy1)) * T->ia;
  gzy = fmaf(d2, T->dx1, -(d1 * T->dx2)) * T->ia;
  T->dzdx = ztri_f2i(gzx);
  T->dzdy = ztri_f2i(gzy);
  /* wrapping: a sliver's gradients saturate, and its depth is then
     meaningless but defined */
  zr = (unsigned int)z0 + (unsigned int)ztri_floor(fmaf(gzx, T->ox, gzy * T->oy) + 0.5f);
  T->zc = zr - (unsigned int)T->dzdx * (unsigned int)T->px -
          (unsigned int)T->dzdy * (unsigned int)T->py;
  return 1;
}

/* phase 3a G03 (s31_zepoch.c), after ztri_setup, with the triangle's
   three plain vertex depths: record the highest depth the triangle can
   store, then move its plane up by the depth epoch's base. A covered pixel
   centre is inside the triangle, so its plane value is at most the
   highest vertex's (the integer plane's rounding is under 1/10 of a stored
   step); a gradient of 2^30 or more has saturated (a sliver) and its
   values are meaningless: ~0. The base is an integer added to the plane,
   so every stored value is TinyGL's plain one plus the base, bit for bit. */
static inline void ztri_zepoch(ZTri *T, const ZPipe *pipe, int za, int zb, int zc)
{
  /* nothing to record and no base (no epoch can follow this frame's
     depths, or depth is not written): one load and branch */
  if (pipe->zact) {
    unsigned int m = (unsigned int)za, m1 = (unsigned int)zb, m2 = (unsigned int)zc, n;
    /* the lowest depth first: at the epoch's farthest step under GL_LESS
       with stale pixels about, the epoch is materialised (s31_zepoch.c) */
    n = m1 < m ? m1 : m;
    n = m2 < n ? m2 : n;
    if (n < pipe->zguard) zep_tri_far((ZPipe *)pipe);
    m = m1 > m ? m1 : m;
    m = m2 > m ? m2 : m;
    if (__builtin_expect((((unsigned int)T->dzdx + (1u << 30)) |
                          ((unsigned int)T->dzdy + (1u << 30))) >> 31, 0))
      /* a sliver: its plane's values are not bounded by its vertices -
         zep_tri_sliver bounds them from its rows instead (review 3a R4).
         Passing T here costs the flat filler ~0.5 instruction a triangle
         (GCC keeps &T in a callee-saved register: gears +0.08%, glxgears
         +0.1%); an empty-asm copy of T, a cold attribute and moving this
         record keeping into ztri_setup (+0.6%) were measured no better */
      zep_tri_sliver((ZPipe *)pipe, T);
    else if (m > pipe->zchk)
      zep_tri_check((ZPipe *)pipe, m);
    T->zc += pipe->zoff;
  }
}

/* phase 3a dirty boxes (s31_zepoch.c): the rows the triangle can cover,
   [part 0's first, part 1's end), both already inside the box. dbc is the
   recorded rows (an empty range under S31GL_DIRTYBOX=2, whose x extent
   zdb_tri works out from the vertices, off this path) */
static inline void ztri_rows(const ZTri *T, const ZPipe *pipe)
{
  if (pipe->bact) {
    int ya = T->part[0].ya, yb = T->part[1].yb;
    if (ya < pipe->dbc[0] || yb > pipe->dbc[1])
      zdb_tri((ZPipe *)pipe, ya, yb, T->x0, T->dx1, T->dx2);
  }
}

/* first and one-past-last pixel of a row from the edges' 16.16 x: the
   pixels whose centre x + 1/2 is in [xl, xr). They are inside the box by
   construction (the vertices are inside it - see ztri_setup - and an
   edge's fixed-point error is below 1/100 px over the whole buffer). */
#define ZTRI_SPAN(xl, xr, x0, x1) do {                                 \
    (x0) = ((xl) + 0x7fff) >> 16; (x1) = ((xr) + 0x7fff) >> 16;         \
  } while (0)

/* gradients of an attribute given at the sorted vertices */
#define ZTRI_GRAD(T, a0, a1, a2, gx, gy) do {                           \
    float d1_ = (a1) - (a0), d2_ = (a2) - (a0);                         \
    (gx) = (d1_ * (T)->dy2 - d2_ * (T)->dy1) * (T)->ia;                 \
    (gy) = (d2_ * (T)->dx1 - d1_ * (T)->dx2) * (T)->ia;                 \
  } while (0)

/* an integer attribute plane: a(x, y) = c + dx x + dy y at pixel (x, y) */
static inline void ztri_iplane(const ZTri *T, int a0, int a1, int a2,
                               unsigned int *c, int *dx, int *dy)
{
  float gx, gy;
  unsigned int ar;
  ZTRI_GRAD(T, (float)a0, (float)a1, (float)a2, gx, gy);
  *dx = ztri_f2i(gx);
  *dy = ztri_f2i(gy);
  ar = (unsigned int)a0 + (unsigned int)ztri_floor(gx * T->ox + gy * T->oy + 0.5f);
  *c = ar - (unsigned int)*dx * (unsigned int)T->px -
       (unsigned int)*dy * (unsigned int)T->py;
}

#endif
