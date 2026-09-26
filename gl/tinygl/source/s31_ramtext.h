/*
 * s31_ramtext.h - lever L1 (plan 6, stage 3b, mechanism (a)): the hot
 * rasteriser copied from XIP flash into RAM. s31, MIT.
 *
 * The hot functions (gl/api/ramtext.list) are linked contiguously in their
 * own output section (gl/api/ramtext.ld). With S31GL_RAMTEXT=1, the first
 * context creation copies that range into RAM, fixes the PC-relative
 * references that leave it, and makes the copy RX (s31_ramtext.c). The
 * code enters the copy only through the pointers below: every other call
 * site keeps calling the XIP copy, which stays the fallback and is what
 * runs with S31GL_RAMTEXT=0 (the default) or when the copy is unavailable.
 *
 * Every entry is selected at state-change time (glBegin's raster update,
 * list/print mode changes), never tested per vertex or per pixel.
 */
#ifndef S31_RAMTEXT_H
#define S31_RAMTEXT_H

#include "zgl.h"

typedef void (*s31_vtx_fn)(float x, float y, float z, float w, GLContext *c);

struct s31_rt {
  /* raster_sel.c: the fillers for GL_LEQUAL (ztriangle.c) and strict
     GL_LESS with the depth write (ztriangle_lt.c) */
  ZB_fillTriangleFunc flat, smooth, map;
  ZB_fillTriangleFunc flat_lt, smooth_lt, map_lt;
  /* raster_sel.c: the GL_FILL triangle entry points */
  gl_draw_triangle_func draw_fill, draw_fill_pq;
  /* api.c glVertex (through GLContext.vtx_run) and glCallList */
  s31_vtx_fn vertex4f;
  void (*call_list)(GLContext *c, GLParam *p);
};

extern struct s31_rt s31_rt __attribute__((visibility("hidden")));

/* once, at the first context creation (s31_ctx.c), before any context
   exists: every GLContext copies what it needs from s31_rt */
void s31_ramtext_init(void) __attribute__((visibility("hidden")));

/* glVertex's executing path (api.c): NULL while compiling or printing */
static inline void gl_update_vtx_run(GLContext *c)
{
  c->vtx_run = (c->compile_flag | c->print_flag) ? NULL : s31_rt.vertex4f;
}

#endif
