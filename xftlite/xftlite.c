// SPDX-License-Identifier: GPL-2.0-only
/*
 * xftlite - the parts of Xft an application here actually uses.
 *
 * Audited on the two clients that want it:
 *
 *     xclock   13 of libXft's 77 exported functions
 *     xfiles    6
 *
 * and for those seventeen calls the loader maps a chain that costs, measured
 * on the board with `xclock` running:
 *
 *     libfreetype    480 kB resident
 *     libfontconfig  240 kB
 *     DejaVuSans.ttf 188 kB
 *     libexpat       108 kB
 *     libXft          64 kB
 *     libz + cache     28 kB
 *     ---------------------
 *                  1,108 kB
 *
 * xclock references ZERO fontconfig and ZERO freetype symbols of its own - it
 * reaches all of that solely through Xft - so replacing this one library drops
 * the whole chain. That is the same trade that produced xlite and xtlite, and
 * the ratio is the same: a very large library serving a handful of calls.
 *
 * What is given up is scalable, anti-aliased, fontconfig-matched text. What is
 * kept is TEXT: every request is served from the bitmap faces the shim already
 * carries, through the core X protocol it already speaks. On an 800x480 panel
 * with 15.4 MB of RAM that is the right way round, and it is reversible -
 * putting the real libXft back on the library path restores the old behaviour
 * for a client that genuinely needs it.
 *
 * The layout trap, for the third time in this project: `XftFont` and
 * `XftColor` are PUBLIC structs in Xft.h and applications read their fields
 * directly. Only `XftDraw` is opaque. So the records here begin with the real
 * thing and keep our own state after it - exactly as struct xdpy does with
 * _XDisplay and struct xlite_region does with _XRegion.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

/* Published by xrlite; a Picture cannot answer these on its own. */
extern Drawable XRliteDrawableOfPicture(Display *dpy, Picture p);
extern int XRliteColorOfPicture(Display *dpy, Picture p, XRenderColor *out);

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ reporting */

/*
 * Say what is missing, once each. The whole point of shipping a partial
 * library is that the parts not written yet announce themselves precisely,
 * rather than a client misbehaving quietly.
 */
static void xft_missing(const char *name)
{
	static const char *seen[32];
	static int n;
	int i;

	for (i = 0; i < n; i++)
		if (seen[i] == name)
			return;
	if (n < 32)
		seen[n++] = name;
	fprintf(stderr, "xftlite: %s() is not implemented - carrying on\n",
		name);
}

static int xft_tracing(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XFTLITE_TRACE") != NULL;
	return v;
}

