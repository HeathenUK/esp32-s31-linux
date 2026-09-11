/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The mechanical half of xlite: request encoders, and the small amount of
 * client-side bookkeeping X puts in the library rather than the server.
 *
 * Nothing here is clever. Each function packs its arguments into the wire
 * format and writes them; the interesting code is all in xlite.c. It is the
 * bulk of libX11 by symbol count and almost none of it by difficulty, which is
 * exactly why replacing the library is tractable at all.
 */
#include <sys/mman.h>
#include <unistd.h>
#include "xlite.h"

/* Defined with the XLITE-SHM cache below; used by the free/destroy paths. */
void xlite_shm_forget(Display *dpy, Drawable d);

static void p16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void p32(unsigned char *p, unsigned long v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static unsigned g16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long g32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) |
	       ((unsigned long)p[3] << 24);
}

/*
 * Our GC. The public prefix must match Xlib.h's struct _XGC exactly, because
 * clients read ext_data and gid; everything after it is ours. The cached
 * values matter more than they look - Xaw asked for them 22,985 times in one
 * xcalc startup, so answering from here rather than the server is the
 * difference between a working desktop and 23,000 round trips.
 */
struct xgc {
	XExtData *ext_data;
	GContext gid;
	XGCValues v;
};

#define REQ(dpy, op, det, words) \
	struct xdpy *x = XD(dpy); \
	unsigned char *r = xlite_req(x, op, det, words)

/* ----------------------------------------------------------------- atoms */

XLITE_IMPL(XInternAtom)
Atom XInternAtom(Display *dpy, const char *name, Bool only_if_exists)
{
	int n = strlen(name), words = 2 + (n + 3) / 4;
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;
	Atom a;

	{
		REQ(dpy, 16, only_if_exists ? 1 : 0, words);
		p16(r + 4, n);
		memcpy(r + 8, name, n);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return None;
	}
	a = g32(hdr + 8);
	free(extra);
	return a;
}

XLITE_IMPL(XGetAtomName)
char *XGetAtomName(Display *dpy, Atom a)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;
	char *out = NULL;

	{
		REQ(dpy, 17, 0, 2);
		p32(r + 4, a);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return NULL;
	}
	{
		unsigned len = g16(hdr + 8);

		out = malloc(len + 1);
		if (out) {
			if (extra && nextra >= len)
				memcpy(out, extra, len);
			else
				len = 0;
			out[len] = 0;
		}
	}
	free(extra);
	return out;
}

XLITE_IMPL(XInternAtoms)
Status XInternAtoms(Display *dpy, char **names, int count, Bool only_if_exists,
		    Atom *atoms)
{
	int i;

	for (i = 0; i < count; i++)
		atoms[i] = XInternAtom(dpy, names[i], only_if_exists);
	return 1;
}

/* --------------------------------------------------------------- windows */

XLITE_IMPL(XCreateWindow)
Window XCreateWindow(Display *dpy, Window parent, int px, int py,
		     unsigned w, unsigned h, unsigned bw, int depth,
		     unsigned class, Visual *vis, unsigned long mask,
		     XSetWindowAttributes *att)
{
	Window id = XAllocID(dpy);
	int nval = 0, i;
	unsigned long v[16];

	for (i = 0; i < 15; i++)
		if (mask & (1UL << i)) {
			unsigned long val = 0;

			switch (1UL << i) {
			case CWBackPixmap:   val = att->background_pixmap; break;
			case CWBackPixel:    val = att->background_pixel; break;
			case CWBorderPixmap: val = att->border_pixmap; break;
			case CWBorderPixel:  val = att->border_pixel; break;
			case CWBitGravity:   val = att->bit_gravity; break;
			case CWWinGravity:   val = att->win_gravity; break;
			case CWBackingStore: val = att->backing_store; break;
			case CWBackingPlanes:val = att->backing_planes; break;
			case CWBackingPixel: val = att->backing_pixel; break;
			case CWOverrideRedirect: val = att->override_redirect; break;
			case CWSaveUnder:    val = att->save_under; break;
			case CWEventMask:    val = att->event_mask; break;
			case CWDontPropagate:val = att->do_not_propagate_mask; break;
			case CWColormap:     val = att->colormap; break;
			case CWCursor:       val = att->cursor; break;
			}
			v[nval++] = val;
		}
	{
		REQ(dpy, 1, depth, 8 + nval);
		p32(r + 4, id);
		p32(r + 8, parent);
		p16(r + 12, px); p16(r + 14, py);
		p16(r + 16, w); p16(r + 18, h);
		p16(r + 20, bw); p16(r + 22, class);
		p32(r + 24, vis ? vis->visualid : 0);
		p32(r + 28, mask);
		for (i = 0; i < nval; i++)
			p32(r + 32 + i * 4, v[i]);
		xlite_send(x, r);
	}
	return id;
}

XLITE_IMPL(XCreateSimpleWindow)
Window XCreateSimpleWindow(Display *dpy, Window parent, int px, int py,
			   unsigned w, unsigned h, unsigned bw,
			   unsigned long border, unsigned long bg)
{
	XSetWindowAttributes a;

	memset(&a, 0, sizeof(a));
	a.border_pixel = border;
	a.background_pixel = bg;
	return XCreateWindow(dpy, parent, px, py, w, h, bw, CopyFromParent,
			     CopyFromParent, CopyFromParent,
			     CWBorderPixel | CWBackPixel, &a);
}

XLITE_IMPL(XChangeWindowAttributes)
int XChangeWindowAttributes(Display *dpy, Window win, unsigned long mask,
			    XSetWindowAttributes *att)
{
	int nval = 0, i;
	unsigned long v[16];

	for (i = 0; i < 15; i++)
		if (mask & (1UL << i)) {
			unsigned long val = 0;

			switch (1UL << i) {
			case CWBackPixmap:   val = att->background_pixmap; break;
			case CWBackPixel:    val = att->background_pixel; break;
			case CWBorderPixmap: val = att->border_pixmap; break;
			case CWBorderPixel:  val = att->border_pixel; break;
			case CWBitGravity:   val = att->bit_gravity; break;
			case CWWinGravity:   val = att->win_gravity; break;
			case CWBackingStore: val = att->backing_store; break;
			case CWBackingPlanes:val = att->backing_planes; break;
			case CWBackingPixel: val = att->backing_pixel; break;
			case CWOverrideRedirect: val = att->override_redirect; break;
			case CWSaveUnder:    val = att->save_under; break;
			case CWEventMask:    val = att->event_mask; break;
			case CWDontPropagate:val = att->do_not_propagate_mask; break;
			case CWColormap:     val = att->colormap; break;
			case CWCursor:       val = att->cursor; break;
			}
			v[nval++] = val;
		}
	{
		REQ(dpy, 2, 0, 3 + nval);
		p32(r + 4, win);
		p32(r + 8, mask);
		for (i = 0; i < nval; i++)
			p32(r + 12 + i * 4, v[i]);
		xlite_send(x, r);
	}
	return 1;
}

XLITE_IMPL(XSelectInput)
int XSelectInput(Display *dpy, Window w, long mask)
{
	XSetWindowAttributes a;

	memset(&a, 0, sizeof(a));
	a.event_mask = mask;
	return XChangeWindowAttributes(dpy, w, CWEventMask, &a);
}

static int simple_win(Display *dpy, int op, Window w)
{
	REQ(dpy, op, 0, 2);
	p32(r + 4, w);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XMapWindow)
int XMapWindow(Display *dpy, Window w) { return simple_win(dpy, 8, w); }
/*
 * Map and raise. SDL uses this rather than XMapWindow, so leaving it a stub
 * meant the surface was created, sized and never put on screen - prboom sat
 * idle at 0 ms of CPU behind a black window.
 */
XLITE_IMPL(XMapRaised)
int XMapRaised(Display *dpy, Window w)
{
	XRaiseWindow(dpy, w);
	return simple_win(dpy, 8, w);
}

/*
 * Colormaps, for the 8-bit paletted path.
 *
 * The shim has one visual and it is 16-bit TrueColor, so there is no colormap
 * A REAL colormap now, because there is a real palette behind it.
 *
 * This used to return a constant 1 and send nothing, on the reasoning that
 * with one TrueColor visual there was nothing to create. That reasoning
 * expired when the server grew a depth-8 PseudoColor visual: the colormap is
 * where an indexed client's palette LIVES, and a colormap the server has
 * never heard of cannot carry one. The visible result was Doom rendering in
 * perfect greyscale - the shim's placeholder ramp - because every colour the
 * game set was written to a handle that went nowhere.
 */
XLITE_IMPL(XCreateColormap)
Colormap XCreateColormap(Display *dpy, Window w, Visual *v, int alloc)
{
	Colormap id = XAllocID(dpy);
	REQ(dpy, 78, (uint8_t)(alloc ? 1 : 0), 4);

	p32(r + 4, id);
	p32(r + 8, w);
	p32(r + 12, v ? v->visualid : 0);
	xlite_send(x, r);
	return id;
}

/*
 * The palette itself: one COLORITEM per entry, 12 bytes each.
 *
 * Channels go on the wire as 16-bit, which is the protocol's width whatever
 * the visual's bits-per-rgb, and the flags byte says which of the three to
 * take (DoRed|DoGreen|DoBlue = 7). This was a generated do-nothing stub, so
 * an 8-bit client set its colours into thin air.
 */
XLITE_IMPL(XStoreColors)
int XStoreColors(Display *dpy, Colormap cmap, XColor *defs, int n)
{
	int i;

	if (!defs || n <= 0)
		return 1;
	{
		REQ(dpy, 89, 0, 2 + 3 * n);

		p32(r + 4, cmap);
		for (i = 0; i < n; i++) {
			uint8_t *it = r + 8 + i * 12;

			p32(it, (uint32_t)defs[i].pixel);
			p16(it + 4, defs[i].red);
			p16(it + 6, defs[i].green);
			p16(it + 8, defs[i].blue);
			it[10] = defs[i].flags ? defs[i].flags : 0x07;
			it[11] = 0;
		}
		xlite_send(x, r);
	}
	return 1;
}

XLITE_IMPL(XSetWindowColormap)
int XSetWindowColormap(Display *dpy, Window w, Colormap c)
{
	(void)dpy; (void)w; (void)c;
	return 1;
}

XLITE_IMPL(XInstallColormap)
int XInstallColormap(Display *dpy, Colormap c) { (void)dpy; (void)c; return 1; }
/* The pair matters more than the function does. The shim has a single
 * visual and no colormap machinery, so both of these are no-ops - but
 * SDL2 dlsyms a fixed list of 125 Xlib names and marks its ENTIRE X11
 * driver unavailable if one is missing, then falls back to the dummy
 * video driver and reports only "x11 not available". Absence of a
 * function a caller will never meaningfully use is still fatal. */
XLITE_IMPL(XUninstallColormap)
int XUninstallColormap(Display *dpy, Colormap c) { (void)dpy; (void)c; return 1; }

XLITE_IMPL(XMapSubwindows)
int XMapSubwindows(Display *dpy, Window w) { return simple_win(dpy, 9, w); }
XLITE_IMPL(XUnmapWindow)
int XUnmapWindow(Display *dpy, Window w) { return simple_win(dpy, 10, w); }
XLITE_IMPL(XUnmapSubwindows)
int XUnmapSubwindows(Display *dpy, Window w) { return simple_win(dpy, 11, w); }
XLITE_IMPL(XDestroyWindow)
int XDestroyWindow(Display *dpy, Window w)
{
	xlite_shm_forget(dpy, w);
	return simple_win(dpy, 4, w);
}
XLITE_IMPL(XDestroySubwindows)
int XDestroySubwindows(Display *dpy, Window w) { return simple_win(dpy, 5, w); }

XLITE_IMPL(XConfigureWindow)
int XConfigureWindow(Display *dpy, Window w, unsigned mask, XWindowChanges *ch)
{
	int nval = 0, i;
	unsigned long v[8];

	for (i = 0; i < 7; i++)
		if (mask & (1U << i)) {
			unsigned long val = 0;

			switch (1U << i) {
			case CWX:           val = ch->x; break;
			case CWY:           val = ch->y; break;
			case CWWidth:       val = ch->width; break;
			case CWHeight:      val = ch->height; break;
			case CWBorderWidth: val = ch->border_width; break;
			case CWSibling:     val = ch->sibling; break;
			case CWStackMode:   val = ch->stack_mode; break;
			}
			v[nval++] = val;
		}
	{
		REQ(dpy, 12, 0, 3 + nval);
		p32(r + 4, w);
		p16(r + 8, mask);
		for (i = 0; i < nval; i++)
			p32(r + 12 + i * 4, v[i]);
		xlite_send(x, r);
	}
	return 1;
}

XLITE_IMPL(XMoveWindow)
int XMoveWindow(Display *dpy, Window w, int px, int py)
{
	XWindowChanges c;

	c.x = px; c.y = py;
	return XConfigureWindow(dpy, w, CWX | CWY, &c);
}

XLITE_IMPL(XResizeWindow)
int XResizeWindow(Display *dpy, Window w, unsigned wd, unsigned ht)
{
	XWindowChanges c;

	c.width = wd; c.height = ht;
	return XConfigureWindow(dpy, w, CWWidth | CWHeight, &c);
}

XLITE_IMPL(XMoveResizeWindow)
int XMoveResizeWindow(Display *dpy, Window w, int px, int py, unsigned wd,
		      unsigned ht)
{
	XWindowChanges c;

	c.x = px; c.y = py; c.width = wd; c.height = ht;
	return XConfigureWindow(dpy, w, CWX | CWY | CWWidth | CWHeight, &c);
}

XLITE_IMPL(XRaiseWindow)
int XRaiseWindow(Display *dpy, Window w)
{
	XWindowChanges c;

	c.stack_mode = Above;
	return XConfigureWindow(dpy, w, CWStackMode, &c);
}

XLITE_IMPL(XLowerWindow)
int XLowerWindow(Display *dpy, Window w)
{
	XWindowChanges c;

	c.stack_mode = Below;
	return XConfigureWindow(dpy, w, CWStackMode, &c);
}

XLITE_IMPL(XClearArea)
int XClearArea(Display *dpy, Window w, int px, int py, unsigned wd,
	       unsigned ht, Bool exposures)
{
	REQ(dpy, 61, exposures ? 1 : 0, 4);
	p32(r + 4, w);
	p16(r + 8, px); p16(r + 10, py);
	p16(r + 12, wd); p16(r + 14, ht);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XClearWindow)
int XClearWindow(Display *dpy, Window w)
{
	return XClearArea(dpy, w, 0, 0, 0, 0, False);
}

/* ------------------------------------------------------------------- GCs */

