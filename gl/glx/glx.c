/*
 * glx.c - client-side GLX 1.4 for the S31 libGL. MIT.
 *
 * Everything is direct and client side (docs/gl-plan-2026-09-25.md 4.1):
 * no GLX protocol is ever sent, so it works the same on stock libX11 (host
 * rig, Xvfb) and on xlite (the board), with plain Xlib + MIT-SHM calls only.
 *
 * Contexts, current state, SwapBuffers, the extension strings and
 * glXGetProcAddress live here; visuals/FBConfigs in glx_config.c; drawables
 * and the present in glx_present.c; the core glue in glx_core.c.
 *
 * Threading: see glx_int.h. One current context per process; GLX calls from
 * one thread at a time.
 *
 * Runtime toggles (platform A/B switches, never per-app steering):
 *   S31GL_TRACE=1     per-drawable present/wait counts on stderr at exit
 *   S31GL_NOSHM=1     present with XPutImage instead of MIT-SHM
 *   S31GL_SHMBUFS=1|2 force one or two ping-pong SHM segments per
 *                     double-buffered drawable; unset, two when a segment is
 *                     at most 200 kB (glx_present.c want_bufs(): board
 *                     glxgears +18% windowed, +32% render-scaled fullscreen)
 *   S31GL_RENDER_SCALE=0  never ask the server to take a panel-size window
 *                     at half size (plan G04; on by default, glx_present.c)
 *
 * Window size is read (XGetGeometry, one round trip) at MakeCurrent when the
 * context/drawable pair changes, and from glViewport - the Mesa xlib
 * convention - only when the rectangle is not one of the last GLXI_NVP
 * checked since the size last changed. An app that is resized and never
 * calls glViewport keeps drawing at the old size, as it would on Mesa's xlib
 * driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "glx_int.h"

/*
 * GLX_EXT_visual_rating is deliberately NOT listed, though the configs still
 * answer GLX_CONFIG_CAVEAT (GLX 1.3 core) honestly with GLX_SLOW_CONFIG.
 * Listing it made SDL add GLX_VISUAL_CAVEAT_EXT to its request whenever an
 * app set SDL_GL_ACCELERATED_VISUAL (SDL 1.2 always asks for NONE,
 * SDL_x11gl.c:188-192; SDL2 asks NONE for 1 and SLOW for 0), and a SLOW
 * config then matched nothing: no visual, the app stops (SDL testgl -accel,
 * foobillardplus). Without the extension SDL never sends the attribute.
 * Reporting caveat NONE instead, as Mesa's swrast does, would have fixed
 * NONE and broken SDL2's accelerated=0 request (review finding
 * slow-caveat-with-visual-rating).
 */
#define GLXI_EXTENSIONS \
	"GLX_ARB_get_proc_address " \
	"GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control"
#define GLXI_VENDOR	"S31 libGL (TinyGL, client-side)"
#define GLXI_VERSION	"1.4"

static struct __GLXcontextRec *cur;	/* the current context, or NULL */
static Display *cur_dpy;
static int ctx_count;

int glxi_trace(void)
{
	static int t = -1;

	if (t < 0)
		t = getenv("S31GL_TRACE") != NULL;
	return t;
}

/* ------------------------------------------------------------- contexts */

static GLXContext make_context(Display *dpy, int screen, VisualID vid, int db,
			       int stencil, int fbid, GLXContext share)
{
	struct __GLXcontextRec *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	c->dpy = dpy;
	c->screen = screen;
	c->vid = vid;
	c->db = db;
	c->stencil = stencil;
	c->fbconfig_id = fbid;
	c->core = glxi_core_create(share ? share->core : NULL, c);
	if (!c->core) {
		free(c);
		return NULL;
	}
	c->id = ++ctx_count;
	return c;
}

GLXI_EXPORT GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis,
					GLXContext shareList, Bool direct)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);
	struct __GLXFBConfigRec *cfg;
	int db;

	(void)direct;		/* always direct */
	if (!d || !vis)
		return NULL;
	db = glxi_visual_db(d, vis->visualid);
	cfg = glxi_cfg_for_visual(d, vis->screen, vis->visualid, db,
				  glxi_visual_stencil(d, vis->visualid));
	if (!cfg)
		return NULL;	/* not a GL visual (BadValue in real GLX) */
	return make_context(dpy, vis->screen, vis->visualid, db, cfg->stencil,
			    cfg->id, shareList);
}

