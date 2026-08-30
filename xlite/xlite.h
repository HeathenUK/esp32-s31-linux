/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XLITE_H
#define XLITE_H

/*
 * xlite - a small libX11 for this board.
 *
 * Not a fork of Xlib and not a patch to it: a separate implementation of the
 * same ABI, so off-the-shelf clients and the off-the-shelf Xt/Xaw toolkit link
 * against it unchanged. Nothing above it is rebuilt - the toolkit references
 * these symbols by name through the dynamic linker, so putting our
 * libX11.so.6 first on the library path is the whole integration.
 *
 * Why it exists, measured on the board: xcalc's 2,104 kB of resident memory is
 * 1,680 kB of library text paged off the SD card, and libX11 alone is 636 kB
 * of that. The whole xcalc chain uses 272 of libX11's 1,177 functions.
 * Replacing it also removes libxcb, libXau and libXdmcp, whose only job is a
 * transport we do ourselves - about 1,550 kB of shared objects for perhaps 80
 * kB of ours, which is what brings the chain inside the flash we can reclaim
 * and therefore into XIP at zero RSS.
 *
 * XLIB_ILLEGAL_ACCESS only NAMES the Display struct; the layout is identical
 * either way, and it is public in Xlib.h precisely so the toolkit's macros can
 * reach ->fd, ->request, ->screens and the rest. The private1..private18
 * placeholders in that layout are ours to use.
 */

/*
 * The REAL struct _XDisplay, not Xlib.h's public prefix.
 *
 * Xlib.h publishes only the front of the record, with private1..private18
 * placeholders, and that sufficed while the only consumers were the toolkit
 * and the application - their macros stay inside the public part.
 *
 * It is not enough for the extension libraries. libXrender, libXft and
 * libXcursor are compiled against Xlibint.h: LockDisplay() dereferences
 * ->lock_fns, Data() writes through ->bufptr and SyncHandle() calls
 * ->synchandler, all in the region Xlib.h describes only as "private to Xlib".
 * Those offsets are baked into those libraries.
 *
 * The header is in the sysroot, so the layout is not unknowable - only
 * unstable across libX11 releases, which does not matter when the whole
 * userspace is built together. Using it makes every offset correct by
 * construction and reduces the internals to seventeen functions.
 */
#include <X11/Xlibint.h>
/*
 * The REGION layout, because libXrender reads it directly. Not for the
 * arithmetic - ours is a bounding box - but for the field offsets.
 */
#include <X11/Xregion.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Marks a function as implemented by hand, so tools/mkxlitestubs.py knows not
 * to generate a stub for it. Grepped, never compiled to anything.
 */
#define XLITE_IMPL(name)	/* implemented: name */

#define XLITE_QSTART	64		/* initial event ring, grows on demand */
/*
 * The input buffer starts small and grows to whatever reply actually arrives.
 * A fixed 16 kB was 16 kB of resident memory in every client for the sake of
 * the one reply in a thousand that is large, and it still could not hold a
 * ListFonts answer bigger than that.
 */
#define XLITE_IBUF	2048

struct xdpy {
	struct _XDisplay pub;		/* MUST be first: clients cast to it */

	int fd;
	/*
	 * The sequence number is pub.request, not a field of our own: the
	 * extension libraries build requests through _XGetRequest(), which
	 * only knows about pub.request, and two counters would drift apart the
	 * first time libXrender sent anything.
	 */
	XID next_id, id_base, id_mask;

	unsigned char *in;		/* bytes read but not yet consumed */
	size_t inlen, incap;
	char *out;			/* request buffer; pub.buffer points here */
	size_t outcap;

	/*
	 * A GROWABLE event ring. It was a fixed 256 and silently dropped when
	 * full, which cost xcalc 39 Expose events during startup - every
	 * widget past the sixteenth painted its background and then never got
	 * told to draw its label. Losing an Expose is not a glitch: it is the
	 * only message that will ever ask for that content.
	 *
	 * Growing also uses LESS memory in the common case, because it starts
	 * at 64 rather than reserving 256.
	 */
	XEvent *q;
	int qcap, qhead, qtail;

	int (*errh)(Display *, XErrorEvent *);
	int (*ioerrh)(Display *);

	char name[64];
	Screen screen;
	Visual visual;
	Depth depth;
	ScreenFormat formats[3];
};

static inline struct xdpy *XD(Display *d) { return (struct xdpy *)d; }

/* Diagnostics: see xlite_diag.c. */
void xlite_missing(int idx, const char *name);

/* Ensure the input buffer can hold `need` bytes. Returns 0 if it cannot. */
int xlite_ingrow(struct xdpy *x, size_t need);
void xlite_note(const char *fmt, ...);
int xlite_tracing(void);
extern const int xlite_nstubs;

/* Core, in xlite.c. */
int xlite_flush(struct xdpy *x);
unsigned char *xlite_req(struct xdpy *x, int opcode, int detail, int words);
int xlite_reply(struct xdpy *x, uint32_t seq, unsigned char *hdr,
		unsigned char **extra, size_t *nextra);
void xlite_queue(struct xdpy *x, const unsigned char *ev);
int xlite_read_more(struct xdpy *x, int block);
int xlite_send(struct xdpy *x, const unsigned char *r);

#endif /* XLITE_H */
