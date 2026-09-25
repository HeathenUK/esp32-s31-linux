/*
 * s31_state.c - fixed-function state TinyGL did not track. s31, MIT.
 *
 * Everything here is RECORDED exactly as GL defines it, so glGet* and
 * glIsEnabled give honest answers and display lists capture it. Whether
 * the rasteriser HONOURS it is a separate question, answered per item in
 * gl/tinygl/README.s31; anything set to a non-default value that is not
 * honoured prints "libGL: unimplemented ..." once.
 *
 * Adding behaviour later (plan F5/F6) means reading these fields at
 * triangle setup and selecting a filler - never an if per pixel.
 */
#include "zgl.h"

/* Every capability glEnable/glDisable/glIsEnabled accepts in GL 1.3 (no
   ARB_imaging). native = TinyGL has its own field for it. */
static const struct cap {
  int cap;
  unsigned char native, warn, dflt;
} caps[] = {
  { GL_ALPHA_TEST, 0, 1, 0 },
  { GL_AUTO_NORMAL, 0, 1, 0 },
  { GL_BLEND, 0, 1, 0 },
  { GL_CLIP_PLANE0, 0, 1, 0 }, { GL_CLIP_PLANE1, 0, 1, 0 },
  { GL_CLIP_PLANE2, 0, 1, 0 }, { GL_CLIP_PLANE3, 0, 1, 0 },
  { GL_CLIP_PLANE4, 0, 1, 0 }, { GL_CLIP_PLANE5, 0, 1, 0 },
  { GL_COLOR_LOGIC_OP, 0, 1, 0 },
  { GL_LOGIC_OP, 0, 0, 0 },           /* = GL_INDEX_LOGIC_OP; no colour-index mode */
  { GL_COLOR_MATERIAL, 1, 0, 0 },
  { GL_CULL_FACE, 1, 0, 0 },
  { GL_DEPTH_TEST, 1, 0, 0 },
  { GL_DITHER, 0, 0, 1 },             /* dithering is implementation-defined */
  { GL_FOG, 0, 1, 0 },
  { GL_LIGHTING, 1, 0, 0 },
  { GL_LINE_SMOOTH, 0, 1, 0 },
  { GL_LINE_STIPPLE, 0, 1, 0 },
  { GL_MAP1_COLOR_4, 0, 1, 0 }, { GL_MAP1_INDEX, 0, 1, 0 },
  { GL_MAP1_NORMAL, 0, 1, 0 }, { GL_MAP1_TEXTURE_COORD_1, 0, 1, 0 },
  { GL_MAP1_TEXTURE_COORD_2, 0, 1, 0 }, { GL_MAP1_TEXTURE_COORD_3, 0, 1, 0 },
  { GL_MAP1_TEXTURE_COORD_4, 0, 1, 0 }, { GL_MAP1_VERTEX_3, 0, 1, 0 },
  { GL_MAP1_VERTEX_4, 0, 1, 0 },
  { GL_MAP2_COLOR_4, 0, 1, 0 }, { GL_MAP2_INDEX, 0, 1, 0 },
  { GL_MAP2_NORMAL, 0, 1, 0 }, { GL_MAP2_TEXTURE_COORD_1, 0, 1, 0 },
  { GL_MAP2_TEXTURE_COORD_2, 0, 1, 0 }, { GL_MAP2_TEXTURE_COORD_3, 0, 1, 0 },
  { GL_MAP2_TEXTURE_COORD_4, 0, 1, 0 }, { GL_MAP2_VERTEX_3, 0, 1, 0 },
  { GL_MAP2_VERTEX_4, 0, 1, 0 },
  { GL_NORMALIZE, 1, 0, 0 },
  { GL_POINT_SMOOTH, 0, 1, 0 },
  { GL_POLYGON_OFFSET_FILL, 1, 0, 0 },
  { GL_POLYGON_OFFSET_LINE, 1, 0, 0 },
  { GL_POLYGON_OFFSET_POINT, 1, 0, 0 },
  { GL_POLYGON_SMOOTH, 0, 1, 0 },
  { GL_POLYGON_STIPPLE, 0, 1, 0 },
  { GL_SCISSOR_TEST, 0, 1, 0 },
  { GL_STENCIL_TEST, 0, 0, 0 },       /* no stencil buffer: the test passes */
  { GL_TEXTURE_1D, 0, 1, 0 },
  { GL_TEXTURE_2D, 1, 0, 0 },
  { GL_TEXTURE_3D, 0, 1, 0 },
  { GL_TEXTURE_CUBE_MAP, 0, 1, 0 },
  { GL_TEXTURE_GEN_Q, 0, 1, 0 }, { GL_TEXTURE_GEN_R, 0, 1, 0 },
  { GL_TEXTURE_GEN_S, 0, 1, 0 }, { GL_TEXTURE_GEN_T, 0, 1, 0 },
  { GL_RESCALE_NORMAL, 0, 1, 0 },
  { GL_MULTISAMPLE, 0, 0, 1 },        /* no sample buffers: no effect */
  { GL_SAMPLE_ALPHA_TO_COVERAGE, 0, 0, 0 },
  { GL_SAMPLE_ALPHA_TO_ONE, 0, 0, 0 },
  { GL_SAMPLE_COVERAGE, 0, 0, 0 },
  { GL_COLOR_SUM, 0, 1, 0 },          /* EXT_secondary_color; harmless */
};
#define NCAPS ((int)(sizeof(caps) / sizeof(caps[0])))

