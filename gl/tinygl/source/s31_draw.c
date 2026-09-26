/*
 * s31_draw.c - the pixel paths of plan F7: glBitmap, glDrawPixels,
 * glCopyPixels, glReadPixels, glCopyTexImage/SubImage, glPolygonStipple.
 * s31, MIT.
 *
 * Fragments of a pixel rectangle or bitmap (GL 1.3 3.6.4-3.7) take the
 * raster position's depth, texture coordinates and fog, and go through the
 * same per-fragment operations as primitives: the general path's stages
 * (zpipe.c), less the ones that belong to primitives (the colour
 * interpolation, the texel walk, the secondary colour and polygon
 * stipple). The colour comes from the image (glDrawPixels) or the raster
 * colour (glBitmap), and the texel is the raster texcoords' one, constant
 * over the rectangle. With no fragment operation enabled - the common case
 * of 2D overlays and bitmap text - pixels are stored directly.
 *
 * Pixel rectangles are clipped to the colour buffer and the scissor box,
 * never to the viewport (GL). Window y points up; the buffer's row 0 is the
 * top. glReadPixels reads the one colour buffer the library has (the back
 * buffer the next swap presents), whatever GL_READ_BUFFER says.
 *
 * Float only (F without D).
 */
#include "zgl.h"
#include "zpipe.h"
#include "raster_int.h"
#include "s31_pixels.h"
#include "s31_fmath.h"

#define PACK565(R, G, B) ((PIXEL)((((R) & 0xf8) << 8) | (((G) & 0xfc) << 3) | ((B) >> 3)))

/* the per-fragment operations for a pixel rectangle; the stage list is
   cached in the context (c->pixpipe) and rebuilt only when gl_build_pipe
   ran (c->pipe_serial), so a run of glBitmap calls - freeglut's text, one
   call a glyph - pays for it once */
typedef struct {
  ZPipe p;                  /* c->pipe with the primitive-only stages dropped */
  unsigned int serial;      /* c->pipe_serial it was made from */
  int direct;               /* no fragment operation: store */
  int whole;                /* glBitmap: a word of bits is one masked chunk
                               (the depth stage writes nothing; phase 4:
                               and no stencil stage) - decided here, once
                               per state change, not per glBitmap call */
  int tex;                  /* the texenv stage runs: idx is tidx */
  unsigned int tidx;
  unsigned int z;           /* the raster depth in TinyGL's zp scale */
  float fq;                 /* the raster fog factor * 255 */
  int box[4];               /* buffer and scissor, x0 y0 x1 y1, rows from the top */
} PixPipe;

static unsigned int raster_zp(float d)
{
  /* vertex.c's window z: (2^30 - 0.5)(1 - d) + 2^13, kept below the wrap
     of the 16 stored bits */
  float z = 1073741824.0f * (1.0f - d) + 8192.0f;
  if (z < 0.0f) z = 0.0f;
  if (z > 1073741823.0f) z = 1073741823.0f;
  return (unsigned int)z;
}

