/*
 * s31_xform.c - the vertex state of plan F7: user clip planes, texture
 * coordinate generation and the raster position. s31, MIT.
 *
 * None of it costs a frame that does not use it:
 *  - glopVertex's one existing test (raster_fog) became vtx_extra, which
 *    gl_update_raster sets from fog and the clip-plane enables, so the
 *    user planes are one call behind a test that was already there;
 *  - texgen rides on apply_texture_matrix (bit 1), which glopVertex
 *    already tested, and moved the texture-matrix product out of line;
 *  - the planes in clip space and texgen's matrices are refreshed at
 *    glBegin only when the matrices changed and a feature is on.
 *
 * User clip planes are tested in CLIP space: the eye-space plane p (GL 1.3
 * 2.11: transformed by the inverse modelview when specified) times the
 * inverse of the projection in use (proj_used, the viewport guard folded
 * in) gives p_c with p_c . pc = p . eye for every vertex, including the
 * ones the clipper makes by interpolating pc - so the clipper needs no
 * extra per-vertex storage. They are clip-code bits 6..11 (clip.c).
 *
 * The per-vertex half is here (-O2); the state commands, the queries and
 * glRasterPos are in s31_rpos.c (cold, -Os).
 *
 * Float only (F without D): sqrtf, never sqrt.
 */
#include "zgl.h"
#include "s31_xform.h"

/* the transposed inverse of the modelview, as light.c's normal transform
   uses it (m[0..2] row 0 = the normal's x) */
void gl_xf_mv_inv_t(GLContext *c, M4 *out)
{
  M4 tmp;
  gl_M4_Inv(&tmp, c->matrix_stack_ptr[0]);
  gl_M4_Transpose(out, &tmp);
}

/* plane p times m (row vector times matrix): out_j = sum_i p_i m[i][j] */
void gl_xf_plane_mul(V4 *out, const V4 *p, const M4 *m)
{
  int i, j;
  V4 r;
  for (j = 0; j < 4; j++) {
    float s = 0.0f;
    for (i = 0; i < 4; i++) s += p->v[i] * m->m[i][j];
    r.v[j] = s;
  }
  *out = r;
}

int gl_user_clipcode(const GLContext *c, const V4 *pc)
{
  int m = c->clip_plane_mask, i, code = 0;
  for (i = 0; m; i++, m >>= 1) {
    const float *p;
    if (!(m & 1)) continue;
    p = c->clip_plane_clip[i].v;
    if (p[0] * pc->X + p[1] * pc->Y + p[2] * pc->Z + p[3] * pc->W < 0.0f)
      code |= 1 << (TGL_CLIP_USER_SHIFT + i);
  }
  return code;
}

static void gl_vertex_texcoord1(GLContext *c, GLVertex *v);

void gl_vertex_extra(GLContext *c, GLVertex *v)
{
  if (c->vtx_extra & 1)
    gl_vertex_fog(c, v);
  if (c->vtx_extra & 2)
    v->clip_code |= gl_user_clipcode(c, &v->pc);
  /* phase 5 O1: texture unit 1's coordinates. Before lighting, which
     overwrites the normal texgen reads (zgl.h GLVertex) */
  if (c->vtx_extra & 4)
    gl_vertex_texcoord1(c, v);
}

void gl_update_xform(GLContext *c)
{
  int i;
  if (c->clip_plane_mask) {
    M4 pinv;
    gl_M4_Inv(&pinv, c->proj_used);
    for (i = 0; i < 6; i++)
      if (c->clip_plane_mask & (1 << i))
        gl_xf_plane_mul(&c->clip_plane_clip[i], &c->clip_plane_eye[i], &pinv);
  }
  c->texgen_eye_needed = 0;
  for (i = 0; i < 4; i++)
    if ((c->texgen_mask & (1 << i)) && c->texgen_mode[i] != GL_OBJECT_LINEAR)
      c->texgen_eye_needed = 1;
  /* phase 5 O1: texture unit 1's texgen, likewise */
  c->tu1.texgen_eye_needed = 0;
  for (i = 0; i < 4; i++)
    if ((c->tu1.texgen_mask & (1 << i)) && c->tu1.texgen_mode[i] != GL_OBJECT_LINEAR)
      c->tu1.texgen_eye_needed = 1;
  if (((c->texgen_mask && c->texgen_eye_needed) ||
       (c->tu1.texgen_mask && c->tu1.texgen_eye_needed)) && !c->lighting_enabled)
    gl_xf_mv_inv_t(c, &c->texgen_mv_inv);
}

