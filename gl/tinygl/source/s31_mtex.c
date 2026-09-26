/*
 * s31_mtex.c - GL_ARB_multitexture with two texture units (GL 1.3 2.7,
 * 2.8, 3.8.x), the state half: glActiveTexture, glClientActiveTexture,
 * glMultiTexCoord for unit 1, and the unit exchange every command that
 * acts on "the active unit" runs through. s31, MIT.
 *
 * Why an exchange. Unit 0's state is where TinyGL always kept its one
 * texture unit's: current_texture, texenv_mode, texgen_*, the texture
 * matrix stack (matrix_stack[2]), current_tex_coord ... Every single-texture
 * path - tier 1, the general path, lines, pixel paths, texgen, glGet,
 * glPushAttrib - reads those fields, and must keep reading exactly them
 * (a frame that uses no new feature runs the phase 4 code, byte for byte).
 * Unit 1's state is a GLTexUnit, GLContext.tu1, which the rasteriser and
 * the vertex path read directly. A command that selects its unit by
 * GL_ACTIVE_TEXTURE (glBindTexture, glTexEnv, glTexParameter,
 * glTexImage*, glTexGen, glEnable of a texture target or texgen, texture
 * matrix operations, and the queries) runs its ordinary code between two
 * tu_swap calls when unit 1 is active: the context fields then hold unit
 * 1's state for the command, and unit 0's again when it returns. So one
 * implementation serves both units, and outside a command the context
 * fields are always unit 0's. The hottest of those commands for a
 * multitexturing game - glBindTexture and glEnable/glDisable of
 * GL_TEXTURE_2D on unit 1, once per surface - set tu1 directly instead.
 * glClientActiveTexture selects the texture-coordinate array the same way
 * (tc_swap).
 *
 * Nothing here is on a per-vertex path except unit 1's glMultiTexCoord,
 * which stores four floats. No double.
 */
#include <stdlib.h>
#include "zgl.h"

#define SWAPV(a, b) do { __typeof__(a) t_ = (a); (a) = (b); (b) = t_; } while (0)

void tu_swap(GLContext *c)
{
  GLTexUnit *u = &c->tu1;
  int i;
  SWAPV(c->current_texture, u->tex2d);
  SWAPV(c->current_texture_1d, u->tex1d);
  SWAPV(c->tex_enables, u->enables);
  SWAPV(c->texture_2d_enabled, u->any_enabled);
  s31_cap_swap_unit(c, &u->capbits);
  SWAPV(c->texenv_mode, u->env_mode);
  SWAPV(c->texenv_color, u->env_color);
  SWAPV(c->comb, u->comb);
  SWAPV(c->texgen_mask, u->texgen_mask);
  SWAPV(c->texgen_eye_needed, u->texgen_eye_needed);
  for (i = 0; i < 4; i++) {
    SWAPV(c->texgen_mode[i], u->texgen_mode[i]);
    SWAPV(c->texgen_obj[i], u->texgen_obj[i]);
    SWAPV(c->texgen_eye[i], u->texgen_eye[i]);
    SWAPV(c->raster_tex[i], u->raster_tex[i]);
  }
  /* the texture matrix stacks: unit 1's is matrix_stack[3] */
  SWAPV(c->matrix_stack[2], c->matrix_stack[3]);
  SWAPV(c->matrix_stack_ptr[2], c->matrix_stack_ptr[3]);
  SWAPV(c->current_tex_coord, u->cur_tc);
}

/* the client state bits of the texture-coordinate arrays (arrays.c):
   unit 0's and unit 1's */
#define TEXCOORD_ARRAY 0x0008
#define TEXCOORD1_ARRAY 0x0040

void tc_swap(GLContext *c)
{
  GLTexClient *t = &c->tc1;
  int s = c->client_states;
  SWAPV(c->texcoord_array, t->array);
  SWAPV(c->texcoord_array_size, t->size);
  SWAPV(c->texcoord_array_stride, t->stride);
  SWAPV(c->texcoord_array_type, t->type);
  SWAPV(c->texcoord_array_bstride, t->bstride);
  c->client_states = (s & ~(TEXCOORD_ARRAY | TEXCOORD1_ARRAY)) |
                     (s & TEXCOORD_ARRAY ? TEXCOORD1_ARRAY : 0) |
                     (s & TEXCOORD1_ARRAY ? TEXCOORD_ARRAY : 0);
}

/* GL 1.3 table 6.17 */
static void comb_defaults(GLCombine *g)
{
  int k;
  g->rgb = g->alpha = GL_MODULATE;
  for (k = 0; k < 2; k++) {
    g->src[k][0] = GL_TEXTURE;
    g->src[k][1] = GL_PREVIOUS;
    g->src[k][2] = GL_CONSTANT;
    g->scale[k] = 1.0f;
  }
  g->op[0][0] = g->op[0][1] = GL_SRC_COLOR;
  g->op[0][2] = GL_SRC_ALPHA;
  g->op[1][0] = g->op[1][1] = g->op[1][2] = GL_SRC_ALPHA;
}

