/*
 * s31_ctx.c - TinyGL contexts that render into a caller's buffer. s31, MIT.
 *
 * TinyGL had one global context created by glInit() around a ZBuffer it
 * usually allocated itself. Here a context is a GLContext whose ZBuffer
 * describes memory the caller owns (an RGB565 colour buffer of any pitch):
 * the rasteriser writes straight into it, there is no ZB_copyFrameBuffer.
 * The 16-bit depth buffer is allocated at the first clear or draw, never
 * at bind or make-current, and dropped when the size changes.
 *
 * gl_ctx (TinyGL's "current context") is never NULL once any context has
 * existed: with nothing current it points at a context with no buffer,
 * whose clears and draws are discarded (gl_prepare fails), so a GL call
 * made after glXMakeCurrent(None) cannot touch freed memory.
 */
#include "zgl.h"

static GLContext *null_ctx;     /* current when nothing is */

static ZBuffer *zb_new(void)
{
  ZBuffer *zb = gl_zalloc(sizeof(ZBuffer));
  if (zb == NULL) return NULL;
  zb->mode = ZB_MODE_5R6G5B;
  return zb;
}

/* drop the depth buffer: freed when it is ours, forgotten when the
   caller's (tgl_ctx_bind_depth) */
static void zb_drop_depth(ZBuffer *zb)
{
  if (!zb->zbuf_ext) gl_free(zb->zbuf);
  zb->zbuf = NULL;
  zb->zbuf_ext = 0;
}

static void zb_free(ZBuffer *zb)
{
  if (zb == NULL) return;
  zb_drop_depth(zb);
  gl_free(zb);
}

/* GLContext with its own (empty) ZBuffer; restores gl_ctx */
static GLContext *ctx_new(GLContext *share)
{
  GLContext *prev = gl_ctx, *c;
  ZBuffer *zb = zb_new();

  if (zb == NULL) return NULL;
  glInit(zb);                   /* allocates the context and sets gl_ctx */
  c = gl_ctx;
  gl_ctx = prev;
  if (c == NULL) { zb_free(zb); return NULL; }

  if (share != NULL) {
    /* glXCreateContext share_list: display lists and texture objects */
    GLSharedState own = c->shared_state;
    GLTexture *t0 = c->current_texture;
    c->shared_state = share->shared_state;
    (*c->shared_state.refs)++;
    /* drop this context's own (empty) tables */
    gl_free(own.lists);
    if (t0) gl_free(t0);
    gl_free(own.texture_hash_table);
    gl_free(own.refs);
    c->current_texture = NULL;
    {
      GLTexture *t = c->shared_state.texture_hash_table[0];
      while (t != NULL && t->handle != 0) t = t->next;
      c->current_texture = t;
    }
  }
  return c;
}

void *tgl_ctx_create(void *share)
{
  GLContext *c;

  if (null_ctx == NULL) {
    null_ctx = ctx_new(NULL);
    if (null_ctx == NULL) return NULL;
    if (gl_ctx == NULL) gl_ctx = null_ctx;
  }
  c = ctx_new((GLContext *)share);
  return c;
}

void tgl_ctx_destroy(void *ctx)
{
  GLContext *c = ctx, *prev = gl_ctx;
  ZBuffer *zb;

  if (c == NULL || c == null_ctx) return;
  zb = c->zb;
  {
    /* attribute stacks the app left pushed (tgl_bridge.h: next first) */
    void **stacks[2] = { &c->attrib.attrib_top, &c->attrib.client_top };
    int k;
    for (k = 0; k < 2; k++)
      while (*stacks[k] != NULL) {
        void *n = *(void **)*stacks[k];
        free(*stacks[k]);
        *stacks[k] = n;
      }
  }
  gl_ctx = c;                   /* glClose works on gl_ctx */
  glClose();
  zb_free(zb);
  gl_ctx = (prev == c) ? null_ctx : prev;
}

/*
 * Point the context at a colour buffer: RGB565, width x height, pitch bytes
 * per row (>= 2 * width, even). pixels may be NULL with a size (the
 * drawable is known, its memory is bound later from the frame hook) or
 * with size 0 (unbind; the depth buffer is kept). A size change drops the
 * depth buffer (reallocated lazily) and re-evaluates the viewport guard.
 * The viewport and scissor box are set by the first bind with a size -
 * GLX's "first MakeCurrent" - unless the application set the viewport
 * before that.
 */
