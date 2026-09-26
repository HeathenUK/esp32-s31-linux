#include "zgl.h"


void glopClearColor(GLContext *c,GLParam *p)
{
  /* s31: GL clamps the clear colour to [0,1] */
  int i;
  for (i = 0; i < 4; i++) {
    float v = p[1 + i].f;
    c->clear_color.v[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  }
}
void glopClearDepth(GLContext *c,GLParam *p)
{
  float d = p[1].f;
  c->clear_depth = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}


void glopClear(GLContext *c,GLParam *p)
{
  int mask=p[1].i;
  int r=(int)(c->clear_color.v[0]*65535);
  int g=(int)(c->clear_color.v[1]*65535);
  int b=(int)(c->clear_color.v[2]*65535);
  ZBuffer *zb;
  int cm, x0, y0, x1, y1, full, dz, dc;

  /* s31: lazily allocate depth (and ask for colour) at the first clear */
  if (!gl_prepare(c)) return;
  zb = c->zb;

  /* s31: TinyGL's Z grows towards the viewer (near = 0xffff, far = 0,
     tested zz >= zpix), so window depth d is stored as (1 - d) * 0xffff,
     in the depth buffer's current epoch (s31_zepoch.c); the default clear
     depth 1.0 is the epoch's lowest value.

     s31 (plan F5/F6): glClear obeys the scissor box, the colour write mask
     and the depth mask (GL 1.3 4.2.3). A scissor box that covers the whole
     buffer is no box. */
  cm = (c->color_mask[0] ? 0xf800 : 0) | (c->color_mask[1] ? 0x07e0 : 0) |
       (c->color_mask[2] ? 0x001f : 0);
  x0 = 0; y0 = 0; x1 = zb->xsize; y1 = zb->ysize;
  if (c->scissor_enabled) {
    /* the box is in the window's units (render scale: GLContext.rscale) */
    int rs = c->rscale, vh = zb->ysize << rs;
    int sx0 = c->scissor[0], sx1 = c->scissor[0] + c->scissor[2];
    int sy0 = vh - (c->scissor[1] + c->scissor[3]);
    int sy1 = vh - c->scissor[1];
    if (rs) {
      sx0 >>= rs; sy0 >>= rs;
      sx1 = -((-sx1) >> rs); sy1 = -((-sy1) >> rs);
    }
    if (sx0 > x0) x0 = sx0;
    if (sx1 < x1) x1 = sx1;
    if (sy0 > y0) y0 = sy0;
    if (sy1 < y1) y1 = sy1;
  }
  full = x0 == 0 && y0 == 0 && x1 == zb->xsize && y1 == zb->ysize;
  dz = (mask & GL_DEPTH_BUFFER_BIT) && c->depth_mask;
  dc = (mask & GL_COLOR_BUFFER_BIT) && cm != 0;

  {
    int creset = 0, zreset = 0;
    if (dz) {
      if (full) {
        zreset = zep_clear(c, c->clear_depth);   /* phase 3a G03: usually no write */
      } else {
        /* fitted above the epoch's base (demoted first if it does not
           fit) and recorded in the shared zmax: s31_zepoch.c */
        unsigned int v = zep_clear_rect_value(c, c->clear_depth);
        ZB_clear_rect(zb, x0, y0, x1, y1, 1, v, 0, 0, 0xffff);
      }
    }
    if (dc) {
      if (full && cm == 0xffff) {
        /* phase 3a dirty box (s31_zepoch.c): only what was drawn into */
        zdb_clear_colour(c, RGB_TO_PIXEL(r, g, b));
        creset = 1;
      } else {
        ZB_clear_rect(zb, x0, y0, x1, y1, 0, 0, 1, RGB_TO_PIXEL(r, g, b), cm);
      }
    }
    /* a partial clear is a drawing like any other for the dirty boxes */
    if ((dz && !full) || (dc && !creset)) zdb_grow(c, x0, y0, x1, y1);
    zdb_fold(c, creset, zreset);
  }
}
