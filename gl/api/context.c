/*
 * context.c - s31gl_* (see s31gl.h), glFlush, glFinish. s31, MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "s31_api.h"
#include "s31gl.h"

struct s31gl_ctx {
	void *tgl;			/* TinyGL GLContext */
	struct s31gl_hooks hooks;
	int id;
};

static s31gl_ctx *current;
static int ncreated;

static int frame_begin(void *user)
{
	s31gl_ctx *c = user;
	if (c->hooks.frame_begin)
		c->hooks.frame_begin(c->hooks.user);
	return 0;
}

static void log_context(int id)
{
	char comm[32] = "?";
	FILE *f = fopen("/proc/self/comm", "r");
	if (f) {
		if (fgets(comm, sizeof(comm), f)) {
			size_t n = strlen(comm);
			if (n && comm[n - 1] == '\n')
				comm[n - 1] = 0;
		}
		fclose(f);
	}
	fprintf(stderr, "libGL: context %d created (pid %d, comm %s)\n", id,
		(int)getpid(), comm);
}

s31gl_ctx *s31gl_create_context(s31gl_ctx *share)
{
	s31gl_ctx *c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->tgl = tgl_ctx_create(share ? share->tgl : NULL);
	if (c->tgl == NULL) {
		free(c);
		return NULL;
	}
	tgl_ctx_set_prepare(c->tgl, frame_begin, c);
	c->id = ++ncreated;
	log_context(c->id);
	return c;
}

void s31gl_destroy_context(s31gl_ctx *ctx)
{
	if (ctx == NULL)
		return;
	if (ctx == current)
		s31gl_make_current(NULL);
	tgl_ctx_destroy(ctx->tgl);
	free(ctx);
}

int s31gl_make_current(s31gl_ctx *ctx)
{
	current = ctx;
	tgl_ctx_make_current(ctx ? ctx->tgl : NULL);
	return 0;
}

int s31gl_set_render_scale(s31gl_ctx *ctx, int shift)
{
	return ctx ? tgl_ctx_set_scale(ctx->tgl, shift) : -1;
}

int s31gl_bind_color(s31gl_ctx *ctx, void *pixels, int w, int h, int pitch)
{
	if (ctx == NULL)
		return -1;
	return tgl_ctx_bind(ctx->tgl, pixels, w, h, pitch);
}

void s31gl_set_hooks(s31gl_ctx *ctx, const struct s31gl_hooks *hooks)
{
	if (ctx == NULL)
		return;
	if (hooks)
		ctx->hooks = *hooks;
	else
		memset(&ctx->hooks, 0, sizeof(ctx->hooks));
}

void s31gl_frame_end(s31gl_ctx *ctx)
{
	if (ctx)
		tgl_ctx_arm(ctx->tgl);
}

void s31gl_finish(s31gl_ctx *ctx)
{
	(void)ctx;		/* TinyGL rasterises synchronously */
}

s31gl_ctx *s31gl_get_current(void)
{
	return current;
}

void s31gl_set_retained(s31gl_ctx *ctx, int retained)
{
	if (ctx)
		tgl_ctx_set_retained(ctx->tgl, retained);
}

void s31gl_set_doublebuffer(s31gl_ctx *ctx, int doublebuffer)
{
	if (ctx)
		tgl_ctx_set_doublebuffer(ctx->tgl, doublebuffer);
}

int s31gl_depth_bytes(s31gl_ctx *ctx)
{
	return ctx ? tgl_ctx_depth_bytes(ctx->tgl) : 0;
}

void s31gl_release_depth(s31gl_ctx *ctx)
{
	if (ctx)
		tgl_ctx_release_depth(ctx->tgl);
}

int s31gl_bind_depth(s31gl_ctx *ctx, void *depth)
{
	return ctx ? tgl_ctx_bind_depth(ctx->tgl, depth) : -1;
}

void s31gl_set_stencil_bits(s31gl_ctx *ctx, int bits)
{
	if (ctx)
		tgl_ctx_set_stencil_bits(ctx->tgl, bits);
}

int s31gl_bind_stencil(s31gl_ctx *ctx, void *stencil)
{
	return ctx ? tgl_ctx_bind_stencil(ctx->tgl, stencil) : -1;
}

void s31gl_stencil_zeroed(void *stencil, int w, int h)
{
	tgl_stencil_zeroed(stencil, w, h);
}

int s31gl_stencil_bytes(s31gl_ctx *ctx)
{
	return ctx ? tgl_ctx_stencil_bytes(ctx->tgl) : 0;
}

void s31gl_fused_stats(unsigned int out[8])
{
	tgl_fused_stats(out);
}

void s31_hook_viewport(int x, int y, int w, int h)
{
	if (current && current->hooks.viewport)
		current->hooks.viewport(current->hooks.user, x, y, w, h);
}

void s31_hook_flush(int finish)
{
	if (current && current->hooks.flush)
		current->hooks.flush(current->hooks.user, finish);
}

void s31_unimpl(const char *name)
{
	tgl_warn_once(name);
}

void GLAPIENTRY glFlush(void)
{
	s31_hook_flush(0);
}

void GLAPIENTRY glFinish(void)
{
	s31_hook_flush(1);
}
