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
		     Pixmap *pix_ret, Pixmap *mask_ret)
{
	struct xpm_col cols[XPM_MAXCOL];
	int w = 0, h = 0, nc = 0, cpp = 1, i, y, ncols = 0;
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
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
		XColor col;

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
				while (*k && *k != ' ' && *k != '\t' &&
				       e < name + sizeof(name) - 1)
					*e++ = *k++;
				*e = 0;
				if (XParseColor(dpy, cmap, name, &col) &&
				    XAllocColor(dpy, cmap, &col))
					cols[i].pixel = col.pixel;
			}
		}
		ncols++;
	}

	pm = XCreatePixmap(dpy, d, w, h,
			   DefaultDepth(dpy, DefaultScreen(dpy)));
	if (!pm) {
		fprintf(stderr, "xstubs: XPM %dx%d: XCreatePixmap failed\n",
			w, h);
		return 3;			/* XpmNoMemory */
	}
	gc = XCreateGC(dpy, pm, 0, NULL);

	/*
	 * Drawn as RUNS of one colour with XFillRectangle rather than through
	 * PutImage, which the shim accepts and ignores. A 16x16 icon is a
	 * handful of requests.
	 */
	for (y = 0; y < h; y++) {
		char *row = lines[1 + ncols + y];
		int x = 0, rowlen;

		if (!row)
			break;
		rowlen = strlen(row);
		while (x < w && rowlen >= (x + 1) * cpp) {
			int run = 1, ci = -1, j;

			for (j = 0; j < ncols; j++)
				if (!memcmp(row + x * cpp, cols[j].key, cpp)) {
					ci = j;
					break;
				}
			while (x + run < w && rowlen >= (x + run + 1) * cpp &&
			       !memcmp(row + (x + run) * cpp,
				       row + x * cpp, cpp))
				run++;
			if (ci >= 0 && !cols[ci].none) {
				XSetForeground(dpy, gc, cols[ci].pixel);
				XFillRectangle(dpy, pm, gc, x, y, run, 1);
			}
			x += run;
		}
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
	(void)attributes;
	return xpm_build(dpy, d, data, 0, pix_ret, mask_ret);
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
	rc = xpm_build(dpy, d, lines, n, pix_ret, mask_ret);
	free(buf);
	return rc;
}


#endif
