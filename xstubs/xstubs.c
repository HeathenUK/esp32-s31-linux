/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Replacements for four X libraries this board maps but barely calls.
 *
 * Audited on a running xcalc, counting how many exported functions the client
 * chain actually references:
 *
 *   libICE    72 kB resident    2 of 108 functions
 *   libSM     32 kB resident   11 of  41, all on the session-manager path
 *   libXext   64 kB resident    2 of 132 (XShapeQueryExtension, ...CombineMask)
 *   libXpm    56 kB resident    2 of  34 (XpmCreatePixmapFromData,
 *                                          XpmReadFileToPixmap)
 *   ------------------------------------------------------------------
 *            224 kB            for FOURTEEN functions
 *
 * Small libraries cost so much because the kernel's fault-around maps ~64 kB
 * around a fault, so a single call pulls in most of one.
 *
 * All four paths are dead on this machine, not merely unused:
 *
 *   - There is no session manager, so SmcOpenConnection cannot succeed. Xt
 *     checks the return and skips the rest, which is exactly what it does on
 *     any machine started without one.
 *   - The shim advertises NO extensions, so XShapeQueryExtension must answer
 *     False and XShapeCombineMask is unreachable.
 *   - XPM is now IMPLEMENTED rather than stubbed. "Nothing here reads XPM
 *     files" stopped being true the moment xfiles was tried, and the note
 *     above naming XpmReadFileToPixmap as the one function used sent the
 *     first attempt at the wrong symbol entirely: xfiles compiles its icons
 *     in and calls XpmCreatePixmapFromData. Check what the client
 *     REFERENCES, not what a comment says.
 *
 * Each function still reports itself once if it is ever called with a result
 * that would matter, so a future client that genuinely needs SHAPE or XPM says
 * so by name instead of misbehaving quietly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef STUB_XPM
#include <X11/Xlib.h>
#include <stdint.h>
extern void *XliteShmMap(Display *, Pixmap, int *, int *, int *, int *);
extern void XliteShmDamaged(Display *, Pixmap);
#endif

static void once(const char *name, const char *what)
{
	static const char *seen[16];
	static int n;
	int i;

	for (i = 0; i < n; i++)
		if (seen[i] == name)
			return;
	if (n < 16)
		seen[n++] = name;
	fprintf(stderr, "xstubs: %s() is stubbed out - %s\n", name, what);
}

#ifdef STUB_ICE
/*
 * Only reachable through a live SM connection, which SmcOpenConnection never
 * returns, so these exist for the linker rather than for the program.
 */
int IceConnectionNumber(void *conn) { (void)conn; return -1; }
int IceProcessMessages(void *conn, void *reply_wait, int *reply_ready)
{
	(void)conn; (void)reply_wait; (void)reply_ready;
	return 1;			/* IceProcessMessagesIOError */
}
#endif

#ifdef STUB_SM
/*
 * SmcOpenConnection returning NULL is the normal, documented result when
 * there is no session manager - it is what every X client sees when
 * SESSION_MANAGER is unset. Xt checks it and moves on.
 */
void *SmcOpenConnection(char *net, void *ctx, int major, int minor,
			unsigned long mask, void *cbs, char *prev, char **id,
			int len, char *err)
{
	(void)net; (void)ctx; (void)major; (void)minor; (void)mask;
	(void)cbs; (void)prev; (void)len; (void)err;
	if (id)
		*id = NULL;
	return NULL;
}

/* Everything below needs a connection SmcOpenConnection never hands out. */
int SmcCloseConnection(void *c, int count, char **reason)
{ (void)c; (void)count; (void)reason; return 0; }
char *SmcClientID(void *c) { (void)c; return NULL; }
void *SmcGetIceConnection(void *c) { (void)c; return NULL; }
void SmcModifyCallbacks(void *c, unsigned long mask, void *cbs)
{ (void)c; (void)mask; (void)cbs; }
void SmcSetProperties(void *c, int n, void **props)
{ (void)c; (void)n; (void)props; }
void SmcDeleteProperties(void *c, int n, char **names)
{ (void)c; (void)n; (void)names; }
int SmcInteractRequest(void *c, int dialog, void *cb, void *data)
{ (void)c; (void)dialog; (void)cb; (void)data; return 0; }
void SmcInteractDone(void *c, int cancel) { (void)c; (void)cancel; }
void SmcRequestSaveYourselfPhase2(void *c, void *cb, void *data)
{ (void)c; (void)cb; (void)data; }
void SmcSaveYourselfDone(void *c, int success) { (void)c; (void)success; }
#endif

