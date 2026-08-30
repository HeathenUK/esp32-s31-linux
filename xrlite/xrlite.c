// SPDX-License-Identifier: GPL-2.0-only
/*
 * xrlite - the parts of libXrender this board's clients actually use.
 *
 * Audited: xclock references 5 of libXrender's 45 exported functions, xfiles 6,
 * and xftlite 5 - ten distinct. The same ratio that produced xlite (346 of
 * 1,225), xtlite (19 of libXt) and xftlite (17 of 77).
 *
 * The saving is modest by comparison - libXrender is 44 kB resident, against
 * the 1.1 MB that xftlite removed - so the reason to own this one is not
 * memory. It is that a Picture is OPAQUE server-side, and two things we need
 * cannot be recovered from one:
 *
 *   - which drawable a Picture refers to. XftTextRender32() takes a
 *     destination Picture and no drawable, so text cannot be drawn into a
 *     picture we did not create - which is exactly where xfiles stopped.
 *   - what colour a solid-fill Picture holds, which is where text gets its
 *     colour from.
 *
 * Owning XRenderCreatePicture() and XRenderCreateSolidFill() makes both
 * knowable, because we are the one handing the identifiers out.
 *
 * The awkward member of the ten is XRenderCompositeDoublePoly(), which is not
 * a protocol request at all: libXrender tessellates the polygon into
 * trapezoids on the CLIENT and sends Trapezoids. That is why the shim only
 * ever saw trapezoids and never a polygon, and it is why xclock's dial depends
 * on the tessellation below being right.
 */
#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- plumbing */

#define R_QueryVersion		0
#define R_QueryPictFormats	1
#define R_CreatePicture		4
#define R_ChangePicture		5
#define R_SetPictureClipRects	6
#define R_FreePicture		7
#define R_Composite		8
#define R_Trapezoids		10
#define R_FillRectangles	26
#define R_CreateSolidFill	33

