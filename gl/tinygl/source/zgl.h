#ifndef _tgl_zgl_h_
#define _tgl_zgl_h_

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <GL/gl.h>
#include "tgl_bridge.h"
#include "zbuffer.h"
#include "zmath.h"
#include "zfeatures.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEBUG
/* #define NDEBUG */

enum {

#define ADD_OP(a,b,c) OP_ ## a ,

#include "opinfo.h"

};

/* initially # of allocated GLVertexes (will grow when necessary) */
#define POLYGON_MAX_VERTEX 16

/* Max # of specular light pow buffers */
#define MAX_SPECULAR_BUFFERS 8
/* # of entries in specular buffer */
#define SPECULAR_BUFFER_SIZE 1024
/* specular buffer granularity */
#define SPECULAR_BUFFER_RESOLUTION 1024


#define MAX_MODELVIEW_STACK_DEPTH  32
#define MAX_PROJECTION_STACK_DEPTH 8
#define MAX_TEXTURE_STACK_DEPTH    8
#define MAX_NAME_STACK_DEPTH       64
#define MAX_TEXTURE_LEVELS         11
#define MAX_LIGHTS                 16

#define VERTEX_HASH_SIZE 1031

#define MAX_DISPLAY_LISTS 1024
#define OP_BUFFER_MAX_SIZE 512

#define TGL_OFFSET_FILL    0x1
#define TGL_OFFSET_LINE    0x2
#define TGL_OFFSET_POINT   0x4

typedef struct GLSpecBuf {
  int shininess_i;
  int last_used;
  float buf[SPECULAR_BUFFER_SIZE+1];
  struct GLSpecBuf *next;
} GLSpecBuf;

typedef struct GLLight {
  V4 ambient;
  V4 diffuse;
  V4 specular;
  V4 position;	
  V3 spot_direction;
  float spot_exponent;
  float spot_cutoff;
  float attenuation[3];
  /* precomputed values */
  float cos_spot_cutoff;
  V3 norm_spot_direction;
  V3 norm_position;
  /* we use a linked list to know which are the enabled lights */
  int enabled;
  struct GLLight *next,*prev;
} GLLight;

typedef struct GLMaterial {
  V4 emission;
  V4 ambient;
  V4 diffuse;
  V4 specular;
  float shininess;

  /* computed values */
  int shininess_i;
  int do_specular;  
} GLMaterial;


typedef struct GLViewport {
  int xmin,ymin,xsize,ysize;   /* as given to glViewport: GL window coords, y up */
  V3 scale;
  V3 trans;
  int updated;
  /* s31: the viewport guard (vertex.c gl_eval_viewport). When the viewport
     is not inside the colour buffer, triangles are clipped to the part that
     is, by folding a clip-space scale/offset into the projection, so the
     rasteriser never writes outside the buffer and costs nothing extra. */
  int guard;                   /* 1: proj_eff = S * P is in use */
  int empty;                   /* 1: nothing of the viewport is on the buffer */
  float gx[2], gy[2];          /* S: x' = gx0*x + gx1*w, y' = gy0*y + gy1*w */
} GLViewport;

typedef union {
  int op;
  float f;
  int i;
  unsigned int ui;
  void *p;
} GLParam;

typedef struct GLParamBuffer {
  GLParam ops[OP_BUFFER_MAX_SIZE];
  struct GLParamBuffer *next;
} GLParamBuffer;

typedef struct GLList {
  GLParamBuffer *first_op_buffer;
  /* TODO: extensions for an hash table or a better allocating scheme */
} GLList;

typedef struct GLVertex {
  int edge_flag;
  V3 normal;
  V4 coord;
  V4 tex_coord;
  V4 color;
  
  /* computed values */
  V4 ec;                /* eye coordinates */
  V4 pc;                /* coordinates in the normalized volume */
  int clip_code;        /* clip code */
  ZBufferPoint zp;      /* integer coordinates for the rasterization */
} GLVertex;

typedef struct GLImage {
  void *pixmap;
  int xsize,ysize;
} GLImage;

/* textures */

#define TEXTURE_HASH_TABLE_SIZE 256

