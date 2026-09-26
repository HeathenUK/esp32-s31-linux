/*
 * glx_int.h - internals of the client-side GLX 1.4 layer. MIT.
 *
 * Threading: GLX state here is process-wide and unlocked. One current
 * context per process (TinyGL keeps a single global GL state), and GLX calls
 * must not race each other. That covers every target app (all single-threaded
 * GL). A multi-threaded GL client would need a lock here and a per-thread
 * current context in the core.
 */
#ifndef GLX_INT_H
#define GLX_INT_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#define GLX_GLXEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>

#include "s31gl_iface.h"

#define GLXI_EXPORT __attribute__((visibility("default")))

/* Everything declared from here on is internal to libGL: only the glX*
 * functions (marked GLXI_EXPORT) are exported. The build compiles gl/glx
 * with default visibility because glx.h's prototypes carry no attribute. */
#pragma GCC visibility push(hidden)

/* ------------------------------------------------------------- configs */

struct __GLXFBConfigRec {
	int id;			/* GLX_FBCONFIG_ID, 1-based, unique per display */
	int screen;
	VisualID vid;
	int db;			/* GLX_DOUBLEBUFFER */
};

#define GLXI_MAXCFG 16

struct glxi_dpy {
	struct glxi_dpy *next;
	Display *dpy;
	int ncfg;
	struct __GLXFBConfigRec cfg[GLXI_MAXCFG];
	/* ChooseVisual's GLX_DOUBLEBUFFER choice, remembered per visual so
	 * glXCreateContext(visual) can tell a single-buffered request (Mesa
	 * fakeglx does the same). */
	VisualID chosen_vid[GLXI_MAXCFG];
	int chosen_db[GLXI_MAXCFG];
	int nchosen;
	int shm;		/* -1 unknown, 0 no MIT-SHM, 1 usable */
	int shm_event;		/* XShmGetEventBase + ShmCompletion */
	/* Every request before this serial (NextRequest numbering) is known
	 * processed, because one of OUR round trips (the wait's XSync, a
	 * geometry query) came back after it. xlite never advances
	 * LastKnownRequestProcessed from XSync or from events, so without this
	 * the wait could not use a round trip it had already paid for. */
	unsigned long proven;
	int proven_valid;
};

void glxi_dpy_proven(Display *dpy, unsigned long serial);

struct glxi_dpy *glxi_dpy_get(Display *dpy);
void glxi_dpy_closed(Display *dpy);
struct __GLXFBConfigRec *glxi_cfg_for_visual(struct glxi_dpy *d, int screen,
					     VisualID vid, int db);
int glxi_cfg_attrib(Display *dpy, const struct __GLXFBConfigRec *c,
		    int attr, int *value);
int glxi_is_our_cfg(struct glxi_dpy *d, const struct __GLXFBConfigRec *c);
int glxi_visual_db(struct glxi_dpy *d, VisualID vid);

/* -------------------------------------------------------------- surfaces */

/* One colour buffer: an XImage over a MIT-SHM segment (or over malloc'd
 * memory for the XPutImage fallback). */
struct glxi_buf {
	XImage *img;
	XShmSegmentInfo shm;	/* stable address: xlite and libXext keep a
				 * pointer to it in img->obdata */
	void *pixels;
	int pending;		/* a ShmPutImage with send_event is in flight */
	unsigned long pend_serial;
};