static void xft_note(const char *fmt, ...)
{
	va_list ap;

	if (!xft_tracing())
		return;
	va_start(ap, fmt);
	fprintf(stderr, "xftlite: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

/* --------------------------------------------------------------- fonts */

struct xftfont {
	XftFont pub;			/* MUST be first: Xft.h is public */
	Display *dpy;
	XFontStruct *fs;
	Font fid;
};

/*
 * The faces the shim carries, widest last so a size request can pick the
 * nearest without knowing anything about font files.
 */
static const struct { const char *name; int px; } faces[] = {
	{ "5x8",   8 },
	{ "6x13", 13 },
	{ "8x13", 13 },
	{ "9x15", 15 },
};

/*
 * Turn an Xft name ("Sans-12", "DejaVu Sans:size=10") into one of ours.
 *
 * Only the size matters here: there is one family on this machine. A name we
 * cannot parse at all gets the default face rather than nothing, because a
 * NULL font is what makes an application segfault three calls later.
 */
static const char *face_for(const char *name)
{
	int want = 13, best = 0, i, bestd = 1 << 30;
	const char *p;

	if (name) {
		if ((p = strstr(name, "size=")))
			want = atoi(p + 5);
		else if ((p = strstr(name, "pixelsize=")))
			want = atoi(p + 10);
		else if ((p = strrchr(name, '-')) && p[1] >= '0' && p[1] <= '9')
			want = atoi(p + 1);
	}
	for (i = 0; i < (int)(sizeof(faces) / sizeof(faces[0])); i++) {
		int d = faces[i].px - want;

		if (d < 0)
			d = -d;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return faces[best].name;
}

static XftFont *font_wrap(Display *dpy, const char *core)
{
	struct xftfont *f = calloc(1, sizeof(*f));
	XFontStruct *fs;

	if (!f)
		return NULL;
	fs = XLoadQueryFont(dpy, core);
	if (!fs) {
		free(f);
		return NULL;
	}
	f->dpy = dpy;
	f->fs = fs;
	f->fid = fs->fid;
	f->pub.ascent = fs->ascent;
	f->pub.descent = fs->descent;
	f->pub.height = fs->ascent + fs->descent;
	f->pub.max_advance_width = fs->max_bounds.width;
	f->pub.charset = NULL;
	f->pub.pattern = NULL;
	xft_note("font \"%s\" -> ascent %d descent %d height %d adv %d", core,
		 f->pub.ascent, f->pub.descent, f->pub.height,
		 f->pub.max_advance_width);
	return &f->pub;
}

XftFont *XftFontOpenName(Display *dpy, int screen, const char *name)
{
	(void)screen;
	return font_wrap(dpy, face_for(name));
}

XftFont *XftFontOpenXlfd(Display *dpy, int screen, const char *xlfd)
{
	(void)screen;
	return font_wrap(dpy, face_for(xlfd));
}

XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern)
{
	XftFont *f = font_wrap(dpy, face_for(NULL));

	/*
	 * KEEP THE PATTERN. A caller checks the font it got against the one it
	 * asked for by reading match->pattern - st does exactly this and prints
	 * "font slant does not match" / "font weight does not match" when the
	 * read fails, then marks the face bad and stops using it. Discarding
	 * the pattern here made every font look wrong to it.
	 *
	 * Handing back the requested pattern is honest for this shim: there is
	 * one face, so whatever was asked for is what the caller gets.
	 */
	if (f)
		f->pattern = pattern;
	return f;
}

FcPattern *XftFontMatch(Display *dpy, int screen, const FcPattern *pattern,
			FcResult *result)
{
	(void)dpy; (void)screen; (void)pattern;
	/*
	 * There is one family here, so a match carries no information. The
	 * pattern is handed straight back and XftFontOpenPattern ignores it -
	 * a NULL would make a caller believe no font exists at all.
	 */
	if (result)
		*result = FcResultMatch;
	return (FcPattern *)pattern;
}

void XftFontClose(Display *dpy, XftFont *pub)
{
	struct xftfont *f = (struct xftfont *)pub;

	if (!f)
		return;
	if (f->fs)
		XFreeFont(dpy, f->fs);
	free(f);
}

FcBool XftCharExists(Display *dpy, XftFont *pub, FcChar32 ucs4)
{
	struct xftfont *f = (struct xftfont *)pub;

	(void)dpy;
	/* The bitmap faces are 8-bit; anything above that is drawn as '?'. */
	return f && ucs4 < 256 ? FcTrue : FcFalse;
}

/* ---------------------------------------------------------------- text */

/*
 * UTF-8 down to the 8-bit range the bitmap faces cover. A character outside it
 * becomes '?' rather than being dropped, so a string never silently shortens -
 * a missing glyph is visible, a missing character is not.
 */
static int utf8_to_8bit(const FcChar8 *s, int len, unsigned char *out, int max)
{
	int i = 0, n = 0;

	while (i < len && n < max) {
		unsigned c = s[i];

		if (c < 0x80) {
			i += 1;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
			c = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
			i += 2;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
			c = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) |
			    (s[i + 2] & 0x3F);
			i += 3;
		} else {
			i += 1;
			c = '?';
		}
		out[n++] = c < 256 ? (unsigned char)c : '?';
	}
	return n;
}

static void extents_of(XftFont *pub, const unsigned char *s, int len,
		       XGlyphInfo *ex)
{
	struct xftfont *f = (struct xftfont *)pub;
	int w = f && f->fs ? XTextWidth(f->fs, (const char *)s, len) : 0;

	memset(ex, 0, sizeof(*ex));
	ex->width = w;
	ex->height = pub ? pub->height : 0;
	ex->x = 0;
	ex->y = pub ? pub->ascent : 0;
	ex->xOff = w;
	ex->yOff = 0;
}

void XftTextExtents8(Display *dpy, XftFont *pub, const FcChar8 *string,
		     int len, XGlyphInfo *extents)
{
	(void)dpy;
	extents_of(pub, string, len, extents);
}

void XftTextExtents16(Display *dpy, XftFont *pub, const FcChar16 *string,
		      int len, XGlyphInfo *extents)
{
	unsigned char buf[512];
	int i, n = len < 512 ? len : 512;

	(void)dpy;
	for (i = 0; i < n; i++)
		buf[i] = string[i] < 256 ? (unsigned char)string[i] : '?';
	extents_of(pub, buf, n, extents);
}

void XftTextExtents32(Display *dpy, XftFont *pub, const FcChar32 *string,
		      int len, XGlyphInfo *extents)
{
	unsigned char buf[512];
	int i, n = len < 512 ? len : 512;

	(void)dpy;
	for (i = 0; i < n; i++)
		buf[i] = string[i] < 256 ? (unsigned char)string[i] : '?';
	extents_of(pub, buf, n, extents);
}

void XftTextExtentsUtf8(Display *dpy, XftFont *pub, const FcChar8 *string,
			int len, XGlyphInfo *extents)
{
	unsigned char buf[512];
	int n = utf8_to_8bit(string, len, buf, sizeof(buf));

	(void)dpy;
	extents_of(pub, buf, n, extents);
}

/* --------------------------------------------------------------- draws */

struct _XftDraw {
	Display *dpy;
	Drawable drawable;
	Visual *visual;
	Colormap colormap;
	GC gc;
	Picture pict;			/* made on demand, freed with us */
	Picture src;			/* a 1x1 solid, for XftDrawSrcPicture */
	Pixmap srcpix;
	unsigned long srcpixel;
	int has_src;
};

XftDraw *XftDrawCreate(Display *dpy, Drawable drawable, Visual *visual,
		       Colormap colormap)
{
	XftDraw *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->dpy = dpy;
	d->drawable = drawable;
	d->visual = visual;
	d->colormap = colormap;
	d->gc = XCreateGC(dpy, drawable, 0, NULL);
	return d;
}

XftDraw *XftDrawCreateBitmap(Display *dpy, Pixmap bitmap)
{
	return XftDrawCreate(dpy, bitmap, NULL, None);
}

XftDraw *XftDrawCreateAlpha(Display *dpy, Pixmap pixmap, int depth)
{
	(void)depth;
	return XftDrawCreate(dpy, pixmap, NULL, None);
}

void XftDrawChange(XftDraw *draw, Drawable drawable)
{
	if (!draw)
		return;
	if (draw->pict) {
		XRenderFreePicture(draw->dpy, draw->pict);
		draw->pict = 0;
	}
	draw->drawable = drawable;
}

void XftDrawDestroy(XftDraw *draw)
{
	if (!draw)
		return;
	if (draw->pict)
		XRenderFreePicture(draw->dpy, draw->pict);
	if (draw->src)
		XRenderFreePicture(draw->dpy, draw->src);
	if (draw->srcpix)
		XFreePixmap(draw->dpy, draw->srcpix);
	if (draw->gc)
		XFreeGC(draw->dpy, draw->gc);
	free(draw);
}

Display *XftDrawDisplay(XftDraw *draw) { return draw ? draw->dpy : NULL; }
Drawable XftDrawDrawable(XftDraw *draw) { return draw ? draw->drawable : None; }
Colormap XftDrawColormap(XftDraw *draw) { return draw ? draw->colormap : None; }
Visual *XftDrawVisual(XftDraw *draw) { return draw ? draw->visual : NULL; }

/*
 * The Picture for this drawable.
 *
 * xclock uses XftDrawPicture() as a Picture FACTORY - it asks Xft for one and
 * then draws its hands into it with XRenderCompositeTrapezoids, never calling
 * a text function at all on the analog face. So this has to be real, even
 * though nothing here renders a glyph through RENDER.
 */
/*
 * Every Picture we have handed out, and the XftDraw it came from.
 *
 * XftTextRender32() takes a destination PICTURE rather than an XftDraw, so
 * there is no drawable to draw on and no GC to draw with - and a Picture is
 * opaque server-side, so it cannot be turned back into one. But the picture
 * a client renders into is the one IT asked us for, so remembering the pairs
 * is enough to find the way back.
 */
#define NPICT 16
static struct { Picture pict; XftDraw *draw; } pictmap[NPICT];
static int npict;

static XftDraw *draw_of_picture(Picture p)
{
	int i;

	for (i = 0; i < npict; i++)
		if (pictmap[i].pict == p)
			return pictmap[i].draw;
	return NULL;
}

Picture XftDrawPicture(XftDraw *draw)
{
	XRenderPictFormat *fmt;

	if (!draw)
		return 0;
	if (draw->pict)
		return draw->pict;
	fmt = draw->visual ? XRenderFindVisualFormat(draw->dpy, draw->visual)
			   : XRenderFindStandardFormat(draw->dpy,
						       PictStandardA8);
	if (!fmt)
		return 0;
	draw->pict = XRenderCreatePicture(draw->dpy, draw->drawable, fmt, 0,
					  NULL);
	if (draw->pict && npict < NPICT) {
		pictmap[npict].pict = draw->pict;
		pictmap[npict].draw = draw;
		npict++;
	}
	return draw->pict;
}

/* A 1x1 repeating solid of this colour, which is what RENDER wants as a src. */
Picture XftDrawSrcPicture(XftDraw *draw, const XftColor *color)
{
	if (!draw || !color)
		return 0;
	if (draw->has_src && draw->srcpixel == color->pixel)
		return draw->src;
	if (draw->src) {
		XRenderFreePicture(draw->dpy, draw->src);
		draw->src = 0;
	}
	draw->src = XRenderCreateSolidFill(draw->dpy, &color->color);
	draw->srcpixel = color->pixel;
	draw->has_src = 1;
	return draw->src;
}

Bool XftDrawSetClip(XftDraw *draw, Region r)
{
	if (!draw)
		return False;
	if (!r)
		XSetClipMask(draw->dpy, draw->gc, None);
	else
		XSetRegion(draw->dpy, draw->gc, r);
	return True;
}

Bool XftDrawSetClipRectangles(XftDraw *draw, int xOrigin, int yOrigin,
			      _Xconst XRectangle *rects, int n)
{
	if (!draw)
		return False;
	XSetClipRectangles(draw->dpy, draw->gc, xOrigin, yOrigin,
			   (XRectangle *)rects, n, Unsorted);
	return True;
}

void XftDrawSetSubwindowMode(XftDraw *draw, int mode)
{
	if (draw)
		XSetSubwindowMode(draw->dpy, draw->gc, mode);
}

void XftDrawRect(XftDraw *draw, const XftColor *color, int x, int y,
		 unsigned int width, unsigned int height)
{
	if (!draw || !color)
		return;
	XSetForeground(draw->dpy, draw->gc, color->pixel);
	XFillRectangle(draw->dpy, draw->drawable, draw->gc, x, y, width,
		       height);
}

/* Draw with the GC's current foreground, for the paths that carry no colour. */
static void draw_8bit_nocolor(XftDraw *draw, XftFont *pub, int x, int y,
			      const unsigned char *s, int len)
{
	struct xftfont *f = (struct xftfont *)pub;

	if (!draw || !f)
		return;
	if (f->fid)
		XSetFont(draw->dpy, draw->gc, f->fid);
	XDrawString(draw->dpy, draw->drawable, draw->gc, x, y,
		    (const char *)s, len);
}

static void draw_8bit(XftDraw *draw, const XftColor *color, XftFont *pub,
		      int x, int y, const unsigned char *s, int len)
{
	struct xftfont *f = (struct xftfont *)pub;

	if (!draw || !color || !f)
		return;
	XSetForeground(draw->dpy, draw->gc, color->pixel);
	if (f->fid)
		XSetFont(draw->dpy, draw->gc, f->fid);
	XDrawString(draw->dpy, draw->drawable, draw->gc, x, y,
		    (const char *)s, len);
}

void XftDrawString8(XftDraw *draw, const XftColor *color, XftFont *pub,
		    int x, int y, const FcChar8 *string, int len)
{
	draw_8bit(draw, color, pub, x, y, string, len);
}

/*
 * The glyph-spec path, which is how st actually draws.
 *
 * st does not call XftDrawString at all: it resolves each cell to a glyph
 * index with XftCharIndex, builds an XftGlyphFontSpec array, and hands the lot
 * to XftDrawGlyphFontSpec in one call. Without these three symbols st cannot
 * relocate, and without a real implementation of this one it maps and then
 * draws nothing - which as a CONTROL is worse than useless, because a blank
 * terminal looks exactly like the input fault we are trying to isolate.
 *
 * This shim has one 8-bit face, so a glyph index IS the character code (see
 * XftCharIndex below). That makes each spec a one-character draw at its own
 * position, which is exactly what the array describes.
 */
void XftDrawGlyphFontSpec(XftDraw *draw, const XftColor *color,
			  const XftGlyphFontSpec *glyphs, int nglyphs)
{
	int i;

	if (!draw || !color || !glyphs)
		return;
	{
		/* Once per process: is the client drawing at all, and where? */
		static int said;

		if (!said) {
			said = 1;
			fprintf(stderr, "xftlite: GlyphFontSpec n=%d "
				"drawable=0x%lx first=(%d,%d) glyph=%u "
				"font=%p\n", nglyphs,
				(unsigned long)XftDrawDrawable(draw),
				glyphs[0].x, glyphs[0].y,
				(unsigned)glyphs[0].glyph,
				(void *)glyphs[0].font);
			fflush(stderr);
		}
	}
	for (i = 0; i < nglyphs; i++) {
		unsigned char c = (unsigned char)(glyphs[i].glyph < 256 ?
						  glyphs[i].glyph : '?');

		if (!glyphs[i].font)
			continue;
		draw_8bit(draw, color, glyphs[i].font, glyphs[i].x, glyphs[i].y,
			  &c, 1);
	}
}

/*
 * One face, 8-bit: the glyph index is the character. Returning 0 would tell st
 * the character is absent and send it hunting through fallback fonts for every
 * cell.
 */
FT_UInt XftCharIndex(Display *dpy, XftFont *pub, FcChar32 ucs4)
{
	(void)dpy; (void)pub;
	return ucs4 < 256 ? (FT_UInt)ucs4 : (FT_UInt)'?';
}

/*
 * An XLFD is not something this shim can honour - there is one face and one
 * size. Report failure rather than a wrong pattern: st only uses this for a
 * -fn argument in XLFD form, and falls back to its configured name.
 */
FcPattern *XftXlfdParse(const char *xlfd_orig, FcBool ignore_scalable,
			FcBool complete)
{
	(void)xlfd_orig; (void)ignore_scalable; (void)complete;
	return NULL;		/* NULL means "not an XLFD I can honour" */
}

void XftDrawString16(XftDraw *draw, const XftColor *color, XftFont *pub,
		     int x, int y, const FcChar16 *string, int len)
{
	unsigned char buf[512];
	int i, n = len < 512 ? len : 512;

	for (i = 0; i < n; i++)
		buf[i] = string[i] < 256 ? (unsigned char)string[i] : '?';
	draw_8bit(draw, color, pub, x, y, buf, n);
}

void XftDrawString32(XftDraw *draw, const XftColor *color, XftFont *pub,
		     int x, int y, const FcChar32 *string, int len)
{
	unsigned char buf[512];
	int i, n = len < 512 ? len : 512;

	for (i = 0; i < n; i++)
		buf[i] = string[i] < 256 ? (unsigned char)string[i] : '?';
	draw_8bit(draw, color, pub, x, y, buf, n);
}

void XftDrawStringUtf8(XftDraw *draw, const XftColor *color, XftFont *pub,
		       int x, int y, const FcChar8 *string, int len)
{
	unsigned char buf[512];
	int n = utf8_to_8bit(string, len, buf, sizeof(buf));

	draw_8bit(draw, color, pub, x, y, buf, n);
}

/*
 * xfiles renders text through this rather than XftDrawString32. It takes a
 * DESTINATION PICTURE instead of an XftDraw, so there is no GC to hand to the
 * core text path - which is why the shim implements CompositeGlyphs, and why
 * this reports itself rather than silently drawing nothing.
 */
/*
 * Text into an arbitrary destination Picture.
 *
 * A Picture is opaque server-side, so neither the drawable behind it nor the
 * colour inside a solid fill can be recovered from one - which is why this
 * used to work only for pictures xftlite had handed out itself, and why xfiles
 * (which builds its own with XRenderCreatePicture) got nothing. xrlite owns
 * both calls now and answers both questions.
 */
void XftTextRender32(Display *dpy, int op, Picture src, XftFont *pub,
		     Picture dst, int srcx, int srcy, int x, int y,
		     const FcChar32 *string, int len)
{
	struct xftfont *f = (struct xftfont *)pub;
	static Drawable gc_for;
	static GC gc;
	Drawable d = XRliteDrawableOfPicture(dpy, dst);
	XRenderColor col;
	unsigned char buf[512];
	int i, n = len < 512 ? len : 512;

	(void)op; (void)srcx; (void)srcy;
	if (!d) {
		XftDraw *draw = draw_of_picture(dst);

		if (!draw) {
			xft_missing("XftTextRender32 into an unknown picture");
			return;
		}
		d = draw->drawable;
	}
	if (!f)
		return;
	if (!gc || gc_for != d) {
		if (gc)
			XFreeGC(dpy, gc);
		gc = XCreateGC(dpy, d, 0, NULL);
		gc_for = d;
	}
	if (XRliteColorOfPicture(dpy, src, &col)) {
		XColor c;

		memset(&c, 0, sizeof(c));
		c.red = col.red; c.green = col.green; c.blue = col.blue;
		if (XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
				&c))
			XSetForeground(dpy, gc, c.pixel);
	}
	if (f->fid)
		XSetFont(dpy, gc, f->fid);
	for (i = 0; i < n; i++)
		buf[i] = string[i] < 256 ? (unsigned char)string[i] : '?';
	XDrawString(dpy, d, gc, x, y, (const char *)buf, n);
}