typedef struct GLTexture {
  GLImage images[MAX_TEXTURE_LEVELS];
  int handle;
  struct GLTexture *next,*prev;
  /* s31: what the application specified, for glGet* and later stages */
  int width, height, internal_format; /* level 0 as uploaded, before resampling */
  int min_filter, mag_filter, wrap_s, wrap_t;
  float priority;
} GLTexture;


/* shared state */

typedef struct GLSharedState {
  GLList **lists;
  GLTexture **texture_hash_table;
  int *refs;     /* s31: contexts sharing these tables (glXCreateContext share_list) */
} GLSharedState;

struct GLContext;

typedef void (*gl_draw_triangle_func)(struct GLContext *c,
                                      GLVertex *p0,GLVertex *p1,GLVertex *p2);

/* display context */

typedef struct GLContext {
  /* Z buffer */
  ZBuffer *zb;

  /* lights */
  GLLight lights[MAX_LIGHTS];
  GLLight *first_light;
  V4 ambient_light_model;
  int local_light_model;
  int lighting_enabled;
  int light_model_two_side;

  /* materials */
  GLMaterial materials[2];
  int color_material_enabled;
  int current_color_material_mode;
  int current_color_material_type;

  /* textures */
  GLTexture *current_texture;
  int texture_2d_enabled;

  /* shared state */
  GLSharedState shared_state;

  /* current list */
  GLParamBuffer *current_op_buffer;
  int current_op_buffer_index;
  int exec_flag,compile_flag,print_flag;

  /* matrix */

  int matrix_mode;
  M4 *matrix_stack[3];
  M4 *matrix_stack_ptr[3];
  int matrix_stack_depth_max[3];

  M4 matrix_model_view_inv;
  M4 matrix_model_projection;
  int matrix_model_projection_updated;
  int matrix_model_projection_no_w_transform; 
  int apply_texture_matrix;

  /* viewport */
  GLViewport viewport;

  /* current state */
  int polygon_mode_back;
  int polygon_mode_front;

  int current_front_face;
  int current_shade_model;
  int current_cull_face;
  int cull_face_enabled;
  int normalize_enabled;
  gl_draw_triangle_func draw_triangle_front,draw_triangle_back;

  /* selection */
  int render_mode;
  unsigned int *select_buffer;
  int select_size;
  unsigned int *select_ptr,*select_hit;
  int select_overflow;
  int select_hits;

  /* names */
  unsigned int name_stack[MAX_NAME_STACK_DEPTH];
  int name_stack_size;

  /* clear */
  float clear_depth;
  V4 clear_color;

  /* current vertex state */
  V4 current_color;
  unsigned int longcurrent_color[3]; /* precomputed integer color */
  V4 current_normal;
  V4 current_tex_coord;
  int current_edge_flag;

  /* glBegin / glEnd */
  int in_begin;
  int begin_type;
  int vertex_n,vertex_cnt;
  int vertex_max;
  GLVertex *vertex;

  /* opengl 1.1 arrays  */
  float *vertex_array;
  int vertex_array_size;
  int vertex_array_stride;
  float *normal_array;
  int normal_array_stride;
  float *color_array;
  int color_array_size;
  int color_array_stride;
  float *texcoord_array;
  int texcoord_array_size;
  int texcoord_array_stride;
  int client_states;
  
  /* opengl 1.1 polygon offset */
  float offset_factor;
  float offset_units;
  int offset_states;
  
  /* specular buffer. could probably be shared between contexts, 
    but that wouldn't be 100% thread safe */
  GLSpecBuf *specbuf_first;
  int specbuf_used_counter;
  int specbuf_num_buffers;

  /* opaque structure for user's use */
  void *opaque;
  /* resize viewport function */
  int (*gl_resize_viewport)(struct GLContext *c,int *xsize,int *ysize);

  /* depth test */
  int depth_test;

  /*
   * s31 additions. State below is recorded exactly as GL defines it so that
   * glGet* and glIsEnabled answer honestly; which of it the rasteriser
   * honours today is stated in gl/tinygl/README.s31.
   */
  int error;                  /* first unread GL error (glGetError) */
  unsigned int caps, caps_hi; /* enable bits, table in s31_state.c */
  int ready;                  /* colour + depth present, frame hook done */
  int armed;                  /* call the frame hook before the next access */
  int prepare_busy;
  int (*prepare)(void *user); /* GLX frame_begin: supplies/validates colour */
  void *prepare_user;
  M4 matrix_proj_eff;         /* S * P when viewport.guard (see GLViewport) */
  M4 *proj_used;              /* &matrix_proj_eff or matrix_stack_ptr[1] */
  int vp_initialized;         /* first buffer bind sets viewport + scissor */
  int doublebuffer;           /* GL_DOUBLEBUFFER, set by the GLX layer */

  int depth_func, depth_mask;
  float depth_range[2];
  int blend_src, blend_dst;
  float blend_color[4];
  int alpha_func; float alpha_ref;
  int color_mask[4];
  int scissor[4];
  float line_width, point_size;
  int line_stipple_factor, line_stipple_pattern;
  int fog_mode; float fog_density, fog_start, fog_end, fog_index; V4 fog_color;
  int texenv_mode; V4 texenv_color;
  int logic_op;
  int stencil_func, stencil_ref, stencil_value_mask, stencil_writemask;
  int stencil_fail, stencil_zfail, stencil_zpass, stencil_clear;
  int hint_perspective, hint_point, hint_line, hint_polygon, hint_fog;
  int draw_buffer, read_buffer;
  int unpack_swap, unpack_lsb, unpack_row_length, unpack_skip_rows,
      unpack_skip_pixels, unpack_alignment, unpack_image_height, unpack_skip_images;
  int pack_swap, pack_lsb, pack_row_length, pack_skip_rows,
      pack_skip_pixels, pack_alignment, pack_image_height, pack_skip_images;
  int list_base, list_index, list_mode;
  int vertex_array_type, color_array_type, normal_array_type, texcoord_array_type;
  int vertex_array_bstride, color_array_bstride, normal_array_bstride, texcoord_array_bstride;
  int edge_flag_array_stride; void *edge_flag_array;
  int proxy_width, proxy_height, proxy_format;
  float raster_pos[4];
  int raster_valid;
  int flat_r, flat_g, flat_b; /* GL_FLAT: the provoking vertex's colour, zp scale */
  struct tgl_attrib_slots attrib; /* glPush/PopAttrib stacks (gl/api/gl_pushattrib.c) */
} GLContext;

