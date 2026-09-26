/*
 * s31_zepoch.c - depth epochs: glClear(GL_DEPTH_BUFFER_BIT) without
 * touching the depth buffer on most frames (phase 3a lever G03). s31, MIT.
 *
 * TinyGL stores window depth d as the 16-bit s = (1 - d) * 65535 and a
 * fragment passes GL_LESS when its s is GREATER than the stored one (Z
 * grows towards the viewer, clear.c). The rasterisers keep zmax, the
 * highest value stored since the depth buffer was last really cleared
 * (per triangle in ztri_setup, per line, point and pixel rectangle). A full
 * glClear of depth to 1.0 then starts a new EPOCH instead of writing: every
 * depth is stored as TinyGL's own value plus a base B above zmax,
 *     s = B + s_plain,   B = zmax + 2
 * so every value left in the buffer (a "stale" pixel) is below every value
 * the new epoch can store - farther than anything drawn now, which is what
 * a pixel cleared to 1.0 is - and two fragments of the epoch compare exactly
 * as they did: the base is an integer added to the depth plane of each
 * triangle (ztri_setup), after the plane is computed, so depth tests,
 * stored values minus B and depth read back are bit for bit the plain
 * 16-bit ones. Nothing is lost: no precision, no depth bit.
 *
 * What it needs is room: s_plain + B must fit in 16 bits. Perspective
 * depth crowds towards the far plane, which is stored low (glxgears and
 * geartrain store s_plain < 4,800 of 65,535), so a dozen epochs fit before
 * a real clear. A clear starts an epoch only when the last epoch's range,
 * with an eighth of margin, fits above the new base. If a primitive then
 * does not fit (the scene came nearer), the buffer is DEMOTED first: every
 * value of the epoch becomes its plain value (s - B) and every stale one 0,
 * the plain 1.0, which is exactly the buffer a plain clear would have given
 * by then; the epoch ends there. A demotion reads and writes the whole
 * buffer, more than the clear it replaced, so after one (or a
 * materialisation, below) the next ZEP_BACKOFF clears are real - 8, then
 * 16, 32 and 64 when they keep coming without a clean epoch between.
 *
 * Why not the classic ping-pong (alternate the two halves, flip the
 * compare, never clear)? It is not invisible: a pixel written two frames
 * ago and not drawn over last frame is back inside the current half and
 * still occludes. Quake's gl_ztrick gets away with it only because its
 * world covers the whole screen every frame; glxgears does not. And it
 * gives up a depth bit for good; this gives up nothing and pays for it in
 * room, which scenes near their far plane have in plenty.
 *
 * Where a stale pixel does NOT behave as the clear value exactly: only
 * against a fragment whose own plain value is 0 (d within one step of
 * 1.0), and only for the depth functions that tell equal from farther:
 * GL_LESS (the fragment must fail, as against 1.0), GL_GEQUAL, GL_EQUAL and
 * GL_NOTEQUAL. LEQUAL, GREATER, ALWAYS and NEVER are exact as they are.
 * So, while stale pixels may exist:
 *  - GL_LESS: a triangle, line, point or pixel rectangle that can produce
 *    such a fragment (its lowest plain zp.z below zguard) first
 *    "materialises" the epoch - every stale value becomes B, the epoch's own
 *    1.0 - and from then on the buffer holds no stale pixel. The check is
 *    per primitive (the fillers' ztri_zepoch, clip.c lines/points,
 *    s31_draw.c), and never per pixel.
 *  - GEQUAL / EQUAL / NOTEQUAL with the depth test on: materialised at once
 *    (gl_update_raster). A clear with one of them current is a real one.
 * Everything that reads depth back decodes it (glReadPixels, glCopyPixels
 * of GL_DEPTH): a stale value reads as 1.0, any other as its plain value.
 * glClearDepth other than 1.0: a real clear. A scissored or masked clear
 * writes the epoch's value of its clear depth inside the box, like a
 * primitive: if that value does not fit above B (glClearDepth < 1.0 late
 * in the epochs) the buffer is demoted first (zep_clear_rect_value).
 *
 * A sliver whose depth gradient saturated (ztri.h) stores depths its
 * vertices do not bound, in both builds; plus B they could wrap below B
 * and read back as 1.0 where the plain build keeps a ramp (review 3a R4).
 * So its true bounds are taken from its one or two rows (zep_tri_sliver)
 * and used like any triangle's vertex depths; when a row wraps and no
 * bound exists, the epoch is demoted first. (Demoting at every sliver was
 * measured: teapotf +2.0% / +5.4% at 320 / 640, the sphere cases +1.5-2.3%
 * - their pole triangles are slivers.) With that, no output differs from
 * the plain buffer.
 *
 * The caller's depth memory (s31gl_bind_depth) is trusted only once the
 * core has taken its tail over (ZDepthState.magic, zep_attach); before
 * that, the tail says "anything may be stored" and the first full clear
 * is a real one, whatever the caller left in the values or the tail.
 *
 * The state is ZDepthState at the tail of the depth memory (zbuffer.h), so
 * contexts that share a drawable's depth buffer agree; each context keeps
 * its copy (zoff, ztop, zguard, its pipe's zoff and zchk) in step with it
 * at gl_prepare_slow (make-current, bind, frame end) and after anything it
 * changes itself, and raises the shared zmax directly.
 *
 * S31GL_ZTRICK=0 turns it off (every clear real), the runtime A/B toggle.
 *
 * DIRTY BOXES (phase 3a, the clears' second lever). When the caller keeps
 * its colour buffer to itself (s31gl_set_retained: GLX's SHM or malloc'd
 * buffer, which only the rasteriser writes; the X server only reads a
 * ShmPutImage segment), a pixel nothing has drawn into since the last full
 * clear to the same value still holds that value. So every rasteriser
 * records the box it can have written (ZPipe.db: per triangle its rows,
 * or with S31GL_DIRTYBOX=2 its rows and x extent, zdb_tri; lines and
 * points with their width; a pixel rectangle or anything else counts as
 * the whole buffer), and a full
 * glClear writes only the union of the boxes drawn since the buffer's last
 * full clear, when that clear had the same value and nothing was bound or
 * made current since (tgl_bind_serial). The colour buffer and the depth
 * buffer each keep their own accumulated box (cbox, dzbox), because a
 * depth epoch leaves the depth buffer uncleared across frames. The result
 * is the buffer a full clear gives, pixel for pixel; S31GL_DIRTYBOX=0
 * turns it off, =2 adds the x extent (the board A/B arm: DB-x in
 * artifacts/gl/phase3a/LEVERS.md - fewer bytes, more instructions).
 */