XLITE_IMPL(XCreateGC)
GC XCreateGC(Display *dpy, Drawable d, unsigned long mask, XGCValues *val)
{
	GContext gid = XAllocID(dpy);
	/*
	 * The public struct is ext_data + gid; the real Xlib's is larger and
	 * private. Over-allocate so that nothing which was compiled against
	 * the full one can walk off the end of ours.
	 */
	GC gc = calloc(1, sizeof(struct xgc) > 256 ? sizeof(struct xgc) : 256);
	int nval = 0, i;
	unsigned long v[24];

	if (!gc)
		return NULL;
	gc->gid = gid;
	if (val)
		((struct xgc *)gc)->v = *val;
	for (i = 0; i < 23; i++)
		if (mask & (1UL << i)) {
			unsigned long q = 0;

			switch (1UL << i) {
			case GCFunction:      q = val->function; break;
			case GCPlaneMask:     q = val->plane_mask; break;
			case GCForeground:    q = val->foreground; break;
			case GCBackground:    q = val->background; break;
			case GCLineWidth:     q = val->line_width; break;
			case GCLineStyle:     q = val->line_style; break;
			case GCCapStyle:      q = val->cap_style; break;
			case GCJoinStyle:     q = val->join_style; break;
			case GCFillStyle:     q = val->fill_style; break;
			case GCFillRule:      q = val->fill_rule; break;
			case GCTile:          q = val->tile; break;
			case GCStipple:       q = val->stipple; break;
			case GCTileStipXOrigin: q = val->ts_x_origin; break;
			case GCTileStipYOrigin: q = val->ts_y_origin; break;
			case GCFont:          q = val->font; break;
			case GCSubwindowMode: q = val->subwindow_mode; break;
			case GCGraphicsExposures: q = val->graphics_exposures; break;
			case GCClipXOrigin:   q = val->clip_x_origin; break;
			case GCClipYOrigin:   q = val->clip_y_origin; break;
			case GCClipMask:      q = val->clip_mask; break;
			case GCDashOffset:    q = val->dash_offset; break;
			case GCDashList:      q = val->dashes; break;
			case GCArcMode:       q = val->arc_mode; break;
			}
			v[nval++] = q;
		}
	{
		REQ(dpy, 55, 0, 4 + nval);
		p32(r + 4, gid);
		p32(r + 8, d);
		p32(r + 12, mask);
		for (i = 0; i < nval; i++)
			p32(r + 16 + i * 4, v[i]);
		xlite_send(x, r);
	}
	return gc;
}

XLITE_IMPL(XChangeGC)
int XChangeGC(Display *dpy, GC gc, unsigned long mask, XGCValues *val)
{
	int nval = 0, i;
	unsigned long v[24];

	if (!gc)
		return 0;
	{	/* Keep the cache in step; XGetGCValues answers from it. */
		struct xgc *g = (struct xgc *)gc;
		XGCValues *d = &g->v;

		if (mask & GCFunction) d->function = val->function;
		if (mask & GCPlaneMask) d->plane_mask = val->plane_mask;
		if (mask & GCForeground) d->foreground = val->foreground;
		if (mask & GCBackground) d->background = val->background;
		if (mask & GCLineWidth) d->line_width = val->line_width;
		if (mask & GCLineStyle) d->line_style = val->line_style;
		if (mask & GCCapStyle) d->cap_style = val->cap_style;
		if (mask & GCJoinStyle) d->join_style = val->join_style;
		if (mask & GCFillStyle) d->fill_style = val->fill_style;
		if (mask & GCFillRule) d->fill_rule = val->fill_rule;
		if (mask & GCTile) d->tile = val->tile;
		if (mask & GCStipple) d->stipple = val->stipple;
		if (mask & GCTileStipXOrigin) d->ts_x_origin = val->ts_x_origin;
		if (mask & GCTileStipYOrigin) d->ts_y_origin = val->ts_y_origin;
		if (mask & GCFont) d->font = val->font;
		if (mask & GCSubwindowMode) d->subwindow_mode = val->subwindow_mode;
		if (mask & GCGraphicsExposures) d->graphics_exposures = val->graphics_exposures;
		if (mask & GCClipXOrigin) d->clip_x_origin = val->clip_x_origin;
		if (mask & GCClipYOrigin) d->clip_y_origin = val->clip_y_origin;
		if (mask & GCClipMask) d->clip_mask = val->clip_mask;
		if (mask & GCDashOffset) d->dash_offset = val->dash_offset;
		if (mask & GCDashList) d->dashes = val->dashes;
		if (mask & GCArcMode) d->arc_mode = val->arc_mode;
	}
	for (i = 0; i < 23; i++)
		if (mask & (1UL << i)) {
			unsigned long q = 0;

			switch (1UL << i) {
			case GCFunction:      q = val->function; break;
			case GCPlaneMask:     q = val->plane_mask; break;
			case GCForeground:    q = val->foreground; break;
			case GCBackground:    q = val->background; break;
			case GCLineWidth:     q = val->line_width; break;
			case GCLineStyle:     q = val->line_style; break;
			case GCCapStyle:      q = val->cap_style; break;
			case GCJoinStyle:     q = val->join_style; break;
			case GCFillStyle:     q = val->fill_style; break;
			case GCFillRule:      q = val->fill_rule; break;
			case GCTile:          q = val->tile; break;
			case GCStipple:       q = val->stipple; break;
			case GCTileStipXOrigin: q = val->ts_x_origin; break;
			case GCTileStipYOrigin: q = val->ts_y_origin; break;
			case GCFont:          q = val->font; break;
			case GCSubwindowMode: q = val->subwindow_mode; break;
			case GCGraphicsExposures: q = val->graphics_exposures; break;
			case GCClipXOrigin:   q = val->clip_x_origin; break;
			case GCClipYOrigin:   q = val->clip_y_origin; break;
			case GCClipMask:      q = val->clip_mask; break;
			case GCDashOffset:    q = val->dash_offset; break;
			case GCDashList:      q = val->dashes; break;
			case GCArcMode:       q = val->arc_mode; break;
			}
			v[nval++] = q;
		}
	{
		REQ(dpy, 56, 0, 3 + nval);
		p32(r + 4, gc->gid);
		p32(r + 8, mask);
		for (i = 0; i < nval; i++)
			p32(r + 12 + i * 4, v[i]);
		xlite_send(x, r);
	}
	return 1;
}

static int gc_one(Display *dpy, GC gc, unsigned long mask, XGCValues *v)
{
	return XChangeGC(dpy, gc, mask, v);
}

XLITE_IMPL(XSetForeground)
int XSetForeground(Display *dpy, GC gc, unsigned long p)
{
	XGCValues v; v.foreground = p; return gc_one(dpy, gc, GCForeground, &v);
}
XLITE_IMPL(XSetBackground)
int XSetBackground(Display *dpy, GC gc, unsigned long p)
{
	XGCValues v; v.background = p; return gc_one(dpy, gc, GCBackground, &v);
}
XLITE_IMPL(XSetFunction)
int XSetFunction(Display *dpy, GC gc, int f)
{
	XGCValues v; v.function = f; return gc_one(dpy, gc, GCFunction, &v);
}
XLITE_IMPL(XSetFont)
int XSetFont(Display *dpy, GC gc, Font f)
{
	XGCValues v; v.font = f; return gc_one(dpy, gc, GCFont, &v);
}
XLITE_IMPL(XSetFillStyle)
int XSetFillStyle(Display *dpy, GC gc, int s)
{
	XGCValues v; v.fill_style = s; return gc_one(dpy, gc, GCFillStyle, &v);
}
XLITE_IMPL(XSetSubwindowMode)
int XSetSubwindowMode(Display *dpy, GC gc, int m)
{
	XGCValues v; v.subwindow_mode = m;
	return gc_one(dpy, gc, GCSubwindowMode, &v);
}
XLITE_IMPL(XSetLineAttributes)
int XSetLineAttributes(Display *dpy, GC gc, unsigned w, int ls, int cs, int js)
{
	XGCValues v;

	v.line_width = w; v.line_style = ls; v.cap_style = cs; v.join_style = js;
	return gc_one(dpy, gc, GCLineWidth | GCLineStyle | GCCapStyle |
		      GCJoinStyle, &v);
}
XLITE_IMPL(XSetClipMask)
int XSetClipMask(Display *dpy, GC gc, Pixmap p)
{
	XGCValues v; v.clip_mask = p; return gc_one(dpy, gc, GCClipMask, &v);
}
XLITE_IMPL(XSetClipOrigin)
int XSetClipOrigin(Display *dpy, GC gc, int px, int py)
{
	XGCValues v;

	v.clip_x_origin = px; v.clip_y_origin = py;
	return gc_one(dpy, gc, GCClipXOrigin | GCClipYOrigin, &v);
}
XLITE_IMPL(XSetTSOrigin)
int XSetTSOrigin(Display *dpy, GC gc, int px, int py)
{
	XGCValues v;

	v.ts_x_origin = px; v.ts_y_origin = py;
	return gc_one(dpy, gc, GCTileStipXOrigin | GCTileStipYOrigin, &v);
}
XLITE_IMPL(XSetStipple)
int XSetStipple(Display *dpy, GC gc, Pixmap p)
{
	XGCValues v; v.stipple = p; return gc_one(dpy, gc, GCStipple, &v);
}

XLITE_IMPL(XFreeGC)
int XFreeGC(Display *dpy, GC gc)
{
	if (!gc)
		return 0;
	{
		REQ(dpy, 60, 0, 2);
		p32(r + 4, gc->gid);
		xlite_send(x, r);
	}
	free(gc);
	return 1;
}

XLITE_IMPL(XGContextFromGC)
GContext XGContextFromGC(GC gc) { return gc ? gc->gid : 0; }

XLITE_IMPL(XSetClipRectangles)
int XSetClipRectangles(Display *dpy, GC gc, int ox, int oy, XRectangle *rects,
		       int n, int order)
{
	int i;
	REQ(dpy, 59, order, 3 + n * 2);

	p32(r + 4, gc ? gc->gid : 0);
	p16(r + 8, ox); p16(r + 10, oy);
	for (i = 0; i < n; i++) {
		p16(r + 12 + i * 8, rects[i].x);
		p16(r + 14 + i * 8, rects[i].y);
		p16(r + 16 + i * 8, rects[i].width);
		p16(r + 18 + i * 8, rects[i].height);
	}
	xlite_send(x, r);
	return 1;
}

/* --------------------------------------------------------------- pixmaps */

XLITE_IMPL(XCreatePixmap)
Pixmap XCreatePixmap(Display *dpy, Drawable d, unsigned w, unsigned h,
		     unsigned depth)
{
	Pixmap id = XAllocID(dpy);
	REQ(dpy, 53, depth, 4);

	p32(r + 4, id);
	p32(r + 8, d);
	p16(r + 12, w); p16(r + 14, h);
	xlite_send(x, r);
	return id;
}

XLITE_IMPL(XFreePixmap)
int XFreePixmap(Display *dpy, Pixmap p)
{
	/* Drop any shared mapping first: after this the id may be reused. */
	xlite_shm_forget(dpy, p);
	{
	REQ(dpy, 54, 0, 2);
	p32(r + 4, p);
	xlite_send(x, r);
	return 1;
	}
}

/* --------------------------------------------------------------- drawing */

static void draw_pts(Display *dpy, int op, Drawable d, GC gc, XPoint *pts,
		     int n, int mode)
{
	int i;
	REQ(dpy, op, mode, 3 + n);

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	for (i = 0; i < n; i++) {
		p16(r + 12 + i * 4, pts[i].x);
		p16(r + 14 + i * 4, pts[i].y);
	}
	xlite_send(x, r);
}

XLITE_IMPL(XDrawLines)
int XDrawLines(Display *dpy, Drawable d, GC gc, XPoint *p, int n, int mode)
{
	draw_pts(dpy, 65, d, gc, p, n, mode);
	return 1;
}

XLITE_IMPL(XDrawPoints)
int XDrawPoints(Display *dpy, Drawable d, GC gc, XPoint *p, int n, int mode)
{
	draw_pts(dpy, 64, d, gc, p, n, mode);
	return 1;
}

XLITE_IMPL(XDrawPoint)
int XDrawPoint(Display *dpy, Drawable d, GC gc, int px, int py)
{
	XPoint p; p.x = px; p.y = py;
	draw_pts(dpy, 64, d, gc, &p, 1, CoordModeOrigin);
	return 1;
}

XLITE_IMPL(XDrawSegments)
int XDrawSegments(Display *dpy, Drawable d, GC gc, XSegment *s, int n)
{
	int i;
	REQ(dpy, 66, 0, 3 + n * 2);

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	for (i = 0; i < n; i++) {
		p16(r + 12 + i * 8, s[i].x1); p16(r + 14 + i * 8, s[i].y1);
		p16(r + 16 + i * 8, s[i].x2); p16(r + 18 + i * 8, s[i].y2);
	}
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XDrawLine)
int XDrawLine(Display *dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
	XSegment s;

	s.x1 = x1; s.y1 = y1; s.x2 = x2; s.y2 = y2;
	return XDrawSegments(dpy, d, gc, &s, 1);
}

static void rects(Display *dpy, int op, Drawable d, GC gc, XRectangle *rc, int n)
{
	int i;
	REQ(dpy, op, 0, 3 + n * 2);

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	for (i = 0; i < n; i++) {
		p16(r + 12 + i * 8, rc[i].x);
		p16(r + 14 + i * 8, rc[i].y);
		p16(r + 16 + i * 8, rc[i].width);
		p16(r + 18 + i * 8, rc[i].height);
	}
	xlite_send(x, r);
}

XLITE_IMPL(XFillRectangles)
int XFillRectangles(Display *dpy, Drawable d, GC gc, XRectangle *rc, int n)
{
	rects(dpy, 70, d, gc, rc, n);
	return 1;
}

XLITE_IMPL(XDrawRectangles)
int XDrawRectangles(Display *dpy, Drawable d, GC gc, XRectangle *rc, int n)
{
	rects(dpy, 67, d, gc, rc, n);
	return 1;
}

XLITE_IMPL(XFillRectangle)
int XFillRectangle(Display *dpy, Drawable d, GC gc, int px, int py,
		   unsigned w, unsigned h)
{
	XRectangle rc;

	rc.x = px; rc.y = py; rc.width = w; rc.height = h;
	return XFillRectangles(dpy, d, gc, &rc, 1);
}

XLITE_IMPL(XDrawRectangle)
int XDrawRectangle(Display *dpy, Drawable d, GC gc, int px, int py,
		   unsigned w, unsigned h)
{
	XRectangle rc;

	rc.x = px; rc.y = py; rc.width = w; rc.height = h;
	return XDrawRectangles(dpy, d, gc, &rc, 1);
}

XLITE_IMPL(XFillPolygon)
int XFillPolygon(Display *dpy, Drawable d, GC gc, XPoint *pts, int n,
		 int shape, int mode)
{
	int i;
	REQ(dpy, 69, 0, 4 + n);

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	r[12] = shape; r[13] = mode;
	for (i = 0; i < n; i++) {
		p16(r + 16 + i * 4, pts[i].x);
		p16(r + 18 + i * 4, pts[i].y);
	}
	xlite_send(x, r);
	return 1;
}