static int knob(const char *name, int def)
{
  const char *e = getenv(name);
  return e ? atoi(e) != 0 : def;
}

/* glInit, after unit 0's defaults (s31_state_init): unit 1's are the same
   GL 1.3 initial values */
void gl_mtex_init(GLContext *c)
{
  GLTexUnit *u = &c->tu1;
  int i;

  c->active_tex = c->client_tex = 0;
  c->mtex_used = 0;
  comb_defaults(&c->comb);
  comb_defaults(&u->comb);
  u->tex2d = c->current_texture;           /* the default objects (name 0) */
  u->tex1d = c->tex1d_default;
  u->enables = u->any_enabled = 0;
  u->capbits = 0;
  u->env_mode = GL_MODULATE;
  u->env_color = gl_V4_New(0, 0, 0, 0);
  u->texgen_mask = 0;
  u->texgen_eye_needed = 0;
  for (i = 0; i < 4; i++) {
    u->texgen_mode[i] = GL_EYE_LINEAR;
    u->texgen_obj[i] = gl_V4_New(i == 0, i == 1, 0, 0);
    u->texgen_eye[i] = u->texgen_obj[i];
    u->raster_tex[i] = i == 3 ? 1.0f : 0.0f;
  }
  u->cur_tc = gl_V4_New(0, 0, 0, 1);
  c->tc1.array = NULL;
  c->tc1.size = 4;
  c->tc1.stride = c->tc1.bstride = 0;
  c->tc1.type = GL_FLOAT;
  c->tu1_on = c->tu1_apply = 0;
  c->tu1_tex = NULL;
  /* S31GL_FUSED=0: every batch with texture unit 1 takes the general
     stage list (the bit-identity gate's reference arm); S31GL_MTEX=0:
     one texture unit, as before phase 5 (a board A/B arm: QuakeSpasm then
     takes its no-combiner paths) */
  c->fused_on = knob("S31GL_FUSED", 1);
  c->mtex_adv = knob("S31GL_MTEX", 1);
}

void gl_mtex_free(GLContext *c)
{
  (void)c;          /* (unit 1's texture matrix stack is matrix_stack[3]) */
}

/* ------------------------------------------------------------ ops */

void glopActiveTexture(GLContext *c, GLParam *p)
{
  int u = p[1].i;
  c->active_tex = u;
  if (u) c->mtex_used = 1;
  /* the texture matrix operations follow the active unit */
  if (c->matrix_mode >= 2) c->matrix_mode = 2 + u;
}

/* glMultiTexCoord for unit 1 (unit 0's is glTexCoord) */
void glopMultiTexCoord(GLContext *c, GLParam *p)
{
  c->tu1.cur_tc.X = p[2].f;
  c->tu1.cur_tc.Y = p[3].f;
  c->tu1.cur_tc.Z = p[4].f;
  c->tu1.cur_tc.W = p[5].f;
}

/* ------------------------------------------------------------ entry points */

int tgl_max_texture_units(void)
{
  GLContext *c = gl_get_context();
  return c && c->mtex_adv ? 2 : 1;
}

void tgl_active_texture(int texture)
{
  GLContext *c = gl_get_context();
  GLParam p[2];
  int u = texture - GL_TEXTURE0;
  if (u < 0 || u >= tgl_max_texture_units()) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (c->in_begin) {
    gl_set_error(c, GL_INVALID_OPERATION);
    return;
  }
  p[0].op = OP_ActiveTexture;
  p[1].i = u;
  gl_add_op(p);
}

/* client state: executed at once, never compiled (GL 1.3 5.4) */
void tgl_client_active_texture(int texture)
{
  GLContext *c = gl_get_context();
  int u = texture - GL_TEXTURE0;
  if (u < 0 || u >= tgl_max_texture_units()) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (u != c->client_tex) {
    c->client_tex = u;
    if (u) c->mtex_used = 1;
  }
}

void tgl_multi_tex_coord(int target, float s, float t, float r, float q)
{
  GLContext *c = gl_get_context();
  GLParam p[6];
  int u = target - GL_TEXTURE0;

  if (u == 0) {
    tgl_glTexCoord4f(s, t, r, q);
    return;
  }
  if (u < 0 || u > 31) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (u >= tgl_max_texture_units()) return;   /* no such unit: nothing to set */
  if (!(c->compile_flag | c->print_flag)) {
    /* executing (glBegin/glEnd included): the op's stores, here */
    c->tu1.cur_tc.X = s;
    c->tu1.cur_tc.Y = t;
    c->tu1.cur_tc.Z = r;
    c->tu1.cur_tc.W = q;
    return;
  }
  p[0].op = OP_MultiTexCoord;
  p[1].i = u;
  p[2].f = s; p[3].f = t; p[4].f = r; p[5].f = q;
  gl_add_op(p);
}
