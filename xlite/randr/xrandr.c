/*
 * libXrandr.so.2 for xlite: the RANDR client SDL2 dlopens by that name.
 *
 * SDL 2.32's X11 backend has no XVidMode code at all - it was dropped years
 * ago - so XRandR is the only way it will ever learn that a mode other than
 * the desktop exists. Without one, SDL_WINDOW_FULLSCREEN means a window the
 * size of the panel, the client scales its frame on the CPU, and a 32-bit
 * 800x480 chocolate-doom frame is copied four times before it is seen (0.2
 * fps, and the board swapped itself to death). With it, SDL picks the mode
 * it wants, xshim switches the panel the way it does for VidMode, and the
 * client renders at 320x200 into a window the PPA scales - the SDL 1.2 path.
 *
 * The real libXrandr is written against Xlib internals xlite does not have
 * (_XReply, the request macros, per-display extension records), so this is
 * the protocol subset SDL walks, on xlite's own request primitives. It is
 * the whole symbol list SDL_x11sym.h names for the XRANDR module, because
 * SDL loads them all or treats the extension as absent. The 1.0-era
 * screen-configuration calls exist and report nothing; SDL never uses them.
 *
 * Wire layout per randrproto.h. The server end is randr_request() in
 * lvdesk/xshim.c.
 */
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/randrproto.h>
#include "xlite.h"

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

/* One display per process here; cache the opcode like xlite's MIT-SHM does. */
static int rr_major, rr_event, rr_error, rr_probed;

static int rr_opcode(Display *dpy)
{
	if (!rr_probed) {
		rr_probed = 1;
		if (!XQueryExtension(dpy, "RANDR", &rr_major, &rr_event,
				     &rr_error))
			rr_major = 0;
	}
	return rr_major;
}

/*
 * Build a request of `words` 32-bit words with the RANDR minor `minor`,
 * send it, and (if `hdr` is given) wait for its reply. Returns the request
 * buffer's fill area through *out before sending, via the callback.
 */
struct rrreq {
	unsigned char *r;
	struct xdpy *x;
	uint32_t seq;
};

static int rr_begin(Display *dpy, int minor, int words, struct rrreq *q)
{
	int major = rr_opcode(dpy);

	if (!major)
		return 0;
	q->x = XD(dpy);
	q->r = xlite_req(q->x, major, minor, words);
	if (!q->r)
		return 0;
	memset(q->r + 4, 0, (size_t)words * 4 - 4);
	q->seq = q->x->pub.request;
	return 1;
}

static void rr_send(struct rrreq *q)
{
	xlite_send(q->x, q->r);
}

/* Send and collect the reply: 32-byte header, malloc'd extra (free it). */
static int rr_round(struct rrreq *q, unsigned char *hdr, unsigned char **extra,
		    size_t *nextra)
{
	*extra = NULL;
	*nextra = 0;
	xlite_send(q->x, q->r);
	return xlite_reply(q->x, q->seq, hdr, extra, nextra);
}

Bool XRRQueryExtension(Display *dpy, int *event_base, int *error_base)
{
	if (!rr_opcode(dpy))
		return False;
	*event_base = rr_event;
	*error_base = rr_error;
	return True;
}

Status XRRQueryVersion(Display *dpy, int *major, int *minor)
{
	struct rrreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (!rr_begin(dpy, X_RRQueryVersion, 3, &q))
		return 0;
	p32(q.r + 4, 1);
	p32(q.r + 8, 5);
	if (!rr_round(&q, hdr, &ex, &nex))
		return 0;
	free(ex);
	*major = (int)g32(hdr + 8);
	*minor = (int)g32(hdr + 12);
	return 1;
}

void XRRSelectInput(Display *dpy, Window window, int mask)
{
	struct rrreq q;

	if (!rr_begin(dpy, X_RRSelectInput, 3, &q))
		return;
	p32(q.r + 4, window);
	p16(q.r + 8, (unsigned)mask);
	rr_send(&q);
}

Status XRRGetScreenSizeRange(Display *dpy, Window window, int *minWidth,
			     int *minHeight, int *maxWidth, int *maxHeight)
{
	struct rrreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (!rr_begin(dpy, X_RRGetScreenSizeRange, 2, &q))
		return 0;
	p32(q.r + 4, window);
	if (!rr_round(&q, hdr, &ex, &nex))
		return 0;
	free(ex);
	*minWidth = g16(hdr + 8);
	*minHeight = g16(hdr + 10);
	*maxWidth = g16(hdr + 12);
	*maxHeight = g16(hdr + 14);
	return 1;
}