int s31_cap_index(int cap)
{
  int i;
  if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS) return NCAPS;
  for (i = 0; i < NCAPS; i++)
    if (caps[i].cap == cap) return i;
  return -1;
}

static void cap_set(GLContext *c, int i, int v)
{
  /* NCAPS <= 64 */
  if (i < 32) {
    if (v) c->caps |= 1u << i; else c->caps &= ~(1u << i);
  } else {
    if (v) c->caps_hi |= 1u << (i - 32); else c->caps_hi &= ~(1u << (i - 32));
  }
}

static int cap_bit(GLContext *c, int i)
{
  return i < 32 ? (c->caps >> i) & 1 : (c->caps_hi >> (i - 32)) & 1;
}

static const char *cap_name(int cap)
{
  switch (cap) {
  case GL_ALPHA_TEST: return "glEnable(GL_ALPHA_TEST)";
  case GL_AUTO_NORMAL: return "glEnable(GL_AUTO_NORMAL)";
  case GL_BLEND: return "glEnable(GL_BLEND)";
  case GL_COLOR_LOGIC_OP: return "glEnable(GL_COLOR_LOGIC_OP)";
  case GL_FOG: return "glEnable(GL_FOG)";
  case GL_LINE_SMOOTH: return "glEnable(GL_LINE_SMOOTH)";
  case GL_LINE_STIPPLE: return "glEnable(GL_LINE_STIPPLE)";
  case GL_POINT_SMOOTH: return "glEnable(GL_POINT_SMOOTH)";
  case GL_POLYGON_SMOOTH: return "glEnable(GL_POLYGON_SMOOTH)";
  case GL_POLYGON_STIPPLE: return "glEnable(GL_POLYGON_STIPPLE)";
  case GL_SCISSOR_TEST: return "glEnable(GL_SCISSOR_TEST)";
  case GL_TEXTURE_1D: return "glEnable(GL_TEXTURE_1D)";
  case GL_TEXTURE_3D: return "glEnable(GL_TEXTURE_3D)";
  case GL_TEXTURE_CUBE_MAP: return "glEnable(GL_TEXTURE_CUBE_MAP)";
  case GL_RESCALE_NORMAL: return "glEnable(GL_RESCALE_NORMAL)";
  case GL_COLOR_SUM: return "glEnable(GL_COLOR_SUM)";
  default:
    if (cap >= GL_CLIP_PLANE0 && cap <= GL_CLIP_PLANE5)
      return "glEnable(GL_CLIP_PLANEi)";
    if (cap >= GL_TEXTURE_GEN_S && cap <= GL_TEXTURE_GEN_Q)
      return "glEnable(GL_TEXTURE_GEN_*)";
    return "glEnable(evaluator map)";
  }
}

