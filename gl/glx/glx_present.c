/*
 * glx_present.c - GLX drawables and the P1 present (docs/gl-plan-2026-09-25.md
 * section 5). MIT.
 *
 * Per drawable, ONE colour buffer in a MIT-SHM segment (XShmCreateImage,
 * ZPixmap, depth 16). The rasteriser renders into it directly; there is no
 * copy inside libGL. glXSwapBuffers is XShmPutImage(send_event=True) plus a
 * flush, so the server starts copying at once and the copy overlaps whatever
 * the app does before it draws again.
 *
 * The one hazard is the server still reading the segment when the next frame
 * starts writing it. So the NEXT frame's first write (the core's frame_begin
 * hook) waits until the server has consumed the put:
 *
 *   1. drain our ShmCompletion events from the queue (XCheckIfEvent with a
 *      predicate that matches ONLY a ShmCompletion for this drawable, so no
 *      app event is ever consumed). One with a serial at or after the put
 *      proves it; older ones are stale and are dropped too.
 *   2. LastKnownRequestProcessed(dpy) >= the put's serial also proves it.
 *      Stock Xlib advances that on every event it reads, so this catches the
 *      case where the APP's own XNextEvent loop ate our completion (glxgears,
 *      freeglut and SDL all drain every pending event each frame). xlite
 *      advances it only on replies, so there it only helps after a round trip.
 *   3. Otherwise XSync(dpy, False): a round trip. The server handles requests
 *      in order and xshim (and every stock server) copies ShmPutImage
 *      synchronously, so when the sync reply arrives the segment is free.
 *      Then drain the completion again so it is not left for the app.
 *
 * This never hangs on a lost event (a put that errored sends none: xshim's
 * short-segment path, a BadDrawable), and it is never wrong. Its cost is
 * one round trip per frame exactly when the completion had not arrived yet
 * or was eaten by the app on xlite; S31GL_TRACE=1 prints which branch each
 * frame took, per drawable, at exit.
 *
 * The swap interval does not change any of this. There is no vblank to skip
 * here, and GL's interval 0 only drops the wait for one: it never lets the
 * window show back-buffer pixels that were not swapped. Skipping the wait at
 * interval 0 did exactly that - with one segment the next frame's clear was
 * copied out mid-put, 195 of 200 frames in the review's probe
 * (/Users/gadyke/.cache/s31-glreview/atk.c "interval0"; glxtest "interval0"
 * now). S31GL_NOWAIT=1 is the old behaviour as a debug toggle only: every
 * put unpaced, torn and half-drawn frames included.
 *
 * A double-buffered drawable gets a SECOND segment when it costs at most
 * 200 kB (want_bufs(); S31GL_SHMBUFS=1 or 2 forces one or two): frame N+1
 * renders into B while the server copies A, and the wait at N+2 is for a
 * copy that has normally long finished. Measured in want_bufs().
 *
 * Without MIT-SHM (or with S31GL_NOSHM=1, the runtime A/B toggle) the buffer
 * is malloc'd and presented with XPutImage, which copies into the request
 * before it returns, so it never needs a wait.
 *
 * RENDER SCALE (plan G04; owner decision: ON by default for panel-size
 * fullscreen windows, S31GL_RENDER_SCALE=0 turns it off for an A/B, and its
 * fps is never quoted as like-for-like). A window the size of the panel does
 * not fit in RAM at native resolution next to the desktop (colour + depth
 * 1.5 MB at 800x480), and its clear alone is 13-15 ms. So when the server is
 * our xshim and the core can map coordinates, glxi_surf_alloc() asks the
 * server (XLITE-SHM RenderScale, lvdesk/xshim.c rscale_request) whether it
 * will take the window at HALF size in each axis. xshim grants it only for
 * exactly the panel size; then the colour and depth buffers are 400x240,
 * the core maps GL's window coordinates onto them (glx_core.c,
 * s31gl_set_render_scale), and the whole-frame XShmPutImage of 400x240 is
 * scanned out through the PPA at 2x. The application sees an 800x480 window
 * throughout - geometry, events, glGet(GL_VIEWPORT), glReadPixels sizes -
 * and only the pixels are softer. Anything that is not a whole-frame put
 * of the half-size buffer is an ordinary put, and a server without the
 * request ("XLITE-RSCALE" absent: stock X, the host Xvfb rig, an older
 * xshim) is never asked. The scale is for fullscreen only (owner's review):
 * a window that is not fullscreen is refused, and one that LEAVES
 * fullscreen is revoked - the server sets a word past the image in our
 * segment, glxi_surf_revoked() sees it at the next present, and the
 * buffers are remade at native size before the next frame.
 *
 * NOT DONE, measured: letting the server show our segment until the next
 * put ("segment hold") saved lvdesk's copy but LOWERED fps, 37.9 -> 32.6
 * windowed - see "REJECTED 2026-09-26" in lvdesk/xshim.c mitshm_request.
 *
 * ZERO-COPY FULLSCREEN (plan section 5 P2; board test in
 * artifacts/gl/phase6/ZEROCOPY.txt). Once render scale is granted - which
 * xshim does only for a panel-size fullscreen window - the colour buffers
 * need not be MIT-SHM at all: xshim can re-home a pixmap of ours as a GEM
 * dumb buffer (XLITE-SHM minor 5, "XLITE-ZC", lvdesk/xshim.c zc_request),
 * we map it through XLITE-SHM GetPixmapFd (xlite's XliteShmMap, found with
 * dlsym, so a stock libX11 simply never gets here), render into it, and a
 * present is a request naming the buffer: the desktop flips that buffer's
 * framebuffer onto the CRTC and the PPA scales it to the panel. Nothing
 * copies the frame. Two buffers by default (S31GL_ZCBUFS=3 for three).
 *
 * The wait is on a FENCE instead of a completion: a control row past each
 * buffer's image holds `consumed`, the newest frame whose buffer the PPA has
 * finished reading. The server publishes it before it reads our next
 * request (the flip's scale is synchronous), so it is normally there long
 * before we need it (two buffers = a whole frame of slack); if not, a short
 * poll and then XSync, after which it is certain - never a hang.
 *
 * Fallbacks: no extension, no XliteShmMap, a refused or failed allocation,
 * S31GL_ZC=0, or fewer than two buffers: MIT-SHM as above. The server says
 * "revoked" (left fullscreen) or "declined" (the desktop would not flip) in
 * the same row; either is handled like the render-scale revoke - this frame
 * goes out, the buffers are remade before the next - and a decline, or a
 * fence that never arrives, keeps this drawable on MIT-SHM for good.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE		/* RTLD_DEFAULT */
