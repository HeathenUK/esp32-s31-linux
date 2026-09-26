/*
 * libXxf86vm.so.1 for xlite: the XFree86-VidMode client a stock X11
 * application links (TyrQuake 0.71's default Linux video target,
 * vid_x11_common.c, does).
 *
 * SDL 1.2 needs no such library - it carries its own copy of the VidMode
 * client compiled into libSDL - and that is the copy xshim's vidmode_request()
 * (lvdesk/xshim.c) has been talking to. The real libXxf86vm is written on
 * Xlib internals xlite does not have (_XReply, XextAddDisplay, the request
 * macros), so this is the protocol subset on xlite's own primitives, in the
 * SAME wire layout xshim already serves SDL: version 2.2, hskew a CARD16 in
 * GetModeLine/SwitchToMode and a CARD32 in the 48-byte GetAllModeLines
 * record, exactly as vm_mode_record() and case 10 there lay it out.
 *
 * Only the calls TyrQuake makes are real: QueryVersion, QueryExtension,
 * GetModeLine, GetAllModeLines, SwitchToMode, SetViewPort, GetViewPort,
 * and (2026-09-26, TyrQuake 0.71 GL's Gamma_Init) GetGammaRampSize,
 * GetGammaRamp and SetGammaRamp, laid out as xf86vmproto.h's
 * xXF86VidMode{Get,Set}GammaRamp{,Size}Req and replies, which xshim serves
 * (vidmode_request cases 17-19).
 * The rest of libXxf86vm's API is not exported; a client that needs it is a
 * client this board has not met.
 */
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/xf86vmode.h>
#include <X11/extensions/xf86vmproto.h>
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

static int vm_major, vm_event, vm_error, vm_probed;
static int vm_opcode(Display *dpy)
{
	if (!vm_probed) {
		vm_probed = 1;
		if (!XQueryExtension(dpy, "XFree86-VidModeExtension", &vm_major,
				     &vm_event, &vm_error))
			vm_major = 0;
	}
	return vm_major;
}

