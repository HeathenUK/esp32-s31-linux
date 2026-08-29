// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enough of the X11 core protocol for off-the-shelf clients to run in lvdesk.
 *
 * Not an X server: there is no root window, no compositing, no server-side
 * screen. Each client window is a buffer sized to that window, drawn into
 * directly, which lvdesk presents as an image. xclock's window is 164x164 =
 * 53 kB, against the 2,456 kB anonymous mapping Xfbdev holds before a single
 * client connects. That difference is the whole reason this exists.
 *
 * The request set is not "X11" - it is what a real xclock was observed to
 * send, captured with tools/xstub.py. See docs/x11-shim.md. Nineteen request
 * types, of which three draw.
 *
 * Build standalone for testing, which is how this was brought up:
 *
 *	gcc -DXSHIM_STANDALONE -o xshim xshim.c
 *	./xshim &            # listens on :0
 *	DISPLAY=:0 xclock
 *
 * It then dumps each window to /tmp/xshim-<id>.ppm, so the drawing can be
 * checked without LVGL in the way. Protocol bugs and integration bugs are
 * different problems and mixing them cost real time elsewhere in this project.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include "xshim.h"

#define MAXCLI		4
#define MAXRES		192
#define MAXATOM		64
#define INBUF		65536

/* Our one visual: TrueColor RGB565, matching the panel. */
#define VISUAL_ID	0x21
#define CMAP_ID		0x20
#define ROOT_ID		0x100
#define RES_BASE	0x00400000
#define RES_MASK	0x001FFFFF

enum { R_FREE = 0, R_WINDOW, R_PIXMAP, R_GC, R_FONT, R_COLORMAP, R_CURSOR };

struct res {
	uint32_t id;
	int type;
	int x, y, w, h;
	int mapped;
	uint32_t parent;
	uint16_t *px;			/* windows and pixmaps only */
	uint32_t fg, bg;		/* GCs only */
	int line_width;
};

struct cli {
	int fd;
	int up;				/* connection setup completed */
	uint32_t seq;
	uint8_t in[INBUF];
	size_t n;
};

static struct res res[MAXRES];
static struct cli cli[MAXCLI];
static char *atom[MAXATOM];
static int natom;
static int lfd = -1;
static void (*win_cb)(uint32_t id, int w, int h);
static void (*draw_cb)(uint32_t id);

/* ------------------------------------------------------------- resources */

static struct res *res_find(uint32_t id)
{
	int i;

	for (i = 0; i < MAXRES; i++)
		if (res[i].type != R_FREE && res[i].id == id)
			return &res[i];
	return NULL;
}

static struct res *res_new(uint32_t id, int type)
{
	int i;

	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_FREE) {
			memset(&res[i], 0, sizeof(res[i]));
			res[i].id = id;
			res[i].type = type;
			return &res[i];
		}
	fprintf(stderr, "xshim: resource table full\n");
	return NULL;
}

static void res_free(uint32_t id)
{
	struct res *r = res_find(id);

	if (!r)
		return;
	free(r->px);
	r->px = NULL;
	r->type = R_FREE;
}

/* --------------------------------------------------------------- drawing */

static void px_set(struct res *d, int x, int y, uint16_t c)
{
	if (!d->px || x < 0 || y < 0 || x >= d->w || y >= d->h)
		return;
	d->px[(size_t)y * d->w + x] = c;
}

static void win_fill(struct res *d, int x, int y, int w, int h)
{
	int i, j;

	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			px_set(d, i, j, (uint16_t)d->bg);
}

static void draw_line(struct res *d, int x0, int y0, int x1, int y1, uint16_t c)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = (dx > dy ? dx : -dy) / 2, e2;

	for (;;) {
		px_set(d, x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = err;
		if (e2 > -dx) { err -= dy; x0 += sx; }
		if (e2 < dy)  { err += dx; y0 += sy; }
	}
}

/*
 * Scanline polygon fill. Even-odd, which is X's default (EvenOddRule); the
 * clock hands are convex so Winding would give the same answer, but the
 * default is what the client asked for.
 */