#include <stdlib.h>
#ifdef ZEP_TRACE
#include <stdio.h>
#endif
#include "zgl.h"
#include "ztri.h"

#ifndef S31GL_ZTRICK_DEFAULT
#define S31GL_ZTRICK_DEFAULT 1
#endif
#define ZEP_BACKOFF 8
#define ZF ZB_POINT_Z_FRAC_BITS
/* the highest stored depth << 14 (65535.99...) */
#define ZEP_TOP 0x3fffffffu
/* a plane may pass its highest vertex by its rounding, under 1/10 of a
   step across the widest buffer; a quarter step of margin */
#define ZEP_MARGIN (1u << (ZF - 2))

#ifndef S31GL_DIRTYBOX_DEFAULT
#define S31GL_DIRTYBOX_DEFAULT 1
#endif
/* ZEP_MAGIC: the tail's state is the core's (ZDepthState.magic, top 24
   bits; the low byte is the backoff level) */
#define ZEP_MAGIC 0x7a3e5000u
#define ZEP_LEVEL(st) ((st)->magic & 0xffu)
/* the backoff doubles with each demotion or materialisation that follows
   another without a clean epoch between (review 3a M4: a scene that
   materialises every epoch - geo12, a far-plane background under GL_LESS -
   paid a whole-buffer read-modify-write every ninth frame, +1.6% on
   average and +10% on those frames against epochs off): 8, 16, 32, 64 */
#define ZEP_LEVEL_MAX 3
static void zep_back_off(ZDepthState *st)
{
  unsigned int l = ZEP_LEVEL(st);
  st->backoff = (unsigned char)(ZEP_BACKOFF << l);
  if (l < ZEP_LEVEL_MAX) st->magic = ZEP_MAGIC | (l + 1);
}