#ifdef STUB_XEXT
/*
 * MIT-SHM, answered honestly as absent.
 *
 * SDL's X11 driver dlsyms every one of these and refuses to load the driver if
 * any is missing - so a client that would happily fall back to plain XPutImage
 * instead falls all the way back to fbcon, on a board with no framebuffer
 * console to fall back to. Existing and returning False is what lets that
 * fallback work.
 *
 * False is also the truthful answer twice over: this kernel is built without
 * CONFIG_SYSVIPC, so the shmget() SDL would do next cannot succeed, and the
 * shim does not implement the extension either.
 */
int XShmQueryExtension(void *dpy) { (void)dpy; return 0; }
int XShmQueryVersion(void *dpy, int *maj, int *min, int *pixmaps)
{
	(void)dpy;
	if (maj) *maj = 0;
	if (min) *min = 0;
	if (pixmaps) *pixmaps = 0;
	return 0;
}
void *XShmCreateImage(void *dpy, void *vis, unsigned depth, int fmt, char *data,
		      void *shminfo, unsigned w, unsigned h)
{
	(void)dpy; (void)vis; (void)depth; (void)fmt; (void)data;
	(void)shminfo; (void)w; (void)h;
	return 0;
}
int XShmAttach(void *dpy, void *shminfo) { (void)dpy; (void)shminfo; return 0; }
int XShmDetach(void *dpy, void *shminfo) { (void)dpy; (void)shminfo; return 0; }
int XShmPutImage(void *dpy, unsigned long d, void *gc, void *im, int sx, int sy,
		 int dx, int dy, unsigned w, unsigned h, int send)
{
	(void)dpy; (void)d; (void)gc; (void)im; (void)sx; (void)sy;
	(void)dx; (void)dy; (void)w; (void)h; (void)send;
	return 0;
}

/* Xlib's extension bookkeeping, which the Shm code above would have used. */
void *XextCreateExtension(void) { return 0; }
void XextDestroyExtension(void *e) { (void)e; }
void *XextAddDisplay(void *e, void *dpy, char *n, void *h, int ev, void *d)
{ (void)e; (void)dpy; (void)n; (void)h; (void)ev; (void)d; return 0; }
int XextRemoveDisplay(void *e, void *dpy) { (void)e; (void)dpy; return 0; }
void *XextFindDisplay(void *e, void *dpy) { (void)e; (void)dpy; return 0; }
int XMissingExtension(void *dpy, const char *name) { (void)dpy; (void)name; return 0; }
/*
 * The shim advertises no extensions, so a real libXext would answer False here
 * too - after a round trip. This is the same answer without the 64 kB.
 */
int XShapeQueryExtension(void *dpy, int *event_base, int *error_base)
{
	(void)dpy;
	if (event_base) *event_base = 0;
	if (error_base) *error_base = 0;
	return 0;			/* False: SHAPE is not present */
}

void XShapeCombineMask(void *dpy, unsigned long dest, int kind, int x, int y,
		       unsigned long src, int op)
{
	(void)dpy; (void)dest; (void)kind; (void)x; (void)y; (void)src;
	(void)op;
	/* Reachable only if a client ignores the query above. */
	once("XShapeCombineMask", "the shim advertises no SHAPE extension");
}
#endif

#ifdef STUB_XPM
/*
 * XPM is a file format nothing on this board reads. Returning the failure code
 * is what a real libXpm does for a missing or unreadable file, and callers
 * already handle it - xcalc's icon conversion fails that way with the real
 * library too.
 */
