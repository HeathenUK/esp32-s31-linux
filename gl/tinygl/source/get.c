#include "zgl.h"

/*
 * glGet* (s31 rewrite; TinyGL answered six names and exited on the rest).
 *
 * tgl_get() returns every GL 1.3 state value this library holds, plus the
 * implementation limits, honestly: the limits are this rasteriser's (RGB565
 * colour, 16-bit depth, an 8-bit stencil only for a context with stencil
 * bits (phase 4), no accum/alpha planes, two texture units (phase 5), 256x256
 * textures). Every capability
 * glIsEnabled knows is also a valid glGet name. gl/api/get.c converts to
 * the four glGet*v types.
 */

#define I1(v) do { iv[0] = (v); n = 1; } while (0)
#define F1(v) do { fv[0] = (v); *kind = TGL_GET_FLOAT; n = 1; } while (0)

static int matrix(const M4 *m, float *fv, int transpose)
{
  int i, j;
  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      fv[i * 4 + j] = transpose ? m->m[i][j] : m->m[j][i];
  return 16;
}

static int get(GLContext *c, int pname, int *iv, float *fv, int *kind);

/* phase 5 O1: the active unit's state (bindings, texture matrix, current
   and raster texcoords, texture enables) and the client-active unit's
   texture-coordinate array, through the unit exchange (s31_mtex.c) */
int tgl_get(int pname, int *iv, float *fv, int *kind)
{
  GLContext *c = gl_get_context();
  int n;
  if (c->active_tex) tu_swap(c);
  if (c->client_tex) tc_swap(c);
  n = get(c, pname, iv, fv, kind);
  if (c->client_tex) tc_swap(c);
  if (c->active_tex) tu_swap(c);
  return n;
}