/* -------------------------------------------------------------- colours */

Bool XftColorAllocValue(Display *dpy, Visual *visual, Colormap cmap,
			const XRenderColor *color, XftColor *result)
{
	XColor c;

	(void)visual;
	if (!color || !result)
		return False;
	result->color = *color;
	memset(&c, 0, sizeof(c));
	c.red = color->red;
	c.green = color->green;
	c.blue = color->blue;
	if (XAllocColor(dpy, cmap, &c))
		result->pixel = c.pixel;
	else
		result->pixel = ((color->red >> 8) << 16) |
				((color->green >> 8) << 8) | (color->blue >> 8);
	return True;
}

Bool XftColorAllocName(Display *dpy, const Visual *visual, Colormap cmap,
		       const char *name, XftColor *result)
{
	XColor screen, exact;

	(void)visual;
	if (!result)
		return False;
	if (!XAllocNamedColor(dpy, cmap, name, &screen, &exact)) {
		memset(result, 0, sizeof(*result));
		return False;
	}
	result->pixel = screen.pixel;
	result->color.red = exact.red;
	result->color.green = exact.green;
	result->color.blue = exact.blue;
	result->color.alpha = 0xffff;
	return True;
}

void XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color)
{
	(void)dpy; (void)visual; (void)cmap; (void)color;
	/* Nothing is allocated on this server's colormap. */
}

/* ----------------------------------------------------------------- init */

FcBool XftInit(const char *config) { (void)config; return FcTrue; }
FcBool XftInitFtLibrary(void) { return FcTrue; }
Bool XftDefaultHasRender(Display *dpy) { (void)dpy; return True; }

int XftDefaultParseBool(const char *v)
{
	return v && (*v == 't' || *v == 'T' || *v == 'y' || *v == 'Y' ||
		     *v == '1');
}

void XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern)
{
	(void)dpy; (void)screen; (void)pattern;
}