static int zep_on = -1, zdb_on = -1;

/* 0 off, 1 rows (the default), 2 rows and columns (the x extent per
   triangle: fewer bytes cleared, ~40 instructions a triangle - the board
   A/B arm of review 3a M2) */
static int zdb_enabled(void)
{
  if (zdb_on < 0) {
    const char *e = getenv("S31GL_DIRTYBOX");
    zdb_on = e ? atoi(e) : S31GL_DIRTYBOX_DEFAULT;
    if (zdb_on < 0 || zdb_on > 2) zdb_on = 1;
  }
  return zdb_on;
}

/* ------------------------------------------------------------ dirty boxes */

#define BOX_EMPTY(b) ((b)[0] = (b)[1] = 0x3fffffff, (b)[2] = (b)[3] = -0x3fffffff)

static void box_union(int *a, const int *b)
{
  if (b[0] < a[0]) a[0] = b[0];
  if (b[1] < a[1]) a[1] = b[1];
  if (b[2] > a[2]) a[2] = b[2];
  if (b[3] > a[3]) a[3] = b[3];
}

static void box_full(const ZBuffer *zb, int *b)
{
  b[0] = 0; b[1] = 0; b[2] = zb->xsize; b[3] = zb->ysize;
}

/* the fillers' inline check (ztri_rows): the recorded rows, or with x
   boxes an empty range, so that every triangle reaches zdb_tri */
static void zdb_set_chk(GLContext *c)
{
  if (zdb_on == 2) {
    c->pipe.dbc[0] = 0x3fffffff;
    c->pipe.dbc[1] = -0x3fffffff;
  } else {
    c->pipe.dbc[0] = c->pipe.db[1];
    c->pipe.dbc[1] = c->pipe.db[3];
  }
}

/* the rasterisers' slow path: the box being drawn grows to take in
   [x0, x1) x [y0, y1); a full box stops the recording until the next
   full clear */
void zdb_grow(GLContext *c, int x0, int y0, int x1, int y1)
{
  ZBuffer *zb = c->zb;
  int *d = c->pipe.db;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > zb->xsize) x1 = zb->xsize;
  if (y1 > zb->ysize) y1 = zb->ysize;
  if (x0 < d[0]) d[0] = x0;
  if (y0 < d[1]) d[1] = y0;
  if (x1 > d[2]) d[2] = x1;
  if (y1 > d[3]) d[3] = y1;
  if (d[0] <= 0 && d[1] <= 0 && d[2] >= zb->xsize && d[3] >= zb->ysize)
    c->pipe.bact = 0;
  zdb_set_chk(c);
}

/* a triangle, from the fillers (ztri.h ztri_rows) when its rows
   [part 0's ya, part 1's yb) are not all recorded yet. Rows only (the
   default): it takes whole rows. S31GL_DIRTYBOX=2: every triangle comes
   here and adds its x extent too - [floor(min x), floor(max x) + 1) of its
   window vertices, which holds every pixel whose centre it covers (an
   edge's 16.16 error is under 1/100 px, the bound's slack at least 1/2) */
void zdb_tri(ZPipe *p, int ya, int yb, float x0, float dx1, float dx2)
{
  GLContext *c = p->zctx;
  if (zdb_on == 2) {
    float x1 = x0 + dx1, x2 = x0 + dx2;
    float lo = fminf(x0, fminf(x1, x2)), hi = fmaxf(x0, fmaxf(x1, x2));
    zdb_grow(c, ztri_floor(lo), ya, ztri_floor(hi) + 1, yb);
  } else {
    zdb_grow(c, 0, ya, c->zb->xsize, yb);
  }
}

/* nothing drawn yet since this clear. A frame whose box ended (nearly)
   full buys nothing and costs its recording: the next ZDB_SKIP frames do
   not record, with the box set full (so their clears are whole) */