int tgl_ctx_bind(void *ctx, void *pixels, int width, int height, int pitch)
{
  GLContext *c = ctx;
  ZBuffer *zb;

  if (c == NULL || width < 0 || height < 0 ||
      (pixels != NULL && (pitch < 2 * width || (pitch & 1))))
    return -1;
  zb = c->zb;
  c->ready = 0;
  c->armed = 1;
  if (pixels == NULL && (width == 0 || height == 0)) {
    /* unbind: keep the size and the depth buffer for the rebind */
    zb->pbuf = NULL;
    return 0;
  }
  /* pixels == NULL with a size: the drawable is known, its memory comes
     later (the GLX layer allocates it in frame_begin) */
  if (zb->xsize != width || zb->ysize != height) {
    zb_drop_depth(zb);
    zb->xsize = width;
    zb->ysize = height;
  }
  zb->linesize = pixels ? pitch : 2 * width;
  zb->pbuf = pixels;
  zb->frame_buffer_allocated = 0;
  c->viewport.updated = 1;
  if (!c->vp_initialized && width > 0 && height > 0) {
    c->vp_initialized = 1;
    c->viewport.xmin = 0;
    c->viewport.ymin = 0;
    c->viewport.xsize = width;
    c->viewport.ysize = height;
    c->scissor[0] = 0;
    c->scissor[1] = 0;
    c->scissor[2] = width;
    c->scissor[3] = height;
  }
  return 0;
}

void tgl_ctx_set_prepare(void *ctx, int (*prepare)(void *user), void *user)
{
  GLContext *c = ctx;
  c->prepare = prepare;
  c->prepare_user = user;
}

void tgl_ctx_make_current(void *ctx)
{
  gl_ctx = ctx ? (GLContext *)ctx : null_ctx;
  gl_ctx->ready = 0;
  gl_ctx->armed = 1;
}

/* the frame was presented: call the frame hook before the next access */
void tgl_ctx_arm(void *ctx)
{
  GLContext *c = ctx;
  if (c == NULL) return;
  c->ready = 0;
  c->armed = 1;
}

void *tgl_ctx_current(void)
{
  return gl_ctx == null_ctx ? NULL : gl_ctx;
}

struct tgl_attrib_slots *tgl_attrib_slots(void)
{
  return gl_ctx == NULL || gl_ctx == null_ctx ? NULL : &gl_ctx->attrib;
}

int tgl_ctx_depth_bytes(void *ctx)
{
  GLContext *c = ctx;
  return c && c->zb->zbuf ? c->zb->xsize * c->zb->ysize * 2 : 0;
}

void tgl_ctx_release_depth(void *ctx)
{
  GLContext *c = ctx;
  if (c == NULL) return;
  zb_drop_depth(c->zb);
  c->ready = 0;
}

/*
 * Use caller-owned depth memory (xsize * ysize 16-bit values at the size
 * bound now) instead of a private buffer: GL's ancillary buffers belong to
 * the DRAWABLE, so every context current on one window shares its depth.
 * NULL goes back to a private buffer, allocated lazily. The caller keeps
 * it valid while bound and rebinds after any bind that changes the size
 * (which forgets it). Returns -1 when no size is bound.
 */
int tgl_ctx_bind_depth(void *ctx, void *depth)
{
  GLContext *c = ctx;
  ZBuffer *zb;
  if (c == NULL) return -1;
  zb = c->zb;
  if (depth != NULL && (zb->xsize <= 0 || zb->ysize <= 0)) return -1;
  if (depth == zb->zbuf && zb->zbuf_ext == (depth != NULL)) return 0;
  zb_drop_depth(zb);
  zb->zbuf = depth;
  zb->zbuf_ext = depth != NULL;
  c->ready = 0;
  return 0;
}

void tgl_ctx_set_doublebuffer(void *ctx, int on)
{
  GLContext *c = ctx;
  if (c) {
    c->doublebuffer = on != 0;
    c->draw_buffer = c->read_buffer = on ? GL_BACK : GL_FRONT;
  }
}

/* slow path of gl_prepare(): 1 = colour and depth exist.
   The frame hook runs once per arming (make-current, bind, frame end),
   before the first access; it may bind a buffer (it is how the colour
   buffer is allocated lazily) and may wait for the previous present. */
int gl_prepare_slow(GLContext *c)
{
  ZBuffer *zb = c->zb;

  if (c->armed && c->prepare != NULL && !c->prepare_busy) {
    c->armed = 0;
    c->prepare_busy = 1;          /* the callback may call back into GL */
    c->prepare(c->prepare_user);
    c->prepare_busy = 0;
    c->armed = 0;                 /* a bind inside the hook re-armed it */
    zb = c->zb;
  }
  c->armed = 0;
  if (zb->pbuf == NULL || zb->xsize <= 0 || zb->ysize <= 0)
    return 0;
  if (zb->zbuf == NULL) {
    zb->zbuf = gl_malloc(zb->xsize * zb->ysize * sizeof(unsigned short));
    if (zb->zbuf == NULL) {
      gl_set_error(c, GL_OUT_OF_MEMORY);
      return 0;
    }
    /* GL leaves new depth undefined; far is the useful value */
    memset(zb->zbuf, 0, zb->xsize * zb->ysize * sizeof(unsigned short));
  }
  c->ready = 1;
  return 1;
}