/*
 * XPM, for real.
 *
 * This was stubbed on the grounds that "nothing here reads XPM files", which
 * stopped being true the moment xfiles was tried: it loads five icons and
 * refuses to start without them. The format is a C array of strings, so the
 * parser is a scan for quoted strings - far less code than the 56 kB library,
 * and it means the icons are the client's own rather than something we drew.
 *
 * Pixels are drawn as RUNS of one colour with XFillRectangle rather than
 * through PutImage, because the shim accepts PutImage and does nothing with
 * it. For a 16x16 icon that is a handful of requests.
 */
#define XPM_MAXCOL 256

struct xpm_col { char key[8]; unsigned long pixel; int none; };

/*
 * Colour names already resolved, across every icon.
 *
 * XParseColor and XAllocColor are each a SYNCHRONOUS round trip, and the first
 * version called both once per colour per icon: 779 of each for xfiles' set,
 * 1,558 blocking round trips, and the client was still loading icons after
 * fourteen seconds - it had not reached MapWindow, so no window ever appeared.
 * Icon palettes repeat heavily, so remembering them turns that into a few
 * dozen.
 */
#define XPM_CACHE 128

static struct { char name[40]; unsigned long pixel; int ok; 	int depth;
} xpm_cache[XPM_CACHE];
static int xpm_ncache;

/* Bits and shift of a visual's channel mask. */
static void mask_bits(unsigned long m, int *shift, int *bits)
{
	*shift = 0; *bits = 0;
	if (!m)
		return;
	while (!(m & 1)) { m >>= 1; (*shift)++; }
	while (m & 1)    { m >>= 1; (*bits)++; }
}

/*
 * A hex colour on a TrueColor visual needs no server at all: the pixel IS the
 * components packed into the visual's masks. Asking anyway costs two
 * synchronous round trips per colour, and an icon set is hundreds of colours.
 */
static int xpm_direct(Display *dpy, const char *name, unsigned long *pixel,
		      int depth)
{
	Visual *v = DefaultVisual(dpy, DefaultScreen(dpy));
	unsigned r = 0, g = 0, b = 0;
	int rs, rb, gs, gb, bs, bb, n;

	if (!v || v->class != TrueColor || name[0] != '#')
		return 0;
	n = strlen(name + 1);
	if (n == 6) {
		if (sscanf(name + 1, "%2x%2x%2x", &r, &g, &b) != 3)
			return 0;
	} else if (n == 12) {
		if (sscanf(name + 1, "%4x%4x%4x", &r, &g, &b) != 3)
			return 0;
		r >>= 8; g >>= 8; b >>= 8;
	} else {
		return 0;
	}
	if (depth > 16) {		/* the depth-32 visual: ARGB8888 */
		*pixel = 0xFF000000ul | ((unsigned long)r << 16) |
			 ((unsigned long)g << 8) | b;
		return 1;
	}
	mask_bits(v->red_mask, &rs, &rb);
	mask_bits(v->green_mask, &gs, &gb);
	mask_bits(v->blue_mask, &bs, &bb);
	*pixel = ((unsigned long)(r >> (8 - rb)) << rs) |
		 ((unsigned long)(g >> (8 - gb)) << gs) |
		 ((unsigned long)(b >> (8 - bb)) << bs);
	return 1;
}

static int xpm_color(Display *dpy, Colormap cmap, const char *name,
		     unsigned long *pixel, int depth)
{
	XColor col;
	int i;

	if (xpm_direct(dpy, name, pixel, depth))
		return 1;
	for (i = 0; i < xpm_ncache; i++)
		if (xpm_cache[i].depth == depth &&
		    !strcmp(xpm_cache[i].name, name)) {
			*pixel = xpm_cache[i].pixel;
			return xpm_cache[i].ok;
		}
	col.pixel = 0;
	i = XParseColor(dpy, cmap, name, &col) && XAllocColor(dpy, cmap, &col);
	*pixel = i ? col.pixel : 0;
	if (xpm_ncache < XPM_CACHE) {
		snprintf(xpm_cache[xpm_ncache].name,
			 sizeof(xpm_cache[xpm_ncache].name), "%s", name);
		xpm_cache[xpm_ncache].pixel = *pixel;
		xpm_cache[xpm_ncache].depth = depth;
		xpm_cache[xpm_ncache].ok = i;
		xpm_ncache++;
	}
	return i;
}

