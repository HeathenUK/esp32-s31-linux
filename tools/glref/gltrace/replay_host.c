/*
 * replay_host.c - replay a gltrace on the host rig through GLX (Mesa or our
 * libGL, whichever libGL.so.1 LD_LIBRARY_PATH gives), under Xvfb
 * 800x480x16. For every full frame it reads the window back the way the
 * tracer did (XGetImage of the root window over a second connection),
 * hashes the RGB565 pixels and compares with the live hash in the trace.
 *
 *   qsreplay_host TRACE OUTDIR [--frames]
 *
 * Writes OUTDIR/hashes.txt ("frame replay-hash live-hash same|DIFF") and,
 * with --frames, OUTDIR/f<N>.raw (RGB565, top row first) for every full
 * frame. Prints one summary line. Exit 0 when every full frame's hash
 * matches (only meaningful against the library the trace was recorded
 * with), 1 otherwise, 2 on errors. s31, MIT.
 */
#define _GNU_SOURCE
#include "replay.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

static Display *dpy, *pdpy;
static const char *outdir;
static int dump;
static FILE *hf;
static long nfull, nsame, ndiff;
static long first_diff = -1;

#define MAXD 64
static struct { Window win; unsigned w, h; XVisualInfo *vi; } drw[MAXD + 1];
static GLXContext ctx[33];
static uint32_t ctx_share[33];
static int cur_ctx, cur_drw;

static XVisualInfo *pick_visual(int db, int depth, int stencil)
{
	int a[20], n = 0;
	a[n++] = GLX_RGBA;
	if (db)
		a[n++] = GLX_DOUBLEBUFFER;
	a[n++] = GLX_RED_SIZE; a[n++] = 5;
	a[n++] = GLX_GREEN_SIZE; a[n++] = 6;
	a[n++] = GLX_BLUE_SIZE; a[n++] = 5;
	a[n++] = GLX_DEPTH_SIZE; a[n++] = depth;
	a[n++] = GLX_STENCIL_SIZE; a[n++] = stencil;
	a[n++] = None;
	XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), a);
	if (!vi) {
		fprintf(stderr, "qsreplay_host: no visual for db %d depth %d stencil %d\n", db, depth, stencil);
		exit(2);
	}
	return vi;
}

static void newctx(uint32_t id, uint32_t share)
{
	if (id > 32)
		return;
	/* created lazily at the first MakeCurrent, when the config is known */
	if (ctx[id]) {
		glXDestroyContext(dpy, ctx[id]);
		ctx[id] = NULL;
	}
	ctx_share[id] = share;
}

static void delctx(uint32_t id)
{
	if (id > 32 || !ctx[id])
		return;
	if (cur_ctx == (int)id) {
		glXMakeCurrent(dpy, None, NULL);
		cur_ctx = 0;
	}
	glXDestroyContext(dpy, ctx[id]);
	ctx[id] = NULL;
}

static void on_ctx(const uint32_t *a)
{
	uint32_t c = a[0], d = a[1], w = a[2], h = a[3];
	if (c == 0 || d == 0) {
		glXMakeCurrent(dpy, None, NULL);
		cur_ctx = cur_drw = 0;
		return;
	}
	if (c > 32 || d > MAXD)
		return;
	if (!drw[d].win) {
		XSetWindowAttributes swa;
		XVisualInfo *vi = pick_visual((int)a[6], (int)a[4], (int)a[5]);
		swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
		swa.border_pixel = 0;
		swa.event_mask = StructureNotifyMask;
		drw[d].win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, w, h, 0, vi->depth,
					   InputOutput, vi->visual, CWBorderPixel | CWColormap | CWEventMask, &swa);
		drw[d].w = w;
		drw[d].h = h;
		drw[d].vi = vi;
		XMapWindow(dpy, drw[d].win);
		for (;;) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == MapNotify && ev.xmap.window == drw[d].win)
				break;
		}
	}
	if (!ctx[c]) {
		GLXContext sh = ctx_share[c] && ctx_share[c] <= 32 ? ctx[ctx_share[c]] : NULL;
		ctx[c] = glXCreateContext(dpy, drw[d].vi, sh, True);
		if (!ctx[c]) {
			fprintf(stderr, "qsreplay_host: glXCreateContext failed\n");
			exit(2);
		}
	}
	if (!glXMakeCurrent(dpy, drw[d].win, ctx[c])) {
		fprintf(stderr, "qsreplay_host: glXMakeCurrent failed\n");
		exit(2);
	}
	cur_ctx = (int)c;
	cur_drw = (int)d;
}

