/*
 * s31_rpos.c - plan F7 state commands: glClipPlane, glTexGen, glRasterPos /
 * glWindowPos and their queries. Run once per call, never per vertex or
 * pixel, so built -Os (gl/api/build-lib.sh); the per-vertex half is
 * s31_xform.c. s31, MIT. Float only (F without D).
 */
#include "zgl.h"
#include "s31_xform.h"

/* ------------------------------------------------------------ clip planes */

void glopClipPlane(GLContext *c, GLParam *p)
{
  int i = p[1].i;
  V4 eq = gl_V4_New(p[2].f, p[3].f, p[4].f, p[5].f);
  M4 inv;
  /* GL 1.3 2.11: p' = p M^-1 with M the modelview when specified */
  gl_M4_Inv(&inv, c->matrix_stack_ptr[0]);
  gl_xf_plane_mul(&c->clip_plane_eye[i], &eq, &inv);
  c->matrix_model_projection_updated = 1;
}

void tgl_clip_plane(int plane, const float *eq)
{
  GLContext *c = gl_get_context();
  GLParam p[6];
  int i;
  if (plane < GL_CLIP_PLANE0 || plane > GL_CLIP_PLANE5) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  p[0].op = OP_ClipPlane;
  p[1].i = plane - GL_CLIP_PLANE0;
  for (i = 0; i < 4; i++) p[2 + i].f = eq[i];
  gl_add_op(p);
}

int tgl_get_clip_plane(int plane, float *eq)
{
  GLContext *c = gl_get_context();
  int i;
  if (plane < GL_CLIP_PLANE0 || plane > GL_CLIP_PLANE5) return -1;
  for (i = 0; i < 4; i++) eq[i] = c->clip_plane_eye[plane - GL_CLIP_PLANE0].v[i];
  return 4;
}

/* ------------------------------------------------------------ texgen */

void glopTexGen(GLContext *c, GLParam *p)
{
  int i = p[1].i, pname = p[2].i;
  V4 eq = gl_V4_New(p[4].f, p[5].f, p[6].f, p[7].f);
  M4 inv;

  switch (pname) {
  case GL_TEXTURE_GEN_MODE:
    c->texgen_mode[i] = p[3].i;
    if (p[3].i == GL_NORMAL_MAP || p[3].i == GL_REFLECTION_MAP)
      gl_warn_once("glTexGen(GL_NORMAL_MAP / GL_REFLECTION_MAP) (no cube maps)");
    break;
  case GL_OBJECT_PLANE:
    c->texgen_obj[i] = eq;
    break;
  case GL_EYE_PLANE:
    /* GL 1.3 2.10.4: p M^-1, M the modelview when specified */
    gl_M4_Inv(&inv, c->matrix_stack_ptr[0]);
    gl_xf_plane_mul(&c->texgen_eye[i], &eq, &inv);
    break;
  }
  c->matrix_model_projection_updated = 1;
}