void s31_cap_record(GLContext *c, int cap, int v)
{
  int i = s31_cap_index(cap);
  if (i < 0 || i >= NCAPS) return;          /* lights are native */
  cap_set(c, i, v);
  if (v && caps[i].warn) {
    /* per glEnable: one bit per capability, so a warned one never scans
       the warn list again (NCAPS <= 64) */
    static unsigned int warned[2];
    unsigned int bit = 1u << (i & 31);
    if (!(warned[i >> 5] & bit)) {
      warned[i >> 5] |= bit;
      gl_warn_once(cap_name(cap));
    }
  }
}

/* 0/1, or -1 when cap is not a capability */
int s31_cap_get(GLContext *c, int cap)
{
  int i = s31_cap_index(cap);
  if (i < 0) return s31_client_state(c, cap);  /* glIsEnabled(GL_*_ARRAY) */
  if (i == NCAPS) return c->lights[cap - GL_LIGHT0].enabled != 0;
  switch (cap) {
  case GL_COLOR_MATERIAL: return c->color_material_enabled != 0;
  case GL_CULL_FACE: return c->cull_face_enabled != 0;
  case GL_DEPTH_TEST: return c->depth_test != 0;
  case GL_LIGHTING: return c->lighting_enabled != 0;
  case GL_NORMALIZE: return c->normalize_enabled != 0;
  case GL_TEXTURE_2D: return c->texture_2d_enabled != 0;
  case GL_POLYGON_OFFSET_FILL: return (c->offset_states & TGL_OFFSET_FILL) != 0;
  case GL_POLYGON_OFFSET_LINE: return (c->offset_states & TGL_OFFSET_LINE) != 0;
  case GL_POLYGON_OFFSET_POINT: return (c->offset_states & TGL_OFFSET_POINT) != 0;
  default: return cap_bit(c, i);
  }
}

int tgl_is_enabled(int cap)
{
  GLContext *c = gl_get_context();
  return s31_cap_get(c, cap);
}

/* GL 1.3 initial values (glspec13 table 6.x) */
void s31_state_init(GLContext *c)
{
  int i;
  c->error = 0;
  c->caps = 0; c->caps_hi = 0;
  for (i = 0; i < NCAPS; i++)
    if (caps[i].dflt) cap_set(c, i, 1);
  c->depth_func = GL_LESS;
  c->depth_mask = 1;
  c->depth_range[0] = 0.0f; c->depth_range[1] = 1.0f;
  c->blend_src = GL_ONE; c->blend_dst = GL_ZERO;
  for (i = 0; i < 4; i++) c->blend_color[i] = 0.0f;
  c->alpha_func = GL_ALWAYS; c->alpha_ref = 0.0f;
  for (i = 0; i < 4; i++) c->color_mask[i] = 1;
  c->line_width = 1.0f; c->point_size = 1.0f;
  c->line_stipple_factor = 1; c->line_stipple_pattern = 0xffff;
  c->fog_mode = GL_EXP; c->fog_density = 1.0f;
  c->fog_start = 0.0f; c->fog_end = 1.0f; c->fog_index = 0.0f;
  c->fog_color = gl_V4_New(0, 0, 0, 0);
  c->texenv_mode = GL_MODULATE;
  c->texenv_color = gl_V4_New(0, 0, 0, 0);
  c->logic_op = GL_COPY;
  c->stencil_func = GL_ALWAYS; c->stencil_ref = 0;
  c->stencil_value_mask = -1; c->stencil_writemask = -1;
  c->stencil_fail = c->stencil_zfail = c->stencil_zpass = GL_KEEP;
  c->stencil_clear = 0;
  c->hint_perspective = c->hint_point = c->hint_line = GL_DONT_CARE;
  c->hint_polygon = c->hint_fog = GL_DONT_CARE;
  c->doublebuffer = 1;
  c->draw_buffer = GL_BACK; c->read_buffer = GL_BACK;
  c->unpack_alignment = 4; c->pack_alignment = 4;
  c->list_base = 0; c->list_index = 0; c->list_mode = 0;
  c->vertex_array_type = c->color_array_type = GL_FLOAT;
  c->normal_array_type = c->texcoord_array_type = GL_FLOAT;
  c->vertex_array_size = 4; c->color_array_size = 4;
  c->texcoord_array_size = 4;
  c->raster_pos[0] = c->raster_pos[1] = c->raster_pos[2] = 0.0f;
  c->raster_pos[3] = 1.0f;
  c->raster_valid = 1;
  c->proj_used = c->matrix_stack_ptr[1];
}