struct vmreq {
	unsigned char *r;
	struct xdpy *x;
	uint32_t seq;
};
static int vm_begin(Display *dpy, int minor, int words, struct vmreq *q)
{
	int major = vm_opcode(dpy);

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
static int vm_round(struct vmreq *q, unsigned char *hdr, unsigned char **extra,
		    size_t *nextra)
{
	*extra = NULL;
	*nextra = 0;
	xlite_send(q->x, q->r);
	return xlite_reply(q->x, q->seq, hdr, extra, nextra);
}

Bool XF86VidModeQueryExtension(Display *dpy, int *event_base, int *error_base)
{
	if (!vm_opcode(dpy))
		return False;
	if (event_base) *event_base = vm_event;
	if (error_base) *error_base = vm_error;
	return True;
}

Bool XF86VidModeQueryVersion(Display *dpy, int *major, int *minor)
{
	struct vmreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (!vm_begin(dpy, X_XF86VidModeQueryVersion, 1, &q))
		return False;
	if (!vm_round(&q, hdr, &ex, &nex))
		return False;
	free(ex);
	if (major) *major = (int)g16(hdr + 8);
	if (minor) *minor = (int)g16(hdr + 10);
	return True;
}

/* The 48-byte record xshim's vm_mode_record() writes. */
static void unpack_info(const unsigned char *m, XF86VidModeModeInfo *mi)
{
	memset(mi, 0, sizeof *mi);
	mi->dotclock = (unsigned int)g32(m + 0);
	mi->hdisplay = g16(m + 4);
	mi->hsyncstart = g16(m + 6);
	mi->hsyncend = g16(m + 8);
	mi->htotal = g16(m + 10);
	mi->hskew = (unsigned short)g32(m + 12);
	mi->vdisplay = g16(m + 16);
	mi->vsyncstart = g16(m + 18);
	mi->vsyncend = g16(m + 20);
	mi->vtotal = g16(m + 22);
	mi->flags = (unsigned int)g32(m + 28);
	mi->privsize = 0;
	mi->private = NULL;
}

Bool XF86VidModeGetModeLine(Display *dpy, int screen, int *dotclock,
			    XF86VidModeModeLine *modeline)
{
	struct vmreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (!vm_begin(dpy, X_XF86VidModeGetModeLine, 2, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	if (!vm_round(&q, hdr, &ex, &nex))
		return False;
	free(ex);
	/* xshim: dotclock @8, hdisplay..htotal @12, hskew (CARD16) @20,
	 * vdisplay..vtotal @22; flags/privsize in the (zero) extra. */
	if (dotclock)
		*dotclock = (int)g32(hdr + 8);
	if (modeline) {
		memset(modeline, 0, sizeof *modeline);
		modeline->hdisplay = g16(hdr + 12);
		modeline->hsyncstart = g16(hdr + 14);
		modeline->hsyncend = g16(hdr + 16);
		modeline->htotal = g16(hdr + 18);
		modeline->hskew = g16(hdr + 20);
		modeline->vdisplay = g16(hdr + 22);
		modeline->vsyncstart = g16(hdr + 24);
		modeline->vsyncend = g16(hdr + 26);
		modeline->vtotal = g16(hdr + 28);
	}
	return True;
}

Bool XF86VidModeGetAllModeLines(Display *dpy, int screen, int *modecount,
				XF86VidModeModeInfo ***modelinesPtr)
{
	struct vmreq q;
	unsigned char hdr[32], *ex;
	size_t nex, i, n;
	XF86VidModeModeInfo **list, *infos;

	if (!vm_begin(dpy, X_XF86VidModeGetAllModeLines, 2, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	if (!vm_round(&q, hdr, &ex, &nex))
		return False;
	n = g32(hdr + 8);
	if (nex < n * 48) {
		free(ex);
		return False;
	}
	/*
	 * One block: the pointer array, then the records. The client frees
	 * it with a single XFree(modelines), as the real library documents.
	 */
	list = malloc(n * sizeof *list + n * sizeof *infos + 1);
	if (!list) {
		free(ex);
		return False;
	}
	infos = (XF86VidModeModeInfo *)(list + n);
	for (i = 0; i < n; i++) {
		unpack_info(ex + i * 48, &infos[i]);
		list[i] = &infos[i];
	}
	free(ex);
	if (modecount) *modecount = (int)n;
	if (modelinesPtr) *modelinesPtr = list;
	return True;
}

Bool XF86VidModeSwitchToMode(Display *dpy, int screen,
			     XF86VidModeModeInfo *modeline)
{
	struct vmreq q;

	/* xshim case 10 reads hdisplay @12 and vdisplay @22: screen (CARD16)
	 * @4, dotclock @8, then the CARD16 timings with a CARD16 hskew. */
	if (!vm_begin(dpy, X_XF86VidModeSwitchToMode, 12, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	p32(q.r + 8, modeline->dotclock);
	p16(q.r + 12, modeline->hdisplay);
	p16(q.r + 14, modeline->hsyncstart);
	p16(q.r + 16, modeline->hsyncend);
	p16(q.r + 18, modeline->htotal);
	p16(q.r + 20, modeline->hskew);
	p16(q.r + 22, modeline->vdisplay);
	p16(q.r + 24, modeline->vsyncstart);
	p16(q.r + 26, modeline->vsyncend);
	p16(q.r + 28, modeline->vtotal);
	p32(q.r + 32, modeline->flags);
	p32(q.r + 44, 0);			/* privsize */
	xlite_send(q.x, q.r);
	return True;
}

Bool XF86VidModeSetViewPort(Display *dpy, int screen, int x, int y)
{
	struct vmreq q;

	if (!vm_begin(dpy, X_XF86VidModeSetViewPort, 4, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	p32(q.r + 8, (unsigned long)x);
	p32(q.r + 12, (unsigned long)y);
	xlite_send(q.x, q.r);
	return True;
}

Bool XF86VidModeGetViewPort(Display *dpy, int screen, int *x, int *y)
{
	struct vmreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (!vm_begin(dpy, X_XF86VidModeGetViewPort, 2, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	if (!vm_round(&q, hdr, &ex, &nex))
		return False;
	free(ex);
	if (x) *x = (int)g32(hdr + 8);
	if (y) *y = (int)g32(hdr + 12);
	return True;
}

Bool XF86VidModeGetGammaRampSize(Display *dpy, int screen, int *size)
{
	struct vmreq q;
	unsigned char hdr[32], *ex;
	size_t nex;

	if (size)
		*size = 0;
	if (!vm_begin(dpy, X_XF86VidModeGetGammaRampSize, 2, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	if (!vm_round(&q, hdr, &ex, &nex))
		return False;
	free(ex);
	if (size)
		*size = (int)g16(hdr + 8);
	return True;
}

Bool XF86VidModeGetGammaRamp(Display *dpy, int screen, int size,
			     unsigned short *red, unsigned short *green,
			     unsigned short *blue)
{
	struct vmreq q;
	unsigned char hdr[32], *ex;
	size_t nex;
	int i, n;

	if (!vm_begin(dpy, X_XF86VidModeGetGammaRamp, 2, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	p16(q.r + 6, (unsigned)size);
	if (!vm_round(&q, hdr, &ex, &nex))
		return False;
	n = (int)g16(hdr + 8);
	if (n && (n != size || nex < (size_t)n * 6)) {
		free(ex);
		return False;
	}
	for (i = 0; i < n; i++) {
		red[i] = (unsigned short)g16(ex + i * 2);
		green[i] = (unsigned short)g16(ex + (n + i) * 2);
		blue[i] = (unsigned short)g16(ex + (2 * n + i) * 2);
	}
	free(ex);
	return True;
}

Bool XF86VidModeSetGammaRamp(Display *dpy, int screen, int size,
			     unsigned short *red, unsigned short *green,
			     unsigned short *blue)
{
	struct vmreq q;
	int i, words = 2 + (size * 6 + 3) / 4;

	if (size < 0 || size > 4096)
		return False;
	if (!vm_begin(dpy, X_XF86VidModeSetGammaRamp, words, &q))
		return False;
	p16(q.r + 4, (unsigned)screen);
	p16(q.r + 6, (unsigned)size);
	for (i = 0; i < size; i++) {
		p16(q.r + 8 + i * 2, red[i]);
		p16(q.r + 8 + (size + i) * 2, green[i]);
		p16(q.r + 8 + (2 * size + i) * 2, blue[i]);
	}
	xlite_send(q.x, q.r);
	return True;
}
