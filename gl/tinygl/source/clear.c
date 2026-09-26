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
  int z;
  int r=(int)(c->clear_color.v[0]*65535);
  int g=(int)(c->clear_color.v[1]*65535);
  int b=(int)(c->clear_color.v[2]*65535);

  /* s31: lazily allocate depth (and ask for colour) at the first clear */
  if (!gl_prepare(c)) return;

  /* s31: TinyGL's Z grows towards the viewer (near = 0xffff, far = 0,
     tested zz >= zpix), so window depth d is stored as (1 - d) * 0xffff.
     The default clear depth 1.0 is 0, as before. */
  z = (int)((1.0f - c->clear_depth) * 65535.0f);

  /* s31 (plan F5/F6): glClear obeys the scissor box, the colour write mask
     and the depth mask (GL 1.3 4.2.3). The default state keeps TinyGL's
     whole-buffer clear. */
  if (c->scissor_enabled || !c->depth_mask ||
      !c->color_mask[0] || !c->color_mask[1] || !c->color_mask[2]) {
    ZBuffer *zb = c->zb;
    int cm = (c->color_mask[0] ? 0xf800 : 0) | (c->color_mask[1] ? 0x07e0 : 0) |
             (c->color_mask[2] ? 0x001f : 0);
    int x0 = 0, y0 = 0, x1 = zb->xsize, y1 = zb->ysize;
    if (c->scissor_enabled) {
      int sx0 = c->scissor[0], sx1 = c->scissor[0] + c->scissor[2];
      int sy0 = zb->ysize - (c->scissor[1] + c->scissor[3]);
      int sy1 = zb->ysize - c->scissor[1];
      if (sx0 > x0) x0 = sx0;
      if (sx1 < x1) x1 = sx1;
      if (sy0 > y0) y0 = sy0;
      if (sy1 < y1) y1 = sy1;
    }
    ZB_clear_rect(zb, x0, y0, x1, y1,
                  (mask & GL_DEPTH_BUFFER_BIT) && c->depth_mask, z,
                  (mask & GL_COLOR_BUFFER_BIT) && cm != 0, RGB_TO_PIXEL(r, g, b), cm);
    return;
  }

  ZB_clear(c->zb,mask & GL_DEPTH_BUFFER_BIT,z,
	   mask & GL_COLOR_BUFFER_BIT,r,g,b);
}