GLXI_EXPORT GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config,
					   int renderType, GLXContext shareList,
					   Bool direct)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);

	(void)direct;
	if (!d || !glxi_is_our_cfg(d, config))
		return NULL;
	if (renderType != GLX_RGBA_TYPE)
		return NULL;	/* no colour-index rendering */
	return make_context(dpy, config->screen, config->vid, config->db,
			    config->stencil, config->id, shareList);
}

static void context_free(struct __GLXcontextRec *c)
{
	glxi_core_destroy(c->core);
	free(c);
}

/* Point the core at the drawable's buffers: colour, and the depth buffer
 * every context current on the drawable shares. With no pixels yet only the
 * size is passed (frame_begin allocates). */
static void bind_surf(struct __GLXcontextRec *c, struct glxi_surf *s)
{
	if (s->pixels) {
		/* a render-scaled buffer is bw x bh = the window >> rscale:
		 * the core maps the window's coordinates onto it */
		glxi_core_set_scale(c->core, s->rscale);
		glxi_core_bind(c->core, s->pixels, s->bw, s->bh, s->pitch);
		glxi_core_bind_depth(c->core, s->depth);
		/* phase 4 F8: the drawable's stencil, made by the first context
		 * with stencil bits that binds it. calloc'd and said to be zero,
		 * so the core's clears write nothing until something draws into
		 * it: QuakeSpasm asks for 8 bits and clears them every frame
		 * without using them, and its pages stay untouched (review 4
		 * R3-stencil: 256-384 kB of RSS). A failure is not fatal: the
		 * core then allocates a private one */
		if (c->stencil && !s->stencil) {
			s->stencil = calloc((size_t)s->bw * s->bh + S31GL_STENCIL_TAIL, 1);
			if (s->stencil) glxi_core_stencil_zeroed(s->stencil, s->bw, s->bh);
		}
		glxi_core_bind_stencil(c->core, c->stencil ? s->stencil : NULL);
	} else {
		glxi_core_set_scale(c->core, 0);
		glxi_core_bind(c->core, NULL, s->w, s->h, 0);
		glxi_core_bind_depth(c->core, NULL);
		glxi_core_bind_stencil(c->core, NULL);
	}
}

/* Unbind `c` from its drawable. A drawable nobody is current on gives its
 * pixels back: the SHM segment is the biggest thing we own, and SDL 1.2
 * recreates its window on every video-mode change. It is re-made lazily if
 * the drawable is bound again. */
static void unbind_draw(struct __GLXcontextRec *c)
{
	struct glxi_surf *s = c->draw;

	if (!s)
		return;
	c->draw = NULL;
	c->draw_id = c->read_id = None;
	glxi_core_release_depth(c->core);
	if (--s->bound <= 0) {
		s->bound = 0;
		glxi_surf_free_buffers(s);
		/*
		 * Hold no X resource past the last unbind, while the display is
		 * certainly open: xlite's XESetCloseDisplay is a no-op, so
		 * after an XCloseDisplay this record would otherwise keep a GC
		 * and a Visual of a freed Display, and the next MakeCurrent's
		 * sweep would XFreeGC through it. Both are re-made at the next
		 * first draw (one request, one XGetVisualInfo).
		 */
		if (s->gc && s->dpy)
			XFreeGC(s->dpy, s->gc);
		s->gc = 0;
		s->visual = NULL;
	}
}

GLXI_EXPORT void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	(void)dpy;
	if (!ctx)
		return;
	if (ctx == cur) {
		/* GLX: destruction of a current context is deferred until it
		 * is no longer current. */
		ctx->destroy_pending = 1;
		return;
	}
	unbind_draw(ctx);
	context_free(ctx);
}

/* glFlush semantics for the context being left or made to wait: a
 * single-buffered drawable shows what was drawn. */
