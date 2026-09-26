/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Xlib's internal entry points, as the extension libraries use them.
 *
 * libXrender, libXft, libXcursor and libXfixes are compiled against
 * Xlibint.h. Its macros build requests through _XGetRequest(), append data
 * through Data()/_XSend(), and wait for answers through _XReply() - so an
 * extension library is not a client of the public API at all, it is a client
 * of Xlib's guts. Seventeen functions and the real struct layout are what it
 * takes to host one.
 *
 * Everything here shares ONE output buffer with the rest of xlite, which is
 * the point: an extension's requests and ours have to reach the server in the
 * order they were made, and two buffers cannot guarantee that.
 *
 * Implemented here, for tools/mkxlitestubs.py which greps rather than parses:
 * XLITE_IMPL(_XGetRequest) XLITE_IMPL(_XSend) XLITE_IMPL(_XFlush)
 * XLITE_IMPL(_XRead) XLITE_IMPL(_XReadPad) XLITE_IMPL(_XEatData)
 * XLITE_IMPL(_XEatDataWords) XLITE_IMPL(_XReply)
 * XLITE_IMPL(_XSetLastRequestRead) XLITE_IMPL(_XVIDtoVisual)
 * XLITE_IMPL(_XAllocScratch) XLITE_IMPL(_XAllocTemp) XLITE_IMPL(_XFreeTemp)
 * XLITE_IMPL(_XGetAsyncReply) XLITE_IMPL(_XDeqAsyncHandler)
 * XLITE_IMPL(_XFlushGCCache) XLITE_IMPL(_XInitImageFuncPtrs)
 * XLITE_IMPL(_XUnknownNativeEvent) XLITE_IMPL(_XData32) XLITE_IMPL(_XRead32)
 */
#include "xlite.h"

#include <errno.h>
#include <poll.h>
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000	/* peer shut down its side (Linux) */
#endif
#include <unistd.h>

/* --------------------------------------------------------------- output */

void xlite_out_acquire(void);
void xlite_out_release(void);

int xlite_flush(struct xdpy *x)
{
	size_t n;
	const char *p;
	int rc = 0;

	xlite_out_acquire();
	n = (size_t)(x->pub.bufptr - x->pub.buffer);
	p = x->pub.buffer;

	if (!n) {
		xlite_out_release();
		return 0;
	}
	while (n && x->ring) {
		/*
		 * XLITE-RING: the bytes go into shared memory and one eventfd
		 * write wakes the server - or none, if its doorbell is still
		 * up from the last flush. A request larger than the free room
		 * goes in pieces; the server never blocks, so it always drains.
		 */
		struct xring_dir *d = &x->ring->c2s;
		uint32_t w = xring_write(d, (uint8_t *)x->ring + XRING_C2S_OFF,
					 XRING_C2S_SIZE, p, (uint32_t)n);

		p += w; n -= w;
		if (n)
			XR_STORE(d->full, 1);
		xlite_ring_bell(x);
		if (n) {
			struct pollfd pf[2] = {
				{ x->efd_in, POLLIN, 0 },
				{ x->fd, POLLRDHUP, 0 },	/* see xlite.c */
			};

			if (xring_used(d) > XRING_C2S_SIZE ||
			    (poll(pf, 2, 5) >= 0 &&
			     (pf[1].revents & (POLLHUP | POLLERR | POLLRDHUP)))) {
				/*
				 * The server closed our socket: an IO error,
				 * and in Xlib an IO error is FATAL - the
				 * handler is called and the process exits.
				 * Returning -1 here instead let xlite_reply()
				 * skip its wait, so a dropped ring client
				 * (prboom, after ignoring WM_DELETE_WINDOW for
				 * 3 s) went on rendering at full speed into a
				 * ring nobody drained, forever (2026-09-24).
				 */
				xlite_ioerrh(&x->pub);
				_exit(1);
			}
			/* The doorbell belongs to the reader; if it is up and
			 * nobody is reading, do not spin on it. */
			if (pf[0].revents & POLLIN)
				usleep(500);
		}
	}
	while (n) {
		ssize_t w = write(x->fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			/*
			 * The socket is blocking, so a failed write is a dead
			 * server. Xlib exits once the IO handler returns and
			 * SDL 1.2's xio_errhandler (SDL_x11video.c:240-249)
			 * NULLs its display expecting exactly that; carrying
			 * on with rc = -1 left the client issuing requests
			 * into a display SDL had already discarded.
			 */
			xlite_ioerrh(&x->pub);
			_exit(1);
		}
		p += w; n -= w;
	}
	if (!rc)
		x->pub.bufptr = x->pub.buffer;
	xlite_out_release();
	return rc;
}