static void fill_poly(struct res *d, const int16_t *pts, int n, uint16_t c)
{
	int ymin = 1 << 30, ymax = -(1 << 30), y, i, j, k;
	int xs[64];

	if (n < 3)
		return;
	for (i = 0; i < n; i++) {
		int py = pts[i * 2 + 1];

		if (py < ymin) ymin = py;
		if (py > ymax) ymax = py;
	}
	if (ymin < 0) ymin = 0;
	if (ymax >= d->h) ymax = d->h - 1;

	for (y = ymin; y <= ymax; y++) {
		int cnt = 0;

		for (i = 0, j = n - 1; i < n; j = i++) {
			int y0 = pts[j * 2 + 1], y1 = pts[i * 2 + 1];
			int x0 = pts[j * 2], x1 = pts[i * 2];

			if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
				if (cnt < (int)(sizeof(xs) / sizeof(xs[0])))
					xs[cnt++] = x0 + (y - y0) * (x1 - x0) /
						    (y1 - y0);
			}
		}
		for (i = 0; i < cnt - 1; i++)		/* insertion sort */
			for (j = i + 1; j < cnt; j++)
				if (xs[j] < xs[i]) {
					k = xs[i]; xs[i] = xs[j]; xs[j] = k;
				}
		for (i = 0; i + 1 < cnt; i += 2)
			for (j = xs[i]; j <= xs[i + 1]; j++)
				px_set(d, j, y, c);
	}
}

/* ---------------------------------------------------------------- protocol */

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int16_t gets16(const uint8_t *p) { return (int16_t)get16(p); }
static uint32_t get32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * The connection setup reply.
 *
 * Its fixed part after the first eight bytes is EXACTLY 32 bytes. Getting that
 * wrong makes the whole reply the wrong length and Xlib does not report an
 * error - it just gives up, which reads as the client never starting.
 */
static void send_setup(struct cli *c)
{
	uint8_t b[256], *p = b + 8;
	static const char vendor[] = "lvdesk-shim";
	int vlen = sizeof(vendor) - 1, vpad = (4 - (vlen & 3)) & 3;
	uint8_t *body = p;

	put32(p, 1);            p += 4;		/* release number */
	put32(p, RES_BASE);     p += 4;
	put32(p, RES_MASK);     p += 4;
	put32(p, 0);            p += 4;		/* motion buffer size */
	put16(p, vlen);         p += 2;
	put16(p, 65535);        p += 2;		/* max request length */
	*p++ = 1;				/* screens */
	*p++ = 3;				/* pixmap formats */
	*p++ = 0;				/* LSB first */
	*p++ = 0;				/* bitmap bit order */
	*p++ = 32;				/* scanline unit */
	*p++ = 32;				/* scanline pad */
	*p++ = 8;				/* min keycode */
	*p++ = 255;				/* max keycode */
	put32(p, 0);            p += 4;		/* unused */

	memcpy(p, vendor, vlen); p += vlen;
	memset(p, 0, vpad);      p += vpad;

	{					/* pixmap formats */
		static const uint8_t f[3][2] = { {1, 1}, {16, 16}, {24, 32} };
		int i;

		for (i = 0; i < 3; i++) {
			*p++ = f[i][0]; *p++ = f[i][1]; *p++ = 32;
			memset(p, 0, 5); p += 5;
		}
	}

	put32(p, ROOT_ID);      p += 4;		/* SCREEN */
	put32(p, CMAP_ID);      p += 4;
	put32(p, 0xFFFF);       p += 4;		/* white */
	put32(p, 0);            p += 4;		/* black */
	put32(p, 0);            p += 4;		/* input masks */
	put16(p, XSHIM_W);      p += 2;
	put16(p, XSHIM_H);      p += 2;
	put16(p, XSHIM_W / 4);  p += 2;
	put16(p, XSHIM_H / 4);  p += 2;
	put16(p, 1);            p += 2;
	put16(p, 1);            p += 2;
	put32(p, VISUAL_ID);    p += 4;
	*p++ = 0; *p++ = 0; *p++ = 16; *p++ = 2;   /* depths: 16 and 1 */

	*p++ = 16; *p++ = 0; put16(p, 1); p += 2; put32(p, 0); p += 4;
	put32(p, VISUAL_ID);    p += 4;		/* VISUALTYPE */
	*p++ = 4;				/* TrueColor */
	*p++ = 8;				/* bits per rgb */
	put16(p, 0);            p += 2;		/* colormap entries */
	put32(p, 0xF800);       p += 4;
	put32(p, 0x07E0);       p += 4;
	put32(p, 0x001F);       p += 4;
	put32(p, 0);            p += 4;

	*p++ = 1; *p++ = 0; put16(p, 0); p += 2; put32(p, 0); p += 4;

	b[0] = 1; b[1] = 0;
	put16(b + 2, 11); put16(b + 4, 0);
	put16(b + 6, (p - body) / 4);
	write(c->fd, b, p - b);
}

