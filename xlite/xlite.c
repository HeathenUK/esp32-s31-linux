/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xlite core: the connection, the id allocator, and the event queue.
 *
 * Everything here is the part of Xlib that has state. The request encoders are
 * mechanical and live in xlite_req.c; what makes a client work or hang is in
 * this file.
 */
#include "xlite.h"
#include "xlite_wirekeys.h"

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ wire */

static void p16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void p32(unsigned char *p, unsigned long v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static unsigned g16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long g32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) |
	       ((unsigned long)p[3] << 24);
}

static int writeall(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w; n -= w;
	}
	return 0;
}

/* --------------------------------------------------------------- requests */

/*
 * Start a request. Returns a zeroed buffer of `words` 4-byte units with the
 * opcode, detail and length already filled in.
 *
 * This now allocates from the SAME output buffer the extension libraries use
 * through _XGetRequest(), which is what keeps their requests and ours in the
 * order they were issued. Two buffers could not.
 */
/*
 * The output lock. xfiles draws thumbnails from a worker thread while its
 * main thread handles events, and both build requests in place in the one
 * output buffer - real Xlib serialises this with LockDisplay held across
 * the whole call, and XInitThreads() here already claims success. Without
 * it, a flush from one thread mid-fill of the other's request sent a torn
 * request, and the thumbnail thread's commitdraw simply vanished: pixels
 * present in the layer pixmap, nothing on screen until the next
 * main-thread redraw.
 *
 * Held from xlite_req() (buffer reservation) to xlite_send() (request
 * complete); recursive, because a full buffer flushes from inside
 * xlite_req(). The INPUT side (replies, events) is deliberately unlocked:
 * the reader would otherwise hold the lock while blocked in read() and
 * deadlock every other thread. That is safe for the clients this library
 * serves - only main threads read events - and wrong in general.
 */
static pthread_mutex_t xlite_out_lock;
static pthread_once_t xlite_out_once = PTHREAD_ONCE_INIT;

static void xlite_out_lock_init(void)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&xlite_out_lock, &a);
	pthread_mutexattr_destroy(&a);
}

void xlite_out_acquire(void)
{
	pthread_once(&xlite_out_once, xlite_out_lock_init);
	pthread_mutex_lock(&xlite_out_lock);
}

void xlite_out_release(void)
{
	pthread_mutex_unlock(&xlite_out_lock);
}

unsigned char *xlite_req(struct xdpy *x, int opcode, int detail, int words)
{
	unsigned char *r;

	xlite_out_acquire();
	r = _XGetRequest(&x->pub, (CARD8)opcode, (size_t)words * 4);
	if (!r) {
		xlite_out_release();
		return NULL;
	}
	r[1] = (unsigned char)detail;
	return r;
}

/* The bytes are already in the shared buffer; releasing the lock is the
 * "send". Every REQ() in this library pairs with exactly one call here. */
int xlite_send(struct xdpy *x, const unsigned char *r)
{
	(void)x; (void)r;
	xlite_out_release();
	return 0;
}

/* ---------------------------------------------------------------- events */

/*
 * Decode a 32-byte wire event into an XEvent. Only the fields a client can
 * actually observe are filled; the rest stays zero. An unhandled type still
 * gets its `type` and `serial` so the toolkit can discard it cleanly rather
 * than act on a stale union.
 */
/* XLITE_TRACE_INPUT=1: narrate the event and reply machinery on stderr. */
static int xlite_tr(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XLITE_TRACE_INPUT") != NULL;
	return v;
}