#define ZDB_SKIP 16
void zdb_reset(GLContext *c)
{
  ZBuffer *zb = c->zb;
  int *d = c->pipe.db;
  int on = zb->retained && zdb_enabled();
  if (on) {
    long area = (long)(d[2] - d[0]) * (long)(d[3] - d[1]);
    if (zb->zdb_skip) {
      zb->zdb_skip--;
      on = 0;
    } else if (d[2] > d[0] && d[3] > d[1] &&
               area * 10 >= (long)zb->xsize * zb->ysize * 9) {
      zb->zdb_skip = ZDB_SKIP;
      on = 0;
    }
  }
  if (on) {
    BOX_EMPTY(d);
  } else {
    box_full(zb, d);
  }
  c->pipe.bact = on;
  zdb_set_chk(c);
}

/* the box to clear: acc and what has been drawn, or all of the buffer when
   the last full clear is not known to have left the rest at v */
static int zdb_region(GLContext *c, int *acc, int valid, int same, int *r)
{
  ZBuffer *zb = c->zb;
  if (!(valid && same && zdb_enabled())) {
    box_full(zb, r);
    return 1;
  }
  r[0] = acc[0]; r[1] = acc[1]; r[2] = acc[2]; r[3] = acc[3];
  box_union(r, c->pipe.db);
  if (r[0] < 0) r[0] = 0;
  if (r[1] < 0) r[1] = 0;
  if (r[2] > zb->xsize) r[2] = zb->xsize;
  if (r[3] > zb->ysize) r[3] = zb->ysize;
  return r[0] <= 0 && r[1] <= 0 && r[2] >= zb->xsize && r[3] >= zb->ysize;
}

/* the slot of the bound colour buffer (a buffer never seen takes the
   other slot, invalid) */
static struct ZColourSlot *zdb_slot(ZBuffer *zb)
{
  int i;
  if (zb->cur >= 0 && zb->cs[zb->cur].buf == zb->pbuf) return &zb->cs[zb->cur];
  for (i = 0; i < 2; i++)
    if (zb->cs[i].buf == zb->pbuf && zb->pbuf != NULL) { zb->cur = i; return &zb->cs[i]; }
  i = zb->cur == 0 ? 1 : 0;
  zb->cur = i;
  zb->cs[i].buf = zb->pbuf;
  zb->cs[i].valid = 0;
  BOX_EMPTY(zb->cs[i].box);
  return &zb->cs[i];
}

/* glClear(GL_COLOR_BUFFER_BIT) of the whole buffer to v, all channels */
void zdb_clear_colour(GLContext *c, unsigned int v)
{
  ZBuffer *zb = c->zb;
  struct ZColourSlot *cs = zdb_slot(zb);
  int r[4];
  int full = zdb_region(c, cs->box, cs->valid,
                        cs->ser == tgl_bind_serial && cs->val == v, r);
  (void)full;
  if (r[2] <= r[0] || r[3] <= r[1])
    ;                                          /* nothing was drawn */
  else if (r[0] == 0 && r[2] == zb->xsize && zb->linesize == zb->xsize * PSZB)
    /* whole rows of a contiguous buffer: one pass */
    ZB_fill16((unsigned short *)((char *)zb->pbuf + r[1] * zb->linesize), v,
              (r[3] - r[1]) * zb->xsize);
  else
    ZB_clear_rect(zb, r[0], r[1], r[2], r[3], 0, 0, 1, (int)v, 0xffff);
  BOX_EMPTY(cs->box);
  cs->valid = zb->retained;
  cs->ser = tgl_bind_serial;
  cs->val = v;
}

/* the bound colour buffer becomes another of the same size (GLX's
   ping-pong after a present): what was drawn since the last fold went into
   the old one (and the depth buffer, which both share) */
void zdb_rebind(GLContext *c)
{
  ZBuffer *zb = c->zb;
  if (zb->pbuf != NULL) box_union(zdb_slot(zb)->box, c->pipe.db);
  box_union(zb->dzbox, c->pipe.db);
  zdb_reset(c);
}

/* every recorded box is forgotten: the next clears are whole */
void zdb_invalidate(ZBuffer *zb)
{
  zb->cs[0].valid = zb->cs[1].valid = 0;
  zb->cs[0].buf = zb->cs[1].buf = NULL;
  zb->cur = -1;
  zb->dzvalid = 0;
}

