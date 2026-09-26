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
#include "zpipe.h"

/* s31 (phase 5, O7): fixed-size word copy and compare for the per-vertex and
   per-glBegin blocks (64 to 156 bytes). A libc memcpy/memcmp of 64 bytes
   or more takes musl's ESP PIE path, and on the lent CPU (hart0) that traps
   and bounces the task back (artifacts/gl/glquake/LIBGL-OPPORTUNITIES.md
   O7). The empty asm makes each word opaque, so GCC's loop-distribution
   pass cannot turn the loop back into a memcpy call. n is in words; both
   blocks 4-aligned. */
typedef unsigned int s31_w32 __attribute__((may_alias));
static inline void s31_wcopy(void *dst, const void *src, int n)
{
  s31_w32 *d = (s31_w32 *)dst;
  const s31_w32 *s = (const s31_w32 *)src;
  /* unrolled: a plain word loop measured +15% on gl/bench geo11 (vertex
     cache hits) against newlib's memcpy in the proxy */
#pragma GCC unroll 8
  for (; n > 0; n--) {
    unsigned int w = *s++;
    __asm__("" : "+r"(w));
    *d++ = w;
  }
}
/* 1 when the n words differ */
static inline int s31_wdiff(const void *a, const void *b, int n)
{
  const s31_w32 *x = (const s31_w32 *)a, *y = (const s31_w32 *)b;
  unsigned int acc = 0;
#pragma GCC unroll 8
  for (; n > 0; n--) {
    unsigned int w = *x++ ^ *y++;
    __asm__("" : "+r"(w));
    acc |= w;
  }
  return acc != 0;
}

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
  /* s31 (phase 3a G02): specular red, green or blue is not zero */
  int has_specular;
  /* s31 (phase 3a G02): ambient * material ambient, per material side,
     while !c->light_dirty (light.c gl_light_products) */
  V3 amb_prod[2];
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
  /* s31 (phase 3a G02): specular red, green or blue is not zero (the
     specular term of a light is skipped when either side's is zero: it
     would add exactly 0), and the specular table last used */
  int do_specular;
  struct GLSpecBuf *specbuf;
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
  /* s31: GL's exact window transform of the (guarded) clip coordinates,
     for the general filler: x = ex[0] * X/W + ex[1], y (rows from the top)
     = ey[0] * Y/W + ey[1] */
  float ex[2], ey[2];
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
  void *owned;   /* s31: pixel data compiled into the list (gl_list_own) */
} GLList;

typedef struct GLVertex {
  int edge_flag;
  /* s31: the normal is dead once the vertex is lit, so with
     GL_SEPARATE_SPECULAR_COLOR the same storage holds the secondary
     (specular) colour (light.c) - the vertex does not grow */
  union { V3 normal; V3 spec; };
  /* s31: the object coordinates are dead once the vertex is lit (lighting
     runs last in glopVertex), so with GL_LIGHT_MODEL_TWO_SIDE the back
     material's colour takes their storage, and the back secondary colour
     that of the eye coordinates (light.c; the vertex does not grow: it is
     copied along every strip) */
  union { V4 coord; V4 color_back; };
  V4 tex_coord;
  V4 color;
  float fog;            /* s31: GL fog factor f in [0,1] (general path only) */
  
  /* computed values */
  union { V4 ec; V3 spec_back; };   /* eye coordinates */
  V4 pc;                /* coordinates in the normalized volume */
  int clip_code;        /* clip code */
  ZBufferPoint zp;      /* integer coordinates for the rasterization */
  /* s31 (phase 5 O1): texture unit 1's coordinates (texgen and its texture
     matrix applied; s, t, r, q), set only while unit 1 is on (tu1_on).
     Last, so every field before it keeps its offset */
  V4 tex_coord1;
} GLVertex;

typedef struct GLImage {
  void *pixmap;
  int xsize,ysize;
} GLImage;

/* textures */

#define TEXTURE_HASH_TABLE_SIZE 256