static void decode(struct xdpy *x, const unsigned char *e, XEvent *ev)
{
	int type = e[0] & 0x7F;

	memset(ev, 0, sizeof(*ev));
	ev->type = type;
	ev->xany.serial = g16(e + 2);
	ev->xany.send_event = (e[0] & 0x80) != 0;
	ev->xany.display = &x->pub;

	switch (type) {
	case KeyPress: case KeyRelease:
	case ButtonPress: case ButtonRelease:
	case MotionNotify:
		/*
		 * Offsets are from the START of the 32-byte event, not from
		 * past its header: time 4, root 8, event 12, child 16,
		 * root-x/y 20/22, event-x/y 24/26, state 28. Reading these
		 * four bytes early put the ROOT window where the event window
		 * belongs, so every click was delivered against 0x100 and no
		 * widget ever matched - input looked dead while the server was
		 * sending it correctly all along.
		 */
		ev->xbutton.time = g32(e + 4);
		ev->xbutton.root = g32(e + 8);
		ev->xbutton.window = g32(e + 12);
		if (type == KeyPress || type == KeyRelease) {
			static int tr = -1;	/* XLITE_TRACE_INPUT */

			if (tr < 0)
				tr = getenv("XLITE_TRACE_INPUT") != NULL;
			if (tr)
				fprintf(stderr, "xlite: key %s kc=%u win=0x%lx\n",
					type == KeyPress ? "press" : "release",
					e[1], (unsigned long)g32(e + 12));
		}
		ev->xbutton.subwindow = g32(e + 16);
		ev->xbutton.x_root = (short)g16(e + 20);
		ev->xbutton.y_root = (short)g16(e + 22);
		ev->xbutton.x = (short)g16(e + 24);
		ev->xbutton.y = (short)g16(e + 26);
		ev->xbutton.state = g16(e + 28);
		ev->xbutton.same_screen = e[30];
		/*
		 * The keycode keeps the WIRE value, including the XLW_ low
		 * codes for specials: several Xlib APIs take a KeyCode - an
		 * unsigned char - so a widened 0xFF54 would truncate to 'T'
		 * at the CALLER's boundary and collide with a real letter.
		 * Low codes survive that truncation; the keysym APIs below
		 * (XLookupString, XKeycodeToKeysym, XkbLookupKeySym) widen.
		 */
		ev->xbutton.button = e[1];	/* keycode for key events */
		ev->xany.window = ev->xbutton.window;
		break;
	case EnterNotify: case LeaveNotify:
		ev->xcrossing.time = g32(e + 4);
		ev->xcrossing.root = g32(e + 8);
		ev->xcrossing.window = g32(e + 12);
		ev->xcrossing.subwindow = g32(e + 16);
		ev->xcrossing.x_root = (short)g16(e + 20);
		ev->xcrossing.y_root = (short)g16(e + 22);
		ev->xcrossing.x = (short)g16(e + 24);
		ev->xcrossing.y = (short)g16(e + 26);
		ev->xcrossing.state = g16(e + 28);
		ev->xany.window = ev->xcrossing.window;
		break;
	case Expose:
		ev->xexpose.window = g32(e + 4);
		ev->xexpose.x = g16(e + 8);
		ev->xexpose.y = g16(e + 10);
		ev->xexpose.width = g16(e + 12);
		ev->xexpose.height = g16(e + 14);
		ev->xexpose.count = g16(e + 16);
		ev->xany.window = ev->xexpose.window;
		break;
	case MapNotify: case UnmapNotify:
		ev->xmap.event = g32(e + 4);
		ev->xmap.window = g32(e + 8);
		ev->xany.window = ev->xmap.window;
		break;
	case ConfigureNotify:
		ev->xconfigure.event = g32(e + 4);
		ev->xconfigure.window = g32(e + 8);
		ev->xconfigure.x = (short)g16(e + 16);
		ev->xconfigure.y = (short)g16(e + 18);
		ev->xconfigure.width = g16(e + 20);
		ev->xconfigure.height = g16(e + 22);
		ev->xany.window = ev->xconfigure.window;
		break;
	case ClientMessage:
		ev->xclient.window = g32(e + 4);
		ev->xclient.message_type = g32(e + 8);
		ev->xclient.format = e[1];
		memcpy(ev->xclient.data.b, e + 12, 20);
		ev->xany.window = ev->xclient.window;
		break;
	default:
		ev->xany.window = g32(e + 4);
		break;
	}
}

/* Grow the ring, unrolling it so head is 0. Returns 0 if it cannot. */
static int queue_grow(struct xdpy *x)
{
	int cap = x->qcap ? x->qcap * 2 : XLITE_QSTART;
	XEvent *q;
	int i, n = x->pub.qlen;

	if (cap > 8192)
		return 0;
	q = calloc(cap, sizeof(*q));
	if (!q)
		return 0;
	for (i = 0; i < n; i++)
		q[i] = x->q[(x->qhead + i) % x->qcap];
	free(x->q);
	x->q = q;
	x->qcap = cap;
	x->qhead = 0;
	x->qtail = n;
	xlite_note("event ring grown to %d", cap);
	return 1;
}