#endif
#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <stdint.h>
#include <sys/shm.h>

#include <X11/Xlibint.h>

#include "glx_int.h"
#include "zc_proto.h"

static struct glxi_surf *surfs;

/* ------------------------------------------------------------ render scale */

/* S31GL_RENDER_SCALE=0: never ask (the A/B toggle). Default on. */
static int rscale_wanted(void)
{
	static int v = -1;

	if (v < 0) {
		const char *e = getenv("S31GL_RENDER_SCALE");

		v = !(e && strcmp(e, "0") == 0);
	}
	return v;
}

/*
 * XLITE-SHM minor 3, RenderScale(window, width, height): the reply's first
 * byte says whether the server will take a width x height whole-frame put
 * as the window's full contents. 0 x 0 withdraws. Returns the granted shift
 * (1) or 0. One round trip, only when buffers are (re)made.
 */
static int rscale_ask(Display *dpy, Window win, int bw, int bh)
{
	int major, ev, er, ok = 0;
	xGenericReply rep;
	struct rscale_req {
		CARD8 reqType, minor;
		CARD16 length;
		CARD32 window;
		CARD16 width, height;
	} *req;

	if (!XQueryExtension(dpy, "XLITE-RSCALE", &major, &ev, &er))
		return 0;
	LockDisplay(dpy);
	req = (struct rscale_req *)_XGetRequest(dpy, (CARD8)major, sizeof(*req));
	if (req) {
		req->minor = 3;
		req->window = (CARD32)win;
		req->width = (CARD16)bw;
		req->height = (CARD16)bh;
		if (_XReply(dpy, (xReply *)&rep, 0, xTrue))
			ok = (rep.data00 & 0xff) == 1 ? 1 : 0;
	}
	UnlockDisplay(dpy);
	SyncHandle();
	return ok;
}

/*
 * The server revoked the render scale (the window left fullscreen, or is
 * not presented scaled): it wrote 1 into the spare word past either
 * segment's image. Read at every present - no round trip, and it works
 * when the application eats every event. The caller then reallocates at
 * native size before the next frame (glx.c glXSwapBuffers).
 */
int glxi_surf_revoked(struct glxi_surf *s)
{
	int i;

	if (s->zc) {
		if (!s->zctl)
			return 1;
		if (s->zctl[ZC_W_DECLINED] || s->zc_off) {	/* MIT-SHM for good */
			if (!s->zc_off && glxi_trace())
				fprintf(stderr, "libGL: drawable 0x%lx: zero-copy "
					"declined by the server\n",
					(unsigned long)s->win);
			s->zc_off = 1;
			return 1;
		}
		return s->zctl[ZC_W_REVOKED] != 0;	/* render scale revoked */
	}
	if (!s->rscale || !s->use_shm)
		return 0;
	for (i = 0; i < s->nbuf; i++)
		if (s->buf[i].pixels &&
		    *(volatile uint32_t *)((char *)s->buf[i].pixels +
					   (size_t)s->pitch * s->bh))
			return 1;
	return 0;
}

/* The window is the panel's size: ask for half of it. */
static int rscale_negotiate(struct glxi_surf *s)
{
	if (!rscale_wanted() || !glxi_core_can_scale() || !s->use_shm ||
	    (s->w & 1) || (s->h & 1) || s->w < 64 || s->h < 64)
		return 0;
	return rscale_ask(s->dpy, s->win, s->w / 2, s->h / 2);
}

/* ------------------------------------------------------------ error trap */

static int (*trap_prev)(Display *, XErrorEvent *);
static int trap_code;
static unsigned long trap_serial;
static Display *trap_dpy;

/* Serial comparison on the low 16 bits: xlite's event and error serials are
 * the raw wire sequence, Xlib's are widened; both agree mod 2^16, and a
 * frame never spans 32768 requests. */
static int serial_at_or_after(unsigned long ev, unsigned long put)
{
	return ((ev - put) & 0xffffUL) < 0x8000UL;
}

/*
 * Only errors for OUR requests - issued on this display since trap_begin -
 * are ours. Anything older is the app's own, asynchronous and still in
 * flight when we made a round trip (XSetInputFocus on an unmapped window,
 * or on the board xshim's BadImplementation for an opcode it lacks): it goes
 * to the handler the app installed, as SDL's and Mesa's traps do, and it
 * must not fail our call. Before this, one stray app error made
 * glXMakeCurrent return False, hid a resize and could push a drawable onto
 * XPutImage for good.
 */