static void send_reply(struct cli *c, uint8_t detail, const uint8_t *d24,
		       const uint8_t *extra, int nextra)
{
	uint8_t h[32];

	h[0] = 1; h[1] = detail;
	put16(h + 2, c->seq);
	put32(h + 4, nextra / 4);
	memcpy(h + 8, d24, 24);
	write(c->fd, h, 32);
	if (nextra)
		write(c->fd, extra, nextra);
}

/* Every X event is exactly 32 bytes. Anything else desynchronises the stream. */
static void send_event(struct cli *c, uint8_t type, const uint8_t *d, int n)
{
	uint8_t e[32];

	memset(e, 0, sizeof(e));
	e[0] = type;
	put16(e + 2, c->seq);
	memcpy(e + 4, d, n > 28 ? 28 : n);
	write(c->fd, e, 32);
}

static void expose_window(struct cli *c, struct res *w)
{
	uint8_t d[28];

	memset(d, 0, sizeof(d));
	put32(d, w->id); put32(d + 4, w->id);
	send_event(c, 19, d, 28);		/* MapNotify */

	memset(d, 0, sizeof(d));
	put32(d, w->id);
	put16(d + 4, 0); put16(d + 6, 0);
	put16(d + 8, w->w); put16(d + 10, w->h);
	send_event(c, 12, d, 28);		/* Expose */
}

