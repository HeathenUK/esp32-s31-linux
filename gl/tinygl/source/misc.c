#include "zgl.h"
#include "msghandling.h"

void glopViewport(GLContext *c,GLParam *p)
{
  int xsize,ysize,xmin,ymin,xsize_req,ysize_req;
  
  xmin=p[1].i;
  ymin=p[2].i;
  xsize=p[3].i;
  ysize=p[4].i;
  (void)xsize_req; (void)ysize_req;

  /* s31: the buffer is the caller's and is never resized from here. Any
     viewport is legal, including one larger than or outside the buffer and
     an empty one: gl_eval_viewport() clips to what is on the buffer. */
  c->vp_initialized = 1;   /* s31: a later first bind must not override it */
  if (c->viewport.xmin != xmin ||
      c->viewport.ymin != ymin ||
      c->viewport.xsize != xsize ||
      c->viewport.ysize != ysize) {
    c->viewport.xmin=xmin;
    c->viewport.ymin=ymin;
    c->viewport.xsize=xsize;
    c->viewport.ysize=ysize;
    
    c->viewport.updated=1;
  }
}

void glopEnableDisable(GLContext *c,GLParam *p)
{
  int code=p[1].i;
  int v=p[2].i;

  /* s31: every capability is recorded for glIsEnabled; the ones the
     rasteriser does not honour yet say so once */
  s31_cap_record(c, code, v);

  switch(code) {
  case GL_CULL_FACE:
    c->cull_face_enabled=v;
    break;
  case GL_LIGHTING:
    c->lighting_enabled=v;
    /* s31: the two vertex paths use different matrices; the composed one
       was left stale when lighting was switched off without a matrix
       change in between */
    c->matrix_model_projection_updated=1;
    break;
  case GL_COLOR_MATERIAL:
    c->color_material_enabled=v;
      break;
  case GL_TEXTURE_2D:
    c->texture_2d_enabled=v;
    break;
  case GL_NORMALIZE:
    c->normalize_enabled=v;
    break;
  case GL_DEPTH_TEST:
    c->depth_test = v;
    break;
  case GL_POLYGON_OFFSET_FILL:
    if (v) c->offset_states |= TGL_OFFSET_FILL;
    else c->offset_states &= ~TGL_OFFSET_FILL;
    break; 
  case GL_POLYGON_OFFSET_POINT:
    if (v) c->offset_states |= TGL_OFFSET_POINT;
    else c->offset_states &= ~TGL_OFFSET_POINT;
    break; 
  case GL_POLYGON_OFFSET_LINE:
    if (v) c->offset_states |= TGL_OFFSET_LINE;
    else c->offset_states &= ~TGL_OFFSET_LINE;
    break; 
  default:
    if (code>=GL_LIGHT0 && code<GL_LIGHT0+MAX_LIGHTS) {
      gl_enable_disable_light(c,code - GL_LIGHT0, v);
    } else {
      /*
      fprintf(stderr,"glEnableDisable: 0x%X not supported.\n",code);
      */
    }
    break;
  }
}

void glopShadeModel(GLContext *c,GLParam *p)
{
  int code=p[1].i;
  c->current_shade_model=code;
}

void glopCullFace(GLContext *c,GLParam *p)
{
  int code=p[1].i;
  c->current_cull_face=code;
}

void glopFrontFace(GLContext *c,GLParam *p)
{
  int code=p[1].i;
  c->current_front_face=code;
}

void glopPolygonMode(GLContext *c,GLParam *p)
{
  int face=p[1].i;
  int mode=p[2].i;
  
  switch(face) {
  case GL_BACK:
    c->polygon_mode_back=mode;
    break;
  case GL_FRONT:
    c->polygon_mode_front=mode;
    break;
  case GL_FRONT_AND_BACK:
    c->polygon_mode_front=mode;
    c->polygon_mode_back=mode;
    break;
  default:
    break;
  }
}

void glopHint(GLContext *c,GLParam *p)
{
  /* s31: recorded for glGet; hints may be ignored by definition */
  int target=p[1].i;
  int mode=p[2].i;

  switch (target) {
  case GL_PERSPECTIVE_CORRECTION_HINT: c->hint_perspective = mode; break;
  case GL_POINT_SMOOTH_HINT: c->hint_point = mode; break;
  case GL_LINE_SMOOTH_HINT: c->hint_line = mode; break;
  case GL_POLYGON_SMOOTH_HINT: c->hint_polygon = mode; break;
  case GL_FOG_HINT: c->hint_fog = mode; break;
  default: break;
  }
}

void 
glopPolygonOffset(GLContext *c, GLParam *p)
{
  c->offset_factor = p[1].f;
  c->offset_units = p[2].f;
}