void xlite_queue(struct xdpy *x, const unsigned char *e)
{
	int next;

	if (!x->qcap && !queue_grow(x))
		return;
	next = (x->qtail + 1) % x->qcap;
	if (next == x->qhead) {
		if (!queue_grow(x)) {
			fprintf(stderr, "xlite: event ring is full at %d and "
				"cannot grow - dropping a type %d event, which "
				"will show as content that never draws\n",
				x->qcap, e[0] & 0x7F);
			return;
		}
		next = (x->qtail + 1) % x->qcap;
	}
	if ((e[0] & 0x7F) == ConfigureNotify) {
		xlite_note("queued ConfigureNotify win 0x%lx %ux%u",
			   (unsigned long)g32(e + 8), g16(e + 20), g16(e + 22));
		/*
		 * A resize may have reallocated the window's pixels on the
		 * server, which makes any shared mapping we hold for it stale:
		 * we would go on writing into pages nothing composites, and
		 * the window would freeze on its last frame. Drop it here and
		 * the next XPutImage maps the new buffer.
		 *
		 * This is the ONLY notification of a size change we get for a
		 * resize the window manager initiated rather than the client,
		 * so it has to be the hook.
		 */
		xlite_shm_forget((Display *)x, (Drawable)g32(e + 8));
	}
	decode(x, e, &x->q[x->qtail]);
	x->qtail = next;
	x->pub.qlen++;
	if (x->wake[1] >= 0)
		write(x->wake[1], "", 1);
}

static void deliver_error(struct xdpy *x, const unsigned char *e)
{
	XErrorEvent ee;

	memset(&ee, 0, sizeof(ee));
	ee.type = 0;
	ee.display = &x->pub;
	ee.serial = g16(e + 2);
	ee.error_code = e[1];
	ee.resourceid = g32(e + 4);
	ee.minor_code = g16(e + 8);
	ee.request_code = e[10];
	if (x->errh)
		x->errh(&x->pub, &ee);
	else
		fprintf(stderr, "xlite: X error %d on request %d (resource "
			"0x%lx)\n", ee.error_code, ee.request_code,
			(unsigned long)ee.resourceid);
}

/*
 * Read at least one more message. Everything from the server is 32 bytes
 * except a reply, which carries a length; both are handled here so callers
 * never see a partial message.
 */
int xlite_ingrow(struct xdpy *x, size_t need)
{
	size_t cap = x->incap ? x->incap : XLITE_IBUF;
	unsigned char *p;

	if (need <= x->incap)
		return 1;
	while (cap < need)
		cap *= 2;
	p = realloc(x->in, cap);
	if (!p)
		return 0;
	x->in = p;
	x->incap = cap;
	return 1;
}

int xlite_read_more(struct xdpy *x, int block)
{
	struct pollfd pfd = { x->fd, POLLIN, 0 };
	ssize_t n;

	if (!block && poll(&pfd, 1, 0) <= 0)
		return 0;
	if (!xlite_ingrow(x, x->inlen + 512))
		return 0;
	/*
	 * recvmsg, not read, so a descriptor sent with SCM_RIGHTS is not
	 * silently discarded.
	 *
	 * The shared-pixmap path attaches a memfd to an ordinary reply, and on
	 * a STREAM socket that descriptor is delivered with whichever read
	 * consumes those bytes - so if the byte stream is drained with read()
	 * the reply still arrives, intact and correct, and the fd is simply
	 * gone. Every caller then sees a perfectly good reply and no memory,
	 * which is not a failure mode anybody would guess from the symptom.
	 */
	{
		struct msghdr m;
		struct iovec io;
		union {
			struct cmsghdr al;
			char b[CMSG_SPACE(sizeof(int)) * 4];
		} u;
		struct cmsghdr *cm;

		memset(&m, 0, sizeof(m));
		io.iov_base = x->in + x->inlen;
		io.iov_len = x->incap - x->inlen;
		m.msg_iov = &io;
		m.msg_iovlen = 1;
		m.msg_control = u.b;
		m.msg_controllen = sizeof(u.b);
		n = recvmsg(x->fd, &m, 0);
		for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
			if (cm->cmsg_level == SOL_SOCKET &&
			    cm->cmsg_type == SCM_RIGHTS) {
				int got;

				memcpy(&got, CMSG_DATA(cm), sizeof(got));
				if (x->shm_fd >= 0)
					close(x->shm_fd);
				x->shm_fd = got;
			}
	}
	if (n <= 0) {
		if (n == 0 || errno != EINTR) {
			if (x->ioerrh)
				x->ioerrh(&x->pub);
			else
				fprintf(stderr, "xlite: connection to the X "
					"server was lost\n");
			_exit(1);
		}
		return 0;
	}
	x->inlen += n;
	return 1;
}

/*
 * Pull whole messages out of the input buffer. If `want` is non-zero, stop
 * when the reply with that sequence number arrives and hand it back; every
 * event met on the way is queued rather than discarded, which is what makes a
 * round trip safe to do from inside an event loop.
 */