static void flush_front(struct __GLXcontextRec *c)
{
	if (c && !c->db && c->draw && c->draw->pixels) {
		glxi_core_finish(c->core);
		glxi_surf_present(c->draw);	/* never switches: nbuf 1 */
		glxi_core_frame_end(c->core);
	}
}

static void release_current(void)
{
	struct __GLXcontextRec *c = cur;

	if (!c)
		return;
	flush_front(c);
	glxi_core_bind(c->core, NULL, 0, 0, 0);
	glxi_core_make_current(NULL);
	cur = NULL;
	cur_dpy = NULL;
	if (c->destroy_pending) {
		unbind_draw(c);
		context_free(c);
	}
}

GLXI_EXPORT Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw,
				       GLXDrawable read, GLXContext ctx)
{
	struct glxi_surf *s;
	int w, h, depth;

	if (!ctx) {
		release_current();
		return True;
	}
	if (!dpy || draw == None)
		return False;		/* BadMatch */
	if (read == None)
		read = draw;
	/* Nothing changes: no round trip. freeglut and SDL re-assert the
	 * current context more often than they change it. */
	if (ctx == cur && ctx->draw && ctx->draw_id == draw &&
	    ctx->read_id == read)
		return True;

	if (glxi_query_geometry(dpy, draw, &w, &h, &depth) != 0)
		return False;		/* BadDrawable */
	if (depth != 16)
		return False;		/* BadMatch: not our visual's depth */
	s = glxi_surf_get(dpy, draw);
	if (!s)
		return False;		/* BadAlloc */

	if (cur && cur != ctx)
		release_current();
	else if (cur == ctx && ctx->draw != s)
		flush_front(ctx);	/* leaving a single-buffered drawable */
	if (ctx->draw != s) {
		unbind_draw(ctx);
		s->bound++;
		ctx->draw = s;
		glxi_surf_sweep(s);
	}
	ctx->draw_id = draw;
	ctx->read_id = read;	/* reads come from the draw buffer */
	if (s->w != w || s->h != h) {
		s->w = w;
		s->h = h;
		s->nvp = 0;			/* old rectangles prove nothing */
		glxi_surf_free_buffers(s);	/* re-made at the first draw */
	}
	if (glxi_core_make_current(ctx->core) != 0) {
		unbind_draw(ctx);
		return False;
	}
	/*
	 * Lazy: bind what exists; if nothing does yet, frame_begin makes it.
	 * The unbound case still passes the window size, so a core that
	 * initialises the viewport from it does so HERE, at the first
	 * MakeCurrent as GLX specifies, and not at the first draw - where it
	 * would overwrite a glViewport the app made in between.
	 */
	bind_surf(ctx, s);
	cur = ctx;
	cur_dpy = dpy;
	return True;
}

GLXI_EXPORT Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable,
				GLXContext ctx)
{
	return glXMakeContextCurrent(dpy, drawable, drawable, ctx);
}

GLXI_EXPORT void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst,
				unsigned long mask)
{
	/* Stub: no state is copied. Nothing in the target list calls it. */
	(void)dpy; (void)src; (void)dst; (void)mask;
}

GLXI_EXPORT Bool glXIsDirect(Display *dpy, GLXContext ctx)
{
	(void)dpy; (void)ctx;
	return True;
}

GLXI_EXPORT GLXContext glXGetCurrentContext(void)
{
	return cur;
}

GLXI_EXPORT GLXDrawable glXGetCurrentDrawable(void)
{
	return cur ? cur->draw_id : None;
}

GLXI_EXPORT GLXDrawable glXGetCurrentReadDrawable(void)
{
	return cur ? cur->read_id : None;
}

GLXI_EXPORT Display *glXGetCurrentDisplay(void)
{
	return cur ? cur_dpy : NULL;
}