/* The next double-quoted string, or NULL. Advances *pp past it. */
static char *xpm_next(char **pp, char *end)
{
	char *p = *pp, *start;

	while (p < end && *p != '"')
		p++;
	if (p >= end)
		return NULL;
	start = ++p;
	while (p < end && *p != '"')
		p++;
	if (p >= end)
		return NULL;
	*p = 0;
	*pp = p + 1;
	return start;
}

/*
 * Build a pixmap from XPM lines. Both entry points land here: the format is
 * identical, only the source differs - xfiles compiles its icons in as a char
 * array, others read a file.
 */
static int xpm_build(Display *dpy, Drawable d, char **lines, int nlines,
		     Pixmap *pix_ret, Pixmap *mask_ret, int depth,
		     Colormap cmap_in)
{
	struct xpm_col cols[XPM_MAXCOL];
	int w = 0, h = 0, nc = 0, cpp = 1, i, y, ncols = 0;
	Colormap cmap = cmap_in ? cmap_in :
			DefaultColormap(dpy, DefaultScreen(dpy));
	Pixmap pm;
	GC gc;

	if (mask_ret)
		*mask_ret = 0;
	/* nlines == 0 means "unknown": compiled-in data carries no count. */
	if (!lines || !lines[0] ||
	    sscanf(lines[0], "%d %d %d %d", &w, &h, &nc, &cpp) < 3 ||
	    w <= 0 || h <= 0 || nc <= 0 || cpp <= 0 || cpp > 7) {
		fprintf(stderr, "xstubs: XPM header rejected: \"%s\"\n",
			lines && lines[0] ? lines[0] : "(null)");
		return 2;			/* XpmFileInvalid */
	}
	if (nc > XPM_MAXCOL)
		nc = XPM_MAXCOL;
	if (nlines && nlines < 1 + nc + h)
		return 2;

	for (i = 0; i < nc; i++) {
		char *c = lines[1 + i], *k;

		if (!c || (int)strlen(c) < cpp)
			break;
		memcpy(cols[i].key, c, cpp);
		cols[i].key[cpp] = 0;
		cols[i].none = 0;
		cols[i].pixel = 0;
		/* "<chars> c <colour>", with s/m/g keys possibly before it. */
		k = strstr(c + cpp, " c ");
		if (!k)
			k = strstr(c + cpp, "\tc ");
		if (k) {
			char name[64], *e = name;

			k += 3;
			while (*k == ' ' || *k == '\t')
				k++;
			if (!strncasecmp(k, "none", 4)) {
				cols[i].none = 1;
			} else {
				unsigned long px;

				while (*k && *k != ' ' && *k != '\t' &&
				       e < name + sizeof(name) - 1)
					*e++ = *k++;
				*e = 0;
				if (xpm_color(dpy, cmap, name, &px,
					      depth > 0 ? depth : 16))
					cols[i].pixel = px;
			}
		}
		ncols++;
	}

	pm = XCreatePixmap(dpy, d, w, h, depth > 0 ? depth :
			   DefaultDepth(dpy, DefaultScreen(dpy)));
	if (!pm) {
		fprintf(stderr, "xstubs: XPM %dx%d: XCreatePixmap failed\n",
			w, h);
		return 3;			/* XpmNoMemory */
	}
	gc = XCreateGC(dpy, pm, 0, NULL);

	/*
	 * One request per COLOUR, not per run.
	 *
	 * The first version drew each run of same-coloured pixels with its own
	 * XFillRectangle, preceded by an XSetForeground - two requests per run.
	 * For xfiles' 64x64 icons that is thousands of round trips each, and
	 * with thirteen icons the client was still loading them after ten
	 * seconds: 10,577 requests logged, all ChangeGC/PolyFillRectangle, and
	 * it had not yet reached MapWindow. The window never appeared because
	 * the icons never finished.
	 *
	 * Gathering every run of one colour and sending them as a single
	 * XFillRectangles turns that into one request per colour - eleven for a
	 * typical icon instead of several thousand.
	 */
	/*
	 * If the server will share the pixmap's pages, just write the pixels.
	 *
	 * This is the whole point of XLITE-SHM: lvdesk is the server and this
	 * is its libX11, so the storage behind a pixmap can be handed over
	 * rather than described. An icon then costs one pass over its own
	 * source data and a single damage message, instead of a request per
	 * colour carrying every run of it - and the run-finding below, which
	 * exists only to compress the protocol, is not needed at all.
	 */
	{
		int sw, sh, stride, bpp;
		void *base = XliteShmMap(dpy, pm, &sw, &sh, &stride, &bpp);

		if (base && (bpp == 2 || bpp == 4) && sw >= w && sh >= h) {
			int yy, x, ci;

			for (yy = 0; yy < h; yy++) {
				char *row = lines[1 + ncols + yy];
				uint16_t *out = (uint16_t *)
					((char *)base + (size_t)yy * stride);

				if (!row)
					break;
				for (x = 0; x < w; x++) {
					if ((int)strlen(row) < (x + 1) * cpp)
						break;
					for (ci = 0; ci < ncols; ci++)
						if (!memcmp(row + x * cpp,
							    cols[ci].key, cpp))
							break;
					if (ci == ncols || cols[ci].none)
						continue;
					if (bpp == 4)
						((uint32_t *)out)[x] =
							(uint32_t)cols[ci].pixel;
					else
						out[x] = (uint16_t)cols[ci].pixel;
				}
			}
			XliteShmDamaged(dpy, pm);
			XFreeGC(dpy, gc);
			/*
			 * The caller's pixmap, which the slow path sets on its
			 * way out. Returning success without it hands back a
			 * pixmap that was filled perfectly and never seen -
			 * the icons drew into shared memory, the server could
			 * read them, and the screen stayed empty.
			 */
			if (pix_ret)
				*pix_ret = pm;
			return 0;		/* XpmSuccess */
		}
	}

	/*
	 * ...but in BATCHES, not one unbounded request per colour.
	 *
	 * Sending every run of a colour in one go makes both buffers scale
	 * with the image: the rectangle array was w*h/2 entries (52 kB for a
	 * 128x103 icon) and the request itself reached 18 kB, which is what
	 * grew xlite's output buffer to 18,480 bytes and left it there for the
	 * life of the client. Neither cost buys anything - the win over the
	 * original was going from thousands of requests to a handful, and a
	 * cap of 512 rectangles keeps that (a 2,300-run colour becomes five
	 * requests, not one) while both buffers stay at 4 kB.
	 */
	{
#define XPM_BATCH	512
		XRectangle *rects = malloc(XPM_BATCH * sizeof(*rects));
		int ci;

		if (!rects) {
			XFreeGC(dpy, gc);
			return 3;		/* XpmNoMemory */
		}
		for (ci = 0; ci < ncols; ci++) {
			int nr = 0, yy;

			if (cols[ci].none)
				continue;	/* transparent: draw nothing */
			for (yy = 0; yy < h; yy++) {
				char *row = lines[1 + ncols + yy];
				int x = 0, rowlen;

				if (!row)
					break;
				rowlen = strlen(row);
				while (x < w && rowlen >= (x + 1) * cpp) {
					int run = 1;

					if (memcmp(row + x * cpp,
						   cols[ci].key, cpp)) {
						x++;
						continue;
					}
					while (x + run < w &&
					       rowlen >= (x + run + 1) * cpp &&
					       !memcmp(row + (x + run) * cpp,
						       cols[ci].key, cpp))
						run++;
					rects[nr].x = x;
					rects[nr].y = yy;
					rects[nr].width = run;
					rects[nr].height = 1;
					nr++;
					x += run;
					if (nr == XPM_BATCH) {
						XSetForeground(dpy, gc,
							cols[ci].pixel);
						XFillRectangles(dpy, pm, gc,
								rects, nr);
						nr = 0;
					}
				}
			}
			if (nr) {
				XSetForeground(dpy, gc, cols[ci].pixel);
				XFillRectangles(dpy, pm, gc, rects, nr);
			}
		}
		free(rects);
	}
	XFreeGC(dpy, gc);
	if (pix_ret)
		*pix_ret = pm;
	return 0;				/* XpmSuccess */
}