void XRRSetScreenSize(Display *dpy, Window window, int width, int height,
		      int mmWidth, int mmHeight)
{
	struct rrreq q;

	if (!rr_begin(dpy, X_RRSetScreenSize, 5, &q))
		return;
	p32(q.r + 4, window);
	p16(q.r + 8, width);
	p16(q.r + 10, height);
	p32(q.r + 12, mmWidth);
	p32(q.r + 16, mmHeight);
	rr_send(&q);
}

/*
 * XRRScreenResources and its arrays live in one malloc'd block so the free
 * is one call: [struct][crtcs][outputs][modes][names]. Mode names point
 * into the names area and are NUL-terminated there for convenience (the
 * protocol does not terminate them; nameLength is authoritative).
 */
static XRRScreenResources *rr_resources(Display *dpy, Window window, int minor)
{
	struct rrreq q;
	unsigned char hdr[32], *ex, *p;
	size_t nex;
	unsigned ncrtc, nout, nmode, nbytes, i;
	XRRScreenResources *res;
	char *names;

	if (!rr_begin(dpy, minor, 2, &q))
		return NULL;
	p32(q.r + 4, window);
	if (!rr_round(&q, hdr, &ex, &nex))
		return NULL;
	ncrtc = g16(hdr + 16);
	nout = g16(hdr + 18);
	nmode = g16(hdr + 20);
	nbytes = g16(hdr + 22);
	if (nex < (size_t)ncrtc * 4 + (size_t)nout * 4 + (size_t)nmode * 32 +
		  nbytes) {
		free(ex);
		return NULL;
	}
	res = calloc(1, sizeof *res + (ncrtc + nout) * sizeof(XID) +
			nmode * sizeof(XRRModeInfo) + nbytes + nmode + 1);
	if (!res) {
		free(ex);
		return NULL;
	}
	res->timestamp = g32(hdr + 8);
	res->configTimestamp = g32(hdr + 12);
	res->ncrtc = (int)ncrtc;
	res->crtcs = (RRCrtc *)(res + 1);
	res->noutput = (int)nout;
	res->outputs = res->crtcs + ncrtc;
	res->nmode = (int)nmode;
	res->modes = (XRRModeInfo *)(res->outputs + nout);
	names = (char *)(res->modes + nmode);
	p = ex;
	for (i = 0; i < ncrtc; i++, p += 4)
		res->crtcs[i] = g32(p);
	for (i = 0; i < nout; i++, p += 4)
		res->outputs[i] = g32(p);
	for (i = 0; i < nmode; i++, p += 32) {
		XRRModeInfo *m = &res->modes[i];

		m->id = g32(p + 0);
		m->width = g16(p + 4);
		m->height = g16(p + 6);
		m->dotClock = g32(p + 8);
		m->hSyncStart = g16(p + 12);
		m->hSyncEnd = g16(p + 14);
		m->hTotal = g16(p + 16);
		m->hSkew = g16(p + 18);
		m->vSyncStart = g16(p + 20);
		m->vSyncEnd = g16(p + 22);
		m->vTotal = g16(p + 24);
		m->nameLength = g16(p + 26);
		m->modeFlags = g32(p + 28);
	}
	for (i = 0; i < nmode; i++) {
		XRRModeInfo *m = &res->modes[i];

		m->name = names;
		memcpy(names, p, m->nameLength);
		names[m->nameLength] = 0;
		names += m->nameLength + 1;
		p += m->nameLength;
	}
	free(ex);
	return res;
}

XRRScreenResources *XRRGetScreenResources(Display *dpy, Window window)
{
	return rr_resources(dpy, window, X_RRGetScreenResources);
}

XRRScreenResources *XRRGetScreenResourcesCurrent(Display *dpy, Window window)
{
	return rr_resources(dpy, window, X_RRGetScreenResourcesCurrent);
}

void XRRFreeScreenResources(XRRScreenResources *resources)
{
	free(resources);
}