/* s31: stored format classes (texture.c): what the texenv needs to know */
enum { TGL_TEXF_RGB,        /* RGB565, no alpha: RGB, LUMINANCE, 3, 1 ... */
       TGL_TEXF_RGBA,       /* RGB565 + A8: RGBA, LUMINANCE_ALPHA, 4, 2 ... */
       TGL_TEXF_ALPHA,      /* A8 (colour plane white): ALPHA */
       TGL_TEXF_INTENSITY   /* I in the colour plane and in A8 */
};

/* s31 phase 5 (s31_tex8.c): how a texture's levels are stored. Every
   stored level of a texture has the same kind (and, for L8, the same
   alpha mode). P8 and L8 hold the texels exactly, 8 bits a channel */
enum { TGL_ST_565,          /* RGB565 (+ A8): the classes above */
       TGL_ST_P8,           /* 8-bit indices into the level's RGBA8 palette:
                               at most 256 distinct texels (ALPHA: a fixed
                               ramp, the index is A) */
       TGL_ST_L8,           /* 8-bit grey (r = g = b) and an alpha by
                               GLTexture.amode: LUMINANCE(_ALPHA),
                               INTENSITY and grey RGB(A) such as lightmaps */
       TGL_ST_W32           /* RGBA8 texels: the unpacked reference of P8
                               and L8 (S31GL_TEX8=2, the bit-identity gate) */
};
/* GLTexture.amode, TGL_ST_L8: where a level's alpha is */
enum { TGL_AM_ONE,          /* 255 everywhere: no plane */
       TGL_AM_BITS,         /* 0 or 255: a bit a texel (texel k: bit k & 7 of
                               byte k >> 3), after the grey plane */
       TGL_AM_A8,           /* an A8 plane after the grey plane */
       TGL_AM_I,            /* the grey value itself (INTENSITY) */
       TGL_AM_ALPHA         /* ALPHA: the plane is the alpha, the colour white */
};

/* s31 phase 5: the palette of a level with 8-bit texels (TGL_ST_P8, and
   TGL_ST_W32, whose storage decisions follow the same palette). The
   entries are the texels as GL defines them for the base format (RGB: a
   255, LUMINANCE: (L, L, L, 255), ALPHA: (255, 255, 255, A), INTENSITY:
   (I, I, I, I)), little-endian r | g << 8 | b << 16 | a << 24, in the
   level's block: its texels, this header (the level's pal points at it),
   the entries, then (level 0) their PACKs */
typedef struct GLTexPal {
  unsigned int *w;           /* the entries */
  unsigned short *p565;      /* level 0: PACK of each entry (tier 1's REPLACE) */
  unsigned short n, cap;     /* used, allocated */
  unsigned char grey;        /* every entry has r == g == b (a lightmap) */
  unsigned char pad[3];
} GLTexPal;

#define TGL_STORED_LEVELS 1
typedef struct GLTexture {
  /* s31: only level 0 is stored (the other levels are recorded in lw, lh,
     lfmt): one image, not MAX_TEXTURE_LEVELS - 120 bytes a texture object
     (review P6; a TyrQuake-class app has hundreds) */
  GLImage images[TGL_STORED_LEVELS];
  int handle;
  struct GLTexture *next,*prev;
  /* s31: what the application specified, for glGet* */
  int width, height, internal_format; /* level 0 as specified (incl. border) */
  int min_filter, mag_filter, wrap_s, wrap_t;
  float priority;
  /* GL 1.2 (review 4 R4): GL_TEXTURE_BASE_LEVEL / MAX_LEVEL (the chain
     ends at MAX_LEVEL; a BASE_LEVEL above 0 is recorded, level 0 stays
     the base) and MIN_LOD / MAX_LOD (lambda is clamped to them) */
  short base_level, max_level;
  float min_lod, max_lod;
  /* s31 (plan F3): level 0 at its own power-of-two size */
  unsigned char *alpha;     /* A8 plane after the colour plane, or NULL */
  int ws, hs;               /* log2 of the stored width and height */
  int fbits;                /* fraction bits of s/t (texture.c) */
  int fmt;                  /* TGL_TEXF_* */
  int border;               /* of level 0 */
  /* every level as specified (width 0: none), for completeness and for
     glGetTexLevelParameter; levels > 0 are not stored */
  unsigned short lw[MAX_TEXTURE_LEVELS], lh[MAX_TEXTURE_LEVELS];
  unsigned short lfmt[MAX_TEXTURE_LEVELS];   /* internal formats fit 16 bits */
  /* s31 (phase 4 F-LIN): levels 1.. as uploaded, allocated at the first
     level > 0 the application specifies (NULL: none - a texture without
     mipmaps pays 4 bytes). Each level is its own RGB565 (+ A8) block of
     its own size, so a full chain costs a third of level 0 more */
  struct GLMipChain *mip;
  /* s31 phase 5 (s31_tex8.c): the storage kind (TGL_ST_*) of every stored
     level; a1: an RGBA-class TGL_ST_565 texture whose texels all have
     alpha 255, stored without A8 planes (its alpha reads as 255); level
     0's palette (TGL_ST_P8, and TGL_ST_W32 of a P8 reference).
     images[0].pixmap (GLMipLevel.pix) is a level's block in every kind:
     565 plane then A8; P8 indices then the palette's entries and PACKs;
     L8 grey plane then its alpha (amode); W32 words then the palette */
  unsigned char st, a1;
  /* TGL_ST_L8: TGL_AM_*; TGL_ST_W32: the kind (P8 or L8) whose decisions
     the reference mirrors (stref) */
  unsigned char amode, stref;
  GLTexPal *pal0;           /* in level 0's block (NULL: none) */
} GLTexture;

