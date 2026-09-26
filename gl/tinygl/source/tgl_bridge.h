/*
 * tgl_bridge.h - the only interface between the Khronos ABI layer (gl/api,
 * compiled against the standard GL/gl.h) and the TinyGL core (compiled
 * against TinyGL's own include/GL/gl.h). s31, MIT.
 *
 * Plain C types only, so both sides can include it. zgl.h includes it too,
 * which makes the compiler check every declaration here against TinyGL's
 * own definitions. Enum VALUES are shared: gl/api/enumcmp.py proves that
 * every GL_* in TinyGL's header has its Khronos value.
 *
 * Nothing here is exported from libGL.so (TinyGL is built with
 * -fvisibility=hidden).
 */
#ifndef TGL_BRIDGE_H
#define TGL_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- TinyGL entry points (renamed by include/GL/tgl_rename.h) ---- */
void tgl_glEnable(int cap);
void tgl_glDisable(int cap);
void tgl_glShadeModel(int mode);
void tgl_glCullFace(int mode);
void tgl_glFrontFace(int mode);
void tgl_glPolygonMode(int face, int mode);
void tgl_glBegin(int mode);
void tgl_glEnd(void);
void tgl_glVertex4f(float x, float y, float z, float w);
void tgl_glNormal3f(float x, float y, float z);
void tgl_glColor4f(float r, float g, float b, float a);
void tgl_glTexCoord4f(float s, float t, float r, float q);
void tgl_glEdgeFlag(int flag);
void tgl_glMatrixMode(int mode);
void tgl_glLoadMatrixf(const float *m);
void tgl_glLoadIdentity(void);
void tgl_glMultMatrixf(const float *m);
void tgl_glPushMatrix(void);
void tgl_glPopMatrix(void);
void tgl_glRotatef(float angle, float x, float y, float z);
void tgl_glTranslatef(float x, float y, float z);
void tgl_glScalef(float x, float y, float z);
void tgl_glViewport(int x, int y, int width, int height);
void tgl_glFrustum(float l, float r, float b, float t, float n, float f);
void tgl_glOrtho(float l, float r, float b, float t, float n, float f);
unsigned int tgl_glGenLists(int range);
int tgl_glIsList(unsigned int list);
void tgl_glNewList(unsigned int list, int mode);
void tgl_glEndList(void);
void tgl_glCallList(unsigned int list);
void tgl_glDeleteLists(unsigned int list, int range);
void tgl_glClear(int mask);
void tgl_glClearColor(float r, float g, float b, float a);
void tgl_glClearDepth(float depth);
int tgl_glRenderMode(int mode);
void tgl_glSelectBuffer(int size, unsigned int *buf);
void tgl_glInitNames(void);
void tgl_glPushName(unsigned int name);
void tgl_glPopName(void);
void tgl_glLoadName(unsigned int name);
void tgl_glGenTextures(int n, unsigned int *textures);
void tgl_glDeleteTextures(int n, const unsigned int *textures);
void tgl_glBindTexture(int target, int texture);
void tgl_glTexImage2D(int target, int level, int components, int width,
                      int height, int border, int format, int type,
                      void *pixels);
void tgl_glTexSubImage2D(int target, int level, int xoffset, int yoffset,
                         int width, int height, int format, int type,
                         const void *pixels);
void tgl_glTexEnvi(int target, int pname, int param);
void tgl_glTexParameteri(int target, int pname, int param);
void tgl_glPixelStorei(int pname, int param);
void tgl_glMaterialfv(int mode, int type, float *v);
void tgl_glMaterialf(int mode, int type, float v);
void tgl_glColorMaterial(int mode, int type);
void tgl_glLightfv(int light, int type, float *v);
void tgl_glLightf(int light, int type, float v);
void tgl_glLightModeli(int pname, int param);
void tgl_glLightModelfv(int pname, float *param);
void tgl_glHint(int target, int mode);
void tgl_glEnableClientState(int array);
void tgl_glDisableClientState(int array);
void tgl_glArrayElement(int i);
void tgl_glVertexPointer(int size, int type, int stride, const void *pointer);
void tgl_glColorPointer(int size, int type, int stride, const void *pointer);
void tgl_glNormalPointer(int type, int stride, const void *pointer);
void tgl_glTexCoordPointer(int size, int type, int stride, const void *pointer);
void tgl_glDrawElements(int mode, int count, int type, const void *indices);
void tgl_glDrawArrays(int mode, int first, int count);
void tgl_glPolygonOffset(float factor, float units);

