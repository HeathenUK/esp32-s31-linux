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

/* phase 6: one address per hot function at run time. Once the copy is
   made, every pointer to a hot function the library holds must be the RAM
   copy's, or pointer equality (zpf_select's stage matching) breaks and the
   code runs from XIP. Data words are rewritten by s31_ramtext.c; code does
   the rest with these (gl/api/ramtext.py's build line counts the cold
   call sites and address-takes that need them):
     S31_RT_RAM(f)   an address to store: the copy's when there is one
     S31_RT_XIP(f)   an address to compare in cold code: the XIP one
     S31_RT_ENTER(f, args) / S31_RT_ENTER_V(f, args): first statement of a
                     hot function that cold code calls directly - the XIP
                     copy re-enters the RAM one (same code) */
#if !defined(S31GL_NO_RAMTEXT) && defined(__linux__) && ((defined(__riscv) && __riscv_xlen == 32) || defined(__aarch64__))
#define S31_RT_COPY 1
#include <stdint.h>
extern intptr_t s31_rt_d __attribute__((visibility("hidden")));   /* 0: no copy */
/* the XIP range as data, NOT __s31hot_start: a pc-relative reference to the
   range's own start from inside the range moves with the copy, so in RAM
   it would name the RAM range */
extern uintptr_t s31_rt_lo __attribute__((visibility("hidden")));
extern uintptr_t s31_rt_len __attribute__((visibility("hidden")));
#define S31_RT_INXIP(f) ((uintptr_t)(f) - s31_rt_lo < s31_rt_len)
#define S31_RT_RAM(f) (s31_rt_d && S31_RT_INXIP(f) ? \
                       (__typeof__(&*(f)))((uintptr_t)(f) + s31_rt_d) : (f))
#define S31_RT_XIP(f) (s31_rt_d && S31_RT_INXIP((uintptr_t)(f) - s31_rt_d) ? \
                       (__typeof__(&*(f)))((uintptr_t)(f) - s31_rt_d) : (f))
/* where this code runs: the pc itself, never &f (a static link or the GOT
   can hand back either copy's address for f, and a test on that recursed) */
static inline __attribute__((always_inline)) uintptr_t s31_rt_pc(void)
{
  uintptr_t pc;
#if defined(__riscv)
  __asm__ volatile ("auipc %0, 0" : "=r"(pc));
#else
  __asm__ volatile ("adr %0, ." : "=r"(pc));
#endif
  return pc;
}
#define S31_RT_ENTER(f, ...) do { if (s31_rt_d && S31_RT_INXIP(s31_rt_pc())) \
    return S31_RT_RAM(f)(__VA_ARGS__); } while (0)
#define S31_RT_ENTER_V(f, ...) do { if (s31_rt_d && S31_RT_INXIP(s31_rt_pc())) { \
    S31_RT_RAM(f)(__VA_ARGS__); return; } } while (0)
#else
#define S31_RT_RAM(f) (f)
#define S31_RT_XIP(f) (f)
#define S31_RT_ENTER(f, ...) do { } while (0)
#define S31_RT_ENTER_V(f, ...) do { } while (0)
#endif

/* once, at the first context creation (s31_ctx.c), before any context
   exists: every GLContext copies what it needs from s31_rt */
void s31_ramtext_init(void) __attribute__((visibility("hidden")));

/* glVertex's executing path (api.c): NULL while compiling or printing */
static inline void gl_update_vtx_run(GLContext *c)
{
  c->vtx_run = (c->compile_flag | c->print_flag) ? NULL : s31_rt.vertex4f;
}

#endif