static int trap_handler(Display *dpy, XErrorEvent *e)
{
	if (dpy == trap_dpy && serial_at_or_after(e->serial, trap_serial)) {
		if (!trap_code)
			trap_code = e->error_code ? e->error_code : 1;
		return 0;
	}
	return trap_prev ? trap_prev(dpy, e) : 0;
}

void glxi_trap_begin(Display *dpy)
{
	trap_code = 0;
	trap_dpy = dpy;
	trap_serial = NextRequest(dpy);
	trap_prev = XSetErrorHandler(trap_handler);
}

int glxi_trap_end(void)
{
	XSetErrorHandler(trap_prev);
	trap_prev = NULL;
	trap_dpy = NULL;
	return trap_code;
}

int glxi_query_geometry(Display *dpy, Drawable d, int *w, int *h, int *depth)
{
	Window root;
	int x, y, err;
	unsigned int uw = 0, uh = 0, bw, dep = 0;
	Status ok;

	unsigned long serial = NextRequest(dpy);

	glxi_trap_begin(dpy);
	ok = XGetGeometry(dpy, d, &root, &x, &y, &uw, &uh, &bw, &dep);
	err = glxi_trap_end();
	if (!ok || err)
		return -1;
	glxi_dpy_proven(dpy, serial);	/* a reply: all before it is done */
	*w = (int)uw;
	*h = (int)uh;
	if (depth)
		*depth = (int)dep;
	return 0;
}

/* A reply to the request numbered `serial` arrived: every request up to
 * it has been processed by the server. */
void glxi_dpy_proven(Display *dpy, unsigned long serial)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);

	if (!d)
		return;
	if (!d->proven_valid || (long)(serial - d->proven) > 0 ||
	    (long)(NextRequest(dpy) - d->proven) <= 0) {
		d->proven = serial;
		d->proven_valid = 1;
	}
}

/* --------------------------------------------------------------- records */

struct glxi_surf *glxi_surf_find(Display *dpy, Drawable d)
{
	struct glxi_surf *s;

	for (s = surfs; s; s = s->next)
		if (s->dpy == dpy && s->win == d)
			return s;
	return NULL;
}

struct glxi_surf *glxi_surf_get(Display *dpy, Drawable d)
{
	struct glxi_surf *s = glxi_surf_find(dpy, d);

	if (s)
		return s;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->dpy = dpy;
	s->win = d;
	s->interval = 1;
	{
		int i;

		for (i = 0; i < GLXI_MAXBUF; i++)
			s->buf[i].shm.shmid = -1;
	}
	s->next = surfs;
	surfs = s;
	return s;
}

static void print_stats(struct glxi_surf *s)
{
	if (!glxi_trace() || !s->n_present)
		return;
	fprintf(stderr, "libGL: drawable 0x%lx %dx%d %s x%d: %lu presents, "
		"%lu allocs; waits: %lu event/fence, %lu serial, "
		"%lu earlier-round-trip, %lu XSync, %lu fence-poll, %lu none\n",
		(unsigned long)s->win, s->last_w, s->last_h,
		s->zc ? "ZERO-COPY" : s->use_shm ? "MIT-SHM" : "XPutImage",
		s->last_nbuf, s->n_present, s->n_alloc,
		s->n_wait_ev, s->n_wait_lkrp, s->n_wait_rt, s->n_wait_sync,
		s->n_wait_poll, s->n_wait_none);
}

static void buf_free(struct glxi_surf *s, struct glxi_buf *b)
{
	if (b->zpix) {
		/* The mapping is xlite's (XliteShmMap caches it per drawable)
		 * and FreePixmap drops it; the server orders the free after
		 * any present still in flight and puts its own mode buffer
		 * back on the CRTC first (xshim.c zc_pix_release). */
		if (s->dpy)
			XFreePixmap(s->dpy, b->zpix);
		memset(b, 0, sizeof(*b));
		b->shm.shmid = -1;
		return;
	}
	if (b->img) {
		if (s->use_shm) {
			/* Ordered after any put still in flight, and the
			 * segment was IPC_RMID'd once attached, so the server
			 * keeps its mapping until it has processed both: no
			 * wait is needed before letting go of ours. */
			if (s->dpy)
				XShmDetach(s->dpy, &b->shm);
			b->img->data = NULL;
			XDestroyImage(b->img);
			shmdt(b->shm.shmaddr);
		} else {
			b->img->data = NULL;	/* ours, freed below */
			XDestroyImage(b->img);
			free(b->pixels);
		}
	}
	memset(b, 0, sizeof(*b));
	b->shm.shmid = -1;
}

void glxi_surf_free_buffers(struct glxi_surf *s)
{
	int i;

	for (i = 0; i < GLXI_MAXBUF; i++)
		buf_free(s, &s->buf[i]);
	s->zc = 0;
	s->zctl = NULL;
	/* the server stops treating half-size puts as the whole window (a
	 * resize has already dropped it there; this covers the unbind) */
	if (s->rscale && s->dpy)
		rscale_ask(s->dpy, s->win, 0, 0);
	s->rscale = 0;
	free(s->depth);
	s->depth = NULL;
	free(s->stencil);
	s->stencil = NULL;
	s->nbuf = s->cur = 0;
	s->pixels = NULL;
	s->bw = s->bh = s->pitch = 0;
}