extern GLContext *gl_ctx;

void gl_add_op(GLParam *p);

/* s31: error recording. The first error sticks until glGetError reads it. */
static inline void gl_set_error(GLContext *c, int e)
{
  if (c->error == 0) c->error = e;
}
/* s31: make sure colour and depth exist before drawing. 0 = drop the draw. */
int gl_prepare_slow(GLContext *c);
static inline int gl_prepare(GLContext *c)
{
  if (c->ready) return 1;
  return gl_prepare_slow(c);
}
/* s31: begin_type that discards every vertex (bad mode, no buffer, off-screen) */
#define TGL_BEGIN_DISCARD 0x7fff
void gl_eval_viewport(GLContext *c);
void gl_warn_once(const char *what);
/* s31_state.c */
int s31_cap_index(int cap);          /* -1: not a GL capability */
void s31_cap_record(GLContext *c, int cap, int v);
int s31_cap_get(GLContext *c, int cap);
void s31_state_init(GLContext *c);
int s31_client_state(GLContext *c, int array);   /* arrays.c; -1 = not one */

/* s31: a vertex colour in the rasteriser's ZBufferPoint scale, clamped as
   glColor4f clamps it (api.c). Used where a vertex's zp colour is not
   already right: clipped vertices, and the provoking vertex of GL_FLAT. */