/* ---- s31 additions (source/s31_*.c) ---- */

/* contexts: s31_ctx.c. A context is a TinyGL GLContext; the caller's
   colour buffer is rendered into directly (RGB565, pitch in bytes). */
void *tgl_ctx_create(void *share);
void tgl_ctx_destroy(void *ctx);
int tgl_ctx_bind(void *ctx, void *pixels, int width, int height, int pitch);
int tgl_ctx_set_scale(void *ctx, int shift);   /* render scale, s31gl.h */
void tgl_ctx_set_prepare(void *ctx, int (*prepare)(void *user), void *user);
void tgl_ctx_make_current(void *ctx);  /* NULL: a context that draws nothing */
void *tgl_ctx_current(void);           /* NULL when none is current */
int tgl_ctx_depth_bytes(void *ctx);    /* bytes of depth allocated now */
void tgl_ctx_release_depth(void *ctx);
int tgl_ctx_bind_depth(void *ctx, void *depth); /* caller-owned depth, NULL: private */
void tgl_ctx_set_stencil_bits(void *ctx, int bits); /* phase 4 F8: 0 or 8 */
int tgl_ctx_bind_stencil(void *ctx, void *stencil); /* caller-owned stencil, NULL: private */
void tgl_stencil_zeroed(void *stencil, int w, int h); /* s31gl_stencil_zeroed */
int tgl_ctx_stencil_bytes(void *ctx);
void tgl_ctx_set_doublebuffer(void *ctx, int on);
void tgl_ctx_set_retained(void *ctx, int on);  /* phase 3a dirty boxes */
void tgl_ctx_arm(void *ctx);            /* frame hook before the next access */

/* glPush/PopAttrib and glPush/PopClientAttrib: the stacks live in the
   context (they are per-context state) but are built by the ABI layer
   (gl/api/gl_pushattrib.c) from ordinary glGet and set calls. Every node
   starts with its `next` pointer and owns no other allocation, so the
   context frees a stack with free() node by node. */
struct tgl_attrib_slots {
  void *attrib_top, *client_top;
  int attrib_depth, client_depth;
};
struct tgl_attrib_slots *tgl_attrib_slots(void); /* current context; NULL if none */

/* errors (GL_NO_ERROR = 0) */
int tgl_get_error(void);               /* returns and clears */
void tgl_set_error(int error);
int tgl_in_begin(void);
int tgl_compiling(void);               /* inside glNewList */
void tgl_warn_once(const char *what);  /* "libGL: unimplemented <what>", once */

/* generic state setter, list-compilable; codes below */
void tgl_state_i(int code, int a, int b, int c, int d);
void tgl_state_f(int code, float a, float b, float c, float d);
enum {
  S31_ST_DEPTH_FUNC = 1,   /* i: func */
  S31_ST_DEPTH_MASK,       /* i: flag */
  S31_ST_DEPTH_RANGE,      /* f: near, far */
  S31_ST_BLEND_FUNC,       /* i: src, dst */
  S31_ST_BLEND_COLOR,      /* f: r g b a */
  S31_ST_ALPHA_FUNC,       /* i: func, f-bits of ref in b (use tgl_state_alpha) */
  S31_ST_COLOR_MASK,       /* i: r g b a */
  S31_ST_SCISSOR,          /* i: x y w h */
  S31_ST_LINE_WIDTH,       /* f */
  S31_ST_POINT_SIZE,       /* f */
  S31_ST_LINE_STIPPLE,     /* i: factor, pattern */
  S31_ST_FOG_MODE,         /* i */
  S31_ST_FOG_DENSITY,      /* f */
  S31_ST_FOG_START,        /* f */
  S31_ST_FOG_END,          /* f */
  S31_ST_FOG_INDEX,        /* f */
  S31_ST_FOG_COLOR,        /* f: r g b a */
  S31_ST_LOGIC_OP,         /* i */
  S31_ST_STENCIL_FUNC,     /* i: func, ref, mask */
  S31_ST_STENCIL_OP,       /* i: fail, zfail, zpass */
  S31_ST_STENCIL_MASK,     /* i */
  S31_ST_STENCIL_CLEAR,    /* i */
  S31_ST_DRAW_BUFFER,      /* i */
  S31_ST_READ_BUFFER,      /* i */
  S31_ST_TEXENV_COLOR,     /* f: r g b a */
  S31_ST_LIST_BASE,        /* i */
  S31_ST_PIXEL_ZOOM,       /* f: xfactor, yfactor */
  S31_ST_PIXEL_TRANSFER,   /* pname in a, value in b (tgl_state_pf) */
  S31_ST_BLEND_FUNC_SEP,   /* i: src rgb, dst rgb, src alpha, dst alpha (phase 4) */
  S31_ST_BLEND_EQ,         /* i: rgb, alpha (phase 4) */
};
void tgl_state_alpha(int func, float ref);
void tgl_state_pf(int code, int pname, float v);   /* p[2].i = pname, p[3].f = v */