/*
 * The one xfiles actually calls: its icons are compiled in, not read from
 * disk. Stubbing this is what made it print "could not open pixmap" five times
 * and give up - and implementing XpmReadFileToPixmap first, on the strength of
 * a comment in this file, fixed nothing at all. Check which symbol the client
 * REFERENCES, not which one sounds right.
 */
int XpmCreatePixmapFromData(Display *dpy, Drawable d, char **data,
			    Pixmap *pix_ret, Pixmap *mask_ret,
			    void *attributes)
{
	/*
	 * The leading fields of XpmAttributes, which is all that is read:
	 * XpmVisual 1<<0, XpmColormap 1<<1, XpmDepth 1<<2. xfiles passes its
	 * depth-32 visual, colormap and depth; ignoring them built every icon
	 * at the default depth 16 and the client composed it as 32-bit - a
	 * dot grid (2026-09-11).
	 */
	struct { unsigned long valuemask; void *visual; Colormap colormap;
		 unsigned int depth; } *a = attributes;
	int depth = (a && (a->valuemask & 4)) ? (int)a->depth : 0;
	Colormap cm = (a && (a->valuemask & 2)) ? a->colormap : 0;

	return xpm_build(dpy, d, data, 0, pix_ret, mask_ret, depth, cm);
}