static int get(GLContext *c, int pname, int *iv, float *fv, int *kind)
{
  int n = -1, i;

  *kind = TGL_GET_INT;
  switch (pname) {
  /* current values */
  case GL_CURRENT_COLOR:
    for (i = 0; i < 4; i++) fv[i] = c->current_color.v[i];
    *kind = TGL_GET_COLOR; n = 4; break;
  case GL_CURRENT_INDEX: I1(1); break;
  case GL_CURRENT_TEXTURE_COORDS:
    for (i = 0; i < 4; i++) fv[i] = c->current_tex_coord.v[i];
    *kind = TGL_GET_FLOAT; n = 4; break;
  case GL_CURRENT_NORMAL:
    for (i = 0; i < 3; i++) fv[i] = c->current_normal.v[i];
    *kind = TGL_GET_COLOR; n = 3; break;
  case GL_CURRENT_RASTER_POSITION:
    for (i = 0; i < 4; i++) fv[i] = c->raster_pos[i];
    *kind = TGL_GET_FLOAT; n = 4; break;
  /* s31: the raster position of glRasterPos / glWindowPos (s31_xform.c) */
  case GL_CURRENT_RASTER_COLOR:
    for (i = 0; i < 4; i++) fv[i] = c->raster_color[i];
    *kind = TGL_GET_COLOR; n = 4; break;
  case GL_CURRENT_RASTER_DISTANCE: F1(c->raster_distance); break;
  case GL_CURRENT_RASTER_INDEX: I1(1); break;
  case GL_CURRENT_RASTER_TEXTURE_COORDS:
    for (i = 0; i < 4; i++) fv[i] = c->raster_tex[i];
    *kind = TGL_GET_FLOAT; n = 4; break;
  case GL_CURRENT_RASTER_POSITION_VALID: I1(c->raster_valid); break;
  case GL_EDGE_FLAG: I1(c->current_edge_flag != 0); break;

  /* vertex arrays */
  case GL_VERTEX_ARRAY: case GL_NORMAL_ARRAY: case GL_COLOR_ARRAY:
  case GL_INDEX_ARRAY: case GL_TEXTURE_COORD_ARRAY: case GL_EDGE_FLAG_ARRAY:
    I1(s31_client_state(c, pname)); break;
  case GL_VERTEX_ARRAY_SIZE: I1(c->vertex_array_size); break;
  case GL_VERTEX_ARRAY_TYPE: I1(c->vertex_array_type); break;
  case GL_VERTEX_ARRAY_STRIDE: I1(c->vertex_array_stride); break;
  case GL_NORMAL_ARRAY_TYPE: I1(c->normal_array_type); break;
  case GL_NORMAL_ARRAY_STRIDE: I1(c->normal_array_stride); break;
  case GL_COLOR_ARRAY_SIZE: I1(c->color_array_size); break;
  case GL_COLOR_ARRAY_TYPE: I1(c->color_array_type); break;
  case GL_COLOR_ARRAY_STRIDE: I1(c->color_array_stride); break;
  case GL_TEXTURE_COORD_ARRAY_SIZE: I1(c->texcoord_array_size); break;
  case GL_TEXTURE_COORD_ARRAY_TYPE: I1(c->texcoord_array_type); break;
  case GL_TEXTURE_COORD_ARRAY_STRIDE: I1(c->texcoord_array_stride); break;
  case GL_EDGE_FLAG_ARRAY_STRIDE: I1(c->edge_flag_array_stride); break;
  case GL_INDEX_ARRAY_TYPE: I1(GL_FLOAT); break;
  case GL_INDEX_ARRAY_STRIDE: I1(0); break;
  case GL_CLIENT_ACTIVE_TEXTURE: I1(GL_TEXTURE0 + c->client_tex); break;
  case GL_ARRAY_BUFFER_BINDING: case GL_ELEMENT_ARRAY_BUFFER_BINDING:
    I1(0); break;

  /* transformation */
  case GL_MODELVIEW_MATRIX:
    n = matrix(c->matrix_stack_ptr[0], fv, 0); *kind = TGL_GET_FLOAT; break;
  case GL_PROJECTION_MATRIX:
    n = matrix(c->matrix_stack_ptr[1], fv, 0); *kind = TGL_GET_FLOAT; break;
  case GL_TEXTURE_MATRIX:
    n = matrix(c->matrix_stack_ptr[2], fv, 0); *kind = TGL_GET_FLOAT; break;
  case GL_TRANSPOSE_MODELVIEW_MATRIX:
    n = matrix(c->matrix_stack_ptr[0], fv, 1); *kind = TGL_GET_FLOAT; break;
  case GL_TRANSPOSE_PROJECTION_MATRIX:
    n = matrix(c->matrix_stack_ptr[1], fv, 1); *kind = TGL_GET_FLOAT; break;
  case GL_TRANSPOSE_TEXTURE_MATRIX:
    n = matrix(c->matrix_stack_ptr[2], fv, 1); *kind = TGL_GET_FLOAT; break;
  case GL_VIEWPORT:
    iv[0] = c->viewport.xmin; iv[1] = c->viewport.ymin;
    iv[2] = c->viewport.xsize; iv[3] = c->viewport.ysize;
    n = 4; break;
  case GL_DEPTH_RANGE:
    fv[0] = c->depth_range[0]; fv[1] = c->depth_range[1];
    *kind = TGL_GET_COLOR; n = 2; break;
  case GL_MODELVIEW_STACK_DEPTH:
    I1((int)(c->matrix_stack_ptr[0] - c->matrix_stack[0]) + 1); break;
  case GL_PROJECTION_STACK_DEPTH:
    I1((int)(c->matrix_stack_ptr[1] - c->matrix_stack[1]) + 1); break;
  case GL_TEXTURE_STACK_DEPTH:
    I1((int)(c->matrix_stack_ptr[2] - c->matrix_stack[2]) + 1); break;
  case GL_MATRIX_MODE:
    I1(c->matrix_mode == 0 ? GL_MODELVIEW :
       c->matrix_mode == 1 ? GL_PROJECTION : GL_TEXTURE);   /* 2, 3: GL_TEXTURE */
    break;

  /* colouring */
  case GL_FOG_COLOR:
    for (i = 0; i < 4; i++) fv[i] = c->fog_color.v[i];
    *kind = TGL_GET_COLOR; n = 4; break;
  case GL_FOG_INDEX: F1(c->fog_index); break;
  case GL_FOG_DENSITY: F1(c->fog_density); break;
  case GL_FOG_START: F1(c->fog_start); break;
  case GL_FOG_END: F1(c->fog_end); break;
  case GL_FOG_MODE: I1(c->fog_mode); break;
  case GL_SHADE_MODEL: I1(c->current_shade_model); break;

  /* lighting */
  case GL_COLOR_MATERIAL_PARAMETER: I1(c->current_color_material_type); break;
  case GL_COLOR_MATERIAL_FACE: I1(c->current_color_material_mode); break;
  case GL_LIGHT_MODEL_AMBIENT:
    for (i = 0; i < 4; i++) fv[i] = c->ambient_light_model.v[i];
    *kind = TGL_GET_COLOR; n = 4; break;
  case GL_LIGHT_MODEL_LOCAL_VIEWER: I1(c->local_light_model != 0); break;
  case GL_LIGHT_MODEL_TWO_SIDE: I1(c->light_model_two_side != 0); break;
  case GL_LIGHT_MODEL_COLOR_CONTROL: I1(c->color_control); break;

  /* rasterisation */
  case GL_POINT_SIZE: F1(c->point_size); break;
  case GL_LINE_WIDTH: F1(c->line_width); break;
  case GL_LINE_STIPPLE_PATTERN: I1(c->line_stipple_pattern); break;
  case GL_LINE_STIPPLE_REPEAT: I1(c->line_stipple_factor); break;
  case GL_CULL_FACE_MODE: I1(c->current_cull_face); break;
  case GL_FRONT_FACE: I1(c->current_front_face ? GL_CW : GL_CCW); break;
  case GL_POLYGON_MODE:
    iv[0] = c->polygon_mode_front; iv[1] = c->polygon_mode_back; n = 2; break;
  case GL_POLYGON_OFFSET_FACTOR: F1(c->offset_factor); break;
  case GL_POLYGON_OFFSET_UNITS: F1(c->offset_units); break;

  /* texturing */
  case GL_TEXTURE_BINDING_2D:
    I1(c->current_texture ? c->current_texture->handle : 0); break;
  case GL_TEXTURE_BINDING_1D:
    I1(c->current_texture_1d ? c->current_texture_1d->handle : 0); break;
  case GL_TEXTURE_BINDING_3D: I1(0); break;
  case GL_ACTIVE_TEXTURE: I1(GL_TEXTURE0 + c->active_tex); break;

  /* pixel operations */
  case GL_SCISSOR_BOX:
    for (i = 0; i < 4; i++) iv[i] = c->scissor[i];
    n = 4; break;
  case GL_ALPHA_TEST_FUNC: I1(c->alpha_func); break;
  case GL_ALPHA_TEST_REF:
    fv[0] = c->alpha_ref; *kind = TGL_GET_COLOR; n = 1; break;
  case GL_STENCIL_FUNC: I1(c->stencil_func); break;
  case GL_STENCIL_VALUE_MASK: I1(c->stencil_value_mask); break;
  case GL_STENCIL_REF: I1(c->stencil_ref); break;
  case GL_STENCIL_FAIL: I1(c->stencil_fail); break;
  case GL_STENCIL_PASS_DEPTH_FAIL: I1(c->stencil_zfail); break;
  case GL_STENCIL_PASS_DEPTH_PASS: I1(c->stencil_zpass); break;
  case GL_DEPTH_FUNC: I1(c->depth_func); break;
  case GL_BLEND_SRC: I1(c->blend_src); break;
  case GL_BLEND_DST: I1(c->blend_dst); break;
  /* phase 4 BLEND-EQ (GL_BLEND_EQUATION is GL_BLEND_EQUATION_RGB) */
  case GL_BLEND_EQUATION: I1(c->blend_eq); break;
  case GL_BLEND_EQUATION_ALPHA: I1(c->blend_eq_a); break;
  case GL_BLEND_SRC_RGB: I1(c->blend_src); break;
  case GL_BLEND_DST_RGB: I1(c->blend_dst); break;
  case GL_BLEND_SRC_ALPHA: I1(c->blend_src_a); break;
  case GL_BLEND_DST_ALPHA: I1(c->blend_dst_a); break;
  case GL_BLEND_COLOR:
    for (i = 0; i < 4; i++) fv[i] = c->blend_color[i];
    *kind = TGL_GET_COLOR; n = 4; break;
  case GL_LOGIC_OP_MODE: I1(c->logic_op); break;

  /* framebuffer control */
  case GL_DRAW_BUFFER: I1(c->draw_buffer); break;
  case GL_READ_BUFFER: I1(c->read_buffer); break;
  case GL_INDEX_WRITEMASK: I1(-1); break;
  case GL_COLOR_WRITEMASK:
    for (i = 0; i < 4; i++) iv[i] = c->color_mask[i];
    n = 4; break;
  case GL_DEPTH_WRITEMASK: I1(c->depth_mask); break;
  case GL_STENCIL_WRITEMASK: I1(c->stencil_writemask); break;
  case GL_COLOR_CLEAR_VALUE:
    for (i = 0; i < 4; i++) fv[i] = c->clear_color.v[i];
    *kind = TGL_GET_COLOR; n = 4; break;
  case GL_INDEX_CLEAR_VALUE: F1(0.0f); break;
  case GL_DEPTH_CLEAR_VALUE:
    fv[0] = c->clear_depth; *kind = TGL_GET_COLOR; n = 1; break;
  case GL_STENCIL_CLEAR_VALUE: I1(c->stencil_clear); break;
  case GL_ACCUM_CLEAR_VALUE:
    fv[0] = fv[1] = fv[2] = fv[3] = 0.0f; *kind = TGL_GET_COLOR; n = 4; break;

  /* pixel storage and transfer */
  case GL_UNPACK_SWAP_BYTES: I1(c->unpack_swap); break;
  case GL_UNPACK_LSB_FIRST: I1(c->unpack_lsb); break;
  case GL_UNPACK_ROW_LENGTH: I1(c->unpack_row_length); break;
  case GL_UNPACK_SKIP_ROWS: I1(c->unpack_skip_rows); break;
  case GL_UNPACK_SKIP_PIXELS: I1(c->unpack_skip_pixels); break;
  case GL_UNPACK_ALIGNMENT: I1(c->unpack_alignment); break;
  case GL_UNPACK_IMAGE_HEIGHT: I1(c->unpack_image_height); break;
  case GL_UNPACK_SKIP_IMAGES: I1(c->unpack_skip_images); break;
  case GL_PACK_SWAP_BYTES: I1(c->pack_swap); break;
  case GL_PACK_LSB_FIRST: I1(c->pack_lsb); break;
  case GL_PACK_ROW_LENGTH: I1(c->pack_row_length); break;
  case GL_PACK_SKIP_ROWS: I1(c->pack_skip_rows); break;
  case GL_PACK_SKIP_PIXELS: I1(c->pack_skip_pixels); break;
  case GL_PACK_ALIGNMENT: I1(c->pack_alignment); break;
  case GL_PACK_IMAGE_HEIGHT: I1(c->pack_image_height); break;
  case GL_PACK_SKIP_IMAGES: I1(c->pack_skip_images); break;
  /* s31: glPixelTransfer / glPixelZoom (s31_state.c) */
  case GL_MAP_COLOR: I1(c->map_color); break;
  case GL_MAP_STENCIL: I1(c->map_stencil); break;
  case GL_INDEX_SHIFT: I1(c->index_shift); break;
  case GL_INDEX_OFFSET: I1(c->index_offset); break;
  case GL_RED_SCALE: case GL_GREEN_SCALE: case GL_BLUE_SCALE:
  case GL_ALPHA_SCALE: case GL_DEPTH_SCALE: case GL_ZOOM_X: case GL_ZOOM_Y:
  case GL_RED_BIAS: case GL_GREEN_BIAS: case GL_BLUE_BIAS:
  case GL_ALPHA_BIAS: case GL_DEPTH_BIAS:
    tgl_pixel_transfer_get(pname, fv); *kind = TGL_GET_FLOAT; n = 1; break;
  case GL_PIXEL_MAP_I_TO_I_SIZE: case GL_PIXEL_MAP_S_TO_S_SIZE:
  case GL_PIXEL_MAP_I_TO_R_SIZE: case GL_PIXEL_MAP_I_TO_G_SIZE:
  case GL_PIXEL_MAP_I_TO_B_SIZE: case GL_PIXEL_MAP_I_TO_A_SIZE:
  case GL_PIXEL_MAP_R_TO_R_SIZE: case GL_PIXEL_MAP_G_TO_G_SIZE:
  case GL_PIXEL_MAP_B_TO_B_SIZE: case GL_PIXEL_MAP_A_TO_A_SIZE:
    I1(1); break;

  /* evaluators (not implemented; initial values) */
  case GL_MAP1_GRID_DOMAIN: fv[0] = 0.0f; fv[1] = 1.0f;
    *kind = TGL_GET_FLOAT; n = 2; break;
  case GL_MAP2_GRID_DOMAIN: fv[0] = 0.0f; fv[1] = 1.0f; fv[2] = 0.0f;
    fv[3] = 1.0f; *kind = TGL_GET_FLOAT; n = 4; break;
  case GL_MAP1_GRID_SEGMENTS: I1(1); break;
  case GL_MAP2_GRID_SEGMENTS: iv[0] = iv[1] = 1; n = 2; break;

  /* hints */
  case GL_PERSPECTIVE_CORRECTION_HINT: I1(c->hint_perspective); break;
  case GL_POINT_SMOOTH_HINT: I1(c->hint_point); break;
  case GL_LINE_SMOOTH_HINT: I1(c->hint_line); break;
  case GL_POLYGON_SMOOTH_HINT: I1(c->hint_polygon); break;
  case GL_FOG_HINT: I1(c->hint_fog); break;
  case GL_TEXTURE_COMPRESSION_HINT: case GL_GENERATE_MIPMAP_HINT:
    I1(GL_DONT_CARE); break;

  /* implementation limits: what this rasteriser really does */
  case GL_MAX_LIGHTS: I1(MAX_LIGHTS); break;
  case GL_MAX_CLIP_PLANES: I1(6); break;            /* s31_xform.c, clip.c */
  case GL_MAX_MODELVIEW_STACK_DEPTH: I1(MAX_MODELVIEW_STACK_DEPTH); break;
  case GL_MAX_PROJECTION_STACK_DEPTH: I1(MAX_PROJECTION_STACK_DEPTH); break;
  case GL_MAX_TEXTURE_STACK_DEPTH: I1(MAX_TEXTURE_STACK_DEPTH); break;
  case GL_SUBPIXEL_BITS: I1(0); break;               /* integer vertices */
  case GL_MAX_TEXTURE_SIZE: I1(TGL_TEX_MAX); break;  /* texture.c TEX_SIZE: native sizes up to it */
  case GL_MAX_3D_TEXTURE_SIZE: I1(0); break;
  case GL_MAX_CUBE_MAP_TEXTURE_SIZE: I1(0); break;
  case GL_MAX_PIXEL_MAP_TABLE: I1(32); break;
  case GL_MAX_NAME_STACK_DEPTH: I1(MAX_NAME_STACK_DEPTH); break;
  case GL_MAX_LIST_NESTING: I1(64); break;
  case GL_MAX_EVAL_ORDER: I1(8); break;
  case GL_MAX_VIEWPORT_DIMS: iv[0] = iv[1] = 4096; n = 2; break;
  case GL_MAX_ATTRIB_STACK_DEPTH: I1(16); break;
  case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: I1(16); break;
  case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: I1(4096); break;
  case GL_MAX_TEXTURE_UNITS: I1(c->mtex_adv ? 2 : 1); break;   /* phase 5 O1 */
  /* s31: aliased widths and sizes are drawn at the nearest integer
     (raster.c). Phase 4 SMOOTH: GL_POINT_SIZE_RANGE and GL_LINE_WIDTH_RANGE
     are the smooth ranges (GL 1.2's GL_SMOOTH_*_RANGE, the same enums), and
     smooth lines and points are drawn at their real size from 1 up, so
     their granularity is fine; 1/8 is stated */
  case GL_ALIASED_POINT_SIZE_RANGE: case GL_POINT_SIZE_RANGE:
  case GL_ALIASED_LINE_WIDTH_RANGE: case GL_LINE_WIDTH_RANGE:
    fv[0] = 1.0f; fv[1] = 64.0f; *kind = TGL_GET_FLOAT; n = 2; break;
  case GL_POINT_SIZE_GRANULARITY: case GL_LINE_WIDTH_GRANULARITY:
    F1(0.125f); break;
  case GL_SAMPLE_BUFFERS: case GL_SAMPLES: I1(0); break;
  case GL_SAMPLE_COVERAGE_VALUE: F1(1.0f); break;
  case GL_SAMPLE_COVERAGE_INVERT: I1(0); break;
  case GL_NUM_COMPRESSED_TEXTURE_FORMATS: I1(0); break;
  case GL_COMPRESSED_TEXTURE_FORMATS: n = 0; break;
  case GL_RGBA_MODE: I1(1); break;
  case GL_INDEX_MODE: I1(0); break;
  case GL_DOUBLEBUFFER: I1(c->doublebuffer); break;
  case GL_STEREO: I1(0); break;
  case GL_RED_BITS: case GL_BLUE_BITS: I1(5); break;
  case GL_GREEN_BITS: I1(6); break;
  case GL_STENCIL_BITS: I1(c->stencil_bits); break;    /* phase 4 F8: 0 or 8 */
  case GL_ALPHA_BITS: case GL_INDEX_BITS:
  case GL_ACCUM_RED_BITS: case GL_ACCUM_GREEN_BITS: case GL_ACCUM_BLUE_BITS:
  case GL_ACCUM_ALPHA_BITS: case GL_AUX_BUFFERS:
    I1(0); break;
  case GL_DEPTH_BITS: I1(16); break;
  case GL_IMPLEMENTATION_COLOR_READ_TYPE: I1(GL_UNSIGNED_SHORT_5_6_5); break;
  case GL_IMPLEMENTATION_COLOR_READ_FORMAT: I1(GL_RGB); break;

  /* miscellaneous */
  case GL_LIST_BASE: I1(c->list_base); break;
  case GL_LIST_INDEX: I1(c->list_index); break;
  case GL_LIST_MODE: I1(c->list_mode); break;
  case GL_ATTRIB_STACK_DEPTH: I1(c->attrib.attrib_depth); break;
  case GL_CLIENT_ATTRIB_STACK_DEPTH: I1(c->attrib.client_depth); break;
  case GL_NAME_STACK_DEPTH: I1(c->name_stack_size); break;
  case GL_RENDER_MODE: I1(c->render_mode); break;
  case GL_SELECTION_BUFFER_SIZE: I1(c->select_size); break;
  case GL_FEEDBACK_BUFFER_SIZE: I1(0); break;

  default: {
    /* every capability is also a glGet name */
    int e = s31_cap_get(c, pname);
    if (e >= 0) I1(e);
    break;
  }
  }

  /* fill the other representation */
  if (n > 0) {
    if (*kind == TGL_GET_INT) {
      for (i = 0; i < n; i++) fv[i] = (float)iv[i];
    } else {
      for (i = 0; i < n; i++) iv[i] = (int)(fv[i] < 0 ? fv[i] - 0.5f : fv[i] + 0.5f);
    }
  }
  return n;
}

