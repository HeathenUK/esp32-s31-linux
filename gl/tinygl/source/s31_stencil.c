/*
 * s31_stencil.c - the stencil buffer (phase 4 F8-STENCIL): its state,
 * glClear of it, and the stencil index pixel paths. s31, MIT.
 *
 * The buffer is 8 bits a pixel, xsize * ysize bytes indexed as the depth
 * buffer is (row y at y * xsize), and it exists only for a context whose
 * GL_STENCIL_BITS is 8 (s31gl_set_stencil_bits: the GLX config asked for
 * stencil). It is the caller's (the GLX drawable's, shared like depth) or a
 * private one allocated with the depth buffer. The test and its ops are a
 * stage of the general path (zpipe.c zp_stencil_fn, chosen in raster_sel.c);
 * nothing here runs per fragment. Cold code (-Os).
 *
 * Dirty range (as phase 3a's dirty rows are for colour): ZStencilState, in
 * the buffer's tail, records the byte range [lo, hi) written since the last
 * full clear to val. The next full clear to the same value writes only that
 * range - a frame that uses the stencil in one part of the screen does not
 * pay a whole-buffer memset (76.8 kB at 320x240; PSRAM stores cost ~11.5
 * cycles each on the board). Every write widens it: the stencil stage per
 * chunk, a line's gathered pixels (the whole buffer), a scissored or
 * masked clear, glDrawPixels / glCopyPixels of stencil (their rectangle).
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "zgl.h"
#include "zpipe.h"

/* S31GL_STENCILRANGE=0: every full stencil clear writes the whole buffer
   (the A/B arm of the dirty range; a runtime toggle, as S31GL_DIRTYBOX) */
#ifndef S31GL_STENCILRANGE_DEFAULT
#define S31GL_STENCILRANGE_DEFAULT 1
#endif
static int zst_range_on(void)
{
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("S31GL_STENCILRANGE");
    on = e ? atoi(e) != 0 : S31GL_STENCILRANGE_DEFAULT;
  }
  return on;
}

/* the state for a (new) buffer. own: the core just allocated and zeroed it
   (contents known: all 0); otherwise the caller's memory is trusted only if
   the core already owns its tail */
void zst_attach(ZBuffer *zb, int own)
{
  int npix = zb->xsize * zb->ysize;
  ZStencilState *ss;
  if (zb->sbuf == NULL) { zb->sst = NULL; return; }
  ss = ZB_STENCIL_STATE(zb->sbuf, npix);
  zb->sst = ss;
  /* own, or the caller said the buffer is all 0 (GLX's calloc,
     s31gl_stencil_zeroed): the first full clear to 0 writes nothing, so a
     stencil that is only cleared (QuakeSpasm clears the one it asks for
     every frame and never tests it) never makes its pages resident */
  if (own || ss->magic == ZST_ZERO) {
    ss->magic = ZST_MAGIC;
    ss->valid = 1; ss->val = 0;
    ss->lo = INT_MAX; ss->hi = 0;
  } else if (ss->magic != ZST_MAGIC) {
    ss->magic = ZST_MAGIC;
    ss->valid = 0;
    ss->lo = 0; ss->hi = npix;
  }
}

/* s31gl_stencil_zeroed: the w * h values at sbuf are all 0 */
void zst_mark_zero(unsigned char *sbuf, int npix)
{
  if (sbuf != NULL && npix > 0) ZB_STENCIL_STATE(sbuf, npix)->magic = ZST_ZERO;
}

void zst_touch_all(ZBuffer *zb)
{
  if (zb->sst) {
    zb->sst->lo = 0;
    zb->sst->hi = zb->xsize * zb->ysize;
  }
}

/* rows y0..y1-1, columns x0..x1-1 (rows from the top) were written */
static void zst_touch_rect(ZBuffer *zb, int x0, int y0, int x1, int y1)
{
  ZStencilState *ss = zb->sst;
  int lo = y0 * zb->xsize + x0, hi = (y1 - 1) * zb->xsize + x1;
  if (ss == NULL || x1 <= x0 || y1 <= y0) return;
  if (lo < ss->lo) ss->lo = lo;
  if (hi > ss->hi) ss->hi = hi;
}