/* the real depth clear to stored value v (zep_clear) */
static void zdb_clear_depth(GLContext *c, unsigned int v)
{
  ZBuffer *zb = c->zb;
  int r[4];
  int full = zdb_region(c, zb->dzbox, zb->dzvalid,
                        zb->dzbuf == zb->zbuf && zb->dzser == tgl_bind_serial &&
                        zb->dzval == v, r);
  (void)full;
  if (r[2] <= r[0] || r[3] <= r[1])
    ;                                          /* nothing was drawn */
  else if (r[0] == 0 && r[2] == zb->xsize)
    ZB_fill16(zb->zbuf + r[1] * zb->xsize, v, (r[3] - r[1]) * zb->xsize);
  else
    ZB_clear_rect(zb, r[0], r[1], r[2], r[3], 1, (int)v, 0, 0, 0xffff);
  BOX_EMPTY(zb->dzbox);
  zb->dzvalid = zb->retained;
  zb->dzbuf = zb->zbuf;
  zb->dzser = tgl_bind_serial;
  zb->dzval = v;
}

/* the end of a glClear: what was drawn goes into the box of each buffer
   that was not cleared whole, and the recording starts again */
void zdb_fold(GLContext *c, int colour_reset, int depth_reset)
{
  ZBuffer *zb = c->zb;
  if (!colour_reset) box_union(zdb_slot(zb)->box, c->pipe.db);
  if (!depth_reset) box_union(zb->dzbox, c->pipe.db);
  zdb_reset(c);
}

/* ------------------------------------------------------------ depth epochs */

static int __attribute__((noinline)) zep_read_env(void)
{
  const char *e = getenv("S31GL_ZTRICK");
  zep_on = e ? atoi(e) != 0 : S31GL_ZTRICK_DEFAULT;
  return zep_on;
}
/* read once; after that a load (it is asked per new zmax) */
static inline int zep_enabled(void)
{
  return zep_on >= 0 ? zep_on : zep_read_env();
}

/* can the next full clear start an epoch above zmax (see the header)? B is
   two steps above the highest stored value (its rounding, and one of
   margin); the last epoch's range S, plus an eighth, must fit above it */
static int zep_room(unsigned int zmax, unsigned int base, unsigned int *bnew)
{
  unsigned int top = zmax >> ZF, B = top + 2, S = top >= base ? top - base : 0;
  *bnew = B;
  return B + S + (S >> 3) + 16 <= 65535u;
}

/* the pipe's zchk for this context: a triangle above it needs
   zep_tri_check - a new zmax while the next clear could still use one, and
   anything that would not fit above a nonzero base */
static void zep_set_chk(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned int chk = ~0u, b;

  /* (st->backoff: the next clear is a real one whatever is drawn, and it
     resets zmax - nothing to record until then; review 3a M4) */
  if (st != NULL && zb->zmaxp == &st->zmax && !st->backoff && zep_enabled() &&
      zep_room(st->zmax, st->base, &b))
    chk = st->zmax > zb->zoff ? st->zmax - zb->zoff : 0;
  if (zb->zoff && zb->ztop < chk) chk = zb->ztop;
  c->pipe.zchk = chk;
  c->pipe.zoff = zb->zoff;
  c->pipe.zact = chk != ~0u || zb->zoff != 0;
}

/* zguard for the current depth state, and the immediate materialisation
   GEQUAL / EQUAL / NOTEQUAL need (gl_update_raster, zep_sync) */
void zep_guard(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;

  zb->zguard = 0;
  c->pipe.zguard = 0;
  if (st == NULL || !st->stale || !c->depth_test) return;
  switch (c->depth_func) {
  case GL_LESS:
    /* a fragment's plain value is 0 exactly when its zp.z < 1 << 14; a
       half step of margin covers the plane's rounding */
    zb->zguard = (1 << ZF) + (1 << (ZF - 1));
    c->pipe.zguard = (unsigned int)zb->zguard;
    break;
  case GL_GEQUAL: case GL_EQUAL: case GL_NOTEQUAL:
    zep_materialise(c);
    break;
  default:
    break;
  }
}

/* where the rasterisers record the depth they store: the shared zmax
   while depth can be written, a sink otherwise (a 2D pass with the depth
   test off costs the next epoch no room); and the pipe's copy */