static void p16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void p32(unsigned char *p, unsigned long v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static unsigned g16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long g32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
	       ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void xr_missing(const char *name)
{
	static const char *seen[24];
	static int n;
	int i;

	for (i = 0; i < n; i++)
		if (seen[i] == name)
			return;
	if (n < 24)
		seen[n++] = name;
	fprintf(stderr, "xrlite: %s() is not implemented - carrying on\n",
		name);
}

/* --------------------------------------------------------- display state */

#define MAXFMT	16
#define MAXVIS	8
#define MAXPICT	64

struct vismap { VisualID vid; PictFormat fmt; };

struct pictrec {
	Picture pict;
	Drawable drawable;		/* 0 for a solid fill */
	XRenderColor color;		/* solid fills only */
	int solid;
};

struct xrdpy {
	Display *dpy;
	int major;			/* extension opcode, 0 = absent */
	int event_base, error_base;
	int ver_major, ver_minor;
	XRenderPictFormat fmt[MAXFMT];
	int nfmt;
	struct vismap vis[MAXVIS];
	int nvis;
	struct pictrec pict[MAXPICT];
	int npict;
	XID next_id_hack;
};

static struct xrdpy dpys[4];
static int ndpys;

static struct xrdpy *find_dpy(Display *dpy)
{
	int i;

	for (i = 0; i < ndpys; i++)
		if (dpys[i].dpy == dpy)
			return &dpys[i];
	return NULL;
}

static unsigned char *xr_req(struct xrdpy *x, int minor, size_t len)
{
	unsigned char *r = _XGetRequest(x->dpy, (CARD8)x->major, len);

	if (r)
		r[1] = (unsigned char)minor;
	return r;
}

/* Remember what a Picture is, so it can be answered for later. */
static void pict_remember(struct xrdpy *x, Picture p, Drawable d, int solid,
			  const XRenderColor *c)
{
	int i;

	for (i = 0; i < x->npict; i++)
		if (x->pict[i].pict == p)
			break;
	if (i == MAXPICT)
		return;
	if (i == x->npict)
		x->npict++;
	x->pict[i].pict = p;
	x->pict[i].drawable = d;
	x->pict[i].solid = solid;
	if (c)
		x->pict[i].color = *c;
}

static struct pictrec *pict_find(struct xrdpy *x, Picture p)
{
	int i;

	for (i = 0; i < x->npict; i++)
		if (x->pict[i].pict == p)
			return &x->pict[i];
	return NULL;
}

/*
 * Published so xftlite can answer the two questions a Picture cannot.
 * Deliberately not part of the Xrender API - a caller that wants them is
 * asking our stack, not X.
 */
Drawable XRliteDrawableOfPicture(Display *dpy, Picture p)
{
	struct xrdpy *x = find_dpy(dpy);
	struct pictrec *r = x ? pict_find(x, p) : NULL;

	return r && !r->solid ? r->drawable : 0;
}

int XRliteColorOfPicture(Display *dpy, Picture p, XRenderColor *out)
{
	struct xrdpy *x = find_dpy(dpy);
	struct pictrec *r = x ? pict_find(x, p) : NULL;

	if (!r || !r->solid)
		return 0;
	if (out)
		*out = r->color;
	return 1;
}

/* ------------------------------------------------------------- start-up */

static void query_formats(struct xrdpy *x);

static struct xrdpy *xr_init(Display *dpy)
{
	struct xrdpy *x = find_dpy(dpy);
	XExtCodes *codes;

	if (x)
		return x->major ? x : NULL;
	if (ndpys == 4)
		return NULL;
	x = &dpys[ndpys++];
	memset(x, 0, sizeof(*x));
	x->dpy = dpy;

	codes = XInitExtension(dpy, "RENDER");
	if (!codes)
		return NULL;		/* honestly absent; caller must cope */
	x->major = codes->major_opcode;
	x->event_base = codes->first_event;
	x->error_base = codes->first_error;

	/* Version handshake first: the format reply's shape depends on it. */
	{
		unsigned char *r = xr_req(x, R_QueryVersion, 12);
		xReply rep;

		if (r) {
			p32(r + 4, 0);
			p32(r + 8, 11);
			if (_XReply(dpy, &rep, 0, xTrue)) {
				x->ver_major = (int)g32((unsigned char *)&rep + 8);
				x->ver_minor = (int)g32((unsigned char *)&rep + 12);
			}
		}
	}
	query_formats(x);
	return x;
}

static void query_formats(struct xrdpy *x)
{
	unsigned char *r = xr_req(x, R_QueryPictFormats, 4);
	unsigned char rep[32], *data;
	unsigned long nf, ns, nd, nv, nsub, rlen;
	unsigned long i, s, d, v;
	unsigned char *p;

	if (!r)
		return;
	if (!_XReply(x->dpy, (xReply *)rep, 0, xFalse))
		return;
	nf = g32(rep + 8);
	ns = g32(rep + 12);
	nd = g32(rep + 16);
	nv = g32(rep + 20);
	nsub = g32(rep + 24);
	rlen = nf * 28 + ns * 8 + nd * 8 + nv * 8 + nsub * 4;
	if (!rlen || rlen > (1 << 16))
		return;
	data = malloc(rlen);
	if (!data)
		return;
	_XRead(x->dpy, (char *)data, (long)rlen);

	p = data;
	for (i = 0; i < nf && x->nfmt < MAXFMT; i++, p += 28) {
		XRenderPictFormat *f = &x->fmt[x->nfmt++];

		memset(f, 0, sizeof(*f));
		f->id = g32(p);
		f->type = p[4];
		f->depth = p[5];
		f->direct.red = g16(p + 8);
		f->direct.redMask = g16(p + 10);
		f->direct.green = g16(p + 12);
		f->direct.greenMask = g16(p + 14);
		f->direct.blue = g16(p + 16);
		f->direct.blueMask = g16(p + 18);
		f->direct.alpha = g16(p + 20);
		f->direct.alphaMask = g16(p + 22);
		f->colormap = g32(p + 24);
	}
	/* screens -> depths -> visuals, in that order and packed */
	for (s = 0; s < ns; s++) {
		unsigned long ndepth = g32(p);

		p += 8;				/* nDepth, fallback */
		for (d = 0; d < ndepth; d++) {
			unsigned long nvis = g16(p + 2);

			p += 8;			/* depth, pad, nvisuals, pad */
			for (v = 0; v < nvis; v++, p += 8)
				if (x->nvis < MAXVIS) {
					x->vis[x->nvis].vid = g32(p);
					x->vis[x->nvis].fmt = g32(p + 4);
					x->nvis++;
				}
		}
	}
	free(data);
}

/* ---------------------------------------------------------------- formats */

Bool XRenderQueryExtension(Display *dpy, int *event_base, int *error_base)
{
	struct xrdpy *x = xr_init(dpy);

	if (!x)
		return False;
	if (event_base)
		*event_base = x->event_base;
	if (error_base)
		*error_base = x->error_base;
	return True;
}

Status XRenderQueryVersion(Display *dpy, int *major, int *minor)
{
	struct xrdpy *x = xr_init(dpy);

	if (!x)
		return 0;
	if (major)
		*major = x->ver_major;
	if (minor)
		*minor = x->ver_minor;
	return 1;
}

static XRenderPictFormat *fmt_by_id(struct xrdpy *x, PictFormat id)
{
	int i;

	for (i = 0; i < x->nfmt; i++)
		if (x->fmt[i].id == id)
			return &x->fmt[i];
	return NULL;
}

XRenderPictFormat *XRenderFindVisualFormat(Display *dpy, const Visual *visual)
{
	struct xrdpy *x = xr_init(dpy);
	int i;

	if (!x || !visual)
		return NULL;
	for (i = 0; i < x->nvis; i++)
		if (x->vis[i].vid == visual->visualid)
			return fmt_by_id(x, x->vis[i].fmt);
	return NULL;
}

XRenderPictFormat *XRenderFindFormat(Display *dpy, unsigned long mask,
				     const XRenderPictFormat *templ, int count)
{
	struct xrdpy *x = xr_init(dpy);
	int i;

	if (!x)
		return NULL;
	for (i = 0; i < x->nfmt; i++) {
		XRenderPictFormat *f = &x->fmt[i];

		if ((mask & PictFormatID) && templ->id != f->id) continue;
		if ((mask & PictFormatType) && templ->type != f->type) continue;
		if ((mask & PictFormatDepth) && templ->depth != f->depth) continue;
		if ((mask & PictFormatRed) &&
		    templ->direct.red != f->direct.red) continue;
		if ((mask & PictFormatRedMask) &&
		    templ->direct.redMask != f->direct.redMask) continue;
		if ((mask & PictFormatGreen) &&
		    templ->direct.green != f->direct.green) continue;
		if ((mask & PictFormatGreenMask) &&
		    templ->direct.greenMask != f->direct.greenMask) continue;
		if ((mask & PictFormatBlue) &&
		    templ->direct.blue != f->direct.blue) continue;
		if ((mask & PictFormatBlueMask) &&
		    templ->direct.blueMask != f->direct.blueMask) continue;
		if ((mask & PictFormatAlpha) &&
		    templ->direct.alpha != f->direct.alpha) continue;
		if ((mask & PictFormatAlphaMask) &&
		    templ->direct.alphaMask != f->direct.alphaMask) continue;
		if (count-- == 0)
			return f;
	}
	return NULL;
}

/*
 * The standard formats, exactly as libXrender defines them. Matching is by
 * TEMPLATE - type, depth and all eight direct fields - which is why the shim
 * writing PictTypeIndexed instead of PictTypeDirect made every lookup here
 * fail while lookups by id still worked.
 */
XRenderPictFormat *XRenderFindStandardFormat(Display *dpy, int format)
{
	static const XRenderPictFormat std[] = {
		/* PictStandardARGB32 */
		{ 0, PictTypeDirect, 32, { 16, 0xff, 8, 0xff, 0, 0xff,
					   24, 0xff }, 0 },
		/* PictStandardRGB24 */
		{ 0, PictTypeDirect, 24, { 16, 0xff, 8, 0xff, 0, 0xff,
					   0, 0x00 }, 0 },
		/* PictStandardA8 */
		{ 0, PictTypeDirect, 8, { 0, 0x00, 0, 0x00, 0, 0x00,
					  0, 0xff }, 0 },
		/* PictStandardA4 */
		{ 0, PictTypeDirect, 4, { 0, 0x00, 0, 0x00, 0, 0x00,
					  0, 0x0f }, 0 },
		/* PictStandardA1 */
		{ 0, PictTypeDirect, 1, { 0, 0x00, 0, 0x00, 0, 0x00,
					  0, 0x01 }, 0 },
	};
	unsigned long mask = PictFormatType | PictFormatDepth |
		PictFormatRed | PictFormatRedMask |
		PictFormatGreen | PictFormatGreenMask |
		PictFormatBlue | PictFormatBlueMask |
		PictFormatAlpha | PictFormatAlphaMask;

	if (format < 0 || format >= (int)(sizeof(std) / sizeof(std[0])))
		return NULL;
	return XRenderFindFormat(dpy, mask, &std[format], 0);
}

Status XRenderParseColor(Display *dpy, char *spec, XRenderColor *def)
{
	XColor c;

	if (!def)
		return 0;
	if (!XParseColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
			 spec, &c))
		return 0;
	def->red = c.red;
	def->green = c.green;
	def->blue = c.blue;
	def->alpha = 0xffff;
	return 1;
}