GLXI_EXPORT int glXQueryContext(Display *dpy, GLXContext ctx, int attribute,
				int *value)
{
	int v;

	(void)dpy;
	if (!ctx)
		return GLX_BAD_CONTEXT;
	switch (attribute) {
	case GLX_FBCONFIG_ID:	v = ctx->fbconfig_id; break;
	case GLX_RENDER_TYPE:	v = GLX_RGBA_TYPE; break;
	case GLX_SCREEN:	v = ctx->screen; break;
	default:		return GLX_BAD_ATTRIBUTE;
	}
	if (value)
		*value = v;
	return Success;
}

/* ------------------------------------------------------- core callbacks */

void glxi_hook_frame_begin(void *user)
{
	struct __GLXcontextRec *c = user;
	struct glxi_surf *s = c ? c->draw : NULL;

	if (!s)
		return;
	if (!s->pixels) {
		if (glxi_surf_alloc(s, c->vid, c->screen, c->db) != 0)
			return;		/* the core drops draws with no buffer */
		bind_surf(c, s);
	}
	glxi_surf_wait(s);
}

/*
 * glViewport is the moment Mesa's xlib driver re-reads the window size, and
 * every app that handles resizing calls it from its reshape handler. A
 * round trip per call would hurt apps that set the viewport every frame, so
 * the rectangles already checked against the current size are remembered
 * (up to GLXI_NVP) and only a new one asks, at most once per presented
 * frame.
 *
 * Two rules keep the memory honest:
 *  - a rectangle is remembered only when it was really checked. One that
 *    arrives after this frame's query is left out and asks next frame;
 *    remembering it unchecked let a resize reported first through it go
 *    unseen for good.
 *  - a size change empties the memory. Otherwise a window resized A -> B
 *    -> A (fullscreen and back) never re-queried on the return to A,
 *    because A was still remembered: the scene was drawn at B's size into
 *    a buffer the window no longer matched (review finding
 *    viewport-memory-misses-resize-back; glxtest "vpmem").
 */
static int vp_known(const struct glxi_surf *s, int x, int y, int w, int h)
{
	int i;

	for (i = 0; i < s->nvp; i++)
		if (s->vp[i][0] == x && s->vp[i][1] == y &&
		    s->vp[i][2] == w && s->vp[i][3] == h)
			return 1;
	return 0;
}

static void vp_remember(struct glxi_surf *s, int x, int y, int w, int h)
{
	int i, n = s->nvp < GLXI_NVP ? s->nvp : GLXI_NVP - 1;

	for (i = n; i > 0; i--)		/* newest first; the oldest drops */
		memcpy(s->vp[i], s->vp[i - 1], sizeof(s->vp[0]));
	s->vp[0][0] = x; s->vp[0][1] = y; s->vp[0][2] = w; s->vp[0][3] = h;
	s->nvp = n + 1;
}

void glxi_hook_viewport(void *user, int x, int y, int w, int h)
{
	struct __GLXcontextRec *c = user;
	struct glxi_surf *s = c ? c->draw : NULL;
	int gw, gh;

	if (!s || !s->dpy)
		return;
	if (vp_known(s, x, y, w, h))
		return;
	if (s->vp_asked)
		return;		/* already asked this frame: ask next frame */
	s->vp_asked = 1;
	if (glxi_query_geometry(s->dpy, s->win, &gw, &gh, NULL) != 0)
		return;
	if (gw == s->w && gh == s->h) {
		vp_remember(s, x, y, w, h);
		return;
	}
	s->w = gw;
	s->h = gh;
	s->nvp = 0;
	vp_remember(s, x, y, w, h);
	if (!s->pixels)
		return;			/* not drawn yet: stays lazy */
	/* Resized mid-life: the frame may already have begun, so make the
	 * new buffer now rather than leave the rest of it with none. */
	glxi_surf_free_buffers(s);
	if (glxi_surf_alloc(s, c->vid, c->screen, c->db) != 0) {
		glxi_core_bind(c->core, NULL, 0, 0, 0);
		glxi_core_bind_depth(c->core, NULL);
		glxi_core_bind_stencil(c->core, NULL);
		return;
	}
	bind_surf(c, s);
}

void glxi_hook_flush(void *user, int finish)
{
	(void)finish;
	flush_front(user);
}

/* ---------------------------------------------------------------- swap */