void zep_track_target(GLContext *c)
{
  ZBuffer *zb = c->zb;
  zb->zmaxp = zb->zst && c->depth_test && c->depth_mask ? &zb->zst->zmax
                                                       : &zb->zmax_none;
  zep_set_chk(c);
}

/* bring this context's copy in step with the depth buffer's state */
void zep_sync(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned int B = st ? st->base : 0;

  zb->zoff = B << ZF;
  /* the highest plain zp.z whose stored value (plus the plane's rounding)
     stays within 16 bits above B */
  zb->ztop = B ? ((65535u - B) << ZF) + (1u << ZF) - 1u - ZEP_MARGIN : ~0u;
  zb->zser = st ? st->serial : 0;
  zep_guard(c);
  zep_track_target(c);
}

/* gl_prepare_slow: the depth buffer (and so its state) may be another one */
void zep_attach(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zbuf ? ZB_DEPTH_STATE(zb->zbuf, zb->xsize * zb->ysize) : NULL;

  c->pipe.zctx = c;
  if (st != NULL && (st->magic & ~0xffu) != ZEP_MAGIC) {
    /* a tail the core has not taken over (review 3a R2): its fields, and
       the values, are whatever the caller left there. Say that anything
       may be stored (zmax at the top): the first full clear is then a
       real one, and nothing else trusts the old contents */
    st->serial++;
    st->zmax = ZEP_TOP;
    st->base = 0;
    st->stale = 0;
    st->backoff = 0;
    st->magic = ZEP_MAGIC;
    zb->zst = NULL;                   /* force the sync below */
  }
  if (st != zb->zst) {
    zb->zst = st;
    zep_sync(c);
  } else if (st != NULL && zb->zser != st->serial) {
    zep_sync(c);
  }
}

/* every stale value becomes B, the epoch's 1.0: the buffer then holds no
   pixel from an earlier epoch, and every depth function is exact */
void zep_materialise(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned int *p, *e, b, b2;
  unsigned short *q;
  int n;

  if (st == NULL || !st->stale) return;
  b = st->base;
  b2 = b << 16;
  q = zb->zbuf;
  n = zb->xsize * zb->ysize;
  if (((unsigned long)q & 2) && n > 0) {
    if (*q < b) *q = (unsigned short)b;
    q++; n--;
  }
  /* two values a word: each half raised to b if below it */
  p = (unsigned int *)q;
  e = p + (n >> 1);
  for (; p < e; p++) {
    unsigned int w = *p, lo = w & 0xffffu, hi = w & 0xffff0000u;
    if (lo < b) lo = b;
    if (hi < b2) hi = b2;
    *p = hi | lo;
  }
  if (n & 1) {
    q = (unsigned short *)p;
    if (*q < b) *q = (unsigned short)b;
  }
  box_full(zb, zb->dzbox);                       /* every pixel may have moved */
  st->stale = 0;
  zep_back_off(st);
  if (st->zmax < (b << ZF) + (1u << (ZF - 1)))
    st->zmax = (b << ZF) + (1u << (ZF - 1));     /* B is stored now */
  st->serial++;
  zep_sync(c);
}

/* back to the plain mapping, exactly: an epoch value s becomes s - B and a
   stale one 0 (the plain 1.0) - the buffer a plain clear would have given */
void zep_demote(GLContext *c)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned short *q;
  unsigned int b;
  int i, n;

  if (st == NULL || st->base == 0) return;
  b = st->base;
  q = zb->zbuf;
  n = zb->xsize * zb->ysize;
  /* two values a word, as zep_materialise (review 3a M4: the halfword
     loop was ~6-7 instructions a pixel; this is ~5 a pixel, half the
     loads and stores) */
  if (((unsigned long)q & 2) && n > 0) {
    unsigned int s = *q;
    *q++ = (unsigned short)(s >= b ? s - b : 0);
    n--;
  }
  {
    unsigned int *w = (unsigned int *)q, *e = w + (n >> 1);
    for (; w < e; w++) {
      unsigned int x = *w, lo = x & 0xffffu, hi = x >> 16;
      lo = (lo > b ? lo : b) - b;           /* maxu: s >= b ? s - b : 0 */
      hi = (hi > b ? hi : b) - b;
      *w = (hi << 16) | lo;
    }
    q = (unsigned short *)w;
  }
  for (i = 0; i < (n & 1); i++) {
    unsigned int s = q[i];
    q[i] = (unsigned short)(s >= b ? s - b : 0);
  }
  st->zmax = st->zmax > (b << ZF) ? st->zmax - (b << ZF) : 0;
  /* a pixel nothing drew into since the real clear held dzval: it holds
     its demoted value now */
  zb->dzval = zb->dzval >= b ? zb->dzval - b : 0;
  st->base = 0;
  st->stale = 0;
  zep_back_off(st);
  st->serial++;
  zep_sync(c);
}