/* --------------------------------------------------------------- pictures */

Picture XRenderCreatePicture(Display *dpy, Drawable drawable,
			     const XRenderPictFormat *format,
			     unsigned long valuemask,
			     const XRenderPictureAttributes *attributes)
{
	struct xrdpy *x = xr_init(dpy);
	Picture pid;
	unsigned char *r;
	int nval = 0, bit;

	if (!x)
		return 0;
	pid = XAllocID(dpy);
	for (bit = 0; bit < 13; bit++)
		if (valuemask & (1u << bit))
			nval++;
	r = xr_req(x, R_CreatePicture, 20 + nval * 4);
	if (!r)
		return 0;
	p32(r + 4, pid);
	p32(r + 8, drawable);
	p32(r + 12, format ? format->id : 0);
	p32(r + 16, valuemask);
	/*
	 * Only the values we can act on are written, but every set bit must
	 * still get its four bytes or the server reads the list out of step.
	 */
	{
		unsigned char *v = r + 20;

		for (bit = 0; bit < 13; bit++) {
			if (!(valuemask & (1u << bit)))
				continue;
			p32(v, 0);
			if (bit == 0 && attributes)
				p32(v, attributes->repeat);
			v += 4;
		}
	}
	pict_remember(x, pid, drawable, 0, NULL);
	return pid;
}

