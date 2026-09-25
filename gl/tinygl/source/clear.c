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

  ZB_clear(c->zb,mask & GL_DEPTH_BUFFER_BIT,z,
	   mask & GL_COLOR_BUFFER_BIT,r,g,b);
}