/* queries: s31_get.c */
enum { TGL_GET_INT = 0, TGL_GET_FLOAT = 1, TGL_GET_COLOR = 2 };
/* Fills iv[] and fv[] (16 each) for pname. Returns the number of values,
   or -1 when pname is not a known glGet name. *kind says which array is
   authoritative (TGL_GET_COLOR: float, maps to the full int range). */
int tgl_get(int pname, int *iv, float *fv, int *kind);
int tgl_is_enabled(int cap);           /* 0/1, -1 = not a capability */
int tgl_is_texture(unsigned int name);
int tgl_get_light(int light, int pname, float *v);        /* count or -1 */
int tgl_get_material(int face, int pname, float *v);      /* count or -1 */
int tgl_get_tex_env(int target, int pname, int *iv, float *fv, int *kind);
int tgl_get_tex_parameter(int target, int pname, int *iv, float *fv, int *kind);
int tgl_get_tex_level_parameter(int target, int level, int pname, int *iv);
void tgl_tex_parameterf(int target, int pname, const float *v, int n);
void *tgl_get_pointer(int pname);      /* glGetPointerv */
void tgl_edge_flag_pointer(int stride, const void *pointer);

/* ---- plan F7: s31_xform.c, s31_draw.c, texture.c ---- */
/* glRasterPos (window 0: object coordinates) and glWindowPos (window 1) */
void tgl_raster_pos(float x, float y, float z, float w, int window);
/* glPush/PopAttrib: 18 floats, pos[4] colour[4] texcoord[4] distance valid,
   then (phase 5 O1) texture unit 1's raster texcoord[4] */
void tgl_raster_state(float *v, int set);
void tgl_clip_plane(int plane, const float *eq);
int tgl_get_clip_plane(int plane, float *eq);             /* 4 or -1 */
/* v NULL: pname GL_TEXTURE_GEN_MODE with iparam */
void tgl_tex_gen(int coord, int pname, int iparam, const float *v);
int tgl_get_tex_gen(int coord, int pname, float *v);      /* count or -1 */
void tgl_polygon_stipple(const unsigned char *mask);      /* unpacked per GL_UNPACK_* */
void tgl_get_polygon_stipple(unsigned char *mask);        /* packed per GL_PACK_* */
void tgl_bitmap(int w, int h, float xorig, float yorig, float xmove,
                float ymove, const unsigned char *bits);
void tgl_draw_pixels(int w, int h, int format, int type, const void *pixels);
void tgl_copy_pixels(int x, int y, int w, int h, int type);
void tgl_read_pixels(int x, int y, int w, int h, int format, int type, void *pixels);
/* glCopyTexImage1D/2D (sub 0) and glCopyTexSubImage1D/2D (sub 1) */
void tgl_copy_tex(int target, int level, int ifmt, int x, int y, int w, int h,
                  int border, int xoff, int yoff, int sub);
void tgl_get_tex_image(int target, int level, int format, int type, void *pixels);
int tgl_pixel_transfer_get(int pname, float *v);          /* 1 or -1 */

/* ---- phase 5 O1: GL_ARB_multitexture, GL_ARB_texture_env_combine
   (s31_mtex.c, texture.c) ---- */
int tgl_max_texture_units(void);        /* 2, or 1 under S31GL_MTEX=0 */
void tgl_active_texture(int texture);   /* GL_TEXTUREi */
void tgl_client_active_texture(int texture);
void tgl_multi_tex_coord(int target, float s, float t, float r, float q);
/* glTexEnv with a float value (GL_RGB_SCALE, GL_ALPHA_SCALE) */
void tgl_tex_envf(int target, int pname, float v);
void tgl_fused_stats(unsigned int out[8]);   /* zpipe_fused.c: s31gl_fused_stats */

#ifdef __cplusplus
}
#endif
#endif /* TGL_BRIDGE_H */