int XpmReadFileToPixmap(Display *dpy, Drawable d, char *file,
			Pixmap *pix_ret, Pixmap *mask_ret, void *attributes)
{
	char *buf, *p, *end, *lines[1024];
	int n = 0, rc;
	long sz;
	FILE *f = fopen(file, "rb");

	(void)attributes;
	if (mask_ret)
		*mask_ret = 0;
	if (!f)
		return 1;			/* XpmOpenFailed */
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > (1 << 20) || !(buf = malloc(sz + 1))) {
		fclose(f);
		return 1;
	}
	sz = fread(buf, 1, sz, f);
	fclose(f);
	buf[sz] = 0;
	p = buf;
	end = buf + sz;
	while (n < 1024) {
		char *l = xpm_next(&p, end);

		if (!l)
			break;
		lines[n++] = l;
	}
	rc = xpm_build(dpy, d, lines, n, pix_ret, mask_ret, 0, 0);
	free(buf);
	return rc;
}


#endif


/* ------------------------------------------------------------ fontconfig */
#ifdef STUB_FONTCONFIG
/*
 * libfontconfig, for clients that only use it to NAME a font.
 *
 * xfiles references thirteen Fc symbols, and through them drags in
 * fontconfig (8 kB RSS), freetype (20 kB), expat (12 kB) and zlib (8 kB) -
 * 48 kB to describe a font. It calls no FT_ symbol itself: freetype is there
 * only because fontconfig needs it, and expat only because fontconfig parses
 * XML configuration at startup.
 *
 * None of that can matter here. xftlite serves the shim's built-in bitmap
 * fonts, and its XftFontOpenPattern() ignores the pattern entirely while
 * XftFontMatch() hands the same pointer straight back. So a pattern needs to
 * be nothing more than an opaque, destroyable allocation that carries a name
 * and a size for anyone who asks.
 *
 * The one rule: never return NULL from a create or match. Clients test for it
 * and take an error path - xfiles warns "could not open font" and gives up.
 */
typedef unsigned char FcChar8;
typedef unsigned int FcChar32;
typedef int FcBool;
typedef int FcResult;
#define FcResultMatch		0
#define FcResultNoMatch		1

struct fc_pattern {
	char name[64];
	double size;
	int nchars;
	/*
	 * st asks for these back after setting them - pixelsize to lay out its
	 * grid, and the boolean hints when it builds a fallback pattern. A
	 * getter that always failed made it fall back to a zero cell size and
	 * divide by it, so these are stored rather than discarded.
	 */
	int slant, weight, spacing, pixelsize_set;
	double pixelsize;
	int antialias, hinting, autohint, minspace, embolden;
};

static void *fc_alloc(void)
{
	struct fc_pattern *p = calloc(1, sizeof(*p));

	if (p)
		p->size = 12.0;
	return p;
}

FcBool FcInit(void) { return 1; }
void FcFini(void) { }

void *FcPatternCreate(void) { return fc_alloc(); }