static void handle(struct cli *c, const uint8_t *r, int len)
{
	uint8_t op = r[0], detail = r[1];
	uint8_t d24[24];

	memset(d24, 0, sizeof(d24));
	c->seq++;
	if (getenv("XSHIM_TRACE"))
		fprintf(stderr, "  [%3u] op=%-3u len=%d\n", c->seq, op, len);

	switch (op) {
	case 98: {					/* QueryExtension */
		/*
		 * Refuse everything. The client was observed to accept this
		 * for BIG-REQUESTS, XKEYBOARD and XFree86-Bigfont and carry
		 * on; refusing XKB in particular sends Xlib down its
		 * core-keyboard path, so there is no layout machinery here.
		 */
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 16: {					/* InternAtom */
		int n = get16(r + 4);
		int i, id = 0;

		for (i = 0; i < natom; i++)
			if (atom[i] && (int)strlen(atom[i]) == n &&
			    !memcmp(atom[i], r + 8, n)) { id = i + 1; break; }
		if (!id && natom < MAXATOM) {
			atom[natom] = strndup((const char *)r + 8, n);
			id = ++natom;
		}
		put32(d24, id);
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 20:					/* GetProperty: None */
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 43:					/* GetInputFocus */
		put32(d24, ROOT_ID);
		send_reply(c, 1, d24, NULL, 0);
		break;
	case 47: {					/* QueryFont */
		/*
		 * A synthetic fixed 8x8 face over ASCII 32..126. The client
		 * takes the core-font path and needs consistent metrics, but
		 * for an analog clock nothing is ever drawn with it.
		 */
		uint8_t ext[52 - 24 + 95 * 12];
		uint8_t ci[12], *p = ext;
		int i;

		memset(ci, 0, sizeof(ci));
		put16(ci + 2, 8); put16(ci + 4, 8);
		put16(ci + 6, 7); put16(ci + 8, 1);

		memcpy(d24, ci, 12); memset(d24 + 12, 0, 4);
		memcpy(d24 + 16, ci, 8);		/* max-bounds starts here */

		memcpy(p, ci + 8, 4); p += 4;
		memset(p, 0, 4); p += 4;
		put16(p, 32); put16(p + 2, 126); put16(p + 4, 32);
		put16(p + 6, 0); p += 8;
		*p++ = 0; *p++ = 0; *p++ = 0; *p++ = 1;
		put16(p, 7); put16(p + 2, 1); p += 4;
		put32(p, 95); p += 4;
		for (i = 0; i < 95; i++) { memcpy(p, ci, 12); p += 12; }
		send_reply(c, 0, d24, ext, p - ext);
		break;
	}
	case 48:					/* QueryTextExtents */
		put16(d24 + 2, 7); put16(d24 + 4, 1);
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 49:					/* ListFonts: none */
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 14:					/* GetGeometry */
		put32(d24, ROOT_ID);
		put16(d24 + 8, XSHIM_W); put16(d24 + 10, XSHIM_H);
		send_reply(c, 16, d24, NULL, 0);
		break;
	case 15:					/* QueryTree */
		put32(d24, ROOT_ID);
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 84: {					/* AllocColor */
		uint16_t rr = get16(r + 8), gg = get16(r + 10), bb = get16(r + 12);

		put16(d24, rr); put16(d24 + 2, gg); put16(d24 + 4, bb);
		put32(d24 + 8, ((rr >> 11) << 11) | ((gg >> 10) << 5) | (bb >> 11));
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 101: {					/* GetKeyboardMapping */
		uint8_t ext[256 * 4];
		int n = r[5] > 64 ? 64 : r[5];

		memset(ext, 0, sizeof(ext));
		send_reply(c, 1, d24, ext, n * 4);
		break;
	}
	case 119: case 109: {				/* GetModifierMapping */
		uint8_t ext[32];

		memset(ext, 0, sizeof(ext));
		send_reply(c, 2, d24, ext, 32);
		break;
	}

	case 1: {					/* CreateWindow */
		uint32_t id = get32(r + 4), parent = get32(r + 8);
		int w = get16(r + 16), h = get16(r + 18);
		uint32_t mask = get32(r + 28);
		const uint8_t *v = r + 32;
		struct res *rr = res_new(id, R_WINDOW);
		int bit;

		if (!rr)
			break;
		rr->x = gets16(r + 12); rr->y = gets16(r + 14);
		rr->w = w; rr->h = h; rr->parent = parent;
		rr->bg = 0xFFFF;
		/*
		 * CWBackPixel is bit 1. This matters more than it looks: an
		 * X client paints only what it considers foreground and
		 * leaves the background to the server, so a window whose
		 * background is never filled renders as an inverted ghost -
		 * which is exactly how xclock first came up here.
		 */
		for (bit = 0; bit < 15; bit++) {
			if (!(mask & (1u << bit)))
				continue;
			if (bit == 1) rr->bg = get32(v);
			v += 4;
		}
		rr->px = calloc((size_t)w * h, 2);
		win_fill(rr, 0, 0, w, h);
		break;
	}
	case 53: {					/* CreatePixmap */
		uint32_t id = get32(r + 4);
		int w = get16(r + 12), h = get16(r + 14);
		struct res *rr = res_new(id, R_PIXMAP);

		if (rr) {
			rr->w = w; rr->h = h;
			rr->px = calloc((size_t)w * h, 2);
		}
		break;
	}
	case 55: {					/* CreateGC */
		struct res *g = res_new(get32(r + 4), R_GC);

		/*
		 * The protocol's CreateGC defaults, which are foreground 0 and
		 * background 1 - NOT white on black. Getting this backwards
		 * draws xclock's face in white on white: the tick-mark GC sets
		 * only background and font (mask 0x4008) and takes the default
		 * foreground for every line it draws.
		 */
		if (g) { g->fg = 0; g->bg = 1; g->line_width = 1; }
		/* fall through to pick up the value list */
	}
	/* fallthrough */
	case 56: {					/* ChangeGC */
		uint32_t gid = get32(r + 4);
		uint32_t mask = get32(op == 55 ? r + 12 : r + 8);
		const uint8_t *v = (op == 55 ? r + 16 : r + 12);
		struct res *g = res_find(gid);
		int bit;

		if (!g)
			break;
		for (bit = 0; bit < 23; bit++) {
			if (!(mask & (1u << bit)))
				continue;
			if (bit == 2) g->fg = get32(v);		/* foreground */
			if (bit == 3) g->bg = get32(v);		/* background */
			if (bit == 4) g->line_width = get32(v);
			v += 4;
		}
		if (getenv("XSHIM_TRACE"))
			fprintf(stderr, "       gc 0x%x mask=%08x fg=%08x bg=%08x\n",
				gid, mask, g->fg, g->bg);
		break;
	}
	case 60: case 54: case 4:			/* Free GC/Pixmap/Window */
		res_free(get32(r + 4));
		break;

	case 8: case 9: {				/* Map(Sub)Windows */
		int i;

		/*
		 * Expose every window, not just this one: the client draws
		 * into a CHILD widget window, and an Expose sent only to the
		 * top-level leaves it waiting in its event loop - which is
		 * indistinguishable from a hang.
		 */
		for (i = 0; i < MAXRES; i++)
			if (res[i].type == R_WINDOW && !res[i].mapped) {
				res[i].mapped = 1;
				/*
				 * Only top-levels become lvdesk windows; the
				 * children are composited into them.
				 */
				if (win_cb && res[i].parent == ROOT_ID)
					win_cb(res[i].id, res[i].w, res[i].h);
				expose_window(c, &res[i]);
			}
		break;
	}

	case 66: {					/* PolySegment */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 8;

		if (!d || !d->px || !g)
			break;
		for (i = 0; i < n; i++) {
			const uint8_t *p = r + 12 + i * 8;

			draw_line(d, gets16(p), gets16(p + 2),
				  gets16(p + 4), gets16(p + 6), g->fg);
		}
		if (draw_cb) draw_cb(d->id);
		break;
	}
	case 65: {					/* PolyLine */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 4;

		if (!d || !d->px || !g || n < 2)
			break;
		for (i = 0; i + 1 < n; i++) {
			const uint8_t *p = r + 12 + i * 4;

			draw_line(d, gets16(p), gets16(p + 2),
				  gets16(p + 4), gets16(p + 6), g->fg);
		}
		if (draw_cb) draw_cb(d->id);
		break;
	}
	case 69: {					/* FillPoly */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 16) / 4;
		int16_t pts[64 * 2];

		if (!d || !d->px || !g || n < 3)
			break;
		if (n > 64) n = 64;
		for (i = 0; i < n; i++) {
			pts[i * 2] = gets16(r + 16 + i * 4);
			pts[i * 2 + 1] = gets16(r + 16 + i * 4 + 2);
		}
		fill_poly(d, pts, n, g->fg);
		if (draw_cb) draw_cb(d->id);
		break;
	}
	case 70: {					/* PolyFillRectangle */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 8, x, y;

		if (!d || !d->px || !g)
			break;
		for (i = 0; i < n; i++) {
			const uint8_t *p = r + 12 + i * 8;
			int rx = gets16(p), ry = gets16(p + 2);
			int rw = get16(p + 4), rh = get16(p + 6);

			for (y = ry; y < ry + rh; y++)
				for (x = rx; x < rx + rw; x++)
					px_set(d, x, y, g->fg);
		}
		if (draw_cb) draw_cb(d->id);
		break;
	}

	case 61: {					/* ClearArea */
		struct res *d = res_find(get32(r + 4));
		int cx = gets16(r + 8), cy = gets16(r + 10);
		int cw = get16(r + 12), ch = get16(r + 14);

		if (!d)
			break;
		if (!cw) cw = d->w - cx;		/* 0 means "to the edge" */
		if (!ch) ch = d->h - cy;
		win_fill(d, cx, cy, cw, ch);
		if (draw_cb) draw_cb(d->id);
		break;
	}
	case 2: case 12: case 18: case 19: case 22: case 25:
	case 36: case 37: case 42: case 45: case 46:
	case 72: case 78: case 93: case 94: case 95: case 127:
		break;					/* accepted, nothing to do */

	default:
		/*
		 * Loudly. A silent gap in a protocol implementation shows up
		 * as a client that hangs with no clue why, which is the most
		 * expensive kind of bug there is.
		 */
		fprintf(stderr, "xshim: UNHANDLED opcode %u (detail %u, len %d)\n",
			op, detail, len);
		break;
	}
}

/* ------------------------------------------------------------------- API */

int xshim_init(void (*on_window)(uint32_t, int, int),
	       void (*on_draw)(uint32_t))
{
	struct sockaddr_un a;
	int i;

	win_cb = on_window;
	draw_cb = on_draw;
	for (i = 0; i < MAXCLI; i++)
		cli[i].fd = -1;

	unlink(XSHIM_SOCKET);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, XSHIM_SOCKET, sizeof(a.sun_path) - 1);
	if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    listen(lfd, 4) < 0) {
		perror("xshim: bind");
		close(lfd);
		lfd = -1;
		return -1;
	}
	return lfd;
}