XRROutputInfo *XRRGetOutputInfo(Display *dpy, XRRScreenResources *resources,
				RROutput output)
{
	struct rrreq q;
	unsigned char hdr[32], *ex, *p;
	size_t nex;
	unsigned ncrtc, nmode, nclone, nlen, i;
	XRROutputInfo *oi;

	if (!rr_begin(dpy, X_RRGetOutputInfo, 3, &q))
		return NULL;
	p32(q.r + 4, output);
	p32(q.r + 8, resources ? resources->configTimestamp : 0);
	if (!rr_round(&q, hdr, &ex, &nex))
		return NULL;
	if (hdr[1] != RRSetConfigSuccess || nex < 4) {
		free(ex);
		return NULL;
	}
	ncrtc = g16(hdr + 26);
	nmode = g16(hdr + 28);
	nclone = g16(ex + 0);
	nlen = g16(ex + 2);
	if (nex < 4 + (size_t)(ncrtc + nmode + nclone) * 4 + nlen) {
		free(ex);
		return NULL;
	}
	oi = calloc(1, sizeof *oi + (ncrtc + nmode + nclone) * sizeof(XID) +
		       nlen + 1);
	if (!oi) {
		free(ex);
		return NULL;
	}
	oi->timestamp = g32(hdr + 8);
	oi->crtc = g32(hdr + 12);
	oi->mm_width = g32(hdr + 16);
	oi->mm_height = g32(hdr + 20);
	oi->connection = hdr[24];
	oi->subpixel_order = hdr[25];
	oi->ncrtc = (int)ncrtc;
	oi->crtcs = (RRCrtc *)(oi + 1);
	oi->nmode = (int)nmode;
	oi->npreferred = (int)g16(hdr + 30);
	oi->modes = oi->crtcs + ncrtc;
	oi->nclone = (int)nclone;
	oi->clones = oi->modes + nmode;
	oi->name = (char *)(oi->clones + nclone);
	oi->nameLen = (int)nlen;
	p = ex + 4;
	for (i = 0; i < ncrtc; i++, p += 4)
		oi->crtcs[i] = g32(p);
	for (i = 0; i < nmode; i++, p += 4)
		oi->modes[i] = g32(p);
	for (i = 0; i < nclone; i++, p += 4)
		oi->clones[i] = g32(p);
	memcpy(oi->name, p, nlen);
	oi->name[nlen] = 0;
	free(ex);
	return oi;
}

void XRRFreeOutputInfo(XRROutputInfo *outputInfo)
{
	free(outputInfo);
}

XRRCrtcInfo *XRRGetCrtcInfo(Display *dpy, XRRScreenResources *resources,
			    RRCrtc crtc)
{
	struct rrreq q;
	unsigned char hdr[32], *ex, *p;
	size_t nex;
	unsigned nout, npos, i;
	XRRCrtcInfo *ci;

	if (!rr_begin(dpy, X_RRGetCrtcInfo, 3, &q))
		return NULL;
	p32(q.r + 4, crtc);
	p32(q.r + 8, resources ? resources->configTimestamp : 0);
	if (!rr_round(&q, hdr, &ex, &nex))
		return NULL;
	if (hdr[1] != RRSetConfigSuccess) {
		free(ex);
		return NULL;
	}
	nout = g16(hdr + 28);
	npos = g16(hdr + 30);
	if (nex < (size_t)(nout + npos) * 4) {
		free(ex);
		return NULL;
	}
	ci = calloc(1, sizeof *ci + (nout + npos) * sizeof(XID));
	if (!ci) {
		free(ex);
		return NULL;
	}
	ci->timestamp = g32(hdr + 8);
	ci->x = (short)g16(hdr + 12);
	ci->y = (short)g16(hdr + 14);
	ci->width = g16(hdr + 16);
	ci->height = g16(hdr + 18);
	ci->mode = g32(hdr + 20);
	ci->rotation = g16(hdr + 24);
	ci->rotations = g16(hdr + 26);
	ci->noutput = (int)nout;
	ci->outputs = (RROutput *)(ci + 1);
	ci->npossible = (int)npos;
	ci->possible = ci->outputs + nout;
	p = ex;
	for (i = 0; i < nout; i++, p += 4)
		ci->outputs[i] = g32(p);
	for (i = 0; i < npos; i++, p += 4)
		ci->possible[i] = g32(p);
	free(ex);
	return ci;
}

void XRRFreeCrtcInfo(XRRCrtcInfo *crtcInfo)
{
	free(crtcInfo);
}

Status XRRSetCrtcConfig(Display *dpy, XRRScreenResources *resources,
			RRCrtc crtc, Time timestamp, int x, int y, RRMode mode,
			Rotation rotation, RROutput *outputs, int noutputs)
{
	struct rrreq q;
	unsigned char hdr[32], *ex;
	size_t nex;
	int i;

	if (noutputs < 0)
		noutputs = 0;
	if (!rr_begin(dpy, X_RRSetCrtcConfig, 7 + noutputs, &q))
		return RRSetConfigFailed;
	p32(q.r + 4, crtc);
	p32(q.r + 8, timestamp);
	p32(q.r + 12, resources ? resources->configTimestamp : 0);
	p16(q.r + 16, (unsigned)x);
	p16(q.r + 18, (unsigned)y);
	p32(q.r + 20, mode);
	p16(q.r + 24, rotation);
	for (i = 0; i < noutputs; i++)
		p32(q.r + 28 + i * 4, outputs[i]);
	if (!rr_round(&q, hdr, &ex, &nex))
		return RRSetConfigFailed;
	free(ex);
	return hdr[1];				/* RRSetConfigSuccess is 0 */
}