void *FcNameParse(const FcChar8 *name)
{
	struct fc_pattern *p = fc_alloc();

	/*
	 * A fontconfig name is "Family-size:option=value". Only the family is
	 * worth keeping, and only so a caller that reads it back sees what it
	 * asked for.
	 */
	if (p && name) {
		size_t i;

		for (i = 0; i < sizeof(p->name) - 1 && name[i] &&
			    name[i] != '-' && name[i] != ':'; i++)
			p->name[i] = (char)name[i];
		p->name[i] = '\0';
	}
	return p;
}

FcBool FcPatternAddDouble(void *pat, const char *object, double d)
{
	struct fc_pattern *p = pat;

	if (p && object && !strcmp(object, "size"))
		p->size = d;
	return 1;
}

FcBool FcPatternAddCharSet(void *pat, const char *object, const void *cs)
{
	(void)pat; (void)object; (void)cs;
	return 1;
}

void FcPatternDestroy(void *pat) { free(pat); }

/*
 * The rest of the fontconfig surface st needs.
 *
 * st is the off-the-shelf terminal we use as a CONTROL: it has its own pty and
 * its own X input handling, so if a fault reproduces in st it is not ours.
 * Without these fifteen symbols it cannot even relocate, and the control does
 * not exist. They are stubs with memory - enough that st computes a sane cell
 * size and draws - not a fontconfig implementation.
 */
FcBool FcPatternAddInteger(void *pat, const char *object, int i)
{
	struct fc_pattern *p = pat;

	if (!p || !object)
		return 1;
	if (!strcmp(object, "slant"))		p->slant = i;
	else if (!strcmp(object, "weight"))	p->weight = i;
	else if (!strcmp(object, "spacing"))	p->spacing = i;
	else if (!strcmp(object, "pixelsize")) {
		p->pixelsize = i;
		p->pixelsize_set = 1;
	}
	return 1;
}

FcBool FcPatternAddBool(void *pat, const char *object, FcBool b)
{
	struct fc_pattern *p = pat;

	if (!p || !object)
		return 1;
	if (!strcmp(object, "antialias"))	p->antialias = b;
	else if (!strcmp(object, "hinting"))	p->hinting = b;
	else if (!strcmp(object, "autohint"))	p->autohint = b;
	else if (!strcmp(object, "minspace"))	p->minspace = b;
	else if (!strcmp(object, "embolden"))	p->embolden = b;
	return 1;
}

FcResult FcPatternGetInteger(const void *pat, const char *object, int n,
			     int *out)
{
	const struct fc_pattern *p = pat;

	(void)n;
	if (!p || !object || !out)
		return FcResultNoMatch;
	if (!strcmp(object, "slant"))		*out = p->slant;
	else if (!strcmp(object, "weight"))	*out = p->weight;
	else if (!strcmp(object, "spacing"))	*out = p->spacing;
	else if (!strcmp(object, "pixelsize"))	*out = (int)(p->pixelsize ?
							     p->pixelsize :
							     p->size);
	else return FcResultNoMatch;
	return FcResultMatch;
}

FcResult FcPatternGetDouble(const void *pat, const char *object, int n,
			    double *out)
{
	const struct fc_pattern *p = pat;

	(void)n;
	if (!p || !object || !out)
		return FcResultNoMatch;
	if (!strcmp(object, "size"))		*out = p->size;
	else if (!strcmp(object, "pixelsize"))	*out = p->pixelsize ?
							p->pixelsize : p->size;
	else return FcResultNoMatch;
	return FcResultMatch;
}

FcBool FcPatternDel(void *pat, const char *object)
{
	struct fc_pattern *p = pat;

	if (!p || !object)
		return 0;
	if (!strcmp(object, "pixelsize")) {
		p->pixelsize = 0;
		p->pixelsize_set = 0;
	} else if (!strcmp(object, "size")) {
		p->size = 0;
	}
	return 1;
}

void *FcPatternDuplicate(const void *pat)
{
	struct fc_pattern *q = fc_alloc();

	if (q && pat)
		memcpy(q, pat, sizeof(*q));
	return q;
}