static void arcs(Display *dpy, int op, Drawable d, GC gc, XArc *a, int n)
{
	int i;
	REQ(dpy, op, 0, 3 + n * 3);

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	for (i = 0; i < n; i++) {
		p16(r + 12 + i * 12, a[i].x);
		p16(r + 14 + i * 12, a[i].y);
		p16(r + 16 + i * 12, a[i].width);
		p16(r + 18 + i * 12, a[i].height);
		p16(r + 20 + i * 12, a[i].angle1);
		p16(r + 22 + i * 12, a[i].angle2);
	}
	xlite_send(x, r);
}

XLITE_IMPL(XDrawArcs)
int XDrawArcs(Display *dpy, Drawable d, GC gc, XArc *a, int n)
{
	arcs(dpy, 68, d, gc, a, n);
	return 1;
}

XLITE_IMPL(XFillArcs)
int XFillArcs(Display *dpy, Drawable d, GC gc, XArc *a, int n)
{
	arcs(dpy, 71, d, gc, a, n);
	return 1;
}

XLITE_IMPL(XDrawArc)
int XDrawArc(Display *dpy, Drawable d, GC gc, int px, int py, unsigned w,
	     unsigned h, int a1, int a2)
{
	XArc a;

	a.x = px; a.y = py; a.width = w; a.height = h;
	a.angle1 = a1; a.angle2 = a2;
	return XDrawArcs(dpy, d, gc, &a, 1);
}

XLITE_IMPL(XFillArc)
int XFillArc(Display *dpy, Drawable d, GC gc, int px, int py, unsigned w,
	     unsigned h, int a1, int a2)
{
	XArc a;

	a.x = px; a.y = py; a.width = w; a.height = h;
	a.angle1 = a1; a.angle2 = a2;
	return XFillArcs(dpy, d, gc, &a, 1);
}

XLITE_IMPL(XCopyArea)
int XCopyArea(Display *dpy, Drawable src, Drawable dst, GC gc, int sx, int sy,
	      unsigned w, unsigned h, int dx, int dy)
{
	REQ(dpy, 62, 0, 7);

	p32(r + 4, src);
	p32(r + 8, dst);
	p32(r + 12, gc ? gc->gid : 0);
	p16(r + 16, sx); p16(r + 18, sy);
	p16(r + 20, dx); p16(r + 22, dy);
	p16(r + 24, w);  p16(r + 26, h);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XCopyPlane)
int XCopyPlane(Display *dpy, Drawable src, Drawable dst, GC gc, int sx, int sy,
	       unsigned w, unsigned h, int dx, int dy, unsigned long plane)
{
	REQ(dpy, 63, 0, 8);

	p32(r + 4, src);
	p32(r + 8, dst);
	p32(r + 12, gc ? gc->gid : 0);
	p16(r + 16, sx); p16(r + 18, sy);
	p16(r + 20, dx); p16(r + 22, dy);
	p16(r + 24, w);  p16(r + 26, h);
	p32(r + 28, plane);
	xlite_send(x, r);
	return 1;
}

/* ------------------------------------------------------------------ text */

XLITE_IMPL(XDrawString)
int XDrawString(Display *dpy, Drawable d, GC gc, int px, int py,
		const char *s, int n)
{
	int words = 4 + (n + 2 + 3) / 4;
	REQ(dpy, 74, 0, words);		/* PolyText8, one item */

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	p16(r + 12, px); p16(r + 14, py);
	r[16] = n;
	r[17] = 0;			/* delta */
	memcpy(r + 18, s, n);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XDrawImageString)
int XDrawImageString(Display *dpy, Drawable d, GC gc, int px, int py,
		     const char *s, int n)
{
	int words = 4 + (n + 3) / 4;
	REQ(dpy, 76, n, words);		/* ImageText8 */

	p32(r + 4, d);
	p32(r + 8, gc ? gc->gid : 0);
	p16(r + 12, px); p16(r + 14, py);
	memcpy(r + 16, s, n);
	xlite_send(x, r);
	return 1;
}

/* ---------------------------------------------------------------- fonts */

static XFontStruct *font_fetch(Display *dpy, Font fid)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;
	XFontStruct *fs;
	unsigned nch;
	int i;

	{
		REQ(dpy, 47, 0, 2);
		p32(r + 4, fid);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return NULL;
	}
	fs = calloc(1, sizeof(*fs));
	if (!fs) {
		free(extra);
		return NULL;
	}
	fs->fid = fid;
	/*
	 * The reply's fixed part is 60 bytes but a reply header is only 32, so
	 * the font's own metrics straddle the boundary: max-bounds starts at
	 * offset 24 and its DESCENT is the first thing in the extra data.
	 * Getting that split wrong put the attributes word into
	 * max_bounds.descent, and Xaw sizes a label from max_bounds - which is
	 * why xcalc's display bevel came out clipped while every glyph was
	 * correct.
	 *
	 *   min-bounds  CHARINFO  hdr+8 .. hdr+19
	 *   max-bounds  CHARINFO  hdr+24 .. extra+3
	 *   min/max-char-or-byte2 extra+8, extra+10
	 *   font-ascent/descent   extra+20, extra+22
	 *   CHARINFO count        extra+24, then m FONTPROPs then the array
	 */
	fs->min_bounds.lbearing = (short)g16(hdr + 8);
	fs->min_bounds.rbearing = (short)g16(hdr + 10);
	fs->min_bounds.width = (short)g16(hdr + 12);
	fs->min_bounds.ascent = (short)g16(hdr + 14);
	fs->min_bounds.descent = (short)g16(hdr + 16);
	fs->max_bounds.lbearing = (short)g16(hdr + 24);
	fs->max_bounds.rbearing = (short)g16(hdr + 26);
	fs->max_bounds.width = (short)g16(hdr + 28);
	fs->max_bounds.ascent = (short)g16(hdr + 30);
	if (extra && nextra >= 28) {
		unsigned m;

		fs->max_bounds.descent = (short)g16(extra + 0);
		fs->min_char_or_byte2 = g16(extra + 8);
		fs->max_char_or_byte2 = g16(extra + 10);
		fs->default_char = g16(extra + 12);
		m = g16(extra + 14);		/* FONTPROPs, skipped over */
		fs->direction = extra[16];
		fs->min_byte1 = extra[17];
		fs->max_byte1 = extra[18];
		fs->all_chars_exist = extra[19];
		fs->ascent = (short)g16(extra + 20);
		fs->descent = (short)g16(extra + 22);
		nch = g32(extra + 24);
		if (nch && nextra >= 28 + (size_t)m * 8 + (size_t)nch * 12) {
			const unsigned char *base = extra + 28 + m * 8;

			fs->per_char = calloc(nch, sizeof(XCharStruct));
			if (fs->per_char)
				for (i = 0; i < (int)nch; i++) {
					const unsigned char *c = base + i * 12;

					fs->per_char[i].lbearing = (short)g16(c);
					fs->per_char[i].rbearing = (short)g16(c + 2);
					fs->per_char[i].width = (short)g16(c + 4);
					fs->per_char[i].ascent = (short)g16(c + 6);
					fs->per_char[i].descent = (short)g16(c + 8);
				}
		}
	}
	free(extra);
	return fs;
}

XLITE_IMPL(XLoadFont)
Font XLoadFont(Display *dpy, const char *name)
{
	Font fid = XAllocID(dpy);
	int n = strlen(name);
	REQ(dpy, 45, 0, 3 + (n + 3) / 4);

	p32(r + 4, fid);
	p16(r + 8, n);
	memcpy(r + 12, name, n);
	xlite_send(x, r);
	return fid;
}

/*
 * Fonts are cached for the life of the process, by name and by id.
 *
 * An XFontStruct is not small: the per-character metrics of a 256-glyph font
 * are 3,072 bytes on their own, and Xaw asks for the font of every widget it
 * builds. xcalc has 70 widgets and every one of them names the same two
 * fonts, so the uncached version held 213,852 bytes of duplicate metrics -
 * 60% of the client's entire heap - and paid 140 synchronous round trips to
 * fetch them. Measured with rootfs/mallocprof.c; see docs/xlite.md.
 *
 * Nothing frees these. A client has a handful of fonts and keeps using them,
 * so the cache IS the lifetime, and XFreeFont becomes a no-op rather than a
 * hazard: the struct it was handed is shared with every other widget.
 */
#define NFCACHE	16

static struct fcache {
	char name[48];
	Font fid;
	XFontStruct *fs;
} fcache[NFCACHE];
static int nfcache;

static XFontStruct *font_by_id(Display *dpy, Font fid)
{
	int i;

	for (i = 0; i < nfcache; i++)
		if (fcache[i].fid == fid)
			return fcache[i].fs;
	{
		XFontStruct *fs = font_fetch(dpy, fid);

		if (fs && nfcache < NFCACHE) {
			fcache[nfcache].fid = fid;
			fcache[nfcache].fs = fs;
			nfcache++;
		}
		return fs;
	}
}

/*
 * The i18n form of XSetWMProperties. Everything it carries beyond the window
 * name is for a window manager that reads it, and lvdesk reads WM_NAME - so
 * the name is set and the rest is dropped rather than the call failing.
 */
XLITE_IMPL(XSetWindowBackgroundPixmap)
int XSetWindowBackgroundPixmap(Display *dpy, Window w, Pixmap px)
{
	REQ(dpy, 2, 0, 4);		/* ChangeWindowAttributes */

	p32(r + 4, w);
	p32(r + 8, 0x00000001);		/* CWBackPixmap */
	p32(r + 12, px);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XmbSetWMProperties)
void XmbSetWMProperties(Display *dpy, Window w, const char *window_name,
			const char *icon_name, char **argv, int argc,
			XSizeHints *normal_hints, XWMHints *wm_hints,
			XClassHint *class_hints)
{
	(void)icon_name; (void)argv; (void)argc; (void)wm_hints;
	if (window_name)
		XStoreName(dpy, w, window_name);
	/*
	 * Forward the size hints. Dropping them is invisible until something
	 * asks the question they answer: a client that declares min == max is
	 * saying it cannot be resized, and a window manager that never receives
	 * that will happily maximise it and leave a band the client can never
	 * fill. xfiles passes its XSizeHints through THIS call rather than
	 * XSetWMNormalHints, so discarding them here silently disabled the
	 * whole mechanism for the one client that uses it.
	 */
	if (normal_hints)
		XSetWMNormalHints(dpy, w, normal_hints);
	if (class_hints)
		XSetClassHint(dpy, w, class_hints);
}

XLITE_IMPL(XNextRequest)
unsigned long XNextRequest(Display *dpy)
{
	return dpy->request + 1;
}

XLITE_IMPL(XQueryFont)
XFontStruct *XQueryFont(Display *dpy, XID fid) { return font_by_id(dpy, fid); }

XLITE_IMPL(XLoadQueryFont)
XFontStruct *XLoadQueryFont(Display *dpy, const char *name)
{
	int i;

	for (i = 0; i < nfcache; i++)
		if (fcache[i].name[0] && !strcmp(fcache[i].name, name))
			return fcache[i].fs;
	{
		Font fid = XLoadFont(dpy, name);
		XFontStruct *fs = fid ? font_fetch(dpy, fid) : NULL;

		if (fs && nfcache < NFCACHE) {
			snprintf(fcache[nfcache].name,
				 sizeof(fcache[nfcache].name), "%s", name);
			fcache[nfcache].fid = fid;
			fcache[nfcache].fs = fs;
			nfcache++;
		} else if (fs) {
			xlite_note("font cache full, %s uncached", name);
		}
		return fs;
	}
}

XLITE_IMPL(XFreeFont)
int XFreeFont(Display *dpy, XFontStruct *fs)
{
	int i;

	(void)dpy;
	if (!fs)
		return 0;
	for (i = 0; i < nfcache; i++)
		if (fcache[i].fs == fs)
			return 1;	/* shared, and still in use */
	free(fs->per_char);
	free(fs);
	return 1;
}

XLITE_IMPL(XUnloadFont)
int XUnloadFont(Display *dpy, Font f)
{
	REQ(dpy, 46, 0, 2);
	p32(r + 4, f);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XFreeFontInfo)
int XFreeFontInfo(char **names, XFontStruct *info, int n)
{
	(void)names; (void)info; (void)n;
	return 1;
}

/* Client-side metrics, which is what a monospace or per-char font allows. */
static const XCharStruct *cs_of(XFontStruct *fs, unsigned c)
{
	if (!fs)
		return NULL;
	if (!fs->per_char)
		return &fs->max_bounds;
	if (c < fs->min_char_or_byte2 || c > fs->max_char_or_byte2)
		return &fs->max_bounds;
	return &fs->per_char[c - fs->min_char_or_byte2];
}

XLITE_IMPL(XTextWidth)
int XTextWidth(XFontStruct *fs, const char *s, int n)
{
	int i, w = 0;

	for (i = 0; i < n; i++)
		w += cs_of(fs, (unsigned char)s[i])->width;
	return w;
}

XLITE_IMPL(XTextExtents)
int XTextExtents(XFontStruct *fs, const char *s, int n, int *dir, int *asc,
		 int *desc, XCharStruct *ov)
{
	int i;

	if (dir)
		*dir = FontLeftToRight;
	if (asc)
		*asc = fs ? fs->ascent : 0;
	if (desc)
		*desc = fs ? fs->descent : 0;
	if (ov) {
		memset(ov, 0, sizeof(*ov));
		ov->ascent = fs ? fs->ascent : 0;
		ov->descent = fs ? fs->descent : 0;
		for (i = 0; i < n; i++)
			ov->width += cs_of(fs, (unsigned char)s[i])->width;
		ov->rbearing = ov->width;
	}
	return 0;
}

/* ------------------------------------------------------------ properties */

XLITE_IMPL(XChangeProperty)
int XChangeProperty(Display *dpy, Window w, Atom prop, Atom type, int fmt,
		    int mode, const unsigned char *data, int nelem)
{
	int bytes = nelem * (fmt / 8);
	REQ(dpy, 18, mode, 6 + (bytes + 3) / 4);

	p32(r + 4, w);
	p32(r + 8, prop);
	p32(r + 12, type);
	r[16] = fmt;
	p32(r + 20, nelem);
	if (bytes > 0)
		memcpy(r + 24, data, bytes);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XDeleteProperty)
int XDeleteProperty(Display *dpy, Window w, Atom prop)
{
	REQ(dpy, 19, 0, 3);
	p32(r + 4, w);
	p32(r + 8, prop);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XGetWindowProperty)
int XGetWindowProperty(Display *dpy, Window w, Atom prop, long off, long len,
		       Bool del, Atom req_type, Atom *type, int *fmt,
		       unsigned long *nitems, unsigned long *after,
		       unsigned char **data)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	{
		REQ(dpy, 20, del ? 1 : 0, 6);
		p32(r + 4, w);
		p32(r + 8, prop);
		p32(r + 12, req_type);
		p32(r + 16, off);
		p32(r + 20, len);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra)) {
			*type = None; *fmt = 0; *nitems = 0; *after = 0;
			*data = NULL;
			return BadImplementation;
		}
	}
	*fmt = hdr[1];
	*type = g32(hdr + 8);
	*after = g32(hdr + 12);
	*nitems = g32(hdr + 16);
	if (*nitems && extra) {
		size_t n = *nitems * (*fmt / 8);

		*data = malloc(n + 1);
		if (*data) {
			memcpy(*data, extra, n < nextra ? n : nextra);
			(*data)[n] = 0;
		}
	} else {
		*data = NULL;
	}
	free(extra);
	return Success;
}