void XRenderChangePicture(Display *dpy, Picture picture,
			  unsigned long valuemask,
			  const XRenderPictureAttributes *attributes)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r, *v;
	int nval = 0, bit;

	if (!x)
		return;
	for (bit = 0; bit < 13; bit++)
		if (valuemask & (1u << bit))
			nval++;
	r = xr_req(x, R_ChangePicture, 12 + nval * 4);
	if (!r)
		return;
	p32(r + 4, picture);
	p32(r + 8, valuemask);
	v = r + 12;
	for (bit = 0; bit < 13; bit++) {
		if (!(valuemask & (1u << bit)))
			continue;
		p32(v, 0);
		if (bit == 0 && attributes)
			p32(v, attributes->repeat);
		v += 4;
	}
}

void XRenderSetPictureClipRectangles(Display *dpy, Picture picture,
				     int xOrigin, int yOrigin,
				     const XRectangle *rects, int n)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r;
	int i;

	if (!x || n < 0)
		return;
	r = xr_req(x, R_SetPictureClipRects, 12 + n * 8);
	if (!r)
		return;
	p32(r + 4, picture);
	p16(r + 8, xOrigin);
	p16(r + 10, yOrigin);
	for (i = 0; i < n; i++) {
		unsigned char *p = r + 12 + i * 8;

		p16(p, rects[i].x);
		p16(p + 2, rects[i].y);
		p16(p + 4, rects[i].width);
		p16(p + 6, rects[i].height);
	}
}