int tgl_get_light(int light, int pname, float *v)
{
  GLContext *c = gl_get_context();
  GLLight *l;
  int i;

  if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS) return -1;
  l = &c->lights[light - GL_LIGHT0];
  switch (pname) {
  case GL_AMBIENT: for (i = 0; i < 4; i++) v[i] = l->ambient.v[i]; return 4;
  case GL_DIFFUSE: for (i = 0; i < 4; i++) v[i] = l->diffuse.v[i]; return 4;
  case GL_SPECULAR: for (i = 0; i < 4; i++) v[i] = l->specular.v[i]; return 4;
  case GL_POSITION: for (i = 0; i < 4; i++) v[i] = l->position.v[i]; return 4;
  case GL_SPOT_DIRECTION:
    for (i = 0; i < 3; i++) v[i] = l->spot_direction.v[i];
    return 3;
  case GL_SPOT_EXPONENT: v[0] = l->spot_exponent; return 1;
  case GL_SPOT_CUTOFF: v[0] = l->spot_cutoff; return 1;
  case GL_CONSTANT_ATTENUATION: v[0] = l->attenuation[0]; return 1;
  case GL_LINEAR_ATTENUATION: v[0] = l->attenuation[1]; return 1;
  case GL_QUADRATIC_ATTENUATION: v[0] = l->attenuation[2]; return 1;
  default: return -1;
  }
}