/* phase 5 O1: glBegin with the matrices changed, once unit 1 was used:
   unit 1's texture matrix and texgen (GLContext.tu1_apply) */
void gl_tu1_begin(GLContext *c)
{
  c->tu1_apply = (!gl_M4_IsId(c->matrix_stack_ptr[3])) | (c->tu1.texgen_mask ? 2 : 0);
  if (c->tu1.texgen_mask && !(c->clip_plane_mask | c->texgen_mask))
    gl_update_xform(c);        /* (else glBegin just did) */
}

/* GL 1.3 2.10.4. obj, eye: the vertex; en: its eye normal (sphere map);
   in: the current texture coordinates; out may be in */
/* (phase 5 O1: the texgen state as arguments - unit 0's is the context's
   fields, unit 1's GLContext.tu1) */
static void texgen_coords(int m, const int *mode, const V4 *tobj, const V4 *teye,
                          const V4 *obj, const V4 *eye, const V3 *en,
                          const V4 *in, V4 *out)
{
  float sm[2] = { 0.0f, 0.0f };
  int i, sphere = 0;
  V4 r = *in;

  for (i = 0; i < 4; i++)
    if ((m & (1 << i)) && mode[i] == GL_SPHERE_MAP) sphere = 1;
  if (sphere) {
    /* u = the unit eye position, r = u - 2 n (n . u),
       m = 2 sqrt(rx^2 + ry^2 + (rz + 1)^2); s,t = r / m + 1/2 */
    float ux = eye->X, uy = eye->Y, uz = eye->Z, l, d, rx, ry, rz, mm;
    l = ux * ux + uy * uy + uz * uz;
    if (l > 0.0f) {
      l = 1.0f / sqrtf(l);
      ux *= l; uy *= l; uz *= l;
    }
    d = 2.0f * (en->X * ux + en->Y * uy + en->Z * uz);
    rx = ux - en->X * d;
    ry = uy - en->Y * d;
    rz = uz - en->Z * d + 1.0f;
    mm = rx * rx + ry * ry + rz * rz;
    mm = mm > 0.0f ? 0.5f / sqrtf(mm) : 0.0f;
    sm[0] = rx * mm + 0.5f;
    sm[1] = ry * mm + 0.5f;
  }
  for (i = 0; i < 4; i++) {
    const float *p;
    if (!(m & (1 << i))) continue;
    switch (mode[i]) {
    case GL_OBJECT_LINEAR:
      p = tobj[i].v;
      r.v[i] = p[0] * obj->X + p[1] * obj->Y + p[2] * obj->Z + p[3] * obj->W;
      break;
    case GL_EYE_LINEAR:
      p = teye[i].v;
      r.v[i] = p[0] * eye->X + p[1] * eye->Y + p[2] * eye->Z + p[3] * eye->W;
      break;
    case GL_SPHERE_MAP:
      if (i < 2) r.v[i] = sm[i];
      break;
    default:
      /* GL_NORMAL_MAP / GL_REFLECTION_MAP: recorded, for cube maps,
         which this library does not have */
      break;
    }
  }
  *out = r;
}

void gl_texgen_coords(GLContext *c, const V4 *obj, const V4 *eye,
                      const V3 *en, const V4 *in, V4 *out)
{
  texgen_coords(c->texgen_mask, c->texgen_mode, c->texgen_obj, c->texgen_eye,
                obj, eye, en, in, out);
}