static float fog_factor(GLContext *c, float d)
{
  float f;
  switch (c->fog_mode) {
  case GL_LINEAR:
    f = c->fog_end != c->fog_start ? (c->fog_end - d) / (c->fog_end - c->fog_start) : 1.0f;
    break;
  case GL_EXP:
    f = s31_expf(-c->fog_density * d);
    break;
  default:
    f = c->fog_density * d;
    f = s31_expf(-f * f);
    break;
  }
  return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static inline int clampi(int v, int hi)
{
  return v < 0 ? 0 : (v > hi ? hi : v);
}

/* the stage list for the current state (cold: once per state change;
   out of line, so pix_begin - once per glBitmap - does not pay its
   registers: phase 4 measured +8 instructions a call when inlined) */
__attribute__((noinline))
static void pix_build(GLContext *c, PixPipe *pp)
{
  ZPipe *p = &pp->p;
  ZStageFn drop[10];
  int i, n;

  *p = c->pipe;
  drop[0] = zp_color_fn(0); drop[1] = zp_color_fn(1);
  drop[2] = zp_texidx_fn(0, 0); drop[3] = zp_texidx_fn(0, 1);
  drop[4] = zp_texidx_fn(1, 0); drop[5] = zp_texidx_fn(1, 1);
  drop[6] = zp_spec_fn(0); drop[7] = zp_spec_fn(1);
  drop[8] = zp_stipple_fn();
  drop[9] = zp_cover_fn();                /* phase 4 SMOOTH: primitives only */
  for (i = n = 0; c->pipe.st[i]; i++) {
    int k, keep = 1;
    for (k = 0; k < 10; k++) if (c->pipe.st[i] == drop[k]) keep = 0;
    /* phase 4: the filtered texel stages and the perspective colour
       stage the last triangle may have left in the list; the pixel paths
       sample level 0 nearest (pp->tidx) */
    if (zpx_is_tex_stage(c->pipe.st[i]) || c->pipe.st[i] == zpx_color_pc()) keep = 0;
    if (keep) p->st[n++] = c->pipe.st[i];
  }
  p->st[n] = NULL;
  p->tex = c->pipex.tex0;
  p->talpha = c->pipex.talpha0;
  p->xact = 0;
  p->stip_on = 0;
  pp->tex = c->tex_active;
  pp->direct = p->dsel == ZP_DEPTH_NONE && !pp->tex && !c->fog_enabled &&
               (p->afunc == GL_ALWAYS) && p->sfactor == GL_ONE &&
               p->dfactor == GL_ZERO && p->cmask == 0xffff && !p->nocolor &&
               c->pipex.beq == GL_FUNC_ADD && !RASTER_STENCIL(c);
  pp->whole = !pp->direct && (p->dsel == ZP_DEPTH_NONE || !c->depth_mask) &&
              !RASTER_STENCIL(c);
  pp->serial = c->pipe_serial;
}

/* NULL: nothing can be drawn (no buffer, or no fragment can pass) */
static PixPipe *pix_begin(GLContext *c)
{
  PixPipe *pp;
  int i;

  if (!gl_prepare(c)) return NULL;
  if (c->viewport.updated) {
    gl_eval_viewport(c);
    c->viewport.updated = 0;
  }
  if (c->raster_dirty) gl_update_raster(c);
  if (c->raster_skip) return NULL;
  if (c->pipe_dirty) gl_build_pipe(c);
  pp = c->pixpipe;
  if (pp == NULL) {
    pp = c->pixpipe = gl_malloc(sizeof(PixPipe));
    if (pp == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return NULL; }
    pp->serial = c->pipe_serial - 1;
  }
  if (pp->serial != c->pipe_serial) pix_build(c, pp);
  for (i = 0; i < 4; i++) pp->box[i] = c->rast_box[i];
  if (pp->box[2] <= pp->box[0] || pp->box[3] <= pp->box[1]) return NULL;
  /* what the raster position gives: the texel, the depth, the fog */
  if (pp->tex) {
    const ZPipe *p = &pp->p;
    float q = c->raster_tex[3] != 0.0f ? 1.0f / c->raster_tex[3] : 1.0f;
    float fs = c->raster_tex[0] * q * c->tex_sscale, ft = c->raster_tex[1] * q * c->tex_tscale;
    int si, ti;
    unsigned int col, row;
    fs = fminf(fmaxf(fs, -2.0e9f), 2.0e9f);
    ft = fminf(fmaxf(ft, -2.0e9f), 2.0e9f);
    si = (int)floorf(fs); ti = (int)floorf(ft);
    col = p->clamp_s ? (unsigned int)clampi(si >> p->fbits, p->wmax)
                     : (unsigned int)((si >> p->fbits) & p->wmax);
    row = p->clamp_t ? (unsigned int)clampi(ti >> (p->fbits + p->ws), p->hmax)
                     : (unsigned int)((ti >> (p->fbits + p->ws)) & p->hmax);
    pp->tidx = (row << p->ws) | col;
  } else {
    pp->tidx = 0;
  }
  pp->z = raster_zp(c->raster_pos[2]);
  /* phase 3a G03 (s31_zepoch.c): the raster depth at the epoch's farthest
     step under GL_LESS with stale pixels about, and the epoch's base */
  if ((int)pp->z < c->zb->zguard) zep_materialise(c);
  pp->z += zep_prim(c, pp->z);
  /* the dirty box (s31_zepoch.c): a pixel rectangle may land anywhere */
  if (c->pipe.bact) zdb_grow(c, 0, 0, c->zb->xsize, c->zb->ysize);
  pp->fq = c->fog_enabled ? fog_factor(c, c->raster_fogz) * 255.0f : 255.0f;
  return pp;
}

/* the span constants of a pixel rectangle: the raster depth and fog, no
   interpolation (made once per command, not per run) */
static void pix_span_init(const PixPipe *pp, ZSpan *s)
{
  memset(s, 0, sizeof(*s));
  s->z = pp->z;
  s->fq = pp->fq;
  s->fz = 1.0f;
}

/* n fragments at buffer (x, row) through the stages; rgba: 4 bytes each,
   or NULL for the colour col; zv: per-fragment depths (zp scale) or NULL.
   s: from pix_span_init (its pp/pz are set here) */
static void pix_span(GLContext *c, PixPipe *pp, ZSpan *s, int x, int row, int n,
                     const unsigned char *rgba, const unsigned char *col,
                     const unsigned int *zv)
{
  ZBuffer *zb = c->zb;
  PIXEL *pix = (PIXEL *)((char *)zb->pbuf + row * zb->linesize) + x;
  const ZStageFn *st;
  int i;

  if (pp->direct) {
    if (rgba) {
      for (i = 0; i < n; i++, rgba += 4) pix[i] = PACK565(rgba[0], rgba[1], rgba[2]);
    } else {
      PIXEL v = PACK565(col[0], col[1], col[2]);
      for (i = 0; i < n; i++) pix[i] = v;
    }
    return;
  }
  s->pp = pix;
  s->pz = zb->zbuf + row * zb->xsize + x;
  while (n > 0) {
    ZFrag f;
    int k = n < ZP_CHUNK ? n : ZP_CHUNK;
    f.n = k;
    if (zv) {
      /* per-fragment depth (GL_DEPTH_COMPONENT): one at a time */
      k = f.n = 1;
      s->z = *zv++;
    }
    if (pp->p.depth(s, &f)) {
      for (i = 0; i < k; i++) {
        const unsigned char *q = rgba ? rgba + 4 * i : col;
        f.r[i] = q[0]; f.g[i] = q[1]; f.b[i] = q[2]; f.a[i] = q[3];
        f.idx[i] = pp->tidx;
      }
      for (st = pp->p.st; *st; st++) (*st)(&pp->p, s, &f);
    }
    n -= k;
    s->pp += k; s->pz += k;
    if (rgba) rgba += 4 * k;
  }
}

/* up to 32 fragments at buffer (x, row) whose bit in m is set (bit k =
   pixel x + k), as one chunk through the stages. Only when the depth
   stage writes nothing (test off or depth mask off): a masked fragment
   must not reach the depth buffer, and a fused test-and-write would */
static void pix_masked(GLContext *c, PixPipe *pp, ZSpan *s, int x, int row, int n,
                       unsigned int m, const unsigned char *col)
{
  ZBuffer *zb = c->zb;
  const ZStageFn *st;
  ZFrag f;
  int i;

  s->pp = (PIXEL *)((char *)zb->pbuf + row * zb->linesize) + x;
  s->pz = zb->zbuf + row * zb->xsize + x;
  f.n = n;
  if (!pp->p.depth(s, &f)) return;
  for (i = 0; i < n; i++) {
    f.m[i] &= (unsigned char)((m >> i) & 1);
    f.r[i] = col[0]; f.g[i] = col[1]; f.b[i] = col[2]; f.a[i] = col[3];
    f.idx[i] = pp->tidx;
  }
  for (st = pp->p.st; *st; st++) (*st)(&pp->p, s, &f);
}

/* floor for the raster position (floorf is a libm call) */
static inline int ifloor(float v)
{
  int i = (int)v;
  return i - (v < (float)i);
}

/*
 * s31 render scale (GLContext.rscale, plan G04): the buffer holds the window
 * at 1/2^rs, and every coordinate here is the WINDOW's. rs_px() fetches the
 * buffer pixel under window pixel (wx, wy) - wy counted up from the bottom,
 * GL's way - or returns 0 when it is off the window; rs_read() makes a w x h
 * RGB565 copy of window rectangle (x, y) nearest-neighbour, top row first,
 * as glCopyPixels' snapshot is. Only the scaled paths call them.
 */
static int rs_px(const GLContext *c, int wx, int wy, unsigned int *pix, unsigned int *z)
{
  const ZBuffer *zb = c->zb;
  int rs = c->rscale, row = (zb->ysize << rs) - 1 - wy;
  if (wx < 0 || row < 0 || wx >= (zb->xsize << rs) || row >= (zb->ysize << rs))
    return 0;
  row >>= rs; wx >>= rs;
  if (pix) *pix = ((const PIXEL *)((const char *)zb->pbuf + row * zb->linesize))[wx];
  if (z) *z = zb->zbuf ? zb->zbuf[row * zb->xsize + wx] : 0;
  return 1;
}

static unsigned short *rs_read(GLContext *c, int x, int y, int w, int h)
{
  unsigned short *tmp = gl_malloc(w * h * 2 + 2);
  int i, j;
  if (tmp == NULL) return NULL;
  for (j = 0; j < h; j++) {
    unsigned short *d = tmp + (h - 1 - j) * w;
    for (i = 0; i < w; i++) {
      unsigned int t = 0;
      d[i] = rs_px(c, x + i, y + j, &t, NULL) ? (unsigned short)t : 0;
    }
  }
  return tmp;
}

static void raster_col8(GLContext *c, unsigned char *col)
{
  int i;
  for (i = 0; i < 4; i++) col[i] = f8(c->raster_color[i]);
}

/* ------------------------------------------------------------ glBitmap */

/* the 8 bits of a byte in reverse order (the 3-multiply trick) */
static inline unsigned int rev8(unsigned int v)
{
  return ((((v * 0x0802u) & 0x22110u) | ((v * 0x8020u) & 0x88440u)) * 0x10101u >> 16) & 0xff;
}

/* pixels x .. x+n-1 (n <= 32) of a bitmap row as a word, bit k = pixel x+k */
static unsigned int row_bits(const S31Bits *b, const unsigned char *rp, int x, int n)
{
  unsigned int m = 0;
  int k = b->skip + x, got = 0;
  while (got < n) {
    unsigned int v = rp[k >> 3];
    int sh = k & 7, take = 8 - sh;
    if (take > n - got) take = n - got;
    if (!b->lsb) v = rev8(v);           /* MSB first: pixel 0 is bit 7 */
    m |= ((v >> sh) & ((1u << take) - 1)) << got;
    got += take;
    k += take;
  }
  return m;
}

void glopBitmap(GLContext *c, GLParam *p)
{
  int w = p[1].i, h = p[2].i, canon = p[8].i;
  float xorig = p[3].f, yorig = p[4].f;
  const unsigned char *bits = p[7].p;
  PixPipe *pp;
  S31Bits b;
  ZSpan sp;
  unsigned char col[4];
  PIXEL pv;
  int x0, y0, j, i, bh, whole, lo, hi;

  if (!c->raster_valid) return;   /* GL 1.3 3.7: no fragments, no move */
  if (c->rscale && w > 0 && h > 0 && bits != NULL && (pp = pix_begin(c)) != NULL) {
    /* render scale: each buffer pixel takes the bit under its centre, in
       window units (a glyph at half size; nearest, as the image paths) */
    int rs = c->rscale, xs, xe, ys, ye, bx, by;
    float sc = (float)(1 << rs), fx0, fy0;
    bh = c->zb->ysize;
    fx0 = (float)ifloor(c->raster_pos[0] - xorig + 0.0001f);
    fy0 = (float)ifloor(c->raster_pos[1] - yorig + 0.0001f);
    if (canon) {
      b.base = bits; b.pitch = (w + 7) / 8; b.skip = 0; b.lsb = 0;
    } else {
      s31_bits_setup(c, &b, w, bits);
    }
    raster_col8(c, col);
    pix_span_init(pp, &sp);
    xs = (int)ceilf(fx0 / sc - 0.5f); xe = (int)ceilf((fx0 + (float)w) / sc - 0.5f);
    ys = (int)ceilf(fy0 / sc - 0.5f); ye = (int)ceilf((fy0 + (float)h) / sc - 0.5f);
    if (xs < pp->box[0]) xs = pp->box[0];
    if (xe > pp->box[2]) xe = pp->box[2];
    if (ys < bh - pp->box[3]) ys = bh - pp->box[3];
    if (ye > bh - pp->box[1]) ye = bh - pp->box[1];
    for (by = ys; by < ye; by++) {
      int jj = (int)floorf(((float)by + 0.5f) * sc - fy0), run = -1;
      const unsigned char *rp;
      if (jj < 0 || jj >= h) continue;
      rp = b.base + jj * b.pitch;
      for (bx = xs; bx <= xe; bx++) {
        int ii = (int)floorf(((float)bx + 0.5f) * sc - fx0);
        int on = bx < xe && ii >= 0 && ii < w && (row_bits(&b, rp, ii, 1) & 1u);
        if (on && run < 0) run = bx;
        if (!on && run >= 0) {
          pix_span(c, pp, &sp, run, bh - 1 - by, bx - run, NULL, col, NULL);
          run = -1;
        }
      }
    }
  } else if (w > 0 && h > 0 && bits != NULL && (pp = pix_begin(c)) != NULL) {
    bh = c->zb->ysize;
    /* Mesa's placement (drawpix.c): the lower left at floor(raster -
       origin + epsilon), so integer positions are not moved by rounding */
    x0 = ifloor(c->raster_pos[0] - xorig + 0.0001f);
    y0 = ifloor(c->raster_pos[1] - yorig + 0.0001f);
    if (canon) {
      b.base = bits; b.pitch = (w + 7) / 8; b.skip = 0; b.lsb = 0;
    } else {
      s31_bits_setup(c, &b, w, bits);
    }
    raster_col8(c, col);
    pv = PACK565(col[0], col[1], col[2]);
    if (!pp->direct) pix_span_init(pp, &sp);
    /* with fragment operations, a word of bits is one masked chunk
       unless the depth stage writes (then each run goes alone; phase 4
       F8: so does the stencil stage, before the mask would apply) */
    whole = pp->whole;
    lo = pp->box[0] - x0; hi = pp->box[2] - x0;
    if (lo < 0) lo = 0;
    if (hi > w) hi = w;
    for (j = 0; j < h; j++) {
      int row = bh - 1 - (y0 + j);
      const unsigned char *rp = b.base + j * b.pitch;
      if (row < pp->box[1] || row >= pp->box[3]) continue;
      /* 32 pixels at a time as a word (bit k = pixel i + k), runs of set
         bits found with ctz: a glyph row costs a few instructions, not a
         few per bit */
      for (i = lo; i < hi; i += 32) {
        int n = hi - i < 32 ? hi - i : 32, s0, len;
        unsigned int m = row_bits(&b, rp, i, n), t;
        if (m == 0) continue;
        if (pp->direct) {
          /* no fragment operation (bitmap text): store the runs */
          PIXEL *dst = (PIXEL *)((char *)c->zb->pbuf + row * c->zb->linesize) + x0 + i;
          do {
            s0 = __builtin_ctz(m);
            t = ~(m >> s0);
            len = t ? __builtin_ctz(t) : 32 - s0;
            for (t = 0; (int)t < len; t++) dst[s0 + t] = pv;
            m &= len + s0 >= 32 ? (1u << s0) - 1 : ~(((1u << len) - 1) << s0);
          } while (m);
        } else if (whole) {
          /* one chunk for the word's set range, masked */
          s0 = __builtin_ctz(m);
          pix_masked(c, pp, &sp, x0 + i + s0, row, 32 - __builtin_clz(m) - s0, m >> s0, col);
        } else {
          do {
            s0 = __builtin_ctz(m);
            t = ~(m >> s0);
            len = t ? __builtin_ctz(t) : 32 - s0;
            pix_span(c, pp, &sp, x0 + i + s0, row, len, NULL, col, NULL);
            m &= len + s0 >= 32 ? (1u << s0) - 1 : ~(((1u << len) - 1) << s0);
          } while (m);
        }
      }
    }
  }
  c->raster_pos[0] += p[5].f;
  c->raster_pos[1] += p[6].f;
}

void tgl_bitmap(int w, int h, float xorig, float yorig, float xmove,
                float ymove, const unsigned char *bits)
{
  GLContext *c = gl_get_context();
  GLParam p[9];
  int canon = 0;

  if (w < 0 || h < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  if (c->compile_flag && bits != NULL && w > 0 && h > 0) {
    /* the list keeps the bits, unpacked with this moment's pixel store */
    unsigned char *blk = s31_bits_copy(c, w, h, bits);
    if (blk != NULL) {
      gl_list_own(c, blk);
      bits = blk + S31_BLOCK_HDR;
      canon = 1;
    }
  }
  p[0].op = OP_Bitmap;
  p[1].i = w; p[2].i = h;
  p[3].f = xorig; p[4].f = yorig; p[5].f = xmove; p[6].f = ymove;
  p[7].p = (void *)bits;
  p[8].i = canon;
  gl_add_op(p);
}

/* ------------------------------------------------------------ images */

/* the window pixels whose centres fall in [a, a + z * n) (GL 1.3 3.6.4,
   the zoomed pixel squares), as [*s, *e) */
static void zoom_range(float a, float z, int n, int *s, int *e)
{
  float lo = a, hi = a + z * (float)n;
  if (hi < lo) { float t = lo; lo = hi; hi = t; }
  *s = (int)ceilf(lo - 0.5f);
  *e = (int)ceilf(hi - 0.5f);
}

/* the image column (row) whose zoomed square holds pixel centre x + 0.5 */
static inline int zoom_src(float a, float z, int x, int n)
{
  int i = (int)floorf(((float)x + 0.5f - a) / z);
  return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

#define ROWBUF 256

/* draw a w x h image from u (colour) at the raster position, zoomed */
static void draw_image(GLContext *c, PixPipe *pp, const S31Unpack *u, int w, int h,
                       const unsigned int *depth, const unsigned char *col)
{
  float zx = c->pixel_zoom[0], zy = c->pixel_zoom[1];
  float rx = c->raster_pos[0], ry = c->raster_pos[1];
  int xs, xe, ys, ye, y, bh = c->zb->ysize;
  unsigned char buf[ROWBUF * 4], *full = NULL;
  unsigned int zbuf[ROWBUF];
  int unit;
  ZSpan sp;

  if (c->rscale) {              /* render scale: into buffer units */
    float rf = 1.0f / (float)(1 << c->rscale);
    zx *= rf; zy *= rf; rx *= rf; ry *= rf;
  }
  unit = zx == 1.0f;

  zoom_range(rx, zx, w, &xs, &xe);
  zoom_range(ry, zy, h, &ys, &ye);
  /* clip to the buffer and scissor (rows from the top: box[1..3]) */
  if (xs < pp->box[0]) xs = pp->box[0];
  if (xe > pp->box[2]) xe = pp->box[2];
  if (ys < bh - pp->box[3]) ys = bh - pp->box[3];
  if (ye > bh - pp->box[1]) ye = bh - pp->box[1];
  if (xe <= xs || ye <= ys) return;
  pix_span_init(pp, &sp);
  if (!unit && !depth) {
    full = gl_malloc(w * 4);
    if (full == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
  }
  for (y = ys; y < ye; y++) {
    int j = zoom_src(ry, zy, y, h), row = bh - 1 - y, x;
    if (!unit && !depth) s31_unpack_row(u, 0, j, w, full);
    for (x = xs; x < xe; ) {
      int n = xe - x < ROWBUF ? xe - x : ROWBUF, k;
      if (depth) {
        for (k = 0; k < n; k++) zbuf[k] = depth[j * w + zoom_src(rx, zx, x + k, w)];
        pix_span(c, pp, &sp, x, row, n, NULL, col, zbuf);
      } else if (unit) {
        s31_unpack_row(u, zoom_src(rx, zx, x, w), j, n, buf);
        pix_span(c, pp, &sp, x, row, n, buf, NULL, NULL);
      } else {
        for (k = 0; k < n; k++)
          memcpy(buf + 4 * k, full + 4 * zoom_src(rx, zx, x + k, w), 4);
        pix_span(c, pp, &sp, x, row, n, buf, NULL, NULL);
      }
      x += n;
    }
  }
  gl_free(full);
}

/* phase 4 F8: write a w x h image of stencil indices at the raster
   position, zoomed (GL 1.3 4.3.1: through GL_STENCIL_WRITEMASK, clipped to
   the buffer and scissor box; no other fragment operation applies). The
   caller made sure a stencil buffer exists */
static void draw_stencil(GLContext *c, const unsigned int *v, int w, int h)
{
  float zx = c->pixel_zoom[0], zy = c->pixel_zoom[1];
  float rx = c->raster_pos[0], ry = c->raster_pos[1];
  unsigned int wm = (unsigned int)c->stencil_writemask & 255;
  int xs, xe, ys, ye, x, y, bh = c->zb->ysize, *box = c->rast_box;

  if (wm == 0) return;
  if (c->rscale) {
    float rf = 1.0f / (float)(1 << c->rscale);
    zx *= rf; zy *= rf; rx *= rf; ry *= rf;
  }
  zoom_range(rx, zx, w, &xs, &xe);
  zoom_range(ry, zy, h, &ys, &ye);
  if (xs < box[0]) xs = box[0];
  if (xe > box[2]) xe = box[2];
  if (ys < bh - box[3]) ys = bh - box[3];
  if (ye > bh - box[1]) ye = bh - box[1];
  if (xe <= xs || ye <= ys) return;
  for (y = ys; y < ye; y++) {
    int j = zoom_src(ry, zy, y, h), row = bh - 1 - y;
    for (x = xs; x < xe; x++)
      zst_put(c->zb, x, row, v[j * w + zoom_src(rx, zx, x, w)], wm);
  }
  zst_touch(c->zb, xs, bh - ye, xe, bh - ys);
}

/* the buffers and the box a stencil pixel path works in; 0: nothing to do
   (no buffer: GL_INVALID_OPERATION, GL 1.3 3.6.4 / 4.3.3) */
static int stencil_begin(GLContext *c)
{
  if (!gl_prepare(c)) return 0;
  if (c->zb->sbuf == NULL) { gl_set_error(c, GL_INVALID_OPERATION); return 0; }
  if (c->viewport.updated) {
    gl_eval_viewport(c);
    c->viewport.updated = 0;
  }
  return 1;
}

/* depths of a GL_DEPTH_COMPONENT image in the zp scale; NULL on failure */
static unsigned int *unpack_depth(GLContext *c, int w, int h, int type,
                                  const void *pixels, int *err)
{
  unsigned int *z;
  int x, y, row_len, align, elem, pitch;
  const unsigned char *base;

  *err = 0;
  switch (type) {
  case GL_UNSIGNED_BYTE: case GL_BYTE: elem = 1; break;
  case GL_UNSIGNED_SHORT: case GL_SHORT: elem = 2; break;
  case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: elem = 4; break;
  default: *err = GL_INVALID_ENUM; return NULL;
  }
  z = gl_malloc(w * h * sizeof(*z));
  if (z == NULL) { *err = GL_OUT_OF_MEMORY; return NULL; }
  row_len = c->unpack_row_length > 0 ? c->unpack_row_length : w;
  align = c->unpack_alignment;
  pitch = elem >= align ? elem * row_len : ((elem * row_len + align - 1) / align) * align;
  base = (const unsigned char *)pixels + c->unpack_skip_rows * pitch +
         c->unpack_skip_pixels * elem;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      const unsigned char *q = base + y * pitch + x * elem;
      float d;
      unsigned int v;
      switch (type) {
      case GL_UNSIGNED_BYTE: d = q[0] * (1.0f / 255.0f); break;
      case GL_BYTE: d = (signed char)q[0] * (1.0f / 127.0f); break;
      case GL_UNSIGNED_SHORT: case GL_SHORT:
        v = q[0] | (q[1] << 8);
        if (c->unpack_swap) v = ((v >> 8) | (v << 8)) & 0xffff;
        d = type == GL_SHORT ? (short)v * (1.0f / 32767.0f) : v * (1.0f / 65535.0f);
        break;
      default:
        v = q[0] | (q[1] << 8) | (q[2] << 16) | ((unsigned int)q[3] << 24);
        if (c->unpack_swap)
          v = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
        if (type == GL_FLOAT) { union { unsigned int i; float f; } t; t.i = v; d = t.f; }
        else if (type == GL_INT) d = (float)(int)v * (1.0f / 2147483647.0f);
        else d = (float)(v >> 8) * (1.0f / 16777215.0f);
        break;
      }
      d = d * c->depth_scale + c->depth_bias;
      d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
      /* GL 2.11.1 window z from the depth range */
      d = c->depth_range[0] + (c->depth_range[1] - c->depth_range[0]) * d;
      z[y * w + x] = raster_zp(d);
    }
  return z;
}

void glopDrawPixels(GLContext *c, GLParam *p)
{
  int w = p[1].i, h = p[2].i, format = p[3].i, type = p[4].i, e;
  const void *pixels = p[5].p;
  PixPipe *pp;
  S31Unpack u;

  if (w < 0 || h < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (format == GL_STENCIL_INDEX) {
    /* phase 4 F8: into the stencil buffer (none: INVALID_OPERATION) */
    unsigned int *sv;
    if (!stencil_begin(c)) return;
    if (!c->raster_valid || w == 0 || h == 0 || pixels == NULL) return;
    sv = zst_unpack(c, w, h, type, pixels, &e);
    if (sv == NULL) { if (e) gl_set_error(c, e); return; }
    draw_stencil(c, sv, w, h);
    gl_free(sv);
    return;
  }
  if (format == GL_COLOR_INDEX) {
    gl_warn_once("glDrawPixels(GL_COLOR_INDEX)");
    return;
  }
  if (format == GL_DEPTH_COMPONENT) {
    unsigned int *z;
    unsigned char col[4];
    if (!c->raster_valid || w == 0 || h == 0 || pixels == NULL) return;
    z = unpack_depth(c, w, h, type, pixels, &e);
    if (z == NULL) { gl_set_error(c, e); return; }
    if ((pp = pix_begin(c)) != NULL) {
      raster_col8(c, col);
      /* phase 3a G03 (s31_zepoch.c): per-pixel depths, any of which may
         be anything - the buffer goes back to the plain mapping (the
         depths above are plain), and the next clear is a real one */
      zep_demote(c);
      zep_prim(c, 0x3fffffffu);
      draw_image(c, pp, NULL, w, h, z, col);
    }
    gl_free(z);
    return;
  }
  e = s31_unpack_setup(c, &u, w, h, format, type, pixels);
  if (e) { gl_set_error(c, e); return; }
  if (!c->raster_valid || w == 0 || h == 0 || pixels == NULL) return;
  if ((pp = pix_begin(c)) == NULL) return;
  draw_image(c, pp, &u, w, h, NULL, NULL);
}

void tgl_draw_pixels(int w, int h, int format, int type, const void *pixels)
{
  GLContext *c = gl_get_context();
  GLParam p[6];

  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  if (c->compile_flag && format == GL_DEPTH_COMPONENT)
    gl_warn_once("glDrawPixels(GL_DEPTH_COMPONENT) in a display list (the list keeps the pointer, not a copy)");
  if (c->compile_flag && format == GL_STENCIL_INDEX)
    gl_warn_once("glDrawPixels(GL_STENCIL_INDEX) in a display list (the list keeps the pointer, not a copy)");
  if (c->compile_flag && pixels != NULL && format != GL_DEPTH_COMPONENT &&
      format != GL_STENCIL_INDEX) {
    void *blk = s31_unpack_copy(c, w, h, format, type, pixels);
    if (blk != NULL) {
      gl_list_own(c, blk);
      pixels = (char *)blk + S31_BLOCK_HDR;
      format = S31_PACKED_RGBA;
      type = GL_UNSIGNED_BYTE;
    }
  }
  p[0].op = OP_DrawPixels;
  p[1].i = w; p[2].i = h; p[3].i = format; p[4].i = type;
  p[5].p = (void *)pixels;
  gl_add_op(p);
}

/* ------------------------------------------------------------ glCopyPixels */

void glopCopyPixels(GLContext *c, GLParam *p)
{
  int x = p[1].i, y = p[2].i, w = p[3].i, h = p[4].i, type = p[5].i;
  ZBuffer *zb;
  PixPipe *pp;
  int j, i;

  if (w < 0 || h < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (type == GL_STENCIL) {
    /* phase 4 F8: a copy of the source indices first (the rectangles may
       overlap), then written as glDrawPixels writes them - the index
       shift and offset applied once, at the write (GL 1.3 4.3.3) */
    unsigned int *sv;
    int rs, vw, vh;
    if (!stencil_begin(c)) return;
    if (!c->raster_valid || w == 0 || h == 0) return;
    sv = gl_malloc(w * h * (int)sizeof(*sv));
    if (sv == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
    zb = c->zb; rs = c->rscale; vw = zb->xsize << rs; vh = zb->ysize << rs;
    for (j = 0; j < h; j++)
      for (i = 0; i < w; i++) {
        int wx = x + i, row = vh - 1 - (y + j);
        sv[j * w + i] = (wx >= 0 && wx < vw && row >= 0 && row < vh) ?
                        zb->sbuf[(row >> rs) * zb->xsize + (wx >> rs)] : 0;
      }
    draw_stencil(c, sv, w, h);
    gl_free(sv);
    return;
  }
  if (!c->raster_valid || w == 0 || h == 0) return;
  if ((pp = pix_begin(c)) == NULL) return;
  zb = c->zb;
  if (type == GL_COLOR) {
    /* a copy of the source first: the rectangles may overlap. Kept as
       RGB565 (2 bytes a pixel), read back through the same unpacker,
       which applies the pixel transfer */
    unsigned short *tmp = c->rscale ? rs_read(c, x, y, w, h) : gl_malloc(w * h * 2);
    S31Unpack u;
    if (tmp == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
    for (j = 0; j < h && !c->rscale; j++) {
      int wy = y + j, row = zb->ysize - 1 - wy;
      unsigned short *d = tmp + (h - 1 - j) * w;
      const PIXEL *s = (const PIXEL *)((const char *)zb->pbuf + row * zb->linesize);
      for (i = 0; i < w; i++) {
        int wx = x + i;
        d[i] = (row >= 0 && row < zb->ysize && wx >= 0 && wx < zb->xsize) ? s[wx] : 0;
      }
    }
    s31_unpack_fb(c, &u, tmp, w, h, w * 2, 0, 0, w, h);
    draw_image(c, pp, &u, w, h, NULL, NULL);
    gl_free(tmp);
  } else {
    /* GL_DEPTH: depth fragments with the raster colour */
    unsigned int *z = gl_malloc(w * h * sizeof(*z));
    unsigned char col[4];
    if (z == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
    /* phase 3a G03 (s31_zepoch.c): as glDrawPixels of GL_DEPTH_COMPONENT -
       back to the plain mapping first, then plain depths */
    zep_demote(c);
    zep_prim(c, 0x3fffffffu);
    for (j = 0; j < h; j++) {
      int row = zb->ysize - 1 - (y + j);
      for (i = 0; i < w; i++) {
        int wx = x + i;
        /* outside the buffer: 1.0, as a stored 0 was. Render scale maps
           the window's pixel onto the buffer (rs_px). */
        unsigned int v;
        float d;
        if (c->rscale)
          d = rs_px(c, wx, y + j, NULL, &v) ? zep_depth(zb, v) : 1.0f;
        else
          d = (row >= 0 && row < zb->ysize && wx >= 0 && wx < zb->xsize) ?
              zep_depth(zb, zb->zbuf[row * zb->xsize + wx]) : 1.0f;
        d = d * c->depth_scale + c->depth_bias;
        d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
        z[j * w + i] = raster_zp(d);
      }
    }
    raster_col8(c, col);
    draw_image(c, pp, NULL, w, h, z, col);
    gl_free(z);
  }
}

void tgl_copy_pixels(int x, int y, int w, int h, int type)
{
  GLContext *c = gl_get_context();
  GLParam p[6];
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  p[0].op = OP_CopyPixels;
  p[1].i = x; p[2].i = y; p[3].i = w; p[4].i = h; p[5].i = type;
  gl_add_op(p);
}

/* ------------------------------------------------------------ glReadPixels */

/* not compiled into display lists (GL 1.3 5.4): executes at once */
void tgl_read_pixels(int x, int y, int w, int h, int format, int type, void *pixels)
{
  GLContext *c = gl_get_context();
  ZBuffer *zb;
  S31Pack k;
  float v[4 * 64];
  int e, j, i, x0, x1;

  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  if (w < 0 || h < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (format == GL_STENCIL_INDEX) {
    /* phase 4 F8: from the stencil buffer (none: INVALID_OPERATION) */
    if (pixels == NULL || w == 0 || h == 0) {
      if (!gl_prepare(c)) return;
      if (c->zb->sbuf == NULL) gl_set_error(c, GL_INVALID_OPERATION);
      return;
    }
    if (!gl_prepare(c)) return;
    if (c->zb->sbuf == NULL) { gl_set_error(c, GL_INVALID_OPERATION); return; }
    zst_read(c, x, y, w, h, type, pixels);
    return;
  }
  if (format == GL_COLOR_INDEX) {
    gl_set_error(c, GL_INVALID_OPERATION);     /* RGBA mode */
    return;
  }
  e = s31_pack_setup(c, &k, w, h, format, type, pixels);
  if (e) { gl_set_error(c, e); return; }
  if (pixels == NULL || w == 0 || h == 0 || !gl_prepare(c)) return;
  zb = c->zb;
  if (c->rscale) {
    /* render scale: window pixels sampled from the smaller buffer; only
       the part inside the WINDOW is written */
    int vw = zb->xsize << c->rscale;
    x0 = x < 0 ? 0 : x;
    x1 = x + w > vw ? vw : x + w;
    for (j = 0; j < h; j++) {
      int xx;
      unsigned int t, zv;
      if (!rs_px(c, x0, y + j, &t, &zv)) continue;
      for (xx = x0; xx < x1; ) {
        int n = x1 - xx < 64 ? x1 - xx : 64;
        for (i = 0; i < n; i++) {
          rs_px(c, xx + i, y + j, &t, &zv);
          if (format == GL_DEPTH_COMPONENT) {
            float d = 1.0f - (float)zv * (1.0f / 65535.0f);
            v[i] = d * c->depth_scale + c->depth_bias;
          } else {
            float *q = v + 4 * i;
            int ch;
            q[0] = (float)(t >> 11) * (1.0f / 31.0f);
            q[1] = (float)((t >> 5) & 63) * (1.0f / 63.0f);
            q[2] = (float)(t & 31) * (1.0f / 31.0f);
            q[3] = 1.0f;
            if (c->xfer_active)
              for (ch = 0; ch < 4; ch++) q[ch] = q[ch] * c->xfer_scale[ch] + c->xfer_bias[ch];
          }
        }
        s31_pack_span(&k, xx - x, j, n, v);
        xx += n;
      }
    }
    return;
  }
  /* only the part inside the buffer is written (GL leaves the rest
     undefined; Mesa leaves it untouched too) */
  x0 = x < 0 ? 0 : x;
  x1 = x + w > zb->xsize ? zb->xsize : x + w;
  for (j = 0; j < h; j++) {
    int wy = y + j, row = zb->ysize - 1 - wy, xx;
    const PIXEL *s;
    if (row < 0 || row >= zb->ysize) continue;
    s = (const PIXEL *)((const char *)zb->pbuf + row * zb->linesize);
    if (format == GL_DEPTH_COMPONENT) {
      const unsigned short *zr = zb->zbuf + row * zb->xsize;
      for (xx = x0; xx < x1; ) {
        int n = x1 - xx < 256 ? x1 - xx : 256;
        for (i = 0; i < n; i++) {
          float d = zep_depth(zb, zr[xx + i]);
          v[i] = d * c->depth_scale + c->depth_bias;
        }
        s31_pack_span(&k, xx - x, j, n, v);
        xx += n;
      }
      continue;
    }
    /* the common screenshot case: RGB/RGBA/BGRA bytes, no transfer */
    if (type == GL_UNSIGNED_BYTE && !c->xfer_active &&
        (format == GL_RGB || format == GL_RGBA || format == GL_BGRA)) {
      unsigned char *d = k.base + j * k.pitch + (x0 - x) * k.group;
      for (xx = x0; xx < x1; xx++, d += k.group) {
        unsigned int t = s[xx];
        unsigned char r = s31_c5to8[t >> 11], g = s31_c6to8[(t >> 5) & 63], b = s31_c5to8[t & 31];
        if (format == GL_BGRA) { d[0] = b; d[1] = g; d[2] = r; }
        else { d[0] = r; d[1] = g; d[2] = b; }
        if (format != GL_RGB) d[3] = 255;
      }
      continue;
    }
    for (xx = x0; xx < x1; ) {
      int n = x1 - xx < 64 ? x1 - xx : 64;
      for (i = 0; i < n; i++) {
        unsigned int t = s[xx + i];
        float *q = v + 4 * i;
        int ch;
        q[0] = (float)(t >> 11) * (1.0f / 31.0f);
        q[1] = (float)((t >> 5) & 63) * (1.0f / 63.0f);
        q[2] = (float)(t & 31) * (1.0f / 31.0f);
        q[3] = 1.0f;               /* no alpha planes */
        if (c->xfer_active)
          for (ch = 0; ch < 4; ch++) q[ch] = q[ch] * c->xfer_scale[ch] + c->xfer_bias[ch];
      }
      s31_pack_span(&k, xx - x, j, n, v);
      xx += n;
    }
  }
}

/* ------------------------------------------------------------ copy to textures */

void tgl_copy_tex(int target, int level, int ifmt, int x, int y, int w, int h,
                  int border, int xoff, int yoff, int sub)
{
  GLContext *c = gl_get_context();
  GLParam p[12];
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  p[0].op = OP_CopyTex;
  p[1].i = target; p[2].i = level; p[3].i = ifmt;
  p[4].i = x; p[5].i = y; p[6].i = w; p[7].i = h; p[8].i = border;
  p[9].i = xoff; p[10].i = yoff; p[11].i = sub;
  gl_add_op(p);
}

/* texture.c does the upload; the source is the colour buffer read as
   RGB565 at the time the command executes */

void glopCopyTex(GLContext *c, GLParam *p)
{
  int target = p[1].i, level = p[2].i, ifmt = p[3].i;
  int x = p[4].i, y = p[5].i, w = p[6].i, h = p[7].i, border = p[8].i;
  int is1d = target == GL_TEXTURE_1D;
  GLParam q[10];
  S31Unpack u;
  ZBuffer *zb;

  if (target != GL_TEXTURE_2D && target != GL_TEXTURE_1D) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (w < 0 || h < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (!gl_prepare(c)) return;
  zb = c->zb;
  if (is1d) h = 1;
  if (c->rscale) {
    /* render scale: the window rectangle, sampled from the smaller buffer */
    unsigned short *tmp = rs_read(c, x, y, w, h);
    if (tmp == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
    s31_unpack_fb(c, &u, tmp, w, h, w * 2, 0, 0, w, h);
    q[0].op = p[11].i ? OP_TexSubImage2D : OP_TexImage2D;
    q[1].i = target; q[2].i = level;
    if (p[11].i) {
      q[3].i = p[9].i; q[4].i = is1d ? 0 : p[10].i;
      q[5].i = w; q[6].i = h;
      q[7].i = S31_FB_565; q[8].i = GL_UNSIGNED_SHORT_5_6_5; q[9].p = tmp;
      gl_tex_subimage_src(c, q, &u);
    } else {
      q[3].i = ifmt; q[4].i = w; q[5].i = h; q[6].i = border;
      q[7].i = S31_FB_565; q[8].i = GL_UNSIGNED_SHORT_5_6_5; q[9].p = tmp;
      gl_tex_image_src(c, q, &u);
    }
    gl_free(tmp);
    return;
  }
  s31_unpack_fb(c, &u, zb->pbuf, zb->xsize, zb->ysize, zb->linesize, x, y, w, h);
  q[0].op = p[11].i ? OP_TexSubImage2D : OP_TexImage2D;
  q[1].i = target; q[2].i = level;
  if (p[11].i) {
    q[3].i = p[9].i; q[4].i = is1d ? 0 : p[10].i;
    q[5].i = w; q[6].i = h;
    q[7].i = S31_FB_565; q[8].i = GL_UNSIGNED_SHORT_5_6_5; q[9].p = zb->pbuf;
    gl_tex_subimage_src(c, q, &u);
  } else {
    q[3].i = ifmt; q[4].i = w; q[5].i = h; q[6].i = border;
    q[7].i = S31_FB_565; q[8].i = GL_UNSIGNED_SHORT_5_6_5; q[9].p = zb->pbuf;
    gl_tex_image_src(c, q, &u);
  }
}

/* ------------------------------------------------------------ polygon stipple */

void glopPolygonStipple(GLContext *c, GLParam *p)
{
  int i;
  for (i = 0; i < 32; i++) c->poly_stipple[i] = p[1 + i].ui;
  c->pipe_dirty = 1;
}

/* the mask is unpacked now, with the pixel store of this moment (a
   display list keeps the result, GL 1.3 5.4) */
void tgl_polygon_stipple(const unsigned char *mask)
{
  GLContext *c = gl_get_context();
  GLParam p[33];
  S31Bits b;
  int x, y;

  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  if (mask == NULL) return;
  s31_bits_setup(c, &b, 32, mask);
  p[0].op = OP_PolygonStipple;
  for (y = 0; y < 32; y++) {
    unsigned int r = 0;
    for (x = 0; x < 32; x++) r |= (unsigned int)s31_bit(&b, x, y) << x;
    p[1 + y].ui = r;
  }
  gl_add_op(p);
}

void tgl_get_polygon_stipple(unsigned char *mask)
{
  GLContext *c = gl_get_context();
  int row_len = c->pack_row_length > 0 ? c->pack_row_length : 32;
  int a = c->pack_alignment, bytes = (row_len + 7) / 8;
  int pitch = ((bytes + a - 1) / a) * a, x, y;
  unsigned char *base;

  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  if (mask == NULL) return;
  base = mask + c->pack_skip_rows * pitch;
  for (y = 0; y < 32; y++)
    for (x = 0; x < 32; x++) {
      int k = c->pack_skip_pixels + x;
      unsigned char *q = base + y * pitch + (k >> 3);
      int bit = c->pack_lsb ? (k & 7) : 7 - (k & 7);
      if ((c->poly_stipple[y] >> x) & 1) *q |= (unsigned char)(1 << bit);
      else *q &= (unsigned char)~(1 << bit);
    }
}

/* ------------------------------------------------------------ glGetTexImage */

/* GL 1.3 6.1.4, table 6.1 for the stored classes. A level above 0 is read
   from its stored block (phase 4, texture.c); one that is not stored
   (S31GL_MIPMAPS=0, no memory) is read as level 0 sampled nearest at the
   level's size */
void tgl_get_tex_image(int target, int level, int format, int type, void *pixels)
{
  GLContext *c = gl_get_context();
  GLTexture *t = gl_tex_target(c, target);
  S31Pack k;
  float v[4 * 64];
  int e, w, h, x, y, lum, TW, TH, sh, n, i, fmt;
  const unsigned short *pix;
  const unsigned char *al;

  if (t == NULL) { gl_set_error(c, GL_INVALID_ENUM); return; }
  if (level < 0 || level >= MAX_TEXTURE_LEVELS) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX ||
      format == GL_COLOR_INDEX) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  w = t->lw[level] - 2 * t->border;
  h = t->lh[level] - (target == GL_TEXTURE_1D ? 0 : 2 * t->border);
  if (w <= 0 || h <= 0 || t->images[0].pixmap == NULL || pixels == NULL) return;
  e = s31_pack_setup(c, &k, w, h, format, type, pixels);
  if (e) { gl_set_error(c, e); return; }
  TW = 1 << t->ws; TH = 1 << t->hs;
  lum = t->internal_format == 1 || t->internal_format == 2 ||
        (t->internal_format >= GL_LUMINANCE4 && t->internal_format <= GL_LUMINANCE16_ALPHA16) ||
        t->internal_format == GL_LUMINANCE || t->internal_format == GL_LUMINANCE_ALPHA ||
        t->fmt == TGL_TEXF_INTENSITY;
  sh = level;
  pix = (const unsigned short *)t->images[0].pixmap;
  al = t->alpha;
  fmt = t->fmt;
  if (level > 0 && t->mip && t->mip->l[level].pix) {
    const GLMipLevel *m = &t->mip->l[level];
    int f = t->lfmt[level];
    pix = m->pix; al = m->alpha; fmt = m->cls;
    TW = 1 << m->ws; TH = 1 << m->hs;
    sh = 0;
    lum = f == 1 || f == 2 || (f >= GL_LUMINANCE4 && f <= GL_LUMINANCE16_ALPHA16) ||
          f == GL_LUMINANCE || f == GL_LUMINANCE_ALPHA || fmt == TGL_TEXF_INTENSITY;
  }
  for (y = 0; y < h; y++) {
    int sy = (y << sh) < TH ? (y << sh) : TH - 1;
    for (x = 0; x < w; x += n) {
      n = w - x < 64 ? w - x : 64;
      for (i = 0; i < n; i++) {
        int sx = ((x + i) << sh) < TW ? ((x + i) << sh) : TW - 1;
        int idx = sy * TW + sx;
        unsigned int tx = pix[idx];
        float *q = v + 4 * i, a = al ? al[idx] * (1.0f / 255.0f) : 1.0f;
        q[0] = (float)(tx >> 11) * (1.0f / 31.0f);
        q[1] = (float)((tx >> 5) & 63) * (1.0f / 63.0f);
        q[2] = (float)(tx & 31) * (1.0f / 31.0f);
        q[3] = a;
        if (fmt == TGL_TEXF_ALPHA) q[0] = q[1] = q[2] = 0.0f;
        else if (lum) q[1] = q[2] = 0.0f;
        if (fmt == TGL_TEXF_INTENSITY) q[3] = 1.0f;
        if (c->xfer_active) {
          int ch;
          for (ch = 0; ch < 4; ch++) q[ch] = q[ch] * c->xfer_scale[ch] + c->xfer_bias[ch];
        }
      }
      s31_pack_span(&k, x, y, n, v);
    }
  }
}