static int pump_ex(struct xdpy *x, uint32_t want, unsigned char *hdr,
		   unsigned char **extra, size_t *nextra, int block)
{
	for (;;) {
		size_t off = 0;

		while (x->inlen - off >= 32) {
			const unsigned char *m = x->in + off;
			size_t need = 32;

			if (m[0] == 1)			/* reply */
				need = 32 + g32(m + 4) * 4;
			if (x->inlen - off < need) {
				if (!xlite_ingrow(x, off + need))
					return 0;
				break;
			}
			if (m[0] == 0) {
				deliver_error(x, m);
			} else if (m[0] == 1) {
				uint32_t seq = g16(m + 2);

				if (want && seq == (want & 0xFFFF)) {
					memcpy(hdr, m, 32);
					*nextra = need - 32;
					if (*nextra) {
						*extra = malloc(*nextra);
						if (*extra)
							memcpy(*extra, m + 32,
							       *nextra);
					} else {
						*extra = NULL;
					}
					off += need;
					memmove(x->in, x->in + off,
						x->inlen - off);
					x->inlen -= off;
					return 1;
				}
				xlite_note("unmatched reply seq %u", seq);
			} else {
				xlite_queue(x, m);
			}
			off += need;
		}
		if (off) {
			memmove(x->in, x->in + off, x->inlen - off);
			x->inlen -= off;
		}
		if (!want && x->qhead != x->qtail)
			return 0;
		if (!block)
			return 0;	/* the caller must not be made to wait */
		xlite_flush(x);		/* never block holding unsent requests */
		if (xlite_tr())
			fprintf(stderr, "xlite: blocking read (want reply seq %u, "
				"qlen %d)\n", want & 0xFFFF, x->pub.qlen);
		if (!xlite_read_more(x, 1))
			return 0;
	}
}

static int pump(struct xdpy *x, uint32_t want, unsigned char *hdr,
		unsigned char **extra, size_t *nextra)
{
	return pump_ex(x, want, hdr, extra, nextra, 1);
}

int xlite_reply(struct xdpy *x, uint32_t seq, unsigned char *hdr,
		unsigned char **extra, size_t *nextra)
{
	int r;

	/*
	 * Reply reads hold the display lock: without it, a worker thread's
	 * reply is consumed by the main thread's event wait, logged as
	 * "unmatched reply", and the worker hangs forever - which is how
	 * xfiles' thumbnail thread died on its first AllocColor. Events
	 * drained here on the way to the reply are queued, and the event
	 * wait picks them up from the queue under the same lock.
	 */
	xlite_out_acquire();
	if (xlite_flush(x) < 0) {	/* it cannot answer what we still hold */
		xlite_out_release();
		return 0;
	}
	r = pump(x, seq, hdr, extra, nextra);
	xlite_out_release();
	return r;
}

/* ------------------------------------------------------------- connection */

static XID alloc_id(Display *d)
{
	struct xdpy *x = XD(d);

	return x->id_base | ((x->next_id++) & x->id_mask);
}

/* Same allocator, reachable from xlite_req.c - MIT-SHM needs an XID for each
 * segment it attaches, and the shm code lives over there with the rest of the
 * shared-memory machinery. */
XID xlite_alloc_id(Display *d)
{
	return alloc_id(d);
}