/* --------------------------------------------------------------- colours */

XLITE_IMPL(XAllocColor)
Status XAllocColor(Display *dpy, Colormap cmap, XColor *c)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	{
		REQ(dpy, 84, 0, 4);
		p32(r + 4, cmap);
		p16(r + 8, c->red); p16(r + 10, c->green); p16(r + 12, c->blue);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return 0;
	}
	c->red = g16(hdr + 8);
	c->green = g16(hdr + 10);
	c->blue = g16(hdr + 12);
	c->pixel = g32(hdr + 16);
	free(extra);
	return 1;
}

XLITE_IMPL(XAllocNamedColor)
Status XAllocNamedColor(Display *dpy, Colormap cmap, const char *name,
			XColor *screen_def, XColor *exact_def)
{
	int n = strlen(name);
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	{
		REQ(dpy, 85, 0, 3 + (n + 3) / 4);
		p32(r + 4, cmap);
		p16(r + 8, n);
		memcpy(r + 12, name, n);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return 0;
	}
	screen_def->pixel = g32(hdr + 8);
	screen_def->red = g16(hdr + 12);
	screen_def->green = g16(hdr + 14);
	screen_def->blue = g16(hdr + 16);
	*exact_def = *screen_def;
	free(extra);
	return 1;
}

XLITE_IMPL(XParseColor)
Status XParseColor(Display *dpy, Colormap cmap, const char *spec, XColor *c)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;
	int n = strlen(spec);

	{
		REQ(dpy, 92, 0, 3 + (n + 3) / 4);	/* LookupColor */
		p32(r + 4, cmap);
		p16(r + 8, n);
		memcpy(r + 12, spec, n);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return 0;
	}
	c->red = g16(hdr + 8);
	c->green = g16(hdr + 10);
	c->blue = g16(hdr + 12);
	c->flags = DoRed | DoGreen | DoBlue;
	free(extra);
	return 1;
}

XLITE_IMPL(XFreeColors)
int XFreeColors(Display *dpy, Colormap c, unsigned long *px, int n,
		unsigned long planes)
{
	(void)dpy; (void)c; (void)px; (void)n; (void)planes;
	return 1;			/* nothing is allocated to free */
}

/* --------------------------------------------------------------- regions */

/*
 * Regions: our arithmetic, X's LAYOUT.
 *
 * Keeping a region as its bounding box is a deliberate simplification. Xt uses
 * regions to accumulate exposed area and then clips redraws to them, and a
 * bounding box is always a superset - the client repaints a little more than
 * it strictly must and the screen is identical. Exact region arithmetic is
 * several hundred lines to save repainting a few rectangles on an 800x480
 * panel.
 *
 * The STRUCT is not ours to simplify, though. `Region` is opaque in Xlib.h,
 * but libXrender includes X's private Xregion.h and reads the fields directly:
 *
 *     typedef struct _XRegion { long size; long numRects; BOX *rects;
 *                               BOX extents; } REGION;
 *
 * A bare `{int x1, y1, x2, y2;}` puts our x2 exactly where libXrender expects
 * the `rects` POINTER, so XRenderSetPictureClipRegion() dereferenced a clip
 * coordinate: `unhandled signal 11 ... at 0x00000077` - 0x77 is 119, the right
 * edge of the box. Same trap as Xlibint.h, and the same answer: use the real
 * layout and keep the cheap behaviour inside it.
 */
struct xlite_region {
	REGION pub;			/* MUST be first: libXrender reads it */
	BOX box;			/* the single rect pub.rects points at */
};

/*
 * Publish the box through the fields an extension library reads. Called after
 * every mutation - including whole-struct copies, which would otherwise leave
 * `rects` pointing at the region it was copied FROM.
 */
static void region_sync(struct xlite_region *g)
{
	int empty = g->box.x2 <= g->box.x1 || g->box.y2 <= g->box.y1;

	g->pub.size = 1;
	g->pub.rects = &g->box;
	g->pub.numRects = empty ? 0 : 1;
	if (empty)
		memset(&g->box, 0, sizeof(g->box));
	g->pub.extents = g->box;
}

XLITE_IMPL(XCreateRegion)
Region XCreateRegion(void)
{
	struct xlite_region *r = calloc(1, sizeof(*r));

	if (r)
		region_sync(r);
	return (Region)r;
}

XLITE_IMPL(XDestroyRegion)
int XDestroyRegion(Region r) { free(r); return 1; }

XLITE_IMPL(XEmptyRegion)
Bool XEmptyRegion(Region r)
{
	struct xlite_region *g = (struct xlite_region *)r;

	return !g || g->box.x2 <= g->box.x1 || g->box.y2 <= g->box.y1;
}

XLITE_IMPL(XClipBox)
int XClipBox(Region r, XRectangle *rect)
{
	struct xlite_region *g = (struct xlite_region *)r;

	if (!g || XEmptyRegion(r)) {
		memset(rect, 0, sizeof(*rect));
		return 1;
	}
	rect->x = g->box.x1; rect->y = g->box.y1;
	rect->width = g->box.x2 - g->box.x1;
	rect->height = g->box.y2 - g->box.y1;
	return 1;
}

XLITE_IMPL(XUnionRectWithRegion)
int XUnionRectWithRegion(XRectangle *rect, Region src, Region dst)
{
	struct xlite_region *s = (struct xlite_region *)src;
	struct xlite_region *d = (struct xlite_region *)dst;
	BOX t;

	if (!d)
		return 0;
	t = s ? s->box : d->box;
	if (rect->width && rect->height) {
		int x2 = rect->x + rect->width, y2 = rect->y + rect->height;

		if (t.x2 <= t.x1 || t.y2 <= t.y1) {
			t.x1 = rect->x; t.y1 = rect->y;
			t.x2 = x2; t.y2 = y2;
		} else {
			if (rect->x < t.x1) t.x1 = rect->x;
			if (rect->y < t.y1) t.y1 = rect->y;
			if (x2 > t.x2) t.x2 = x2;
			if (y2 > t.y2) t.y2 = y2;
		}
	}
	d->box = t;
	region_sync(d);
	return 1;
}

XLITE_IMPL(XUnionRegion)
int XUnionRegion(Region a, Region b, Region dst)
{
	struct xlite_region *p = (struct xlite_region *)a;
	struct xlite_region *q = (struct xlite_region *)b;
	struct xlite_region *d = (struct xlite_region *)dst;
	XRectangle rc;

	if (!d)
		return 0;
	if (p)
		d->box = p->box;
	else
		memset(&d->box, 0, sizeof(d->box));
	region_sync(d);
	if (q && !XEmptyRegion(b)) {
		rc.x = q->box.x1; rc.y = q->box.y1;
		rc.width = q->box.x2 - q->box.x1;
		rc.height = q->box.y2 - q->box.y1;
		XUnionRectWithRegion(&rc, dst, dst);
	}
	return 1;
}

XLITE_IMPL(XIntersectRegion)
int XIntersectRegion(Region a, Region b, Region dst)
{
	struct xlite_region *p = (struct xlite_region *)a;
	struct xlite_region *q = (struct xlite_region *)b;
	struct xlite_region *d = (struct xlite_region *)dst;

	if (!d)
		return 0;
	if (!p || !q) {
		memset(&d->box, 0, sizeof(d->box));
		region_sync(d);
		return 1;
	}
	d->box.x1 = p->box.x1 > q->box.x1 ? p->box.x1 : q->box.x1;
	d->box.y1 = p->box.y1 > q->box.y1 ? p->box.y1 : q->box.y1;
	d->box.x2 = p->box.x2 < q->box.x2 ? p->box.x2 : q->box.x2;
	d->box.y2 = p->box.y2 < q->box.y2 ? p->box.y2 : q->box.y2;
	if (d->box.x2 < d->box.x1) d->box.x2 = d->box.x1;
	if (d->box.y2 < d->box.y1) d->box.y2 = d->box.y1;
	region_sync(d);
	return 1;
}

XLITE_IMPL(XSubtractRegion)
int XSubtractRegion(Region a, Region b, Region dst)
{
	struct xlite_region *p = (struct xlite_region *)a;
	struct xlite_region *d = (struct xlite_region *)dst;

	(void)b;			/* a bounding box cannot lose a hole */
	if (d && p) {
		d->box = p->box;
		region_sync(d);
	}
	return 1;
}

XLITE_IMPL(XOffsetRegion)
int XOffsetRegion(Region r, int dx, int dy)
{
	struct xlite_region *g = (struct xlite_region *)r;

	if (g) {
		g->box.x1 += dx; g->box.x2 += dx;
		g->box.y1 += dy; g->box.y2 += dy;
		region_sync(g);
	}
	return 1;
}

XLITE_IMPL(XPointInRegion)
Bool XPointInRegion(Region r, int px, int py)
{
	struct xlite_region *g = (struct xlite_region *)r;

	return g && px >= g->box.x1 && px < g->box.x2 &&
	       py >= g->box.y1 && py < g->box.y2;
}

XLITE_IMPL(XRectInRegion)
int XRectInRegion(Region r, int px, int py, unsigned w, unsigned h)
{
	struct xlite_region *g = (struct xlite_region *)r;

	if (!g || XEmptyRegion(r))
		return RectangleOut;
	if ((int)(px + w) <= g->box.x1 || px >= g->box.x2 ||
	    (int)(py + h) <= g->box.y1 || py >= g->box.y2)
		return RectangleOut;
	if (px >= g->box.x1 && (int)(px + w) <= g->box.x2 &&
	    py >= g->box.y1 && (int)(py + h) <= g->box.y2)
		return RectangleIn;
	return RectanglePart;
}

XLITE_IMPL(XEqualRegion)
Bool XEqualRegion(Region a, Region b)
{
	struct xlite_region *p = (struct xlite_region *)a;
	struct xlite_region *q = (struct xlite_region *)b;

	return p && q && !memcmp(&p->box, &q->box, sizeof(p->box));
}

XLITE_IMPL(XSetRegion)
int XSetRegion(Display *dpy, GC gc, Region r)
{
	XRectangle rc;

	XClipBox(r, &rc);
	return XSetClipRectangles(dpy, gc, 0, 0, &rc, 1, Unsorted);
}

/* ----------------------------------------------------------------- misc */

XLITE_IMPL(XGetGeometry)
Status XGetGeometry(Display *dpy, Drawable d, Window *root, int *px, int *py,
		    unsigned *w, unsigned *h, unsigned *bw, unsigned *depth)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	{
		REQ(dpy, 14, 0, 2);
		p32(r + 4, d);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return 0;
	}
	if (depth) *depth = hdr[1];
	if (root) *root = g32(hdr + 8);
	if (px) *px = (short)g16(hdr + 12);
	if (py) *py = (short)g16(hdr + 14);
	if (w) *w = g16(hdr + 16);
	if (h) *h = g16(hdr + 18);
	if (bw) *bw = g16(hdr + 20);
	free(extra);
	return 1;
}

/*
 * XGetWindowAttributes is TWO requests, and that is not a detail.
 *
 * GetWindowAttributes (3) carries the visual, class, gravities, map state,
 * colormap and event masks. It does NOT carry x, y, width, height, depth or
 * root - those live in GetGeometry (14). Real Xlib issues both and merges
 * them, so a client that asks for its own size gets it from the geometry
 * half. The shim already answered request 3 (for Qt), but xlite never sent
 * either, and the generic stub returned 0 WITHOUT TOUCHING the struct.
 *
 * A caller that does not check the return value then reads its own
 * uninitialised stack as the window size. SDL2 does exactly that in
 * SetupWindowData, which is how a 320x200 window became -1657983272x1 and
 * sent SDL2 on to ask the kernel for 979 MB. The allocation failed, the
 * software renderer failed with it, and SDL_CreateRenderer reported
 * "Couldn't find matching render driver" - a message about flags, for a
 * fault that had nothing to do with flags. Filling the struct is the fix;
 * returning a status nobody reads is not.
 */
XLITE_IMPL(XGetWindowAttributes)
Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *a)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	unsigned gw = 0, gh = 0, gbw = 0, gdepth = 0;
	int gx = 0, gy = 0;
	Window root = 0;
	uint32_t seq;

	if (!a)
		return 0;
	memset(a, 0, sizeof *a);

	/* geometry half first: it is the half callers actually use */
	if (!XGetGeometry(dpy, w, &root, &gx, &gy, &gw, &gh, &gbw, &gdepth))
		return 0;
	a->x = gx;
	a->y = gy;
	a->width = (int)gw;
	a->height = (int)gh;
	a->border_width = (int)gbw;
	a->depth = (int)gdepth;
	a->root = root;
	a->visual = DefaultVisual(dpy, 0);
	a->screen = DefaultScreenOfDisplay(dpy);

	{
		REQ(dpy, 3, 0, 2);
		p32(r + 4, w);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra)) {
			/* geometry is still valid and is what callers read */
			a->class = InputOutput;
			a->map_state = IsViewable;
			return 1;
		}
	}
	a->backing_store        = hdr[1];
	a->class                = g16(hdr + 12);
	a->bit_gravity          = hdr[14];
	a->win_gravity          = hdr[15];
	a->backing_planes       = g32(hdr + 16);
	a->backing_pixel        = g32(hdr + 20);
	a->save_under           = hdr[24];
	a->map_installed        = hdr[25];
	a->map_state            = hdr[26];
	a->override_redirect    = hdr[27];
	a->colormap             = g32(hdr + 28);
	if (extra && nextra >= 12) {
		a->all_event_masks      = (long)g32(extra + 0);
		a->your_event_mask      = (long)g32(extra + 4);
		a->do_not_propagate_mask = (long)g16(extra + 8);
	}
	free(extra);
	return 1;
}

/* Same trap, smaller blast radius: the out-parameters must be written. */
XLITE_IMPL(XGetInputFocus)
int XGetInputFocus(Display *dpy, Window *focus, int *revert_to)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	if (focus) *focus = None;
	if (revert_to) *revert_to = RevertToNone;
	{
		REQ(dpy, 43, 0, 1);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return 0;
	}
	if (revert_to) *revert_to = hdr[1];
	if (focus) *focus = g32(hdr + 8);
	free(extra);
	return 1;
}