static int valid_func(int f)
{
  return f >= GL_NEVER && f <= GL_ALWAYS;
}

static int valid_blend(int f, int is_src)
{
  switch (f) {
  case GL_ZERO: case GL_ONE:
  case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
  case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
    return 1;
  case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR:
    return !is_src;
  case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
    return is_src;
  case GL_SRC_ALPHA_SATURATE:
    return is_src;
  default:
    return 0;
  }
}

/* validation happens here, at call time, so errors are reported where GL
   reports them; the op below only stores */
static int validate(GLContext *c, int code, const GLParam *p)
{
  switch (code) {
  case S31_ST_DEPTH_FUNC:
  case S31_ST_ALPHA_FUNC:
    return valid_func(p[2].i) ? 0 : GL_INVALID_ENUM;
  case S31_ST_BLEND_FUNC:
    return valid_blend(p[2].i, 1) && valid_blend(p[3].i, 0) ? 0 : GL_INVALID_ENUM;
  case S31_ST_SCISSOR:
    return (p[4].i < 0 || p[5].i < 0) ? GL_INVALID_VALUE : 0;
  case S31_ST_LINE_WIDTH:
  case S31_ST_POINT_SIZE:
    return p[2].f <= 0.0f ? GL_INVALID_VALUE : 0;
  case S31_ST_FOG_MODE:
    return (p[2].i == GL_LINEAR || p[2].i == GL_EXP || p[2].i == GL_EXP2)
           ? 0 : GL_INVALID_ENUM;
  case S31_ST_FOG_DENSITY:
    return p[2].f < 0.0f ? GL_INVALID_VALUE : 0;
  case S31_ST_STENCIL_FUNC:
    return valid_func(p[2].i) ? 0 : GL_INVALID_ENUM;
  case S31_ST_LOGIC_OP:
    return (p[2].i >= GL_CLEAR && p[2].i <= GL_SET) ? 0 : GL_INVALID_ENUM;
  default:
    return 0;
  }
}

