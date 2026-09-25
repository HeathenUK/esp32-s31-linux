/*
 * s31_api.h - internal header of the Khronos ABI layer (every .c in gl/api). s31, MIT.
 *
 * Every file here compiles against the STANDARD headers (gl/include/GL,
 * identical to Mesa's) and talks to TinyGL only through tgl_bridge.h.
 */
#ifndef S31_API_H
#define S31_API_H

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stddef.h>
#include "tgl_bridge.h"
#include "s31_float.h"

/* record a GL error (first one sticks until glGetError) */
#define S31_ERR(e) tgl_set_error(e)

/* "libGL: unimplemented <name>" once per name */
void s31_unimpl(const char *name);

/* frame/viewport/flush hooks of the current context (context.c) */
void s31_hook_viewport(int x, int y, int w, int h);
void s31_hook_flush(int finish);

#endif