XLITE_IMPL(XSendEvent)
Status XSendEvent(Display *dpy, Window w, Bool propagate, long mask,
		  XEvent *ev)
{
	REQ(dpy, 25, propagate ? 1 : 0, 11);

	p32(r + 4, w);
	p32(r + 8, mask);
	/* Only the fields a client actually round-trips are re-encoded. */
	r[12] = ev->type;
	if (ev->type == ClientMessage) {
		r[13] = ev->xclient.format;
		p32(r + 16, ev->xclient.window);
		p32(r + 20, ev->xclient.message_type);
		memcpy(r + 24, ev->xclient.data.b, 20);
	}
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XNoOp)
int XNoOp(Display *dpy)
{
	REQ(dpy, 127, 0, 1);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XSynchronize)
int (*XSynchronize(Display *dpy, Bool on))(Display *)
{
	(void)dpy; (void)on;
	return NULL;			/* every request is already written */
}

/*
 * Extension bookkeeping.
 *
 * XAddExtension() is a client registering hooks of its own, so a fresh record
 * is the right answer. XInitExtension() is a LIBRARY asking whether the server
 * has an extension, and the answer here is always no - so it has to actually
 * ask, and return NULL.
 *
 * Returning a record with major_opcode 0, as this used to, is worse than
 * useless: libXrender's RenderCheckExtension() passes on a non-NULL codes
 * pointer, so every Render call was built and sent with request type 0. The
 * shim logged "UNIMPLEMENTED ? (opcode 0)" and the client saw BadImplementation
 * from calls it had no reason to think would fail. An honest NULL makes
 * libXrender, libXft, libXcursor and libXfixes disable themselves on the paths
 * they already have for a server without RENDER.
 */
XLITE_IMPL(XAddExtension)
XExtCodes *XAddExtension(Display *dpy)
{
	static int next = 128;
	XExtCodes *c = calloc(1, sizeof(*c));

	(void)dpy;
	if (c) {
		c->extension = next++;
		c->major_opcode = 0;
	}
	return c;
}

XLITE_IMPL(XQueryExtension)
Bool XQueryExtension(Display *dpy, const char *name, int *major,
		     int *first_event, int *first_error)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0, n = strlen(name);
	uint32_t seq;

	*major = *first_event = *first_error = 0;
	{
		REQ(dpy, 98, 0, 2 + (int)((n + 3) / 4));
		p16(r + 4, n);
		memcpy(r + 8, name, n);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return False;
	}
	free(extra);
	if (!hdr[8]) {
		xlite_note("extension %s: not present", name);
		return False;
	}
	*major = hdr[9];
	*first_event = hdr[10];
	*first_error = hdr[11];
	return True;
}

XLITE_IMPL(XInitExtension)
XExtCodes *XInitExtension(Display *dpy, const char *name)
{
	int major, first_event, first_error;
	XExtCodes *c;

	if (!XQueryExtension(dpy, name, &major, &first_event, &first_error))
		return NULL;
	c = XAddExtension(dpy);
	if (c) {
		c->major_opcode = major;
		c->first_event = first_event;
		c->first_error = first_error;
	}
	return c;
}

XLITE_IMPL(XAddConnectionWatch)
Status XAddConnectionWatch(Display *dpy, XConnectionWatchProc proc,
			   XPointer data)
{
	(void)dpy; (void)proc; (void)data;
	return 1;		/* one fd, and it never changes */
}

XLITE_IMPL(XRemoveConnectionWatch)
void XRemoveConnectionWatch(Display *dpy, XConnectionWatchProc proc,
			    XPointer data)
{
	(void)dpy; (void)proc; (void)data;
}

XLITE_IMPL(XSupportsLocale)
Bool XSupportsLocale(void) { return False; }

XLITE_IMPL(XSetLocaleModifiers)
char *XSetLocaleModifiers(const char *m) { (void)m; return NULL; }

XLITE_IMPL(XDisplayKeycodes)
int XDisplayKeycodes(Display *dpy, int *min, int *max)
{
	if (min) *min = XD(dpy)->pub.min_keycode;
	if (max) *max = XD(dpy)->pub.max_keycode;
	return 1;
}

XLITE_IMPL(XGetKeyboardMapping)
KeySym *XGetKeyboardMapping(Display *dpy, KeyCode first, int count,
			    int *per_code)
{
	KeySym *k = calloc(count > 0 ? count : 1, sizeof(KeySym));
	int i;

	(void)dpy;
	if (per_code)
		*per_code = 1;
	/*
	 * Latin-1 identity for the printable range. The server here has no
	 * keymap of its own and lvdesk sends keycodes that are already
	 * characters, so this is the mapping that makes them arrive.
	 */
	for (i = 0; k && i < count; i++)
		k[i] = first + i;
	return k;
}

XLITE_IMPL(XRefreshKeyboardMapping)
int XRefreshKeyboardMapping(XMappingEvent *e) { (void)e; return 1; }

XLITE_IMPL(XDefaultScreen)
int XDefaultScreen(Display *dpy) { return XD(dpy)->pub.default_screen; }
XLITE_IMPL(XRootWindow)
Window XRootWindow(Display *dpy, int s) { (void)s; return XD(dpy)->screen.root; }
XLITE_IMPL(XDefaultRootWindow)
Window XDefaultRootWindow(Display *dpy) { return XD(dpy)->screen.root; }
XLITE_IMPL(XBlackPixel)
unsigned long XBlackPixel(Display *dpy, int s) { (void)s; return XD(dpy)->screen.black_pixel; }
XLITE_IMPL(XWhitePixel)
unsigned long XWhitePixel(Display *dpy, int s) { (void)s; return XD(dpy)->screen.white_pixel; }
XLITE_IMPL(XDefaultColormap)
Colormap XDefaultColormap(Display *dpy, int s) { (void)s; return XD(dpy)->screen.cmap; }
XLITE_IMPL(XDefaultVisual)
Visual *XDefaultVisual(Display *dpy, int s) { (void)s; return &XD(dpy)->visual; }
XLITE_IMPL(XDefaultDepth)
int XDefaultDepth(Display *dpy, int s) { (void)s; return XD(dpy)->screen.root_depth; }
XLITE_IMPL(XDisplayWidth)
int XDisplayWidth(Display *dpy, int s) { (void)s; return XD(dpy)->screen.width; }
XLITE_IMPL(XDisplayHeight)
int XDisplayHeight(Display *dpy, int s) { (void)s; return XD(dpy)->screen.height; }
XLITE_IMPL(XScreenCount)
int XScreenCount(Display *dpy) { return XD(dpy)->pub.nscreens; }
XLITE_IMPL(XDefaultScreenOfDisplay)
Screen *XDefaultScreenOfDisplay(Display *dpy) { return &XD(dpy)->screen; }
XLITE_IMPL(XScreenOfDisplay)
Screen *XScreenOfDisplay(Display *dpy, int s) { (void)s; return &XD(dpy)->screen; }
XLITE_IMPL(XConnectionNumber)
int XConnectionNumber(Display *dpy) { return XD(dpy)->fd; }

/*
 * The XESet* family: libXext registers per-extension hooks through these and
 * keeps whatever it gets back as "the previous handler". Nothing here
 * advertises an extension, so there is never a previous handler and NULL is
 * the correct answer - but the functions must exist and return cleanly,
 * because the caller stores the result and calls it later if it is non-NULL.
 *
 * Declared with plain pointer arguments rather than the real typedefs, which
 * live in Xlibint.h - a header whose Display definition would collide with the
 * public one we build against. Every parameter is a pointer either way, so the
 * ABI is identical.
 */

/* ------------------------------------------------------- context manager */

/*
 * Xt's association between an X resource id and its widget lives here, and it
 * is consulted on every event dispatch - so this is not an optional corner of
 * Xlib. A resource id plus a context is the key; the value is opaque to us.
 *
 * A chained hash table, because Xt stores one entry per widget and looks them
 * up constantly. Linear search over a few hundred widgets on every event would
 * be felt on this board.
 */
#define CTX_BUCKETS	127

struct ctx {
	XID rid;
	XContext context;
	XPointer data;
	struct ctx *next;
};

static struct ctx *ctx_tab[CTX_BUCKETS];

static unsigned ctx_hash(XID rid, XContext c)
{
	return ((unsigned)rid * 31u + (unsigned)c) % CTX_BUCKETS;
}

XLITE_IMPL(XSaveContext)
int XSaveContext(Display *dpy, XID rid, XContext context, const char *data)
{
	unsigned h = ctx_hash(rid, context);
	struct ctx *c;

	(void)dpy;
	for (c = ctx_tab[h]; c; c = c->next)
		if (c->rid == rid && c->context == context) {
			c->data = (XPointer)data;
			return 0;
		}
	c = malloc(sizeof(*c));
	if (!c)
		return XCNOMEM;
	c->rid = rid;
	c->context = context;
	c->data = (XPointer)data;
	c->next = ctx_tab[h];
	ctx_tab[h] = c;
	return 0;
}

XLITE_IMPL(XFindContext)
int XFindContext(Display *dpy, XID rid, XContext context, XPointer *data)
{
	unsigned h = ctx_hash(rid, context);
	struct ctx *c;

	(void)dpy;
	for (c = ctx_tab[h]; c; c = c->next)
		if (c->rid == rid && c->context == context) {
			*data = c->data;
			return 0;
		}
	*data = NULL;
	return XCNOENT;
}

XLITE_IMPL(XDeleteContext)
int XDeleteContext(Display *dpy, XID rid, XContext context)
{
	unsigned h = ctx_hash(rid, context);
	struct ctx *c, **pp = &ctx_tab[h];

	(void)dpy;
	while ((c = *pp)) {
		if (c->rid == rid && c->context == context) {
			*pp = c->next;
			free(c);
			return 0;
		}
		pp = &c->next;
	}
	return XCNOENT;
}

/*
 * Xutil.h defines this as a macro, so callers never reference the symbol - but
 * libX11 exports it too, and something linked against the real library may.
 */
#undef XUniqueContext
XLITE_IMPL(XUniqueContext)
XContext XUniqueContext(void)
{
	static XContext next = 1;

	return next++;
}

/* ------------------------------------------------------------- odds/ends */

XLITE_IMPL(XScreenNumberOfScreen)
int XScreenNumberOfScreen(Screen *s) { (void)s; return 0; }

XLITE_IMPL(XFreeStringList)
void XFreeStringList(char **list)
{
	if (list) {
		free(list[0]);		/* one block, as XTextProperty gives */
		free(list);
	}
}

/*
 * A cursor id with nothing behind it. lvdesk draws the pointer itself and the
 * shim ignores cursor attributes entirely, so a client that sets one gets the
 * desktop's pointer - which is the right pointer for this machine anyway.
 */
XLITE_IMPL(XCreateFontCursor)
Cursor XCreateFontCursor(Display *dpy, unsigned shape)
{
	(void)shape;
	return XAllocID(dpy);
}

XLITE_IMPL(XFreeCursor)
int XFreeCursor(Display *dpy, Cursor c) { (void)dpy; (void)c; return 1; }

/* XLITE_TRACE_INPUT=1: say what the client asks of the pointer, on stderr. */
static int input_trace(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XLITE_TRACE_INPUT") != NULL;
	return v;
}

XLITE_IMPL(XDefineCursor)
int XDefineCursor(Display *dpy, Window w, Cursor c)
{
	XSetWindowAttributes a;

	if (input_trace())
		fprintf(stderr, "xlite: XDefineCursor(0x%lx, 0x%lx)\n",
			(unsigned long)w, (unsigned long)c);

	memset(&a, 0, sizeof(a));
	a.cursor = c;
	return XChangeWindowAttributes(dpy, w, CWCursor, &a);
}

XLITE_IMPL(XUndefineCursor)
int XUndefineCursor(Display *dpy, Window w) { return XDefineCursor(dpy, w, None); }

/*
 * A cursor made from a 1-bit source and mask (core CreateCursor, opcode 93).
 * SDL builds its "invisible" cursor this way - a 1x1 pixmap with an all-zero
 * mask - and installs it with XDefineCursor to hide the pointer over a game.
 * The shim looks at the mask: all zero means nothing is ever drawn, and the
 * desktop hides its own pointer while that cursor is the one in force.
 */
XLITE_IMPL(XCreatePixmapCursor)
Cursor XCreatePixmapCursor(Display *dpy, Pixmap source, Pixmap mask,
			   XColor *fg, XColor *bg, unsigned int hx, unsigned int hy)
{
	Cursor cid = XAllocID(dpy);

	REQ(dpy, 93, 0, 8);
	p32(r + 4, cid);
	p32(r + 8, source);
	p32(r + 12, mask);
	p16(r + 16, fg ? fg->red : 0);
	p16(r + 18, fg ? fg->green : 0);
	p16(r + 20, fg ? fg->blue : 0);
	p16(r + 22, bg ? bg->red : 0xffff);
	p16(r + 24, bg ? bg->green : 0xffff);
	p16(r + 26, bg ? bg->blue : 0xffff);
	p16(r + 28, hx);
	p16(r + 30, hy);
	xlite_send(x, r);
	return cid;
}

/* ----------------------------------------------------- grabs and warps */

/*
 * Pointer and keyboard grabs, and the warp SDL needs for relative motion.
 *
 * SDL 1.2 recentres the pointer with XWarpPointer after every MotionNotify
 * while the mouse is grabbed and hidden, and takes the delta from where it
 * lands. With these as no-ops Doom's view turned until the pointer reached
 * the window edge and stopped, and SDL_WM_GrabInput() looped for ever on a
 * grab that never reported success. The shim confines and routes; lvdesk
 * moves its own pointer.
 */
XLITE_IMPL(XGrabPointer)
int XGrabPointer(Display *dpy, Window w, Bool owner_events,
		 unsigned int event_mask, int pointer_mode, int keyboard_mode,
		 Window confine_to, Cursor cursor, Time t)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	{
		REQ(dpy, 26, owner_events ? 1 : 0, 6);
		p32(r + 4, w);
		p16(r + 8, event_mask);
		r[10] = pointer_mode;
		r[11] = keyboard_mode;
		p32(r + 12, confine_to);
		p32(r + 16, cursor);
		p32(r + 20, t);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return GrabNotViewable;
	}
	free(extra);
	if (input_trace())
		fprintf(stderr, "xlite: XGrabPointer(0x%lx owner=%d confine=0x%lx "
			"cursor=0x%lx) -> %d\n", (unsigned long)w, owner_events,
			(unsigned long)confine_to, (unsigned long)cursor, hdr[1]);
	return hdr[1];				/* status */
}

XLITE_IMPL(XUngrabPointer)
int XUngrabPointer(Display *dpy, Time t)
{
	REQ(dpy, 27, 0, 2);
	if (input_trace())
		fprintf(stderr, "xlite: XUngrabPointer()\n");
	p32(r + 4, t);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XGrabKeyboard)
int XGrabKeyboard(Display *dpy, Window w, Bool owner_events,
		  int pointer_mode, int keyboard_mode, Time t)
{
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	uint32_t seq;

	{
		REQ(dpy, 31, owner_events ? 1 : 0, 4);
		p32(r + 4, w);
		p32(r + 8, t);
		r[12] = pointer_mode;
		r[13] = keyboard_mode;
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return GrabNotViewable;
	}
	free(extra);
	return hdr[1];
}