void glxi_surf_destroy(struct glxi_surf *s)
{
	struct glxi_surf **pp;

	print_stats(s);
	glxi_surf_free_buffers(s);
	if (s->gc && s->dpy)
		XFreeGC(s->dpy, s->gc);
	for (pp = &surfs; *pp; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	free(s);
}

/*
 * Forget drawables nobody is current on and nobody configured. Their pixels
 * went at unbind already; this drops the record and its GC, so a client that
 * recreates its window (SDL 1.2 does on every SDL_SetVideoMode) does not
 * accumulate one per window. Records carrying a GLXWindow, a swap interval or
 * an event mask are kept: the app may still refer to them.
 */
void glxi_surf_sweep(struct glxi_surf *keep)
{
	struct glxi_surf *s = surfs, *n;

	for (; s; s = n) {
		n = s->next;
		if (s != keep && !s->bound && !s->glxwin && s->interval == 1 &&
		    !s->event_mask && !s->pixels)
			glxi_surf_destroy(s);
	}
}

/* XCloseDisplay: the connection is gone, so the server has already dropped
 * the segments and GCs; only our side remains. */
void glxi_surfs_for_display_closed(Display *dpy)
{
	struct glxi_surf *s = surfs, *n;

	for (; s; s = n) {
		n = s->next;
		if (s->dpy != dpy)
			continue;
		s->dpy = NULL;
		s->gc = 0;
		glxi_surf_destroy(s);
	}
}

static int nowait(void);

/* ------------------------------------------------------------ zero-copy */

/* S31GL_ZC=0: never ask (the A/B toggle). Default on. */
static int zc_wanted(void)
{
	static int v = -1;

	if (v < 0) {
		const char *e = getenv("S31GL_ZC");

		v = !(e && strcmp(e, "0") == 0);
	}
	return v;
}

/* S31GL_ZCBUFS=2|3; default 2 (one frame of slack is all the fence needs:
 * the server publishes it as it handles the present). */
static int zc_bufs(void)
{
	static int n = -1;

	if (n < 0) {
		const char *e = getenv("S31GL_ZCBUFS");

		n = e && atoi(e) >= 3 ? 3 : 2;
	}
	return n;
}

typedef void *(*xlite_map_fn)(Display *, Pixmap, int *, int *, int *, int *);

/* xlite's XLITE-SHM GetPixmapFd + mmap, exported by the board's libX11 and
 * absent from every stock one. */
static xlite_map_fn zc_mapper(void)
{
	static int looked;
	static xlite_map_fn fn;

	if (!looked) {
		looked = 1;
		fn = (xlite_map_fn)dlsym(RTLD_DEFAULT, "XliteShmMap");
	}
	return fn;
}

/*
 * XLITE-SHM minor 5 (lvdesk/xshim.c zc_request): op 0 ALLOC (a reply), op 1
 * PRESENT seq (none). Returns the reply's words for ALLOC, 0 on refusal.
 */
static int zc_send(Display *dpy, Window win, Pixmap pix, int op, uint32_t seq)
{
	int major, ev, er, ok = 0;
	struct zc_req {
		CARD8 reqType, minor;
		CARD16 length;
		CARD32 window, pixmap;
		CARD8 op, pad0, pad1, pad2;
		CARD32 seq;
	} *req;

	if (!XQueryExtension(dpy, "XLITE-ZC", &major, &ev, &er))
		return 0;
	LockDisplay(dpy);
	req = (struct zc_req *)_XGetRequest(dpy, (CARD8)major,
					    op == ZC_OP_PRESENT ? 20 : 16);
	if (req) {
		req->minor = ZC_MINOR;
		req->window = (CARD32)win;
		req->pixmap = (CARD32)pix;
		req->op = (CARD8)op;
		req->pad0 = req->pad1 = req->pad2 = 0;
		if (op == ZC_OP_PRESENT)
			req->seq = seq;
		ok = 1;
	}
	UnlockDisplay(dpy);
	SyncHandle();
	return ok;
}

static int zc_ask_alloc(Display *dpy, Window win, Pixmap pix, int *pitch,
			uint32_t *ctl_off, uint32_t *len)
{
	int major, ev, er, ok = 0;
	xGenericReply rep;
	struct zc_req {
		CARD8 reqType, minor;
		CARD16 length;
		CARD32 window, pixmap;
		CARD8 op, pad0, pad1, pad2;
	} *req;

	if (!XQueryExtension(dpy, "XLITE-ZC", &major, &ev, &er))
		return 0;
	LockDisplay(dpy);
	req = (struct zc_req *)_XGetRequest(dpy, (CARD8)major, sizeof(*req));
	if (req) {
		req->minor = ZC_MINOR;
		req->window = (CARD32)win;
		req->pixmap = (CARD32)pix;
		req->op = ZC_OP_ALLOC;
		req->pad0 = req->pad1 = req->pad2 = 0;
		if (_XReply(dpy, (xReply *)&rep, 0, xTrue) &&
		    (rep.data00 & 0xff) == 1) {
			ok = 1;
			*pitch = (int)(rep.data00 >> 16);
			*ctl_off = rep.data01;
			*len = rep.data02;
		}
	}
	UnlockDisplay(dpy);
	SyncHandle();
	return ok;
}

/* ZC_OP_QUERY: the buffer size the server would grant zero-copy at. */
static int zc_query(Display *dpy, Window win, int *bw, int *bh)
{
	int major, ev, er, ok = 0;
	xGenericReply rep;
	struct zc_req {
		CARD8 reqType, minor;
		CARD16 length;
		CARD32 window, pixmap;
		CARD8 op, pad0, pad1, pad2;
	} *req;

	if (!XQueryExtension(dpy, "XLITE-ZC", &major, &ev, &er))
		return 0;
	LockDisplay(dpy);
	req = (struct zc_req *)_XGetRequest(dpy, (CARD8)major, sizeof(*req));
	if (req) {
		req->minor = ZC_MINOR;
		req->window = (CARD32)win;
		req->pixmap = 0;
		req->op = ZC_OP_QUERY;
		req->pad0 = req->pad1 = req->pad2 = 0;
		if (_XReply(dpy, (xReply *)&rep, 0, xTrue) &&
		    (rep.data00 & 0xff) == 1) {
			ok = 1;
			*bw = (int)(rep.data00 >> 16);
			*bh = (int)(rep.data01 & 0xffff);
		}
	}
	UnlockDisplay(dpy);
	SyncHandle();
	return ok;
}

/*
 * The colour buffers as zero-copy GEM pixmaps. 0 on success (s->zc set,
 * s->nbuf buffers, s->pitch the GEM pitch); -1 leaves nothing behind and
 * the caller makes MIT-SHM segments as before.
 */
static int zc_alloc(struct glxi_surf *s)
{
	xlite_map_fn map = zc_mapper();
	int major, ev, er, i, n = zc_bufs(), pitch = 0, err;
	uint32_t ctl_off = 0, len = 0;

	if (!zc_wanted() || s->zc_off || !map || !s->dpy ||
	    !XQueryExtension(s->dpy, "XLITE-ZC", &major, &ev, &er))
		return -1;
	{
		/* one round trip, at allocation only: is this drawable a
		 * fullscreen the server scans out whole, at our size? */
		int qw = 0, qh = 0;

		if (!zc_query(s->dpy, s->win, &qw, &qh) || qw != s->bw ||
		    qh != s->bh) {
			if (glxi_trace())
				fprintf(stderr, "libGL: drawable 0x%lx %dx%d: "
					"zero-copy not offered (server %dx%d)\n",
					(unsigned long)s->win, s->bw, s->bh,
					qw, qh);
			return -1;
		}
	}
	glxi_trap_begin(s->dpy);
	for (i = 0; i < n; i++) {
		struct glxi_buf *b = &s->buf[i];
		int w = 0, h = 0, stride = 0, bpp = 0, p = 0;
		void *px;

		b->zpix = XCreatePixmap(s->dpy, s->win, (unsigned)s->bw,
					(unsigned)s->bh, 16);
		if (!b->zpix)
			break;
		if (!zc_ask_alloc(s->dpy, s->win, b->zpix, &p, &ctl_off, &len))
			break;
		px = map(s->dpy, b->zpix, &w, &h, &stride, &bpp);
		if (!px || w != s->bw || h != s->bh || stride != p ||
		    bpp != 2 || ctl_off < (uint32_t)p * (uint32_t)s->bh ||
		    (size_t)ctl_off + 16 > len)
			break;
		b->pixels = px;
		b->pending = 0;
		b->zseq = 0;
		pitch = p;
	}
	err = glxi_trap_end();
	if (i < n || err) {
		if (glxi_trace())
			fprintf(stderr, "libGL: drawable 0x%lx: zero-copy "
				"buffer %d of %d refused (X error %d), "
				"MIT-SHM\n", (unsigned long)s->win, i + 1, n,
				err);
		for (i = 0; i < n; i++)
			if (s->buf[i].zpix) {
				XFreePixmap(s->dpy, s->buf[i].zpix);
				memset(&s->buf[i], 0, sizeof(s->buf[i]));
				s->buf[i].shm.shmid = -1;
			}
		return -1;
	}
	s->zctl = (volatile uint32_t *)((char *)s->buf[0].pixels + ctl_off);
	if (s->zctl[ZC_W_MAGIC] != ZC_MAGIC) {
		s->zctl = NULL;
		for (i = 0; i < n; i++) {
			XFreePixmap(s->dpy, s->buf[i].zpix);
			memset(&s->buf[i], 0, sizeof(s->buf[i]));
			s->buf[i].shm.shmid = -1;
		}
		return -1;
	}
	/* number our frames on from what the window has consumed, so a
	 * fresh buffer's first wait can never be satisfied by an old value */
	s->zseq = __atomic_load_n(&s->zctl[ZC_W_CONSUMED], __ATOMIC_SEQ_CST);
	s->zc = 1;
	s->nbuf = n;
	s->pitch = pitch;
	return 0;
}

static int zc_consumed(struct glxi_surf *s, const struct glxi_buf *b)
{
	if (!s->zctl)
		return 1;
	return zc_seq_done(__atomic_load_n(&s->zctl[ZC_W_CONSUMED],
					   __ATOMIC_SEQ_CST), b->zseq);
}

/*
 * Before the first write into a zero-copy buffer: the PPA must have read the
 * frame it last presented. Normally already true (the server published it a
 * frame ago). Otherwise poll for up to 2 ms - the desktop is mid-flip - then
 * one round trip, after which the server has handled the present and the
 * fence is certain. If it is STILL not there the protocol is broken: say so
 * once, stop waiting and leave zero-copy for this drawable (glXSwapBuffers
 * sees zc_off through glxi_surf_revoked and remakes the buffers).
 */
static void zc_wait(struct glxi_surf *s, struct glxi_buf *b)
{
	unsigned long sync_serial;
	struct timespec ts = { 0, 125000 };
	int i;

	if (zc_consumed(s, b)) {
		b->pending = 0;
		s->n_wait_ev++;
		return;
	}
	for (i = 0; i < 16; i++) {
		nanosleep(&ts, NULL);
		if (zc_consumed(s, b)) {
			b->pending = 0;
			s->n_wait_poll++;
			return;
		}
	}
	sync_serial = NextRequest(s->dpy);
	XSync(s->dpy, False);
	glxi_dpy_proven(s->dpy, sync_serial);
	s->n_wait_sync++;
	b->pending = 0;
	if (zc_consumed(s, b))
		return;
	for (i = 0; i < 400 && !zc_consumed(s, b); i++)
		nanosleep(&ts, NULL);	/* <= 50 ms: an async re-present */
	if (!zc_consumed(s, b)) {
		static int said;

		if (!said++)
			fprintf(stderr, "libGL: drawable 0x%lx: zero-copy fence "
				"stuck (consumed %u, need %u) - MIT-SHM from now "
				"on\n", (unsigned long)s->win,
				(unsigned)s->zctl[ZC_W_CONSUMED],
				(unsigned)b->zseq);
		s->zc_off = 1;
	}
}

/* ------------------------------------------------------------ allocation */

static int shm_usable(Display *dpy, struct glxi_dpy *d)
{
	if (d->shm >= 0)
		return d->shm;
	d->shm = 0;
	if (getenv("S31GL_NOSHM"))
		return 0;
	if (XShmQueryExtension(dpy)) {
		d->shm = 1;
		d->shm_event = XShmGetEventBase(dpy) + ShmCompletion;
	}
	return d->shm;
}

static int alloc_shm(struct glxi_surf *s, struct glxi_buf *b)
{
	Display *dpy = s->dpy;
	XImage *img;
	size_t len;
	int err;

	b->shm.shmid = -1;
	img = XShmCreateImage(dpy, s->visual, 16, ZPixmap, NULL, &b->shm,
			      s->bw, s->bh);
	if (!img)
		return -1;
	/* + one word past the image: the render-scale revoke flag, which the
	 * server sets (xshim.c rs_revoke; glxi_surf_revoked) */
	len = (size_t)img->bytes_per_line * img->height + 4;
	b->shm.shmid = shmget(IPC_PRIVATE, len, IPC_CREAT | 0600);
	if (b->shm.shmid < 0) {
		XDestroyImage(img);
		return -1;
	}
	b->shm.shmaddr = shmat(b->shm.shmid, NULL, 0);
	if (b->shm.shmaddr == (char *)-1) {
		shmctl(b->shm.shmid, IPC_RMID, NULL);
		b->shm.shmid = -1;
		b->shm.shmaddr = NULL;
		XDestroyImage(img);
		return -1;
	}
	b->shm.readOnly = False;
	img->data = b->shm.shmaddr;
	/* Attach, and find out NOW whether the server could: the error is
	 * asynchronous, and a failed attach would otherwise surface as
	 * BadShmSeg on the first put (SDL's try_mitshm does the same). */
	glxi_trap_begin(dpy);
	XShmAttach(dpy, &b->shm);
	XSync(dpy, False);
	err = glxi_trap_end();
	/* Either way the id can go: the attaches that exist keep it alive,
	 * and nothing leaks if we die. */
	shmctl(b->shm.shmid, IPC_RMID, NULL);
	if (err) {
		img->data = NULL;
		XDestroyImage(img);
		shmdt(b->shm.shmaddr);
		b->shm.shmid = -1;
		b->shm.shmaddr = NULL;
		return -1;
	}
	b->img = img;
	b->pixels = b->shm.shmaddr;
	s->pitch = img->bytes_per_line;
	return 0;
}

static int alloc_plain(struct glxi_surf *s, struct glxi_buf *b)
{
	int pitch = ((s->bw * 16 + 31) / 32) * 4;
	void *p = malloc((size_t)pitch * s->bh);
	XImage *img;

	if (!p)
		return -1;
	img = XCreateImage(s->dpy, s->visual, 16, ZPixmap, 0, p, s->bw, s->bh,
			   32, pitch);
	if (!img) {
		free(p);
		return -1;
	}
	b->img = img;
	b->pixels = p;
	s->pitch = img->bytes_per_line;
	return 0;
}

/*
 * How many segments a double-buffered SHM drawable gets. S31GL_SHMBUFS=1 or
 * 2 forces it; unset, a second segment is made when it costs at most
 * GLXI_BUF2_MAX bytes. MEASURED 2026-09-26 (stock glxgears from SD, kernel
 * #391, fresh boot per arm, 6 runs of 10 s, run 1 discarded; presents/s and
 * CPU% of each process, artifacts/gl/present/arms-*):
 *
 *   windowed 300x300        1 buf  fps 28.6-30.4 (median 28.9)  gears 60-63%  lvdesk 50-52%
 *                           2 bufs fps 32.3-35.1 (median 34.1)  gears 91-97%  lvdesk 59-61%
 *   fullscreen, render      1 buf  fps 25.3-28.5 (median 26.1)  gears 65-70%  lvdesk 34-37%
 *   scale (400x240 buffer)  2 bufs fps 30.2-37.3 (median 34.4)  gears 93-97%  lvdesk 43-63%
 *
 * The second segment wins fps in both (+18%, +32%; the ranges do not
 * overlap) for 176-192 kB. It is NOT a spin: the "98% of a core" it was
 * suspected of is the client no longer idling while the server copies the
 * frame (h1s, glxgears pinned to CPU0: 80% of samples in the rasteriser -
 * memset_16 24.5%, ZB_fillTriangleFlat_lt 19.9%, glopVertex 8.2% - and no
 * xlite or GLX wait symbol in the top 30). Per frame the client costs ~31%
 * more (21 -> 28 ms: the clear and the fill slow down while the desktop's
 * copy of the previous frame shares PSRAM with them), so the cap keeps big
 * windowed drawables, where RAM is the thing that runs out, on one.
 */
#define GLXI_BUF2_MAX	(200 * 1024)

static int want_bufs(size_t seg_bytes)
{
	static int n = -1;

	if (n < 0) {
		const char *e = getenv("S31GL_SHMBUFS");

		n = !e ? 0 : atoi(e) >= 2 ? 2 : 1;
	}
	if (n)
		return n;
	return seg_bytes <= GLXI_BUF2_MAX ? 2 : 1;
}

/*
 * Allocate the colour buffer(s) at the window's current size. Called lazily
 * from the first draw, never from MakeCurrent: a context that is made current
 * and never draws (SDL2's extension probe does exactly that) costs no pixels.
 */
int glxi_surf_alloc(struct glxi_surf *s, VisualID vid, int screen, int db)
{
	struct glxi_dpy *d = glxi_dpy_get(s->dpy);

	if (s->pixels)
		return 0;
	if (!d || s->w <= 0 || s->h <= 0)
		return -1;
	if (!s->visual) {
		XVisualInfo tmpl, *vi;
		int n = 0;

		memset(&tmpl, 0, sizeof tmpl);
		tmpl.visualid = vid;
		tmpl.screen = screen;
		vi = XGetVisualInfo(s->dpy, VisualIDMask | VisualScreenMask,
				    &tmpl, &n);
		if (!vi)
			return -1;
		s->visual = vi[0].visual;
		XFree(vi);
	}
	if (!s->gc) {
		XGCValues gv;

		gv.graphics_exposures = False;
		s->gc = XCreateGC(s->dpy, s->win, GCGraphicsExposures, &gv);
		if (!s->gc)
			return -1;
	}
	/*
	 * Render scale first: it decides the size of everything below. The
	 * use_shm guess makes it SHM-only (the scale is a server-side
	 * redirect of ShmPutImage); if the SHM allocation then fails, the
	 * scale is withdrawn and the buffers are made at native size.
	 */
	s->use_shm = shm_usable(s->dpy, d);
	s->rscale = rscale_negotiate(s);
	s->bw = s->w >> s->rscale;
	s->bh = s->h >> s->rscale;
	/* zero-copy: render scale granted (panel-size fullscreen), or a
	 * VidMode fullscreen - the server's QUERY decides */
	if (db && s->use_shm && zc_alloc(s) == 0)
		goto have_colour;
	s->use_shm = s->use_shm && alloc_shm(s, &s->buf[0]) == 0;
	if (!s->use_shm && s->rscale) {
		rscale_ask(s->dpy, s->win, 0, 0);
		s->rscale = 0;
		s->bw = s->w;
		s->bh = s->h;
	}
	if (!s->use_shm) {
		if (d->shm == 1 && glxi_trace())
			fprintf(stderr, "libGL: MIT-SHM attach failed (%s), "
				"using XPutImage\n", strerror(errno));
		if (alloc_plain(s, &s->buf[0]) != 0)
			return -1;
	}
	s->nbuf = 1;
	/* A second segment only helps a SHM double-buffered drawable: a
	 * single-buffered one must keep one persistent front buffer, and
	 * XPutImage has copied before it returns. A failure is not fatal. */
	if (s->use_shm && db &&
	    want_bufs((size_t)s->pitch * s->bh) == 2 &&
	    alloc_shm(s, &s->buf[1]) == 0)
		s->nbuf = 2;
have_colour:
	s->cur = 0;
	s->pixels = s->buf[0].pixels;
	/* The drawable's depth buffer, shared by every context current on it.
	 * calloc: 0 is the far plane, as the core's private buffer starts,
	 * and S31GL_DEPTH_TAIL zero bytes after it hold the core's depth
	 * epoch state (s31gl.h). A failure is not fatal: the core then
	 * allocates a private one. */
	s->depth = calloc((size_t)s->bw * s->bh * 2 + S31GL_DEPTH_TAIL, 1);
	s->last_w = s->w;
	s->last_h = s->h;
	s->last_nbuf = s->nbuf;
	s->n_alloc++;
	if (glxi_trace() || s->rscale)
		fprintf(stderr, "libGL: drawable 0x%lx %dx%d: buffers %dx%d%s%s\n",
			(unsigned long)s->win, s->w, s->h, s->bw, s->bh,
			s->rscale ? " (render scale 2x, server-scaled)" : "",
			s->zc ? (s->nbuf == 3 ? ", zero-copy x3" :
				 ", zero-copy x2") : "");
	return 0;
}

/* -------------------------------------------------------------- pacing */

/* S31GL_NOWAIT=1: never wait for the server's copy (debug only; see the
 * header - the window then shows frames that were never swapped). */
static int nowait(void)
{
	static int n = -1;

	if (n < 0)
		n = getenv("S31GL_NOWAIT") != NULL;
	return n;
}

struct pred_arg {
	Drawable win;
	int type;
};

/* Matches ONLY a ShmCompletion for this drawable: app events are never
 * taken. XShmCompletionEvent.drawable aliases xany.window in both Xlib and
 * xlite (which decodes an extension event's first word into xany.window). */
static Bool pred_completion(Display *dpy, XEvent *ev, XPointer arg)
{
	struct pred_arg *a = (struct pred_arg *)arg;

	(void)dpy;
	return ev->type == a->type && ev->xany.window == a->win;
}

/* Take every ShmCompletion for this drawable off the queue, clearing the
 * pending flag of each buffer whose put it proves consumed. */
static void drain_completions(struct glxi_surf *s)
{
	struct glxi_dpy *d = glxi_dpy_get(s->dpy);
	struct pred_arg a;
	XEvent ev;
	int i;

	a.win = s->win;
	a.type = d ? d->shm_event : -1;
	while (XCheckIfEvent(s->dpy, &ev, pred_completion, (XPointer)&a))
		for (i = 0; i < s->nbuf; i++)
			if (s->buf[i].pending &&
			    serial_at_or_after(ev.xany.serial, s->buf[i].pend_serial))
				s->buf[i].pending = 0;
}

/* Before the first write of a frame: the buffer about to be written must
 * no longer be read by the server. */
void glxi_surf_wait(struct glxi_surf *s)
{
	struct glxi_buf *b = &s->buf[s->cur];
	struct glxi_dpy *d;
	unsigned long sync_serial;

	if (!b->pending)
		return;
	if (s->zc) {
		zc_wait(s, b);
		return;
	}
	drain_completions(s);
	if (!b->pending) {
		s->n_wait_ev++;
		return;
	}
	if ((long)(LastKnownRequestProcessed(s->dpy) - b->pend_serial) >= 0) {
		b->pending = 0;
		s->n_wait_lkrp++;
		return;
	}
	/* 2b. one of our own round trips (an XSync here, a geometry query)
	 * already came back after the put. Usually the completion drain
	 * above has settled it by then; this covers the completion having
	 * been eaten by the app's loop in between, on xlite, where
	 * LastKnownRequestProcessed never moves (branch 2 cannot help). */
	d = glxi_dpy_get(s->dpy);
	if (d && d->proven_valid && (long)(d->proven - b->pend_serial) >= 0 &&
	    /* never from the future: a record outliving an xlite
	     * XCloseDisplay (no close hook) may meet a new connection at the
	     * same address, whose serials restart */
	    (long)(NextRequest(s->dpy) - d->proven) > 0) {
		b->pending = 0;
		s->n_wait_rt++;
		return;
	}
	sync_serial = NextRequest(s->dpy);
	XSync(s->dpy, False);
	glxi_dpy_proven(s->dpy, sync_serial);
	b->pending = 0;		/* the server has handled the put */
	drain_completions(s);	/* and leave no completion for the app */
	s->n_wait_sync++;
}

/*
 * Present the colour buffer. The core has finished the frame (TinyGL is
 * synchronous). The image is put at 0,0 at the BUFFER's size: if the window
 * grew and the app never called glViewport, the rest of the window keeps its
 * background, which is also what Mesa's xlib driver shows.
 *
 * Returns 1 when it switched to the other segment (S31GL_SHMBUFS=2): the
 * caller must rebind the core to s->pixels.
 */
int glxi_surf_present(struct glxi_surf *s)
{
	struct glxi_buf *b = &s->buf[s->cur];

	s->vp_asked = 0;	/* a new frame: glViewport may ask again */
	if (s->zc) {
		if (!b->zpix || !s->dpy)
			return 0;
		b->zseq = ++s->zseq;
		b->pending = !nowait();
		if (!b->pending)
			s->n_wait_none++;
		zc_send(s->dpy, s->win, b->zpix, ZC_OP_PRESENT, b->zseq);
		XFlush(s->dpy);	/* the flip runs while we compute */
		s->n_present++;
		s->cur = (s->cur + 1) % s->nbuf;
		s->pixels = s->buf[s->cur].pixels;
		return 1;
	}
	if (!b->img || !s->dpy)
		return 0;
	if (s->use_shm) {
		int ev = !nowait();

		b->pend_serial = NextRequest(s->dpy);
		XShmPutImage(s->dpy, s->win, s->gc, b->img, 0, 0, 0, 0,
			     s->bw, s->bh, ev);
		b->pending = ev;
		if (!ev)
			s->n_wait_none++;
	} else {
		XPutImage(s->dpy, s->win, s->gc, b->img, 0, 0, 0, 0,
			  s->bw, s->bh);
	}
	/* Real libXext does not flush after a put and xlite deliberately does
	 * not either (xlite_req.c: it would split SDL's put+XSync into two
	 * server wakeups). We have no XSync behind us, so flush: the copy
	 * then runs while the app computes its next frame. */
	XFlush(s->dpy);
	s->n_present++;
	if (s->nbuf == 2) {
		s->cur ^= 1;
		s->pixels = s->buf[s->cur].pixels;
		return 1;
	}
	return 0;
}

void glxi_surf_print_all(void)
{
	struct glxi_surf *q;

	for (q = surfs; q; q = q->next)
		print_stats(q);
}