GLXI_EXPORT void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
	struct glxi_surf *s = glxi_surf_find(dpy, drawable);

	if (!s || !s->pixels)
		return;			/* nothing was ever drawn */
	/*
	 * Presented for single-buffered drawables too: SwapBuffers is an
	 * implicit glFlush, and a flushed front buffer is what the window
	 * shows. That also keeps an app that asked for no double buffer yet
	 * relies on SwapBuffers (FBConfig sorting puts single first) visible.
	 */
	if (cur && cur->draw == s)
		glxi_core_finish(cur->core);
	if (glxi_surf_revoked(s)) {
		/*
		 * The server withdrew the render scale (the window left
		 * fullscreen). This frame is already drawn at half size: it
		 * goes out (the server expands it on the CPU), and the next
		 * one is drawn at native size into buffers made at the next
		 * draw - the scale is never a steady state out of fullscreen.
		 */
		glxi_surf_present(s);
		glxi_surf_free_buffers(s);
		if (cur && cur->draw == s) {
			glxi_core_set_scale(cur->core, 0);
			glxi_core_bind(cur->core, NULL, s->w, s->h, 0);
			glxi_core_bind_depth(cur->core, NULL);
			glxi_core_bind_stencil(cur->core, NULL);
		}
		return;
	}
	if (glxi_surf_present(s)) {
		/* ping-pong: the next frame renders into the other segment
		 * (a rebind of the same size keeps the depth buffer) */
		if (cur && cur->draw == s)
			glxi_core_bind(cur->core, s->pixels, s->bw, s->bh,
				       s->pitch);
	} else if (cur && cur->draw == s) {
		glxi_core_frame_end(cur->core);
	}
}

GLXI_EXPORT void glXWaitGL(void)
{
	if (!cur)
		return;
	glxi_core_finish(cur->core);
	flush_front(cur);
}

GLXI_EXPORT void glXWaitX(void)
{
	/* X requests and our puts share one ordered connection, and GL never
	 * reads the window, so there is nothing to wait for. Flush so the
	 * server at least has the X rendering. */
	if (cur_dpy)
		XFlush(cur_dpy);
}

/* --------------------------------------------------------- swap control */

GLXI_EXPORT void glXSwapIntervalEXT(Display *dpy, GLXDrawable drawable,
				    int interval)
{
	struct glxi_surf *s;

	if (interval < 0)
		return;		/* BadValue: no GLX_EXT_swap_control_tear */
	s = glxi_surf_get(dpy, drawable);
	if (s)
		s->interval = interval > 1 ? 1 : interval;
}

GLXI_EXPORT int glXSwapIntervalMESA(unsigned int interval)
{
	if (!cur || !cur->draw)
		return GLX_BAD_CONTEXT;
	if ((int)interval < 0)
		return GLX_BAD_VALUE;
	cur->draw->interval = interval > 1 ? 1 : (int)interval;
	return 0;
}

GLXI_EXPORT int glXGetSwapIntervalMESA(void)
{
	return cur && cur->draw ? cur->draw->interval : 0;
}

GLXI_EXPORT int glXSwapIntervalSGI(int interval)
{
	if (interval <= 0)
		return GLX_BAD_VALUE;
	if (!cur || !cur->draw)
		return GLX_BAD_CONTEXT;
	cur->draw->interval = 1;
	return 0;
}

/* ------------------------------------------------------------ drawables */

GLXI_EXPORT GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config,
				      Window win, const int *attribList)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);
	struct glxi_surf *s;

	(void)attribList;
	if (!d || !glxi_is_our_cfg(d, config) || win == None)
		return None;
	s = glxi_surf_get(dpy, win);
	if (!s)
		return None;
	s->glxwin = 1;
	return win;		/* a GLX 1.3 window IS the X window here */
}

GLXI_EXPORT void glXDestroyWindow(Display *dpy, GLXWindow window)
{
	struct glxi_surf *s = glxi_surf_find(dpy, window);

	if (!s)
		return;
	if (s->bound) {
		s->glxwin = 0;	/* still current somewhere: keep it */
		return;
	}
	glxi_surf_destroy(s);
}