XLITE_IMPL(XUngrabKeyboard)
int XUngrabKeyboard(Display *dpy, Time t)
{
	REQ(dpy, 32, 0, 2);
	p32(r + 4, t);
	xlite_send(x, r);
	return 1;
}

XLITE_IMPL(XWarpPointer)
int XWarpPointer(Display *dpy, Window src, Window dst, int sx, int sy,
		 unsigned int sw, unsigned int sh, int dx, int dy)
{
	REQ(dpy, 41, 0, 6);
	p32(r + 4, src);
	p32(r + 8, dst);
	p16(r + 12, sx); p16(r + 14, sy);
	p16(r + 16, sw); p16(r + 18, sh);
	p16(r + 20, dx); p16(r + 22, dy);
	if (input_trace()) {
		static unsigned n;

		if (n++ % 50 == 0)
			fprintf(stderr, "xlite: XWarpPointer #%u -> 0x%lx %d,%d\n",
				n, (unsigned long)dst, dx, dy);
	}
	xlite_send(x, r);
	return 1;
}

/*
 * Acceleration is the desktop's business (lvdesk has its own curve), so the
 * answer is a fixed, sane default and a change is accepted and ignored. SDL
 * asks so it can turn acceleration off while it reads raw mickeys.
 */
XLITE_IMPL(XGetPointerControl)
int XGetPointerControl(Display *dpy, int *num, int *den, int *thresh)
{
	(void)dpy;
	if (num) *num = 2;
	if (den) *den = 1;
	if (thresh) *thresh = 4;
	return 1;
}

XLITE_IMPL(XChangePointerControl)
int XChangePointerControl(Display *dpy, Bool do_accel, Bool do_thresh,
			  int num, int den, int thresh)
{
	(void)dpy; (void)do_accel; (void)do_thresh; (void)num; (void)den;
	(void)thresh;
	return 1;
}

/* No screensaver exists to reset. */
XLITE_IMPL(XResetScreenSaver)
int XResetScreenSaver(Display *dpy) { (void)dpy; return 1; }

/* ------------------------------------------------- Xext bookkeeping */
/*
 * The per-display registry libXext keeps for extension client code. SDL's
 * bundled XF86VidMode client (its own copy of Xxf86vm, compiled into
 * libSDL) is written against it: XextAddDisplay() asks the server for the
 * extension's opcodes and remembers them per display, and everything else
 * is find/remove. Until now libXext's copies were stubs returning NULL, so
 * XF86VidModeQueryExtension() said "absent" and SDL_FULLSCREEN could only
 * mean a window the size of the panel. SDL resolves these from libX11 first,
 * so they live here. The hooks (close_display and friends) are accepted and
 * not called; nothing here needs them.
 */
#include <X11/extensions/extutil.h>

XLITE_IMPL(XextCreateExtension)
XExtensionInfo *XextCreateExtension(void)
{
	return calloc(1, sizeof(XExtensionInfo));
}

XLITE_IMPL(XextDestroyExtension)
void XextDestroyExtension(XExtensionInfo *info)
{
	if (!info)
		return;
	while (info->head) {
		XExtDisplayInfo *d = info->head;

		info->head = d->next;
		free(d);
	}
	free(info);
}

XLITE_IMPL(XextFindDisplay)
XExtDisplayInfo *XextFindDisplay(XExtensionInfo *info, Display *dpy)
{
	XExtDisplayInfo *d;

	if (!info)
		return NULL;
	if (info->cur && info->cur->display == dpy)
		return info->cur;
	for (d = info->head; d; d = d->next)
		if (d->display == dpy) {
			info->cur = d;
			return d;
		}
	return NULL;
}

XLITE_IMPL(XextAddDisplay)
XExtDisplayInfo *XextAddDisplay(XExtensionInfo *info, Display *dpy,
				const char *name, XExtensionHooks *hooks,
				int nevents, XPointer data)
{
	XExtDisplayInfo *d;

	(void)hooks; (void)nevents;
	if (!info)
		return NULL;
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->display = dpy;
	d->data = data;
	d->codes = XInitExtension(dpy, name);	/* NULL: not present */
	d->next = info->head;
	info->head = d;
	info->cur = d;
	info->ndisplays++;
	return d;
}

XLITE_IMPL(XextRemoveDisplay)
int XextRemoveDisplay(XExtensionInfo *info, Display *dpy)
{
	XExtDisplayInfo **pp;

	if (!info)
		return 0;
	for (pp = &info->head; *pp; pp = &(*pp)->next)
		if ((*pp)->display == dpy) {
			XExtDisplayInfo *d = *pp;

			*pp = d->next;
			if (info->cur == d)
				info->cur = NULL;
			info->ndisplays--;
			free(d);
			return 1;
		}
	return 0;
}

/*
 * No fontsets. XSupportsLocale() already returns False, so the toolkit takes
 * its single-font path; returning NULL here is the answer that matches.
 */
XLITE_IMPL(XCreateFontSet)
XFontSet XCreateFontSet(Display *dpy, const char *base, char ***missing,
			int *nmissing, char **def)
{
	(void)dpy; (void)base;
	if (missing) *missing = NULL;
	if (nmissing) *nmissing = 0;
	if (def) *def = NULL;
	return NULL;
}

XLITE_IMPL(XCreatePixmapFromBitmapData)
Pixmap XCreatePixmapFromBitmapData(Display *dpy, Drawable d, char *data,
				   unsigned w, unsigned h, unsigned long fg,
				   unsigned long bg, unsigned depth)
{
	Pixmap p = XCreatePixmap(dpy, d, w, h, depth);
	GC gc;
	XGCValues v;
	int stride = (w + 7) / 8, bytes = stride * h;

	v.foreground = fg;
	v.background = bg;
	gc = XCreateGC(dpy, p, GCForeground | GCBackground, &v);
	{	/* PutImage, XYBitmap, one 1-bit plane. */
		REQ(dpy, 72, 0, 6 + (bytes + 3) / 4);

		p32(r + 4, p);
		p32(r + 8, gc ? gc->gid : 0);
		p16(r + 12, w); p16(r + 14, h);
		p16(r + 16, 0); p16(r + 18, 0);
		r[20] = 0;			/* left-pad */
		r[21] = 1;			/* depth 1 */
		if (bytes > 0 && data)
			memcpy(r + 24, data, bytes);
		xlite_send(x, r);
	}
	XFreeGC(dpy, gc);
	return p;
}

XLITE_IMPL(XGetGCValues)
Status XGetGCValues(Display *dpy, GC gc, unsigned long mask, XGCValues *out)
{
	struct xgc *g = (struct xgc *)gc;

	(void)dpy;
	if (!g || !out)
		return 0;
	/*
	 * From the cache, never the server. There is no GetGCValues request in
	 * the X protocol at all - real Xlib answers from its own copy too.
	 */
	if (mask & GCFunction) out->function = g->v.function;
	if (mask & GCPlaneMask) out->plane_mask = g->v.plane_mask;
	if (mask & GCForeground) out->foreground = g->v.foreground;
	if (mask & GCBackground) out->background = g->v.background;
	if (mask & GCLineWidth) out->line_width = g->v.line_width;
	if (mask & GCLineStyle) out->line_style = g->v.line_style;
	if (mask & GCCapStyle) out->cap_style = g->v.cap_style;
	if (mask & GCJoinStyle) out->join_style = g->v.join_style;
	if (mask & GCFillStyle) out->fill_style = g->v.fill_style;
	if (mask & GCFillRule) out->fill_rule = g->v.fill_rule;
	if (mask & GCTile) out->tile = g->v.tile;
	if (mask & GCStipple) out->stipple = g->v.stipple;
	if (mask & GCTileStipXOrigin) out->ts_x_origin = g->v.ts_x_origin;
	if (mask & GCTileStipYOrigin) out->ts_y_origin = g->v.ts_y_origin;
	if (mask & GCFont) out->font = g->v.font;
	if (mask & GCSubwindowMode) out->subwindow_mode = g->v.subwindow_mode;
	if (mask & GCGraphicsExposures)
		out->graphics_exposures = g->v.graphics_exposures;
	if (mask & GCClipXOrigin) out->clip_x_origin = g->v.clip_x_origin;
	if (mask & GCClipYOrigin) out->clip_y_origin = g->v.clip_y_origin;
	if (mask & GCClipMask) out->clip_mask = g->v.clip_mask;
	if (mask & GCDashOffset) out->dash_offset = g->v.dash_offset;
	if (mask & GCDashList) out->dashes = g->v.dashes;
	if (mask & GCArcMode) out->arc_mode = g->v.arc_mode;
	return 1;
}

/* The XAlloc*Hints family is nothing but zeroed structures. */
XLITE_IMPL(XAllocSizeHints)
XSizeHints *XAllocSizeHints(void) { return calloc(1, sizeof(XSizeHints)); }
XLITE_IMPL(XAllocWMHints)
XWMHints *XAllocWMHints(void) { return calloc(1, sizeof(XWMHints)); }
XLITE_IMPL(XAllocClassHint)
XClassHint *XAllocClassHint(void) { return calloc(1, sizeof(XClassHint)); }
XLITE_IMPL(XAllocIconSize)
XIconSize *XAllocIconSize(void) { return calloc(1, sizeof(XIconSize)); }
XLITE_IMPL(XAllocStandardColormap)
XStandardColormap *XAllocStandardColormap(void)
{
	return calloc(1, sizeof(XStandardColormap));
}

/* ------------------------------------------------------- shared pixmaps */
/*
 * XLITE-SHM: draw into a pixmap's real pages instead of describing them.
 *
 * The server is lvdesk and we are its libX11, so a pixmap's storage can simply
 * be handed over: the shim moves it into a memfd, passes the descriptor down
 * the socket that already exists, and both sides then address the same memory.
 * Loading an icon stops being thousands of requests, or even a batched few -
 * it becomes a memcpy, and the protocol carries only the damage rectangle.
 *
 * Falls back silently: XliteShmMap() returns NULL against a server that does
 * not advertise the extension, and every caller keeps its drawing path.
 */
/*
 * MIT-SHM, client side.
 *
 * XLITE-SHM (below) is ours, so only we ever use it - which means an
 * off-the-shelf client never learns that a window's pixels are shareable. SDL
 * renders into a buffer it malloc'd and pushes whole frames through
 * XPutImage, and XPutImage then memcpys them into the window: 64 kB in and
 * 64 kB out per frame at 320x200, four times that at 640x400. That copy is
 * pure overhead and it exists only because SDL could not ask.
 *
 * MIT-SHM is the extension it DOES know how to ask for. SDL probes for these
 * five symbols by dlsym (SDL_x11sym.h), and takes its copy-free path if they
 * are all present. Nothing about SDL changes; it simply stops being lied to.
 *
 * The struct layout is fixed by the real XShm.h - SDL was compiled against it
 * and hands us a pointer - so it must match exactly:
 *
 *     ShmSeg shmseg;  int shmid;  char *shmaddr;  Bool readOnly;
 */
static int ximg_destroy_shared(XImage *im);
typedef unsigned long XliteShmSeg;
typedef struct {
	XliteShmSeg shmseg;
	int shmid;
	char *shmaddr;
	int readOnly;
} XliteShmSegmentInfo;

static int mitshm_major(Display *dpy)
{
	static int major = -1;
	int ev, er;

	if (major < 0) {
		/* XLITE_NOMITSHM forces clients back onto XPutImage, so the
		 * two paths can be compared on one board without relinking. */
		if (getenv("XLITE_NOMITSHM") ||
		    !XQueryExtension(dpy, "MIT-SHM", &major, &ev, &er))
			major = 0;
	}
	return major;
}

XLITE_IMPL(XShmQueryExtension)
Bool XShmQueryExtension(Display *dpy)
{
	return mitshm_major(dpy) ? True : False;
}

XLITE_IMPL(XShmAttach)
Status XShmAttach(Display *dpy, XliteShmSegmentInfo *si)
{
	int major = mitshm_major(dpy);

	if (!major || !si)
		return 0;
	si->shmseg = xlite_alloc_id(dpy);
	{
		REQ(dpy, major, 1, 4);		/* ShmAttach */

		if (!r)
			return 0;
		p32(r + 4, (uint32_t)si->shmseg);
		p32(r + 8, (uint32_t)si->shmid);
		r[12] = si->readOnly ? 1 : 0;
		xlite_send(x, r);
	}
	/*
	 * Flush and sync. The server must have attached before the client
	 * puts anything, and SDL's try_mitshm() decides whether the path
	 * works from the X error handler firing during its XSync - so an
	 * attach that is still sitting in the output buffer reads as success
	 * and the first ShmPutImage then fails against an unknown segment.
	 */
	XSync(dpy, False);
	return 1;
}

XLITE_IMPL(XShmDetach)
Status XShmDetach(Display *dpy, XliteShmSegmentInfo *si)
{
	int major = mitshm_major(dpy);

	if (!major || !si)
		return 0;
	{
		REQ(dpy, major, 2, 2);		/* ShmDetach */

		if (!r)
			return 0;
		p32(r + 4, (uint32_t)si->shmseg);
		xlite_send(x, r);
	}
	XFlush(dpy);
	return 1;
}

XLITE_IMPL(XShmCreateImage)
XImage *XShmCreateImage(Display *dpy, Visual *vis, unsigned int depth,
			int format, char *data, XliteShmSegmentInfo *si,
			unsigned int width, unsigned int height)
{
	XImage *im;

	(void)si;
	/*
	 * A plain XImage over the client's own memory. XCreateImage already
	 * fills in the depth-appropriate masks and bits_per_pixel from the
	 * server's format list, and the shared segment differs only in who
	 * allocated it.
	 */
	im = XCreateImage(dpy, vis, depth, format, 0, data,
			  width, height, 32, 0);
	/*
	 * The pixels are the CLIENT'S shared segment, not ours to free.
	 * XCreateImage installs ximg_destroy, which frees im->data - right
	 * for an image over malloc'd memory, and a crash for this one: SDL's
	 * X11_DestroyImage calls XDestroyImage BEFORE shmdt(), so at exit the
	 * shmat() address went to free() and prboom died in musl's
	 * get_meta() every time it quit. libXext's _XShmDestroyImage frees
	 * only the XImage, and so does this.
	 */
	if (im)
		im->f.destroy_image = ximg_destroy_shared;
	/*
	 * Real Xlib hangs the segment info off the image here, and
	 * XShmPutImage reads it back from there - it has no shminfo argument
	 * of its own. Anything that copies the image without obdata would
	 * lose the association, which is exactly why Xlib puts it on the
	 * object rather than in a side table.
	 */
	if (im)
		im->obdata = (XPointer)si;
	return im;
}