/* the fillers (ztri.h ztri_zepoch): a triangle at the epoch's farthest
   step under GL_LESS with stale pixels about */
void zep_tri_far(ZPipe *p)
{
  zep_materialise(p->zctx);
}

/* ztri_setup's slow path (zpipe.h): m is the triangle's highest plain
   depth, above the pipe's zchk */
void zep_tri_check(ZPipe *p, unsigned int m)
{
  GLContext *c = p->zctx;
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned int s;

  if (st == NULL) return;
  if (m == ~0u) {
    /* unbounded depths (zep_tri_sliver could not bound them): no epoch
       can be started above them, and none may run under them */
    if (zb->zoff) zep_demote(c);
    s = ZEP_TOP;
  } else {
    if (zb->zoff && m > zb->ztop) zep_demote(c);   /* it does not fit */
    s = m + zb->zoff;
  }
  if (zb->zmaxp == &st->zmax && s > st->zmax) st->zmax = s;
  zep_set_chk(c);
}

/* A sliver (ztri.h ztri_zepoch): a triangle whose depth gradient
   saturated, so its plane's values - the plain build's too - are not
   bounded by its vertex depths, and plus B they could wrap below B (read
   back as 1.0 where the plain build keeps a ramp: review 3a R4). A sliver
   is thin in x or in y: a row or two, or a needle one pixel or less wide
   on each of its rows (teapotf and the UV spheres make both). The plane
   is evaluated at both ends of each row's span exactly as the filler
   does (wrapping unsigned), and when no row holds a 2^32 wrap the lowest
   and highest value are the triangle's true bounds: the far-plane check
   and the fit check then use those, like any triangle's vertex depths.
   Otherwise the bounds are unknown: the epoch is demoted and zmax goes to
   the top. Exact either way; ~20 instructions a row, off the fillers'
   path. (Demoting at every sliver, or bounding only slivers of up to two
   rows, cost teapotf 640 +5.4%: a demotion reads and writes the whole
   buffer.) */
#ifdef ZEP_TRACE
static unsigned int zep_nsl, zep_nslb;
#endif
void zep_tri_sliver(ZPipe *p, const struct ZTri *T)
{
  unsigned int lo = ~0u, hi = 0;
  int k, y, ok = 1;

  for (k = 0; k < 2 && ok; k++) {
    int ya = T->part[k].ya, yb = T->part[k].yb;
    for (y = ya; y < yb; y++) {
      int xl = (int)((unsigned int)T->part[k].xl + (unsigned int)(y - ya) * (unsigned int)T->part[k].dxl);
      int xr = (int)((unsigned int)T->part[k].xr + (unsigned int)(y - ya) * (unsigned int)T->part[k].dxr);
      int x0, x1;
      unsigned int z0;
      long long z1;
      ZTRI_SPAN(xl, xr, x0, x1);
      if (x1 <= x0) continue;
      z0 = T->zc + (unsigned int)T->dzdy * (unsigned int)y + (unsigned int)T->dzdx * (unsigned int)x0;
      z1 = (long long)z0 + (long long)T->dzdx * (x1 - 1 - x0);
      if (z1 < 0 || z1 > 0xffffffffll) { ok = 0; break; }
      if (z0 < lo) lo = z0;
      if (z0 > hi) hi = z0;
      if ((unsigned int)z1 < lo) lo = (unsigned int)z1;
      if ((unsigned int)z1 > hi) hi = (unsigned int)z1;
    }
  }
#ifdef ZEP_TRACE
  zep_nsl++; zep_nslb += ok;
#endif
  if (!ok) {
    zep_tri_check(p, ~0u);
    return;
  }
  if (hi < lo) return;                         /* no pixel */
  if (lo < p->zguard) zep_tri_far(p);
  if (hi > p->zchk) zep_tri_check(p, hi);
}