/* s31 (phase 4): a stored level > 0 (texture.c). cls is the stored class
   (TGL_TEXF_*) its data was converted to; a level whose class is not
   level 0's is not used (the mipmap filters then sample level 0) */
typedef struct GLMipLevel {
  void *pix;                /* RGB565, then the A8 plane when cls has alpha;
                               phase 5: the level's block of GLTexture.st */
  unsigned char *alpha;
  unsigned char ws, hs, cls, pad;
  GLTexPal *pal;            /* phase 5: TGL_ST_P8 / W32, in the block */
} GLMipLevel;
typedef struct GLMipChain {
  GLMipLevel l[MAX_TEXTURE_LEVELS];   /* l[0] unused */
} GLMipChain;


/* s31 (phase 5 O1): GL_ARB_texture_env_combine state of one texture unit
   (GL 1.3 table 6.17): the functions, the three sources and operands of
   the RGB and alpha parts, and the scales (1, 2 or 4) */
typedef struct GLCombine {
  int rgb, alpha;               /* GL_COMBINE_RGB / GL_COMBINE_ALPHA */
  int src[2][3];                /* [0] SOURCEn_RGB, [1] SOURCEn_ALPHA */
  int op[2][3];                 /* [0] OPERANDn_RGB, [1] OPERANDn_ALPHA */
  float scale[2];               /* GL_RGB_SCALE, GL_ALPHA_SCALE */
} GLCombine;

/* s31 (phase 5 O1): the state of a texture unit that glActiveTexture
   selects (GL 1.3 2.7, 3.8.x). Unit 0's lives in the GLContext fields TinyGL
   always had - current_texture, texenv_mode, texgen_*, matrix_stack[2] ... -
   so every path that uses one texture reads what it read before. Unit 1's
   is a GLTexUnit (GLContext.tu1). A command that acts on the ACTIVE unit
   while unit 1 is active swaps the two sets around its body (s31_mtex.c
   tu_swap), so the existing code serves both units, and between commands
   the context fields always hold unit 0. */
typedef struct GLTexUnit {
  GLTexture *tex2d, *tex1d;     /* bindings */
  int enables;                  /* tex_enables: bit 0 2D, bit 1 1D */
  int any_enabled;              /* texture_2d_enabled */
  unsigned int capbits;         /* glIsEnabled bits of the unit's caps (s31_state.c) */
  int env_mode; V4 env_color;
  GLCombine comb;
  int texgen_mask, texgen_mode[4], texgen_eye_needed;
  V4 texgen_obj[4], texgen_eye[4];
  V4 cur_tc;                    /* the current texture coordinates */
  float raster_tex[4];          /* the raster position's */
} GLTexUnit;