XLITE_IMPL(XShmPutImage)
Status XShmPutImage(Display *dpy, Drawable d, GC gc, XImage *im,
		    int src_x, int src_y, int dst_x, int dst_y,
		    unsigned int w, unsigned int h, Bool send_event)
{
	int major = mitshm_major(dpy);
	XliteShmSegmentInfo *si;

	(void)gc; (void)send_event;
	if (!major || !im)
		return 0;
	si = (XliteShmSegmentInfo *)im->obdata;
	if (!si)
		return 0;		/* not a shared image after all */
	{
		REQ(dpy, major, 3, 10);		/* ShmPutImage */

		if (!r)
			return 0;
		/* xShmPutImageReq (shmproto.h) - the offsets are the ABI. */
		p32(r + 4, (uint32_t)d);
		p32(r + 8, (uint32_t)XGContextFromGC(gc));
		p16(r + 12, im->width);		/* totalWidth  */
		p16(r + 14, im->height);	/* totalHeight */
		p16(r + 16, src_x);
		p16(r + 18, src_y);
		p16(r + 20, w);			/* srcWidth  */
		p16(r + 22, h);			/* srcHeight */
		p16(r + 24, dst_x);
		p16(r + 26, dst_y);
		r[28] = (uint8_t)im->depth;
		r[29] = (uint8_t)(im->format == XYPixmap ? 1 : 2);
		r[30] = 0;			/* sendEvent: no completion */
		r[31] = 0;
		p32(r + 32, (uint32_t)si->shmseg);
		p32(r + 36, 0);			/* offset into the segment */
		xlite_send(x, r);
	}
	/*
	 * DO NOT FLUSH HERE. Real Xlib/libXext does not, and the flush is what
	 * splits one frame into two server wakeups.
	 *
	 * SDL calls XShmPutImage and then XSync in the same function. XSync
	 * appends a 4-byte GetInputFocus to the SAME output buffer, so without
	 * a flush both requests leave in one write and the server wakes once,
	 * reads both, and replies. With the flush, the 40-byte put goes on its
	 * own: the server wakes out of poll(), recvmsgs, handles a request
	 * that produces no output, and goes back to poll() - then wakes again
	 * for the 4 bytes. Measured server-side at ~119 us of poll and ~386 us
	 * of recvmsg per pass, that second pass is ~2.8% of the machine.
	 *
	 * Safe because every path that could strand the buffer flushes first:
	 * XSync, xlite_wait_event() and pump_ex() all flush before blocking.
	 * XLITE_SHMFLUSH=1 restores the old behaviour for comparison.
	 */
	{
		static int always = -1;

		if (always < 0)
			always = getenv("XLITE_SHMFLUSH") != NULL;
		if (always)
			XFlush(dpy);
	}
	return 1;
}

static int xshm_major(Display *dpy)
{
	static int major = -1;
	int ev, er;

	if (major < 0) {
		/* XLITE_NOSHM forces the protocol path, so the two can be
		 * compared on the same board without relinking anything. */
		if (getenv("XLITE_NOSHM") ||
		    !XQueryExtension(dpy, "XLITE-SHM", &major, &ev, &er))
			major = 0;
	}
	return major;
}

/*
 * Cache the answer, because asking is not free.
 *
 * XPutImage calls XliteShmMap on EVERY call, and each call was a full
 * request/reply round trip - the client blocks until the server answers. On
 * this board a socket syscall costs 1.3-5.8 ms (see docs), so a client
 * pushing frames pays two blocking round trips per frame purely to ask a
 * question whose answer never changes. prboom at ~31 fps was making ~62 of
 * them a second, every one of which was refused: the shared path only covers
 * PIXMAPS, and SDL draws to a WINDOW.
 *
 * Both answers are worth keeping. A refusal is permanent for that drawable,
 * and a success used to mmap afresh on every call - the same pages mapped
 * again and again, never unmapped, which is a leak as well as a cost.
 *
 * Keyed on the drawable id and dropped when the drawable is freed, which is
 * the only moment an id can come to mean something else.
 */
struct shm_cache {
	Display *dpy;
	Drawable d;
	void *base;			/* the mapping, for munmap */
	void *ptr;			/* this drawable's origin inside it */
	size_t len;
	int w, h, stride, bpp;
	int used;
};
static struct shm_cache shmc[16];

static struct shm_cache *shm_lookup(Display *dpy, Drawable d)
{
	int i;

	for (i = 0; i < (int)(sizeof shmc / sizeof shmc[0]); i++)
		if (shmc[i].used && shmc[i].dpy == dpy && shmc[i].d == d)
			return &shmc[i];
	return NULL;
}

static struct shm_cache *shm_slot(Display *dpy, Drawable d)
{
	int i;

	for (i = 0; i < (int)(sizeof shmc / sizeof shmc[0]); i++)
		if (!shmc[i].used) {
			shmc[i].used = 1;
			shmc[i].dpy = dpy;
			shmc[i].d = d;
			return &shmc[i];
		}
	return NULL;			/* full: fall back to asking */
}

/* Called when a drawable goes away, so its id cannot be reused under us. */
void xlite_shm_forget(Display *dpy, Drawable d)
{
	struct shm_cache *e = shm_lookup(dpy, d);

	if (!e)
		return;
	if (e->base && e->len)
		munmap(e->base, e->len);
	memset(e, 0, sizeof *e);
}

void *XliteShmMap(Display *dpy, Pixmap p, int *w, int *h, int *stride, int *bpp)
{
	struct xdpy *x = (struct xdpy *)dpy;
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0, len;
	uint32_t seq;
	void *m, *m2;
	int major = xshm_major(dpy), fd;
	struct shm_cache *e = shm_lookup(dpy, p);

	if (e) {				/* asked before; same answer */
		if (!e->base)
			return NULL;
		if (w) *w = e->w;
		if (h) *h = e->h;
		if (stride) *stride = e->stride;
		if (bpp) *bpp = e->bpp;
		return e->ptr;
	}
	if (!major)
		return NULL;
	{
		REQ(dpy, major, 1, 2);
		p32(r + 4, p);
		seq = x->pub.request;
		xlite_send(x, r);
		if (!xlite_reply(x, seq, hdr, &extra, &nextra))
			return NULL;
	}
	free(extra);
	fd = x->shm_fd;
	x->shm_fd = -1;
	if (fd < 0) {
		/*
		 * Refused - and deliberately NOT remembered.
		 *
		 * A refusal is not necessarily permanent: a window has no
		 * pixels until something first paints it, so the very first
		 * XPutImage can be told "no" for a drawable that becomes
		 * shareable a frame later. Caching that answer locked the
		 * client onto the slow path for its whole life, which is
		 * exactly what happened when this cache was first written -
		 * Doom kept pushing 128 KB a frame through the socket with
		 * the fast path sitting right there.
		 *
		 * Asking again costs a round trip, and a round trip measured
		 * as nothing here (9/9/4 fps against 7/10/9 with the negative
		 * cache in place). Correctness wins an argument it does not
		 * even have to pay for. Successes are still cached below.
		 */
		return NULL;
	}
	if (w) *w = hdr[8] | (hdr[9] << 8);
	if (h) *h = hdr[10] | (hdr[11] << 8);
	if (stride) *stride = hdr[12] | (hdr[13] << 8);
	if (bpp) *bpp = hdr[14];
	len = (size_t)hdr[16] | ((size_t)hdr[17] << 8) |
	      ((size_t)hdr[18] << 16) | ((size_t)hdr[19] << 24);
	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);		/* the mapping keeps it alive */
	if (m == MAP_FAILED)
		return NULL;
	/*
	 * The drawable's origin within the mapping. A child window is a view
	 * into its top-level's buffer, so the pixels it owns start here, not
	 * at the start of the shared pages.
	 */
	{
		size_t off = (size_t)hdr[20] | ((size_t)hdr[21] << 8) |
			     ((size_t)hdr[22] << 16) | ((size_t)hdr[23] << 24);

		if (off >= len)
			off = 0;
		m2 = (char *)m + off;
	}
	e = shm_slot(dpy, p);
	if (e) {
		e->base = m;
		e->ptr = m2;
		e->len = len;
		e->w = w ? *w : 0;
		e->h = h ? *h : 0;
		e->stride = stride ? *stride : 0;
		e->bpp = bpp ? *bpp : 0;
	}
	return m2;
}

/* Tell the server which part of a shared pixmap changed. No reply: a round
 * trip here would reinstate exactly the cost this path exists to remove. */
void XliteShmDamaged(Display *dpy, Pixmap p)
{
	struct xdpy *x = (struct xdpy *)dpy;
	int major = xshm_major(dpy);

	if (!major)
		return;
	{
		REQ(dpy, major, 2, 2);
		p32(r + 4, p);
		/*
		 * FLUSH, INSIDE THE LOCK - AND AT EVERY SIZE.
		 *
		 * xlite_send() only releases the output lock; the bytes sit in
		 * the request buffer until something else pushes them out. On
		 * the zero-copy path there IS nothing else: the client wrote
		 * its pixels straight into the shared mapping and this Damaged
		 * expects no reply, so no later request arrives to flush it.
		 *
		 * DO NOT gate this on surface size. px_alloc()'s 65536-byte
		 * threshold only decides whether a surface is BORN shareable;
		 * px_share() converts a smaller one on demand, so a 320x200
		 * window gets zero-copy too and needs this just as much. That
		 * mistake was made on 2026-09-06 while reverting: an
		 * interactive run still painted, because SDL's event polling
		 * generates other traffic that flushes the queue incidentally,
		 * but a -timedemo runs flat out and generates none - so the
		 * window went black at 320x200 with BOTH the CPU LUT and the
		 * hardware CLUT, which is what proved it was the flush and not
		 * the expander.
		 *
		 * Inside the lock: flushing after xlite_send() runs unlocked,
		 * which is the torn-request hazard xlite_out_lock exists for.
		 */
		xlite_flush(x);
		xlite_send(x, r);
		/* nothing before this needs ordering against the next write */
		x->shm_seq = x->pub.request;
	}
}

/*
 * The server's pixmap formats.
 *
 * SDL asks for these to learn how many bits a pixel of a given depth actually
 * occupies, and then indexes the array. Returning NULL - which the generated
 * stub did, benignly, like every other unimplemented entry point - is the one
 * case where "carry on" cannot work: the caller dereferences the result and
 * dies. prboom exited on SIGSEGV here with no other symptom, and the shim's
 * own to-do log named it in a single run.
 *
 * Three formats, matching what xshim actually stores: depth 1 for bitmaps,
 * depth 8 for the paletted surfaces it keeps at one byte per pixel, and depth
 * 16 for everything else. scanline_pad is 32 because every row in the shim is
 * padded to a four-byte boundary.
 */
XLITE_IMPL(XListPixmapFormats)
XPixmapFormatValues *XListPixmapFormats(Display *dpy, int *count)
{
	/*
	 * These MUST match what xshim sends in the connection setup, because
	 * XCreateImage below derives bits_per_pixel from them: the server said
	 * {1,1}, {16,16}, {24,32}, so saying anything else here makes the two
	 * halves of the same stack disagree about the size of a pixel.
	 */
	/*
	 * FIVE, matching what xshim sends in the connection setup: depth 1
	 * bitmaps, depth 8 indexed, depth 16 RGB565, and 24/32 at four bytes.
	 * A client derives bits_per_pixel for a depth from THIS list, so an
	 * entry missing here makes the two halves of the same stack disagree
	 * about the size of a pixel.
	 */
	static const struct { int d, b; } fmt[] = { {1, 1}, {8, 8}, {16, 16},
						    {24, 32}, {32, 32} };
	XPixmapFormatValues *v;
	unsigned i, n = sizeof(fmt) / sizeof(fmt[0]);

	(void)dpy;
	v = malloc(n * sizeof(*v));
	if (!v) {
		if (count)
			*count = 0;
		return NULL;
	}
	for (i = 0; i < n; i++) {
		v[i].depth = fmt[i].d;
		v[i].bits_per_pixel = fmt[i].b;
		v[i].scanline_pad = 32;
	}
	if (count)
		*count = (int)n;
	return v;
}

/*
 * Out-parameter fillers.
 *
 * These are the dangerous shape for this library. The generated stub returns 0
 * and lets the client carry on, which is right for a call whose result nobody
 * inspects - and WRONG here, because 0 is Success and the caller then uses a
 * structure that was never written. It reads whatever was on the stack.
 *
 * That cost two SIGSEGVs in prboom with no message beyond the signal:
 * XListPixmapFormats (SDL indexes the array it returns) and
 * Xutf8TextListToTextProperty below (SDL passes the property straight to
 * XSetWMName). If a stub has a pointer out-parameter, it has to be written by
 * hand or it is a crash waiting for a caller.
 */
XLITE_IMPL(Xutf8TextListToTextProperty)
int Xutf8TextListToTextProperty(Display *dpy, char **list, int count,
				XICCEncodingStyle style, XTextProperty *tp)
{
	const char *src = (count > 0 && list && list[0]) ? list[0] : "";
	size_t n = strlen(src);

	(void)dpy; (void)style;
	if (!tp)
		return -1;
	tp->value = (unsigned char *)malloc(n + 1);
	if (!tp->value) {
		tp->nitems = 0;
		tp->format = 8;
		tp->encoding = 31;		/* XA_STRING */
		return -1;
	}
	memcpy(tp->value, src, n + 1);
	tp->encoding = 31;			/* XA_STRING */
	tp->format = 8;
	tp->nitems = n;
	return 0;				/* Success */
}

/*
 * The shim answers QueryPointer for real, but nothing here needs the answer
 * badly enough to pay a round trip on a path SDL calls per frame. Zeroed and
 * honest beats uninitialised: the caller gets a valid, if uninteresting,
 * pointer position rather than stack contents.
 */
XLITE_IMPL(XQueryPointer)
Bool XQueryPointer(Display *dpy, Window w, Window *root, Window *child,
		   int *rx, int *ry, int *wx, int *wy, unsigned int *mask)
{
	(void)dpy; (void)w;
	if (root)  *root = 0;
	if (child) *child = 0;
	if (rx) *rx = 0;
	if (ry) *ry = 0;
	if (wx) *wx = 0;
	if (wy) *wy = 0;
	if (mask) *mask = 0;
	return True;
}

/* 32 bytes of keyboard state; no key held is the truthful answer here. */
XLITE_IMPL(XQueryKeymap)
int XQueryKeymap(Display *dpy, char keys[32])
{
	(void)dpy;
	if (keys)
		memset(keys, 0, 32);
	return 0;
}

/* ------------------------------------------------------------- images */
/*
 * XCreateImage / XPutImage / XGetImage.
 *
 * All three were stubs, and the shim ACCEPTED PutImage and threw it away - so
 * any off-the-shelf client that drew an image drew nothing, silently. This is
 * the ordinary way pixels reach a drawable, so that was a hole underneath
 * every application rather than a slow path in one.
 *
 * Where the destination is a pixmap the shared path takes over completely: the
 * pixels are memcpy'd into the server's own pages and only a damage message
 * crosses the socket. Otherwise the wire request carries them, which is what
 * any other X server would do.
 */
/*
 * XDestroyImage is a MACRO in Xutil.h that calls through ximage->f.destroy_image,
 * so it cannot be provided as a symbol - the function pointer has to be filled
 * in on every image we hand out, or the client jumps through NULL on free.
 */