void XRenderSetPictureClipRegion(Display *dpy, Picture picture, Region r)
{
	XRectangle rc;

	if (!r) {
		XRenderPictureAttributes a;

		memset(&a, 0, sizeof(a));
		XRenderChangePicture(dpy, picture, CPClipMask, &a);
		return;
	}
	XClipBox(r, &rc);
	XRenderSetPictureClipRectangles(dpy, picture, 0, 0, &rc, 1);
}

void XRenderFreePicture(Display *dpy, Picture picture)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r;
	struct pictrec *rec;

	if (!x)
		return;
	r = xr_req(x, R_FreePicture, 8);
	if (r)
		p32(r + 4, picture);
	rec = pict_find(x, picture);
	if (rec)
		rec->pict = 0;		/* the slot is reusable */
}

Picture XRenderCreateSolidFill(Display *dpy, const XRenderColor *color)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r;
	Picture pid;

	if (!x || !color)
		return 0;
	pid = XAllocID(dpy);
	r = xr_req(x, R_CreateSolidFill, 16);
	if (!r)
		return 0;
	p32(r + 4, pid);
	p16(r + 8, color->red);
	p16(r + 10, color->green);
	p16(r + 12, color->blue);
	p16(r + 14, color->alpha);
	pict_remember(x, pid, 0, 1, color);
	return pid;
}

/* --------------------------------------------------------------- drawing */

void XRenderComposite(Display *dpy, int op, Picture src, Picture mask,
		      Picture dst, int src_x, int src_y, int mask_x,
		      int mask_y, int dst_x, int dst_y, unsigned int width,
		      unsigned int height)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r;

	if (!x)
		return;
	r = xr_req(x, R_Composite, 36);
	if (!r)
		return;
	r[4] = (unsigned char)op;
	p32(r + 8, src);
	p32(r + 12, mask);
	p32(r + 16, dst);
	p16(r + 20, src_x);
	p16(r + 22, src_y);
	p16(r + 24, mask_x);
	p16(r + 26, mask_y);
	p16(r + 28, dst_x);
	p16(r + 30, dst_y);
	p16(r + 32, width);
	p16(r + 34, height);
}

void XRenderFillRectangles(Display *dpy, int op, Picture dst,
			   const XRenderColor *color, const XRectangle *rects,
			   int n)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r;
	int i;

	if (!x || n <= 0 || !color)
		return;
	r = xr_req(x, R_FillRectangles, 20 + n * 8);
	if (!r)
		return;
	r[4] = (unsigned char)op;
	p32(r + 8, dst);
	p16(r + 12, color->red);
	p16(r + 14, color->green);
	p16(r + 16, color->blue);
	p16(r + 18, color->alpha);
	for (i = 0; i < n; i++) {
		unsigned char *p = r + 20 + i * 8;

		p16(p, rects[i].x);
		p16(p + 2, rects[i].y);
		p16(p + 4, rects[i].width);
		p16(p + 6, rects[i].height);
	}

	/*
	 * A single 1x1 rect at the origin with PictOpSrc is not an ordinary
	 * fill: it is the pre-CreateSolidFill idiom for a solid colour source -
	 * a 1x1 pixmap with RepeatNormal, painted once and then used as the
	 * source of every Composite that wants that colour. Recording the
	 * colour here is what lets XRliteColorOfPicture() answer for it.
	 *
	 * Without this, xftlite could not tell what colour a string was meant
	 * to be, left the GC at its protocol default foreground of 0, and drew
	 * every one of xfiles' filenames in black on black. xfiles builds its
	 * colours exactly this way (widget.c inittheme()), and so does most
	 * pre-2005 RENDER code - CreateSolidFill only arrived in RENDER 0.10.
	 */
	if (n == 1 && op == 1 && rects[0].x == 0 && rects[0].y == 0 &&
	    rects[0].width == 1 && rects[0].height == 1) {
		struct pictrec *pr = pict_find(x, dst);

		if (pr) {
			pr->solid = 1;
			pr->color = *color;
		}
	}
}