/* s31 (phase 5 O1): the vertex-array state that glClientActiveTexture
   selects (the texture coordinate array); unit 0's is the context's */
typedef struct GLTexClient {
  float *array;
  int size, stride, type, bstride;
} GLTexClient;                  /* (enabled: client_states bit 0x40, arrays.c) */

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

  /* lights (s31 phase 3a: the array itself is at the end of the struct) */
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
  /* phase 4 L1 (s31_ramtext.h): glVertex's executing path, NULL while
     compiling or printing - one load and test in place of the two flags,
     and the RAM copy of gl_vertex4f when S31GL_RAMTEXT put one there */
  void (*vtx_run)(float x, float y, float z, float w, struct GLContext *c);

  /* matrix */

  int matrix_mode;
  /* s31 (phase 5 O1): [3] is texture unit 1's texture matrix stack;
     matrix_mode is 3 for GL_TEXTURE while unit 1 is active (matrix.c) */
  M4 *matrix_stack[4];
  M4 *matrix_stack_ptr[4];
  int matrix_stack_depth_max[4];

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
  M4 *proj_used;              /* &matrix_proj_eff or matrix_stack_ptr[1] */
  int vp_initialized;         /* first buffer bind sets viewport + scissor */
  /* s31 render scale (plan G04, s31gl_set_render_scale): the colour and
     depth buffers hold the window at 1/2^rscale in each axis. Viewport,
     scissor, raster position and every pixel-rectangle coordinate stay in
     the WINDOW's units (what glGet reports); they are mapped onto the
     buffer where they are used: gl_eval_viewport, the scissored clear,
     the line/point widths and s31_draw.c's pixel paths. 0 = native. */
  int rscale;
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
  int proxy_width, proxy_height, proxy_format, proxy_border, proxy_level;
  float raster_pos[4];
  int raster_valid;
  int flat_r, flat_g, flat_b; /* GL_FLAT: the provoking vertex's colour, zp scale */
  struct tgl_attrib_slots attrib; /* glPush/PopAttrib stacks (gl/api/gl_pushattrib.c) */

  /* s31 (plan F3-F6): which rasteriser path draws, chosen at glBegin by
     raster.c gl_update_raster whenever raster_dirty */
  GLVertex *flat_vtx;         /* the provoking vertex (alpha, general path) */
  int raster_fog;             /* fog factor per vertex (general path + GL_FOG) */
  int blend_enabled, alpha_test_enabled, fog_enabled, scissor_enabled;
  int rescale_normal_enabled;
  int color_control;          /* GL_LIGHT_MODEL_COLOR_CONTROL */
  int raster_sepspec;         /* secondary colour kept apart (light.c, zpipe.c) */
  float rescale;              /* GL_RESCALE_NORMAL factor (vertex.c) */
  int raster_dirty;
  int pipe_dirty;             /* the general path's stages need building */
  int raster_general;         /* GL_FILL triangles take the general path */
  int raster_gen_lines, raster_gen_points;
  int raster_need_attr;       /* clipping keeps alpha and fog */
  int raster_skip;            /* nothing can pass: depth/alpha GL_NEVER ... */
  gl_draw_triangle_func draw_fill, draw_fill_inner;
  /* tier 1: TinyGL's fillers for the depth state (LEQUAL, off, no write,
     strict LESS), and its line and point routines */
  ZB_fillTriangleFunc zb_flat, zb_smooth;
  ZB_fillTriangleFunc zb_map;
  void (*zb_line)(ZBuffer *, ZBufferPoint *, ZBufferPoint *);
  void (*zb_plot)(ZBuffer *, ZBufferPoint *);
  int tex_active;             /* GL_TEXTURE_2D on and the texture complete */
  float tex_sscale, tex_tscale; /* texcoord 1.0 in s/t fixed point */
  int tex_smax, tex_tmax;       /* the same as ints */
  float fog_scale;            /* 1 / (end - start) */
  int line_w, point_w;        /* integer widths */
  int rast_box[4];            /* buffer and scissor: x0 y0 x1 y1, rows from the top */
  ZPipe pipe;

  /* s31 (plan F7): pixel paths and the remaining vertex state. Nothing
     here is read on a path that uses none of it: glopVertex tests
     vtx_extra where it tested raster_fog, and texgen rides on
     apply_texture_matrix (bit 1) */
  /* s31 (phase 3a) hot per-vertex state, kept below the 2 kB that RV32
     loads reach from the context pointer (see the note at the end). G02: emission + ambient * scene ambient, per material side, and the
     lights' amb_prod, valid while light_dirty is 0; glMaterial, glLight,
     glLightModel and enabling a light set it */
  V3 light_base[2];
  int light_dirty;
  /* G14: the shape of the matrices the lit vertex path multiplies by
     (vertex.c gl_xf_kind): TGL_XF_* of proj_used, found at glBegin when
     xf_dirty bit 1 says the projection (or the guard, or the path) changed
     (matrix.c, vertex.c gl_eval_viewport, misc.c), and whether the
     modelview is affine (gl_normal_matrix) */
  int xf_proj, xf_mv_affine, xf_dirty;
  int vtx_extra;              /* 1: fog factor, 2: user clip planes (s31_xform.c) */
  int clip_plane_mask;        /* enabled GL_CLIP_PLANEi, bit i */
  int texgen_mask;            /* GL_TEXTURE_GEN_S/T/R/Q enabled, bits 0-3 */
  /* s31 (phase 5 O1): texture unit 1 was ever selected or given state
     (s31_mtex.c): glBegin and gl_update_raster test it, so it sits here */
  int mtex_used;
  int texgen_mode[4];
  int texgen_eye_needed;      /* a mode reads eye coordinates or the eye normal */
  float raster_color[4], raster_tex[4], raster_distance;
  float raster_fogz;          /* |z_e|: the fog distance of pixel fragments, as vertices use */
  float pixel_zoom[2];
  float xfer_scale[4], xfer_bias[4], depth_scale, depth_bias;
  int index_shift, index_offset, map_color, map_stencil;
  int xfer_active;            /* some scale/bias is not the identity */
  int poly_stipple_enabled, line_stipple_enabled, line_stipple_counter;
  int tex_enables;            /* bit 0 GL_TEXTURE_2D, bit 1 GL_TEXTURE_1D */
  GLTexture *current_texture_1d;
  GLTexture *tex1d_default;   /* this context's default 1D object */
  void *pixpipe;              /* s31_draw.c: the pixel paths' stage list, cached */
  unsigned int pipe_serial;   /* bumped by gl_build_pipe: the cache's key */

  /* s31 (phase 3a). Layout: GLContext is ~4.8 kB and RV32 loads reach
     +-2 kB from a base register; a field past 2 kB costs an extra addi at
     each use, so a field inserted in front of hot ones can cost glopVertex
     an instruction per vertex (measured: +4.8 k per teapot frame). Cold
     state goes HERE, at the end; hot state below 2 kB (check with DWARF:
     artifacts/gl/phase3a/LEVERS.md, "context layout") */
  /* G02: the modelview matrix_model_view_inv was computed from (bit
     patterns), so an unchanged modelview is not inverted again */
  unsigned int mvinv_src[16];
  int mvinv_valid;
  /* G14: glDrawElements' post-transform vertex cache (arrays.c):
     TGL_VCACHE vertices, direct-mapped by index, valid for one call
     (vc_gen); allocated at the first glDrawElements */
  GLVertex *vc;
  int *vc_idx;
  unsigned int *vc_tag;
  unsigned int vc_gen;
  /* (phase 3a) cold arrays moved here from the middle, so the hot fields
     sit below 2 kB: selection names, the guard's S * P (read through
     proj_used), the user clip planes and texgen planes (read only when
     enabled), the polygon stipple */
  unsigned int name_stack[MAX_NAME_STACK_DEPTH];
  M4 matrix_proj_eff;         /* S * P when viewport.guard (see GLViewport) */
  V4 clip_plane_eye[6];       /* eye coordinates, as glGetClipPlane reports */
  V4 clip_plane_clip[6];      /* the same planes in clip coordinates (proj_used) */
  V4 texgen_obj[4], texgen_eye[4];
  M4 texgen_mv_inv;           /* transposed inverse modelview, when lighting is off */
  unsigned int poly_stipple[32]; /* row y%32; bit i = window x%32 == i */
  /* the lights, reached through first_light / l pointers, not by offset
     from c: 16 x 136 B that sat between c and every hot field */
  GLLight lights[MAX_LIGHTS];
  /* s31 (phase 4, s31_tfilter.c): the general path's per-triangle state -
     texture filter levels, the filtered texels of a chunk, perspective
     colour - reached through pipe.x; cold here, at the end */
  ZPipeX pipex;
  int tex_filtered;           /* the active texture has a linear/mipmap filter */
  ZB_fillTriangleFunc zb_smooth_pc;  /* F-PERSP: tier 1's smooth filler for the depth state */
  ZB_fillTriangleFunc zb_smooth_long;/* long spans at equal w (s31_tfilter.c) */
  int mip_store;              /* store levels > 0 (S31GL_MIPMAPS, default 1) */
  int pc_enable;              /* S31GL_PERSPCOLOR (default 1): perspective-
                                 correct Gouraud colour (s31_tfilter.c) */
  int tex_filter;             /* S31GL_TEXFILTER (default 1): 0 draws every
                                 filter as nearest in level 0 (review 4) */
  /* s31 (phase 4 BLEND-EQ): glBlendEquation(Separate) and the separate
     alpha factors of glBlendFuncSeparate (blend_src / blend_dst are the RGB
     ones). There is no destination alpha plane, so the alpha factors and
     the alpha equation are recorded for glGet and change no stored pixel
     (raster_sel.c) */
  int blend_src_a, blend_dst_a, blend_eq, blend_eq_a;
  /* s31 (phase 4 F8-STENCIL, SMOOTH): GL_STENCIL_BITS of this context (0
     or 8, s31gl_set_stencil_bits: the GLX config's); the enables of the
     phase 4 features as one word (P4_EN_*), so gl_update_raster pays one
     load and one test for all of them when none is on; and what it
     decided, as one word too (P4_R_*: the stencil test runs - enabled and
     a buffer - and whether it can write; lines / points are drawn with
     coverage). Far from the context pointer (past 2 kB), so one of each */
  int stencil_bits;
  int p4_en, p4_raster;
  float aa_lw, aa_ps;         /* smooth width and size in buffer pixels (>= 1) */
  /* s31 (phase 5 O1): GL_ARB_multitexture, 2 units (s31_mtex.c). Far from
     the context pointer: nothing a single-texture frame runs reads these,
     except gl_update_raster's one test of mtex_used */
  int active_tex, client_tex;   /* glActiveTexture / glClientActiveTexture: 0 or 1 */
  GLCombine comb;               /* unit 0's GL_COMBINE state */
  GLTexUnit tu1;                /* unit 1's (see GLTexUnit) */
  GLTexClient tc1;              /* unit 1's texture coordinate array */
  /* what glBegin / gl_update_raster derived for unit 1 */
  int tu1_on;                   /* unit 1 is enabled and its texture complete */
  int tu1_apply;                /* its texture matrix is not identity (1), texgen (2) */
  GLTexture *tu1_tex;           /* the object unit 1 samples (2D over 1D) */
  float tex1_sscale, tex1_tscale;  /* texcoord 1.0 in its s/t fixed point */
  int tex1_speriod, tex1_tperiod;  /* one REPEAT period of s and t */
  int tu1_filtered;             /* its texture has a linear/mipmap filter */
  int fused_on;                 /* S31GL_FUSED (default 1): the fused fillers */
  int filt8;                    /* phase 5 S31GL_FILT8 (default 0): filtered
                                   RGB565 textures through the 8-bit (Mesa)
                                   filters too (raster_sel.c UNIT_W8) */
  int tex8;                     /* phase 5 S31GL_TEX8 (s31_tex8.c): 0 store as
                                   phase 4 did, 1 (default) 8-bit palette
                                   levels, 2 their unpacked RGBA8 reference */
  int mtex_adv;                 /* S31GL_MTEX (default 1): advertise 2 units */
} GLContext;