/* glClear(GL_STENCIL_BUFFER_BIT) in the (scissored) box, through
   GL_STENCIL_WRITEMASK (GL 1.3 4.2.3); full: the box is the whole buffer */
void zst_clear(GLContext *c, int x0, int y0, int x1, int y1, int full)
{
  ZBuffer *zb = c->zb;
  ZStencilState *ss = zb->sst;
  unsigned int wm = (unsigned int)c->stencil_writemask & 255;
  unsigned int v = (unsigned int)c->stencil_clear & 255;   /* GL: masked to s bits */
  int npix = zb->xsize * zb->ysize, x, y;

  if (zb->sbuf == NULL || ss == NULL || wm == 0) return;
  if (full && wm == 255) {
    if (ss->valid && ss->val == v && zst_range_on()) {
      /* only what was written since the last full clear to v */
      int lo = ss->lo < 0 ? 0 : ss->lo, hi = ss->hi > npix ? npix : ss->hi;
      if (hi > lo) memset(zb->sbuf + lo, (int)v, hi - lo);
    } else {
      memset(zb->sbuf, (int)v, npix);
    }
    ss->valid = 1;
    ss->val = (unsigned char)v;
    ss->lo = INT_MAX;
    ss->hi = 0;
    return;
  }
  for (y = y0; y < y1; y++) {
    unsigned char *p = zb->sbuf + y * zb->xsize;
    if (wm == 255) {
      memset(p + x0, (int)v, x1 - x0);
    } else {
      for (x = x0; x < x1; x++) p[x] = (unsigned char)((p[x] & ~wm) | (v & wm));
    }
  }
  zst_touch_rect(zb, x0, y0, x1, y1);
}

/* one stencil index written by glDrawPixels / glCopyPixels (GL 1.3 4.3.1:
   the write mask applies, and nothing else of the fragment operations but
   the pixel ownership and scissor tests, which the caller's box did) */
void zst_put(ZBuffer *zb, int x, int row, unsigned int v, unsigned int wm)
{
  unsigned char *p = zb->sbuf + row * zb->xsize + x;
  *p = (unsigned char)((*p & ~wm) | (v & wm));
}

void zst_touch(ZBuffer *zb, int x0, int y0, int x1, int y1)
{
  zst_touch_rect(zb, x0, y0, x1, y1);
}

/* ------------------------------------------------------------ pixel paths */

/* GL 1.3 3.6.4 / 4.3.2: the index shift and offset of glPixelTransfer */
static unsigned int zst_xfer(const GLContext *c, int v)
{
  int s = c->index_shift;
  if (s > 0) v = s >= 31 ? 0 : (int)((unsigned int)v << s);
  else if (s < 0) v = s <= -31 ? (v < 0 ? -1 : 0) : v >> -s;
  return (unsigned int)(v + c->index_offset);
}

static unsigned int rd16(const unsigned char *q, int swap)
{
  unsigned int v = q[0] | (q[1] << 8);
  return swap ? ((v >> 8) | (v << 8)) & 0xffff : v;
}

static unsigned int rd32(const unsigned char *q, int swap)
{
  unsigned int v = q[0] | (q[1] << 8) | (q[2] << 16) | ((unsigned int)q[3] << 24);
  return swap ? (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24) : v;
}

static int zst_elem(int type)
{
  switch (type) {
  case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
  case GL_UNSIGNED_SHORT: case GL_SHORT: return 2;
  case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
  default: return 0;
  }
}

/* glDrawPixels(GL_STENCIL_INDEX): the w * h indices of a client image,
   unpacked with GL_UNPACK_* and put through the index shift and offset
   (GL_MAP_STENCIL is recorded, not applied: the maps are the identity
   here); NULL and *err on failure. GL_BITMAP is not taken */