void XRenderFillRectangle(Display *dpy, int op, Picture dst,
			  const XRenderColor *color, int x, int y,
			  unsigned int width, unsigned int height)
{
	XRectangle rc;

	rc.x = x; rc.y = y; rc.width = width; rc.height = height;
	XRenderFillRectangles(dpy, op, dst, color, &rc, 1);
}

void XRenderCompositeTrapezoids(Display *dpy, int op, Picture src, Picture dst,
				const XRenderPictFormat *maskFormat,
				int xSrc, int ySrc, const XTrapezoid *traps,
				int ntrap)
{
	struct xrdpy *x = xr_init(dpy);
	unsigned char *r;
	int i;

	if (!x || ntrap <= 0)
		return;
	r = xr_req(x, R_Trapezoids, 24 + ntrap * 40);
	if (!r)
		return;
	r[4] = (unsigned char)op;
	p32(r + 8, src);
	p32(r + 12, dst);
	p32(r + 16, maskFormat ? maskFormat->id : 0);
	p16(r + 20, xSrc);
	p16(r + 22, ySrc);
	for (i = 0; i < ntrap; i++) {
		unsigned char *p = r + 24 + i * 40;
		const XTrapezoid *t = &traps[i];

		p32(p, t->top);
		p32(p + 4, t->bottom);
		p32(p + 8, t->left.p1.x);
		p32(p + 12, t->left.p1.y);
		p32(p + 16, t->left.p2.x);
		p32(p + 20, t->left.p2.y);
		p32(p + 24, t->right.p1.x);
		p32(p + 28, t->right.p1.y);
		p32(p + 32, t->right.p2.x);
		p32(p + 36, t->right.p2.y);
	}
}

/* ----------------------------------------------------------- tessellation */

/*
 * Polygon -> trapezoids, which libXrender does on the client and which is
 * therefore ours now. xclock's dial and hands are drawn entirely through this,
 * so a bug here is a visibly wrong clock rather than a subtle one.
 *
 * The method is the standard one: cut the polygon into horizontal BANDS at
 * every vertex y, and within a band every edge is a straight line, so the
 * crossings can be sorted by x and paired off. Even-odd or winding decides
 * which pairs are inside.
 */
#define MAXPT	64

struct xr_edge {
	XFixed x1, y1, x2, y2;		/* y1 < y2 */
	int dir;			/* +1 downwards, -1 upwards */
};

static XFixed edge_x_at(const struct xr_edge *e, XFixed y)
{
	if (e->y2 == e->y1)
		return e->x1;
	return e->x1 + (XFixed)(((long long)(e->x2 - e->x1) *
				 (y - e->y1)) / (e->y2 - e->y1));
}