int tgl_get_material(int face, int pname, float *v)
{
  GLContext *c = gl_get_context();
  GLMaterial *m;
  int i;

  if (face != GL_FRONT && face != GL_BACK) return -1;
  m = &c->materials[face == GL_FRONT ? 0 : 1];
  switch (pname) {
  case GL_AMBIENT: for (i = 0; i < 4; i++) v[i] = m->ambient.v[i]; return 4;
  case GL_DIFFUSE: for (i = 0; i < 4; i++) v[i] = m->diffuse.v[i]; return 4;
  case GL_SPECULAR: for (i = 0; i < 4; i++) v[i] = m->specular.v[i]; return 4;
  case GL_EMISSION: for (i = 0; i < 4; i++) v[i] = m->emission.v[i]; return 4;
  case GL_SHININESS: v[0] = m->shininess; return 1;
  case GL_COLOR_INDEXES: v[0] = 0; v[1] = 1; v[2] = 1; return 3;
  default: return -1;
  }
}

void *tgl_get_pointer(int pname)
{
  GLContext *c = gl_get_context();
  switch (pname) {
  case GL_VERTEX_ARRAY_POINTER: return c->vertex_array;
  case GL_NORMAL_ARRAY_POINTER: return c->normal_array;
  case GL_COLOR_ARRAY_POINTER: return c->color_array;
  case GL_TEXTURE_COORD_ARRAY_POINTER:      /* phase 5 O1: the client-active unit's */
    return c->client_tex ? (void *)c->tc1.array : (void *)c->texcoord_array;
  case GL_EDGE_FLAG_ARRAY_POINTER: return c->edge_flag_array;
  case GL_SELECTION_BUFFER_POINTER: return c->select_buffer;
  default: return (void *)-1;
  }
}