static uint32_t grab(uint32_t frame, unsigned *pw, unsigned *ph)
{
	XWindowAttributes wa;
	Window child;
	int x, y;
	uint32_t h = 2166136261u;
	Window win = drw[cur_drw].win;
	*pw = *ph = 0;
	if (!win || !XGetWindowAttributes(pdpy, win, &wa))
		return 0;
	XTranslateCoordinates(pdpy, win, DefaultRootWindow(pdpy), 0, 0, &x, &y, &child);
	XImage *img = XGetImage(pdpy, DefaultRootWindow(pdpy), x, y, (unsigned)wa.width,
				(unsigned)wa.height, AllPlanes, ZPixmap);
	if (!img)
		return 0;
	FILE *f = NULL;
	if (dump) {
		char p[1024];
		snprintf(p, sizeof p, "%s/f%u.raw", outdir, frame);
		f = fopen(p, "wb");
	}
	for (int yy = 0; yy < wa.height; yy++) {
		const uint16_t *row = (const uint16_t *)(img->data + (long)yy * img->bytes_per_line);
		for (int xx = 0; xx < wa.width; xx++) {
			h ^= row[xx];
			h *= 16777619u;
		}
		if (f)
			fwrite(row, 2, (size_t)wa.width, f);
	}
	if (f)
		fclose(f);
	XDestroyImage(img);
	*pw = (unsigned)wa.width;
	*ph = (unsigned)wa.height;
	return h;
}

static void on_swap(const uint32_t *a)
{
	uint32_t frame = a[0], fl = a[1], live = a[2];
	if (cur_drw)
		glXSwapBuffers(dpy, drw[cur_drw].win);
	if (!(fl & TRS_FULL))
		return;
	XSync(dpy, False);
	unsigned w, h;
	uint32_t got = grab(frame, &w, &h);
	int same = got == live;
	nfull++;
	if (same)
		nsame++;
	else {
		ndiff++;
		if (first_diff < 0)
			first_diff = frame;
	}
	fprintf(hf, "%u %08x %08x %s %s\n", frame, got, live, same ? "same" : "DIFF",
		fl & TRS_COUNT ? "count" : "warm");
}

static void on_unhandled(const char *n)
{
	fprintf(stderr, "qsreplay_host: UNHANDLED %s in the trace\n", n);
}

static int have(int k)
{
	return rp_fp[k] != NULL;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: qsreplay_host TRACE OUTDIR [--frames]\n");
		return 2;
	}
	outdir = argv[2];
	dump = argc > 3 && !strcmp(argv[3], "--frames");
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "qsreplay_host: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	dpy = XOpenDisplay(NULL);
	pdpy = XOpenDisplay(NULL);
	if (!dpy || !pdpy) {
		fprintf(stderr, "qsreplay_host: no display\n");
		return 2;
	}
	/* every entry point through GetProcAddress, then the export (ours
	   returns NULL for names it does not really implement) */
	for (int i = 0; i < rp_nnames; i++) {
		void *p = (void *)glXGetProcAddressARB((const GLubyte *)rp_names[i]);
		if (!p)
			p = dlsym(RTLD_DEFAULT, rp_names[i]);
		rp_fp[i] = p;
	}
	struct rp_trace t;
	if (rp_load(&t, buf, (size_t)n, have))
		return 2;
	char p[1024];
	snprintf(p, sizeof p, "%s/hashes.txt", outdir);
	hf = fopen(p, "w");
	if (!hf) {
		perror(p);
		return 2;
	}
	struct rp_platform pl = { newctx, delctx, on_ctx, on_swap, on_unhandled };
	rp_run(&t, &pl);
	fclose(hf);
	printf("qsreplay_host: %u frames, %ld full: %ld same as live, %ld differ%s (vendor %s)\n",
	       t.frames, nfull, nsame, ndiff, ndiff ? "" : " - bit-exact", (const char *)glGetString(GL_VENDOR));
	if (first_diff >= 0)
		printf("qsreplay_host: first differing frame %ld\n", first_diff);
	return ndiff ? 1 : 0;
}