static float clampf01(float v)
{
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void glopState(GLContext *c, GLParam *p)
{
  int i;
  switch (p[1].i) {
  case S31_ST_DEPTH_FUNC:
    c->depth_func = p[2].i;
    /* TinyGL tests zz >= zpix with Z inverted, which is GL_LESS/GL_LEQUAL */
    if (p[2].i != GL_LESS && p[2].i != GL_LEQUAL)
      gl_warn_once("glDepthFunc(other than GL_LESS/GL_LEQUAL)");
    break;
  case S31_ST_DEPTH_MASK:
    c->depth_mask = p[2].i != 0;
    /* honoured by the triangle fillers (clip.c); lines still write Z */
    break;
  case S31_ST_DEPTH_RANGE:
    c->depth_range[0] = clampf01(p[2].f);
    c->depth_range[1] = clampf01(p[3].f);
    if (c->depth_range[0] != 0.0f || c->depth_range[1] != 1.0f)
      gl_warn_once("glDepthRange(other than 0,1)");
    break;
  case S31_ST_BLEND_FUNC:
    c->blend_src = p[2].i; c->blend_dst = p[3].i;
    break;
  case S31_ST_BLEND_COLOR:
    for (i = 0; i < 4; i++) c->blend_color[i] = clampf01(p[2 + i].f);
    break;
  case S31_ST_ALPHA_FUNC:
    c->alpha_func = p[2].i; c->alpha_ref = clampf01(p[3].f);
    break;
  case S31_ST_COLOR_MASK:
    for (i = 0; i < 4; i++) c->color_mask[i] = p[2 + i].i != 0;
    if (!c->color_mask[0] || !c->color_mask[1] || !c->color_mask[2])
      gl_warn_once("glColorMask(GL_FALSE on a colour channel)");
    break;
  case S31_ST_SCISSOR:
    for (i = 0; i < 4; i++) c->scissor[i] = p[2 + i].i;
    break;
  case S31_ST_LINE_WIDTH:
    c->line_width = p[2].f;
    if (p[2].f != 1.0f) gl_warn_once("glLineWidth(other than 1)");
    break;
  case S31_ST_POINT_SIZE:
    c->point_size = p[2].f;
    if (p[2].f != 1.0f) gl_warn_once("glPointSize(other than 1)");
    break;
  case S31_ST_LINE_STIPPLE:
    c->line_stipple_factor = p[2].i < 1 ? 1 : (p[2].i > 256 ? 256 : p[2].i);
    c->line_stipple_pattern = p[3].i & 0xffff;
    break;
  case S31_ST_FOG_MODE: c->fog_mode = p[2].i; break;
  case S31_ST_FOG_DENSITY: c->fog_density = p[2].f; break;
  case S31_ST_FOG_START: c->fog_start = p[2].f; break;
  case S31_ST_FOG_END: c->fog_end = p[2].f; break;
  case S31_ST_FOG_INDEX: c->fog_index = p[2].f; break;
  case S31_ST_FOG_COLOR:
    for (i = 0; i < 4; i++) c->fog_color.v[i] = clampf01(p[2 + i].f);
    break;
  case S31_ST_LOGIC_OP: c->logic_op = p[2].i; break;
  case S31_ST_STENCIL_FUNC:
    c->stencil_func = p[2].i; c->stencil_ref = p[3].i;
    c->stencil_value_mask = p[4].i;
    break;
  case S31_ST_STENCIL_OP:
    c->stencil_fail = p[2].i; c->stencil_zfail = p[3].i;
    c->stencil_zpass = p[4].i;
    break;
  case S31_ST_STENCIL_MASK: c->stencil_writemask = p[2].i; break;
  case S31_ST_STENCIL_CLEAR: c->stencil_clear = p[2].i; break;
  case S31_ST_DRAW_BUFFER: c->draw_buffer = p[2].i; break;
  case S31_ST_READ_BUFFER: c->read_buffer = p[2].i; break;
  case S31_ST_TEXENV_COLOR:
    for (i = 0; i < 4; i++) c->texenv_color.v[i] = clampf01(p[2 + i].f);
    break;
  case S31_ST_LIST_BASE: c->list_base = p[2].i; break;
  default: break;
  }
}

static void state_op(GLParam *p)
{
  GLContext *c = gl_get_context();
  int e = validate(c, p[1].i, p);
  if (e) { gl_set_error(c, e); return; }
  if (c->in_begin) { gl_set_error(c, GL_INVALID_OPERATION); return; }
  gl_add_op(p);
}

void tgl_state_i(int code, int a, int b, int cc, int d)
{
  GLParam p[6];
  p[0].op = OP_State;
  p[1].i = code;
  p[2].i = a; p[3].i = b; p[4].i = cc; p[5].i = d;
  state_op(p);
}

void tgl_state_f(int code, float a, float b, float cc, float d)
{
  GLParam p[6];
  p[0].op = OP_State;
  p[1].i = code;
  p[2].f = a; p[3].f = b; p[4].f = cc; p[5].f = d;
  state_op(p);
}

void tgl_state_alpha(int func, float ref)
{
  GLParam p[6];
  p[0].op = OP_State;
  p[1].i = S31_ST_ALPHA_FUNC;
  p[2].i = func; p[3].f = ref; p[4].i = 0; p[5].i = 0;
  state_op(p);
}

int tgl_get_error(void)
{
  GLContext *c = gl_get_context();
  int e;
  if (c == NULL) return 0;
  e = c->error;
  c->error = 0;
  return e;
}

void tgl_set_error(int e)
{
  GLContext *c = gl_get_context();
  if (c != NULL) gl_set_error(c, e);
}

int tgl_in_begin(void)
{
  GLContext *c = gl_get_context();
  return c != NULL && c->in_begin;
}

int tgl_compiling(void)
{
  GLContext *c = gl_get_context();
  return c != NULL && c->compile_flag;
}