/* coord GL_S..GL_Q; v NULL: the integer mode */
void tgl_tex_gen(int coord, int pname, int iparam, const float *v)
{
  GLContext *c = gl_get_context();
  GLParam p[8];
  int i;

  if (coord < GL_S || coord > GL_Q) { gl_set_error(c, GL_INVALID_ENUM); return; }
  switch (pname) {
  case GL_TEXTURE_GEN_MODE:
    switch (iparam) {
    case GL_OBJECT_LINEAR: case GL_EYE_LINEAR:
      break;
    case GL_SPHERE_MAP:
      if (coord == GL_R || coord == GL_Q) { gl_set_error(c, GL_INVALID_ENUM); return; }
      break;
    case GL_NORMAL_MAP: case GL_REFLECTION_MAP:
      if (coord == GL_Q) { gl_set_error(c, GL_INVALID_ENUM); return; }
      break;
    default:
      gl_set_error(c, GL_INVALID_ENUM);
      return;
    }
    break;
  case GL_OBJECT_PLANE: case GL_EYE_PLANE:
    if (v == NULL) { gl_set_error(c, GL_INVALID_ENUM); return; }
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  p[0].op = OP_TexGen;
  p[1].i = coord - GL_S;
  p[2].i = pname;
  p[3].i = iparam;
  for (i = 0; i < 4; i++) p[4 + i].f = v ? v[i] : 0.0f;
  gl_add_op(p);
}

int tgl_get_tex_gen(int coord, int pname, float *v)
{
  GLContext *c = gl_get_context();
  int i, k;
  if (coord < GL_S || coord > GL_Q) return -1;
  k = coord - GL_S;
  switch (pname) {
  case GL_TEXTURE_GEN_MODE: v[0] = (float)c->texgen_mode[k]; return 1;
  case GL_OBJECT_PLANE: for (i = 0; i < 4; i++) v[i] = c->texgen_obj[k].v[i]; return 4;
  case GL_EYE_PLANE: for (i = 0; i < 4; i++) v[i] = c->texgen_eye[k].v[i]; return 4;
  default: return -1;
  }
}

/* ------------------------------------------------------------ raster position */

static float clamp01(float v)
{
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* GL 1.3 2.12: p[1..4] the object coordinates; p[5] = 1 for glWindowPos
   (GL 1.4 2.12, ARB_window_pos): p[1..3] are window coordinates */
void glopRasterPos(GLContext *c, GLParam *p)
{
  V4 obj = gl_V4_New(p[1].f, p[2].f, p[3].f, p[4].f), eye, clip, tc;
  const float *m;
  float n = c->depth_range[0], f = c->depth_range[1], winv;
  int i;

  if (p[5].i) {
    c->raster_pos[0] = obj.X;
    c->raster_pos[1] = obj.Y;
    c->raster_pos[2] = n + (f - n) * clamp01(obj.Z);
    c->raster_pos[3] = obj.W;          /* 1, or MESA_window_pos's w */
    c->raster_valid = 1;
    for (i = 0; i < 4; i++) {
      c->raster_color[i] = c->current_color.v[i];
      c->raster_tex[i] = c->current_tex_coord.v[i];
    }
    c->raster_distance = 0.0f;
    c->raster_fogz = 0.0f;
    return;
  }

  gl_xf_eye_coords(c, &obj, &eye);
  m = &c->matrix_stack_ptr[1]->m[0][0];
  clip.X = eye.X * m[0] + eye.Y * m[1] + eye.Z * m[2] + eye.W * m[3];
  clip.Y = eye.X * m[4] + eye.Y * m[5] + eye.Z * m[6] + eye.W * m[7];
  clip.Z = eye.X * m[8] + eye.Y * m[9] + eye.Z * m[10] + eye.W * m[11];
  clip.W = eye.X * m[12] + eye.Y * m[13] + eye.Z * m[14] + eye.W * m[15];

  /* the view volume and the user planes (GL 1.3 2.12) */
  if (clip.X < -clip.W || clip.X > clip.W || clip.Y < -clip.W ||
      clip.Y > clip.W || clip.Z < -clip.W || clip.Z > clip.W) {
    c->raster_valid = 0;
    return;
  }
  for (i = 0; i < 6; i++) {
    const float *q = c->clip_plane_eye[i].v;
    if ((c->clip_plane_mask & (1 << i)) &&
        q[0] * eye.X + q[1] * eye.Y + q[2] * eye.Z + q[3] * eye.W < 0.0f) {
      c->raster_valid = 0;
      return;
    }
  }
  c->raster_valid = 1;
  winv = 1.0f / clip.W;
  /* GL's viewport transform (window y up, the full viewport) */
  c->raster_pos[0] = (clip.X * winv + 1.0f) * (float)c->viewport.xsize * 0.5f +
                     (float)c->viewport.xmin;
  c->raster_pos[1] = (clip.Y * winv + 1.0f) * (float)c->viewport.ysize * 0.5f +
                     (float)c->viewport.ymin;
  c->raster_pos[2] = n + (f - n) * (clip.Z * winv + 1.0f) * 0.5f;
  c->raster_pos[3] = clip.W;
  /* GL 2.12: the eye distance (glGet); fog of the pixel fragments uses
     |z_e|, the approximation GL 3.10 allows and the vertices use
     (vertex.c gl_vertex_fog) */
  c->raster_distance = sqrtf(eye.X * eye.X + eye.Y * eye.Y + eye.Z * eye.Z);
  c->raster_fogz = fabsf(eye.Z);

  /* the raster colour: lit as a vertex is */
  if (c->lighting_enabled) {
    GLVertex v;
    M4 mit;
    if (c->raster_dirty) gl_update_raster(c);  /* raster_sepspec */
    memset(&v, 0, sizeof(v));
    gl_xf_mv_inv_t(c, &mit);
    gl_xf_eye_normal(c, &mit, &v.normal);
    v.ec = eye;
    v.coord = obj;
    gl_shade_vertex(c, &v);
    if (c->raster_sepspec) {
      /* no colour sum on the pixel paths: fold the secondary colour in */
      for (i = 0; i < 3; i++) v.color.v[i] = clamp01(v.color.v[i] + v.spec.v[i]);
    }
    for (i = 0; i < 4; i++) c->raster_color[i] = v.color.v[i];
  } else {
    for (i = 0; i < 4; i++) c->raster_color[i] = c->current_color.v[i];
  }

  /* the raster texture coordinates: texgen, then the texture matrix */
  tc = c->current_tex_coord;
  if (c->texgen_mask) {
    V3 en = { { 0.0f, 0.0f, 1.0f } };
    M4 mit;
    gl_xf_mv_inv_t(c, &mit);
    gl_xf_eye_normal(c, &mit, &en);
    gl_texgen_coords(c, &obj, &eye, &en, &tc, &tc);
  }
  if (!gl_M4_IsId(c->matrix_stack_ptr[2]))
    gl_M4_MulV4((V4 *)c->raster_tex, c->matrix_stack_ptr[2], &tc);
  else
    for (i = 0; i < 4; i++) c->raster_tex[i] = tc.v[i];
}

void tgl_raster_pos(float x, float y, float z, float w, int window)
{
  GLContext *c = gl_get_context();
  GLParam p[6];
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  p[0].op = OP_RasterPos;
  p[1].f = x; p[2].f = y; p[3].f = z; p[4].f = w;
  p[5].i = window;
  gl_add_op(p);
}

/* glPush/PopAttrib(GL_CURRENT_BIT): pos[4] colour[4] texcoord[4]
   distance valid */
void tgl_raster_state(float *v, int set)
{
  GLContext *c = gl_get_context();
  int i;
  if (set) {
    for (i = 0; i < 4; i++) {
      c->raster_pos[i] = v[i];
      c->raster_color[i] = v[4 + i];
      c->raster_tex[i] = v[8 + i];
    }
    c->raster_distance = v[12];
    c->raster_valid = v[13] != 0.0f;
  } else {
    for (i = 0; i < 4; i++) {
      v[i] = c->raster_pos[i];
      v[4 + i] = c->raster_color[i];
      v[8 + i] = c->raster_tex[i];
    }
    v[12] = c->raster_distance;
    v[13] = (float)c->raster_valid;
  }
}