RROutput XRRGetOutputPrimary(Display *dpy, Window window)
{
	struct rrreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (!rr_begin(dpy, X_RRGetOutputPrimary, 2, &q))
		return None;
	p32(q.r + 4, window);
	if (!rr_round(&q, hdr, &ex, &nex))
		return None;
	free(ex);
	return g32(hdr + 8);
}

Atom *XRRListOutputProperties(Display *dpy, RROutput output, int *nprop)
{
	struct rrreq q;
	unsigned char hdr[32], *ex;
	size_t nex;
	unsigned n, i;
	Atom *atoms;

	*nprop = 0;
	if (!rr_begin(dpy, X_RRListOutputProperties, 2, &q))
		return NULL;
	p32(q.r + 4, output);
	if (!rr_round(&q, hdr, &ex, &nex))
		return NULL;
	n = g16(hdr + 8);
	if (!n || nex < (size_t)n * 4) {
		free(ex);
		return NULL;
	}
	atoms = malloc(n * sizeof *atoms);
	if (atoms) {
		for (i = 0; i < n; i++)
			atoms[i] = g32(ex + i * 4);
		*nprop = (int)n;
	}
	free(ex);
	return atoms;
}

XRRPropertyInfo *XRRQueryOutputProperty(Display *dpy, RROutput output,
					Atom property)
{
	return NULL;			/* no output has properties here */
}

int XRRGetOutputProperty(Display *dpy, RROutput output, Atom property,
			 long offset, long length, Bool _delete, Bool pending,
			 Atom req_type, Atom *actual_type, int *actual_format,
			 unsigned long *nitems, unsigned long *bytes_after,
			 unsigned char **prop)
{
	struct rrreq q;
	unsigned char hdr[32], *ex;
	size_t nex, nbytes;

	*actual_type = None;
	*actual_format = 0;
	*nitems = 0;
	*bytes_after = 0;
	*prop = NULL;
	if (!rr_begin(dpy, X_RRGetOutputProperty, 7, &q))
		return BadImplementation;
	p32(q.r + 4, output);
	p32(q.r + 8, property);
	p32(q.r + 12, req_type);
	p32(q.r + 16, (unsigned long)offset);
	p32(q.r + 20, (unsigned long)length);
	q.r[24] = _delete ? 1 : 0;
	q.r[25] = pending ? 1 : 0;
	if (!rr_round(&q, hdr, &ex, &nex))
		return BadImplementation;
	*actual_format = hdr[1];
	*actual_type = g32(hdr + 8);
	*bytes_after = g32(hdr + 12);
	*nitems = g32(hdr + 16);
	nbytes = *nitems * (size_t)(*actual_format / 8);
	if (nbytes > nex)
		nbytes = nex;
	/* Xlib pads the property with one extra zero long, as its callers rely on. */
	*prop = calloc(1, nbytes + 4);
	if (*prop && ex)
		memcpy(*prop, ex, nbytes);
	free(ex);
	return Success;
}

/*
 * RANDR 1.0: the screen-configuration API. SDL loads these names and never
 * calls them (its mode code is 1.2+ throughout), so they are honest about
 * having nothing: no configuration, no sizes, no rates.
 */
XRRScreenConfiguration *XRRGetScreenInfo(Display *dpy, Window window)
{
	return NULL;
}

void XRRFreeScreenConfigInfo(XRRScreenConfiguration *config)
{
}

SizeID XRRConfigCurrentConfiguration(XRRScreenConfiguration *config,
				     Rotation *rotation)
{
	if (rotation)
		*rotation = RR_Rotate_0;
	return 0;
}

short XRRConfigCurrentRate(XRRScreenConfiguration *config)
{
	return 0;
}

short *XRRConfigRates(XRRScreenConfiguration *config, int sizeID, int *nrates)
{
	*nrates = 0;
	return NULL;
}

XRRScreenSize *XRRConfigSizes(XRRScreenConfiguration *config, int *nsizes)
{
	*nsizes = 0;
	return NULL;
}

Status XRRSetScreenConfigAndRate(Display *dpy, XRRScreenConfiguration *config,
				 Drawable draw, int size_index,
				 Rotation rotation, short rate, Time timestamp)
{
	return RRSetConfigFailed;
}