/* GLContext.p4_en */
#define P4_EN_STENCIL 1
#define P4_EN_LSMOOTH 2
#define P4_EN_PSMOOTH 4
/* GLContext.p4_raster */
#define P4_R_STENCIL 1
#define P4_R_STENCIL_W 2
#define P4_R_AA_LINES 4
#define P4_R_AA_POINTS 8
#define RASTER_STENCIL(c) ((c)->p4_raster & P4_R_STENCIL)
#define RASTER_STENCIL_W(c) ((c)->p4_raster & P4_R_STENCIL_W)
#define RASTER_AA_LINES(c) ((c)->p4_raster & P4_R_AA_LINES)
#define RASTER_AA_POINTS(c) ((c)->p4_raster & P4_R_AA_POINTS)

/* s31 (phase 3a G14): hidden in the declaration too, so every entry point
   reaches it PC-relative (auipc + lw) instead of through the GOT (one
   more load); nothing outside libGL.so names it */
extern GLContext *gl_ctx __attribute__((visibility("hidden")));

void gl_add_op(GLParam *p);
/* s31: while compiling, hand a gl_malloc'd block to the list being built;
   it is freed with the list. 0 = not compiling (the caller keeps it). */
int gl_list_own(GLContext *c, void *block);

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
/* s31_zepoch.c (phase 3a G03): depth epochs */
void zep_guard(GLContext *c);
void zep_sync(GLContext *c);
void zep_track_target(GLContext *c);
void zep_attach(GLContext *c);
void zep_materialise(GLContext *c);
void zep_demote(GLContext *c);
unsigned int zep_prim(GLContext *c, unsigned int m);
unsigned int zep_clear_value(GLContext *c, float cd);
unsigned int zep_clear_rect_value(GLContext *c, float cd);
float zep_depth(const ZBuffer *zb, unsigned int s);
int zep_clear(GLContext *c, float cd);
/* phase 3a dirty boxes (s31_zepoch.c) */
extern unsigned int tgl_bind_serial;
void zdb_grow(GLContext *c, int x0, int y0, int x1, int y1);
void zdb_reset(GLContext *c);
void zdb_clear_colour(GLContext *c, unsigned int v);
void zdb_fold(GLContext *c, int colour_reset, int depth_reset);
void zdb_rebind(GLContext *c);
void zdb_invalidate(ZBuffer *zb);
/* s31_stencil.c (phase 4 F8-STENCIL) */
void zst_attach(ZBuffer *zb, int own);
void zst_mark_zero(unsigned char *sbuf, int npix);
void zst_touch_all(ZBuffer *zb);
void zst_touch(ZBuffer *zb, int x0, int y0, int x1, int y1);
void zst_clear(GLContext *c, int x0, int y0, int x1, int y1, int full);
void zst_put(ZBuffer *zb, int x, int row, unsigned int v, unsigned int wm);
unsigned int *zst_unpack(GLContext *c, int w, int h, int type, const void *pixels, int *err);
void zst_read(GLContext *c, int x, int y, int w, int h, int type, void *pixels);
void gl_warn_once(const char *what);
/* s31: "libGL: approximated <what>", once: an honoured feature drawn by a
   documented approximation (not a gap: tools/glref does not count it) */