struct glxi_surf {
	struct glxi_surf *next;
	Display *dpy;
	Drawable win;
	int w, h;		/* window size as last queried */
	/* The colour buffer(s): allocated lazily at the first draw. With
	 * S31GL_SHMBUFS=2 a double-buffered drawable gets two segments and
	 * renders into one while the server copies the other. */
	struct glxi_buf buf[2];
	int nbuf, cur;
	void *pixels;		/* == buf[cur].pixels, what the core renders to */
	/* the drawable's depth buffer, bw * bh 16-bit values, made with the
	 * colour buffer: GL's ancillary buffers belong to the drawable, so
	 * every context current on this window renders into the same one */
	void *depth;
	int bw, bh, pitch;
	/* RENDER SCALE (glx_present.c, plan G04): the buffers are the window
	 * at 1/2^rscale in each axis and the server scales them back up; 0 =
	 * native. The core maps GL's window coordinates (s->w x s->h) onto
	 * them (s31gl_set_render_scale). */
	int rscale;
	int use_shm;
	GC gc;
	Visual *visual;
	int interval;		/* GLX_SWAP_INTERVAL_EXT as set; no vblank here,
				 * so it does not change the pacing */
	/* viewport memory: the last GLXI_NVP distinct rectangles that were
	 * checked against the window size, so a client that cycles viewports
	 * every frame (Quake: 2D, then 3D view; 3D + HUD + map) does not buy a
	 * round trip per call. Emptied whenever the size is seen to change. */
#define GLXI_NVP 8
	int vp[GLXI_NVP][4];
	int nvp;
	/* at most one XGetGeometry from glViewport per presented frame: an app
	 * with three or more viewports a frame (3D, HUD, map; one per cell)
	 * would otherwise pay a round trip for each */
	int vp_asked;
	int bound;		/* contexts it is current in */
	int glxwin;		/* made by glXCreateWindow */
	unsigned long event_mask;	/* glXSelectEvent, never delivered */
	/* stats, printed at exit with S31GL_TRACE */
	unsigned long n_present, n_wait_ev, n_wait_lkrp, n_wait_rt,
		      n_wait_sync, n_wait_none, n_alloc;
	int last_w, last_h, last_nbuf;
};

struct glxi_surf *glxi_surf_find(Display *dpy, Drawable d);
struct glxi_surf *glxi_surf_get(Display *dpy, Drawable d);
void glxi_surf_destroy(struct glxi_surf *s);
void glxi_surf_free_buffers(struct glxi_surf *s);
int glxi_surf_alloc(struct glxi_surf *s, VisualID vid, int screen, int db);
void glxi_surf_wait(struct glxi_surf *s);
int glxi_surf_present(struct glxi_surf *s);	/* 1: s->pixels moved */
int glxi_query_geometry(Display *dpy, Drawable d, int *w, int *h, int *depth);
void glxi_surfs_for_display_closed(Display *dpy);
void glxi_surf_print_all(void);
void glxi_surf_sweep(struct glxi_surf *keep);

/* --------------------------------------------------------------- context */

struct __GLXcontextRec {
	int id;
	Display *dpy;
	int screen;
	VisualID vid;
	int db;
	int fbconfig_id;
	s31gl_ctx *core;
	struct glxi_surf *draw;
	GLXDrawable draw_id, read_id;
	int destroy_pending;
};

/* ------------------------------------------------------------ core glue */

s31gl_ctx *glxi_core_create(s31gl_ctx *share, struct __GLXcontextRec *c);
void glxi_core_destroy(s31gl_ctx *ctx);
int glxi_core_make_current(s31gl_ctx *ctx);
int glxi_core_bind(s31gl_ctx *ctx, void *p, int w, int h, int pitch);
void glxi_core_frame_end(s31gl_ctx *ctx);
void glxi_core_finish(s31gl_ctx *ctx);
void *glxi_core_get_proc(const char *name);
void glxi_core_release_depth(s31gl_ctx *ctx);
int glxi_core_bind_depth(s31gl_ctx *ctx, void *depth);
int glxi_core_can_scale(void);
int glxi_core_set_scale(s31gl_ctx *ctx, int shift);
/* callbacks the core calls, defined in glx.c */
void glxi_hook_frame_begin(void *user);
void glxi_hook_viewport(void *user, int x, int y, int w, int h);
void glxi_hook_flush(void *user, int finish);

/* ------------------------------------------------------------- misc */

int glxi_trace(void);
/* X error trap around one call: install, run, check. Not thread-safe.
 * Claims only errors for requests issued after trap_begin; older ones (the
 * app's own, still in flight) go to the app's handler. */
void glxi_trap_begin(Display *dpy);
int glxi_trap_end(void);	/* returns the X error code seen, 0 if none */

#pragma GCC visibility pop

#endif
