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
#define XLIB_ILLEGAL_ACCESS 1

#include <X11/Xlib.h>
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
#define XLITE_IBUF	16384

struct xdpy {
	Display pub;			/* MUST be first: clients cast to it */

	int fd;
	uint32_t seq;			/* sequence of the last request sent */
	XID next_id, id_base, id_mask;

	unsigned char in[XLITE_IBUF];	/* bytes read but not yet consumed */
	size_t inlen;

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
