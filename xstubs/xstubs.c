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
 *   libXpm    56 kB resident    1 of  34 (XpmReadFileToPixmap)
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
 *   - Nothing here reads XPM files.
 *
 * Each function still reports itself once if it is ever called with a result
 * that would matter, so a future client that genuinely needs SHAPE or XPM says
 * so by name instead of misbehaving quietly.
 */
#include <stdio.h>
#include <stdlib.h>

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
int XpmReadFileToPixmap(void *dpy, unsigned long d, char *file,
			unsigned long *pix, unsigned long *shape, void *attr)
{
	(void)dpy; (void)d; (void)file; (void)attr;
	if (pix) *pix = 0;
	if (shape) *shape = 0;
	once("XpmReadFileToPixmap", "no XPM support is built in");
	return -3;			/* XpmOpenFailed */
}

int XpmCreatePixmapFromData(void *dpy, unsigned long d, char **data,
			    unsigned long *pix, unsigned long *shape,
			    void *attr)
{
	(void)dpy; (void)d; (void)data; (void)attr;
	if (pix) *pix = 0;
	if (shape) *shape = 0;
	once("XpmCreatePixmapFromData", "no XPM support is built in");
	return -3;
}
#endif