/*
 * Reserve `len` bytes for a request and fill in its header. Grows the buffer
 * rather than truncating: a request that does not fit cannot be split, and
 * silently sending a short one desynchronises the stream for good.
 */
void *_XGetRequest(Display *dpy, CARD8 type, size_t len)
{
	struct xdpy *x = XD(dpy);
	xReq *req;

	if (dpy->bufptr + len > dpy->bufmax)
		xlite_flush(x);
	if (len > x->outcap) {
		size_t cap = len * 2;
		char *nb = realloc(x->out, cap);

		if (!nb)
			return NULL;
		x->out = nb;
		x->outcap = cap;
		dpy->buffer = nb;
		dpy->bufptr = nb;
		dpy->bufmax = nb + cap;
	}
	req = (xReq *)dpy->bufptr;
	memset(req, 0, len);
	req->reqType = type;
	req->length = len / 4;
	dpy->bufptr += len;
	dpy->request++;
	dpy->last_req = (char *)req;
	return req;
}

/* Append raw bytes after the request already being built. */
void _XSend(Display *dpy, _Xconst char *data, long size)
{
	struct xdpy *x = XD(dpy);

	while (size > 0) {
		long room = dpy->bufmax - dpy->bufptr;

		if (room <= 0) {
			xlite_flush(x);
			continue;
		}
		if (room > size)
			room = size;
		memcpy(dpy->bufptr, data, room);
		dpy->bufptr += room;
		data += room;
		size -= room;
	}
}

void _XFlush(Display *dpy) { xlite_flush(XD(dpy)); }

/* ---------------------------------------------------------------- input */

/* Read exactly `size` bytes of reply payload, past anything already buffered. */
int _XRead(Display *dpy, char *data, long size)
{
	long want = size;

	struct xdpy *x = XD(dpy);

	while (size > 0) {
		size_t have = x->inlen;

		if (!have) {
			if (!xlite_read_more(x, 1))
				return -1;
			continue;
		}
		if (have > (size_t)size)
			have = size;
		if (data)
			memcpy(data, x->in, have);
		memmove(x->in, x->in + have, x->inlen - have);
		x->inlen -= have;
		if (data)
			data += have;
		size -= have;
	}
	return (int)want;
}

void _XReadPad(Display *dpy, char *data, long size)
{
	_XRead(dpy, data, size);
	if (size & 3)
		_XEatData(dpy, 4 - (size & 3));
}

void _XEatData(Display *dpy, unsigned long n)
{
	char sink[256];

	while (n) {
		unsigned long take = n > sizeof(sink) ? sizeof(sink) : n;

		_XRead(dpy, sink, take);
		n -= take;
	}
}

void _XEatDataWords(Display *dpy, unsigned long n) { _XEatData(dpy, n * 4); }

/*
 * Wait for the reply to the request just sent. Events met on the way are
 * queued, never discarded - a round trip issued from inside an event loop must
 * not lose the events that arrive during it.
 */
Status _XReply(Display *dpy, xReply *rep, int extra, Bool discard)
{
	struct xdpy *x = XD(dpy);
	unsigned char hdr[32], *ex = NULL;
	size_t nex = 0;

	if (xlite_flush(x) < 0)
		return 0;
	if (!xlite_reply(x, x->pub.request, hdr, &ex, &nex))
		return 0;
	memcpy(rep, hdr, 32);
	if (extra > 0) {
		size_t want = (size_t)extra * 4;

		if (want > nex)
			want = nex;
		if (ex && want)
			memcpy((char *)rep + 32, ex, want);
	}
	if (!discard && extra == 0 && nex) {
		/*
		 * The caller intends to read the payload itself with _XRead,
		 * so put it back at the front of the input buffer.
		 */
		if (xlite_ingrow(x, nex + x->inlen)) {
			memmove(x->in + nex, x->in, x->inlen);
			memcpy(x->in, ex, nex);
			x->inlen += nex;
		}
	}
	free(ex);
	/* last_request_read was advanced by the read itself (xlite_seen). It
	 * used to be set to pub.request here, which claimed too much whenever
	 * another thread had queued a request since this one. */
	return 1;
}