static int ximg_destroy(XImage *im)
{
	if (im) {
		free(im->data);
		free(im);
	}
	return 1;
}

/* For images over memory the client owns (MIT-SHM): the struct only. */
static int ximg_destroy_shared(XImage *im)
{
	free(im);
	return 1;
}

/*
 * The server's visuals - all one of them.
 *
 * A stub returning NULL is not survivable for a client that asks this to find
 * out what a pixel looks like. SDL does exactly that: with no visual it builds
 * a surface format whose red/green/blue masks are zero, every SDL_MapRGB then
 * returns 0, and the client renders a perfectly correct image in which every
 * pixel is black. prboom did, at 32% of a core, sending well-formed PutImage
 * requests the shim accepted and drew - all of them black.
 *
 * The template and mask are ignored deliberately: this server has one visual,
 * so it either matches or the caller has no alternative anyway, and answering
 * with it beats answering with nothing.
 */
/*
 * BOTH visuals, and the template is honoured.
 *
 * This used to return one hardcoded depth-16 TrueColor entry and ignore the
 * mask and template completely. The server grew a depth-8 PseudoColor visual
 * so that palette applications could render at their native depth - and it
 * made no difference whatsoever, because a toolkit does not read the server's
 * advertisement directly: it asks HERE. SDL enumerates visuals, found only
 * the 16-bit one, concluded no 8-bit visual existed and quietly built a
 * shadow surface, converting every frame itself. The server offering a visual
 * that the client library hides is worse than not offering it, because
 * everything looks like it works.
 *
 * VISUAL8_ID must match the id xshim advertises for its depth-8 visual.
 */
#define XLITE_VISUAL8_ID	0x22
#define XLITE_VISUAL32_ID	0x23

XLITE_IMPL(XGetVisualInfo)
XVisualInfo *XGetVisualInfo(Display *dpy, long mask, XVisualInfo *tmpl,
			    int *nitems)
{
	static Visual v8;		/* the depth-8 PseudoColor visual */
	Visual *v32p = &XD(dpy)->visual32;	/* the screen table's object */
	XVisualInfo all[3];
	XVisualInfo *out;
	int n = 0, i;

	memset(all, 0, sizeof all);

	all[0].visual = DefaultVisual(dpy, 0);
	all[0].visualid = all[0].visual ? all[0].visual->visualid : 1;
	all[0].screen = 0;
	all[0].depth = 16;
	all[0].class = TrueColor;
	all[0].red_mask = 0xF800;
	all[0].green_mask = 0x07E0;
	all[0].blue_mask = 0x001F;
	all[0].colormap_size = 32;
	all[0].bits_per_rgb = 6;

	v8.visualid = XLITE_VISUAL8_ID;
	v8.class = PseudoColor;
	v8.red_mask = v8.green_mask = v8.blue_mask = 0;
	v8.bits_per_rgb = 8;
	v8.map_entries = 256;
	all[1].visual = &v8;
	all[1].visualid = XLITE_VISUAL8_ID;
	all[1].screen = 0;
	all[1].depth = 8;
	all[1].class = PseudoColor;
	all[1].colormap_size = 256;
	all[1].bits_per_rgb = 8;

	/*
	 * Depth 32, TrueColor - ARGB8888, matching the server's setup.
	 *
	 * st asks for this one by name and does not check whether it got it:
	 * XMatchVisualInfo(dpy, scr, 32, TrueColor, &vis) followed by
	 * xw.vis = vis.visual. Without the entry, vis stays uninitialised and
	 * st draws its whole terminal into a garbage visual - it runs, takes
	 * input, and shows nothing.
	 */
	all[2].visual = v32p;
	all[2].visualid = XLITE_VISUAL32_ID;
	all[2].screen = 0;
	all[2].depth = 32;
	all[2].class = TrueColor;
	all[2].red_mask = 0x00FF0000;
	all[2].green_mask = 0x0000FF00;
	all[2].blue_mask = 0x000000FF;
	all[2].colormap_size = 256;
	all[2].bits_per_rgb = 8;

	out = calloc(3, sizeof(*out));
	if (!out) {
		if (nitems)
			*nitems = 0;
		return NULL;
	}
	for (i = 0; i < 3; i++) {
		if (tmpl) {
			if ((mask & VisualIDMask) &&
			    tmpl->visualid != all[i].visualid)
				continue;
			if ((mask & VisualScreenMask) && tmpl->screen != 0)
				continue;
			if ((mask & VisualDepthMask) &&
			    tmpl->depth != all[i].depth)
				continue;
			if ((mask & VisualClassMask) &&
			    tmpl->class != all[i].class)
				continue;
		}
		out[n++] = all[i];
	}
	if (nitems)
		*nitems = n;
	if (!n) {
		free(out);
		return NULL;
	}
	return out;
}

XLITE_IMPL(XVisualIDFromVisual)
VisualID XVisualIDFromVisual(Visual *v) { return v ? v->visualid : 1; }

XLITE_IMPL(XCreateImage)
XImage *XCreateImage(Display *dpy, Visual *vis, unsigned int depth, int format,
		     int offset, char *data, unsigned int w, unsigned int h,
		     int pad, int stride)
{
	XImage *im = calloc(1, sizeof(*im));

	(void)dpy; (void)vis;
	if (!im)
		return NULL;
	im->width = (int)w;
	im->height = (int)h;
	im->xoffset = offset;
	im->format = format;
	im->data = data;
	im->byte_order = LSBFirst;
	im->bitmap_unit = 32;
	im->bitmap_bit_order = LSBFirst;
	im->bitmap_pad = pad ? pad : 32;
	im->depth = (int)depth;
	/*
	 * bits_per_pixel comes from the SERVER'S pixmap format for this depth,
	 * which is what Xlib does and the only rule that serves every caller.
	 *
	 * This used to be a heuristic - "depth 16 with bitmap_pad 32 and no
	 * stride means someone composed BGRA" - with a comment asserting that
	 * the caller it would misread did not exist. SDL 1.2 is exactly that
	 * caller: it asks for a true 16bpp surface in that shape, then adopts
	 * our bytes_per_line as its pitch having already allocated at its own.
	 * Promoting it to 32bpp made it write 1280-byte rows into a buffer
	 * sized for 640, 128 kB past the end every frame, and prboom died in
	 * musl's get_meta() on the next free() - a crash that points at the
	 * heap and says nothing about the image.
	 *
	 * A guess about which client is asking cannot be right for both. The
	 * format list can, because it is the server describing itself.
	 */
	{
		XPixmapFormatValues *pf;
		int nf = 0, i;

		im->bits_per_pixel = depth <= 8 ? 8 : depth <= 16 ? 16 : 32;
		pf = XListPixmapFormats(dpy, &nf);
		if (pf) {
			for (i = 0; i < nf; i++)
				if (pf[i].depth == (int)depth) {
					im->bits_per_pixel = pf[i].bits_per_pixel;
					break;
				}
			free(pf);
		}
	}
	/*
	 * xfiles' thumbnailer composes BGRA and passes DefaultDepth with
	 * bitmap_pad 32 and no stride - correct on the 24/32-bit servers it
	 * was written against, impossible on this 16-bit one under strict
	 * Xlib rules (bits_per_pixel comes from the server's format list).
	 * Recognise that caller shape and carry the image as 32bpp; the
	 * wire request then declares depth 24 and xshim's PutImage already
	 * converts BGRA bytes to RGB565. The one legitimate caller this
	 * could misread - a true 16bpp image with pad 32 and no stride -
	 * does not exist among the clients this library serves.
	 */
	/*
	 * ...and that caller DOES exist: SDL 1.2 asks for exactly this shape -
	 * depth 16, bitmap_pad 32, no stride - for a true 16bpp surface, then
	 * adopts our bytes_per_line as its pitch having already allocated the
	 * buffer at its own. Promoting it to 32bpp made SDL write 200 rows of
	 * 1280 bytes into a 128,000-byte allocation: 128 kB past the end, every
	 * frame. prboom died in musl's get_meta() on the next free(), which
	 * points at the heap and says nothing about the image.
	 *
	 * So the promotion is opt-in now, and correct Xlib behaviour is the
	 * default. xfiles is off-the-shelf and cannot be changed, but its
	 * launcher is ours, so s31-open sets XLITE_IMG32 for it.
	 */

	im->bytes_per_line = stride ? stride :
		(int)(((w * im->bits_per_pixel + 31) / 32) * 4);
	im->f.destroy_image = ximg_destroy;
	im->red_mask = 0xF800;
	im->green_mask = 0x07E0;
	im->blue_mask = 0x001F;
	return im;
}

/*
 * A client-built XImage (xfiles fills the struct itself for its icons and
 * thumbnails, then calls this). Real Xlib validates the fields and installs
 * the accessor functions; here the accessors are real functions, so what
 * matters is the derived geometry the caller may have left at zero.
 * Returned 0 from a stub until 2026-09-10, so every such image was treated
 * as unusable and xfiles drew its icons from garbage.
 */
XLITE_IMPL(XInitImage)
Status XInitImage(XImage *im)
{
	if (!im || im->width <= 0 || im->height <= 0 || im->depth <= 0)
		return 0;
	if (!im->bits_per_pixel)
		im->bits_per_pixel = im->depth <= 8 ? 8 :
				     im->depth <= 16 ? 16 : 32;
	if (!im->bitmap_pad)
		im->bitmap_pad = 32;
	if (!im->bitmap_unit)
		im->bitmap_unit = 32;
	if (!im->bytes_per_line)
		im->bytes_per_line = (int)(((im->width * im->bits_per_pixel +
					     im->bitmap_pad - 1) /
					    im->bitmap_pad) *
					   (im->bitmap_pad / 8));
	im->f.destroy_image = ximg_destroy;
	return 1;
}


XLITE_IMPL(XPutImage)
int XPutImage(Display *dpy, Drawable d, GC gc, XImage *im, int sx, int sy,
	      int dx, int dy, unsigned int w, unsigned int h)
{
	struct xdpy *x = (struct xdpy *)dpy;
	int sw, sh, stride, bpp, y;
	void *base;

	if (!im || !im->data || !w || !h)
		return 0;

	/*
	 * Straight into the server's pages when it will share them. No pixel
	 * ever enters the socket, and the whole transfer costs one memcpy per
	 * row plus a single damage message.
	 */
	/*
	 * Any byte-sized depth, not just 16.
	 *
	 * This used to require `bpp == 2 && bits_per_pixel == 16`, which meant
	 * an 8-bit client silently LOST the shared path and went back to
	 * pushing every frame through the socket. That is not hypothetical:
	 * giving the shim a depth-8 PseudoColor visual - so Doom could render
	 * at its native depth, worth +22% on its own - turned this condition
	 * false and took the zero-copy transfer away in the same change. The
	 * two wins silently cancelled.
	 *
	 * The server tells us its bytes-per-pixel; match the client's image to
	 * it and the row copy is identical arithmetic at either size.
	 */
	base = XliteShmMap(dpy, d, &sw, &sh, &stride, &bpp);
	if (base && bpp > 0 && im->bits_per_pixel == bpp * 8) {
		/*
		 * ORDER the write behind anything already queued for the
		 * server. Wire requests execute when the server reads them;
		 * this memcpy executes now. A client that clears a rectangle
		 * and then puts an image into it - xfiles, for every thumbnail
		 * - had the clear land AFTER the image and wipe it. Real Xlib
		 * never has this problem because PutImage is itself a request.
		 * One round trip, and only when something is actually pending:
		 * SDL's frame loop is PutImage + Damaged and never pays it.
		 */
		if (x->pub.request != x->shm_seq)
			XSync(dpy, False);
		for (y = 0; (unsigned)y < h && dy + y < sh; y++) {
			const char *s = im->data +
					(size_t)(sy + y) * im->bytes_per_line +
					(size_t)sx * bpp;
			char *o = (char *)base + (size_t)(dy + y) * stride +
				  (size_t)dx * bpp;
			unsigned n = w;

			if (dx + (int)n > sw)
				n = sw - dx;
			memcpy(o, s, (size_t)n * bpp);
		}
		XliteShmDamaged(dpy, d);
		return 0;
	}

	/*
	 * Otherwise put it on the wire - in BANDS that fit one request.
	 *
	 * A request cannot exceed the server's maximum length, which it states
	 * at connection setup (65,536 bytes here). A 320x200 image is 128 kB,
	 * so sending it whole produces no drawing and no error: the request is
	 * simply too big to exist. Real Xlib splits for the same reason, and a
	 * client that draws a large image is the normal case, not an edge one.
	 */
	{
		int rowb = (int)(((w * im->bits_per_pixel / 8) + 3) & ~3u);
		unsigned long cap = x->pub.max_request_size ?
				    x->pub.max_request_size * 4 : 65536;
		int rows = (int)((cap - 64) / (unsigned long)(rowb ? rowb : 1));
		int done = 0;

		if (rows < 1)
			rows = 1;
		while ((unsigned)done < h) {
			int nrows = (int)h - done < rows ? (int)h - done : rows;
			int nb = rowb * nrows;
			REQ(dpy, 72, 2, 6 + (nb + 3) / 4);

			p32(r + 4, d);
			p32(r + 8, gc ? ((struct xgc *)gc)->gid : 0);
			p16(r + 12, w);
			p16(r + 14, nrows);
			p16(r + 16, dx);
			p16(r + 18, dy + done);
			r[20] = 0;
			/* 32bpp data must say so; see XCreateImage above. */
			r[21] = im->bits_per_pixel == 32 ? 24 :
				(unsigned char)im->depth;
			for (y = 0; y < nrows; y++)
				memcpy(r + 24 + (size_t)y * rowb,
				       im->data +
				       (size_t)(sy + done + y) *
				       im->bytes_per_line +
				       (size_t)sx * im->bits_per_pixel / 8,
				       (size_t)rowb);
			xlite_send(x, r);
			done += nrows;
		}
	}
	return 0;
}

XLITE_IMPL(XGetImage)
XImage *XGetImage(Display *dpy, Drawable d, int sx, int sy, unsigned int w,
		  unsigned int h, unsigned long plane, int format)
{
	int sw, sh, stride, bpp, y;
	void *base;
	XImage *im;
	char *buf;

	(void)plane;
	base = XliteShmMap(dpy, d, &sw, &sh, &stride, &bpp);
	if (!base || bpp != 2)
		return NULL;		/* only shared drawables can be read */
	if (((struct xdpy *)dpy)->pub.request != ((struct xdpy *)dpy)->shm_seq)
		XSync(dpy, False);	/* read what the queued requests drew */
	buf = malloc((size_t)w * h * 2);
	if (!buf)
		return NULL;
	for (y = 0; (unsigned)y < h; y++)
		memcpy(buf + (size_t)y * w * 2,
		       (char *)base + (size_t)(sy + y) * stride + (size_t)sx * 2,
		       (size_t)w * 2);
	im = XCreateImage(dpy, NULL, 16, format, 0, buf, w, h, 32, (int)w * 2);
	if (!im)
		free(buf);
	return im;
}