XLITE_IMPL(XOpenDisplay)
Display *XOpenDisplay(const char *name)
{
	struct sockaddr_un a;
	struct xdpy *x;
	unsigned char hdr[8], *body, *p;
	unsigned vlen, nfmt, len;
	int i;

	if (!name)
		name = getenv("DISPLAY");
	if (!name || !*name)
		name = ":0";

	x = calloc(1, sizeof(*x));
	if (!x)
		return NULL;
	snprintf(x->name, sizeof(x->name), "%s", name);

	/*
	 * Unix domain only. A TCP display would need the whole host-parsing
	 * and authorisation path, and nothing on this board has a use for it -
	 * the server is in the same process tree.
	 */
	x->shm_fd = -1;			/* 0 is a real descriptor */
	/*
	 * The doorbell: when a worker thread's locked reply-read drains the
	 * socket, the main thread's events land in the QUEUE and the socket
	 * goes quiet - so an event wait polling only the socket sleeps its
	 * full timeout with input sitting queued. xfiles' thumbnail thread
	 * did exactly this and every click and keystroke lagged up to
	 * 100 ms while thumbnails generated. xlite_queue() writes a byte
	 * here; the event wait polls both descriptors.
	 */
	if (pipe(x->wake) < 0) {
		x->wake[0] = x->wake[1] = -1;
	} else {
		int i;

		/*
		 * Nonblocking both ends - a full pipe must not stall
		 * xlite_queue() and the drain reads must not stall the
		 * wait - and cloexec, because this client fork+execs.
		 */
		for (i = 0; i < 2; i++) {
			fcntl(x->wake[i], F_SETFL,
			      fcntl(x->wake[i], F_GETFL, 0) | O_NONBLOCK);
			fcntl(x->wake[i], F_SETFD, FD_CLOEXEC);
		}
	}
	x->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (x->fd < 0) {
		free(x);
		return NULL;
	}
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	{
		const char *colon = strrchr(name, ':');
		int dnum = colon ? atoi(colon + 1) : 0;

		snprintf(a.sun_path, sizeof(a.sun_path),
			 "/tmp/.X11-unix/X%d", dnum);
	}
	if (connect(x->fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		xlite_note("cannot connect to %s: %s", a.sun_path,
			   strerror(errno));
		close(x->fd);
		free(x);
		return NULL;
	}

	{	/* Setup request. No authorisation - see above. */
		unsigned char s[12];

		memset(s, 0, sizeof(s));
		s[0] = 'l';
		p16(s + 2, 11); p16(s + 4, 0);
		p16(s + 6, 0); p16(s + 8, 0);
		if (writeall(x->fd, s, sizeof(s)) < 0)
			goto fail;
	}

	{	/* Setup reply: 8 bytes, then `length` 4-byte units. */
		size_t got = 0;

		while (got < 8) {
			ssize_t n = read(x->fd, hdr + got, 8 - got);

			if (n <= 0)
				goto fail;
			got += n;
		}
		if (hdr[0] != 1) {
			fprintf(stderr, "xlite: the X server refused the "
				"connection (code %d)\n", hdr[0]);
			goto fail;
		}
		len = g16(hdr + 6) * 4;
		body = malloc(len);
		if (!body)
			goto fail;
		got = 0;
		while (got < len) {
			ssize_t n = read(x->fd, body + got, len - got);

			if (n <= 0) {
				free(body);
				goto fail;
			}
			got += n;
		}
	}

	p = body;
	x->pub.release = g32(p);
	x->id_base = g32(p + 4);
	x->id_mask = g32(p + 8);
	x->pub.motion_buffer = g32(p + 12);
	vlen = g16(p + 16);
	x->pub.max_request_size = g16(p + 18);
	x->pub.nscreens = p[20];
	nfmt = p[21];
	x->pub.byte_order = p[22];
	x->pub.bitmap_bit_order = p[23];
	x->pub.bitmap_unit = p[24];
	x->pub.bitmap_pad = p[25];
	x->pub.min_keycode = p[26];
	x->pub.max_keycode = p[27];
	p += 32;

	x->pub.vendor = malloc(vlen + 1);
	if (x->pub.vendor) {
		memcpy(x->pub.vendor, p, vlen);
		x->pub.vendor[vlen] = 0;
	}
	p += (vlen + 3) & ~3u;

	for (i = 0; i < (int)nfmt && i < 3; i++) {
		x->formats[i].depth = p[i * 8];
		x->formats[i].bits_per_pixel = p[i * 8 + 1];
		x->formats[i].scanline_pad = p[i * 8 + 2];
	}
	x->pub.nformats = nfmt < 3 ? nfmt : 3;
	x->pub.pixmap_format = x->formats;
	p += nfmt * 8;

	/*
	 * One screen, one depth, one visual - which is all this server has and
	 * all a client on a single panel can use. Parsing the full nested list
	 * would be dead code.
	 */
	x->screen.root = g32(p);
	x->screen.cmap = g32(p + 4);
	x->screen.white_pixel = g32(p + 8);
	x->screen.black_pixel = g32(p + 12);
	x->screen.root_input_mask = g32(p + 16);
	x->screen.width = g16(p + 20);
	x->screen.height = g16(p + 22);
	x->screen.mwidth = g16(p + 24);
	x->screen.mheight = g16(p + 26);
	x->screen.min_maps = g16(p + 28);
	x->screen.max_maps = g16(p + 30);
	x->screen.backing_store = p[36];
	x->screen.save_unders = p[37];
	x->screen.root_depth = p[38];
	x->screen.ndepths = 1;
	x->screen.display = &x->pub;
	x->screen.depths = &x->depth;

	x->visual.visualid = g32(p + 32);
	x->depth.depth = x->screen.root_depth;
	x->depth.nvisuals = 1;
	x->depth.visuals = &x->visual;
	{
		const unsigned char *v = p + 40 + 8;	/* first VISUALTYPE */

		x->visual.class = v[4];
		x->visual.bits_per_rgb = v[5];
		x->visual.map_entries = g16(v + 6);
		x->visual.red_mask = g32(v + 8);
		x->visual.green_mask = g32(v + 12);
		x->visual.blue_mask = g32(v + 16);
		if (!x->visual.red_mask) {	/* trust the header, not luck */
			x->visual.class = TrueColor;
			x->visual.bits_per_rgb = 8;
			x->visual.red_mask = 0xF800;
			x->visual.green_mask = 0x07E0;
			x->visual.blue_mask = 0x001F;
		}
	}
	x->screen.root_visual = &x->visual;
	free(body);

	x->pub.fd = x->fd;
	x->pub.proto_major_version = 11;
	x->pub.proto_minor_version = 0;
	x->pub.screens = &x->screen;
	x->pub.default_screen = 0;
	x->pub.nscreens = 1;
	x->pub.resource_alloc = alloc_id;
	x->pub.display_name = x->name;
	x->next_id = 1;

	/*
	 * The output buffer, and the private fields Xlibint.h's macros reach
	 * into directly: lock_fns NULL means LockDisplay() is a no-op, and
	 * synchandler NULL means SyncHandle() does nothing.
	 */
	x->outcap = 4096;
	x->out = malloc(x->outcap);
	if (!x->out)
		goto fail;
	x->pub.buffer = x->out;
	x->pub.bufptr = x->out;
	x->pub.bufmax = x->out + x->outcap;
	x->pub.lock_fns = NULL;
	x->pub.synchandler = NULL;
	x->pub.last_req = x->out;

	xlite_note("connected to %s, root 0x%lx %dx%d depth %d",
		   a.sun_path, (unsigned long)x->screen.root,
		   x->screen.width, x->screen.height, x->screen.root_depth);
	return &x->pub;

fail:
	close(x->fd);
	free(x);
	return NULL;
}

XLITE_IMPL(XCloseDisplay)
int XCloseDisplay(Display *d)
{
	struct xdpy *x = XD(d);

	if (!x)
		return 0;
	close(x->fd);
	free(x->pub.vendor);
	free(x);
	return 0;
}

/* --------------------------------------------------------------- plumbing */

/*
 * Input-method registration.
 *
 * st asks to be told when an input method appears, and if none ever does it
 * carries on with plain key events. Refusing the callback is the CORRECT
 * answer here, not a degraded one: it puts st on exactly the path we want when
 * using it as a control for input faults, with no IM between the server and
 * the application.
 */
XLITE_IMPL(XRegisterIMInstantiateCallback)
Bool XRegisterIMInstantiateCallback(Display *dpy, struct _XrmHashBucketRec *db,
				    char *res_name, char *res_class,
				    XIDProc cb, XPointer data)
{
	(void)dpy; (void)db; (void)res_name; (void)res_class;
	(void)cb; (void)data;
	return False;
}

XLITE_IMPL(XUnregisterIMInstantiateCallback)
Bool XUnregisterIMInstantiateCallback(Display *dpy,
				      struct _XrmHashBucketRec *db,
				      char *res_name, char *res_class,
				      XIDProc cb, XPointer data)
{
	(void)dpy; (void)db; (void)res_name; (void)res_class;
	(void)cb; (void)data;
	return False;
}

XLITE_IMPL(XSetIMValues)
char *XSetIMValues(XIM im, ...)
{
	(void)im;
	return NULL;		/* NULL means "all values set" */
}

XLITE_IMPL(XFlush)
int XFlush(Display *d) { return xlite_flush(XD(d)); }

XLITE_IMPL(XSync)
int XSync(Display *d, Bool discard)
{
	struct xdpy *x = XD(d);
	unsigned char hdr[32], *extra = NULL;
	size_t nextra = 0;
	unsigned char *r = xlite_req(x, 43, 0, 1);	/* GetInputFocus */
	uint32_t seq = x->pub.request;

	xlite_send(x, r);
	if (xlite_reply(x, seq, hdr, &extra, &nextra))
		free(extra);
	if (discard) {
		x->qhead = x->qtail = 0;
		x->pub.qlen = 0;
	}
	return 0;
}

XLITE_IMPL(XEventsQueued)
int XEventsQueued(Display *d, int mode)
{
	struct xdpy *x = XD(d);

	if (x->qhead != x->qtail)
	{
		if (xlite_tr())
			fprintf(stderr, "xlite: XEventsQueued -> %d\n", x->pub.qlen);
		return x->pub.qlen;
	}
	if (mode == QueuedAlready)
		return 0;
	/*
	 * NON-BLOCKING, and that is the whole point of this call.
	 *
	 * pump() blocks when the bytes it consumes are a reply or an error
	 * rather than an event: nothing gets queued, so it loops and waits for
	 * more. Asking it to do that from here wedges XPending(), and a main
	 * loop written as `while (!XPending()) { poll(); fire_timers(); }`
	 * then never reaches its timers - xclock drew the right time once and
	 * never ticked again.
	 */
	xlite_out_acquire();
	while (xlite_read_more(x, 0))
		pump_ex(x, 0, NULL, NULL, NULL, 0);
	xlite_out_release();
	return x->pub.qlen;
}

XLITE_IMPL(XPending)
int XPending(Display *d) { return XEventsQueued(d, QueuedAfterFlush); }

static void dequeue(struct xdpy *x, XEvent *ev)
{
	*ev = x->q[x->qhead];
	x->qhead = (x->qhead + 1) % x->qcap;
	x->pub.qlen--;

	/*
	 * Give the ring back once it drains. An Expose storm at map time grew
	 * it to 512 slots - 48 kB, held for the life of a process that is idle
	 * ~always afterwards. queue_grow() reallocates on demand, so dropping
	 * it here costs one calloc the next time a burst arrives.
	 */
	if (!x->pub.qlen && x->qcap > XLITE_QSTART) {
		free(x->q);
		x->q = NULL;
		x->qcap = x->qhead = x->qtail = 0;
	}
}

/*
 * The event waits cannot block inside the lock - a worker thread would
 * starve until the next input event - and cannot read the socket unlocked,
 * or they eat other threads' replies. So: drain nonblockingly under the
 * lock, and sleep in poll() outside it. The 100 ms cap covers the window
 * where another thread consumed the readable bytes between our poll waking
 * and the lock being taken; events it drained are waiting in the queue.
 */
static int xlite_wait_event(struct xdpy *x, XEvent *ev, int dequeue_it)
{
	for (;;) {
		int got = 0;

		xlite_out_acquire();
		if (x->qhead == x->qtail) {
			xlite_flush(x);
			while (xlite_read_more(x, 0))
				pump_ex(x, 0, NULL, NULL, NULL, 0);
		}
		if (x->qhead != x->qtail) {
			if (dequeue_it)
				dequeue(x, ev);
			else
				*ev = x->q[x->qhead];
			got = 1;
		}
		xlite_out_release();
		if (got) {
			/* Swallow stale doorbell bytes outside the lock. */
			if (x->wake[0] >= 0) {
				char b[16];

				while (read(x->wake[0], b, sizeof(b)) ==
				       (ssize_t)sizeof(b))
					;
			}
			return 0;
		}
		{
			struct pollfd pfd[2] = {
				{ x->fd, POLLIN, 0 },
				{ x->wake[0], POLLIN, 0 },
			};

			if (xlite_tr())
				fprintf(stderr, "xlite: wait_event poll (peek=%d qlen %d)\n",
					!dequeue_it, x->pub.qlen);
			poll(pfd, x->wake[0] >= 0 ? 2 : 1, 1000);
			if (pfd[1].revents & POLLIN) {
				char b[16];

				read(x->wake[0], b, sizeof(b));
			}
		}
	}
}

XLITE_IMPL(XNextEvent)
int XNextEvent(Display *d, XEvent *ev)
{
	return xlite_wait_event(XD(d), ev, 1);
}

XLITE_IMPL(XPeekEvent)
int XPeekEvent(Display *d, XEvent *ev)
{
	return xlite_wait_event(XD(d), ev, 0);
}

/*
 * Which mask bit selects an event of this type. Needed only by XMaskEvent.
 */
static long ev_mask_for(int type)
{
	switch (type) {
	case KeyPress:		return KeyPressMask;
	case KeyRelease:	return KeyReleaseMask;
	case ButtonPress:	return ButtonPressMask;
	case ButtonRelease:	return ButtonReleaseMask;
	case MotionNotify:	return PointerMotionMask | ButtonMotionMask;
	case EnterNotify:	return EnterWindowMask;
	case LeaveNotify:	return LeaveWindowMask;
	case FocusIn: case FocusOut:		return FocusChangeMask;
	case Expose: case GraphicsExpose: case NoExpose:
						return ExposureMask;
	case VisibilityNotify:	return VisibilityChangeMask;
	case PropertyNotify:	return PropertyChangeMask;
	case CreateNotify:	return SubstructureNotifyMask;
	case DestroyNotify: case UnmapNotify: case MapNotify:
	case ReparentNotify: case ConfigureNotify: case GravityNotify:
	case CirculateNotify:
		return StructureNotifyMask | SubstructureNotifyMask;
	default:		return 0;
	}
}

/*
 * Block until an event matching the mask arrives.
 *
 * Not optional, and the generated stub's benign "return 0" is actively fatal
 * here: SDL maps its window with XMapRaised and then calls this to wait for
 * the MapNotify before finishing setup. Answering immediately without filling
 * the event in left SDL spinning in that wait - prboom ran, held 1.6 MB, and
 * never put a window on the screen, with the shim's log filling with
 * XMaskEvent.
 *
 * Non-matching events are consumed rather than left queued. A real server
 * would keep them; doing that needs a queue this library does not have, and
 * the alternative - peeking and refusing to advance - spins forever on the
 * first non-match. The loss is bounded to a masked wait, and the only event
 * SDL can lose that way is an Expose it repaints on the next frame anyway.
 */
/*
 * Take the first queued event that `pred` accepts, leaving the others where
 * they are. XMaskEvent and XCheckTypedEvent used to dequeue and DROP every
 * event they were not looking for: SDL waits in XMaskEvent for the motion a
 * warp generates, and every key typed while it waited vanished - which is
 * why a grabbed game went deaf to its keyboard.
 */
static int queue_take(struct xdpy *x, int (*pred)(const XEvent *, long),
		      long arg, XEvent *out)
{
	int i, idx = x->qhead, n = x->pub.qlen;

	for (i = 0; i < n; i++, idx = (idx + 1) % x->qcap) {
		int j;

		if (!pred(&x->q[idx], arg))
			continue;
		*out = x->q[idx];
		for (j = idx;; j = (j + 1) % x->qcap) {
			int nx = (j + 1) % x->qcap;

			if (nx == x->qtail)
				break;
			x->q[j] = x->q[nx];
		}
		x->qtail = (x->qtail + x->qcap - 1) % x->qcap;
		x->pub.qlen--;
		if (!x->pub.qlen)
			x->qhead = x->qtail = 0;
		return 1;
	}
	return 0;
}

static int pred_mask(const XEvent *e, long mask)
{
	return (ev_mask_for(e->type) & mask) != 0;
}

static int pred_type(const XEvent *e, long type)
{
	return e->type == (int)type;
}

/* Blocking: read until an event `pred` accepts is queued, then take it. */
static int take_wait(struct xdpy *x, int (*pred)(const XEvent *, long),
		     long arg, XEvent *ev)
{
	for (;;) {
		int got;

		xlite_out_acquire();
		got = queue_take(x, pred, arg, ev);
		if (!got) {
			xlite_flush(x);
			while (xlite_read_more(x, 0))
				pump_ex(x, 0, NULL, NULL, NULL, 0);
			got = queue_take(x, pred, arg, ev);
		}
		xlite_out_release();
		if (got)
			return 0;
		{
			struct pollfd pfd[2] = {
				{ x->fd, POLLIN, 0 },
				{ x->wake[0], POLLIN, 0 },
			};

			poll(pfd, x->wake[0] >= 0 ? 2 : 1, 1000);
			if (pfd[1].revents & POLLIN) {
				char b[16];

				read(x->wake[0], b, sizeof(b));
			}
		}
	}
}

/* Non-blocking: whatever has arrived, take a match if there is one. */
static Bool take_check(struct xdpy *x, int (*pred)(const XEvent *, long),
		       long arg, XEvent *ev)
{
	int got;

	xlite_out_acquire();
	got = queue_take(x, pred, arg, ev);
	if (!got) {
		xlite_flush(x);
		while (xlite_read_more(x, 0))
			pump_ex(x, 0, NULL, NULL, NULL, 0);
		got = queue_take(x, pred, arg, ev);
	}
	xlite_out_release();
	return got ? True : False;
}

XLITE_IMPL(XMaskEvent)
int XMaskEvent(Display *d, long mask, XEvent *ev)
{
	return take_wait(XD(d), pred_mask, mask, ev);
}

XLITE_IMPL(XCheckMaskEvent)
Bool XCheckMaskEvent(Display *d, long mask, XEvent *ev)
{
	return take_check(XD(d), pred_mask, mask, ev);
}

/* Non-blocking, and non-destructive when it does not match. */
XLITE_IMPL(XCheckTypedEvent)
Bool XCheckTypedEvent(Display *d, int type, XEvent *ev)
{
	return take_check(XD(d), pred_type, type, ev);
}

XLITE_IMPL(XSetErrorHandler)
int (*XSetErrorHandler(int (*h)(Display *, XErrorEvent *)))(Display *,
							    XErrorEvent *)
{
	static int (*cur)(Display *, XErrorEvent *);
	int (*old)(Display *, XErrorEvent *) = cur;

	cur = h;
	return old;
}

XLITE_IMPL(XSetIOErrorHandler)
int (*XSetIOErrorHandler(int (*h)(Display *)))(Display *)
{
	static int (*cur)(Display *);
	int (*old)(Display *) = cur;

	cur = h;
	return old;
}

XLITE_IMPL(XFree)
int XFree(void *p) { free(p); return 1; }

XLITE_IMPL(XDisplayName)
char *XDisplayName(const char *s)
{
	static char buf[64];

	if (s && *s)
		return (char *)s;
	s = getenv("DISPLAY");
	snprintf(buf, sizeof(buf), "%s", s ? s : ":0");
	return buf;
}