/*
 * The extension libraries' wire-to-event converters call this with the event
 * they are decoding, to learn its full serial. Widen it against the requests
 * sent and move last_request_read, as Xlib does.
 */
unsigned long _XSetLastRequestRead(Display *dpy, xGenericReply *rep)
{
	if (!rep)
		return dpy->last_request_read;
	return xlite_seen(XD(dpy), rep->sequenceNumber);
}

/* ----------------------------------------------------------- odds & ends */

Visual *_XVIDtoVisual(Display *dpy, VisualID id)
{
	Visual *v = DefaultVisual(dpy, 0);

	return (v && v->visualid == id) ? v : v;   /* one visual on this server */
}

/*
 * Scratch space. Real Xlib keeps one buffer on the Display and hands it out
 * when it is big enough; the fields exist in the struct, so use them.
 */
char *_XAllocScratch(Display *dpy, unsigned long size)
{
	if (dpy->scratch_length < size) {
		free(dpy->scratch_buffer);
		dpy->scratch_buffer = malloc(size);
		dpy->scratch_length = dpy->scratch_buffer ? size : 0;
	}
	return dpy->scratch_buffer;
}

char *_XAllocTemp(Display *dpy, unsigned long size)
{
	return _XAllocScratch(dpy, size);
}

void _XFreeTemp(Display *dpy, char *buf, unsigned long size)
{
	(void)dpy; (void)buf; (void)size;	/* owned by the Display */
}

/*
 * Asynchronous handlers let a library overlap a reply with other work. Nothing
 * here does, so the queue stays empty and the handler is simply not called -
 * which is the same outcome as a handler that declines every reply.
 */
char *_XGetAsyncReply(Display *dpy, char *replbuf, xReply *rep, char *buf,
		      int len, int extra, Bool discard)
{
	(void)dpy; (void)replbuf; (void)rep; (void)buf; (void)len;
	(void)extra; (void)discard;
	return NULL;		/* nothing here issues asynchronous replies */
}

void _XDeqAsyncHandler(Display *dpy, _XAsyncHandler *handler)
{
	_XAsyncHandler **pp;

	for (pp = &dpy->async_handlers; *pp; pp = &(*pp)->next)
		if (*pp == handler) {
			*pp = handler->next;
			return;
		}
}

void _XFlushGCCache(Display *dpy, GC gc) { (void)dpy; (void)gc; }

int _XInitImageFuncPtrs(XImage *image) { (void)image; return 1; }

/*
 * An extension library's event-to-wire converter, asked about an event type
 * it does not own, falls back to this. Nothing here converts events back to
 * the wire (XESetEventToWire is a no-op, xlite_ext.c), so "not converted" is
 * the whole answer - and the one Xlib gives. Stock libXi imports it, so
 * without it libXi.so.6 cannot be loaded at all: libglut links libXi, and
 * musl binds every symbol at load, so every GLUT program died before main
 * (GL phase 1).
 */
Status _XUnknownNativeEvent(Display *dpy, XEvent *re, xEvent *event)
{
	(void)dpy; (void)re; (void)event;
	return 0;
}

#ifdef LONG64
/*
 * Data32 / _XRead32 for 64-bit longs. Xlibint.h makes both plain macros over
 * Data/_XRead on ILP32 - the board - and only an LP64 libX11 has them as
 * functions, so they exist here only there. That is what lets the host rig
 * (tools/glref/xlite-load.sh) load the host's libXi against xlite; the RV32
 * library, like real Xlib on ILP32, does not export them.
 *
 * `len` is in wire bytes: 4 per item, each item a long in client memory.
 */
int _XData32(Display *dpy, _Xconst long *data, unsigned len)
{
	CARD32 buf[64];

	while (len >= 4) {
		unsigned n = len > sizeof(buf) ? (unsigned)sizeof(buf) : len & ~3u;
		unsigned i;

		for (i = 0; i < n / 4; i++)
			buf[i] = (CARD32)*data++;
		_XSend(dpy, (const char *)buf, n);
		len -= n;
	}
	return 0;
}

void _XRead32(Display *dpy, long *data, long len)
{
	CARD32 *wire = (CARD32 *)data;
	long i, n = len / 4;

	/* Read the 32-bit items into the front of the caller's longs, then
	 * widen from the end so no item is overwritten before it is moved. */
	_XRead(dpy, (char *)data, len);
	for (i = n - 1; i >= 0; i--)
		data[i] = (long)wire[i];
}
#endif