/*
 * Blit every mapped child into its parent. Toolkits put the real content in a
 * child widget window - xclock's face is in 0x40000f, not the top-level - so a
 * shim that presents only the top-level presents an empty box.
 */
static void composite(struct res *top)
{
	int i, x, y;

	for (i = 0; i < MAXRES; i++) {
		struct res *c = &res[i];

		if (c->type != R_WINDOW || c->parent != top->id || !c->mapped ||
		    !c->px)
			continue;
		composite(c);
		for (y = 0; y < c->h; y++)
			for (x = 0; x < c->w; x++)
				px_set(top, c->x + x, c->y + y,
				       c->px[(size_t)y * c->w + x]);
	}
}

const uint16_t *xshim_window_pixels(uint32_t id, int *w, int *h)
{
	struct res *r = res_find(id);

	if (!r || r->type != R_WINDOW || !r->px)
		return NULL;
	composite(r);
	*w = r->w;
	*h = r->h;
	return r->px;
}

static void client_data(struct cli *c)
{
	ssize_t n = read(c->fd, c->in + c->n, sizeof(c->in) - c->n);
	size_t off = 0;

	if (n <= 0) {
		close(c->fd);
		c->fd = -1;
		return;
	}
	c->n += n;

	if (!c->up) {
		uint16_t na, nd;
		size_t need;

		if (c->n < 12)
			return;
		na = get16(c->in + 6);
		nd = get16(c->in + 8);
		need = 12 + ((na + 3) & ~3) + ((nd + 3) & ~3);
		if (c->n < need)
			return;
		send_setup(c);
		c->up = 1;
		memmove(c->in, c->in + need, c->n - need);
		c->n -= need;
	}

	while (c->n - off >= 4) {
		const uint8_t *r = c->in + off;
		int len = get16(r + 2) * 4;

		if (len == 0 || c->n - off < (size_t)len)
			break;
		handle(c, r, len);
		off += len;
	}
	if (off) {
		memmove(c->in, c->in + off, c->n - off);
		c->n -= off;
	}
}