void gl_note_once(const char *what);
/* s31_state.c */
int s31_cap_index(int cap);          /* -1: not a GL capability */
void s31_cap_record(GLContext *c, int cap, int v);
int s31_cap_get(GLContext *c, int cap);
void s31_state_init(GLContext *c);
void s31_cap_swap_unit(GLContext *c, unsigned int *bits);   /* phase 5 O1 */
int s31_cap_is_unit(int cap);
/* s31_mtex.c (phase 5 O1): exchange the ACTIVE-unit state (GLTexUnit) of
   the context fields and tu1 / the client texture-coordinate array state */
void tu_swap(GLContext *c);
void tc_swap(GLContext *c);
void gl_mtex_init(GLContext *c);
void gl_mtex_free(GLContext *c);
int s31_client_state(GLContext *c, int array);   /* arrays.c; -1 = not one */

/* s31: a vertex colour in the rasteriser's ZBufferPoint scale, clamped as
   glColor4f clamps it (api.c). Used where a vertex's zp colour is not
   already right: clipped vertices, and the provoking vertex of GL_FLAT. */
static inline int gl_zp_chan(float v, int lo, int hi)
{
  /* s31 (phase 3a G14): fmax.s/fmin.s instead of two branches; the same
     integer for every number (0 -> lo, 1 -> hi exactly) */
  return (int)(fminf(fmaxf(v, 0.0f), 1.0f) * (hi - lo) + lo);
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

/* raster.c */
void gl_update_raster(GLContext *c);
void gl_draw_triangle_general(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2);
/* phase 5 O1: the general path with texture unit 1 on */
void gl_draw_triangle_mt(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2);
void gl_draw_triangle_modwhite(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2);
/* s31_tfilter.c (phase 4 F-PERSP): smooth tier 1 with the perspective-colour test */
void gl_draw_triangle_fill_pq(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2);
void gl_draw_triangle_offset(GLContext *c, GLVertex *p0, GLVertex *p1, GLVertex *p2);
/* lines and points of the general path; the colour of a flat line is the
   provoking vertex's (c->flat_vtx) */
void gl_general_line(GLContext *c, GLVertex *a, GLVertex *b, int flat);
void gl_general_point(GLContext *c, GLVertex *p);
/* texture.c */
int gl_texture_complete(const GLTexture *t);
void gl_tex_free_mip(GLTexture *t);       /* phase 4: the stored levels > 0 */
/* vertex.c: GL fog factor of a vertex */
void gl_vertex_fog(GLContext *c, GLVertex *v);

/* s31_xform.c (plan F7) */
void gl_vertex_extra(GLContext *c, GLVertex *v);     /* vtx_extra: fog, clip planes */
void gl_vertex_texcoord(GLContext *c, GLVertex *v);  /* texgen and/or texture matrix */
int gl_user_clipcode(const GLContext *c, const V4 *pc);  /* bits 6.. */
void gl_update_xform(GLContext *c);   /* glBegin, matrices changed: planes, texgen */
void gl_tu1_begin(GLContext *c);      /* phase 5 O1: the same for texture unit 1 */
void gl_texgen_coords(GLContext *c, const V4 *obj, const V4 *eye,
                      const V3 *en, const V4 *in, V4 *out);
#define TGL_CLIP_USER_SHIFT 6
/* 64 (9,984 B with the tags, allocated at a context's first glDrawElements):
   the 33-wide bench grid (gl/bench geo3) runs 34% fewer instructions than
   with no cache (4.03 -> 2.65 M a frame); 32 entries gave only -6%
   (3.78 M), because a grid row no longer fits and the next row misses
   (LEVERS.md G14) */
#ifndef TGL_VCACHE
#define TGL_VCACHE 64           /* power of two; 148 B each */
#endif
/* the zero pattern of a 4x4 matrix (row-major), vertex.c */
#define TGL_XF_GENERAL 0
#define TGL_XF_PERSP   1   /* glFrustum's: rows (a 0 b 0)(0 c d 0)(0 0 e f)(0 0 g 0) */
#define TGL_XF_ORTHO   2   /* glOrtho's:   rows (a 0 0 b)(0 c 0 d)(0 0 e f)(0 0 0 g) */
/* texture.c */
GLTexture *gl_tex_target(GLContext *c, int target);  /* bound object, NULL: bad target */

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
void gl_color_material(GLContext *c, float r, float g, float b, float a);
/* vertex.c (phase 3a G14): glopVertex for glDrawElements' vertex cache */
void gl_vertex_indexed(GLContext *c, GLParam *p, const GLVertex *hit, GLVertex *save);
void gl_vertex_indexed_mt(GLContext *c, GLParam *p, const GLVertex *hit, GLVertex *save);
void gl_vertex4f(float x, float y, float z, float w, GLContext *c);  /* glopVertex's body */

void glInitTextures(GLContext *c);
void glEndTextures(GLContext *c);
GLTexture *alloc_texture(GLContext *c,int h);
GLTexture *alloc_texture_detached(void);   /* s31: not in the name table */
void free_texture_detached(GLTexture *t);

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
