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
#include "xlite.h"

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
		seq = x->seq;
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
		seq = x->seq;
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
XLITE_IMPL(XMapSubwindows)
int XMapSubwindows(Display *dpy, Window w) { return simple_win(dpy, 9, w); }
XLITE_IMPL(XUnmapWindow)
int XUnmapWindow(Display *dpy, Window w) { return simple_win(dpy, 10, w); }
XLITE_IMPL(XUnmapSubwindows)
int XUnmapSubwindows(Display *dpy, Window w) { return simple_win(dpy, 11, w); }
XLITE_IMPL(XDestroyWindow)
int XDestroyWindow(Display *dpy, Window w) { return simple_win(dpy, 4, w); }
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
	REQ(dpy, 54, 0, 2);
	p32(r + 4, p);
	xlite_send(x, r);
	return 1;
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

static XFontStruct *font_query(Display *dpy, Font fid)
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
		seq = x->seq;
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
	/* min-bounds is at hdr+8, max-bounds starts at hdr+24 and spans. */
	fs->min_bounds.width = (short)g16(hdr + 12);
	fs->min_bounds.ascent = (short)g16(hdr + 14);
	fs->min_bounds.descent = (short)g16(hdr + 16);
	fs->max_bounds.width = (short)g16(hdr + 28);
	if (extra && nextra >= 28) {
		fs->max_bounds.ascent = (short)g16(extra + 0);
		fs->max_bounds.descent = (short)g16(extra + 2);
		fs->min_char_or_byte2 = g16(extra + 8);
		fs->max_char_or_byte2 = g16(extra + 10);
		fs->default_char = g16(extra + 12);
		fs->all_chars_exist = extra[19];
		fs->ascent = (short)g16(extra + 20);
		fs->descent = (short)g16(extra + 22);
		nch = g32(extra + 24);
		if (nch && nextra >= 28 + (size_t)nch * 12) {
			fs->per_char = calloc(nch, sizeof(XCharStruct));
			if (fs->per_char)
				for (i = 0; i < (int)nch; i++) {
					const unsigned char *c = extra + 28 + i * 12;

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

XLITE_IMPL(XQueryFont)
XFontStruct *XQueryFont(Display *dpy, XID fid) { return font_query(dpy, fid); }

XLITE_IMPL(XLoadQueryFont)
XFontStruct *XLoadQueryFont(Display *dpy, const char *name)
{
	return font_query(dpy, XLoadFont(dpy, name));
}

XLITE_IMPL(XFreeFont)
int XFreeFont(Display *dpy, XFontStruct *fs)
{
	if (!fs)
		return 0;
	{
		REQ(dpy, 46, 0, 2);
		p32(r + 4, fs->fid);
		xlite_send(x, r);
	}
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
		seq = x->seq;
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
		seq = x->seq;
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
		seq = x->seq;
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
		seq = x->seq;
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
 * Regions as their bounding box.
 *
 * Xt uses regions to accumulate exposed area and then clips redraws to them.
 * A bounding box is always a superset, so the client redraws a little more
 * than it strictly must and the result on screen is identical. Exact region
 * arithmetic is several hundred lines to save repainting a few rectangles on
 * a 800x480 panel.
 */
struct xlite_region { int x1, y1, x2, y2; };	/* empty when x2 <= x1 */

XLITE_IMPL(XCreateRegion)
Region XCreateRegion(void)
{
	struct xlite_region *r = calloc(1, sizeof(*r));

	return (Region)r;
}

XLITE_IMPL(XDestroyRegion)
int XDestroyRegion(Region r) { free(r); return 1; }

XLITE_IMPL(XEmptyRegion)
Bool XEmptyRegion(Region r)
{
	struct xlite_region *g = (struct xlite_region *)r;

	return !g || g->x2 <= g->x1 || g->y2 <= g->y1;
}

XLITE_IMPL(XClipBox)
int XClipBox(Region r, XRectangle *rect)
{
	struct xlite_region *g = (struct xlite_region *)r;

	if (!g || XEmptyRegion(r)) {
		memset(rect, 0, sizeof(*rect));
		return 1;
	}
	rect->x = g->x1; rect->y = g->y1;
	rect->width = g->x2 - g->x1;
	rect->height = g->y2 - g->y1;
	return 1;
}

XLITE_IMPL(XUnionRectWithRegion)
int XUnionRectWithRegion(XRectangle *rect, Region src, Region dst)
{
	struct xlite_region *s = (struct xlite_region *)src;
	struct xlite_region *d = (struct xlite_region *)dst;
	struct xlite_region t;

	if (!d)
		return 0;
	t = s ? *s : *d;
	if (rect->width && rect->height) {
		int x2 = rect->x + rect->width, y2 = rect->y + rect->height;

		if (t.x2 <= t.x1 || t.y2 <= t.y1) {
			t.x1 = rect->x; t.y1 = rect->y; t.x2 = x2; t.y2 = y2;
		} else {
			if (rect->x < t.x1) t.x1 = rect->x;
			if (rect->y < t.y1) t.y1 = rect->y;
			if (x2 > t.x2) t.x2 = x2;
			if (y2 > t.y2) t.y2 = y2;
		}
	}
	*d = t;
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
	*d = p ? *p : (struct xlite_region){ 0, 0, 0, 0 };
	if (q && !XEmptyRegion(b)) {
		rc.x = q->x1; rc.y = q->y1;
		rc.width = q->x2 - q->x1; rc.height = q->y2 - q->y1;
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
		memset(d, 0, sizeof(*d));
		return 1;
	}
	d->x1 = p->x1 > q->x1 ? p->x1 : q->x1;
	d->y1 = p->y1 > q->y1 ? p->y1 : q->y1;
	d->x2 = p->x2 < q->x2 ? p->x2 : q->x2;
	d->y2 = p->y2 < q->y2 ? p->y2 : q->y2;
	if (d->x2 < d->x1) d->x2 = d->x1;
	if (d->y2 < d->y1) d->y2 = d->y1;
	return 1;
}

XLITE_IMPL(XSubtractRegion)
int XSubtractRegion(Region a, Region b, Region dst)
{
	struct xlite_region *p = (struct xlite_region *)a;
	struct xlite_region *d = (struct xlite_region *)dst;

	(void)b;			/* a bounding box cannot lose a hole */
	if (d && p)
		*d = *p;
	return 1;
}

XLITE_IMPL(XOffsetRegion)
int XOffsetRegion(Region r, int dx, int dy)
{
	struct xlite_region *g = (struct xlite_region *)r;

	if (g) {
		g->x1 += dx; g->x2 += dx;
		g->y1 += dy; g->y2 += dy;
	}
	return 1;
}

XLITE_IMPL(XPointInRegion)
Bool XPointInRegion(Region r, int px, int py)
{
	struct xlite_region *g = (struct xlite_region *)r;

	return g && px >= g->x1 && px < g->x2 && py >= g->y1 && py < g->y2;
}

XLITE_IMPL(XRectInRegion)
int XRectInRegion(Region r, int px, int py, unsigned w, unsigned h)
{
	struct xlite_region *g = (struct xlite_region *)r;

	if (!g || XEmptyRegion(r))
		return RectangleOut;
	if ((int)(px + w) <= g->x1 || px >= g->x2 ||
	    (int)(py + h) <= g->y1 || py >= g->y2)
		return RectangleOut;
	if (px >= g->x1 && (int)(px + w) <= g->x2 &&
	    py >= g->y1 && (int)(py + h) <= g->y2)
		return RectangleIn;
	return RectanglePart;
}

XLITE_IMPL(XEqualRegion)
Bool XEqualRegion(Region a, Region b)
{
	struct xlite_region *p = (struct xlite_region *)a;
	struct xlite_region *q = (struct xlite_region *)b;

	return p && q && !memcmp(p, q, sizeof(*p));
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
		seq = x->seq;
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
 * Extension bookkeeping. Nothing here advertises an extension, but libXext
 * still registers itself and dereferences what it gets back, so this has to
 * return a real record.
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

XLITE_IMPL(XInitExtension)
XExtCodes *XInitExtension(Display *dpy, const char *name)
{
	(void)name;
	return XAddExtension(dpy);
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
/*
 * Listed here for the stub generator, which greps the source rather than
 * expanding macros and so cannot see the names the macro below defines:
 * XLITE_IMPL(XESetCloseDisplay) XLITE_IMPL(XESetCreateGC)
 * XLITE_IMPL(XESetCopyGC) XLITE_IMPL(XESetFlushGC) XLITE_IMPL(XESetFreeGC)
 * XLITE_IMPL(XESetCreateFont) XLITE_IMPL(XESetFreeFont)
 * XLITE_IMPL(XESetWireToEvent) XLITE_IMPL(XESetEventToWire)
 * XLITE_IMPL(XESetWireToError) XLITE_IMPL(XESetError)
 * XLITE_IMPL(XESetErrorString) XLITE_IMPL(XESetPrintErrorValues)
 * XLITE_IMPL(XESetCopyEventCookie) XLITE_IMPL(XESetWireToEventCookie)
 */
#define XESET(name) \
	void *name(Display *dpy, int ext, void *proc) \
	{ (void)dpy; (void)ext; (void)proc; return NULL; }

XESET(XESetCloseDisplay)
XESET(XESetCreateGC)
XESET(XESetCopyGC)
XESET(XESetFlushGC)
XESET(XESetFreeGC)
XESET(XESetCreateFont)
XESET(XESetFreeFont)
XESET(XESetWireToEvent)
XESET(XESetEventToWire)
XESET(XESetWireToError)
XESET(XESetError)
XESET(XESetErrorString)
XESET(XESetPrintErrorValues)
XESET(XESetCopyEventCookie)
XESET(XESetWireToEventCookie)

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

XLITE_IMPL(XDefineCursor)
int XDefineCursor(Display *dpy, Window w, Cursor c)
{
	XSetWindowAttributes a;

	memset(&a, 0, sizeof(a));
	a.cursor = c;
	return XChangeWindowAttributes(dpy, w, CWCursor, &a);
}

XLITE_IMPL(XUndefineCursor)
int XUndefineCursor(Display *dpy, Window w) { return XDefineCursor(dpy, w, None); }

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