void xshim_poll(void)
{
	struct pollfd p[MAXCLI + 1];
	int map[MAXCLI + 1], n = 0, i;

	if (lfd < 0)
		return;
	p[n].fd = lfd; p[n].events = POLLIN; map[n] = -1; n++;
	for (i = 0; i < MAXCLI; i++)
		if (cli[i].fd >= 0) {
			p[n].fd = cli[i].fd; p[n].events = POLLIN;
			map[n] = i; n++;
		}
	if (poll(p, n, 0) <= 0)
		return;
	for (i = 0; i < n; i++) {
		if (!(p[i].revents & (POLLIN | POLLERR | POLLHUP)))
			continue;
		if (map[i] < 0) {
			int fd = accept(lfd, NULL, NULL);
			int j;

			if (fd < 0)
				continue;
			for (j = 0; j < MAXCLI; j++)
				if (cli[j].fd < 0) {
					memset(&cli[j], 0, sizeof(cli[j]));
					cli[j].fd = fd;
					break;
				}
			if (j == MAXCLI)
				close(fd);
		} else {
			client_data(&cli[map[i]]);
		}
	}
}

#ifdef XSHIM_STANDALONE
/*
 * Standalone: prove the protocol and the drawing without LVGL anywhere near
 * it. Dumps each window as a PPM so the result can actually be looked at.
 */
static void dump_window(uint32_t id)
{
	char path[64];
	int w, h, i;
	const uint16_t *px = xshim_window_pixels(id, &w, &h);
	FILE *f;

	if (!px)
		return;
	snprintf(path, sizeof(path), "/tmp/xshim-%x.ppm", id);
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < w * h; i++) {
		uint16_t c = px[i];
		uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 0x1F) << 3),
				   (uint8_t)(((c >> 5) & 0x3F) << 2),
				   (uint8_t)((c & 0x1F) << 3) };

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static void on_window(uint32_t id, int w, int h)
{
	printf("xshim: window 0x%x %dx%d mapped\n", id, w, h);
}

static void on_draw(uint32_t id)
{
	struct res *r = res_find(id);

	while (r && r->parent != ROOT_ID)		/* dump the top-level */
		r = res_find(r->parent);
	if (r)
		dump_window(r->id);
}

int main(void)
{
	int i;

	if (xshim_init(on_window, on_draw) < 0)
		return 1;
	printf("xshim: listening on %s (DISPLAY=:0)\n", XSHIM_SOCKET);
	fflush(stdout);
	for (i = 0; i < 3000; i++) {		/* ~30 s, then dump and exit */
		xshim_poll();
		usleep(10000);
	}
	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_WINDOW && res[i].parent == ROOT_ID)
			dump_window(res[i].id);
	printf("xshim: done\n");
	return 0;
}
#endif