void XRenderCompositeDoublePoly(Display *dpy, int op, Picture src, Picture dst,
				const XRenderPictFormat *maskFormat,
				int xSrc, int ySrc, int xDst, int yDst,
				const XPointDouble *fpoints, int npoints,
				int winding)
{
	struct xr_edge edges[MAXPT];
	XFixed ys[MAXPT * 2];
	XTrapezoid traps[MAXPT * 2];
	int nedge = 0, nys = 0, ntrap = 0, i, j;

	if (npoints < 3 || npoints > MAXPT)
		return;

	for (i = 0; i < npoints; i++) {
		int k = (i + 1) % npoints;
		XFixed ax = XDoubleToFixed(fpoints[i].x + xDst);
		XFixed ay = XDoubleToFixed(fpoints[i].y + yDst);
		XFixed bx = XDoubleToFixed(fpoints[k].x + xDst);
		XFixed by = XDoubleToFixed(fpoints[k].y + yDst);
		struct xr_edge *e = &edges[nedge];

		if (ay == by)
			continue;		/* horizontal: contributes none */
		if (ay < by) {
			e->x1 = ax; e->y1 = ay; e->x2 = bx; e->y2 = by;
			e->dir = 1;
		} else {
			e->x1 = bx; e->y1 = by; e->x2 = ax; e->y2 = ay;
			e->dir = -1;
		}
		nedge++;
		ys[nys++] = e->y1;
		ys[nys++] = e->y2;
	}
	if (!nedge)
		return;

	for (i = 0; i < nys; i++)		/* sort the band boundaries */
		for (j = i + 1; j < nys; j++)
			if (ys[j] < ys[i]) {
				XFixed t = ys[i]; ys[i] = ys[j]; ys[j] = t;
			}

	for (i = 0; i + 1 < nys; i++) {
		XFixed top = ys[i], bot = ys[i + 1];
		XFixed mid;
		struct { XFixed x; int dir, idx; } cross[MAXPT];
		int nc = 0, wind = 0;

		if (top >= bot)
			continue;
		mid = top + (bot - top) / 2;
		for (j = 0; j < nedge; j++) {
			if (edges[j].y1 > top || edges[j].y2 < bot)
				continue;	/* does not span the band */
			cross[nc].x = edge_x_at(&edges[j], mid);
			cross[nc].dir = edges[j].dir;
			cross[nc].idx = j;
			nc++;
		}
		for (j = 0; j < nc; j++)	/* sort crossings by x */
			for (int k = j + 1; k < nc; k++)
				if (cross[k].x < cross[j].x) {
					typeof(cross[0]) t = cross[j];
					cross[j] = cross[k]; cross[k] = t;
				}
		for (j = 0; j + 1 < nc; j++) {
			int inside;

			wind += cross[j].dir;
			inside = winding ? (wind != 0) : ((j & 1) == 0);
			if (!inside || ntrap >= MAXPT * 2)
				continue;
			{
				struct xr_edge *l = &edges[cross[j].idx];
				struct xr_edge *r = &edges[cross[j + 1].idx];
				XTrapezoid *t = &traps[ntrap++];

				t->top = top;
				t->bottom = bot;
				t->left.p1.x = l->x1; t->left.p1.y = l->y1;
				t->left.p2.x = l->x2; t->left.p2.y = l->y2;
				t->right.p1.x = r->x1; t->right.p1.y = r->y1;
				t->right.p2.x = r->x2; t->right.p2.y = r->y2;
			}
		}
	}
	if (ntrap)
		XRenderCompositeTrapezoids(dpy, op, src, dst, maskFormat,
					   xSrc, ySrc, traps, ntrap);
}

/*
 * Cursors. lvdesk draws the pointer itself, so the server accepts these and
 * does nothing with them - but the IDENTIFIER still has to be real, because a
 * client stores it and passes it to XDefineCursor afterwards.
 */
Cursor XRenderCreateCursor(Display *dpy, Picture source, unsigned int x,
			   unsigned int y)
{
	struct xrdpy *xd = xr_init(dpy);
	unsigned char *r;
	Cursor cid;

	if (!xd)
		return None;
	cid = XAllocID(dpy);
	r = xr_req(xd, 27, 16);			/* CreateCursor */
	if (!r)
		return None;
	p32(r + 4, cid);
	p32(r + 8, source);
	p16(r + 12, x);
	p16(r + 14, y);
	return cid;
}

Cursor XRenderCreateAnimCursor(Display *dpy, int ncursor,
			       XAnimCursor *cursors)
{
	struct xrdpy *xd = xr_init(dpy);
	unsigned char *r;
	Cursor cid;
	int i;

	if (!xd || ncursor <= 0)
		return None;
	cid = XAllocID(dpy);
	r = xr_req(xd, 34, 8 + ncursor * 8);	/* CreateAnimCursor */
	if (!r)
		return None;
	p32(r + 4, cid);
	for (i = 0; i < ncursor; i++) {
		p32(r + 8 + i * 8, cursors[i].cursor);
		p32(r + 12 + i * 8, cursors[i].delay);
	}
	return cid;
}

void XRenderCompositeDoubleTriangle(Display *dpy, int op, Picture src,
				    Picture dst,
				    const XRenderPictFormat *maskFormat,
				    int xSrc, int ySrc,
				    const XPointDouble *points, int npoints)
{
	(void)dpy; (void)op; (void)src; (void)dst; (void)maskFormat;
	(void)xSrc; (void)ySrc; (void)points; (void)npoints;
	xr_missing("XRenderCompositeDoubleTriangle");
}