/* lines, points and pixel rectangles (their plain depths up to m): make
   them fit, record them, and return the base to add */
unsigned int zep_prim(GLContext *c, unsigned int m)
{
  ZBuffer *zb = c->zb;
  if (zb->zoff && m > zb->ztop) zep_demote(c);
  if (m + zb->zoff > *zb->zmaxp) {
    *zb->zmaxp = m + zb->zoff;
    zep_set_chk(c);
  }
  return zb->zoff;
}

/* the stored value of clear depth cd in the current epoch (the full
   clear's, after zep_clear has chosen the base) */
unsigned int zep_clear_value(GLContext *c, float cd)
{
  return ((unsigned int)(int)((1.0f - cd) * 65535.0f) & 0xffffu) + (c->zb->zoff >> ZF);
}

/* a scissored or masked glClear(GL_DEPTH_BUFFER_BIT) to cd: the value to
   write inside the box. Like a primitive (zep_prim) it must fit above the
   base - a plain value above ztop would wrap below B and read back as 1.0
   (review 3a R1: glClearDepth < 1.0 in a box during an epoch) - so it
   demotes first when it does not; and it is written whatever the depth
   test, so it goes straight into the shared zmax */
unsigned int zep_clear_rect_value(GLContext *c, float cd)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned int p = (unsigned int)(int)((1.0f - cd) * 65535.0f) & 0xffffu, v, z;

  if (zb->zoff && (p << ZF) > zb->ztop) zep_demote(c);
  v = p + (zb->zoff >> ZF);
  if (st != NULL) {
    z = (v << ZF) + (1u << (ZF - 1));
    if (z > st->zmax) st->zmax = z;
    zep_set_chk(c);
  }
  return v;
}

/* the window depth a stored value stands for (glReadPixels, glCopyPixels):
   the plain decode of s - B; a stale value is 1.0 */
float zep_depth(const ZBuffer *zb, unsigned int s)
{
  unsigned int b = zb->zoff >> ZF;
  if (s < b) return 1.0f;
  return 1.0f - (float)(s - b) * (1.0f / 65535.0f);
}

/* the depth functions under which a stale pixel is the clear value for
   every fragment (see the header) */
static int zep_func_ok(GLContext *c)
{
  if (!c->depth_test) return 1;
  switch (c->depth_func) {
  case GL_GEQUAL: case GL_EQUAL: case GL_NOTEQUAL: return 0;
  default: return 1;
  }
}

/* glClear(GL_DEPTH_BUFFER_BIT) of the whole buffer to cd, depth mask on */
int zep_clear(GLContext *c, float cd)
{
  ZBuffer *zb = c->zb;
  ZDepthState *st = zb->zst;
  unsigned int v, B;

#ifdef ZEP_TRACE
  if (st) fprintf(stderr, "zep: slivers %u bounded %u\n", zep_nsl, zep_nslb);
  if (st) fprintf(stderr, "zep: zmax %u (step %u) base %u backoff %u\n", st->zmax,
                  st->zmax >> ZF, st->base, st->backoff);
#endif
  /* an epoch that ends here with its stale pixels intact was a clean one:
     the backoff starts again from 8 */
  if (st != NULL && st->base != 0 && st->stale && ZEP_LEVEL(st))
    st->magic = ZEP_MAGIC;
  if (st != NULL && zep_enabled() && cd == 1.0f && !st->backoff && zep_func_ok(c) &&
      zep_room(st->zmax, st->base, &B)) {
    /* the next epoch: nothing is written */
    st->base = (unsigned short)B;
    st->stale = 1;
    st->serial++;
    zep_sync(c);
    return 0;
  }
  if (st != NULL) {
    if (st->backoff) st->backoff--;
    st->base = 0;
    st->stale = 0;
    st->serial++;
    zep_sync(c);
  }
  v = zep_clear_value(c, cd);
  zdb_clear_depth(c, v);
  if (st != NULL) st->zmax = (v << ZF) + (1u << (ZF - 1));
  zep_set_chk(c);
  return 1;
}