unsigned int *zst_unpack(GLContext *c, int w, int h, int type, const void *pixels, int *err)
{
  int elem = zst_elem(type), row_len, align, pitch, x, y;
  const unsigned char *base;
  unsigned int *out;

  *err = 0;
  if (elem == 0) {
    *err = type == GL_BITMAP ? 0 : GL_INVALID_ENUM;
    if (type == GL_BITMAP) gl_warn_once("glDrawPixels(GL_STENCIL_INDEX, GL_BITMAP)");
    return NULL;
  }
  out = gl_malloc(w * h * (int)sizeof(*out));
  if (out == NULL) { *err = GL_OUT_OF_MEMORY; return NULL; }
  if (c->map_stencil) gl_warn_once("glPixelTransfer(GL_MAP_STENCIL) (pixel maps)");
  row_len = c->unpack_row_length > 0 ? c->unpack_row_length : w;
  align = c->unpack_alignment;
  pitch = elem >= align ? elem * row_len : ((elem * row_len + align - 1) / align) * align;
  base = (const unsigned char *)pixels + c->unpack_skip_rows * pitch +
         c->unpack_skip_pixels * elem;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      const unsigned char *q = base + y * pitch + x * elem;
      int v;
      switch (type) {
      case GL_UNSIGNED_BYTE: v = q[0]; break;
      case GL_BYTE: v = (signed char)q[0]; break;
      case GL_UNSIGNED_SHORT: v = (int)rd16(q, c->unpack_swap); break;
      case GL_SHORT: v = (short)rd16(q, c->unpack_swap); break;
      case GL_FLOAT: {
        union { unsigned int i; float f; } t;
        t.i = rd32(q, c->unpack_swap);
        v = t.f >= 2.0e9f ? 2000000000 : (t.f <= -2.0e9f ? -2000000000 :
            (int)(t.f < 0.0f ? t.f - 0.5f : t.f + 0.5f));
        break;
      }
      default: v = (int)rd32(q, c->unpack_swap); break;
      }
      out[y * w + x] = zst_xfer(c, v);
    }
  return out;
}

/* glReadPixels(GL_STENCIL_INDEX), window rectangle (x, y) w x h: each index
   through the shift and offset, then masked to the type (GL 1.3 4.3.2), or
   converted for GL_FLOAT. Pixels outside the buffer are not written. Render
   scale: the buffer pixel under each window pixel */
void zst_read(GLContext *c, int x, int y, int w, int h, int type, void *pixels)
{
  ZBuffer *zb = c->zb;
  int elem = zst_elem(type), row_len, align, pitch, i, j, rs = c->rscale;
  int swap = c->pack_swap && elem > 1, vw = zb->xsize << rs, vh = zb->ysize << rs;
  unsigned char *base;

  if (elem == 0) {
    gl_set_error(c, type == GL_BITMAP ? 0 : GL_INVALID_ENUM);
    if (type == GL_BITMAP) gl_warn_once("glReadPixels(GL_STENCIL_INDEX, GL_BITMAP)");
    return;
  }
  row_len = c->pack_row_length > 0 ? c->pack_row_length : w;
  align = c->pack_alignment;
  pitch = elem >= align ? elem * row_len : ((elem * row_len + align - 1) / align) * align;
  base = (unsigned char *)pixels + c->pack_skip_rows * pitch + c->pack_skip_pixels * elem;
  for (j = 0; j < h; j++) {
    int wy = y + j, row = vh - 1 - wy;
    if (row < 0 || row >= vh) continue;
    row >>= rs;
    for (i = 0; i < w; i++) {
      int wx = x + i;
      unsigned char *q = base + j * pitch + i * elem;
      unsigned int v;
      if (wx < 0 || wx >= vw) continue;
      v = zst_xfer(c, zb->sbuf[row * zb->xsize + (wx >> rs)]);
      switch (type) {
      case GL_UNSIGNED_BYTE: q[0] = (unsigned char)v; break;
      case GL_BYTE: q[0] = (unsigned char)(v & 0x7f); break;
      case GL_UNSIGNED_SHORT: v &= 0xffff; goto w16;
      case GL_SHORT: v &= 0x7fff;
      w16:
        if (swap) v = ((v >> 8) | (v << 8)) & 0xffff;
        q[0] = (unsigned char)v; q[1] = (unsigned char)(v >> 8);
        break;
      default: {
        union { unsigned int i; float f; } t;
        if (type == GL_FLOAT) { t.f = (float)(int)v; v = t.i; }
        else if (type == GL_INT) v &= 0x7fffffff;
        if (swap) v = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
        q[0] = (unsigned char)v; q[1] = (unsigned char)(v >> 8);
        q[2] = (unsigned char)(v >> 16); q[3] = (unsigned char)(v >> 24);
        break;
      }
      }
    }
  }
}