/*
 * A font set of exactly one: whatever was asked for. st walks the set looking
 * for a face that covers a character; with one entry it always picks that one,
 * which is what the single face this shim has would give anyway.
 */
struct fc_fontset {
	int nfont;
	int sfont;
	void **fonts;
};

static void *fc_fontset_of(const void *pat)
{
	struct fc_fontset *fs = calloc(1, sizeof(*fs));

	if (!fs)
		return NULL;
	fs->fonts = calloc(1, sizeof(void *));
	if (!fs->fonts) { free(fs); return NULL; }
	fs->fonts[0] = FcPatternDuplicate(pat);
	fs->nfont = fs->sfont = 1;
	return fs;
}

void FcFontSetDestroy(void *set)
{
	struct fc_fontset *fs = set;
	int i;

	if (!fs)
		return;
	for (i = 0; i < fs->nfont; i++)
		free(fs->fonts[i]);
	free(fs->fonts);
	free(fs);
}

void *FcFontSetMatch(void *config, void **sets, int nsets, void *pat,
		     FcResult *result)
{
	(void)config; (void)sets; (void)nsets;
	if (result)
		*result = FcResultMatch;
	return FcPatternDuplicate(pat);
}

void *FcFontSort(void *config, void *pat, FcBool trim, void *csp,
		 FcResult *result)
{
	(void)config; (void)trim; (void)csp;
	if (result)
		*result = FcResultMatch;
	return fc_fontset_of(pat);
}


FcBool FcConfigSubstitute(void *config, void *pat, int kind)
{
	(void)config; (void)pat; (void)kind;
	return 1;
}

void FcDefaultSubstitute(void *pat) { (void)pat; }

/*
 * A match must be a SEPARATE allocation from the pattern: callers destroy both
 * (xfiles' font.c does, on every path including the error path), so returning
 * the same pointer is a double free.
 */
void *FcFontMatch(void *config, void *pat, int *result)
{
	struct fc_pattern *p = pat, *m = fc_alloc();

	(void)config;
	if (result)
		*result = 0;			/* FcResultMatch */
	if (m && p)
		*m = *p;
	return m;
}

void *FcCharSetCreate(void) { return calloc(1, sizeof(struct fc_pattern)); }

FcBool FcCharSetAddChar(void *cs, FcChar32 c)
{
	struct fc_pattern *p = cs;

	(void)c;
	if (p)
		p->nchars++;
	return 1;
}

void FcCharSetDestroy(void *cs) { free(cs); }
#endif

/* -------------------------------------------------------------- Xcursor */
#ifdef STUB_XCURSOR
/*
 * libXcursor exists here to answer ONE call. xfiles references exactly one of
 * its symbols - XcursorLibraryLoadCursor - for a busy pointer and a set of
 * drag-and-drop pointers, and pays 32 kB of RSS for it, the largest single
 * library cost in the process after libX11 itself.
 *
 * A themed cursor cannot be honoured anyway: lvdesk draws the pointer through
 * the DRM cursor plane and the shim accepts CreateCursor without acting on it.
 * The name is therefore mapped to the nearest core font cursor, which is what
 * a client without a cursor theme installed would have got in any case.
 */
#include <X11/Xlib.h>
#include <X11/cursorfont.h>

Cursor XcursorLibraryLoadCursor(Display *dpy, const char *name)
{
	static const struct { const char *name; unsigned int shape; } map[] = {
		{ "watch",	XC_watch },
		{ "wait",	XC_watch },
		{ "progress",	XC_watch },
		{ "hand1",	XC_hand1 },
		{ "hand2",	XC_hand2 },
		{ "grabbing",	XC_hand2 },
		{ "dnd-move",	XC_fleur },
		{ "dnd-copy",	XC_hand2 },
		{ "dnd-none",	XC_X_cursor },
		{ "move",	XC_fleur },
		{ "crosshair",	XC_crosshair },
		{ "text",	XC_xterm },
		{ "xterm",	XC_xterm },
	};
	unsigned int shape = XC_left_ptr;
	size_t i;

	if (!dpy)
		return None;
	for (i = 0; name && i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(name, map[i].name)) {
			shape = map[i].shape;
			break;
		}
	return XCreateFontCursor(dpy, shape);
}
#endif