/* the eye normal of the current normal: M^-T n, normalised as GL_NORMALIZE
   says (GL_RESCALE_NORMAL's factor exists only with lighting on) */
void gl_xf_eye_normal(GLContext *c, const M4 *mit, V3 *en)
{
  const float *m = &mit->m[0][0];
  const V4 *n = &c->current_normal;
  en->X = n->X * m[0] + n->Y * m[1] + n->Z * m[2];
  en->Y = n->X * m[4] + n->Y * m[5] + n->Z * m[6];
  en->Z = n->X * m[8] + n->Y * m[9] + n->Z * m[10];
  if (c->normalize_enabled) {
    /* float only (gl_V3_Norm's sqrt is a double libcall) */
    float l = en->X * en->X + en->Y * en->Y + en->Z * en->Z;
    if (l > 0.0f) {
      l = 1.0f / sqrtf(l);
      en->X *= l; en->Y *= l; en->Z *= l;
    }
  } else if (c->rescale_normal_enabled && c->lighting_enabled) {
    en->X *= c->rescale; en->Y *= c->rescale; en->Z *= c->rescale;
  }
}

void gl_xf_eye_coords(GLContext *c, const V4 *o, V4 *e)
{
  const float *m = &c->matrix_stack_ptr[0]->m[0][0];
  e->X = o->X * m[0] + o->Y * m[1] + o->Z * m[2] + o->W * m[3];
  e->Y = o->X * m[4] + o->Y * m[5] + o->Z * m[6] + o->W * m[7];
  e->Z = o->X * m[8] + o->Y * m[9] + o->Z * m[10] + o->W * m[11];
  e->W = o->X * m[12] + o->Y * m[13] + o->Z * m[14] + o->W * m[15];
}

/* glopVertex when apply_texture_matrix: bit 1 texgen, bit 0 the matrix */
void gl_vertex_texcoord(GLContext *c, GLVertex *v)
{
  V4 tc = c->current_tex_coord;

  if (c->apply_texture_matrix & 2) {
    V4 eye;
    V3 en = { { 0.0f, 0.0f, 1.0f } };
    if (c->texgen_eye_needed) {
      if (c->lighting_enabled) {
        eye = v->ec;
        gl_xf_eye_normal(c, &c->matrix_model_view_inv, &en);
      } else {
        gl_xf_eye_coords(c, &v->coord, &eye);
        gl_xf_eye_normal(c, &c->texgen_mv_inv, &en);
      }
    } else {
      eye = v->coord;
    }
    gl_texgen_coords(c, &v->coord, &eye, &en, &tc, &tc);
  }
  if (c->apply_texture_matrix & 1)
    gl_M4_MulV4(&v->tex_coord, c->matrix_stack_ptr[2], &tc);
  else
    v->tex_coord = tc;
}


/* phase 5 O1: texture unit 1's coordinates of a vertex, as
   gl_vertex_texcoord makes unit 0's: texgen (tu1_apply bit 1), then its
   texture matrix (bit 0) */
static void gl_vertex_texcoord1(GLContext *c, GLVertex *v)
{
  const GLTexUnit *u = &c->tu1;
  V4 tc = u->cur_tc;

  if (c->tu1_apply & 2) {
    V4 eye;
    V3 en = { { 0.0f, 0.0f, 1.0f } };
    if (u->texgen_eye_needed) {
      if (c->lighting_enabled) {
        eye = v->ec;
        gl_xf_eye_normal(c, &c->matrix_model_view_inv, &en);
      } else {
        gl_xf_eye_coords(c, &v->coord, &eye);
        gl_xf_eye_normal(c, &c->texgen_mv_inv, &en);
      }
    } else {
      eye = v->coord;
    }
    texgen_coords(u->texgen_mask, u->texgen_mode, u->texgen_obj, u->texgen_eye,
                  &v->coord, &eye, &en, &tc, &tc);
  }
  if (c->tu1_apply & 1)
    gl_M4_MulV4(&v->tex_coord1, c->matrix_stack_ptr[3], &tc);
  else
    v->tex_coord1 = tc;
}
