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
  c->raster_dirty = 1;   /* s31: raster.c chooses the fillers again */

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
    c->xf_dirty |= 2;   /* s31 (phase 3a G14) */
    break;
  case GL_COLOR_MATERIAL:
    c->color_material_enabled=v;
      break;
  case GL_TEXTURE_2D:
  case GL_TEXTURE_1D:
    /* s31: texture_2d_enabled means "a texture target is enabled"
       (the vertex path carries texcoords); raster.c picks the object,
       2D over 1D as GL 3.8.15 orders them */
    {
      int bit = code == GL_TEXTURE_2D ? 1 : 2;
      if (v) c->tex_enables |= bit; else c->tex_enables &= ~bit;
      c->texture_2d_enabled = c->tex_enables != 0;
    }
    break;
  /* s31: plan F7 */
  case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
  case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5:
    if (v) c->clip_plane_mask |= 1 << (code - GL_CLIP_PLANE0);
    else c->clip_plane_mask &= ~(1 << (code - GL_CLIP_PLANE0));
    c->matrix_model_projection_updated = 1;   /* glBegin: clip-space planes */
    break;
  case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T:
  case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q:
    if (v) c->texgen_mask |= 1 << (code - GL_TEXTURE_GEN_S);
    else c->texgen_mask &= ~(1 << (code - GL_TEXTURE_GEN_S));
    c->matrix_model_projection_updated = 1;   /* glBegin: apply_texture_matrix */
    break;
  case GL_POLYGON_STIPPLE:
    c->poly_stipple_enabled = v;
    break;
  case GL_LINE_STIPPLE:
    c->line_stipple_enabled = v;
    break;
  case GL_NORMALIZE:
    c->normalize_enabled=v;
    break;
  case GL_DEPTH_TEST:
    c->depth_test = v;
    break;
  /* s31: honoured by raster.c / zpipe.c (plan F5, F6) */
  case GL_BLEND:
    c->blend_enabled = v;
    break;
  case GL_ALPHA_TEST:
    c->alpha_test_enabled = v;
    break;
  case GL_FOG:
    c->fog_enabled = v;
    break;
  /* s31 phase 4: F8-STENCIL (raster_sel.c, zpipe.c) and SMOOTH (raster.c) */
  case GL_STENCIL_TEST:
    c->p4_en = v ? c->p4_en | P4_EN_STENCIL : c->p4_en & ~P4_EN_STENCIL;
    break;
  case GL_LINE_SMOOTH:
    c->p4_en = v ? c->p4_en | P4_EN_LSMOOTH : c->p4_en & ~P4_EN_LSMOOTH;
    break;
  case GL_POINT_SMOOTH:
    c->p4_en = v ? c->p4_en | P4_EN_PSMOOTH : c->p4_en & ~P4_EN_PSMOOTH;
    break;
  case GL_SCISSOR_TEST:
    c->scissor_enabled = v;
    c->viewport.updated = 1;   /* the scissor box clips like the viewport */
    break;
  case GL_RESCALE_NORMAL:
    c->rescale_normal_enabled = v;
    c->matrix_model_projection_updated = 1;
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
  c->raster_dirty = 1;   /* s31: flat/smooth colour stage */
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