static inline int gl_zp_chan(float v, int lo, int hi)
{
  if (v <= 0.0f) return lo;
  if (v >= 1.0f) return hi;
  return (int)(v * (hi - lo) + lo);
}
static inline void gl_zp_color(ZBufferPoint *zp, const V4 *col)
{
  zp->r = gl_zp_chan(col->v[0], ZB_POINT_RED_MIN, ZB_POINT_RED_MAX);
  zp->g = gl_zp_chan(col->v[1], ZB_POINT_GREEN_MIN, ZB_POINT_GREEN_MAX);
  zp->b = gl_zp_chan(col->v[2], ZB_POINT_BLUE_MIN, ZB_POINT_BLUE_MAX);
}
/* GL_FLAT takes the whole primitive's colour from one vertex (GL 1.5 table
   2.12: the last vertex of a line, triangle or quad, the first of a
   polygon). TinyGL's fillers read p2, which clipping and the quad/strip
   decompositions do not keep as that vertex, so the primitive assembler
   records it here before each gl_draw_triangle / gl_draw_line. */
void gl_set_provoking_flat(GLContext *c, GLVertex *v);   /* clip.c */
static inline void gl_set_provoking(GLContext *c, GLVertex *v)
{
  if (c->current_shade_model != GL_SMOOTH)
    gl_set_provoking_flat(c, v);
}

/* clip.c */
void gl_transform_to_viewport(GLContext *c,GLVertex *v);
void gl_draw_triangle(GLContext *c,GLVertex *p0,GLVertex *p1,GLVertex *p2);
void gl_draw_line(GLContext *c,GLVertex *p0,GLVertex *p1);
void gl_draw_point(GLContext *c,GLVertex *p0);

void gl_draw_triangle_point(GLContext *c,
                            GLVertex *p0,GLVertex *p1,GLVertex *p2);
void gl_draw_triangle_line(GLContext *c,
                           GLVertex *p0,GLVertex *p1,GLVertex *p2);
void gl_draw_triangle_fill(GLContext *c,
                           GLVertex *p0,GLVertex *p1,GLVertex *p2);
void gl_draw_triangle_select(GLContext *c,
                             GLVertex *p0,GLVertex *p1,GLVertex *p2);

/* matrix.c */
void gl_print_matrix(const float *m);
/*
void glopLoadIdentity(GLContext *c,GLParam *p);
void glopTranslate(GLContext *c,GLParam *p);*/

/* light.c */
void gl_add_select(GLContext *c,unsigned int zmin,unsigned int zmax);
void gl_enable_disable_light(GLContext *c,int light,int v);
void gl_shade_vertex(GLContext *c,GLVertex *v);

void glInitTextures(GLContext *c);
void glEndTextures(GLContext *c);
GLTexture *alloc_texture(GLContext *c,int h);

/* image_util.c */
void gl_convertRGB_to_5R6G5B(unsigned short *pixmap,unsigned char *rgb,
                             int xsize,int ysize);
void gl_convertRGB_to_8A8R8G8B(unsigned int *pixmap, unsigned char *rgb,
                               int xsize, int ysize);
void gl_resizeImage(unsigned char *dest,int xsize_dest,int ysize_dest,
                    unsigned char *src,int xsize_src,int ysize_src);
void gl_resizeImageNoInterpolate(unsigned char *dest,int xsize_dest,int ysize_dest,
                                 unsigned char *src,int xsize_src,int ysize_src);

GLContext *gl_get_context(void);

/* s31: no longer exits; prints once per call site and returns */
void gl_fatal_error(char *format, ...);


/* specular buffer "api" */
GLSpecBuf *specbuf_get_buffer(GLContext *c, const int shininess_i, 
                              const float shininess);

/* glopXXX functions */

#define ADD_OP(a,b,c) void glop ## a (GLContext *,GLParam *);
#include "opinfo.h"

/* this clip epsilon is needed to avoid some rounding errors after
   several clipping stages */

/* s31: a float constant. As (1E-5) the product below was done in double -
   __extendsfdf2 + __muldf3 + __truncdfsf2 on EVERY vertex (inlined into
   glopVertex) and four more sets per clipped triangle - the one soft-double
   left on the per-vertex path (review finding R2-clipcode-soft-double). */
#define CLIP_EPSILON (1E-5f)

static inline int gl_clipcode(float x,float y,float z,float w1)
{
  float w;

  w=w1 * (1.0f + CLIP_EPSILON);
  return (x<-w) |
    ((x>w)<<1) |
    ((y<-w)<<2) |
    ((y>w)<<3) |
    ((z<-w)<<4) | 
    ((z>w)<<5) ;
}

#endif /* _tgl_zgl_h_ */