/* Pixmaps and pbuffers are not supported, and no config advertises them
 * (GLX_DRAWABLE_TYPE is GLX_WINDOW_BIT only). These fail by returning None;
 * they do not raise an X error, because a stock Xlib default error handler
 * exits the process and an app probing for pixmap support should survive. */
GLXI_EXPORT GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *visual,
					 Pixmap pixmap)
{
	(void)dpy; (void)visual; (void)pixmap;
	return None;
}

GLXI_EXPORT void glXDestroyGLXPixmap(Display *dpy, GLXPixmap pixmap)
{
	(void)dpy; (void)pixmap;
}

GLXI_EXPORT GLXPixmap glXCreatePixmap(Display *dpy, GLXFBConfig config,
				      Pixmap pixmap, const int *attribList)
{
	(void)dpy; (void)config; (void)pixmap; (void)attribList;
	return None;
}

GLXI_EXPORT void glXDestroyPixmap(Display *dpy, GLXPixmap pixmap)
{
	(void)dpy; (void)pixmap;
}

GLXI_EXPORT GLXPbuffer glXCreatePbuffer(Display *dpy, GLXFBConfig config,
					const int *attribList)
{
	(void)dpy; (void)config; (void)attribList;
	return None;
}

GLXI_EXPORT void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf)
{
	(void)dpy; (void)pbuf;
}

GLXI_EXPORT void glXQueryDrawable(Display *dpy, GLXDrawable draw,
				  int attribute, unsigned int *value)
{
	struct glxi_surf *s = glxi_surf_find(dpy, draw);
	unsigned int v = 0;
	int w, h;

	if (!value)
		return;
	switch (attribute) {
	case GLX_WIDTH:
	case GLX_HEIGHT:
		if (s && s->w > 0) {
			w = s->w;
			h = s->h;
		} else if (glxi_query_geometry(dpy, draw, &w, &h, NULL) != 0) {
			w = h = 0;
		}
		v = attribute == GLX_WIDTH ? (unsigned)w : (unsigned)h;
		break;
	case GLX_PRESERVED_CONTENTS:
	case GLX_LARGEST_PBUFFER:
		v = 0;
		break;
	case GLX_FBCONFIG_ID:
		v = cur && cur->draw == s && s ? (unsigned)cur->fbconfig_id : 1;
		break;
	case GLX_SWAP_INTERVAL_EXT:
		v = s ? (unsigned)s->interval : 1;
		break;
	case GLX_MAX_SWAP_INTERVAL_EXT:
		v = 1;
		break;
	default:
		return;		/* GLX_BAD_ATTRIBUTE: value left untouched */
	}
	*value = v;
}

GLXI_EXPORT void glXSelectEvent(Display *dpy, GLXDrawable drawable,
				unsigned long mask)
{
	struct glxi_surf *s = glxi_surf_get(dpy, drawable);

	if (s)
		s->event_mask = mask;	/* no GLX event is ever generated */
}

GLXI_EXPORT void glXGetSelectedEvent(Display *dpy, GLXDrawable drawable,
				     unsigned long *mask)
{
	struct glxi_surf *s = glxi_surf_find(dpy, drawable);

	if (mask)
		*mask = s ? s->event_mask : 0;
}

/* -------------------------------------------------------- strings, etc. */

GLXI_EXPORT Bool glXQueryExtension(Display *dpy, int *errorb, int *event)
{
	(void)dpy;		/* freeglut passes NULL for both bases */
	if (errorb)
		*errorb = 0;
	if (event)
		*event = 0;
	return True;
}

GLXI_EXPORT Bool glXQueryVersion(Display *dpy, int *maj, int *min)
{
	(void)dpy;
	if (maj)
		*maj = 1;
	if (min)
		*min = 4;
	return True;
}

GLXI_EXPORT const char *glXQueryExtensionsString(Display *dpy, int screen)
{
	(void)dpy; (void)screen;
	return GLXI_EXTENSIONS;
}

static const char *glx_string(int name)
{
	switch (name) {
	case GLX_VENDOR:	return GLXI_VENDOR;
	case GLX_VERSION:	return GLXI_VERSION;
	case GLX_EXTENSIONS:	return GLXI_EXTENSIONS;
	default:		return NULL;
	}
}

