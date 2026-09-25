/*
 * glx_core.c - the only file in gl/glx/ that calls the rasteriser core.
 * MIT.
 *
 * If the core's API (gl/api/s31gl.h) differs from s31gl_iface.h, this is the
 * file to adapt.
 */
#include "glx_int.h"

/*
 * gl/api/s31gl.h (through s31gl_iface.h) is the authority: a mismatch in any
 * prototype is a compile error here rather than an ABI bug.
 *
 * Review fix (2026-09-25): the core's extras (set_doublebuffer,
 * release_depth) used to be called only under an #ifdef keyed on
 * S31GL_IFACE_FROM_CORE, a macro the integrated s31gl_iface.h no longer
 * defines - so both were silently compiled out: GL_DOUBLEBUFFER and the
 * default GL_DRAW_BUFFER never followed the config, and a context's depth
 * buffer was never given back at unbind. They are called unconditionally
 * now.
 */

s31gl_ctx *glxi_core_create(s31gl_ctx *share, struct __GLXcontextRec *c)
{
	struct s31gl_hooks h;
	/* The core prints the one "libGL: context N created" line per
	 * context (plan 4.1); GLX does not print a second. */
	s31gl_ctx *ctx = s31gl_create_context(share);

	if (!ctx)
		return NULL;
	/* GL_DOUBLEBUFFER and the default GL_DRAW_BUFFER follow the config */
	s31gl_set_doublebuffer(ctx, c->db);
	h.user = c;
	h.frame_begin = glxi_hook_frame_begin;
	h.viewport = glxi_hook_viewport;
	h.flush = glxi_hook_flush;
	s31gl_set_hooks(ctx, &h);
	return ctx;
}

void glxi_core_destroy(s31gl_ctx *ctx)
{
	if (ctx)
		s31gl_destroy_context(ctx);
}

int glxi_core_make_current(s31gl_ctx *ctx)
{
	return s31gl_make_current(ctx);
}

int glxi_core_bind(s31gl_ctx *ctx, void *p, int w, int h, int pitch)
{
	return s31gl_bind_color(ctx, p, w, h, pitch);
}

void glxi_core_frame_end(s31gl_ctx *ctx)
{
	s31gl_frame_end(ctx);
}

void glxi_core_finish(s31gl_ctx *ctx)
{
	s31gl_finish(ctx);
}

/* The context is no longer bound to the drawable: forget the drawable's
 * depth buffer (or free a private one) until the next bind. */
void glxi_core_release_depth(s31gl_ctx *ctx)
{
	if (ctx)
		s31gl_release_depth(ctx);
}

/* Caller-owned depth (the drawable's); NULL: the core's own, lazily. */
int glxi_core_bind_depth(s31gl_ctx *ctx, void *depth)
{
	return ctx ? s31gl_bind_depth(ctx, depth) : -1;
}

void *glxi_core_get_proc(const char *name)
{
	return s31gl_get_proc(name);
}