GLXI_EXPORT const char *glXQueryServerString(Display *dpy, int screen,
					     int name)
{
	(void)dpy; (void)screen;
	return glx_string(name);
}

GLXI_EXPORT const char *glXGetClientString(Display *dpy, int name)
{
	(void)dpy;
	return glx_string(name);
}

GLXI_EXPORT void glXUseXFont(Font font, int first, int count, int list)
{
	int i;

	/*
	 * Stub: the display lists are created EMPTY, so glCallLists on them is
	 * valid and draws nothing. Rasterising the X font through glBitmap is
	 * plan item F7 (the core's glBitmap has to exist first).
	 */
	(void)font; (void)first;
	if (!cur)
		return;
	for (i = 0; i < count; i++) {
		glNewList((GLuint)(list + i), GL_COMPILE);
		glEndList();
	}
}

/* ------------------------------------------------------ GetProcAddress */

#define F(n) { #n, (void (*)(void))n }
static const struct {
	const char *name;
	void (*fn)(void);
} glx_funcs[] = {
	F(glXChooseFBConfig),
	F(glXChooseVisual),
	F(glXCopyContext),
	F(glXCreateContext),
	F(glXCreateGLXPixmap),
	F(glXCreateNewContext),
	F(glXCreatePbuffer),
	F(glXCreatePixmap),
	F(glXCreateWindow),
	F(glXDestroyContext),
	F(glXDestroyGLXPixmap),
	F(glXDestroyPbuffer),
	F(glXDestroyPixmap),
	F(glXDestroyWindow),
	F(glXGetClientString),
	F(glXGetConfig),
	F(glXGetCurrentContext),
	F(glXGetCurrentDisplay),
	F(glXGetCurrentDrawable),
	F(glXGetCurrentReadDrawable),
	F(glXGetFBConfigAttrib),
	F(glXGetFBConfigs),
	F(glXGetProcAddress),
	F(glXGetProcAddressARB),
	F(glXGetSelectedEvent),
	F(glXGetSwapIntervalMESA),
	F(glXGetVisualFromFBConfig),
	F(glXIsDirect),
	F(glXMakeContextCurrent),
	F(glXMakeCurrent),
	F(glXQueryContext),
	F(glXQueryDrawable),
	F(glXQueryExtension),
	F(glXQueryExtensionsString),
	F(glXQueryServerString),
	F(glXQueryVersion),
	F(glXSelectEvent),
	F(glXSwapBuffers),
	F(glXSwapIntervalEXT),
	F(glXSwapIntervalMESA),
	F(glXSwapIntervalSGI),
	F(glXUseXFont),
	F(glXWaitGL),
	F(glXWaitX),
};
#undef F

/*
 * GLX functions from our table, GL functions from the core, and NULL for
 * everything else. Never a catch-all stub: SDL takes a non-NULL answer as
 * "this exists" (SDL_x11opengl.c:281-284 fetches all of GLX this way).
 */
GLXI_EXPORT __GLXextFuncPtr glXGetProcAddressARB(const GLubyte *procName)
{
	const char *n = (const char *)procName;
	size_t i;

	if (!n)
		return NULL;
	if (n[0] == 'g' && n[1] == 'l' && n[2] == 'X') {
		for (i = 0; i < sizeof(glx_funcs) / sizeof(glx_funcs[0]); i++)
			if (!strcmp(glx_funcs[i].name, n))
				return glx_funcs[i].fn;
		return NULL;
	}
	return (__GLXextFuncPtr)glxi_core_get_proc(n);
}

GLXI_EXPORT void (*glXGetProcAddress(const GLubyte *procName))(void)
{
	return glXGetProcAddressARB(procName);
}

/* ------------------------------------------------------------ exit stats */

__attribute__((destructor)) static void glxi_fini(void)
{
	/* S31GL_TRACE: per-drawable present/wait counts. The drawables still
	 * alive at exit are the ones that matter. */
	if (!glxi_trace())
		return;
	glxi_surf_print_all();
}
