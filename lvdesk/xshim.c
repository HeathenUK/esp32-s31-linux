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
#define _GNU_SOURCE
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include "kms.h"
#include "xshim.h"
#include "xshim_font.h"

#define MAXCLI		4
#define MAXRES		320
/*
 * Qt interns far more atoms than the Xaw clients this started with: xclock and
 * xcalc between them use a couple of dozen, Qt walks straight past 64 during
 * QXcbConnection setup (_NET_WM_*, _XSETTINGS_*, XInput device properties,
 * EDID, ...). Running out is not fatal by itself - InternAtom returns 0 - but
 * a client then compares against atom 0 forever and behaves as if every
 * property is absent.
 */
#define MAXATOM		256
#define INBUF		65536

/* Our one visual: TrueColor RGB565, matching the panel. */
#define VISUAL_ID	0x21
#define VISUAL8_ID	0x22		/* depth 8, PseudoColor */
#define VISUAL32_ID	0x23		/* depth 32, TrueColor (ARGB8888) */

/*
 * The 256-entry palette of the depth-8 visual, in RGB565.
 *
 * ONE table, not one per client, and that is not a shortcut - it is what a
 * single screen with a single installed colormap means. It also happens to be
 * the only model that works: SDL 1.2 opens TWO connections to the server, a
 * control one and a graphics one, creates its window on the first and sends
 * XStoreColors on the SECOND. A per-client palette therefore stored the
 * colours against a connection that owned no windows, and the window kept
 * rendering through the placeholder ramp - Doom in perfect greyscale, with
 * every colour it set landing in the wrong table.
 */
static uint16_t pal8[256];
static void pal8_init(void)
{
	int i;

	for (i = 0; i < 256; i++)		/* a grey ramp until a client sets one */
		pal8[i] = (uint16_t)(((i >> 3) << 11) | ((i >> 2) << 5) | (i >> 3));
}
#define CMAP_ID		0x20
#define ROOT_ID		0x100
#define RES_BASE	0x00200000
#define RES_MASK	0x001FFFFF

enum { R_FREE = 0, R_WINDOW, R_PIXMAP, R_GC, R_FONT, R_COLORMAP, R_CURSOR };

struct res {
	uint32_t id;
	int type;
	int x, y, w, h;
	int mapped;
	uint32_t parent;
	/*
	 * Only a TOP-LEVEL window or a pixmap owns pixels. A child window is
	 * a clipped view into its top-level's buffer, exactly as in an X
	 * server without backing store - so 69 xcalc windows cost one buffer,
	 * not 69, and a redraw of one button is not a re-composite of the
	 * whole tree.
	 */
	uint16_t *px;			/* owner only */
	/*
	 * RGB565 expansion of an 8-bit (PseudoColor) window.
	 * The desktop presents window pixels zero-copy as RGB565,
	 * so an indexed window needs somewhere to be expanded to.
	 * Allocated on first use, only for depth-8 windows.
	 */
	uint16_t *shadow;

	/*
	 * Coalesced Expose. paint_subtree() walks a window and every mapped
	 * descendant, so a maximise - which reconfigures each widget in turn
	 * - exposed the same widget once from its own configure and again
	 * from every ancestor's: 190 Exposes for 68 ConfigureWindows, and
	 * the client repainted for each one. Accumulate here and emit one
	 * per window at the end of the poll pass instead. X servers are
	 * explicitly allowed to compress Exposes.
	 */
	int exp_pend, exp_cli;
	int ex0, ey0, ex1, ey1;
	struct res *buf;		/* who owns the pixels we draw into */
	/*
	 * Damage accumulated since the desktop last presented this buffer, in
	 * BUFFER coordinates. dmg_valid distinguishes "nothing recorded" from
	 * "rect is (0,0,0,0)": a drawing path that has not been taught to
	 * record its rectangle leaves dmg_valid unset, and the desktop then
	 * repaints the whole window exactly as it always did. Partial damage
	 * is an opt-in per drawing path, so a forgotten path costs speed and
	 * never pixels.
	 */
	uint8_t dmg_valid;
	int dmg_x0, dmg_y0, dmg_x1, dmg_y1;
	/*
	 * One hash per row of this buffer, from the last image put into it.
	 * A client that hands us a whole window every frame (SDL does) tells
	 * us nothing about what actually changed; this recovers it.
	 */
	uint32_t *rowhash;
	int rowhash_h;
	int ax, ay;			/* our origin within that buffer */
	int cx0, cy0, cx1, cy1;		/* clip, in that buffer's coords */
	/*
	 * WM_NORMAL_HINTS, for windows that declared one. A client that sets
	 * min == max is telling the window manager it cannot be resized, and
	 * xcalc does exactly that - so maximising it stretches a frame around
	 * a widget tree that will never fill it.
	 */
	uint16_t min_w, min_h, max_w, max_h;
	uint8_t has_hints;
	uint32_t fg, bg;		/* GCs only */
	int clip_set;			/* GCs only: a clip was installed */
	uint32_t font;			/* GCs only: the font it selects */
	int font_idx;			/* fonts only: index into xfonts[] */
	uint8_t depth;			/* pixmaps: 1 means an alpha bitmap */
	uint8_t dirty;			/* pixmaps: has anything ever drawn here */
	uint8_t hole;			/* every pixel is known to be 0: nothing
					 * has written since allocation or since a
					 * whole-surface clear was punched out */
	uint8_t bpp;			/* bytes per pixel in px: 1, 2 or 4 */
	int shm_fd;			/* memfd or dma-buf backing px, or -1 */
	/*
	 * GEM handle when the index plane lives in the reserved DMA pool.
	 *
	 * The PPA can only address lcd_reserved (see the DT memory-region on
	 * both the lcd and ppa nodes); an ordinary memfd's pages are invisible
	 * to it and esp32s31_ppa_in_range() refuses them. So a window that
	 * wants hardware expansion has to be born in that pool, and is handed
	 * to the client as a dma-buf rather than a memfd.
	 */
	uint32_t gem_src;
	size_t gem_len;
	/*
	 * The pixels belong to a CLIENT's MIT-SHM segment, not to us. Never
	 * free() or munmap() them here - the segment is released when the
	 * client detaches, and shmseg_drop() hands the window a buffer of its
	 * own first.
	 */
	uint8_t px_adopted;
	size_t shm_len;
	int line_width;
	int owner;			/* index into cli[] */
	uint32_t event_mask;		/* what this window asked to receive */
	int bw;				/* border width, drawn in the PARENT */
	uint32_t border_pixel;
	uint32_t bg_pixmap;		/* windows: CWBackPixmap, 0 = none */
	/*
	 * A top-level whose entire content comes from its background pixmap
	 * does not need a buffer of its own: px points straight at the
	 * pixmap's pixels. Holds the pixmap's id while that is true, so the
	 * borrowed storage is never freed here and can be repointed if the
	 * pixmap's own allocation moves.
	 */
	uint32_t alias;
	char title[32];			/* WM_NAME, windows only */
	uint32_t cursor;		/* windows: CWCursor, 0 = inherit */
	uint8_t cur_hidden;		/* cursors: mask all zero, draws nothing */
};

struct cli {
	int fd;
	/*
	 * File descriptors the client has passed us with SCM_RIGHTS, in
	 * arrival order, waiting to be claimed by the request they belong to.
	 *
	 * They have to be queued rather than read on demand because the
	 * ancillary data rides on whatever recvmsg() happened to return - the
	 * kernel delivers the fd with the byte stream, not with the request -
	 * so an fd can and does arrive in the same read as several earlier
	 * requests. MIT-SHM 1.2 defines them as consumed in order.
	 */
	int rfd[8];
	int nrfd;

	int up;				/* connection setup completed */
	uint32_t seq;
	uint8_t in[INBUF];
	size_t n;
	/*
	 * Bytes still to be swallowed from a request too big to buffer.
	 * See the oversized-request path in client_data().
	 */
	size_t skip;
	/*
	 * Replies and events accumulate here and go out in ONE write at the
	 * flush points. Measured with XSHIM_PROF: a single write() on this
	 * board costs ~2.1 ms (the af_unix/skb/wakeup path is kernel text
	 * executing from 80 MHz flash), so a client-startup burst of N
	 * requests used to cost N writes of kernel time. OUTBUF holds the
	 * largest single reply (QueryFont, ~3.2 kB) many times over; anything
	 * bigger flushes first and goes out directly.
	 */
	uint8_t out[16384];
	size_t outn;
	/*
	 * What the socket would not take. Writes are non-blocking now: a
	 * client that has stopped reading - SDL blocked in XSync while ITS
	 * request pipe to us is full - used to block the whole desktop in
	 * write(), and with both sides waiting on the other nothing moved for
	 * 56 s (lvdesk's "input starved" line). Spill here, retry every pass,
	 * and give up on a client that lets it grow past PEND_MAX.
	 */
	uint8_t *pend;
	size_t pendn, pendcap;
	/*
	 * The last few requests, for when something goes wrong. An X client
	 * that fails does so several requests after the one that broke it, so
	 * the opcode we refused is rarely the whole story.
	 */
	uint8_t recent[8];
	int nrecent;
	int nbad;			/* requests answered with an error */
	/*
	 * How many times each request was refused. A widget toolkit redraws,
	 * so one missing primitive is not one log line - xcalc asked for
	 * PolyText8 over two hundred times and buried everything else. Report
	 * each gap once, in full, then count.
	 */
	uint16_t nunimpl[128];
	uint16_t nrender[64];		/* the same, per RENDER minor opcode */
};

static struct res res[MAXRES];
static struct cli cli[MAXCLI];
static char *atom[MAXATOM];

/*
 * Pointer state. Declared up here rather than beside xshim_pointer() because
 * the request handler needs it too and sits earlier in the file.
 */
static uint16_t ptr_btn_state;		/* Button1Mask.. of held buttons */
static int ptr_root_x, ptr_root_y;	/* last pointer position, root coords */

/*
 * Selection ownership. The X server itself only has to remember who claimed
 * which selection and hand that back - the data never passes through it, the
 * two clients transfer it between themselves with ConvertSelection and a
 * property. Qt asks for the owner of CLIPBOARD and PRIMARY during startup and
 * BLOCKS on the reply, so this small table is the difference between a Qt app
 * starting and hanging.
 */
#define MAXSEL		8
static struct { uint32_t sel, owner; } selown[MAXSEL];
static int nselown;
static int natom;
static int cur_owner;			/* client whose request is in flight */
/*
 * What the shim itself is holding. Drawable buffers are the only allocation of
 * any size here, and on a board with ~3 MB free they are worth knowing about
 * to the byte rather than by inference.
 */
static size_t mem_win, mem_pix, mem_glyph;
static unsigned long n_punch;	/* whole-surface clears turned into holes */
static int n_win, n_pix;
static int lfd = -1;
static void (*win_cb)(uint32_t id, int w, int h);
static void (*draw_cb)(uint32_t id);
static void (*close_cb)(uint32_t id);
static void (*title_cb)(uint32_t id);
static void (*warp_cb)(uint32_t top, int x, int y);
static void (*mode_cb)(int w, int h);

/*
 * XFree86-VidMode. The list is what a client may switch to; the panel is the
 * first entry and the current mode until someone switches. A switch is
 * remembered with its client so that the client going away puts the panel
 * back, the way a real server restores the mode when its client exits.
 */
static const struct { uint16_t w, h; } vm_modes[] = {
	{ XSHIM_W, XSHIM_H }, { 640, 480 }, { 640, 400 }, { 640, 384 },
	{ 512, 384 }, { 480, 300 }, { 400, 240 }, { 320, 240 }, { 320, 200 },
};
static int vm_cur;			/* index into vm_modes */
static int vm_cli = -1;			/* who switched away from the panel */

/*
 * Colormaps were never tracked - every AllocColor answered with an RGB565
 * pixel. A client on the depth-32 visual then draws with 16-bit pixel
 * values in 32-bit drawables (xfiles' labels: fg=0xffff). Remember which
 * visual each colormap was made for and answer in that visual's format.
 */
#define MAXCMAP 32
static struct { uint32_t id, visual; } cmaps[MAXCMAP];
static uint32_t cmap_visual(uint32_t cmap)
{
	int i;

	for (i = 0; i < MAXCMAP; i++)
		if (cmaps[i].id == cmap)
			return cmaps[i].visual;
	return 0;
}
static uint32_t pixel_for(uint32_t cmap, unsigned r16, unsigned g16,
			  unsigned b16)
{
	if (cmap_visual(cmap) == VISUAL32_ID)
		return 0xFF000000u | ((r16 >> 8) << 16) | ((g16 >> 8) << 8) |
		       (b16 >> 8);
	return ((r16 >> 11) << 11) | ((g16 >> 10) << 5) | (b16 >> 11);
}

/* One pointer, one keyboard, so at most one grab of each. */
static uint32_t grab_win, grab_confine, grab_cursor;
static int grab_cli = -1, grab_owner_ev;
static uint32_t kgrab_win;
static int kgrab_cli = -1;

/* ------------------------------------------------------------- resources */

/*
 * The top-level ancestor of a drawable. Consumers present top-levels, so every
 * draw notification is reported against one - a client that draws into a child
 * widget window (all of them do) would otherwise report ids lvdesk never saw.
 */
static struct res *top_of(struct res *r);
struct cli;
static void out_flush(struct cli *c);
static void vidmode_request(struct cli *c, const uint8_t *r, int len);
static void out_push(struct cli *c, const void *p, size_t n);
static void notify_draw(struct res *d);
static int trace_on(void);
static void px_release(struct res *r);
static int px_share(struct res *r);

static unsigned long rf_calls, rf_steps;
static unsigned long nreplies, nreqs;
/*
 * XSHIM_NOROWDMG=1 falls back to copying and damaging the whole rectangle,
 * so the two can be compared on one boot without a reflash.
 */
static int rowdmg_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XSHIM_NOROWDMG") == NULL;
	return v;
}

static unsigned long nshmput;
/* Frames whose every row hashed identical - nothing presented at all. */
static unsigned long nrow_skip;	/* MIT-SHM ShmPutImage requests served */

/*
 * A one-entry-per-bucket index over the resource table.
 *
 * res_find() is the hottest thing in the shim: measured across three clients
 * and a few drags, 875,089 calls walking an average of 127 entries each -
 * 111 million iterations of a loop that exists to answer "which slot is this
 * XID". Nothing else in the request path comes close.
 *
 * X IDs are handed out sequentially from a per-client base, so their low bits
 * are close to unique and a direct-mapped bucket hits almost always. The entry
 * is only a HINT: it is validated against the slot's own id before use, so a
 * collision, a freed slot or a reused id costs one failed compare and falls
 * back to the scan. That is why nothing has to invalidate it.
 */
#define RIDX		512
static int16_t ridx[RIDX];		/* slot + 1; 0 means empty */

static struct res *res_find(uint32_t id)
{
	unsigned h = id & (RIDX - 1);
	int i = ridx[h] - 1;

	rf_calls++;
	if (i >= 0 && res[i].type != R_FREE && res[i].id == id) {
		rf_steps++;
		return &res[i];
	}
	for (i = 0; i < MAXRES; i++) {
		rf_steps++;
		if (res[i].type != R_FREE && res[i].id == id) {
			ridx[h] = (int16_t)(i + 1);
			return &res[i];
		}
	}
	return NULL;
}

/*
 * May this CHILD window be handed a writable mapping of its parent's pixels?
 *
 * Only when it is the parent's ONLY child, and the parent is a top level.
 * The restriction is not fussiness - a direct write bypasses the server's
 * clipping entirely, so a child that shares a buffer with siblings could
 * paint over whichever of them happens to overlap it, and nothing in the
 * protocol would report it. One child cannot overlap anybody.
 *
 * This is exactly the shape SDL produces: it creates the window it hands the
 * window manager, then ONE child of it to draw into. Toolkits that build a
 * tree of widget windows - xcalc has dozens - get the socket path, which is
 * correct for them anyway because they send small images rarely.
 *
 * Still not free of hazard, and the remaining one is written down rather than
 * hidden: if the PARENT is resized its buffer is reallocated, and a mapping
 * handed out before that points at pages the server no longer composites. The
 * child's own resize brings a ConfigureNotify, which xlite uses to drop the
 * mapping; a parent resized without the child following would freeze the
 * window's contents. Nothing here does that today.
 */
static int win_share_ok(const struct res *w)
{
	const struct res *par = res_find(w->parent);
	int i, kids = 0;

	if (!par || par->type != R_WINDOW || par->buf != par)
		return 0;		/* parent must own the buffer */
	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_WINDOW && res[i].parent == w->parent)
			kids++;
	return kids == 1;
}


static struct res *res_new(uint32_t id, int type)
{
	int i;

	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_FREE) {
			memset(&res[i], 0, sizeof(res[i]));
			res[i].id = id;
			res[i].type = type;
			res[i].owner = cur_owner;
			res[i].shm_fd = -1;	/* 0 is a real fd */
			return &res[i];
		}
	fprintf(stderr, "xshim: resource table full (%d) - raise MAXRES\n",
		MAXRES);
	return NULL;
}

static void res_free(uint32_t id)
{
	struct res *r = res_find(id);
	int i;

	if (!r)
		return;
	px_release(r);
	r->type = R_FREE;
	/*
	 * Anything that was drawing into those pixels must stop. Children
	 * outlive their parent here only during teardown, but a stale buf
	 * pointer is a use-after-free in the drawing path.
	 */
	for (i = 0; i < MAXRES; i++)
		if (res[i].buf == r)
			res[i].buf = NULL;
}

static struct res *top_of(struct res *r)
{
	int guard = 16;

	while (r && r->type == R_WINDOW && r->parent != ROOT_ID && guard--)
		r = res_find(r->parent);
	return r;
}

static void notify_draw(struct res *d)
{
	struct res *t = top_of(d);

	/*
	 * A window borrowing this pixmap's pixels has just changed too - they
	 * are the same memory - but it is not in this drawable's parent chain,
	 * so nothing else would tell the desktop to repaint it.
	 */
	if (d->type == R_PIXMAP) {
		int i;

		for (i = 0; i < MAXRES; i++)
			if (res[i].type == R_WINDOW && res[i].alias == d->id) {
				/*
				 * The window SHOWS this pixmap's pixels, so a
				 * draw into the pixmap is a draw into the
				 * window - and the window's RGB565 shadow
				 * (xshim_window_pixels) refreshes only on the
				 * window's own dirty flag. Without this, a
				 * depth-32 client that composes into its
				 * background pixmap and clears the window
				 * kept showing the shadow from its FIRST
				 * paint: xfiles' thumbnails landed in the
				 * pixmap, were composited, and never appeared.
				 */
				res[i].dirty = 1;
				if (draw_cb)
					draw_cb(res[i].id);
				break;
			}
	}

	/*
	 * XSHIM_WATCH=<hex id> prints the drawable's non-zero pixel count after
	 * every operation that touches it. notify_draw() runs at the end of
	 * each one, so this is an ordered history of a surface's contents -
	 * which is what it takes to find the operation that wipes something,
	 * as opposed to knowing only that it ended up wiped.
	 */
	{
		static unsigned long watch = ~0UL;
		const char *e;

		if (watch == ~0UL) {
			e = getenv("XSHIM_WATCH");
			watch = e ? strtoul(e, NULL, 16) : 0;
		}
		if (watch && d->id == (uint32_t)watch && d->px) {
			size_t i, nz = 0, tot = (size_t)d->w * d->h;

			for (i = 0; i < tot; i++)
				if (d->bpp == 1 ? ((uint8_t *)d->px)[i] :
				    d->px[i])
					nz++;
			fprintf(stderr, "xshim: WATCH 0x%x after op: %zu/%zu "
				"non-zero\n", d->id, nz, tot);
		}
	}
	if (trace_on())
		fprintf(stderr, "xshim: draw 0x%x (%s) -> top 0x%x\n", d->id,
			d->mapped ? "mapped" : "UNMAPPED", t ? t->id : 0);
	if (draw_cb && t)
		draw_cb(t->id);
}

/* --------------------------------------------------------------- drawing */

static int drawable_ok(struct res *d)
{
	return d && d->buf && d->buf->px;
}

/*
 * Storage for a drawable, at the width its DEPTH actually needs.
 *
 * Everything used to be RGB565 regardless. That is right for the 16- and
 * 24-bit drawables, and exactly twice what an 8-bit one needs - and RENDER
 * clients allocate large A8 surfaces to composite through. With three clients
 * open the census showed two of the four biggest pixmaps were depth 8:
 *
 *     0x600054  512x618  depth 8  618 kB
 *     0x600056  600x434  depth 8  508 kB
 *
 * 1,126 kB held for 563 kB of content, on a machine with one or two megabytes
 * free - which is why lvdesk itself was being swapped out and faulting back in
 * while the pointer moved.
 *
 * An 8-bit drawable stores one byte of intensity per pixel, which is what the
 * RGB565 form was really carrying: writes kept the red channel and the mask
 * path read coverage back out of it. Doing that directly is both half the
 * memory and slightly more accurate, since it no longer quantises to 5 bits.
 */

/*
 * A pixmap that holds ONE value everywhere needs no pixels, only the value.
 *
 * This is not a corner case. With three clients open, the two largest masks
 * xfiles keeps - 512x618 and 600x434, 563 kB between them - contain exactly
 * one run each: they are allocated, filled once, and composited through
 * without ever varying. Storing half a megabyte to remember a single number
 * is what was pushing lvdesk into swap.
 *
 * So a pixmap begins uniform at 0 (which is what calloc promised anyway) and
 * materialises real storage only when something writes a DIFFERENT value.
 * Windows are excluded: lvdesk hands their buffer pointer straight to LVGL,
 * so they must always be backed.
 */
/*
 * XSHIM_PPACLUT=1: expand depth-8 windows on the PPA instead of the CPU.
 *
 * DEFAULT OFF, and measured. Undisturbed timedemos, both arms the same way:
 *
 *     320x200   CPU 29.4 / 29.9 / 29.0 fps    PPA 27.2      PPA 7.5% WORSE
 *     640x400   CPU 12.1 fps                  PPA 12.2      parity
 *
 * It is correct (cluttest, cluttest2 - pixel-exact, no spill) and stable
 * (5,000 expansions across a full 416 s run), and it scales the way the
 * arithmetic says it should: its overhead is per-ROW, the CPU's is per-PIXEL,
 * so 4x the pixels closes a 7.5% deficit to parity.
 *
 * But parity is its ceiling, and the reason is the point: at 640x400 the
 * compositor moves 768 kB a frame - 9.3 MB/s at 12 fps against a 13.6 MB/s
 * PSRAM copy ceiling - so the BUS is the constraint, and the PPA moves exactly
 * the same bytes as the CPU. Offloading the processor cannot help there. No
 * expander wins at high resolution; only moving fewer bytes does, which means
 * rendering small and upscaling on the SRM engine.
 *
 * Kept, off, because it is correct and because the arms must live in one
 * binary to be comparable. Do not enable it expecting frames.
 */
static int ppaclut_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XSHIM_PPACLUT") != NULL ||
		    getenv("XSHIM_GEMONLY") != NULL;
	return v;
}

/*
 * XSHIM_GEMONLY=1: put the index plane in the reserved pool exactly as the
 * hardware path does, but expand it on the CPU anyway.
 *
 * This is not useful, it is a REPRODUCER. Every "the board died at 640x400"
 * event was this combination: the PPA was armed, so the client's surface was
 * GEM-backed, but the expansion declined (the window sat one row off the
 * panel) and fell back to the CPU. So the crash was never the PPA - it ran
 * 5,000 expansions across a full timedemo once the placement was fixed - it
 * was the CPU reading and the client WRITING a write-combine buffer.
 *
 * drm_gem_dma maps these write-combine, and the driver's own notes warn that
 * handing a client a GEM buffer moves DOOM'S OWN drawing surface into
 * uncached memory, at the game's expense. At 320x200 that measured within
 * 0.05%; at 640x400 it is four times the traffic. This isolates it.
 */
int xshim_gemonly(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XSHIM_GEMONLY") != NULL;
	return v;
}

/*
 * Allocate an index plane the PPA can actually reach.
 *
 * A dumb buffer comes from the device's DMA pool, which for this driver is
 * the lcd_reserved shared-dma-pool - the only memory the blend engine can
 * address. Returns 0 and leaves r untouched on any failure, so the caller
 * falls back to a memfd and the CPU path.
 */
static int win8_gem_alloc(struct res *r, int w, int h)
{
	struct drm_mode_create_dumb cs;
	struct drm_mode_map_dumb ms;
	int fd = kms_get_fd();
	void *m;

	if (fd < 0)
		return 0;
	memset(&cs, 0, sizeof cs);
	cs.width = w; cs.height = h; cs.bpp = 8;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cs) < 0)
		return 0;
	memset(&ms, 0, sizeof ms);
	ms.handle = cs.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &ms) < 0)
		goto drop;
	m = mmap(NULL, cs.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 ms.offset);
	if (m == MAP_FAILED)
		goto drop;
	/*
	 * CREATE_DUMB does not promise zeroed pages the way memfd does, and
	 * every drawing path here relies on calloc semantics.
	 */
	memset(m, 0, cs.size);
	r->px = m;
	r->gem_src = cs.handle;
	r->gem_len = cs.size;
	return 1;
drop:
	{
		struct drm_mode_destroy_dumb dd;

		memset(&dd, 0, sizeof dd);
		dd.handle = cs.handle;
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
	}
	return 0;
}

static uint16_t *px_alloc(struct res *r, int w, int h)
{
	size_t n;

	/*
	 * 1, 2 or 4 bytes. Depth 32 (and 24, which clients pad to 32) is an
	 * ARGB8888 surface: the client writes whole pixels and the shim
	 * converts to the panel's RGB565 when it presents, exactly as the
	 * depth-8 path converts through the palette.
	 */
	r->bpp = (r->depth && r->depth <= 8) ? 1 : (r->depth > 16 ? 4 : 2);
	n = (size_t)w * h * r->bpp;

	/*
	 * Big surfaces are born shareable. A client that loads pixels through
	 * xlite-SHM asks for the memfd behind a drawable, and px_share() used
	 * to answer by creating one and COPYING the surface into it: 733 KB
	 * per maximised window, and two such requests measured 292 ms of a
	 * 3.0 s maximise - the single largest item in it. Allocating anything
	 * over 64 KB as a memfd from the start makes that request a sendmsg.
	 * memfd pages are zero-filled on first touch, so the calloc semantics
	 * every drawing path relies on are unchanged; and they are only
	 * committed as they are touched, exactly like calloc's. Small
	 * surfaces keep calloc - an fd per icon-sized pixmap is not worth it.
	 */
	/*
	 * A depth-8 window destined for hardware expansion is born in the
	 * reserved pool. Only windows: a pixmap is never presented, so it
	 * gains nothing and would just consume a scarce 6 MB pool.
	 */
	if (ppaclut_on() && r->bpp == 1 && r->type == R_WINDOW &&
	    win8_gem_alloc(r, w, h)) {
		r->hole = 1;
		mem_win += n; n_win++;
		return r->px;
	}

	if (n >= 65536) {
		int fd = memfd_create("xshim-surface", 0);

		if (fd >= 0 && ftruncate(fd, (off_t)n) == 0) {
			void *m = mmap(NULL, n, PROT_READ | PROT_WRITE,
				       MAP_SHARED, fd, 0);

			if (m != MAP_FAILED) {
				r->px = m;
				r->shm_fd = fd;
				r->shm_len = n;
				r->hole = 1;
				if (r->type == R_PIXMAP) {
					mem_pix += n; n_pix++;
				} else {
					mem_win += n; n_win++;
				}
				return r->px;
			}
		}
		if (fd >= 0)
			close(fd);
		/* fall through to the heap on any failure */
	}
	r->px = calloc((size_t)w * h, r->bpp);
	if (!r->px)
		return NULL;
	r->hole = 1;
	if (r->type == R_PIXMAP) {
		mem_pix += n; n_pix++;
	} else {
		mem_win += n; n_win++;
	}
	return r->px;
}

/*
 * Give an aliased window its own pixels back.
 *
 * Called before anything writes to the window itself, and whenever the pixmap
 * underneath is about to move or disappear. Copying the current contents in
 * makes the break invisible: the window carries on with exactly what it was
 * showing a moment earlier.
 */
static int alias_break_ex(struct res *w, int keep_contents)
{
	struct res *pm;
	uint16_t *own;
	size_t n;

	if (!w->alias)
		return 1;
	pm = res_find(w->alias);
	n = (size_t)w->w * w->h * (w->bpp ? w->bpp : 2);
	own = malloc(n);
	if (!own)
		return 0;
	/*
	 * The copy preserves what the window currently shows - but a caller
	 * about to overwrite the WHOLE drawable does not need it preserved,
	 * and for xfiles' full-window Clear that copy was 539 kB and ~30 ms
	 * per navigation, measured as the largest single request cost left.
	 */
	if (!keep_contents)
		memset(own, 0, n);
	else if (pm && pm->px)
		memcpy(own, pm->px, n);
	else
		memset(own, 0, n);
	w->px = own;
	/* zeroed: all zero; copied: whatever the pixmap was known to be */
	w->hole = keep_contents ? (pm && pm->px && pm->hole) : 1;
	w->alias = 0;
	notify_draw(w);			/* the desktop caches this pointer */
	mem_win += n; n_win++;
	return 1;
}

static int alias_break(struct res *w)
{
	return alias_break_ex(w, 1);
}

/* The pixmap's storage moved or went away: nobody may keep borrowing it. */
static void alias_drop(struct res *pm)
{
	int i;

	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_WINDOW && res[i].alias == pm->id)
			alias_break(&res[i]);
}

static void px_release(struct res *r)
{
	if (r->type == R_PIXMAP)
		alias_drop(r);
	if (r->alias) {			/* borrowed pixels are not ours */
		r->px = NULL;
		r->alias = 0;
		return;
	}
	size_t n = (size_t)r->w * r->h * (r->bpp ? r->bpp : 2);

	if (!r->px)
		return;
	/*
	 * ADOPTED PIXELS ARE NOT OURS - and were never charged to mem_win
	 * when adopted, so crediting them back here would walk the counters
	 * negative one window at a time. shmseg_drop() replaces the pointer
	 * before the segment goes away; freeing it here would be a double
	 * free of somebody else's allocation.
	 */
	if (r->px_adopted) {
		free(r->shadow);
		r->shadow = NULL;
		r->px = NULL;
		r->px_adopted = 0;
		return;
	}
	if (r->type == R_PIXMAP) {
		mem_pix -= n; n_pix--;
	} else {
		mem_win -= n; n_win--;
	}
	free(r->shadow);
	r->shadow = NULL;
	free(r->rowhash);
	r->rowhash = NULL;
	r->rowhash_h = 0;
	/*
	 * GEM FIRST. This test used to be `shm_fd >= 0`, which decides between
	 * munmap and free - and a GEM surface that has not been SHARED yet has
	 * no fd, so it fell through to free() on an mmap'd pointer. That is
	 * heap corruption, and it killed lvdesk with no segfault in dmesg and
	 * nothing in its own log: the desktop simply was not there any more,
	 * and the client reported "connection to the X server was lost".
	 *
	 * The handle also has to be destroyed, not just unmapped. It comes
	 * from a 6 MB reserved pool that also holds the scanout buffer, so
	 * leaking one per window would exhaust it in a handful of launches.
	 */
	if (r->gem_src) {
		int fd = kms_get_fd();

		munmap(r->px, r->gem_len);
		if (r->shm_fd >= 0) {		/* the exported dma-buf */
			close(r->shm_fd);
			r->shm_fd = -1;
			r->shm_len = 0;
		}
		if (fd >= 0) {
			struct drm_mode_destroy_dumb dd;

			memset(&dd, 0, sizeof dd);
			dd.handle = r->gem_src;
			ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		}
		r->gem_src = 0;
		r->gem_len = 0;
		r->px = NULL;
		return;
	}
	if (r->shm_fd >= 0) {
		munmap(r->px, r->shm_len);
		close(r->shm_fd);
		r->shm_fd = -1;
		r->shm_len = 0;
		r->px = NULL;
		return;
	}
	free(r->px);
	r->px = NULL;
}

void xshim_mem_report(void)
{
	int i, nd1 = 0, nempty = 0;

	fprintf(stderr, "xshim: res_find %lu calls, %lu steps (%lu avg); "
		"%lu requests, %lu replies (%lu%% are round trips)\n",
		rf_calls, rf_steps, rf_calls ? rf_steps / rf_calls : 0,
		nreqs, nreplies, nreqs ? nreplies * 100 / nreqs : 0);
	fprintf(stderr, "xshim: %lu MIT-SHM ShmPutImage, %lu identical "
		"(no rows changed)\n", nshmput, nrow_skip);
	size_t d1 = 0, empty = 0;

	fprintf(stderr, "xshim: %d window buffers %zu kB, %d pixmaps %zu kB, "
		"glyphs %zu kB, total %zu kB\n", n_win, mem_win / 1024,
		n_pix, mem_pix / 1024, mem_glyph / 1024,
		(mem_win + mem_pix + mem_glyph) / 1024);
	fprintf(stderr, "xshim: %lu whole-surface clears punched to holes\n",
		n_punch);

	/*
	 * The census that decides what to do about it. Totals say the pixmaps
	 * are the cost; they do not say whether the cure is storing masks at
	 * their real depth, or not backing pixmaps nobody draws into.
	 */
	for (i = 0; i < MAXRES; i++) {
		size_t n;

		if (res[i].type != R_PIXMAP)
			continue;
		n = (size_t)res[i].w * res[i].h * (res[i].bpp ? res[i].bpp : 2);
		if (res[i].depth == 1) {
			nd1++;
			d1 += n;
		}
		if (!res[i].dirty) {
			nempty++;
			empty += n;
		}
		if (n >= 64 * 1024) {
			/*
			 * How much of this is actually distinct? A run-length
			 * count is a cheap upper bound on what compressing a
			 * cold surface would leave, and decides whether that is
			 * worth building at all.
			 */
			size_t k, tot = (size_t)res[i].w * res[i].h, runs = 1;

			if (res[i].px && tot && !res[i].hole) {
				if (res[i].bpp == 1) {
					const uint8_t *q = (uint8_t *)res[i].px;

					for (k = 1; k < tot; k++)
						if (q[k] != q[k - 1])
							runs++;
				} else {
					const uint16_t *q = res[i].px;

					for (k = 1; k < tot; k++)
						if (q[k] != q[k - 1])
							runs++;
				}
			}
			fprintf(stderr, "xshim:   pixmap 0x%x %dx%d depth %u "
				"%zu kB, first=0x%04x, %zu runs = %zu kB RLE "
				"(%zu%%)%s\n",
				res[i].id, res[i].w, res[i].h, res[i].depth,
				n / 1024,
				res[i].px ? (res[i].bpp == 1 ?
					     ((uint8_t *)res[i].px)[0] :
					     res[i].px[0]) : 0,
				runs,
				runs * (res[i].bpp + 2) / 1024,
				runs * (res[i].bpp + 2) * 100 / (n ? n : 1),
				res[i].hole ? " HOLE (not walked)" :
				res[i].dirty ? "" : " NEVER DRAWN");
		}
	}
	fprintf(stderr, "xshim:   depth-1: %d pixmaps %zu kB (would be %zu kB "
		"at 1 byte/px); never drawn: %d pixmaps %zu kB\n",
		nd1, d1 / 1024, d1 / 2 / 1024, nempty, empty / 1024);
}

/*
 * The clip of the GC in the request being served.
 *
 * The drawing primitives take a drawable and a colour, not a GC - a line does
 * not need to know what drew it - so threading a clip through all of them
 * would touch every one. The shim serves exactly one request at a time, so the
 * active clip is set once from the GC in handle() and read here.
 */
static int gcclip_on, gcclip_x0, gcclip_y0, gcclip_x1, gcclip_y1;

static void px_set(struct res *d, int x, int y, uint32_t c)
{
	struct res *b = d->buf;
	int ax, ay;

	if (gcclip_on && (x < gcclip_x0 || y < gcclip_y0 ||
			  x >= gcclip_x1 || y >= gcclip_y1))
		return;
	if (!b)
		return;
	if (b->alias && !alias_break(b))	/* about to write: stop borrowing */
		return;
	if (!b->px)
		return;
	ax = x + d->ax;
	ay = y + d->ay;
	if (ax < d->cx0 || ay < d->cy0 || ax >= d->cx1 || ay >= d->cy1)
		return;
	b->dirty = 1;
	b->hole = 0;
	/*
	 * The colour is a PIXEL VALUE in the drawable's own visual, so its
	 * width follows the surface: an index at depth 8, RGB565 at 16, and
	 * ARGB8888 at 32. Narrowing everything to uint16_t - which this did -
	 * makes a depth-32 client's pixels unrepresentable, so st drew its
	 * whole terminal in colours that could not be stored.
	 */
	if (b->bpp == 1)
		((uint8_t *)b->px)[(size_t)ay * b->w + ax] = (uint8_t)c;
	else if (b->bpp == 4)
		((uint32_t *)b->px)[(size_t)ay * b->w + ax] = c;
	else
		b->px[(size_t)ay * b->w + ax] = (uint16_t)c;
}

/* Read one pixel, with the same clip rules px_set() writes under. */
static uint32_t px_get(struct res *d, int x, int y)
{
	struct res *b = d->buf;
	int ax = x + d->ax, ay = y + d->ay;

	if (!b || !b->px)
		return 0;
	if (ax < d->cx0 || ay < d->cy0 || ax >= d->cx1 || ay >= d->cy1)
		return 0;
	if (b->bpp == 1)
		return ((const uint8_t *)b->px)[(size_t)ay * b->w + ax];
	if (b->bpp == 4)
		return ((const uint32_t *)b->px)[(size_t)ay * b->w + ax];
	return b->px[(size_t)ay * b->w + ax];
}

/*
 * Record that a drawable-local rectangle was drawn, so the desktop can
 * invalidate that area instead of the whole window. The union is kept on the
 * BUFFER OWNER because that is whose pixels changed - a child window's rect
 * lands on its top-level, translated by the child's origin.
 */
static void damage_add(struct res *d, int x, int y, int w, int h)
{
	struct res *b = d->buf;
	int x0, y0, x1, y1;

	if (!b || w <= 0 || h <= 0)
		return;
	x0 = x + d->ax; y0 = y + d->ay;
	x1 = x0 + w;    y1 = y0 + h;
	if (x0 < d->cx0) x0 = d->cx0;
	if (y0 < d->cy0) y0 = d->cy0;
	if (x1 > d->cx1) x1 = d->cx1;
	if (y1 > d->cy1) y1 = d->cy1;
	if (x0 >= x1 || y0 >= y1)
		return;
	if (!b->dmg_valid) {
		b->dmg_x0 = x0; b->dmg_y0 = y0;
		b->dmg_x1 = x1; b->dmg_y1 = y1;
		b->dmg_valid = 1;
		return;
	}
	if (x0 < b->dmg_x0) b->dmg_x0 = x0;
	if (y0 < b->dmg_y0) b->dmg_y0 = y0;
	if (x1 > b->dmg_x1) b->dmg_x1 = x1;
	if (y1 > b->dmg_y1) b->dmg_y1 = y1;
}

/*
 * The row-run fast path. px_set() pays ~6 branches and a call per pixel;
 * measured on a 400x300 fill that is the whole cost. span_clip() does the
 * same tests ONCE for a horizontal run and the callers then move whole rows.
 *
 * Clip semantics are px_set()'s exactly: the GC clip applies in
 * drawable-LOCAL coordinates before the origin translation, the buffer clip
 * after it. Returns the writable run [*bx, *bx + *bw) at buffer row *by, or
 * 0 for an empty run. The caller handles alias_break/dirty once per op.
 */
static int span_clip(struct res *d, int x, int y, int w, int *bx, int *by,
		     int *bw)
{
	int x1 = x + w;

	if (gcclip_on) {
		if (y < gcclip_y0 || y >= gcclip_y1)
			return 0;
		if (x < gcclip_x0) x = gcclip_x0;
		if (x1 > gcclip_x1) x1 = gcclip_x1;
	}
	x += d->ax; x1 += d->ax; y += d->ay;
	if (y < d->cy0 || y >= d->cy1)
		return 0;
	if (x < d->cx0) x = d->cx0;
	if (x1 > d->cx1) x1 = d->cx1;
	if (x >= x1)
		return 0;
	*bx = x; *by = y; *bw = x1 - x;
	return 1;
}

static void px_hspan(struct res *d, int x, int y, int w, uint32_t c);

/*
 * Arcs, for the clients that draw with them (xeyes, oclock). X measures
 * angles in 1/64 degree, counterclockwise from three o'clock, in a
 * coordinate system where y grows DOWN - hence the minus on the sine.
 * float throughout: this machine has a single-precision FPU and double is
 * a library call.
 */
static float arc_norm(float a)
{
	while (a < 0)
		a += 360.0f;
	while (a >= 360.0f)
		a -= 360.0f;
	return a;
}

static int arc_in_sector(float ang, float a1, float sweep)
{
	float rel;

	if (sweep >= 360.0f || sweep <= -360.0f)
		return 1;
	if (sweep < 0) {
		a1 += sweep;
		sweep = -sweep;
	}
	rel = arc_norm(ang - arc_norm(a1));
	return rel <= sweep;
}

static void fill_arc(struct res *d, int ax, int ay, int aw, int ah,
		     int a1_64, int a2_64, uint16_t color)
{
	float cx = ax + aw / 2.0f, cy = ay + ah / 2.0f;
	float rx = aw / 2.0f, ry = ah / 2.0f;
	float a1 = a1_64 / 64.0f, sweep = a2_64 / 64.0f;
	int full = a2_64 >= 360 * 64 || a2_64 <= -360 * 64;
	int y;

	if (aw <= 0 || ah <= 0 || rx < 0.5f || ry < 0.5f)
		return;
	for (y = ay; y < ay + ah; y++) {
		float dy = (y + 0.5f - cy) / ry;
		float half, x0f, x1f;
		int x0, x1, x;

		if (dy < -1.0f || dy > 1.0f)
			continue;
		half = rx * sqrtf(1.0f - dy * dy);
		x0f = cx - half; x1f = cx + half;
		x0 = (int)ceilf(x0f - 0.5f);
		x1 = (int)floorf(x1f - 0.5f);
		if (x1 < x0)
			continue;
		if (full) {
			px_hspan(d, x0, y, x1 - x0 + 1, color);
			continue;
		}
		for (x = x0; x <= x1; x++) {
			float ang = atan2f(-(y + 0.5f - cy) / ry,
					   (x + 0.5f - cx) / rx) *
				    (180.0f / 3.14159265f);

			if (arc_in_sector(arc_norm(ang), a1, sweep))
				px_hspan(d, x, y, 1, color);
		}
	}
	damage_add(d, ax, ay, aw, ah);
}

static void draw_arc(struct res *d, int ax, int ay, int aw, int ah,
		     int a1_64, int a2_64, uint16_t color)
{
	float cx = ax + aw / 2.0f, cy = ay + ah / 2.0f;
	float rx = (aw - 1) / 2.0f, ry = (ah - 1) / 2.0f;
	float a1 = a1_64 * (3.14159265f / (180.0f * 64.0f));
	float sweep = a2_64 * (3.14159265f / (180.0f * 64.0f));
	int n = 2 * (aw + ah), i;

	if (aw <= 0 || ah <= 0)
		return;
	if (n < 8)
		n = 8;
	for (i = 0; i <= n; i++) {
		float t = a1 + sweep * i / n;
		int px = (int)(cx + rx * cosf(t) + 0.5f);
		int py = (int)(cy - ry * sinf(t) + 0.5f);

		px_hspan(d, px, py, 1, color);
	}
	damage_add(d, ax, ay, aw, ah);
}

/* One write-ready check per operation instead of one per pixel.
 * full_cover says the caller will overwrite the entire drawable, so an
 * alias can be broken without copying the contents it replaces. */
static struct res *op_target_ex(struct res *d, int full_cover)
{
	struct res *b = d->buf;

	if (!b)
		return NULL;
	if (b->alias && !alias_break_ex(b, !full_cover))
		return NULL;
	if (!b->px)
		return NULL;
	b->dirty = 1;
	b->hole = 0;
	return b;
}

static struct res *op_target(struct res *d)
{
	return op_target_ex(d, 0);
}

static void px_hspan(struct res *d, int x, int y, int w, uint32_t c)
{
	struct res *b = d->buf;
	int bx, by, bw, i;

	if (!b || !b->px || !span_clip(d, x, y, w, &bx, &by, &bw))
		return;
	if (b->bpp == 1) {
		memset((uint8_t *)b->px + (size_t)by * b->w + bx,
		       (uint8_t)c, (size_t)bw);
	} else if (b->bpp == 4) {
		uint32_t *row = (uint32_t *)b->px + (size_t)by * b->w + bx;

		for (i = 0; i < bw; i++)
			row[i] = c;
	} else {
		uint16_t *row = b->px + (size_t)by * b->w + bx;

		for (i = 0; i < bw; i++)
			row[i] = (uint16_t)c;
	}
}

/*
 * Recompute where a drawable lives inside its buffer, and its clip, for it and
 * everything under it. Called whenever geometry, mapping or parentage changes.
 *
 * An unmapped window gets an empty clip rather than a special case in the
 * drawing path: drawing into an unmapped window is discarded, and the client
 * repaints when we Expose it on map.
 */
static void geom_update(struct res *w)
{
	struct res *p;
	int i;

	if (!w || w->type == R_FREE)
		return;
	if (w->type == R_PIXMAP || (w->type == R_WINDOW &&
				    w->parent == ROOT_ID)) {
		w->buf = w;
		w->ax = w->ay = 0;
		w->cx0 = w->cy0 = 0;
		w->cx1 = w->w; w->cy1 = w->h;
	} else if (w->type == R_WINDOW && (p = res_find(w->parent)) &&
		   p->type == R_WINDOW && p->buf) {
		w->buf = p->buf;
		/*
		 * X positions a window by the corner of its BORDER: the
		 * content begins bw further in. Treating x,y as the content
		 * corner drew oclock 6 px up-left of where it belonged, its
		 * border's top and left arms clipped off by the parent and
		 * the vacated right and bottom showing as thick black bands.
		 */
		w->ax = p->ax + w->x + w->bw;
		w->ay = p->ay + w->y + w->bw;
		w->cx0 = w->ax > p->cx0 ? w->ax : p->cx0;
		w->cy0 = w->ay > p->cy0 ? w->ay : p->cy0;
		w->cx1 = w->ax + w->w < p->cx1 ? w->ax + w->w : p->cx1;
		w->cy1 = w->ay + w->h < p->cy1 ? w->ay + w->h : p->cy1;
		if (!w->mapped)
			w->cx1 = w->cx0;	/* empty */
	} else {
		w->buf = NULL;
		return;
	}
	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_WINDOW && res[i].parent == w->id &&
		    &res[i] != w)
			geom_update(&res[i]);
}

/*
 * Fill a window's background, skipping the areas its mapped children occupy.
 *
 * In X a child window is a separate drawable clipped OUT of its parent, so
 * painting the parent never touches it. Sharing one buffer makes that
 * something we have to do by hand - and not doing it is not subtle: mapping a
 * container erases every child already drawn inside it, which is how xcalc
 * lost its Xaw bevels and its base indicator while every individual draw was
 * provably correct.
 *
 * Per row, gather the children's x-spans and fill the gaps. Rows times
 * children, and only on a background fill, which is rare.
 */
static int is_descendant(struct res *w, struct res *of)
{
	int guard = 16;

	while (w && guard--) {
		if (w->parent == of->id)
			return 1;
		if (w->parent == ROOT_ID)
			return 0;
		w = res_find(w->parent);
	}
	return 0;
}

/*
 * Fill a window's background, skipping every mapped DESCENDANT.
 *
 * In X a child window is a separate drawable clipped out of its ancestors, so
 * painting a container never touches what is inside it. Sharing one buffer
 * makes that something we do by hand, and it has to cover the whole subtree,
 * not just direct children: excluding only direct children still let the
 * top-level's fill erase its grandchildren, which is how xcalc lost the black
 * bezel around its display while every individual draw was provably correct.
 *
 * Rects are gathered once and filtered per row; this only runs on a background
 * fill, which is rare.
 */
/*
 * What a window's background IS at one point: a tile from its background
 * pixmap if it has one, otherwise its background colour. Tiling is what X
 * specifies, and it costs nothing here because the common case is a pixmap the
 * size of the window.
 */
static uint32_t win_bg_at(struct res *d, int x, int y)
{
	struct res *p = d->bg_pixmap ? res_find(d->bg_pixmap) : NULL;


	if (p && p->type == R_PIXMAP && drawable_ok(p) && p->w > 0 && p->h > 0)
		return px_get(p, ((x % p->w) + p->w) % p->w,
			      ((y % p->h) + p->h) % p->h);
	return d->bg;
}

static void win_fill(struct res *d, int x, int y, int w, int h)
{
	struct { int x0, y0, x1, y1; } ob[80];
	int nob = 0, i, j, k;
	struct res *bgp;
	int tiled;

	/*
	 * An aliased window IS its background pixmap, so painting the
	 * background into it is not merely wasted - the first write would take
	 * the alias apart and reinstate the copy this exists to avoid.
	 */
	if (d->type == R_WINDOW && d->alias && d->alias == d->bg_pixmap)
		return;

	/*
	 * About to paint the WHOLE window from a background pixmap of exactly
	 * its size? Then the window's content is that pixmap, and it can
	 * simply borrow the pixels instead of holding a second copy and
	 * refreshing it from the first. Doing it HERE rather than when the
	 * background is set keeps X's semantics: a window does not adopt a new
	 * background until something clears it, and this is that moment.
	 *
	 * For xfiles it retires a 600x460 duplicate - 539 kB - and a
	 * full-window copy on every repaint.
	 */
	if (d->type == R_WINDOW && d->buf == d && !d->alias && d->bg_pixmap &&
	    x <= 0 && y <= 0 && w >= d->w && h >= d->h) {
		struct res *pm = res_find(d->bg_pixmap);

		if (pm && pm->type == R_PIXMAP && pm->px && pm->w == d->w &&
		    pm->h == d->h && pm->bpp == d->bpp) {
			px_release(d);
			d->px = pm->px;
			d->hole = pm->hole;
			d->alias = pm->id;
			/*
			 * The desktop CACHES this pointer (lvdesk keeps it in
			 * an image descriptor and only re-reads it when it
			 * notices a change), so swapping the buffer without
			 * saying so leaves LVGL rendering from storage we just
			 * released. free() made that invisible - the pages
			 * stayed mapped - which is why it survived until the
			 * allocator changed underneath it.
			 */
			notify_draw(d);
			if (trace_on())
				fprintf(stderr, "xshim: win 0x%x now aliases "
					"pixmap 0x%x (%dx%d, %zu kB saved)\n",
					d->id, pm->id, d->w, d->h,
					(size_t)d->w * d->h * d->bpp / 1024);
			return;
		}
	}

	if (d->type != R_WINDOW) {
		if (!op_target(d))
			return;
		for (j = y; j < y + h; j++)
			px_hspan(d, x, j, w, d->bg);	/* raw: px_hspan
							 * writes it in the
							 * buffer's own
							 * width - a 32-bit
							 * client's pixel
							 * truncated to 16
							 * painted xfiles'
							 * sheet black */
		return;
	}
	for (i = 0; i < MAXRES && nob < (int)(sizeof(ob) / sizeof(ob[0])); i++) {
		struct res *c2 = &res[i];

		if (c2->type != R_WINDOW || c2 == d || !c2->mapped)
			continue;
		if (c2->buf != d->buf || !is_descendant(c2, d))
			continue;
		/* Into d's own coordinate space, border included. */
		ob[nob].x0 = c2->ax - d->ax - c2->bw;
		ob[nob].y0 = c2->ay - d->ay - c2->bw;
		ob[nob].x1 = c2->ax - d->ax + c2->w + c2->bw;
		ob[nob].y1 = c2->ay - d->ay + c2->h + c2->bw;
		nob++;
	}
	/*
	 * A window background is a solid colour unless a background PIXMAP is
	 * set, and win_bg_at() re-derives that per pixel. Decide once, then
	 * move whole rows: px_set() pays ~6 branches, an alias_break() and a
	 * call for every single pixel, which a PC profile of one maximise put
	 * at 16.4% in px_set plus 1.6% in alias_break_ex - for a fill whose
	 * colour never changes. Only a TILED background still needs the
	 * per-pixel path, because there the colour really does vary.
	 */
	bgp = d->bg_pixmap ? res_find(d->bg_pixmap) : NULL;
	tiled = bgp && bgp->type == R_PIXMAP && drawable_ok(bgp) &&
		bgp->w > 0 && bgp->h > 0;
	if (!op_target(d))
		return;
	/*
	 * Sort the obscuring rectangles by x ONCE. This used to be an
	 * insertion sort inside the row loop, so a form with many children
	 * re-sorted the same list for every scanline - O(rows * k^2) to
	 * produce an ordering that never changes. A maximise puts 68
	 * ConfigureWindows through here, and this function was 268 ms of the
	 * 527 ms the shim spent handling one.
	 */
	for (i = 0; i + 1 < nob; i++)
		for (k = i + 1; k < nob; k++)
			if (ob[k].x0 < ob[i].x0) {
				typeof(ob[0]) t = ob[i];

				ob[i] = ob[k];
				ob[k] = t;
			}
	for (j = y; j < y + h; j++) {
		int sp[80][2], n = 0, cur = x;

		/* ob is x-sorted, so sp comes out sorted for free. */
		for (i = 0; i < nob; i++) {
			if (j < ob[i].y0 || j >= ob[i].y1)
				continue;
			if (ob[i].x1 <= x || ob[i].x0 >= x + w)
				continue;
			sp[n][0] = ob[i].x0;
			sp[n][1] = ob[i].x1;
			n++;
		}
		for (i = 0; i < n; i++) {
			int end = sp[i][0] < x + w ? sp[i][0] : x + w;

			if (end > cur) {
				if (tiled)
					for (k = cur; k < end; k++)
						px_set(d, k, j,
						       win_bg_at(d, k, j));
				else
					px_hspan(d, cur, j, end - cur,
						 d->bg);
			}
			if (sp[i][1] > cur)
				cur = sp[i][1];
		}
		if (x + w > cur) {
			if (tiled)
				for (k = cur; k < x + w; k++)
					px_set(d, k, j, win_bg_at(d, k, j));
			else
				px_hspan(d, cur, j, x + w - cur,
					 d->bg);
		}
	}
}

static void draw_border(struct res *w)
{
	struct res *p;
	int i, j;

	if (!w || w->bw <= 0 || w->parent == ROOT_ID)
		return;
	p = res_find(w->parent);
	if (!p || p->type != R_WINDOW || !drawable_ok(p))
		return;
	/*
	 * Four spans, not a scan of the whole box. This used to walk every
	 * pixel of (w + 2bw) x (h + 2bw) and `continue` over the interior, so
	 * painting a ring cost the area it encloses: a 150x90 child was 13,500
	 * px_set calls - each ~6 branches, a call and an alias_break - to set
	 * about 480 pixels. With ~60 children, and redraw_child_borders()
	 * re-running it after every fill that touches them, that was the bulk
	 * of ConfigureWindow's 263 ms of CPU across one maximise.
	 */
	if (!op_target(p))
		return;
	{
		int bw = w->bw, x0 = w->x, y0 = w->y;
		int tw = w->w + 2 * bw, th = w->h + 2 * bw;
		uint32_t c = w->border_pixel;

		for (j = 0; j < bw; j++) {		/* top and bottom */
			px_hspan(p, x0, y0 + j, tw, c);
			px_hspan(p, x0, y0 + th - 1 - j, tw, c);
		}
		for (j = bw; j < th - bw; j++) {	/* left and right */
			px_hspan(p, x0, y0 + j, bw, c);
			px_hspan(p, x0 + tw - bw, y0 + j, bw, c);
		}
	}
	(void)i;
}

/*
 * A fill on a window paints the PARENT's pixels, and a child's border lives
 * exactly there - it is drawn in the parent and belongs to no window's own
 * content, so no Expose ever brings it back. xcalc's button outlines flashed
 * at startup (drawn once at creation) and were then erased by the Form's
 * first background fill, permanently. Any fill that touches a window now
 * re-draws the borders of the mapped children it may have painted over.
 */
static void redraw_child_borders(struct res *d, int x, int y, int w, int h)
{
	int i;

	if (!d || d->type != R_WINDOW)
		return;
	for (i = 0; i < MAXRES; i++) {
		struct res *ch = &res[i];

		if (ch->type != R_WINDOW || ch->parent != d->id ||
		    !ch->mapped || ch->bw <= 0)
			continue;
		if (ch->x - ch->bw >= x + w || ch->y - ch->bw >= y + h ||
		    ch->x + ch->w + ch->bw <= x || ch->y + ch->h + ch->bw <= y)
			continue;
		draw_border(ch);
	}
}

static void draw_line(struct res *d, int x0, int y0, int x1, int y1, uint32_t c)
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
static void fill_poly(struct res *d, const int16_t *pts, int n, uint32_t c)
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

/* ------------------------------------------------------------- diagnostics */

/*
 * Request names, so a log line says QueryPointer rather than 38. Worth the
 * 500-odd bytes: the whole point of this table is that the next client to fail
 * here fails with a message somebody can act on.
 */
static const char *const opname[128] = {
	[1] = "CreateWindow", [2] = "ChangeWindowAttributes",
	[3] = "GetWindowAttributes", [4] = "DestroyWindow",
	[5] = "DestroySubwindows", [6] = "ChangeSaveSet", [7] = "ReparentWindow",
	[8] = "MapWindow", [9] = "MapSubwindows", [10] = "UnmapWindow",
	[11] = "UnmapSubwindows", [12] = "ConfigureWindow",
	[13] = "CirculateWindow", [14] = "GetGeometry", [15] = "QueryTree",
	[16] = "InternAtom", [17] = "GetAtomName", [18] = "ChangeProperty",
	[19] = "DeleteProperty", [20] = "GetProperty", [21] = "ListProperties",
	[22] = "SetSelectionOwner", [23] = "GetSelectionOwner",
	[24] = "ConvertSelection", [25] = "SendEvent", [26] = "GrabPointer",
	[27] = "UngrabPointer", [28] = "GrabButton", [29] = "UngrabButton",
	[30] = "ChangeActivePointerGrab", [31] = "GrabKeyboard",
	[32] = "UngrabKeyboard", [33] = "GrabKey", [34] = "UngrabKey",
	[35] = "AllowEvents", [36] = "GrabServer", [37] = "UngrabServer",
	[38] = "QueryPointer", [39] = "GetMotionEvents",
	[40] = "TranslateCoordinates", [41] = "WarpPointer",
	[42] = "SetInputFocus", [43] = "GetInputFocus", [44] = "QueryKeymap",
	[45] = "OpenFont", [46] = "CloseFont", [47] = "QueryFont",
	[48] = "QueryTextExtents", [49] = "ListFonts",
	[50] = "ListFontsWithInfo", [51] = "SetFontPath",
	[52] = "GetFontPath", [53] = "CreatePixmap", [54] = "FreePixmap",
	[55] = "CreateGC", [56] = "ChangeGC", [57] = "CopyGC",
	[58] = "SetDashes", [59] = "SetClipRectangles", [60] = "FreeGC",
	[61] = "ClearArea", [62] = "CopyArea", [63] = "CopyPlane",
	[64] = "PolyPoint", [65] = "PolyLine", [66] = "PolySegment",
	[67] = "PolyRectangle", [68] = "PolyArc", [69] = "FillPoly",
	[70] = "PolyFillRectangle", [71] = "PolyFillArc", [72] = "PutImage",
	[73] = "GetImage", [74] = "PolyText8", [75] = "PolyText16",
	[76] = "ImageText8", [77] = "ImageText16", [78] = "CreateColormap",
	[79] = "FreeColormap", [80] = "CopyColormapAndFree",
	[81] = "InstallColormap", [82] = "UninstallColormap",
	[83] = "ListInstalledColormaps", [84] = "AllocColor",
	[85] = "AllocNamedColor", [86] = "AllocColorCells",
	[87] = "AllocColorPlanes", [88] = "FreeColors", [89] = "StoreColors",
	[90] = "StoreNamedColor", [91] = "QueryColors", [92] = "LookupColor",
	[93] = "CreateCursor", [94] = "CreateGlyphCursor", [95] = "FreeCursor",
	[96] = "RecolorCursor", [97] = "QueryBestSize",
	[98] = "QueryExtension", [99] = "ListExtensions",
	[100] = "ChangeKeyboardMapping", [101] = "GetKeyboardMapping",
	[102] = "ChangeKeyboardControl", [103] = "GetKeyboardControl",
	[104] = "Bell", [105] = "ChangePointerControl",
	[106] = "GetPointerControl", [107] = "SetScreenSaver",
	[108] = "GetScreenSaver", [109] = "ChangeHosts", [110] = "ListHosts",
	[111] = "SetAccessControl", [112] = "SetCloseDownMode",
	[113] = "KillClient", [114] = "RotateProperties",
	[115] = "ForceScreenSaver", [116] = "SetPointerMapping",
	[117] = "GetPointerMapping", [118] = "SetModifierMapping",
	[119] = "GetModifierMapping", [127] = "NoOperation",
};

static const char *opstr(uint8_t op)
{
	return (op < 128 && opname[op]) ? opname[op] : "?";
}

/*
 * Which requests the client then blocks waiting for a reply to. This is the
 * distinction that matters: an unimplemented request that wants no reply is
 * a missing feature, and an unimplemented request that wants one is a HANG -
 * the client sits in _XReply for ever with nothing on stderr from either side.
 */
static int expects_reply(uint8_t op)
{
	static const uint32_t m[4] = {
		/*  0-31 */ (1u<<3)|(1u<<14)|(1u<<15)|(1u<<16)|(1u<<17)|
			    (1u<<20)|(1u<<21)|(1u<<23)|(1u<<26)|(1u<<31),
		/* 32-63 */ (1u<<(38-32))|(1u<<(39-32))|(1u<<(40-32))|
			    (1u<<(43-32))|(1u<<(44-32))|(1u<<(47-32))|
			    (1u<<(48-32))|(1u<<(49-32))|(1u<<(50-32))|
			    (1u<<(52-32)),
		/* 64-95 */ (1u<<(73-64))|(1u<<(83-64))|(1u<<(84-64))|
			    (1u<<(85-64))|(1u<<(86-64))|(1u<<(87-64))|
			    (1u<<(91-64))|(1u<<(92-64)),
		/* 96-127*/ (1u<<(97-96))|(1u<<(98-96))|(1u<<(99-96))|
			    (1u<<(101-96))|(1u<<(103-96))|(1u<<(106-96))|
			    (1u<<(108-96))|(1u<<(110-96))|(1u<<(116-96))|
			    (1u<<(117-96))|(1u<<(118-96))|(1u<<(119-96)),
	};

	return op < 128 && (m[op >> 5] & (1u << (op & 31)));
}

static int trace_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XSHIM_TRACE") != NULL;
	return v;
}

/* The last few requests this client sent, oldest first. */
static void dump_recent(struct cli *c)
{
	int i;

	fprintf(stderr, "xshim:   last %d requests:", c->nrecent);
	for (i = 0; i < c->nrecent; i++)
		fprintf(stderr, " %s", opstr(c->recent[i]));
	fprintf(stderr, "\n");
}

/*
 * Pick the closest embedded font for an X font name.
 *
 * Clients ask three ways: a short alias ("8x13", "fixed"), a full XLFD, or an
 * XLFD full of wildcards with only a size pinned down. All three have to land
 * somewhere sensible, because a client that cannot open a font usually gives
 * up rather than choose for itself.
 *
 * The size wanted comes from XLFD field 7 (pixels) or field 8 (decipoints);
 * the family only has to separate Adobe Symbol from everything else, since
 * that is the distinction that changes which glyph a byte means.
 */
static int font_pick(const uint8_t *name, int n)
{
	char b[160];
	const char *tok[16];
	int i, ntok = 0, want = 0, symbol = 0, best = 0, bestd = 1 << 30;

	if (n > (int)sizeof(b) - 1)
		n = sizeof(b) - 1;
	for (i = 0; i < n; i++)
		b[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32
							  : name[i];
	b[n] = 0;

	for (i = 0; i < XFONT_N; i++)		/* exact alias, e.g. "8x13" */
		if (!strcmp(b, xfonts[i].alias))
			return i;
	if (!strcmp(b, "fixed"))
		return 1;			/* 6x13, X's traditional default */
	if (strstr(b, "symbol"))
		symbol = 1;

	/* Split the XLFD; tok[7] is pixel size and tok[8] point size. */
	tok[ntok++] = b;
	for (i = 0; i < n && ntok < 16; i++)
		if (b[i] == '-') {
			b[i] = 0;
			tok[ntok++] = b + i + 1;
		}
	if (ntok > 7 && tok[7][0] >= '1' && tok[7][0] <= '9')
		want = atoi(tok[7]);
	else if (ntok > 8 && tok[8][0] >= '1' && tok[8][0] <= '9')
		want = (atoi(tok[8]) * 75 + 360) / 720;	/* decipoints at 75dpi */
	if (!want)
		want = 13;

	for (i = 0; i < XFONT_N; i++) {
		int is_sym = strstr(xfonts[i].xlfd, "symbol") != NULL;
		int d = xfonts[i].h - want;

		if (is_sym != symbol)
			continue;
		if (d < 0)
			d = -d;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

static const struct xfont *font_of(struct res *g)
{
	struct res *f = g ? res_find(g->font) : NULL;

	if (f && f->type == R_FONT && f->font_idx >= 0 &&
	    f->font_idx < XFONT_N)
		return &xfonts[f->font_idx];
	return &xfonts[0];
}

static int glyph_adv(const struct xfont *f, uint8_t ch)
{
	if (ch < f->first || ch > f->last)
		return f->w ? f->w : f->box;
	return f->adv ? f->adv[ch - f->first] : f->w;
}

/*
 * One glyph, origin on the baseline as X defines it: bitmap row r lands at
 * y - ascent + r, so the descent rows fall below the baseline. Anything
 * outside the font's range is skipped rather than substituted - a missing
 * glyph should look missing, not like a different character.
 */
static void draw_glyph(struct res *d, int x, int y, uint8_t ch, uint32_t c,
		       const struct xfont *f)
{
	const unsigned char *g;
	int row, col;

	if (ch < f->first || ch > f->last)
		return;
	g = f->bits + (size_t)(ch - f->first) * f->h * f->bpr;
	for (row = 0; row < f->h; row++)
		for (col = 0; col < f->box; col++)
			if (g[row * f->bpr + (col >> 3)] & (0x80 >> (col & 7)))
				px_set(d, x + col, y - f->ascent + row, c);
}

static int draw_string(struct res *d, int x, int y, const uint8_t *str, int n,
		       uint32_t c, const struct xfont *f)
{
	int i, adv = 0;

	for (i = 0; i < n; i++) {
		draw_glyph(d, x + adv, y, str[i], c, f);
		adv += glyph_adv(f, str[i]);
	}
	return adv;
}

static int text_width(const struct xfont *f, const uint8_t *str, int n)
{
	int i, adv = 0;

	for (i = 0; i < n; i++)
		adv += glyph_adv(f, str[i]);
	return adv;
}

/*
 * Colour names. Every X client resolves at least a few of these through the
 * server, and one that cannot resolve a name usually gives up rather than pick
 * a default - so this is cheap generality, not decoration.
 *
 * The full rgb.txt is 750-odd names and a file on the card; this is the set
 * that actually turns up, plus the numeric forms. An unknown name is logged BY
 * NAME, which makes the next gap a one-line fix rather than an investigation.
 */
static const struct { const char *name; uint8_t r, g, b; } cnames[] = {
	{ "black", 0x00, 0x00, 0x00 }, { "white", 0xFF, 0xFF, 0xFF },
	{ "red", 0xFF, 0x00, 0x00 },   { "green", 0x00, 0xFF, 0x00 },
	{ "blue", 0x00, 0x00, 0xFF },  { "cyan", 0x00, 0xFF, 0xFF },
	{ "magenta", 0xFF, 0x00, 0xFF }, { "yellow", 0xFF, 0xFF, 0x00 },
	{ "gray", 0xBE, 0xBE, 0xBE },  { "grey", 0xBE, 0xBE, 0xBE },
	{ "lightgray", 0xD3, 0xD3, 0xD3 }, { "lightgrey", 0xD3, 0xD3, 0xD3 },
	{ "darkgray", 0xA9, 0xA9, 0xA9 },  { "darkgrey", 0xA9, 0xA9, 0xA9 },
	{ "navy", 0x00, 0x00, 0x80 },  { "maroon", 0x80, 0x00, 0x00 },
	{ "orange", 0xFF, 0xA5, 0x00 },{ "purple", 0xA0, 0x20, 0xF0 },
	{ "brown", 0xA5, 0x2A, 0x2A }, { "pink", 0xFF, 0xC0, 0xCB },
};

static int hexval(int ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

/* 16-bit components out, which is what the protocol carries. */
static int color_lookup(const uint8_t *nm, int n, uint16_t *ro, uint16_t *go,
			uint16_t *bo)
{
	char buf[40];
	int i, j = 0;

	if (n > 0 && nm[0] == '#') {		/* #rgb, #rrggbb, #rrrrggggbbbb */
		int digits = n - 1, per = digits / 3, v[3] = { 0, 0, 0 }, k;

		if (per < 1 || per > 4 || per * 3 != digits)
			return 0;
		for (k = 0; k < 3; k++) {
			int acc = 0, w = per;

			for (i = 0; i < per; i++) {
				int h = hexval(nm[1 + k * per + i]);

				if (h < 0)
					return 0;
				acc = acc * 16 + h;
			}
			while (w * 4 < 16) {	/* scale up to 16 bits */
				acc = (acc << (w * 4)) | acc;
				w *= 2;
			}
			v[k] = acc & 0xFFFF;
		}
		*ro = v[0]; *go = v[1]; *bo = v[2];
		return 1;
	}
	/* Case- and space-insensitive, as the protocol requires. */
	for (i = 0; i < n && j < (int)sizeof(buf) - 1; i++) {
		int ch = nm[i];

		if (ch == ' ')
			continue;
		buf[j++] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
	}
	buf[j] = 0;
	for (i = 0; i < (int)(sizeof(cnames) / sizeof(cnames[0])); i++)
		if (!strcmp(buf, cnames[i].name)) {
			*ro = cnames[i].r * 0x101;
			*go = cnames[i].g * 0x101;
			*bo = cnames[i].b * 0x101;
			return 1;
		}
	/*
	 * grayN / greyN, the numeric family from rgb.txt: N is a PERCENTAGE,
	 * 0 to 100, not a byte. st asks for gray50 and gray90 for its cursor
	 * and reverse video, and resolving those to black gave a black cursor
	 * on a black cell. Parsing the number covers all 101 of them in a few
	 * lines instead of adding rows to the table one complaint at a time.
	 */
	if (!strncmp(buf, "gray", 4) || !strncmp(buf, "grey", 4)) {
		const char *d = buf + 4;
		int pct = 0, ndig = 0;

		while (*d >= '0' && *d <= '9' && ndig < 3) {
			pct = pct * 10 + (*d++ - '0');
			ndig++;
		}
		if (ndig && !*d && pct <= 100) {
			int v = (pct * 255 + 50) / 100;

			*ro = *go = *bo = (uint16_t)(v * 0x101);
			return 1;
		}
	}
	/*
	 * The numbered brightness variants - blue2, cyan3, magenta3 and the
	 * rest of them. rgb.txt gives every colour four, scaling the
	 * full-intensity version by 255, 238, 205 and 139. st uses them for
	 * its ANSI palette, so without this its bright colours were black.
	 *
	 * Same reasoning as grayN above: parse the family, do not grow the
	 * table one bug report at a time.
	 */
	if (j > 1 && buf[j - 1] >= '1' && buf[j - 1] <= '4') {
		static const uint8_t level[4] = { 255, 238, 205, 139 };
		int lv = buf[j - 1] - '1';
		char base[64];

		memcpy(base, buf, (size_t)j - 1);
		base[j - 1] = 0;
		for (i = 0; i < (int)(sizeof(cnames) / sizeof(cnames[0])); i++)
			if (!strcmp(base, cnames[i].name)) {
				*ro = (uint16_t)(cnames[i].r * level[lv] / 255)
				      * 0x101;
				*go = (uint16_t)(cnames[i].g * level[lv] / 255)
				      * 0x101;
				*bo = (uint16_t)(cnames[i].b * level[lv] / 255)
				      * 0x101;
				return 1;
			}
	}
	fprintf(stderr, "xshim: unknown colour name '%s' - add it to cnames[] "
		"(using black)\n", buf);
	*ro = *go = *bo = 0;
	return 1;				/* never fail; clients give up */
}

static uint32_t rgb565(uint16_t r, uint16_t g, uint16_t b)
{
	return ((uint32_t)(r >> 11) << 11) | ((uint32_t)(g >> 10) << 5) |
	       (uint32_t)(b >> 11);
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
	uint8_t b[384], *p = b + 8;	/* depth-8 and depth-32 visuals */
	static const char vendor[] = "lvdesk-shim";
	int vlen = sizeof(vendor) - 1, vpad = (4 - (vlen & 3)) & 3;
	uint8_t *body = p;

	put32(p, 1);            p += 4;		/* release number */
	/*
	 * Each client gets its OWN resource-id range, which is what a real X
	 * server does and what the base/mask pair is for. Handing every client
	 * the same base means two clients allocate the SAME ids, res_find()
	 * cannot tell them apart, and the second one to disconnect frees the
	 * first one's window buffers - which lvdesk is still presenting
	 * through an lv_image. That killed the desktop, and only ever with two
	 * clients connected, which is why xclock alone never showed it.
	 */
	put32(p, RES_BASE * (uint32_t)(c - cli + 1)); p += 4;
	put32(p, RES_MASK);     p += 4;
	put32(p, 0);            p += 4;		/* motion buffer size */
	put16(p, vlen);         p += 2;
	/*
	 * Max request length, in 4-byte units. This MUST fit the input buffer:
	 * advertising 65535 (256 kB) into a 64 kB buffer invites a client to
	 * send a request we can never frame, and the failure is a stall
	 * followed by a disconnect with nothing to explain it. BIG-REQUESTS is
	 * refused, so this is a hard ceiling.
	 */
	put16(p, INBUF / 4);    p += 2;
	*p++ = 1;				/* screens */
	*p++ = 5;				/* pixmap formats */
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
		/*
		 * {8,8} is what makes an 8-bit client possible: a client
		 * takes bits_per_pixel for a depth from THIS list, never from
		 * a guess, so without the entry an 8-bit image is packed at
		 * the wrong stride and nothing lines up.
		 */
		static const uint8_t f[5][2] = { {1, 1}, {8, 8}, {16, 16},
						 {24, 32}, {32, 32} };
		int i;

		for (i = 0; i < 5; i++) {
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
	*p++ = 0; *p++ = 0; *p++ = 16; *p++ = 4; /* depths: 16, 1, 8, 32 */

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

	/*
	 * Depth 8, PseudoColor - the whole point of which is that the client
	 * writes palette INDICES and the server owns the colours. Doom is a
	 * palette engine; forcing it through a 16-bit visual makes it do a
	 * lookup per pixel inside its column loops and move twice the bytes,
	 * which measured 13.3 fps against 16.2 for the same demo.
	 *
	 * colormap_entries is 256 and the masks are zero: for an indexed
	 * visual there are no channel masks, and a client that finds one
	 * would try to compose pixels itself.
	 */
	*p++ = 8; *p++ = 0; put16(p, 1); p += 2; put32(p, 0); p += 4;
	put32(p, VISUAL8_ID);   p += 4;
	*p++ = 3;				/* PseudoColor */
	*p++ = 8;				/* bits per rgb */
	put16(p, 256);          p += 2;		/* colormap entries */
	put32(p, 0);            p += 4;		/* no red mask   */
	put32(p, 0);            p += 4;		/* no green mask */
	put32(p, 0);            p += 4;		/* no blue mask  */
	put32(p, 0);            p += 4;

	/*
	 * Depth 32, TrueColor - ARGB8888.
	 *
	 * st asks for exactly this, unconditionally, and does NOT check the
	 * result: `XMatchVisualInfo(dpy, scr, 32, TrueColor, &vis)` then
	 * `xw.vis = vis.visual`. With no depth-32 visual the match fails, vis
	 * is left uninitialised, and st builds its window and backing pixmap
	 * on a garbage visual - it runs, accepts input and draws nothing at
	 * all, which is exactly what it did here.
	 *
	 * The masks are the ordinary ARGB layout. Alpha is advertised only by
	 * the depth being 32 while the masks cover 24 bits, which is how every
	 * server does it.
	 */
	*p++ = 32; *p++ = 0; put16(p, 1); p += 2; put32(p, 0); p += 4;
	put32(p, VISUAL32_ID);  p += 4;
	*p++ = 4;				/* TrueColor */
	*p++ = 8;				/* bits per rgb */
	put16(p, 0);            p += 2;		/* colormap entries */
	put32(p, 0x00FF0000);   p += 4;		/* red   */
	put32(p, 0x0000FF00);   p += 4;		/* green */
	put32(p, 0x000000FF);   p += 4;		/* blue  */
	put32(p, 0);            p += 4;

	b[0] = 1; b[1] = 0;
	put16(b + 2, 11); put16(b + 4, 0);
	put16(b + 6, (p - body) / 4);
	out_push(c, b, p - b);
}

/*
 * A reply with a file descriptor attached.
 *
 * The socket is already a unix socket, so a descriptor can ride along the same
 * reply that carries the geometry - no second channel, no name to agree on and
 * nothing to clean up if the client dies. This is the whole mechanism behind
 * the shared-pixmap path: the client ends up with the SAME pages the shim
 * draws into, so bulk pixel traffic stops being protocol at all.
 */
static void send_reply_fd_detail(struct cli *c, uint8_t detail,
				 const uint8_t *d24, int fd)
{
	out_flush(c);	/* the fd rides THIS reply; keep the stream ordered */
	uint8_t h[32];
	struct msghdr m;
	struct iovec io;
	union {
		struct cmsghdr al;
		char b[CMSG_SPACE(sizeof(int))];
	} u;
	struct cmsghdr *cm;

	nreplies++;
	memset(h, 0, sizeof(h));
	h[0] = 1;
	h[1] = detail;
	put16(h + 2, c->seq);
	memcpy(h + 8, d24, 24);

	memset(&m, 0, sizeof(m));
	memset(&u, 0, sizeof(u));
	io.iov_base = h;
	io.iov_len = sizeof(h);
	m.msg_iov = &io;
	m.msg_iovlen = 1;
	m.msg_control = u.b;
	m.msg_controllen = sizeof(u.b);
	cm = CMSG_FIRSTHDR(&m);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
	if (sendmsg(c->fd, &m, 0) < 0)
		perror("xshim: sendmsg");
}

/*
 * The detail byte carries meaning in some replies and not others. XLITE-SHM's
 * GetPixmapFd does not use it; MIT-SHM's ShmCreateSegment puts its `nfd` count
 * there. Keep the old spelling for the callers that mean zero.
 */
static void send_reply_fd(struct cli *c, const uint8_t *d24, int fd)
{
	send_reply_fd_detail(c, 0, d24, fd);
}

/*
 * XSHIM_PROF=1: where does a request's time actually go? Added because a
 * no-op GetInputFocus round trip measured ~3.8 ms inside xshim_poll while
 * every step in it should be microseconds. Counters, not per-call printf -
 * a print per request would BE the cost it claims to measure.
 */
static uint64_t xsp_read, xsp_handle, xsp_write, xsp_poll, xsp_calls;
/* handle time by request: core ops 0..127, RENDER minors at 128+minor */
static uint64_t xsp_op_ns[256];
static uint32_t xsp_op_n[256];
/* pixels touched per request, so ns/px separates a slow loop from sheer
 * volume - a fast loop asked to repaint everything looks identical to a
 * slow loop on the profile until you divide. */
static uint64_t xsp_op_px[256];
/* Events SENT, by type - which of them a client answers with a full
 * repaint is the question a slow resize turns on. */
static uint32_t xsp_ev[64];
/*
 * Wall time in a handler is NOT lvdesk's cost. There is one core: after
 * send_reply wakes the client, the client runs and this process is simply
 * off-CPU, with the wall clock still running. QueryPictFormats "taking"
 * 20 ms - a 140-byte static reply - is that, not work. CPU time per handler
 * separates "we are slow" from "we are waiting for the client".
 */
static uint64_t xsp_op_cpu[256];
/*
 * Extension requests are keyed by minor opcode, and two extensions can
 * share one - "R1" was ambiguous between RENDER and anything else the
 * client asked for. Record the major so a hot request can be named.
 */
static uint8_t xsp_op_major[256];
static uint64_t xsp_cpu_total;
uint64_t xshim_px_acc;

static uint64_t xsp_cpu_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int xsp_on = -1;

/*
 * CPU time. This was CLOCK_MONOTONIC, which on a saturated single core also
 * counts the time the CLIENT is running while we are descheduled - and for a
 * request that requires a round trip, that is most of it. It is why `handle`
 * read 1901 us wall against 667 us of actual CPU.
 */
static uint64_t xsp_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void xsp_dump(void)
{
	static uint64_t last;
	uint64_t now = xsp_now();

	if (last && now - last < 1000000000ull)
		return;
	last = now;
	if (xsp_calls)
		fprintf(stderr, "xsp: %llu calls  poll=%lluus read=%lluus "
			"handle=%lluus(cpu %lluus) write=%lluus (per call "
			"p=%llu r=%llu h=%llu w=%llu)\n",
			(unsigned long long)xsp_calls,
			(unsigned long long)(xsp_poll / 1000),
			(unsigned long long)(xsp_read / 1000),
			(unsigned long long)(xsp_handle / 1000),
			(unsigned long long)(xsp_cpu_total / 1000),
			(unsigned long long)(xsp_write / 1000),
			(unsigned long long)(xsp_poll / 1000 / xsp_calls),
			(unsigned long long)(xsp_read / 1000 / xsp_calls),
			(unsigned long long)(xsp_handle / 1000 / xsp_calls),
			(unsigned long long)(xsp_write / 1000 / xsp_calls));
	if (xsp_calls) {
		int k, t, top[4] = { -1, -1, -1, -1 };

		for (k = 0; k < 256; k++) {
			if (!xsp_op_ns[k])
				continue;
			for (t = 0; t < 4; t++)
				if (top[t] < 0 ||
				    xsp_op_ns[k] > xsp_op_ns[top[t]]) {
					memmove(top + t + 1, top + t,
						(size_t)(3 - t) * sizeof(int));
					top[t] = k;
					break;
				}
		}
		fprintf(stderr, "xsp:   top:");
		for (t = 0; t < 4 && top[t] >= 0; t++)
			fprintf(stderr, "  %s%u/maj%u x%u %llums %lluKpx %lluns/px",
				top[t] >= 128 ? "R" : "",
				(unsigned)(top[t] >= 128 ? top[t] - 128
							 : top[t]),
				(unsigned)xsp_op_major[top[t]],
				xsp_op_n[top[t]],
				(unsigned long long)
				(xsp_op_ns[top[t]] / 1000000),
				(unsigned long long)
				(xsp_op_px[top[t]] / 1000),
				(unsigned long long)
				(xsp_op_px[top[t]] ?
				 xsp_op_ns[top[t]] / xsp_op_px[top[t]] : 0));
		for (t = 0; t < 4 && top[t] >= 0; t++)
			fprintf(stderr, "%s cpu %llums",
				t ? "," : "\nxsp:   cpu:",
				(unsigned long long)
				(xsp_op_cpu[top[t]] / 1000000));
		fprintf(stderr, "\n");
	}
	{
		static const char *evn[] = { [6] = "Motion", [7] = "Enter",
			[8] = "Leave", [12] = "Expose", [19] = "Map",
			[22] = "Configure", [4] = "Press", [5] = "Release" };
		int k, any = 0;

		for (k = 0; k < 64; k++)
			if (xsp_ev[k]) {
				fprintf(stderr, "%s %s(%d)=%u",
					any ? "" : "xsp:   sent:", 
					(k < 32 && evn[k]) ? evn[k] : "ev", k,
					xsp_ev[k]);
				any = 1;
			}
		if (any)
			fprintf(stderr, "\n");
		memset(xsp_ev, 0, sizeof(xsp_ev));
	}
	memset(xsp_op_ns, 0, sizeof(xsp_op_ns));
	memset(xsp_op_n, 0, sizeof(xsp_op_n));
	memset(xsp_op_px, 0, sizeof(xsp_op_px));
	memset(xsp_op_cpu, 0, sizeof(xsp_op_cpu));
	xsp_cpu_total = 0;
	xsp_read = xsp_handle = xsp_write = xsp_poll = xsp_calls = 0;
}

#define PEND_MAX	(2u << 20)

/* Queue bytes the socket refused; they go out on a later pass. */
static void pend_add(struct cli *c, const uint8_t *p, size_t n)
{
	if (c->pendn + n > c->pendcap) {
		size_t cap = c->pendcap ? c->pendcap : 65536;
		uint8_t *np;

		while (cap < c->pendn + n)
			cap *= 2;
		np = realloc(c->pend, cap);
		if (!np)
			return;			/* dropped; the client is doomed */
		c->pend = np;
		c->pendcap = cap;
	}
	memcpy(c->pend + c->pendn, p, n);
	c->pendn += n;
}

/* Send as much of [p, p+n) as the socket takes now; return what went. */
static size_t out_try(struct cli *c, const uint8_t *p, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t w;
		uint64_t tw = xsp_on > 0 ? xsp_now() : 0;

		w = send(c->fd, p + off, n - off, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (tw)
			xsp_write += xsp_now() - tw;
		if (w <= 0)
			break;		/* EAGAIN, or an error client_data sees */
		off += (size_t)w;
	}
	return off;
}

static void out_flush(struct cli *c)
{
	size_t went;

	if (c->pendn) {
		went = out_try(c, c->pend, c->pendn);
		if (went < c->pendn) {
			memmove(c->pend, c->pend + went, c->pendn - went);
			c->pendn -= went;
			if (c->outn) {
				pend_add(c, c->out, c->outn);
				c->outn = 0;
			}
			return;
		}
		c->pendn = 0;
	}
	went = out_try(c, c->out, c->outn);
	if (went < c->outn)
		pend_add(c, c->out + went, c->outn - went);
	c->outn = 0;
}

static void out_push(struct cli *c, const void *p, size_t n)
{
	if (c->outn + n > sizeof(c->out))
		out_flush(c);
	if (n > sizeof(c->out)) {	/* larger than the buffer: direct */
		out_flush(c);
		if (c->pendn)
			pend_add(c, p, n);
		else {
			size_t went = out_try(c, p, n);

			if (went < n)
				pend_add(c, (const uint8_t *)p + went, n - went);
		}
		return;
	}
	memcpy(c->out + c->outn, p, n);
	c->outn += n;
}

static void send_reply(struct cli *c, uint8_t detail, const uint8_t *d24,
		       const uint8_t *extra, int nextra)
{
	nreplies++;
	uint8_t h[32];

	h[0] = 1; h[1] = detail;
	put16(h + 2, c->seq);
	put32(h + 4, nextra / 4);
	memcpy(h + 8, d24, 24);
	out_push(c, h, 32);
	if (nextra)
		out_push(c, extra, nextra);
}

/*
 * An X Error - same 32 bytes as an event, and it satisfies a client blocked in
 * _XReply. Xlib's default handler then prints, on the CLIENT's stderr:
 *
 *   X Error of failed request:  BadImplementation
 *     Major opcode of failed request:  38 (X_QueryPointer)
 *
 * which names the request we failed to implement, from the client's own point
 * of view. That is worth far more than anything this end can print, and it is
 * the difference between "xcalc hangs" and a one-line to-do.
 */
#define X_BAD_REQUEST		1
#define X_BAD_VALUE		2
#define X_BAD_WINDOW		3
#define X_BAD_DRAWABLE		9
#define X_BAD_ALLOC		11
#define X_BAD_GC		13
#define X_BAD_IMPLEMENTATION	17

static void send_error(struct cli *c, uint8_t code, uint32_t bad, uint8_t major)
{
	uint8_t e[32];

	memset(e, 0, sizeof(e));
	e[1] = code;
	put16(e + 2, c->seq);
	put32(e + 4, bad);
	e[10] = major;
	out_push(c, e, 32);
	c->nbad++;
}

/* Every X event is exactly 32 bytes. Anything else desynchronises the stream. */
static void send_event_d(struct cli *c, uint8_t type, uint8_t detail,
			 const uint8_t *d, int n)
{
	uint8_t e[32];

	memset(e, 0, sizeof(e));
	e[0] = type;
	/*
	 * Byte 1 is the event's detail - for a button event, WHICH button.
	 * Leaving it zero delivers "button 0", which no translation table
	 * matches: Xt's <Btn1Down> wants 1. The click arrives, the widget is
	 * dispatched to, and nothing happens - which reads as the toolkit
	 * ignoring us rather than as a one-byte omission.
	 */
	e[1] = detail;
	put16(e + 2, c->seq);
	memcpy(e + 4, d, n > 28 ? 28 : n);
	if (xsp_on > 0 && type < 64)
		xsp_ev[type]++;
	out_push(c, e, 32);
}

static void send_event(struct cli *c, uint8_t type, const uint8_t *d, int n)
{
	send_event_d(c, type, 0, d, n);
}

/*
 * An Expose for part of a window. This is not optional bookkeeping: an X
 * client does not repaint because it drew something, it repaints because the
 * SERVER told it the pixels are gone. ClearArea with exposures set is the
 * commonest case, and swallowing it leaves the client waiting for ever - which
 * looks exactly like a drawing bug at this end.
 */
static void send_expose(struct cli *c, struct res *w, int x, int y, int ww,
			int hh)
{
	uint8_t d[28];

	memset(d, 0, sizeof(d));
	put32(d, w->id);
	put16(d + 4, x); put16(d + 6, y);
	put16(d + 8, ww); put16(d + 10, hh);
	send_event(c, 12, d, 28);		/* Expose, count 0 */
}

static void draw_border(struct res *w);

/* Union this rectangle into the window's pending Expose. */
static void queue_expose(struct cli *c, struct res *w, int x, int y,
			 int ww, int hh)
{
	if (ww <= 0 || hh <= 0)
		return;
	if (!w->exp_pend) {
		w->ex0 = x; w->ey0 = y;
		w->ex1 = x + ww; w->ey1 = y + hh;
		w->exp_cli = (int)(c - cli);
		w->exp_pend = 1;
		return;
	}
	if (x < w->ex0) w->ex0 = x;
	if (y < w->ey0) w->ey0 = y;
	if (x + ww > w->ex1) w->ex1 = x + ww;
	if (y + hh > w->ey1) w->ey1 = y + hh;
}

/*
 * Emit one Expose per window that accumulated damage this pass. Called at the
 * end of the poll pass, so a burst of reconfigures produces one repaint per
 * widget rather than one per ancestor that happened to walk over it.
 */
static void expose_flush(void)
{
	int i;

	for (i = 0; i < MAXRES; i++) {
		struct res *w = &res[i];
		struct cli *c;

		if (!w->exp_pend)
			continue;
		w->exp_pend = 0;
		if (w->exp_cli < 0 || w->exp_cli >= MAXCLI)
			continue;
		c = &cli[w->exp_cli];
		if (c->fd < 0)
			continue;
		send_expose(c, w, w->ex0, w->ey0,
			    w->ex1 - w->ex0, w->ey1 - w->ey0);
	}
	for (i = 0; i < MAXCLI; i++)
		if (cli[i].fd >= 0 && cli[i].outn)
			out_flush(&cli[i]);
}

/*
 * Paint a window's background and border, tell it to redraw, and do the same
 * for everything mapped beneath it.
 *
 * The recursion is the point. A toolkit maps CHILDREN BEFORE PARENTS - xcalc
 * sends four MapSubwindows and only then MapWindow for the shell - so when a
 * child is mapped its ancestors are still unmapped, its clip is empty, and its
 * background fill goes nowhere. Nothing ever repaints it, because the client
 * considers the background the server's job. That is what cost xcalc the black
 * bezel around its display while every individual draw was correct.
 *
 * Top-down, because win_fill() skips descendants: parents first, then the
 * children that sit on top of them.
 */
static void paint_subtree(struct cli *c, struct res *w)
{
	int i;

	win_fill(w, 0, 0, w->w, w->h);
	draw_border(w);
	queue_expose(c, w, 0, 0, w->w, w->h);
	for (i = 0; i < MAXRES; i++)
		if (res[i].type == R_WINDOW && res[i].parent == w->id &&
		    res[i].mapped && &res[i] != w)
			paint_subtree(c, &res[i]);
}

static void expose_window(struct cli *c, struct res *w)
{
	uint8_t d[28];

	memset(d, 0, sizeof(d));
	put32(d, w->id); put32(d + 4, w->id);
	send_event(c, 19, d, 28);		/* MapNotify */
	/*
	 * VisibilityNotify, Unobscured, for a client that asked for it. st
	 * refuses to draw at all until one arrives (WIN_VISIBLE is set only
	 * in its visibility handler), so without this it forked its shell
	 * after MapNotify and then sat on a blank window forever - which read
	 * as a pixel-path bug and cost an evening (2026-09-10). There is no
	 * stacking here that would ever obscure a window partially, so
	 * Unobscured on map is also the truth.
	 */
	if (w->event_mask & (1u << 16)) {		/* VisibilityChangeMask */
		memset(d, 0, sizeof(d));
		put32(d, w->id);
		d[4] = 0;				/* VisibilityUnobscured */
		send_event(c, 15, d, 28);
	}
	paint_subtree(c, w);
}

/* ========================================================================
 * RENDER
 *
 * Xft goes through RENDER for every glyph, and Xft is not optional: xfiles
 * refuses to start without a picture format, and `xclock -digital` draws its
 * text with it. Refusing the extension is honest but it caps the program base
 * at applications that still use core text, which by now is almost none.
 *
 * What Xft actually asks for is a small corner of the protocol:
 *
 *   QueryVersion, QueryPictFormats   once, at startup
 *   CreatePicture / FreePicture      one per drawable, plus a solid source
 *   CreateGlyphSet / AddGlyphs       the glyph cache, A8 coverage bitmaps
 *   CompositeGlyphs8/16/32           the actual text
 *   FillRectangles                   backgrounds and underlines
 *   Composite                        images, and Xft's fallback paths
 *
 * so that is what this implements, and everything else reports itself by name
 * through the same counter the core opcodes use.
 *
 * NOTE: handle() passes `len` in BYTES, not 4-byte words. Every list-walking
 * request here bounds itself with `r + len`, and writing `r + len * 4` reads
 * four times past the end - which showed up as the FIRST trapezoid of every
 * request being perfect and every one after it being garbage, because the
 * garbage only begins where the request does. The compositing is done here
 * in software against RGB565, which is the only format the panel has - alpha
 * exists in the SOURCE, never in the destination.
 * ===================================================================== */

#define XSHM_MAJOR		201
/*
 * MIT-SHM, the REAL one, because that is the only shared-memory extension an
 * off-the-shelf client knows how to ask for.
 *
 * XLITE-SHM (above) is ours, so only our own libX11 uses it - and SDL never
 * learns the window's pixels are shareable. It renders into a buffer it
 * malloc'd and pushes whole frames through XPutImage, which xlite then has to
 * memcpy into the window buffer: 64 kB in and 64 kB out per frame at 320x200,
 * four times that at 640x400. MIT-SHM lets the CLIENT own the memory and tell
 * us where it is, so nothing copies at all.
 */
#define MITSHM_MAJOR		202
#define VIDMODE_MAJOR	203
#define VIDMODE_ERROR	170
#define MITSHM_ERROR		144

/* Segments a client has attached. Small: a client has one or two. */
#define MAXSHMSEG 8
static struct shmseg {
	uint32_t id;		/* the client's shmseg XID */
	int shmid;		/* SysV id it passed */
	void *addr;		/* our attachment */
	size_t len;
	int owner;		/* which client, so a disconnect can clean up */
	int ro;			/* it asked for read-only; never adopt one */
	int is_fd;		/* mmap of a passed fd, not a SysV shmat */
} shmsegs[MAXSHMSEG];
#define RENDER_MAJOR	140		/* our major opcode for RENDER */
#define RENDER_ERROR	160		/* first of its five error codes */

/*
 * The picture formats we advertise. Xft matches by TEMPLATE - depth plus the
 * shifts and masks - so these have to be exactly the standard ones or
 * XRenderFindStandardFormat() returns NULL and the client exits saying it
 * could not find a format.
 */
#define PF_A8		0x30
#define PF_RGB565	0x31
#define PF_A1		0x32
#define PF_ARGB32	0x33

/*
 * The RENDER operators, in protocol order. Only three of these used to be
 * named, and blend_px()'s fast path wrote the source for anything it did not
 * recognise - so PictOpOverReverse, which must leave an opaque destination
 * alone, painted xfiles' whole 600x460 sheet black immediately after the
 * content had been composited into it. An unknown operator has to be reasoned
 * about, not defaulted to Src.
 */
enum { PICT_OP_CLEAR = 0, PICT_OP_SRC = 1, PICT_OP_DST = 2, PICT_OP_OVER = 3,
       PICT_OP_OVER_REVERSE = 4, PICT_OP_IN = 5, PICT_OP_IN_REVERSE = 6,
       PICT_OP_OUT = 7, PICT_OP_OUT_REVERSE = 8, PICT_OP_ATOP = 9,
       PICT_OP_ATOP_REVERSE = 10, PICT_OP_XOR = 11, PICT_OP_ADD = 12 };

#define MAXPICT		64
#define MAXGSET		16

struct pict {
	uint32_t id;
	uint32_t drawable;
	uint32_t format;
	int solid;			/* a CreateSolidFill picture */
	int repeat;
	uint8_t a, rr, gg, bb;		/* solid colour, 8 bits per channel */
	int has_clip;
	int cx, cy, cw, ch;		/* clip, relative to the drawable */
	int owner;
};

struct glyph {
	uint32_t id;
	int w, h, ox, oy, ax, ay;
	uint8_t *a;			/* w*h coverage, one byte each */
};

struct gset {
	uint32_t id;
	uint32_t format;
	int owner;
	struct glyph *g;
	int ng, cap;
};

static struct pict picts[MAXPICT];
static struct gset gsets[MAXGSET];

static struct pict *pict_find(uint32_t id)
{
	int i;

	/*
	 * None is not a picture. An empty slot has id 0, so looking up None
	 * returned the first FREE slot - a non-NULL record with no drawable -
	 * and every ordinary unmasked Composite was misrouted into the masked
	 * path and refused. xfiles never used a mask at all; the mask was an
	 * artefact of this lookup.
	 */
	if (!id)
		return NULL;
	for (i = 0; i < MAXPICT; i++)
		if (picts[i].id == id)
			return &picts[i];
	return NULL;
}

static struct pict *pict_new(uint32_t id)
{
	int i;

	for (i = 0; i < MAXPICT; i++)
		if (!picts[i].id) {
			memset(&picts[i], 0, sizeof(picts[i]));
			picts[i].id = id;
			picts[i].owner = cur_owner;
			return &picts[i];
		}
	fprintf(stderr, "xshim: picture table full (%d)\n", MAXPICT);
	return NULL;
}

static struct gset *gset_find(uint32_t id)
{
	int i;

	for (i = 0; i < MAXGSET; i++)
		if (gsets[i].id == id)
			return &gsets[i];
	return NULL;
}

static void gset_free(struct gset *s)
{
	int i;

	for (i = 0; i < s->ng; i++) {
		mem_glyph -= (size_t)s->g[i].w * s->g[i].h;
		free(s->g[i].a);
	}
	free(s->g);
	memset(s, 0, sizeof(*s));
}

/* Release everything a departing client owns. */
static void render_drop_client(int owner)
{
	int i;

	for (i = 0; i < MAXPICT; i++)
		if (picts[i].id && picts[i].owner == owner)
			memset(&picts[i], 0, sizeof(picts[i]));
	for (i = 0; i < MAXGSET; i++)
		if (gsets[i].id && gsets[i].owner == owner)
			gset_free(&gsets[i]);
}

/* ------------------------------------------------------------ compositing */

/*
 * Blend one pixel. `cov` is 0..255 coverage from the glyph or the source
 * alpha; the destination is RGB565 with no alpha of its own, so OVER reduces
 * to a lerp and SRC to a store. That is the whole compositor: RENDER's other
 * operators exist for layers this display does not have.
 */
static void blend_px(struct res *d, int x, int y, int r8, int g8, int b8,
		     int cov, int op)
{
	struct res *b = d->buf;
	int ax = x + d->ax, ay = y + d->ay;
	uint16_t *p;
	int dr, dg, db;

	if (!b || !b->px)
		return;
	if (ax < d->cx0 || ay < d->cy0 || ax >= d->cx1 || ay >= d->cy1)
		return;
	b->dirty = 1;
	b->hole = 0;
	if (b->bpp == 1) {
		/*
		 * One byte of intensity. This is the same quantity the RGB565
		 * form carried in its red channel, so the operators below would
		 * add nothing an A8 surface can represent - Src and a coverage
		 * blend are the only two that differ, and both reduce to this.
		 */
		uint8_t *q = &((uint8_t *)b->px)[(size_t)ay * b->w + ax];

		switch (op) {
		case PICT_OP_DST:
		case PICT_OP_OVER_REVERSE:
			return;
		case PICT_OP_CLEAR:
		case PICT_OP_OUT:
			*q = 0;
			return;
		default:
			break;
		}
		if (cov <= 0)
			return;
		if (op == PICT_OP_SRC || op == PICT_OP_IN || cov >= 255)
			*q = (uint8_t)r8;
		else
			*q = (uint8_t)(*q + (((r8 - *q) * cov) >> 8));
		return;
	}
	if (b->bpp == 4) {
		/*
		 * ARGB8888 target - the depth-32 visual that st and xfiles
		 * pick - and the one surface here that HAS a destination
		 * alpha, so the RGB565 shortcuts below (Ad = 1 everywhere) are
		 * wrong for it. This was a copy of that table with the alpha
		 * byte written opaque: Clear left opaque black, OverReverse
		 * did nothing, Atop was Over. xfiles builds its sheet from
		 * exactly those - Clear the layers, Over the icons, Atop the
		 * selection colour through an alpha layer, then OverReverse
		 * the background UNDER it all - and the result was a black
		 * sheet with the icon cells painted out.
		 *
		 * Pixels are premultiplied, as RENDER's ARGB32 is: S = C*As
		 * with As = cov (source alpha times mask coverage), and the
		 * general form R = S*Fa + D*Fb from the RENDER spec.
		 */
		uint32_t *q = &((uint32_t *)b->px)[(size_t)ay * b->w + ax];
		int as = cov < 0 ? 0 : cov > 255 ? 255 : cov;
		int sr, sg, sb2, da, ra, rr, rg, rb, fa, fb;

		if (op == PICT_OP_DST)
			return;
		if (op == PICT_OP_CLEAR) {
			*q = 0;
			return;
		}
		if (op == PICT_OP_SRC || (op == PICT_OP_OVER && as >= 255)) {
			*q = ((uint32_t)as << 24) |
			     ((uint32_t)(r8 * as / 255) << 16) |
			     ((uint32_t)(g8 * as / 255) << 8) |
			     (uint32_t)(b8 * as / 255);
			return;
		}
		if (op == PICT_OP_OVER && as == 0)
			return;
		sr = r8 * as / 255; sg = g8 * as / 255; sb2 = b8 * as / 255;
		da = (*q >> 24) & 0xFF;
		dr = (*q >> 16) & 0xFF; dg = (*q >> 8) & 0xFF; db = *q & 0xFF;
		/* Fa and Fb in 0..255 */
		switch (op) {
		case PICT_OP_OVER:		fa = 255;      fb = 255 - as; break;
		case PICT_OP_OVER_REVERSE:	fa = 255 - da; fb = 255;      break;
		case PICT_OP_IN:		fa = da;       fb = 0;        break;
		case PICT_OP_IN_REVERSE:	fa = 0;        fb = as;       break;
		case PICT_OP_OUT:		fa = 255 - da; fb = 0;        break;
		case PICT_OP_OUT_REVERSE:	fa = 0;        fb = 255 - as; break;
		case PICT_OP_ATOP:		fa = da;       fb = 255 - as; break;
		case PICT_OP_ATOP_REVERSE:	fa = 255 - da; fb = as;       break;
		case PICT_OP_XOR:		fa = 255 - da; fb = 255 - as; break;
		case PICT_OP_ADD:		fa = 255;      fb = 255;      break;
		default:			fa = 255;      fb = 255 - as; break;
		}
		ra = (as * fa + da * fb) / 255;
		rr = (sr * fa + dr * fb) / 255;
		rg = (sg * fa + dg * fb) / 255;
		rb = (sb2 * fa + db * fb) / 255;
		if (ra > 255) ra = 255;
		if (rr > 255) rr = 255;
		if (rg > 255) rg = 255;
		if (rb > 255) rb = 255;
		*q = ((uint32_t)ra << 24) | ((uint32_t)rr << 16) |
		     ((uint32_t)rg << 8) | (uint32_t)rb;
		return;
	}
	p = &b->px[(size_t)ay * b->w + ax];

	/*
	 * Every drawable here is RGB565 with no alpha channel, so the
	 * destination is opaque - Ad = 1 - and each operator collapses from the
	 * general Porter-Duff form S*Fa + D*Fb to something much simpler:
	 *
	 *   OverReverse  Fa=1-Ad Fb=1     -> D            (a no-op, NOT Src)
	 *   Out          Fa=1-Ad Fb=0     -> 0
	 *   In           Fa=Ad   Fb=0     -> S
	 *   Atop         Fa=Ad   Fb=1-As  -> same as Over
	 *   AtopReverse  Fa=1-Ad Fb=As    -> D*As
	 *   Xor          Fa=1-Ad Fb=1-As  -> D*(1-As)
	 *
	 * Operator numbering is from X11/extensions/render.h (xorgproto), not
	 * from memory - guessing that 4 was Src is what caused this function to
	 * paint xfiles' sheet black after it had been drawn.
	 *
	 * One deliberate simplification: `cov` serves as both a source alpha
	 * and a glyph/trapezoid mask, and the two want different things from
	 * Src (premultiply S*As versus lerp S*m + D*(1-m)). With no destination
	 * alpha to carry the difference, the mask reading is taken - it is what
	 * the glyph and trapezoid callers mean, and it is what makes a
	 * translucent fill look like a wash rather than a darkening.
	 */
	switch (op) {
	case PICT_OP_DST:
	case PICT_OP_OVER_REVERSE:	/* D + S*(1-Ad) = D */
		return;
	case PICT_OP_CLEAR:
	case PICT_OP_OUT:		/* S*(1-Ad) = 0 */
		*p = 0;
		return;
	default:
		break;
	}
	if (cov <= 0) {
		/* A transparent source still erases where D is scaled by As. */
		if (op == PICT_OP_IN_REVERSE || op == PICT_OP_ATOP_REVERSE ||
		    op == PICT_OP_XOR || op == PICT_OP_OUT_REVERSE)
			return;			/* D*(1-0) = D, or D*0 below */
		return;
	}
	if (op == PICT_OP_SRC || op == PICT_OP_IN ||	/* S*Ad = S */
	    ((op == PICT_OP_OVER || op == PICT_OP_ATOP) && cov >= 255)) {
		*p = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) |
				(b8 >> 3));
		return;
	}
	dr = (*p >> 11) << 3;
	dg = ((*p >> 5) & 0x3F) << 2;
	db = (*p & 0x1F) << 3;

	switch (op) {
	case PICT_OP_IN_REVERSE:	/* D*As */
	case PICT_OP_ATOP_REVERSE:	/* D*As + S*(1-Ad) = D*As */
		dr = dr * cov / 255;
		dg = dg * cov / 255;
		db = db * cov / 255;
		break;
	case PICT_OP_OUT_REVERSE:	/* D*(1-As) */
	case PICT_OP_XOR:		/* S*(1-Ad) + D*(1-As) = D*(1-As) */
		dr = dr * (255 - cov) / 255;
		dg = dg * (255 - cov) / 255;
		db = db * (255 - cov) / 255;
		break;
	case PICT_OP_ADD:
		dr += r8 * cov / 255; if (dr > 255) dr = 255;
		dg += g8 * cov / 255; if (dg > 255) dg = 255;
		db += b8 * cov / 255; if (db > 255) db = 255;
		break;
	default:			/* Over, Atop: S*As + D*(1-As) */
		dr += ((r8 - dr) * cov) >> 8;
		dg += ((g8 - dg) * cov) >> 8;
		db += ((b8 - db) * cov) >> 8;
		break;
	}
	*p = (uint16_t)(((dr & 0xF8) << 8) | ((dg & 0xFC) << 3) | (db >> 3));
}

/* Intersect a picture's clip with a rectangle, in drawable coordinates. */
static void pict_clip(struct pict *pi, int *x0, int *y0, int *x1, int *y1)
{
	if (!pi || !pi->has_clip)
		return;
	if (pi->cx > *x0) *x0 = pi->cx;
	if (pi->cy > *y0) *y0 = pi->cy;
	if (pi->cx + pi->cw < *x1) *x1 = pi->cx + pi->cw;
	if (pi->cy + pi->ch < *y1) *y1 = pi->cy + pi->ch;
}

static void render_fill(struct res *d, struct pict *dp, int x, int y,
			int w, int h, int r8, int g8, int b8, int a8, int op)
{
	if (trace_on())
		fprintf(stderr, "xshim:   fill op=%d %d,%d %dx%d rgba %02x%02x%02x/%02x -> 0x%x bpp %d\n",
			op, x, y, w, h, r8, g8, b8, a8, d->id,
			d->buf ? d->buf->bpp : -1);
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h, i, j;

	pict_clip(dp, &x0, &y0, &x1, &y1);
	if (op == PICT_OP_CLEAR) {
		/*
		 * Zero everything. On RGB565 that is black; on ARGB8888 it is
		 * TRANSPARENT black, which is the whole point of the operator
		 * - xfiles clears its alpha layers with it before drawing.
		 */
		r8 = g8 = b8 = 0;
		a8 = d->buf && d->buf->bpp == 4 ? 0 : 255;
		op = PICT_OP_SRC;
	}
	/*
	 * The measured top cost of an xfiles directory load: the XPM batcher
	 * turns every icon into FillRectangles requests holding hundreds of
	 * short per-colour runs, and each run was painted a pixel at a time
	 * through blend_px - 28 ms per request. An opaque Src/Over fill is a
	 * span write; anything else keeps the per-pixel path.
	 */
	if ((op == PICT_OP_SRC || (op == PICT_OP_OVER && a8 >= 255)) &&
	    d->buf && (d->buf->bpp == 2 || d->buf->bpp == 4) && op_target(d)) {
		uint32_t c = d->buf->bpp == 4 ?
			((uint32_t)a8 << 24) |
				((uint32_t)(r8 * a8 / 255) << 16) |
				((uint32_t)(g8 * a8 / 255) << 8) |
				(uint32_t)(b8 * a8 / 255) :
			(uint32_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) |
				   (b8 >> 3));

		for (j = y0; j < y1; j++)
			px_hspan(d, x0, j, x1 - x0, c);
		xshim_px_acc += (uint64_t)(x1 - x0) * (y1 - y0);
		return;
	}
	for (j = y0; j < y1; j++)
		for (i = x0; i < x1; i++)
			blend_px(d, i, j, r8, g8, b8, a8, op);
	xshim_px_acc += (uint64_t)(x1 - x0) * (y1 - y0);
}

static struct glyph *glyph_find(struct gset *s, uint32_t id)
{
	int i;

	for (i = 0; i < s->ng; i++)
		if (s->g[i].id == id)
			return &s->g[i];
	return NULL;
}


/* ------------------------------------------------------------ trapezoids */

/*
 * Walk a picture's value list.
 *
 * The mask must be walked in FULL even for the values we ignore: each set bit
 * is four bytes, so skipping one misreads every value after it. The bit that
 * matters most is CPClipMask - a client sets a clip for a partial repaint and
 * then clears it with clipMask None, and treating ChangePicture as a no-op
 * leaves the old clip in force for ever. xclock drew its whole face correctly
 * into the top-left corner of the pixmap and nothing anywhere else.
 */
#define CP_REPEAT	(1u << 0)
#define CP_CLIP_X_ORG	(1u << 4)
#define CP_CLIP_Y_ORG	(1u << 5)
#define CP_CLIP_MASK	(1u << 6)

static void pict_values(struct pict *pi, uint32_t mask, const uint8_t *v,
			const uint8_t *end)
{
	int bit;

	for (bit = 0; bit < 13 && v + 4 <= end; bit++) {
		uint32_t m = 1u << bit;

		if (!(mask & m))
			continue;
		switch (m) {
		case CP_REPEAT:
			pi->repeat = (int)get32(v);
			break;
		case CP_CLIP_X_ORG:
			pi->cx += (int16_t)get32(v);
			break;
		case CP_CLIP_Y_ORG:
			pi->cy += (int16_t)get32(v);
			break;
		case CP_CLIP_MASK:
			if (!get32(v))		/* None: no clip at all */
				pi->has_clip = 0;
			break;
		}
		v += 4;
	}
}

static void render_unimpl(struct cli *c, uint8_t minor, const char *name);


/*
 * Anti-aliased trapezoids, which is how RENDER draws vector shapes.
 *
 * Everything Xft and cairo-style code puts on screen that is not a glyph
 * arrives here: xclock's hands and tick marks are trapezoids, and with this
 * unimplemented its render path drew a perfectly correct nothing.
 *
 * A trapezoid is a top and bottom scanline plus a left and a right EDGE, each
 * an arbitrary line - so the left and right boundaries move per scanline. The
 * rasteriser samples NSUB sub-scanlines per pixel row and accumulates exact
 * horizontal coverage for each, which anti-aliases both axes: vertically by
 * how many sub-scanlines fall inside, horizontally by what fraction of each
 * pixel the span covers.
 *
 * Coordinates are 16.16 fixed point, so the arithmetic is integer throughout -
 * this board has single-precision hardware float but no reason to use it here,
 * and 64-bit intermediates keep the edge interpolation exact.
 */
#define NSUB	8			/* sub-scanlines per pixel row */

/* Where a line crosses scanline y. Both in 16.16. */
static int32_t line_x_at(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
			 int32_t y)
{
	if (y2 == y1)
		return x1;
	return x1 + (int32_t)(((int64_t)(x2 - x1) * (y - y1)) / (y2 - y1));
}

/* Add `w` of coverage for the 16.16 span [xl,xr) into a row of bytes. */
static void cov_span(uint8_t *cov, int w_px, int32_t xl, int32_t xr, int w)
{
	int first, last, i;

	if (xr <= xl)
		return;
	if (xl < 0)
		xl = 0;
	if (xr > (int32_t)(w_px << 16))
		xr = (int32_t)(w_px << 16);
	if (xr <= xl)
		return;
	first = xl >> 16;
	last = (xr - 1) >> 16;
	if (first >= w_px)
		return;
	if (last >= w_px)
		last = w_px - 1;
	if (first == last) {
		int a = cov[first] + (int)(((int64_t)w * (xr - xl)) >> 16);

		cov[first] = a > 255 ? 255 : a;
		return;
	}
	{
		int a = cov[first] + (int)(((int64_t)w *
					    (65536 - (xl & 0xFFFF))) >> 16);

		cov[first] = a > 255 ? 255 : a;
	}
	for (i = first + 1; i < last; i++) {
		int a = cov[i] + w;

		cov[i] = a > 255 ? 255 : a;
	}
	{
		int a = cov[last] + (int)(((int64_t)w * (xr & 0xFFFF)) >> 16);

		cov[last] = a > 255 ? 255 : a;
	}
}

static void render_trapezoids(struct cli *c, const uint8_t *r, int len)
{
	struct pict *sp = pict_find(get32(r + 8));
	struct pict *dp = pict_find(get32(r + 12));
	struct res *d;
	const uint8_t *p = r + 24, *end = r + len;   /* len is BYTES */
	int op = r[4];
	static uint8_t cov[XSHIM_W];

	if (!dp)
		return;
	d = res_find(dp->drawable);
	if (!drawable_ok(d))
		return;
	if (!sp || !sp->solid) {
		render_unimpl(c, 10, "Trapezoids from a non-solid source");
		return;
	}
	if (trace_on())
		fprintf(stderr, "xshim:   traps on 0x%x drawable %dx%d clip "
			"%s %d,%d %dx%d  n=%d\n", dp->id, d->w, d->h,
			dp->has_clip ? "yes" : "no", dp->cx, dp->cy,
			dp->cw, dp->ch, (int)((end - p) / 40));
	for (; p + 40 <= end; p += 40) {
		int32_t top = (int32_t)get32(p), bot = (int32_t)get32(p + 4);
		if (0 && trace_on())
			fprintf(stderr, "xshim:   trap y %.2f..%.2f L(%.1f,%.1f)-(%.1f,%.1f) "
				"R(%.1f,%.1f)-(%.1f,%.1f) src=%d,%d,%d,%d\n",
				top / 65536.0, ((int32_t)get32(p + 4)) / 65536.0,
				((int32_t)get32(p + 8)) / 65536.0,
				((int32_t)get32(p + 12)) / 65536.0,
				((int32_t)get32(p + 16)) / 65536.0,
				((int32_t)get32(p + 20)) / 65536.0,
				((int32_t)get32(p + 24)) / 65536.0,
				((int32_t)get32(p + 28)) / 65536.0,
				((int32_t)get32(p + 32)) / 65536.0,
				((int32_t)get32(p + 36)) / 65536.0,
				sp->rr, sp->gg, sp->bb, sp->a);
		int32_t lx1 = (int32_t)get32(p + 8),  ly1 = (int32_t)get32(p + 12);
		int32_t lx2 = (int32_t)get32(p + 16), ly2 = (int32_t)get32(p + 20);
		int32_t rx1 = (int32_t)get32(p + 24), ry1 = (int32_t)get32(p + 28);
		int32_t rx2 = (int32_t)get32(p + 32), ry2 = (int32_t)get32(p + 36);
		int y0 = top >> 16, y1 = (bot + 0xFFFF) >> 16, iy;
		int cx0 = 0, cy0 = 0, cx1 = d->w, cy1 = d->h;

		pict_clip(dp, &cx0, &cy0, &cx1, &cy1);
		if (y0 < cy0)
			y0 = cy0;
		if (y1 > cy1)
			y1 = cy1;
		for (iy = y0; iy < y1; iy++) {
			int sub, any = 0, x;

			memset(cov, 0, (size_t)(cx1 > 0 ? cx1 : 0));
			for (sub = 0; sub < NSUB; sub++) {
				int32_t sy = ((int32_t)iy << 16) +
					(int32_t)((65536 * (2 * sub + 1)) /
						  (2 * NSUB));
				int32_t xl, xr;

				if (sy < top || sy >= bot)
					continue;
				xl = line_x_at(lx1, ly1, lx2, ly2, sy);
				xr = line_x_at(rx1, ry1, rx2, ry2, sy);
				if (xr <= xl)
					continue;
				cov_span(cov, cx1, xl, xr, 255 / NSUB);
				any = 1;
			}
			if (!any)
				continue;
			for (x = cx0; x < cx1; x++)
				if (cov[x])
					blend_px(d, x, iy, sp->rr, sp->gg,
						 sp->bb,
						 cov[x] * sp->a / 255, op);
		}
	}
	notify_draw(d);
}

/* ------------------------------------------------------------- the requests */

/*
 * Advertised formats. Xft looks these up by template - depth plus every shift
 * and mask - so an approximation is not a smaller version of this, it is a
 * client that exits saying it cannot find a format.
 */
static void put_format(uint8_t *p, uint32_t id, int depth, int rs, int rm,
		       int gs, int gm, int bs, int bm, int as, int am)
{
	memset(p, 0, 28);
	put32(p, id);
	/*
	 * PictTypeDirect is 1. Writing 0 here is PictTypeIndexed, and the
	 * failure is asymmetric in a way that hides it: looking a format up by
	 * ID still works, so XRenderFindVisualFormat() succeeds, while
	 * XRenderFindStandardFormat() - which matches a TEMPLATE including the
	 * type - finds nothing, so Xft cannot get its A8 mask format and every
	 * XftFontOpen returns NULL.
	 */
	p[4] = 1;			/* PictTypeDirect */
	p[5] = (uint8_t)depth;
	put16(p + 8, rs);  put16(p + 10, rm);
	put16(p + 12, gs); put16(p + 14, gm);
	put16(p + 16, bs); put16(p + 18, bm);
	put16(p + 20, as); put16(p + 22, am);
}

static void render_unimpl(struct cli *c, uint8_t minor, const char *name)
{
	/*
	 * Its OWN counter. Sharing nrender[] with the dispatcher's
	 * first-request logger meant the count was always non-zero by the time
	 * this ran, so every unimplemented RENDER path reported NOTHING - and
	 * a search for refused requests came back clean while xfiles drew a
	 * black window. An instrument that cannot fire is worse than none.
	 */
	static uint16_t seen[64];

	(void)c;
	if (seen[minor & 63]++ == 0)
		fprintf(stderr, "xshim: RENDER %s (minor %u) is not "
			"implemented - the client will be told BadRequest\n",
			name, minor);
}

/* Un-premultiply an xRenderColor into 8-bit channels the blender can use. */
static void render_color(const uint8_t *p, int *r, int *g, int *b, int *a)
{
	int A = get16(p + 6) >> 8;

	*r = get16(p + 0) >> 8;
	*g = get16(p + 2) >> 8;
	*b = get16(p + 4) >> 8;
	*a = A;
	if (A > 0 && A < 255) {
		*r = *r * 255 / A; if (*r > 255) *r = 255;
		*g = *g * 255 / A; if (*g > 255) *g = 255;
		*b = *b * 255 / A; if (*b > 255) *b = 255;
	}
}

static void render_glyphs(struct cli *c, const uint8_t *r, int len, int idsize)
{
	struct pict *sp = pict_find(get32(r + 8));
	struct pict *dp = pict_find(get32(r + 12));
	struct gset *gs = gset_find(get32(r + 20));
	struct res *d;
	const uint8_t *p = r + 28, *end = r + len;   /* len is BYTES */
	int op = r[4], x = 0, y = 0;

	if (!dp || !gs) {
		send_error(c, X_BAD_VALUE, 0, RENDER_MAJOR);
		return;
	}
	d = res_find(dp->drawable);
	if (!drawable_ok(d))
		return;
	/*
	 * Xft always paints text through a SOLID source picture - the colour -
	 * with the glyph as the mask. A non-solid source would mean textured
	 * text, which nothing here asks for.
	 */
	if (!sp || !sp->solid) {
		render_unimpl(c, 23, "CompositeGlyphs from a non-solid source");
		return;
	}
	while (p + 8 <= end) {
		int n = p[0], i;

		if (n == 255) {			/* switch glyph set */
			gs = gset_find(get32(p + 4));
			p += 8;
			if (!gs)
				return;
			continue;
		}
		x += gets16(p + 4);
		y += gets16(p + 6);
		p += 8;
		for (i = 0; i < n && p + idsize <= end; i++, p += idsize) {
			uint32_t id = idsize == 1 ? p[0] :
				      idsize == 2 ? get16(p) : get32(p);
			struct glyph *g = glyph_find(gs, id);
			int gx, gy;

			if (!g)
				continue;
			for (gy = 0; gy < g->h; gy++)
				for (gx = 0; gx < g->w; gx++) {
					int cov = g->a[gy * g->w + gx];
					int x0 = x - g->ox + gx;
					int y0 = y - g->oy + gy;
					int cl0 = x0, cl1 = x0 + 1;
					int ct0 = y0, ct1 = y0 + 1;

					pict_clip(dp, &cl0, &ct0, &cl1, &ct1);
					if (cl0 >= cl1 || ct0 >= ct1)
						continue;
					blend_px(d, x0, y0, sp->rr, sp->gg,
						 sp->bb,
						 cov * sp->a / 255, op);
				}
			x += g->ax;
			y += g->ay;
		}
		p = r + ((p - r + 3) & ~3);	/* ids are padded to 4 bytes */
	}
	notify_draw(d);
}

static void render_add_glyphs(struct cli *c, const uint8_t *r, int len)
{
	struct gset *s = gset_find(get32(r + 4));
	uint32_t n = get32(r + 8);
	const uint8_t *ids = r + 12, *info, *img, *end = r + len;
	int bpp = 8;
	uint32_t i;

	if (!s) {
		send_error(c, X_BAD_VALUE, 0, RENDER_MAJOR);
		return;
	}
	if (s->format == PF_A1)
		bpp = 1;
	info = ids + n * 4;
	img = info + n * 12;
	if (s->ng + (int)n > s->cap) {
		int cap = s->cap ? s->cap * 2 : 64;
		struct glyph *g;

		while (cap < s->ng + (int)n)
			cap *= 2;
		g = realloc(s->g, (size_t)cap * sizeof(*g));
		if (!g)
			return;
		s->g = g;
		s->cap = cap;
	}
	for (i = 0; i < n && img <= end; i++) {
		const uint8_t *gi = info + i * 12;
		struct glyph *g = &s->g[s->ng];
		int w = get16(gi), h = get16(gi + 2);
		int stride = bpp == 1 ? ((w + 31) / 32) * 4 : (w + 3) & ~3;
		int gx, gy;

		if (w <= 0 || h <= 0 || img + (size_t)stride * h > end) {
			img += (size_t)stride * h;
			continue;
		}
		memset(g, 0, sizeof(*g));
		g->id = get32(ids + i * 4);
		g->w = w; g->h = h;
		g->ox = gets16(gi + 4);
		g->oy = gets16(gi + 6);
		g->ax = gets16(gi + 8);
		g->ay = gets16(gi + 10);
		g->a = malloc((size_t)w * h);
		if (!g->a)
			return;
		mem_glyph += (size_t)w * h;
		for (gy = 0; gy < h; gy++)
			for (gx = 0; gx < w; gx++) {
				const uint8_t *row = img + (size_t)gy * stride;

				g->a[gy * w + gx] = bpp == 1 ?
					((row[gx >> 3] >> (gx & 7)) & 1) * 255 :
					row[gx];
			}
		img += (size_t)stride * h;
		s->ng++;
	}
}

/*
 * Copy or blend one picture onto another. The only sources that arrive here
 * are a solid fill (which is a rectangle) and another RGB565 drawable of ours
 * (which is a copy) - anything else says so rather than painting nonsense.
 */
static void render_composite(struct cli *c, const uint8_t *r)
{
	struct pict *sp = pict_find(get32(r + 8));
	struct pict *mp = pict_find(get32(r + 12));
	struct pict *dp = pict_find(get32(r + 16));
	struct res *d, *s;
	int op = r[4];
	int sx = gets16(r + 20), sy = gets16(r + 22);
	int mask_x = gets16(r + 24), mask_y = gets16(r + 26);
	int dx = gets16(r + 28), dy = gets16(r + 30);
	int w = get16(r + 32), h = get16(r + 34);
	int i, j, x0, y0, x1, y1, wrote = 0;

	if (!dp)
		return;
	d = res_find(dp->drawable);
	if (!drawable_ok(d))
		return;
	/*
	 * A mask picture: dst = src*a + dst*(1-a), with the coverage coming
	 * from a third surface. This is how an icon with transparency is
	 * drawn, and ignoring the mask is why xfiles composited its file list
	 * into a background pixmap that stayed black.
	 *
	 * The mask is nominally A8, but every drawable here is RGB565 - there
	 * is no 8-bit surface to read - so coverage is taken from the red
	 * channel, which is monotonic in what a client writes: 0xFFFF opaque,
	 * 0 transparent, and anything between in proportion.
	 *
	 * HARDWARE: the PPA BLEND engine does exactly this operation and is
	 * implemented but unused (docs/accel-plan.md). It is NOT used here
	 * because of the size: programming the PPA costs a fixed ~13 us and it
	 * only overtakes the CPU above ~128 KB, while these composites are
	 * icon-sized. The sizes are logged below so the crossover can be
	 * checked against real traffic rather than assumed.
	 */
	if (mp) {
		/*
		 * A SOLID mask has no drawable: it is a constant alpha, used
		 * to draw something uniformly translucent. Rejecting it as
		 * "unreadable" is what left xfiles' window black even after
		 * masked compositing was implemented.
		 */
		struct res *m = mp->solid ? NULL : res_find(mp->drawable);
		int x0, y0, x1, y1, i, j;

		if (!sp || (!mp->solid && !drawable_ok(m))) {
			if (trace_on())
			fprintf(stderr, "xshim: masked composite: src 0x%x=%s "
				"mask 0x%x=%s dst 0x%x=%s\n",
				get32(r + 8), sp ? "ok" : "MISSING",
				get32(r + 12), mp ? (mp->solid ? "solid" :
				(drawable_ok(m) ? "ok" : "no drawable")) :
				"MISSING",
				get32(r + 16), dp ? "ok" : "MISSING");
			render_unimpl(c, 8, "Composite with an unreadable mask");
			return;
		}
		if (trace_on())
			fprintf(stderr, "xshim:   masked composite %dx%d "
				"(%d bytes) src=%s\n", w, h, w * h * 2,
				sp->solid ? "solid" : "picture");
		/*
		 * A mask that is known to be all zero (fresh, or a punched
		 * whole-surface clear) makes these operators no-ops. Skipping
		 * the read is what keeps the punched pages punched: a read
		 * fault on shared memory allocates, so compositing through the
		 * mask would silently refill the 705 kB it just freed.
		 */
		if (!mp->solid && m->buf && m->buf->hole &&
		    (op == PICT_OP_OVER || op == PICT_OP_ADD ||
		     op == PICT_OP_ATOP || op == PICT_OP_OVER_REVERSE ||
		     op == PICT_OP_DST))
			return;
		x0 = dx; y0 = dy; x1 = dx + w; y1 = dy + h;
		pict_clip(dp, &x0, &y0, &x1, &y1);

		/*
		 * The measured heart of xfiles' lag: one directory navigation
		 * was ~3.6 s of handle() time, nearly all of it this loop at
		 * per-pixel px_get + blend_px over icon sheets whose masks
		 * are almost entirely 0 (transparent gaps) or 255 (opaque
		 * icon body). The run below keeps the generic loop as the
		 * fallback and takes Over with a real A8/A1 mask and a real
		 * RGB565 source through row pointers: coverage 0 skips with
		 * one compare, 255 is a store, and only antialiased edges
		 * pay the blend arithmetic (identical to blend_px's).
		 */
		if (!mp->solid && sp && !sp->solid &&
		    (op == PICT_OP_OVER || op == PICT_OP_ATOP) &&
		    d->buf && d->buf->bpp == 2 && m->buf &&
		    m->buf->bpp != 4 && op_target(d)) {
			struct res *ss = res_find(sp->drawable);

			if (drawable_ok(ss) && ss->buf &&
			    ss->buf->bpp == 2) {
				struct res *db = d->buf, *sb = ss->buf,
					   *mb = m->buf;

				for (j = y0; j < y1; j++) {
					int my = mask_y + (j - dy);
					int py = sy + (j - dy);
					int ry = j + d->ay;
					int i0 = x0, i1 = x1, i2;
					uint16_t *drow;
					const uint16_t *srow;

					if (my < 0 || my >= m->h ||
					    py < 0 || py >= ss->h ||
					    ry < d->cy0 || ry >= d->cy1 ||
					    my + m->ay < 0 ||
					    py + ss->ay < 0)
						continue;
					/* clip i so every index below is in
					 * range for dst, src and mask */
					if (i0 + d->ax < d->cx0)
						i0 = d->cx0 - d->ax;
					if (i1 + d->ax > d->cx1)
						i1 = d->cx1 - d->ax;
					if (mask_x + (i0 - dx) < 0)
						i0 = dx - mask_x;
					if (mask_x + (i1 - dx) > m->w)
						i1 = dx - mask_x + m->w;
					if (sx + (i0 - dx) < 0)
						i0 = dx - sx;
					if (sx + (i1 - dx) > ss->w)
						i1 = dx - sx + ss->w;
					if (i0 >= i1)
						continue;
					drow = db->px + (size_t)ry * db->w +
					       d->ax;
					srow = sb->px +
					       (size_t)(py + ss->ay) * sb->w +
					       ss->ax + (sx - dx);
					/*
					 * Hoist the mask row out of the pixel
					 * loop, then take the shape masks in
					 * RUNS. An icon's mask is nearly all
					 * 0 or all 255 - transparent surround
					 * and opaque body - so per-pixel
					 * coverage arithmetic is paid almost
					 * entirely on pixels that need none of
					 * it. Skipping a transparent run and
					 * memcpy'ing an opaque one leaves only
					 * the antialiased edge on the slow
					 * path. Byte masks only (A8 and
					 * depth-1); a 16-bit mask keeps the
					 * generic loop below.
					 */
					{
					size_t mbase = (size_t)(my + m->ay) *
						       mb->w + m->ax +
						       mask_x - dx;

					if (mb->bpp == 2 && m->depth != 1) {
						/*
						 * Same runs, 16-bit mask.
						 * Coverage comes from the red
						 * channel, so full coverage is
						 * (v >> 11) == 31 and none is
						 * (v >> 11) == 0.
						 */
						const uint16_t *mrow =
						  mb->px + mbase;

						i2 = i0;
						while (i2 < i1) {
							int st;

							while (i2 < i1 &&
							       (mrow[i2] >> 11)
							       == 0)
								i2++;
							st = i2;
							while (i2 < i1 &&
							       (mrow[i2] >> 11)
							       == 31)
								i2++;
							if (i2 > st) {
								size_t nb =
								  (size_t)
								  (i2 - st) * 2;
								if (sb != db)
									memcpy(&drow[st],
									       &srow[st],
									       nb);
								else
									memmove(&drow[st],
										&srow[st],
										nb);
							}
							if (i2 < i1 &&
							    (mrow[i2] >> 11)) {
								int cov =
								  (((mrow[i2] >> 11)
								    & 0x1F) * 255)
								  / 31;
								uint16_t v =
								  srow[i2];
								uint16_t *pd =
								  &drow[i2];
								int sr = (v >> 11) << 3;
								int sg = ((v >> 5) & 0x3F) << 2;
								int sb8 = (v & 0x1F) << 3;
								int dr = (*pd >> 11) << 3;
								int dg = ((*pd >> 5) & 0x3F) << 2;
								int db8 = (*pd & 0x1F) << 3;

								dr += ((sr - dr) * cov) >> 8;
								dg += ((sg - dg) * cov) >> 8;
								db8 += ((sb8 - db8) * cov) >> 8;
								*pd = (uint16_t)
								  (((dr & 0xF8) << 8) |
								   ((dg & 0xFC) << 3) |
								   (db8 >> 3));
								i2++;
							}
						}
						xshim_px_acc += (uint64_t)
								(i1 - i0);
						continue;
					}
					if (mb->bpp == 1) {
						const uint8_t *mrow =
						  (const uint8_t *)mb->px +
						  mbase;
						int bin = m->depth == 1;

						i2 = i0;
						while (i2 < i1) {
							int st;

							while (i2 < i1 &&
							       !mrow[i2])
								i2++;
							st = i2;
							while (i2 < i1 &&
							       (bin ? mrow[i2]
								: mrow[i2] ==
								  255))
								i2++;
							if (i2 > st) {
								size_t nb =
								  (size_t)
								  (i2 - st) * 2;
								if (sb != db)
									memcpy(&drow[st],
									       &srow[st],
									       nb);
								else
									memmove(&drow[st],
										&srow[st],
										nb);
							}
							/* partial coverage:
							 * one pixel, then look
							 * for the next run */
							if (i2 < i1 &&
							    mrow[i2]) {
								int cov =
								  mrow[i2];
								uint16_t v =
								  srow[i2];
								uint16_t *pd =
								  &drow[i2];
								int sr = (v >> 11) << 3;
								int sg = ((v >> 5) & 0x3F) << 2;
								int sb8 = (v & 0x1F) << 3;
								int dr = (*pd >> 11) << 3;
								int dg = ((*pd >> 5) & 0x3F) << 2;
								int db8 = (*pd & 0x1F) << 3;

								dr += ((sr - dr) * cov) >> 8;
								dg += ((sg - dg) * cov) >> 8;
								db8 += ((sb8 - db8) * cov) >> 8;
								*pd = (uint16_t)
								  (((dr & 0xF8) << 8) |
								   ((dg & 0xFC) << 3) |
								   (db8 >> 3));
								i2++;
							}
						}
						xshim_px_acc += (uint64_t)
								(i1 - i0);
						continue;
					}
					}
					for (i2 = i0; i2 < i1; i2++) {
						int mxx = mask_x + (i2 - dx);
						int raw, cov;

						if (mb->bpp == 1)
							raw = ((const uint8_t *)
							  mb->px)[(size_t)
							  (my + m->ay) *
							  mb->w + mxx + m->ax];
						else
							raw = mb->px[
							  (size_t)(my + m->ay) *
							  mb->w + mxx + m->ax];
						/*
						 * depth-1 stores 0/1 whatever
						 * the byte width - using the
						 * raw byte as coverage made an
						 * A1 mask nearly transparent.
						 */
						cov = m->depth == 1 ?
						      (raw ? 255 : 0) :
						      mb->bpp == 1 ? raw :
						      ((((raw) >> 11) & 0x1F)
						       * 255) / 31;
						if (!cov)
							continue;
						if (cov >= 255) {
							drow[i2] = srow[i2];
							continue;
						}
						{
						uint16_t v = srow[i2];
						uint16_t *pd = &drow[i2];
						int sr = (v >> 11) << 3;
						int sg = ((v >> 5) & 0x3F) << 2;
						int sb8 = (v & 0x1F) << 3;
						int dr = (*pd >> 11) << 3;
						int dg = ((*pd >> 5) & 0x3F) << 2;
						int db8 = (*pd & 0x1F) << 3;

						dr += ((sr - dr) * cov) >> 8;
						dg += ((sg - dg) * cov) >> 8;
						db8 += ((sb8 - db8) * cov) >> 8;
						*pd = (uint16_t)
						  (((dr & 0xF8) << 8) |
						   ((dg & 0xFC) << 3) |
						   (db8 >> 3));
						}
					}
				}
				xshim_px_acc += (uint64_t)(x1 - x0) *
						(y1 - y0);
				damage_add(d, dx, dy, w, h);
				notify_draw(d);
				return;
			}
		}
		if (xsp_on > 0) {
			static uint64_t lastc;
			uint64_t nowc = xsp_now();

			if (nowc - lastc > 1000000000ull) {
				lastc = nowc;
				fprintf(stderr, "xsp: SLOW-COMP op=%d %dx%d "
					"msolid=%d ssolid=%d mbpp=%d mdepth=%d"
					" dbpp=%d\n", op, w, h,
					mp->solid, sp ? sp->solid : -1,
					m && m->buf ? m->buf->bpp : -1,
					m ? m->depth : -1,
					d->buf ? d->buf->bpp : -1);
			}
		}
		for (j = y0; j < y1; j++)
			for (i = x0; i < x1; i++) {
				int mx = mask_x + (i - dx);
				int my = mask_y + (j - dy);
				uint32_t mv = 0;
				int cov, sr, sg, sb;

				if (mp->solid) {
					cov = mp->a;
				} else {
					if (mx < 0 || my < 0 || mx >= m->w ||
					    my >= m->h)
						continue;
					mv = px_get(m, mx, my);
					cov = m->depth == 1 ? (mv ? 255 : 0) :
					      m->bpp == 1 ? mv :
					      (m->buf && m->buf->bpp == 4) ?
					      (mv >> 24) & 0xFF :
					      (((mv >> 11) & 0x1F) * 255) / 31;
				}
				if (!cov)
					continue;
				if (sp->solid) {
					sr = sp->rr; sg = sp->gg; sb = sp->bb;
					cov = cov * sp->a / 255;
				} else {
					struct res *ss =
						res_find(sp->drawable);
					int px = sx + (i - dx);
					int py = sy + (j - dy);
					uint32_t v;

					if (!drawable_ok(ss) || px < 0 ||
					    py < 0 || px >= ss->w ||
					    py >= ss->h)
						continue;
					v = px_get(ss, px, py);
					if (ss->buf && ss->buf->bpp == 4) {
						/*
						 * ARGB8888 source (a pixmap
						 * made for the depth-32
						 * visual): its own alpha
						 * scales the coverage, as
						 * Over requires.
						 */
						int va = (v >> 24) & 0xFF;

						if (!va)
							continue;
						/* stored premultiplied; blend_px
						 * multiplies by cov itself */
						sr = ((v >> 16) & 0xFF) * 255 / va;
						sg = ((v >> 8) & 0xFF) * 255 / va;
						sb = (v & 0xFF) * 255 / va;
						cov = cov * va / 255;
						if (!cov)
							continue;
					} else {
						sr = (v >> 11) << 3;
						sg = ((v >> 5) & 0x3F) << 2;
						sb = (v & 0x1F) << 3;
					}
				}
				blend_px(d, i, j, sr, sg, sb, cov, op);
			}
		damage_add(d, dx, dy, w, h);
		notify_draw(d);
		return;
	}
	if (sp && sp->solid) {
		render_fill(d, dp, dx, dy, w, h, sp->rr, sp->gg, sp->bb,
			    sp->a, op);
		damage_add(d, dx, dy, w, h);
		notify_draw(d);
		return;
	}
	s = sp ? res_find(sp->drawable) : NULL;
	if (!drawable_ok(s)) {
		render_unimpl(c, 8, "Composite from an unknown source");
		return;
	}
	/*
	 * Same reduction as the fills: an opaque destination makes
	 * OverReverse and Dst no-ops, and a 565 source has no alpha, so Over
	 * IS Src - a row copy. xfiles' sheet-to-background composite is
	 * 600x460 of exactly this, and it was going through blend_px.
	 */
	if (op == PICT_OP_DST || op == PICT_OP_OVER_REVERSE)
		return;
	x0 = dx; y0 = dy; x1 = dx + w; y1 = dy + h;
	pict_clip(dp, &x0, &y0, &x1, &y1);
	if ((op == PICT_OP_OVER || op == PICT_OP_ATOP ||
	     op == PICT_OP_SRC) && !sp->repeat &&
	    d->buf && d->buf->bpp == 2 && s->buf && s->buf->bpp == 2 &&
	    op_target(d)) {
		struct res *db = d->buf, *sb = s->buf;

		for (j = y0; j < y1; j++) {
			int py = sy + (j - dy);
			int ry = j + d->ay;
			int i0 = x0, i1 = x1;

			if (py < 0 || py >= s->h || ry < d->cy0 ||
			    ry >= d->cy1)
				continue;
			if (i0 + d->ax < d->cx0)
				i0 = d->cx0 - d->ax;
			if (i1 + d->ax > d->cx1)
				i1 = d->cx1 - d->ax;
			if (sx + (i0 - dx) < 0)
				i0 = dx - sx;
			if (sx + (i1 - dx) > s->w)
				i1 = dx - sx + s->w;
			if (i0 >= i1)
				continue;
			memcpy(db->px + (size_t)ry * db->w + d->ax + i0,
			       sb->px + (size_t)(py + s->ay) * sb->w +
			       s->ax + sx + (i0 - dx),
			       (size_t)(i1 - i0) * 2);
		}
		damage_add(d, dx, dy, w, h);
		notify_draw(d);
		return;
	}
	damage_add(d, dx, dy, w, h);
	notify_draw(d);
	if (trace_on())
		fprintf(stderr, "xshim:   composite op=%d %dx%d src(%d,%d) "
			"dst(%d,%d) -> clipped %d,%d..%d,%d  src %dx%d\n",
			op, w, h, sx, sy, dx, dy, x0, y0, x1, y1, s->w, s->h);
	for (j = y0; j < y1; j++)
		for (i = x0; i < x1; i++) {
			int px = sx + (i - dx), py = sy + (j - dy);
			uint16_t v;

			if (sp->repeat) {
				px %= s->w ? s->w : 1;
				py %= s->h ? s->h : 1;
			}
			if (px < 0 || py < 0 || px >= s->w || py >= s->h)
				continue;
			{
				size_t o = (size_t)(py + s->ay) * s->buf->w +
					   px + s->ax;

				if (s->buf->bpp == 1) {
					/* 8-bit source: one intensity, not a
					 * packed colour. */
					int g = ((const uint8_t *)
						 s->buf->px)[o];

					blend_px(d, i, j, g, g, g, 255,
						 PICT_OP_SRC);
					wrote++;
					continue;
				}
				if (s->buf->bpp == 4) {
					/*
					 * A premultiplied ARGB source keeps
					 * its own alpha and the request's
					 * operator. Forcing Src at full
					 * coverage - as this did - stamped
					 * the transparent surround of
					 * xfiles' icon layer onto its sheet
					 * as opaque black.
					 */
					uint32_t a = ((const uint32_t *)
						      s->buf->px)[o];
					int va = (a >> 24) & 0xFF;

					if (!va && op != PICT_OP_SRC) {
						continue;
					}
					blend_px(d, i, j,
						 va ? ((a >> 16) & 0xFF) * 255 / va : 0,
						 va ? ((a >> 8) & 0xFF) * 255 / va : 0,
						 va ? (a & 0xFF) * 255 / va : 0,
						 va, op);
					wrote++;
					continue;
				}
				v = s->buf->px[o];
			}
			blend_px(d, i, j, (v >> 11) << 3, ((v >> 5) & 0x3F) << 2,
				 (v & 0x1F) << 3, 255, op);
			wrote++;
		}
	if (trace_on())
		fprintf(stderr, "xshim:   composite wrote %d px into 0x%x\n",
			wrote, d->id);
}

static const char *render_opstr(uint8_t m)
{
	static const char *n[] = {
		[0] = "QueryVersion", [1] = "QueryPictFormats",
		[2] = "QueryPictIndexValues", [4] = "CreatePicture",
		[5] = "ChangePicture", [6] = "SetPictureClipRectangles",
		[7] = "FreePicture", [8] = "Composite", [10] = "Trapezoids",
		[11] = "Triangles", [17] = "CreateGlyphSet",
		[18] = "ReferenceGlyphSet", [19] = "FreeGlyphSet",
		[20] = "AddGlyphs", [22] = "FreeGlyphs",
		[23] = "CompositeGlyphs8", [24] = "CompositeGlyphs16",
		[25] = "CompositeGlyphs32", [26] = "FillRectangles",
		[27] = "CreateCursor", [33] = "CreateSolidFill",
		[34] = "CreateAnimCursor",
	};

	return m < sizeof(n) / sizeof(n[0]) && n[m] ? n[m] : "?";
}

/* Returns 1 if the request was handled, 0 to fall through to an error. */
static int render_request(struct cli *c, const uint8_t *r, int len)
{
	uint8_t minor = r[1];

	/*
	 * Say what a client asks for the first time it asks. RENDER is where
	 * a client that "just exits" is now most likely to be failing, and
	 * the sequence it got through before giving up is the whole diagnosis.
	 */
	if (minor < 64 && c->nrender[minor]++ == 0)
		fprintf(stderr, "xshim: RENDER %s (minor %u)\n",
			render_opstr(minor), minor);
	if (trace_on()) {
		struct pict *tp = NULL;

		switch (minor) {
		case 5: case 6: case 7:
			tp = pict_find(get32(r + 4)); break;
		case 8: case 10: case 23: case 24: case 25:
			tp = pict_find(get32(r + 12)); break;
		case 26:
			tp = pict_find(get32(r + 8)); break;
		}
		if (minor == 8 || minor == 26) {
			struct pict *dq = pict_find(get32(r + (minor == 8 ?
							     16 : 8)));
			struct res *dd = dq ? res_find(dq->drawable) : NULL;

			fprintf(stderr, "xshim:  R %s -> pict 0x%x drawable "
				"0x%x (%s %dx%d)\n", render_opstr(minor),
				dq ? dq->id : 0, dq ? dq->drawable : 0,
				dd ? (dd->type == R_WINDOW ? "window" :
				      "pixmap") : "?",
				dd ? dd->w : 0, dd ? dd->h : 0);
		}
		fprintf(stderr, "xshim:  R %-22s dst=0x%x clip=%s %d,%d %dx%d\n",
			render_opstr(minor), tp ? tp->id : 0,
			tp && tp->has_clip ? "YES" : "no",
			tp ? tp->cx : 0, tp ? tp->cy : 0,
			tp ? tp->cw : 0, tp ? tp->ch : 0);
	}

	switch (minor) {
	case 0: {					/* QueryVersion */
		uint8_t d24[24];

		memset(d24, 0, sizeof(d24));
		put32(d24, 0);				/* major 0 */
		put32(d24 + 4, 11);			/* minor 11 */
		send_reply(c, 0, d24, NULL, 0);
		return 1;
	}
	case 1: {					/* QueryPictFormats */
		uint8_t d24[24], ext[4 * 28 + 8 + (8 + 8) * 2 + 4];
		uint8_t *p = ext;

		memset(d24, 0, sizeof(d24));
		memset(ext, 0, sizeof(ext));
		put32(d24 + 0, 4);			/* numFormats */
		put32(d24 + 4, 1);			/* numScreens */
		put32(d24 + 8, 2);			/* numDepths */
		put32(d24 + 12, 2);			/* numVisuals */
		put32(d24 + 16, 1);			/* numSubpixel */

		put_format(p, PF_RGB565, 16, 11, 0x1F, 5, 0x3F, 0, 0x1F, 0, 0);
		p += 28;
		put_format(p, PF_ARGB32, 32, 16, 0xFF, 8, 0xFF, 0, 0xFF,
			   24, 0xFF);
		p += 28;
		put_format(p, PF_A8, 8, 0, 0, 0, 0, 0, 0, 0, 0xFF);
		p += 28;
		put_format(p, PF_A1, 1, 0, 0, 0, 0, 0, 0, 0, 0x01);
		p += 28;

		/*
		 * Both visuals the setup advertises, each bound to its format.
		 * The depth-32 entry was missing after the depth-32 visual was
		 * added for st (2026-09-07): xfiles asks XMatchVisualInfo for
		 * 32/TrueColor, got it, then XRenderFindVisualFormat() found
		 * no format for it and the client exited - "could not find
		 * XRender visual format" - where before it had fallen back to
		 * the default visual. The client library's screen tables must
		 * carry the same visual (xlite), or libXrender cannot resolve
		 * the id it is told here.
		 */
		put32(p, 2); put32(p + 4, PF_RGB565);	/* screen: 2 depths */
		p += 8;
		p[0] = 16; p[1] = 0; put16(p + 2, 1); put32(p + 4, 0);
		p += 8;					/* depth 16, 1 visual */
		put32(p, VISUAL_ID); put32(p + 4, PF_RGB565);
		p += 8;
		p[0] = 32; p[1] = 0; put16(p + 2, 1); put32(p + 4, 0);
		p += 8;					/* depth 32, 1 visual */
		put32(p, VISUAL32_ID); put32(p + 4, PF_ARGB32);
		p += 8;
		put32(p, 0);				/* SubPixelUnknown */
		p += 4;
		send_reply(c, 0, d24, ext, p - ext);
		return 1;
	}
	case 4: {					/* CreatePicture */
		struct pict *pi = pict_new(get32(r + 4));

		if (!pi)
			return 1;
		pi->drawable = get32(r + 8);
		pi->format = get32(r + 12);
		pict_values(pi, get32(r + 16), r + 20, r + len);
		return 1;
	}
	case 5: {					/* ChangePicture */
		struct pict *pi = pict_find(get32(r + 4));

		if (pi)
			pict_values(pi, get32(r + 8), r + 12, r + len);
		return 1;
	}
	case 6: {					/* SetPictureClipRectangles */
		struct pict *pi = pict_find(get32(r + 4));
		int ox = gets16(r + 8), oy = gets16(r + 10);
		const uint8_t *p = r + 12, *end = r + len;

		if (!pi)
			return 1;
		pi->has_clip = 0;
		if (trace_on())
			fprintf(stderr, "xshim:   setclip 0x%x org %d,%d "
				"nrect %d\n", pi->id, ox, oy,
				(int)((end - p) / 8));
		for (; p + 8 <= end; p += 8) {
			int x = ox + gets16(p), y = oy + gets16(p + 2);
			int w = get16(p + 4), h = get16(p + 6);

			if (!pi->has_clip) {
				pi->cx = x; pi->cy = y;
				pi->cw = w; pi->ch = h;
				pi->has_clip = 1;
			} else {		/* the union, so nothing is lost */
				int x1 = pi->cx + pi->cw, y1 = pi->cy + pi->ch;

				if (x < pi->cx) pi->cx = x;
				if (y < pi->cy) pi->cy = y;
				if (x + w > x1) x1 = x + w;
				if (y + h > y1) y1 = y + h;
				pi->cw = x1 - pi->cx;
				pi->ch = y1 - pi->cy;
			}
		}
		return 1;
	}
	case 7: {					/* FreePicture */
		struct pict *pi = pict_find(get32(r + 4));

		if (pi)
			memset(pi, 0, sizeof(*pi));
		return 1;
	}
	case 8:						/* Composite */
		render_composite(c, r);
		return 1;
	case 10:					/* Trapezoids */
		render_trapezoids(c, r, len);
		return 1;
	case 17: {					/* CreateGlyphSet */
		int i;

		for (i = 0; i < MAXGSET; i++)
			if (!gsets[i].id) {
				memset(&gsets[i], 0, sizeof(gsets[i]));
				gsets[i].id = get32(r + 4);
				gsets[i].format = get32(r + 8);
				gsets[i].owner = cur_owner;
				return 1;
			}
		fprintf(stderr, "xshim: glyph-set table full (%d)\n", MAXGSET);
		return 1;
	}
	case 19: {					/* FreeGlyphSet */
		struct gset *s = gset_find(get32(r + 4));

		if (s)
			gset_free(s);
		return 1;
	}
	case 20:					/* AddGlyphs */
		render_add_glyphs(c, r, len);
		return 1;
	case 22:					/* FreeGlyphs */
		return 1;			/* the set is freed as a whole */
	case 27:					/* CreateCursor */
	case 34:					/* CreateAnimCursor */
		/*
		 * Accepted and ignored: lvdesk draws the pointer itself, so a
		 * client's cursor is never shown. The identifier it allocated
		 * stays valid, which is what XDefineCursor needs afterwards.
		 */
		return 1;
	case 23:					/* CompositeGlyphs8 */
		render_glyphs(c, r, len, 1);
		return 1;
	case 24:					/* CompositeGlyphs16 */
		render_glyphs(c, r, len, 2);
		return 1;
	case 25:					/* CompositeGlyphs32 */
		render_glyphs(c, r, len, 4);
		return 1;
	case 26: {					/* FillRectangles */
		struct pict *dp = pict_find(get32(r + 8));
		struct res *d = dp ? res_find(dp->drawable) : NULL;
		const uint8_t *p = r + 20, *end = r + len;
		int cr, cg, cb, ca;

		if (!drawable_ok(d))
			return 1;
		render_color(r + 12, &cr, &cg, &cb, &ca);
		/*
		 * Ad = 1 everywhere here, so OverReverse (D + S*(1-Ad)) and
		 * Dst leave the destination untouched, and Over with a=0 is
		 * the same. xfiles paints FULL-SHEET rectangles with exactly
		 * these - 276k pixels of no-op that blend_px was faithfully
		 * computing one pixel at a time, 46 ms per request.
		 */
		if (r[4] == PICT_OP_DST ||
		    (r[4] == PICT_OP_OVER_REVERSE &&
		     !(d->buf && d->buf->bpp == 4)) ||
		    (r[4] == PICT_OP_OVER && ca == 0))
			return 1;
		/*
		 * A single request can carry THOUSANDS of rectangles - the
		 * XPM-style batching turns an icon into one rect per colour
		 * run - and paying render_fill's whole call chain per rect
		 * measured 42 ms per request during an xfiles navigation.
		 * Everything invariant is hoisted out here: the clip, the
		 * write-readiness, the packed colour, one damage union.
		 */
		if ((r[4] == PICT_OP_CLEAR || r[4] == PICT_OP_SRC ||
		     (r[4] == PICT_OP_OVER && ca >= 255)) && d->buf) {
			struct res *b;
			uint16_t col;
			uint32_t c32;
			int full = 0;

			if (r[4] == PICT_OP_CLEAR) {
				cr = cg = cb = 0;
				/* transparent where the surface has alpha */
				ca = d->buf->bpp == 4 ? 0 : 255;
			}
			/* premultiplied ARGB8888, carrying the colour's alpha */
			c32 = ((uint32_t)ca << 24) |
			      ((uint32_t)(cr * ca / 255) << 16) |
			      ((uint32_t)(cg * ca / 255) << 8) |
			      (uint32_t)(cb * ca / 255);
			/* one rect spanning the drawable = nothing to keep */
			if (end - p == 8 && gets16(p) <= 0 &&
			    gets16(p + 2) <= 0 &&
			    gets16(p) + (int)get16(p + 4) >= d->w &&
			    gets16(p + 2) + (int)get16(p + 6) >= d->h)
				full = 1;
			int was_hole = d->buf && d->buf->hole;

			b = op_target_ex(d, full);
			if (!b)
				return 1;
			col = (uint16_t)(((cr & 0xF8) << 8) |
					 ((cg & 0xFC) << 3) |
					 (cb >> 3));
			/*
			 * Zero into a surface that is known to be all zero
			 * changes nothing, so do nothing - and in particular
			 * do not memset, which is what was refilling the
			 * punched masks: xfiles clears its A8 mask sheets a
			 * cell at a time (62 partial FillRectangles per
			 * maximise, traced 2026-09-03). Nothing changed, so
			 * there is no damage to report either.
			 */
			if (was_hole && (b->bpp == 1 ? cr == 0 :
					 b->bpp == 4 ? c32 == 0 : col == 0)) {
				b->hole = 1;
				return 1;
			}
			int px0 = -32768, py0 = -32768;
			int px1 = 32767, py1 = 32767;
			int bx0 = 1 << 30, by0 = 1 << 30;
			int bx1 = -(1 << 30), by1 = -(1 << 30);

			if (xsp_on > 0) {
				/*
				 * Is this batch a few big rectangles or a
				 * thousand small ones? The answer decides
				 * whether to make fills faster or to stop
				 * asking for them. Rate limited: one line a
				 * second, never per rect.
				 */
				static uint64_t lastf;
				uint64_t nowf = xsp_now();

				if (nowf - lastf > 1000000000ull) {
					lastf = nowf;
					fprintf(stderr, "xsp: FILL batch "
						"nrect=%d dst=%dx%d bpp=%d "
						"first=%dx%d\n",
						(int)((end - p) / 8),
						d->w, d->h, b->bpp,
						(int)get16(p + 4),
						(int)get16(p + 6));
				}
			}
			pict_clip(dp, &px0, &py0, &px1, &py1);
			for (; p + 8 <= end; p += 8) {
				int rx = gets16(p), ry = gets16(p + 2);
				int x0 = rx, y0 = ry;
				int x1 = rx + get16(p + 4);
				int y1 = ry + get16(p + 6);
				int y;

				if (x0 < px0) x0 = px0;
				if (y0 < py0) y0 = py0;
				if (x1 > px1) x1 = px1;
				if (y1 > py1) y1 = py1;
				/* drawable-local -> buffer, clip once */
				x0 += d->ax; x1 += d->ax;
				y0 += d->ay; y1 += d->ay;
				if (x0 < d->cx0) x0 = d->cx0;
				if (y0 < d->cy0) y0 = d->cy0;
				if (x1 > d->cx1) x1 = d->cx1;
				if (y1 > d->cy1) y1 = d->cy1;
				if (x0 >= x1 || y0 >= y1)
					continue;
				/*
				 * A whole-surface clear to zero on a memfd-born
				 * surface does not need to write anything: punch
				 * the pages out and the kernel hands back zero
				 * pages on the next touch, ours or the client's
				 * (it maps the same memfd). This is the uniform-
				 * pixmap idea from px_alloc() done where the three
				 * earlier attempts died: no NULL px, no munmap
				 * with a recomputed size. xfiles clears its
				 * full-window A8 masks like this on every frame,
				 * and those two masks were 705 kB of resident
				 * zeros. Damage still accrues below.
				 */
				if (b->shm_fd >= 0 && x0 == 0 && y0 == 0 &&
				    x1 == b->w && y1 == b->h &&
				    ((b->bpp == 1 && cr == 0) ||
				     (b->bpp == 4 && c32 == 0) ||
				     (b->bpp == 2 && col == 0)) &&
				    fallocate(b->shm_fd, FALLOC_FL_PUNCH_HOLE |
					      FALLOC_FL_KEEP_SIZE, 0,
					      (off_t)b->shm_len) == 0) {
					n_punch++;
					b->hole = 1;
					xshim_px_acc += (uint64_t)(x1 - x0) *
							(y1 - y0);
					if (x0 < bx0) bx0 = x0;
					if (y0 < by0) by0 = y0;
					if (x1 > bx1) bx1 = x1;
					if (y1 > by1) by1 = y1;
					continue;
				}
				if (b->bpp == 1) {
					/*
					 * An A8 surface stores one intensity
					 * byte; blend_px writes r8 there, so
					 * the span form is a memset. This is
					 * the mask-sheet clear xfiles does at
					 * full window size.
					 */
					for (y = y0; y < y1; y++)
						memset((uint8_t *)b->px +
						       (size_t)y * b->w + x0,
						       (uint8_t)cr,
						       (size_t)(x1 - x0));
				} else if (b->bpp == 4) {
					/*
					 * ARGB8888 target. The 16-bit loop
					 * below wrote `col` as halfwords at
					 * a halfword stride into these
					 * buffers, which painted xfiles'
					 * sheet black (2026-09-11). The
					 * value carries the request's alpha:
					 * a Clear here is 0, not opaque
					 * black - these are xfiles' alpha
					 * layers, and an opaque "clear" made
					 * every later masked composite paint
					 * its colour over the whole cell.
					 */
					for (y = y0; y < y1; y++) {
						uint32_t *q = (uint32_t *)b->px +
							(size_t)y * b->w + x0;
						int nn = x1 - x0, k;

						for (k = 0; k < nn; k++)
							q[k] = c32;
					}
				} else for (y = y0; y < y1; y++) {
					uint16_t *q = b->px +
						(size_t)y * b->w + x0;
					int nn = x1 - x0;

					/*
					 * Store 32 bits at a time. A halfword
					 * store per pixel measured 78 ns/px
					 * (~19 cycles at 240 MHz) for a loop
					 * that is one store - the cost is the
					 * write to PSRAM, not the arithmetic,
					 * so the fix is fewer, wider writes
					 * rather than a cleverer loop.
					 */
					if (nn >= 8) {
						uint32_t c2 =
						  ((uint32_t)col << 16) | col;
						uint32_t *q32;
						int n32;

						if ((uintptr_t)q & 2) {
							*q++ = col;
							nn--;
						}
						q32 = (uint32_t *)q;
						n32 = nn >> 1;
						while (n32--)
							*q32++ = c2;
						q = (uint16_t *)q32;
						nn &= 1;
					}
					while (nn--)
						*q++ = col;
				}
				xshim_px_acc += (uint64_t)(x1 - x0) *
						(y1 - y0);
				if (x0 < bx0) bx0 = x0;
				if (y0 < by0) by0 = y0;
				if (x1 > bx1) bx1 = x1;
				if (y1 > by1) by1 = y1;
			}
			if (bx1 > bx0) {
				if (!b->dmg_valid) {
					b->dmg_x0 = bx0; b->dmg_y0 = by0;
					b->dmg_x1 = bx1; b->dmg_y1 = by1;
					b->dmg_valid = 1;
				} else {
					if (bx0 < b->dmg_x0) b->dmg_x0 = bx0;
					if (by0 < b->dmg_y0) b->dmg_y0 = by0;
					if (bx1 > b->dmg_x1) b->dmg_x1 = bx1;
					if (by1 > b->dmg_y1) b->dmg_y1 = by1;
				}
			}
			notify_draw(d);
			return 1;
		}
		if (xsp_on > 0) {
			static uint64_t lastp;
			uint64_t nowp = xsp_now();

			if (nowp - lastp > 1000000000ull) {
				lastp = nowp;
				fprintf(stderr, "xsp: SLOW-FILL op=%u ca=%d "
					"nrects=%d first=%ux%u dstbpp=%d\n",
					r[4], ca, (int)((end - p) / 8),
					get16(p + 4), get16(p + 6),
					d->buf ? d->buf->bpp : -1);
			}
		}
		for (; p + 8 <= end; p += 8) {
			if (trace_on())
				fprintf(stderr, "xshim:   fill op=%u %ux%u at "
					"%d,%d rgba %d,%d,%d,%d\n", r[4],
					get16(p + 4), get16(p + 6), gets16(p),
					gets16(p + 2), cr, cg, cb, ca);
			render_fill(d, dp, gets16(p), gets16(p + 2),
				    get16(p + 4), get16(p + 6),
				    cr, cg, cb, ca, r[4]);
			damage_add(d, gets16(p), gets16(p + 2),
				   get16(p + 4), get16(p + 6));
		}
		notify_draw(d);
		return 1;
	}
	case 33: {					/* CreateSolidFill */
		struct pict *pi = pict_new(get32(r + 4));
		int cr, cg, cb, ca;

		if (!pi)
			return 1;
		render_color(r + 8, &cr, &cg, &cb, &ca);
		pi->solid = 1;
		pi->rr = (uint8_t)cr; pi->gg = (uint8_t)cg;
		pi->bb = (uint8_t)cb; pi->a = (uint8_t)ca;
		if (trace_on())
			fprintf(stderr, "xshim:   solid 0x%x = rgba %d,%d,%d,%d\n",
				pi->id, cr, cg, cb, ca);
		return 1;
	}
	}
	return 0;
}

/*
 * XLITE-SHM. Two requests, both about one pixmap:
 *
 *   1 GetPixmapFd - reply carries width, height, stride and bytes-per-pixel,
 *                   with the memfd attached.
 *   2 Damaged     - the client has written to those pages, so mark the surface
 *                   dirty and repaint.
 *
 * There is no reply to Damaged: the point of this path is that pixels move
 * without round trips, and a round trip per update would put one straight back.
 */
/*
 * MIT-SHM, server side. Four requests matter:
 *
 *   0 ShmQueryVersion  - version, and "yes, shared pixmaps are a thing"
 *   1 ShmAttach        - the client hands us a SysV shmid; we attach it
 *   2 ShmDetach        - and let it go
 *   3 ShmPutImage      - pixels are already in that segment; use them
 *
 * THE POINT IS ShmPutImage NOT COPYING. A normal X server copies out of the
 * client's segment into its own drawable, and MIT-SHM's win is only that the
 * pixels never crossed the socket. We already avoid the socket (XLITE-SHM), so
 * copying here would buy exactly nothing.
 *
 * Instead, when a client puts a full-window image from a segment big enough to
 * BE the window, we ADOPT the segment as the window's pixel storage. After
 * that the client renders straight into what the compositor reads, and a frame
 * costs one damage message. That is not what a normal X server does; it is
 * available to us because we own both halves, and it mirrors XLITE-SHM, which
 * does the same thing in the other direction.
 */
static struct shmseg *shmseg_find(uint32_t id)
{
	int i;

	for (i = 0; i < MAXSHMSEG; i++)
		if (shmsegs[i].addr && shmsegs[i].id == id)
			return &shmsegs[i];
	return NULL;
}

static void shmseg_drop(struct shmseg *sg)
{
	int i;

	if (!sg || !sg->addr)
		return;
	/*
	 * A window may have ADOPTED this memory. Hand it back a buffer of its
	 * own before the pages go, or the compositor reads freed address
	 * space - which is a use-after-free that would show up as garbage on
	 * the panel long after the client exited.
	 */
	for (i = 0; i < MAXRES; i++) {
		struct res *w = &res[i];

		if (w->type == R_WINDOW && w->px_adopted &&
		    (void *)w->px == sg->addr) {
			size_t n = (size_t)w->w * w->h * (w->bpp ? w->bpp : 2);

			w->px = calloc(n, 1);
			w->px_adopted = 0;
			if (!w->px)
				w->w = w->h = 0;	/* nothing to draw */
		}
	}
	/*
	 * Unmap it the way it was mapped. shmdt() on an mmap'd pointer fails
	 * with EINVAL and leaks the mapping; munmap() on a SysV attachment
	 * unmaps someone else's idea of that address.
	 */
	if (sg->is_fd)
		munmap(sg->addr, sg->len);
	else
		shmdt(sg->addr);
	memset(sg, 0, sizeof(*sg));
}

static void mitshm_request(struct cli *c, const uint8_t *r, int len)
{
	uint8_t d24[24];
	uint8_t op = r[1];

	(void)len;
	memset(d24, 0, sizeof d24);

	switch (op) {
	case 0: {				/* ShmQueryVersion */
		/*
		 * 1.2, which is the version that has ShmAttachFd (6) and
		 * ShmCreateSegment (7). The minor version is a CONTRACT, not a
		 * label - xcb clients test `minor >= 2` and then call
		 * shm_create_segment without asking anything else - so this
		 * number and the op table below have to agree.
		 *
		 * We still do not implement ShmGetImage (4) or ShmCreatePixmap
		 * (5): 5 is gated behind the sharedPixmaps flag we return as
		 * 0, and 4 is a read-back path nothing here uses. Both answer
		 * with BadImplementation rather than silence.
		 */
		put16(d24 + 0, 1);		/* major */
		put16(d24 + 2, 2);		/* minor */
		put16(d24 + 4, 0);		/* uid */
		put16(d24 + 6, 0);		/* gid */
		d24[8] = 2;			/* ZPixmap format */
		/*
		 * sharedPixmaps = 0. We do not implement ShmCreatePixmap, and
		 * claiming it would invite a client to try. SDL only needs
		 * ShmPutImage.
		 */
		send_reply(c, 0, d24, NULL, 0);	/* detail = sharedPixmaps */
		break;
	}
	case 1: {				/* ShmAttach */
		uint32_t seg = get32(r + 4);
		int shmid = (int)get32(r + 8);
		int i, ro;
		void *a;

		for (i = 0; i < MAXSHMSEG; i++)
			if (!shmsegs[i].addr)
				break;
		if (i == MAXSHMSEG) {
			send_error(c, X_BAD_ALLOC, seg, MITSHM_MAJOR);
			break;
		}
		/*
		 * Attach the way the client asked. This is not a detail: an
		 * ADOPTED segment becomes the window's pixels, and every
		 * server-side drawing path - a background fill, an Expose
		 * repaint, CopyArea - then writes through this mapping. Under
		 * SHM_RDONLY that write is a SIGSEGV that takes the whole
		 * desktop down, so a read-only segment is never adopted
		 * (below) and only ever copied out of.
		 */
		ro = r[12] ? 1 : 0;
		a = shmat(shmid, NULL, ro ? SHM_RDONLY : 0);
		if (a == (void *)-1) {
			fprintf(stderr, "xshim: ShmAttach shmid %d: %s\n",
				shmid, strerror(errno));
			send_error(c, X_BAD_VALUE, seg, MITSHM_MAJOR);
			break;
		}
		{
			struct shmid_ds ds;

			shmsegs[i].len = shmctl(shmid, IPC_STAT, &ds) == 0 ?
					 ds.shm_segsz : 0;
		}
		shmsegs[i].id = seg;
		shmsegs[i].shmid = shmid;
		shmsegs[i].addr = a;
		shmsegs[i].ro = ro;
		shmsegs[i].owner = (int)(c - cli);
		fprintf(stderr, "xshim: MIT-SHM attach seg 0x%x shmid %d "
			"(%zu bytes)\n", seg, shmid, shmsegs[i].len);
		break;
	}
	case 2:					/* ShmDetach */
		shmseg_drop(shmseg_find(get32(r + 4)));
		break;
	case 3: {				/* ShmPutImage */
		uint32_t did = get32(r + 4);
		struct res *d = res_find(did);
		/*
		 * xShmPutImageReq, verbatim from shmproto.h - gc at 8,
		 * geometry from 12, then depth/format/sendEvent/pad at 28,
		 * shmseg at 32 and offset at 36. Do not "tidy" these; a stock
		 * libXext writes exactly this and nothing negotiates it.
		 */
		uint16_t tw = get16(r + 12), th = get16(r + 14);
		uint16_t sx = get16(r + 16), sy = get16(r + 18);
		uint16_t sw = get16(r + 20), sh = get16(r + 22);
		int16_t dx = (int16_t)get16(r + 24), dy = (int16_t)get16(r + 26);
		uint32_t seg = get32(r + 32);
		uint32_t off = get32(r + 36);
		struct shmseg *sg = shmseg_find(seg);
		struct res *db;
		int bpp, sstride;
		size_t need;

		if (!d || !sg) {
			static int once;

			if (!once++)
				fprintf(stderr, "xshim: ShmPutImage drawable "
					"0x%x %s, seg 0x%x %s\n", did,
					d ? "ok" : "NOT FOUND", seg,
					sg ? "ok" : "NOT FOUND");
			break;
		}
		nshmput++;
		db = d->buf ? d->buf : d;
		bpp = db->bpp ? db->bpp : 2;
		/*
		 * Rows in a ZPixmap are padded to the scanline pad we
		 * advertise in the connection setup - 32 bits - exactly as the
		 * ordinary PutImage path computes it. It happens to be a no-op
		 * at 320 and 640 wide, which is precisely why getting it wrong
		 * here would sit undetected until some client picked an odd
		 * width and sheared.
		 */
		sstride = ((int)tw * bpp + 3) & ~3;
		need = (size_t)sstride * th;
		if (off + need > sg->len)
			break;			/* would read off the end */

		/*
		 * COPY. Always. Do not adopt the client's segment.
		 *
		 * Pointing the window's pixels at the segment removed a
		 * 64 kB copy per frame and was worth ~11% - and it is not a
		 * legal implementation of this request. The protocol's
		 * contract is that the server CONSUMES the segment while
		 * handling ShmPutImage: that is exactly why a client may pass
		 * send_event=False and reuse its buffer the moment the request
		 * is processed, as SDL does. Adopting left prboom rendering
		 * its next frame into the pixels we had not drawn yet.
		 *
		 * The symptom is worth recording because no instrument here
		 * could see it. Frame rate cannot. CPU accounting cannot. A
		 * still screenshot structurally cannot - it catches one
		 * instant and says nothing about the one either side, so every
		 * "painting verified" capture passed. It took a person
		 * watching the panel: Doom draws the weapon sprite LAST, so a
		 * mid-render read yields a complete scene with the shotgun
		 * missing, and the sprite flickers.
		 *
		 * The copy is not where the win came from anyway. Without
		 * MIT-SHM, SDL pushes 64,000 bytes through the SOCKET every
		 * frame, and a socket syscall costs ~350 us on this board
		 * (docs/current-state.md). With MIT-SHM the pixels never touch
		 * the socket and this is a plain memcpy out of shared memory,
		 * which keeps most of the benefit and all of the correctness.
		 */
		if (d->buf && d->buf->px) {
			const uint8_t *src = (const uint8_t *)sg->addr + off;
			struct res *b = d->buf;
			int y, cy0 = -1, cy1 = -1;

			/*
			 * COPY AND HASH IN ONE PASS, and damage only the rows
			 * that actually changed.
			 *
			 * SDL hands us the whole window every frame regardless
			 * of what moved, so without this the desktop expands
			 * and then copies 200 rows when perhaps 168 of them
			 * differ. Doom's status bar is the bottom sixth and is
			 * static most of the time; a terminal with a blinking
			 * cursor is static almost everywhere.
			 *
			 * Comparing source against destination would be the
			 * obvious way and is the WRONG one: it reads the
			 * destination too, and on a path that is memory-bound
			 * at ~38 MB/s that extra 64 kB of reads costs more
			 * than the rows it saves. Hashing costs ALU on words
			 * we are already loading to copy, and ALU is close to
			 * free while we are waiting on memory.
			 *
			 * 32-bit FNV-1a, and the width is the whole point.
			 * The first version used the 64-bit constants, which
			 * this 32-bit core has to synthesise from several
			 * multiplies and adds - 16,000 times a frame. It cost
			 * more than it saved: 29.7 fps against 30.5. A 32-bit
			 * multiply is one instruction.
			 *
			 * A collision leaves one row stale until it next
			 * changes. At 2^-32 per row and ~6000 rows/s that is
			 * one event per several days, which is the right trade
			 * for one native multiply per four bytes.
			 */
			if (!rowdmg_on()) {
				b->rowhash_h = 0;
			} else if (b->rowhash_h != b->h) {
				free(b->rowhash);
				b->rowhash = calloc((size_t)(b->h > 0 ? b->h : 1),
						    sizeof *b->rowhash);
				b->rowhash_h = b->rowhash ? b->h : 0;
			}

			for (y = 0; y < sh; y++) {
				int ty = dy + y;
				const uint8_t *sp;
				uint8_t *dp;
				size_t n, k;
				uint32_t hv = 2166136261u;
				int changed = 1;

				if (ty < 0 || ty >= b->h)
					continue;
				sp = src + (size_t)(sy + y) * sstride +
				     (size_t)sx * bpp;
				dp = (uint8_t *)b->px +
				     (size_t)ty * b->w * bpp +
				     (size_t)dx * bpp;
				n = (size_t)sw * bpp;

				if (!b->rowhash || !rowdmg_on()) {
					memcpy(dp, sp, n);
					if (cy0 < 0)
						cy0 = y;
					cy1 = y;
					continue;
				}
				/*
				 * Word at a time where alignment allows, so
				 * the hash costs one multiply per 4 bytes
				 * rather than per byte.
				 */
				if ((((uintptr_t)sp | (uintptr_t)dp) & 3u) == 0) {
					const uint32_t *s4 = (const uint32_t *)sp;
					uint32_t *d4 = (uint32_t *)dp;
					size_t w = n / 4;

					for (k = 0; k < w; k++) {
						uint32_t v = s4[k];

						hv = (hv ^ v) * 16777619u;
						d4[k] = v;
					}
					for (k = w * 4; k < n; k++) {
						hv = (hv ^ sp[k]) * 16777619u;
						dp[k] = sp[k];
					}
				} else {
					for (k = 0; k < n; k++) {
						hv = (hv ^ sp[k]) * 16777619u;
						dp[k] = sp[k];
					}
				}
				changed = b->rowhash[ty] != hv;
				b->rowhash[ty] = hv;
				if (changed) {
					if (cy0 < 0)
						cy0 = y;
					cy1 = y;
				}
			}
			/*
			 * Damage in DRAWABLE coordinates - damage_add()
			 * translates to the buffer owner itself. Nothing
			 * changed means nothing to report, and the desktop
			 * then does no expansion and no scanout copy at all.
			 */
			if (cy0 >= 0) {
				damage_add(d, dx, dy + cy0, sw,
					   cy1 - cy0 + 1);
			} else {
				/*
				 * Not one row differs, so there is nothing to
				 * show. Return WITHOUT marking dirty or
				 * notifying: the desktop's fallback for "no
				 * damage recorded" is to invalidate the whole
				 * window, so reporting an empty change here
				 * would trigger a full repaint - precisely the
				 * opposite of the point.
				 */
				nrow_skip++;
				break;
			}
		}
		d->dirty = 1;
		if (d->buf)
			d->buf->dirty = 1;
		d->hole = 0;
		notify_draw(d);
		break;
	}
	case 6: {				/* ShmAttachFd (1.2) */
		/*
		 * xShmAttachFdReq: shmseg at 4, readOnly at 8. The fd itself
		 * is NOT in the request - it came over SCM_RIGHTS and is
		 * waiting in the client's queue.
		 */
		uint32_t seg = get32(r + 4);
		int ro = r[8] ? 1 : 0;
		int fd, i;
		void *a;
		off_t len;

		if (c->nrfd < 1) {
			fprintf(stderr, "xshim: ShmAttachFd with no fd\n");
			send_error(c, X_BAD_VALUE, seg, MITSHM_MAJOR);
			break;
		}
		fd = c->rfd[0];			/* consumed in order */
		memmove(c->rfd, c->rfd + 1, --c->nrfd * sizeof c->rfd[0]);

		for (i = 0; i < MAXSHMSEG; i++)
			if (!shmsegs[i].addr)
				break;
		if (i == MAXSHMSEG) {
			close(fd);
			send_error(c, X_BAD_ALLOC, seg, MITSHM_MAJOR);
			break;
		}
		/*
		 * The client does not tell us how big it is, so ask the fd.
		 * A zero-length or unseekable fd would otherwise become a
		 * zero-length mapping that every later bounds check passes by
		 * refusing to draw.
		 */
		len = lseek(fd, 0, SEEK_END);
		if (len <= 0) {
			fprintf(stderr, "xshim: ShmAttachFd fd has no size\n");
			close(fd);
			send_error(c, X_BAD_VALUE, seg, MITSHM_MAJOR);
			break;
		}
		a = mmap(NULL, (size_t)len, ro ? PROT_READ :
			 (PROT_READ | PROT_WRITE), MAP_SHARED, fd, 0);
		close(fd);			/* the mapping holds the file */
		if (a == MAP_FAILED) {
			fprintf(stderr, "xshim: ShmAttachFd mmap: %s\n",
				strerror(errno));
			send_error(c, X_BAD_ALLOC, seg, MITSHM_MAJOR);
			break;
		}
		shmsegs[i].id = seg;
		shmsegs[i].shmid = -1;
		shmsegs[i].addr = a;
		shmsegs[i].len = (size_t)len;
		shmsegs[i].ro = ro;
		shmsegs[i].is_fd = 1;
		shmsegs[i].owner = (int)(c - cli);
		fprintf(stderr, "xshim: MIT-SHM attachfd seg 0x%x "
			"(%zu bytes%s)\n", seg, shmsegs[i].len,
			ro ? ", read-only" : "");
		break;
	}
	case 7: {				/* ShmCreateSegment (1.2) */
		/*
		 * The server allocates and hands BACK an fd. memfd is the
		 * right primitive: it is anonymous, it needs no filesystem
		 * (this root is a read-only overlay), and its lifetime is the
		 * descriptor's - so a client that dies never strands a named
		 * object the way SysV shm does.
		 */
		uint32_t seg = get32(r + 4);
		uint32_t size = get32(r + 8);
		int ro = r[12] ? 1 : 0;
		uint8_t d24[24];
		int fd, i;
		void *a;

		memset(d24, 0, sizeof d24);
		for (i = 0; i < MAXSHMSEG; i++)
			if (!shmsegs[i].addr)
				break;
		/*
		 * Cap it. This is a request to allocate on a board with 15.4
		 * MB of RAM, made by a client that picked the number - an
		 * unbounded one is a denial of service with a single request.
		 */
		if (i == MAXSHMSEG || !size || size > 8u * 1024 * 1024) {
			send_error(c, X_BAD_ALLOC, seg, MITSHM_MAJOR);
			break;
		}
		fd = memfd_create("xshim-shm", 0);
		if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
			fprintf(stderr, "xshim: ShmCreateSegment %u: %s\n",
				size, strerror(errno));
			if (fd >= 0)
				close(fd);
			send_error(c, X_BAD_ALLOC, seg, MITSHM_MAJOR);
			break;
		}
		a = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (a == MAP_FAILED) {
			close(fd);
			send_error(c, X_BAD_ALLOC, seg, MITSHM_MAJOR);
			break;
		}
		shmsegs[i].id = seg;
		shmsegs[i].shmid = -1;
		shmsegs[i].addr = a;
		shmsegs[i].len = size;
		shmsegs[i].ro = ro;
		shmsegs[i].is_fd = 1;
		shmsegs[i].owner = (int)(c - cli);
		fprintf(stderr, "xshim: MIT-SHM createsegment seg 0x%x "
			"(%u bytes)\n", seg, size);
		/*
		 * nfd = 1 rides in the reply's detail byte, and the fd itself
		 * goes as ancillary data on this very reply - which is why
		 * send_reply_fd() flushes the output buffer first.
		 */
		send_reply_fd_detail(c, 1, d24, fd);
		close(fd);
		break;
	}
	default:
		/*
		 * Say so. A silently dropped request is the worst answer here:
		 * ShmCreateSegment (7) and ShmGetImage (4) both expect a
		 * REPLY, so ignoring one hangs the client on a read that will
		 * never complete, and it hangs there looking like our
		 * compositor has died rather than like a request we declined.
		 * An error unwinds it in the client's own error handler.
		 */
		fprintf(stderr, "xshim: MIT-SHM minor op %u not implemented\n",
			op);
		send_error(c, X_BAD_IMPLEMENTATION, 0, MITSHM_MAJOR);
		break;
	}
}

static void xshm_request(struct cli *c, const uint8_t *r, int len)
{
	uint8_t d24[24];
	uint8_t op = r[0];

	(void)len;
	memset(d24, 0, sizeof(d24));

		/*
		 * XLITE-SHM. Two requests, both about one pixmap:
		 *
		 *   1 GetPixmapFd  - reply carries width, height, stride and
		 *                    bytes-per-pixel, with the memfd attached.
		 *   2 Damaged      - the client has written to those pages, so
		 *                    mark the surface dirty and repaint.
		 *
		 * There is no reply to Damaged: the point of the whole path is
		 * that pixels move without round trips, and a round trip per
		 * update would put one straight back.
		 */
		struct res *p = res_find(get32(r + 4));

		if (r[1] == 1) {
			/*
			 * Windows too, not just pixmaps - and this is where
			 * the frames are.
			 *
			 * Every SDL client draws with XPutImage to a WINDOW,
			 * so refusing windows meant Doom pushed 128 KB per
			 * frame through af_unix: measured at ~3.9 ms per band
			 * in the shim, with the socket READ costing more than
			 * the drawing (97 ms vs 72 ms over 32 passes), on a
			 * board whose memory bandwidth is 88 MB/s.
			 *
			 * Safe for a TOP-LEVEL window only, and the test is
			 * exact: a top level gets `w->buf = w` with ax/ay 0
			 * (line ~907), so it owns its buffer outright and the
			 * client cannot reach anything else through it. A
			 * CHILD gets `w->buf = p->buf` and an offset into its
			 * parent's pixels, where a stray write would land in a
			 * sibling - those keep the socket path.
			 *
			 * op_target() first: it breaks any copy-on-write alias
			 * and guarantees px, so what we hand out is the
			 * window's own private pixels.
			 */
			struct res *b = NULL;

			if (p && p->type == R_PIXMAP)
				b = p;
			else if (p && p->type == R_WINDOW && p->buf == p)
				b = op_target(p);	/* owns its pixels */
			else if (p && p->type == R_WINDOW && p->buf &&
				 win_share_ok(p))
				b = op_target(p);	/* lone child */
			if (!b || !px_share(b)) {
				/*
				 * Answer "not shareable" as an ordinary reply
				 * with no descriptor, NOT an error: a client
				 * asking about a window is asking a reasonable
				 * question, and an error leaves it waiting for
				 * a reply that never arrives. XPutImage to a
				 * window drew nothing and hung there.
				 */
				memset(d24, 0, sizeof(d24));
				send_reply(c, 0, d24, NULL, 0);
				return;
			}
			/*
			 * Report the DRAWABLE's size but the BUFFER's stride,
			 * plus the drawable's byte offset into that buffer.
			 *
			 * A child window does not own pixels - it is a clipped
			 * view into its top-level's buffer at (ax, ay) - and
			 * this used to refuse anything that was not a top
			 * level, which is every window SDL draws into: SDL
			 * creates its drawing window as a CHILD of the window
			 * it hands the window manager. So the fast path was
			 * declined for exactly the clients it was written for.
			 *
			 * Handing over the origin lets the client write into
			 * its own rectangle of the shared buffer. It cannot
			 * reach a sibling: the width and height above are the
			 * WINDOW's, and xlite clamps every row to them.
			 */
			{
				int ox = (p->type == R_WINDOW) ? p->ax : 0;
				int oy = (p->type == R_WINDOW) ? p->ay : 0;
				int vw = (p->type == R_WINDOW) ? p->w : b->w;
				int vh = (p->type == R_WINDOW) ? p->h : b->h;

				put16(d24, (uint16_t)vw);
				put16(d24 + 2, (uint16_t)vh);
				put16(d24 + 4, (uint16_t)(b->w * b->bpp));
				d24[6] = b->bpp;
				put32(d24 + 8, (uint32_t)b->shm_len);
				put32(d24 + 12, (uint32_t)
				      (((size_t)oy * b->w + ox) * b->bpp));
			}
			send_reply_fd(c, d24, b->shm_fd);
			/*
			 * Unconditional: a share is granted once per drawable,
			 * never per frame, so this cannot flood the console -
			 * and "did this client get zero-copy?" is the first
			 * question asked of every measurement here. It used to
			 * be answerable only with XSHIM_TRACE=1, which on this
			 * build walks every pixel per damage and kills the
			 * client it is meant to observe.
			 */
			fprintf(stderr, "xshim: ZEROCOPY %s 0x%x %dx%d bpp %u "
				"-> fd %d (%zu bytes)\n",
				p->type == R_WINDOW ? "window" : "pixmap",
				b->id, b->w, b->h, b->bpp, b->shm_fd,
				b->shm_len);
	} else if (r[1] == 2 && p) {
		if (trace_on()) {
			size_t i, tot = (size_t)p->w * p->h, nz = 0;

			for (i = 0; i < tot; i++)
				if (p->bpp == 1 ? ((uint8_t *)p->px)[i]
						: p->px[i])
					nz++;
			fprintf(stderr, "xshim: SHM damaged 0x%x %zu/%zu "
				"non-zero\n", p->id, nz, tot);
		}
		/*
		 * MARK THE BUFFER OWNER, NOT THE DRAWABLE.
		 *
		 * SDL creates a top-level and ONE child to draw into, and the
		 * child shares the parent's pixels (w->buf = p->buf), so the
		 * Damaged request names the CHILD while the desktop asks for
		 * pixels - and xshim_window_pixels() tests and clears dirty -
		 * by the TOP-LEVEL id. Marking only `p` therefore set the flag
		 * on a res nobody reads, and the expansion's early-out could
		 * never be re-armed.
		 *
		 * That defect was invisible for as long as the depth-8 path
		 * forgot to clear dirty at all: the flag sat at 1 from window
		 * creation, so every repaint re-expanded 64,000 pixels and the
		 * picture stayed correct by accident, at 18.6 fps instead of
		 * 27. Restoring the clear alone turned the window BLACK, which
		 * is what exposed this. Both halves are needed.
		 *
		 * `buf` is the general answer, not a special case for SDL: it
		 * is by definition "who owns the pixels we draw into", so it
		 * is the res whose shadow an expansion actually fills.
		 */
		if (p->buf)
			p->buf->dirty = 1;
		p->dirty = 1;
		p->hole = 0;
		notify_draw(p);
	}
}

static void handle(struct cli *c, const uint8_t *r, int len)
{
	uint8_t op = r[0], detail = r[1];
	uint8_t d24[24];

	cur_owner = (int)(c - cli);
	nreqs++;
	memset(d24, 0, sizeof(d24));
	c->seq++;
	if (c->nrecent < (int)sizeof(c->recent)) {
		c->recent[c->nrecent++] = op;
	} else {
		memmove(c->recent, c->recent + 1, sizeof(c->recent) - 1);
		c->recent[sizeof(c->recent) - 1] = op;
	}
	if (trace_on())
		fprintf(stderr, "  [%3u] %-22s op=%-3u len=%d\n",
			c->seq, opstr(op), op, len);

	/*
	 * Extension requests. Every QueryExtension is answered "not present",
	 * so a client using one is either ignoring that answer or talking to
	 * an extension it never asked about - either way, say so rather than
	 * fall into the switch and land on an unrelated core opcode.
	 */
	/*
	 * Install the clip of whichever GC this request names, so the drawing
	 * primitives - which take a drawable and a colour, not a GC - honour
	 * it without every one of them growing a parameter.
	 */
	gcclip_on = 0;
	if (op >= 62 && op <= 77) {
		/* CopyArea and CopyPlane carry the GC third, the rest second */
		const uint8_t *gp = (op == 62 || op == 63) ? r + 12 : r + 8;
		struct res *g = len >= (int)(gp - r) + 4 ?
				res_find(get32(gp)) : NULL;

		if (g && g->type == R_GC && g->clip_set) {
			gcclip_on = 1;
			gcclip_x0 = g->cx0; gcclip_y0 = g->cy0;
			gcclip_x1 = g->cx1; gcclip_y1 = g->cy1;
		}
	}

	if (op >= 128) {
		if (op == RENDER_MAJOR && render_request(c, r, len))
			return;
		/*
		 * Extension opcodes are intercepted HERE, before the switch -
		 * so a case label for one down there is dead code, and the
		 * request is answered "not implemented" while the client waits
		 * for a reply that never comes.
		 */
		if (op == MITSHM_MAJOR) {
			mitshm_request(c, r, len);
			return;
		}
		if (op == XSHM_MAJOR) {
			xshm_request(c, r, len);
			return;
		}
		if (op == VIDMODE_MAJOR) {
			vidmode_request(c, r, len);
			return;
		}
		if (c->nunimpl[op & 127]++ == 0) {
			fprintf(stderr, "xshim: extension request, major "
				"opcode %u minor %u - not implemented\n", op,
				detail);
			dump_recent(c);
		}
		send_error(c, X_BAD_REQUEST, 0, op);
		return;
	}

	switch (op) {
	case 98: {					/* QueryExtension */
		int n = get16(r + 4);

		/*
		 * RENDER is answered YES, and everything else no. Refusing
		 * XKB in particular sends Xlib down its core-keyboard path,
		 * so there is no layout machinery here; refusing RENDER used
		 * to cap the program base at applications that still draw
		 * text with core requests, which by now is almost none.
		 */
		if (n == 6 && !memcmp(r + 8, "RENDER", 6)) {
			d24[0] = 1;			/* present */
			d24[1] = RENDER_MAJOR;
			d24[2] = 0;			/* no events */
			d24[3] = RENDER_ERROR;
		}
		/*
		 * Our own extension, which only our own libX11 asks for. It
		 * hands a client the actual pages behind a pixmap, so bulk
		 * pixel loading stops being protocol traffic entirely.
		 */
		if (n == 9 && !memcmp(r + 8, "XLITE-SHM", 9)) {
			d24[0] = 1;
			d24[1] = XSHM_MAJOR;
			d24[2] = 0;
			d24[3] = 0;
		}
		/*
		 * MIT-SHM. SDL probes for this by name and takes a completely
		 * different, copy-free path when it is present - see
		 * try_mitshm() in SDL's SDL_x11image.c. Answering it is what
		 * removes a full-frame memcpy from every SDL client.
		 */
		if (n == 24 && !memcmp(r + 8, "XFree86-VidModeExtension", 24)) {
			d24[0] = 1;
			d24[1] = VIDMODE_MAJOR;
			d24[2] = 0;
			d24[3] = VIDMODE_ERROR;
		}
		if (n == 7 && !memcmp(r + 8, "MIT-SHM", 7)) {
			d24[0] = 1;
			d24[1] = MITSHM_MAJOR;
			d24[2] = 0;		/* no events: we never
						 * ShmCompletion, and nothing
						 * asks us to */
			d24[3] = MITSHM_ERROR;
		}
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
		} else if (!id) {
			fprintf(stderr, "xshim: out of atoms (%d) interning "
				"'%.*s' - raise MAXATOM\n", MAXATOM, n, r + 8);
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
		 * The metrics of the font this id refers to, not a synthetic
		 * face. A toolkit lays out its widgets from exactly these
		 * numbers, so they have to describe the glyphs we will
		 * actually draw or the layout is computed on a lie.
		 */
		struct res *fr = res_find(get32(r + 4));
		const struct xfont *f = (fr && fr->type == R_FONT &&
					 fr->font_idx < XFONT_N)
					? &xfonts[fr->font_idx] : &xfonts[0];
		int nch = f->last - f->first + 1;
		uint8_t ext[28 + 256 * 12], ci[12], *p = ext;
		int i, wmin = 1 << 30, wmax = 0;

		for (i = 0; i < nch; i++) {
			int a = glyph_adv(f, (uint8_t)(f->first + i));

			if (a < wmin) wmin = a;
			if (a > wmax) wmax = a;
		}
		/* min-bounds, then max-bounds, spanning the 24-byte header. */
		memset(ci, 0, sizeof(ci));
		put16(ci + 2, wmin); put16(ci + 4, wmin);
		put16(ci + 6, f->ascent); put16(ci + 8, f->descent);
		memcpy(d24, ci, 12); memset(d24 + 12, 0, 4);
		put16(ci + 2, wmax); put16(ci + 4, wmax);
		memcpy(d24 + 16, ci, 8);

		memcpy(p, ci + 8, 4); p += 4;
		memset(p, 0, 4); p += 4;
		put16(p, f->first); put16(p + 2, f->last);
		put16(p + 4, f->first); put16(p + 6, 0); p += 8;
		*p++ = 0; *p++ = 0; *p++ = 0; *p++ = 1;
		put16(p, f->ascent); put16(p + 2, f->descent); p += 4;
		put32(p, nch); p += 4;
		for (i = 0; i < nch; i++) {
			int a = glyph_adv(f, (uint8_t)(f->first + i));

			memset(ci, 0, sizeof(ci));
			put16(ci + 2, a); put16(ci + 4, a);
			put16(ci + 6, f->ascent); put16(ci + 8, f->descent);
			memcpy(p, ci, 12); p += 12;
		}
		send_reply(c, 0, d24, ext, p - ext);
		break;
	}
	case 48: {					/* QueryTextExtents */
		/*
		 * The string is CHAR2B even for an 8-bit font, and r[1] is the
		 * odd-length flag. Every field here used to be one slot out,
		 * which put 0 in overall-width - so every string measured as
		 * zero pixels wide and a toolkit sized from that collapsed its
		 * whole layout.
		 */
		struct res *fr = res_find(get32(r + 4));
		const struct xfont *f = (fr && fr->type == R_FONT &&
					 fr->font_idx < XFONT_N)
					? &xfonts[fr->font_idx] : &xfonts[0];
		int n = (len - 8) / 2 - (r[1] ? 1 : 0), i, wid = 0;

		if (n < 0)
			n = 0;
		for (i = 0; i < n; i++)		/* low byte of each CHAR2B */
			wid += glyph_adv(f, r[8 + i * 2 + 1]);
		put16(d24 + 0, f->ascent);
		put16(d24 + 2, f->descent);
		put16(d24 + 4, f->ascent);
		put16(d24 + 6, f->descent);
		put32(d24 + 8, (uint32_t)wid);		/* overall-width */
		put32(d24 + 12, 0);			/* overall-left */
		put32(d24 + 16, (uint32_t)wid);		/* overall-right */
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 49:					/* ListFonts: none */
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 14: {					/* GetGeometry */
		/*
		 * Was a stub: root, panel size, depth 16 for every drawable.
		 * xftlite now asks the target's depth to pick a pixel format,
		 * and a depth-32 pixmap answered "16" drew its text as RGB565
		 * (xfiles' labels, 2026-09-11). Real geometry and depth.
		 */
		struct res *g = res_find(get32(r + 4));
		unsigned depth = 16;

		put32(d24, ROOT_ID);
		if (g && (g->type == R_WINDOW || g->type == R_PIXMAP)) {
			put16(d24 + 4, (uint16_t)g->x);
			put16(d24 + 6, (uint16_t)g->y);
			put16(d24 + 8, (uint16_t)g->w);
			put16(d24 + 10, (uint16_t)g->h);
			put16(d24 + 12, (uint16_t)g->bw);
			if (g->depth)
				depth = g->depth;
		} else {
			put16(d24 + 8, XSHIM_W); put16(d24 + 10, XSHIM_H);
		}
		send_reply(c, (uint8_t)depth, d24, NULL, 0);
		break;
	}
	case 15:					/* QueryTree */
		put32(d24, ROOT_ID);
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 85: {					/* AllocNamedColor */
		uint16_t cr, cg, cb;
		int n = get16(r + 8);

		if (12 + n > len)
			n = len - 12;
		color_lookup(r + 12, n, &cr, &cg, &cb);
		put32(d24, pixel_for(get32(r + 4), cr, cg, cb));
		put16(d24 + 4, cr); put16(d24 + 6, cg); put16(d24 + 8, cb);
		put16(d24 + 10, cr); put16(d24 + 12, cg); put16(d24 + 14, cb);
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 92: {					/* LookupColor */
		uint16_t cr, cg, cb;
		int n = get16(r + 8);

		if (12 + n > len)
			n = len - 12;
		color_lookup(r + 12, n, &cr, &cg, &cb);
		put16(d24 + 0, cr); put16(d24 + 2, cg); put16(d24 + 4, cb);
		put16(d24 + 6, cr); put16(d24 + 8, cg); put16(d24 + 10, cb);
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 84: {					/* AllocColor */
		uint16_t rr = get16(r + 8), gg = get16(r + 10), bb = get16(r + 12);

		put16(d24, rr); put16(d24 + 2, gg); put16(d24 + 4, bb);
		put32(d24 + 8, pixel_for(get32(r + 4), rr, gg, bb));
		if (trace_on())
			fprintf(stderr, "xshim:   alloccolor cmap 0x%x (visual 0x%x) %04x/%04x/%04x -> pixel %08x\n",
				get32(r + 4), cmap_visual(get32(r + 4)), rr, gg, bb,
				pixel_for(get32(r + 4), rr, gg, bb));
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
	/*
	 * 118 SetModifierMapping and 119 GetModifierMapping. This used to read
	 * `case 119: case 109:` - but 109 is ChangeHosts, which takes no reply
	 * at all, so answering it would have put an extra 32 bytes into the
	 * stream and desynchronised every reply after it. Nothing observed had
	 * sent ChangeHosts, so it never fired; it would have been a
	 * spectacularly confusing first symptom.
	 */
	case 118: case 119: {
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

		if (!rr) {
			send_error(c, X_BAD_ALLOC, id, op);
			break;
		}
		rr->x = gets16(r + 12); rr->y = gets16(r + 14);
		rr->w = w; rr->h = h; rr->parent = parent;
		rr->bw = get16(r + 20);
		rr->bg = 0xFFFF;
		/*
		 * The window's DEPTH, which was being thrown away.
		 *
		 * It is byte 1 of the request, and 0 means CopyFromParent.
		 * Ignoring it left every window at depth 0, so px_alloc()'s
		 * `depth <= 8 ? 1 : 2` always chose 2 and an 8-bit client
		 * wrote palette indices into a 16-bit surface. The symptom is
		 * unmistakable once seen: the image is perfectly formed and
		 * entirely BLUE, because an index of 0-255 read as RGB565 is
		 * 0x0000-0x00FF, which is the blue channel and nothing else.
		 */
		{
			struct res *par = res_find(parent);

			rr->depth = r[1] ? r[1]
					 : (par && par->depth ? par->depth : 16);
		}
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
			if (bit == 3) rr->border_pixel = get32(v);
			if (bit == 14) rr->cursor = get32(v);
			if (bit == 11) rr->event_mask = get32(v);
			v += 4;
		}
		if (parent == ROOT_ID) {
			px_alloc(rr, w, h);
			if (!rr->px) {
				send_error(c, X_BAD_ALLOC, id, op);
				break;
			}
		}
		geom_update(rr);
		win_fill(rr, 0, 0, w, h);
		if (trace_on())
			fprintf(stderr, "       +win 0x%x %dx%d+%d+%d bw=%d "
				"bg=%08x bp=%08x parent=0x%x\n", id, w, h,
				rr->x, rr->y, rr->bw, rr->bg, rr->border_pixel,
				parent);
		break;
	}
	case 53: {					/* CreatePixmap */
		uint32_t id = get32(r + 4);
		int w = get16(r + 12), h = get16(r + 14);
		struct res *rr = res_new(id, R_PIXMAP);

		if (trace_on())
			fprintf(stderr, "xshim:   pixmap 0x%x %dx%d depth %u\n",
				id, get16(r + 12), get16(r + 14), r[1]);

		rr->w = w; rr->h = h;
		/*
		 * Depth matters for exactly one thing: a depth-1 pixmap is a
		 * BITMAP, and clients build text that way - fill with pixel 0,
		 * draw the string with pixel 1, hand the result to RENDER as an
		 * A1 mask. Stored as RGB565 those are the values 0 and 1, and
		 * reading coverage from the red channel makes every glyph
		 * completely transparent. xfiles' filenames were invisible for
		 * exactly this reason (control/font.c drawtext()).
		 */
		rr->depth = r[1];
		if (!rr || !px_alloc(rr, w, h)) {
			send_error(c, X_BAD_ALLOC, id, op);
			break;
		}
		geom_update(rr);
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

		if (trace_on())
			fprintf(stderr, "xshim:   %s gc 0x%x mask 0x%x "
				"v0 0x%x\n", op == 55 ? "CreateGC" : "ChangeGC",
				gid, mask, v + 4 <= r + len ? get32(v) : 0);

		if (!g)
			break;
		for (bit = 0; bit < 23; bit++) {
			if (!(mask & (1u << bit)))
				continue;
			if (bit == 2) g->fg = get32(v);		/* foreground */
			if (bit == 3) g->bg = get32(v);		/* background */
			if (bit == 4) g->line_width = get32(v);
			if (bit == 14) g->font = get32(v);
			/*
			 * GCClipMask (bit 19). A client resets its clip by
			 * setting this to None, and ignoring that leaves the
			 * previous clip in force for every later draw.
			 */
			if (bit == 19 && !get32(v)) g->clip_set = 0;
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
		/*
		 * MapWindow (8) maps THIS window; MapSubwindows (9) maps its
		 * children. The first version mapped every unmapped window the
		 * client owned, which was a shortcut that worked for xclock
		 * and actively broke xcalc: a toolkit unmaps widgets it wants
		 * hidden, and the next MapWindow for something else brought
		 * them all back.
		 */
		uint32_t target = get32(r + 4);
		struct res *w = res_find(target);

		if (!w || w->type != R_WINDOW) {
			send_error(c, X_BAD_WINDOW, target, op);
			break;
		}
		for (i = 0; i < MAXRES; i++) {
			struct res *m = &res[i];

			if (m->type != R_WINDOW || m->owner != cur_owner)
				continue;
			if (op == 8 ? m != w : m->parent != target)
				continue;
			if (m->mapped)
				continue;
			m->mapped = 1;
			geom_update(m);
			/*
			 * The SERVER paints the background when a window is
			 * mapped; the client then draws its content in
			 * response to the Expose and never repaints the
			 * background itself. Filling at CreateWindow instead
			 * is not equivalent - the window is not mapped yet, so
			 * it has no visible area to fill - and the missing
			 * fill shows up as Xaw's black bevels disappearing.
			 */
			/* Only top-levels become lvdesk windows. */
			if (win_cb && m->parent == ROOT_ID)
				win_cb(m->id, m->w, m->h);
			expose_window(c, m);
		}
		break;
	}

	case 74: {					/* PolyText8 */
		/*
		 * A list of TEXTITEM8: either {length, delta, string} or a
		 * font shift, which is a 255 byte followed by four font-id
		 * bytes. Only one font exists here, so a shift is skipped
		 * rather than honoured - but it still has to be STEPPED OVER
		 * at the right width or the rest of the item list decodes as
		 * garbage.
		 */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int x = gets16(r + 12), y = gets16(r + 14);
		const uint8_t *p = r + 16, *end = r + len;
		const struct xfont *f;

		if (!d || !drawable_ok(d) || !g) {
			send_error(c, d ? X_BAD_GC : X_BAD_DRAWABLE,
				   get32(d ? r + 8 : r + 4), op);
			break;
		}
		f = font_of(g);
		while (p + 2 <= end) {
			int m, delta;

			if (*p == 255) {		/* font shift */
				/* Four bytes of font id, MSB first. */
				struct res *fr;
				uint32_t fid;

				if (p + 5 > end)
					break;
				fid = ((uint32_t)p[1] << 24) |
				      ((uint32_t)p[2] << 16) |
				      ((uint32_t)p[3] << 8) | p[4];
				fr = res_find(fid);
				if (fr && fr->type == R_FONT &&
				    fr->font_idx < XFONT_N)
					f = &xfonts[fr->font_idx];
				p += 5;
				continue;
			}
			m = *p;
			delta = (int8_t)p[1];
			if (p + 2 + m > end)
				break;
			x += delta;
			if (trace_on())
				fprintf(stderr, "xshim: PolyText8 0x%x '%.*s' "
					"fg=%04x bg=%04x at %d,%d\n", d->id, m,
					p + 2, g->fg, g->bg, x, y);
			x += draw_string(d, x, y, p + 2, m, g->fg,
					 f);
			p += 2 + m;
		}
		notify_draw(d);
		break;
	}
	case 76: {					/* ImageText8 */
		/*
		 * Unlike PolyText8 this paints the character cell first, in
		 * the GC's BACKGROUND, then the glyphs in the foreground. That
		 * is the whole difference between the two requests, and it is
		 * how a client overwrites text without clearing first.
		 */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int n = r[1], x = gets16(r + 12), y = gets16(r + 14);
		int i, j;

		if (!d || !drawable_ok(d) || !g) {
			send_error(c, d ? X_BAD_GC : X_BAD_DRAWABLE,
				   get32(d ? r + 8 : r + 4), op);
			break;
		}
		if (16 + n > len)
			n = len - 16;
		{
			const struct xfont *f = font_of(g);
			int wid = text_width(f, r + 16, n);

			for (j = y - f->ascent; j < y + f->descent; j++)
				for (i = x; i < x + wid; i++)
					px_set(d, i, j, g->bg);
		}
		if (trace_on())
			fprintf(stderr, "xshim: ImageText8 0x%x '%.*s' fg=%04x "
				"bg=%04x at %d,%d\n", d->id, n, r + 16, g->fg,
				g->bg, x, y);
		draw_string(d, x, y, r + 16, n, g->fg, font_of(g));
		notify_draw(d);
		break;
	}
	case 10: {					/* UnmapWindow */
		/*
		 * Children are composited into their parent's buffer, which
		 * means unmapping one leaves its pixels behind. Clear the area
		 * it occupied to the parent's background and expose the parent
		 * so it repaints, or the widget stays on screen after the
		 * client has hidden it.
		 */
		struct res *w = res_find(get32(r + 4));
		struct res *par;

		if (!w || w->type != R_WINDOW) {
			send_error(c, X_BAD_WINDOW, get32(r + 4), op);
			break;
		}
		if (w->mapped) {
			/*
			 * UnmapNotify. MapNotify has always been sent; its
			 * opposite never was, and SDL's X11_LeaveFullScreen
			 * unmaps its fullscreen window and then XIfEvent()s
			 * for exactly this before it will close the display.
			 * With the exit crash fixed, that wait was the next
			 * thing on the path: prboom sat in ppoll forever on
			 * SIGTERM from fullscreen (2026-09-11).
			 */
			uint8_t d[28];

			memset(d, 0, sizeof(d));
			put32(d, w->id);		/* event window */
			put32(d + 4, w->id);		/* window */
			send_event(c, 18, d, 28);
		}
		w->mapped = 0;
		geom_update(w);
		par = res_find(w->parent);
		if (par && par->type == R_WINDOW && drawable_ok(par)) {
			win_fill(par, w->x, w->y, w->w, w->h);
			expose_window(c, par);
			notify_draw(par);
		}
		break;
	}
	case 66: {					/* PolySegment */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 8;

		if (!d || !drawable_ok(d) || !g) {
			send_error(c, d ? X_BAD_GC : X_BAD_DRAWABLE,
				   get32(d ? r + 8 : r + 4), op);
			break;
		}
		for (i = 0; i < n; i++) {
			const uint8_t *p = r + 12 + i * 8;

			draw_line(d, gets16(p), gets16(p + 2),
				  gets16(p + 4), gets16(p + 6), g->fg);
		}
		notify_draw(d);
		break;
	}
	case 65: {					/* PolyLine */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 4;

		if (!d || !drawable_ok(d) || !g || n < 2)
			break;
		for (i = 0; i + 1 < n; i++) {
			const uint8_t *p = r + 12 + i * 4;

			draw_line(d, gets16(p), gets16(p + 2),
				  gets16(p + 4), gets16(p + 6), g->fg);
		}
		notify_draw(d);
		break;
	}
	case 69: {					/* FillPoly */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 16) / 4;
		int16_t pts[64 * 2];

		if (!d || !drawable_ok(d) || !g || n < 3)
			break;
		if (n > 64) n = 64;
		for (i = 0; i < n; i++) {
			pts[i * 2] = gets16(r + 16 + i * 4);
			pts[i * 2 + 1] = gets16(r + 16 + i * 4 + 2);
		}
		fill_poly(d, pts, n, g->fg);
		notify_draw(d);
		break;
	}
	case 59: {					/* SetClipRectangles */
		/*
		 * Refusing this was not free: a client that clips its drawing
		 * got BadImplementation, and the error arriving during an idle
		 * poll is what exposed a latent hang in xlite's XPending().
		 *
		 * Stored as the bounding box of the rectangles, which is a
		 * superset - text may extend a little past where the client
		 * asked rather than being cut short, and nothing is drawn
		 * outside the widget that set it.
		 */
		struct res *g = res_find(get32(r + 4));
		int ox = gets16(r + 8), oy = gets16(r + 10);
		const uint8_t *p = r + 12, *end = r + len;
		int first = 1;

		if (!g) {
			send_error(c, X_BAD_GC, get32(r + 4), op);
			break;
		}
		g->clip_set = 0;
		for (; p + 8 <= end; p += 8) {
			int rx = ox + gets16(p), ry = oy + gets16(p + 2);
			int rw = get16(p + 4), rh = get16(p + 6);

			if (first) {
				g->cx0 = rx; g->cy0 = ry;
				g->cx1 = rx + rw; g->cy1 = ry + rh;
				first = 0;
				g->clip_set = 1;
			} else {
				if (rx < g->cx0) g->cx0 = rx;
				if (ry < g->cy0) g->cy0 = ry;
				if (rx + rw > g->cx1) g->cx1 = rx + rw;
				if (ry + rh > g->cy1) g->cy1 = ry + rh;
			}
		}
		if (first) {		/* no rectangles at all: draw nothing */
			g->clip_set = 1;
			g->cx0 = g->cy0 = g->cx1 = g->cy1 = 0;
		}
		break;
	}
	case 62: {					/* CopyArea */
		/*
		 * A toolkit that double-buffers draws into a pixmap and blits
		 * it, so without this such a client renders perfectly into a
		 * buffer nobody ever sees - xclock's face came out blank while
		 * every drawing request it made succeeded.
		 *
		 * Copied through a row at a time because source and
		 * destination are frequently the SAME buffer (a window and its
		 * own parent), where a naive per-pixel copy over overlapping
		 * areas reads pixels it has already written.
		 */
		struct res *src = res_find(get32(r + 4));
		struct res *dst = res_find(get32(r + 8));
		int sx = gets16(r + 16), sy = gets16(r + 18);
		int dx = gets16(r + 20), dy = gets16(r + 22);
		int w = get16(r + 24), h = get16(r + 26);
		static uint32_t row[XSHIM_W];	/* up to 4 bytes a pixel */
		int j, i;

		if (!drawable_ok(src) || !drawable_ok(dst)) {
			send_error(c, X_BAD_DRAWABLE, get32(r + 4), op);
			break;
		}
		if (trace_on())
			fprintf(stderr, "xshim:   copyarea 0x%x(%dx%d) %d,%d "
				"-> 0x%x(%dx%d) %d,%d  %dx%d\n",
				src->id, src->w, src->h, sx, sy,
				dst->id, dst->w, dst->h, dx, dy, w, h);
		if (w > XSHIM_W)
			w = XSHIM_W;
		/*
		 * ANY matching pixel size, not just two bytes.
		 *
		 * This used to require bpp == 2 at both ends, so a depth-32
		 * client fell through to the px_get/px_set fallback - and
		 * those work in uint16_t, so a 32bpp pixel cannot survive the
		 * round trip. st double-buffers into a depth-32 pixmap and
		 * blits it to its window, which meant its entire terminal was
		 * drawn correctly and then thrown away: a window that ran,
		 * accepted input and showed nothing.
		 *
		 * The body below is byte-oriented now, so 1, 2 and 4 bytes per
		 * pixel all take the fast path and none of them lose data.
		 */
		if (src->buf && src->buf->px && dst->buf && op_target(dst) &&
		    src->buf->bpp == dst->buf->bpp && src->buf->bpp) {
			/*
			 * Whole rows at a time. The old loop paid two calls
			 * and ~12 branches per pixel; a blit is the second
			 * hottest thing a toolkit does after fills. The row
			 * buffer stays: source and destination are frequently
			 * the same buffer, and reading the whole row first is
			 * what makes overlap safe in both directions.
			 */
			struct res *sb = src->buf, *db = dst->buf;

			unsigned bp = src->buf->bpp;
			uint8_t *rowb = (uint8_t *)row;

			for (j = 0; j < h; j++) {
				int rx0 = sx + src->ax;
				int ry = sy + j + src->ay;
				int i0, i1;

				/* source run readable inside its clip */
				i0 = src->cx0 - rx0;
				i1 = src->cx1 - rx0;
				if (i0 < 0) i0 = 0;
				if (i1 > w) i1 = w;
				if (ry < src->cy0 || ry >= src->cy1 ||
				    i0 >= i1) {
					memset(rowb, 0, (size_t)w * bp);
				} else {
					if (i0 > 0)
						memset(rowb, 0,
						       (size_t)i0 * bp);
					memcpy(rowb + (size_t)i0 * bp,
					       (const uint8_t *)sb->px +
					       ((size_t)ry * sb->w + rx0 + i0) *
					       bp,
					       (size_t)(i1 - i0) * bp);
					if (i1 < w)
						memset(rowb + (size_t)i1 * bp,
						       0,
						       (size_t)(w - i1) * bp);
				}
				{
					int bx, by, bw;

					if (span_clip(dst, dx, dy + j, w,
						      &bx, &by, &bw))
						memcpy((uint8_t *)db->px +
						       ((size_t)by * db->w + bx)
						       * bp,
						       rowb +
						       (size_t)(bx - (dx +
							dst->ax)) * bp,
						       (size_t)bw * bp);
				}
			}
			damage_add(dst, dx, dy, w, h);
		} else {
			for (j = 0; j < h; j++) {
				for (i = 0; i < w; i++)
					row[i] = px_get(src, sx + i, sy + j);
				for (i = 0; i < w; i++)
					px_set(dst, dx + i, dy + j, row[i]);
			}
			damage_add(dst, dx, dy, w, h);
		}
		notify_draw(dst);
		break;
	}
	case 68:					/* PolyArc */
	case 71: {					/* PolyFillArc */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 12;

		if (!d || !drawable_ok(d) || !g) {
			send_error(c, d ? X_BAD_GC : X_BAD_DRAWABLE,
				   get32(d ? r + 8 : r + 4), op);
			break;
		}
		if (!op_target(d))
			break;
		for (i = 0; i < n; i++) {
			const uint8_t *p = r + 12 + i * 12;
			int x = gets16(p), y = gets16(p + 2);
			int w = get16(p + 4), h = get16(p + 6);
			int a1 = (int16_t)get16(p + 8);
			int a2 = (int16_t)get16(p + 10);

			if (op == 71)
				fill_arc(d, x, y, w, h, a1, a2, g->fg);
			else
				draw_arc(d, x, y, w, h, a1, a2, g->fg);
		}
		notify_draw(d);
		break;
	}
	case 70: {					/* PolyFillRectangle */
		struct res *d = res_find(get32(r + 4));
		struct res *g = res_find(get32(r + 8));
		int i, n = (len - 12) / 8, y;

		if (!d || !drawable_ok(d) || !g) {
			send_error(c, d ? X_BAD_GC : X_BAD_DRAWABLE,
				   get32(d ? r + 8 : r + 4), op);
			break;
		}
		if (!op_target(d))
			break;
		for (i = 0; i < n; i++) {
			const uint8_t *p = r + 12 + i * 8;
			int rx = gets16(p), ry = gets16(p + 2);
			int rw = get16(p + 4), rh = get16(p + 6);

			if (trace_on())
				fprintf(stderr, "xshim:   fillrect on 0x%x "
					"(%dx%d) %d,%d %dx%d fg=0x%04x\n",
					d->id, d->w, d->h, rx, ry, rw, rh,
					g->fg);
			for (y = ry; y < ry + rh; y++)
				px_hspan(d, rx, y, rw, g->fg);
			damage_add(d, rx, ry, rw, rh);
			redraw_child_borders(d, rx, ry, rw, rh);
		}
		notify_draw(d);
		break;
	}

	case 3: {					/* GetWindowAttributes */
		/*
		 * Qt calls this on its own windows during creation and blocks
		 * on the reply. The interesting fields here are map-state and
		 * your-event-mask; the rest are the fixed answers a server
		 * with one visual and one colormap always gives.
		 *
		 * The reply is 44 bytes, not 32: 24 in d24 plus 12 of extra,
		 * so the reply LENGTH field is 3. Getting that wrong desyncs
		 * the client's stream and every later reply is misparsed.
		 */
		struct res *w = res_find(get32(r + 4));
		uint8_t ext[12];

		memset(d24, 0, sizeof d24);
		memset(ext, 0, sizeof ext);
		put32(d24 + 0, VISUAL_ID);
		put16(d24 + 4, 1);			/* InputOutput */
		d24[6] = 0;				/* bit gravity Forget */
		d24[7] = 1;				/* win gravity NorthWest */
		/* backing planes/pixel stay 0; no backing store here */
		d24[16] = 0;				/* save-under False */
		d24[17] = 1;				/* map-is-installed */
		d24[18] = (w && w->mapped) ? 2 : 0;	/* Viewable/Unmapped */
		d24[19] = 0;				/* override-redirect */
		put32(d24 + 20, 0);			/* colormap: None */
		put32(ext + 0, w ? w->event_mask : 0);	/* all-event-masks */
		put32(ext + 4, w ? w->event_mask : 0);	/* your-event-mask */
		send_reply(c, 0 /* backing-store NotUseful */, d24, ext, 12);
		break;
	}
	case 38: {					/* QueryPointer */
		/*
		 * Qt calls this during window creation and BLOCKS. Answering
		 * it wrongly is worse than not answering: the coordinates
		 * decide where menus and tooltips are placed, so a constant
		 * would put every popup in the corner.
		 */
		struct res *w = res_find(get32(r + 4));

		memset(d24, 0, sizeof d24);
		put32(d24 + 0, ROOT_ID);
		put32(d24 + 4, 0);			/* child: None */
		put16(d24 + 8, (uint16_t)ptr_root_x);
		put16(d24 + 10, (uint16_t)ptr_root_y);
		put16(d24 + 12, (uint16_t)(ptr_root_x - (w ? w->x : 0)));
		put16(d24 + 14, (uint16_t)(ptr_root_y - (w ? w->y : 0)));
		put16(d24 + 16, ptr_btn_state);
		send_reply(c, 1 /* same-screen */, d24, NULL, 0);
		break;
	}
	case 40: {					/* TranslateCoordinates */
		/*
		 * Both windows are on the one screen here, so this is just the
		 * difference of their origins. Toolkits use it constantly to
		 * place popups relative to a widget.
		 */
		struct res *sw = res_find(get32(r + 4));
		struct res *dw = res_find(get32(r + 8));
		int sx = gets16(r + 12), sy = gets16(r + 14);
		int rx = (sw ? sw->x : 0) + sx, ry = (sw ? sw->y : 0) + sy;

		memset(d24, 0, sizeof d24);
		put32(d24 + 0, 0);			/* child: None */
		put16(d24 + 4, (uint16_t)(rx - (dw ? dw->x : 0)));
		put16(d24 + 6, (uint16_t)(ry - (dw ? dw->y : 0)));
		send_reply(c, 1 /* same-screen */, d24, NULL, 0);
		break;
	}
	case 22: {					/* SetSelectionOwner */
		uint32_t owner = get32(r + 4), sel = get32(r + 8);
		int i;

		for (i = 0; i < nselown; i++)
			if (selown[i].sel == sel)
				break;
		if (i == nselown && nselown < MAXSEL)
			selown[nselown++].sel = sel;
		if (i < MAXSEL)
			selown[i].owner = owner;
		break;
	}
	case 23: {					/* GetSelectionOwner */
		uint32_t sel = get32(r + 4), owner = 0;
		int i;

		for (i = 0; i < nselown; i++)
			if (selown[i].sel == sel) {
				owner = selown[i].owner;
				break;
			}
		memset(d24, 0, sizeof d24);
		put32(d24, owner);			/* 0 = None */
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 2: {					/* ChangeWindowAttributes */
		/*
		 * Same value list as CreateWindow. Only the event mask and the
		 * background matter here, but the list has to be WALKED in
		 * full - each set bit is four bytes, so skipping one desyncs
		 * every value after it.
		 */
		struct res *w = res_find(get32(r + 4));
		uint32_t mask = get32(r + 8);
		const uint8_t *v = r + 12;
		int bit;

		if (!w)
			break;
		for (bit = 0; bit < 15; bit++) {
			if (!(mask & (1u << bit)))
				continue;
			/*
			 * CWBackPixmap. A client that draws its content into
			 * a pixmap, installs it as the background and then
			 * ClearArea()s to show it gets a black window without
			 * this - the drawing is accepted, lands nowhere
			 * visible, and is then painted over. That is exactly
			 * how xfiles presents its file list.
			 */
			if (bit == 14)
				w->cursor = get32(v);
			if (bit == 0) {
				w->bg_pixmap = get32(v);
							fprintf(stderr, "xshim: win 0x%x background "
					"pixmap = 0x%x%s\n", w->id,
					w->bg_pixmap, "");
			}
			if (bit == 1) { w->bg = get32(v); w->bg_pixmap = 0; }
			if (bit == 3) w->border_pixel = get32(v);
			if (bit == 11) w->event_mask = get32(v);
			v += 4;
		}
		break;
	}
	case 26: {					/* GrabPointer */
		struct res *w = res_find(get32(r + 4));

		if (!w || w->type != R_WINDOW) {
			send_error(c, X_BAD_WINDOW, get32(r + 4), op);
			break;
		}
		memset(d24, 0, sizeof d24);
		if (grab_cli >= 0 && grab_cli != cur_owner) {
			send_reply(c, 1 /* AlreadyGrabbed */, d24, NULL, 0);
			break;
		}
		grab_win = w->id;
		grab_cli = cur_owner;
		grab_owner_ev = r[1];
		grab_confine = get32(r + 12);
		grab_cursor = get32(r + 16);
		fprintf(stderr, "xshim: pointer grabbed by 0x%x\n", w->id);
		send_reply(c, 0 /* Success */, d24, NULL, 0);
		break;
	}
	case 27:					/* UngrabPointer */
		if (grab_cli == cur_owner) {
			fprintf(stderr, "xshim: pointer released by 0x%x\n",
				grab_win);
			grab_win = 0;
			grab_cli = -1;
		}
		break;
	case 31: {					/* GrabKeyboard */
		struct res *w = res_find(get32(r + 4));

		if (!w || w->type != R_WINDOW) {
			send_error(c, X_BAD_WINDOW, get32(r + 4), op);
			break;
		}
		memset(d24, 0, sizeof d24);
		if (kgrab_cli >= 0 && kgrab_cli != cur_owner) {
			send_reply(c, 1, d24, NULL, 0);
			break;
		}
		kgrab_win = w->id;
		kgrab_cli = cur_owner;
		send_reply(c, 0, d24, NULL, 0);
		break;
	}
	case 32:					/* UngrabKeyboard */
		if (kgrab_cli == cur_owner) {
			kgrab_win = 0;
			kgrab_cli = -1;
		}
		break;
	case 41: {					/* WarpPointer */
		/*
		 * dst == None: move by (dst_x, dst_y). Otherwise land at
		 * (dst_x, dst_y) inside dst, which the desktop resolves
		 * through dst's origin in its top-level. The src rectangle
		 * (warp only if the pointer is inside it) is not honoured:
		 * nothing here uses it.
		 */
		struct res *dst = res_find(get32(r + 8));
		int dx = gets16(r + 20), dy = gets16(r + 22);

		if (!warp_cb)
			break;
		if (!dst || dst->type != R_WINDOW) {
			warp_cb(0, dx, dy);
			break;
		}
		if (!dst->buf)
			break;
		warp_cb(dst->buf->id, dst->ax + dx, dst->ay + dy);
		/*
		 * A warp generates MotionNotify, and SDL relies on it: after
		 * recentring the pointer it blocks in XMaskEvent for exactly
		 * that event. TEST TOGGLE XSHIM_WARPMOTION: this synth is
		 * suspected of a warp/motion feedback loop that wedges the
		 * board under sustained relative motion.
		 */
		if (getenv("XSHIM_WARPMOTION")) {
			xshim_pointer(dst->buf->id, dst->ax + dx,
				      dst->ay + dy, 0, 0);
			out_flush(c);
		}
		break;
	}
	case 12: {					/* ConfigureWindow */
		/*
		 * Not ignorable. A toolkit creates its shell window at some
		 * placeholder size, computes the layout, then resizes - so a
		 * shim that drops this presents whatever the placeholder was.
		 * xcalc came up as an 82x40 box for exactly this reason, which
		 * reads as "the shim cannot draw it" rather than "the shim
		 * never resized it".
		 */
		struct res *w = res_find(get32(r + 4));
		uint16_t mask = get16(r + 8);
		const uint8_t *v = r + 12;
		int nw, nh, bit;
		uint8_t d[28];

		if (!w || w->type != R_WINDOW) {
			send_error(c, X_BAD_WINDOW, get32(r + 4), op);
			break;
		}
		int ox = w->x, oy = w->y, ow = w->w, oh = w->h, obw = w->bw;

		nw = w->w; nh = w->h;
		for (bit = 0; bit < 7; bit++) {
			if (!(mask & (1u << bit)))
				continue;
			if (bit == 0) w->x = gets16(v);
			if (bit == 1) w->y = gets16(v);
			if (bit == 2) nw = get16(v);
			if (bit == 3) nh = get16(v);
			if (bit == 4) w->bw = get16(v);
			v += 4;
		}
		if ((nw != w->w || nh != w->h) && nw > 0 && nh > 0) {
			if (w->px) {			/* a buffer owner */
				px_release(w);
				w->w = nw; w->h = nh;
				if (!px_alloc(w, nw, nh)) {
					send_error(c, X_BAD_ALLOC, w->id, op);
					break;
				}
			}
			w->w = nw; w->h = nh;
			geom_update(w);
			win_fill(w, 0, 0, nw, nh);
		} else {
			geom_update(w);
		}
		/*
		 * A border is drawn in the PARENT's pixels, so changing a
		 * child's geometry - oclock reconfigures its border 1 -> 5 px
		 * - leaves the old ring painted and the new one nonexistent
		 * until some unrelated expose happens to repaint the parent.
		 * That made oclock's thick ring appear and vanish depending on
		 * how busy the desktop was. Repaint the union of the old and
		 * new border-inclusive extents in the parent ourselves.
		 */
		if (w->parent != ROOT_ID && w->mapped) {
			struct res *par = res_find(w->parent);

			if (par && par->type == R_WINDOW) {
				int ux0 = ox < w->x ? ox : w->x;
				int uy0 = oy < w->y ? oy : w->y;
				int ux1a = ox + ow + 2 * obw;
				int ux1b = w->x + w->w + 2 * w->bw;
				int uy1a = oy + oh + 2 * obw;
				int uy1b = w->y + w->h + 2 * w->bw;
				int ux1 = ux1a > ux1b ? ux1a : ux1b;
				int uy1 = uy1a > uy1b ? uy1a : uy1b;

				win_fill(par, ux0, uy0, ux1 - ux0, uy1 - uy0);
				redraw_child_borders(par, ux0, uy0,
						     ux1 - ux0, uy1 - uy0);
				damage_add(par, ux0, uy0,
					   ux1 - ux0, uy1 - uy0);
			}
		}
		if (trace_on())
			fprintf(stderr, "       ~win 0x%x -> %dx%d+%d+%d (mask %04x)\n",
				w->id, w->w, w->h, w->x, w->y, mask);
		/* Tell the client where it ended up, then make it repaint. */
		memset(d, 0, sizeof(d));
		put32(d, w->id); put32(d + 4, w->id);
		put16(d + 12, w->x); put16(d + 14, w->y);
		put16(d + 16, w->w); put16(d + 18, w->h);
		send_event(c, 22, d, 28);		/* ConfigureNotify */
		if (w->mapped) {
			/*
			 * Repaint and Expose - but NOT MapNotify.
			 *
			 * This used to call expose_window(), which sends a
			 * MapNotify first. MapNotify means "this window has
			 * just gone from unmapped to mapped", and nothing of
			 * the sort happened: the window was already mapped and
			 * the client merely reconfigured it. Toolkits re-apply
			 * their whole window state when they see one - refresh
			 * the surface, re-grab, re-raise - and a raise is
			 * itself a ConfigureWindow, so the answer to the
			 * configure caused the next configure.
			 *
			 * That closed a loop that no client could escape.
			 * prboom (SDL 1.2) spins inside a single SDL_PollEvent
			 * for ever: measured with an LD_PRELOAD shim, one
			 * "PollEvent enter" and no matching exit in 100 s,
			 * SDL_GetTicks called exactly ONCE, SDL_Flip never.
			 * Doom therefore never runs a tic and never draws, and
			 * the frames on the wire are SDL refreshing an
			 * untouched buffer - which is why the window is black
			 * rather than empty. The trace shows the engine:
			 * "~win 0x200002 -> 320x200+0+0 (mask 0040)" repeating,
			 * mask 0040 being CWStackMode, i.e. XRaiseWindow.
			 */
			paint_subtree(c, w);
			notify_draw(w);
		}
		break;
	}
	case 78: {					/* CreateColormap */
		uint32_t id = get32(r + 4), vis = get32(r + 12);
		int i;

		for (i = 0; i < MAXCMAP; i++)
			if (!cmaps[i].id || cmaps[i].id == id) {
				cmaps[i].id = id;
				cmaps[i].visual = vis;
				break;
			}
		if (trace_on())
			fprintf(stderr, "xshim:   colormap 0x%x visual 0x%x\n",
				id, vis);
		break;
	}
	case 79:					/* FreeColormap */
	case 81:					/* InstallColormap */
	case 82:					/* UninstallColormap */
		/*
		 * Accepted and otherwise ignored. There is one visible
		 * palette because there is one screen and no colour cells to
		 * arbitrate, so creating, installing and freeing a colormap
		 * are all no-ops - but they must SUCCEED. Answering an error
		 * makes SDL abandon the 8-bit visual and fall back to a
		 * 16-bit one, which is the slow path we are trying to leave.
		 */
		break;
	case 89: {					/* StoreColors */
		/*
		 * The palette itself. Items are 12 bytes: a 32-bit pixel,
		 * three 16-bit channels, then a do-red/green/blue mask.
		 * Channels are 16-bit REGARDLESS of the visual's 8 bits per
		 * rgb, so they are scaled down here, not assumed.
		 */
		int n = (len - 8) / 12, i;

		for (i = 0; i < n; i++) {
			const uint8_t *it = r + 8 + i * 12;
			uint32_t pix = get32(it);
			unsigned rr = get16(it + 4), gg = get16(it + 6);
			unsigned bb = get16(it + 8);

			if (pix > 255)
				continue;	/* not our 256 entries */
			pal8[pix] = (uint16_t)(((rr & 0xF800)) |
						 ((gg & 0xFC00) >> 5) |
						 ((bb & 0xF800) >> 11));
		}
		/*
		 * Every expansion of every window this client owns is now
		 * stale. Marking the buffers dirty is enough: the expansion
		 * happens when the desktop next asks for pixels.
		 */
		for (i = 0; i < MAXRES; i++)
			if (res[i].type == R_WINDOW &&
			    res[i].owner == (int)(c - cli) && res[i].bpp == 1)
				res[i].dirty = 1;
		break;
	}
	case 91: {					/* QueryColors */
		int n = (len - 8) / 4, i;
		uint8_t *ext = calloc(n ? n : 1, 8);

		for (i = 0; ext && i < n; i++) {
			uint32_t pix = get32(r + 8 + i * 4) & 0xFF;
			uint16_t v = pal8[pix];

			put16(ext + i * 8 + 0, (uint16_t)((v & 0xF800)));
			put16(ext + i * 8 + 2, (uint16_t)((v & 0x07E0) << 5));
			put16(ext + i * 8 + 4, (uint16_t)((v & 0x001F) << 11));
		}
		memset(d24, 0, sizeof d24);
		put16(d24, (uint16_t)n);
		send_reply(c, 0, d24, ext, n * 8);
		free(ext);
		break;
	}
	case 45: {					/* OpenFont */
		/*
		 * We have exactly one set of glyphs, so the only thing that
		 * matters about a font is its ENCODING. A client that asks for
		 * -adobe-symbol-* wants mathematical glyphs at ASCII
		 * positions: xcalc's radical sign is \326 and its pi is \160,
		 * which in any other font are 'O' and 'p'.
		 */
		struct res *f = res_new(get32(r + 4), R_FONT);
		int n = get16(r + 8);

		if (!f)
			break;
		if (12 + n > len)
			n = len - 12;
		f->font_idx = font_pick(r + 12, n);
		if (trace_on())
			fprintf(stderr, "xshim: OpenFont '%.*s' -> %s\n", n,
				r + 12, xfonts[f->font_idx].alias);
		break;
	}
	case 61: {					/* ClearArea */
		struct res *d = res_find(get32(r + 4));
		int cx = gets16(r + 8), cy = gets16(r + 10);
		int cw = get16(r + 12), ch = get16(r + 14);

		if (!d) {
			send_error(c, X_BAD_DRAWABLE, get32(r + 4), op);
			break;
		}
		if (!cw) cw = d->w - cx;		/* 0 means "to the edge" */
		if (!ch) ch = d->h - cy;
		win_fill(d, cx, cy, cw, ch);
		damage_add(d, cx, cy, cw, ch);
		redraw_child_borders(d, cx, cy, cw, ch);
		/*
		 * r[1] is `exposures`. When set, the client is asking to be
		 * told to repaint what we just erased.
		 */
		if (r[1])
			send_expose(c, d, cx, cy, cw, ch);
		notify_draw(d);
		break;
	}
	case 18: {					/* ChangeProperty */
		/*
		 * Only WM_NAME (predefined atom 39, format 8) is acted on -
		 * it is what puts "xclock" rather than "X client" in the title
		 * bar. Every other property is accepted and dropped; clients
		 * set a dozen of them and never read one back.
		 */
		struct res *w = res_find(get32(r + 4));
		uint32_t prop = get32(r + 8), nch = get32(r + 20);

		if (w && w->type == R_WINDOW && prop == 39 && r[16] == 8) {
			if (nch > sizeof(w->title) - 1)
				nch = sizeof(w->title) - 1;
			if (24 + nch <= (uint32_t)len &&
			    (nch != strlen(w->title) ||
			     memcmp(w->title, r + 24, nch))) {
				memcpy(w->title, r + 24, nch);
				w->title[nch] = 0;
				if (title_cb)
					title_cb(w->id);
			}
		}
		/*
		 * WM_NORMAL_HINTS (predefined atom 40), format 32. The wire
		 * layout is the old XSizeHints: flags, then four obsolete
		 * position/size fields, then min and max. PMinSize is bit 4 and
		 * PMaxSize bit 5; a client that sets both to the same value is
		 * saying it does not resize.
		 */
		if (w && w->type == R_WINDOW && prop == 40 && r[16] == 32 &&
		    nch >= 9 && 24 + nch * 4 <= (uint32_t)len) {
			uint32_t fl = get32(r + 24);

			if (fl & (1u << 4)) {
				w->min_w = (uint16_t)get32(r + 24 + 20);
				w->min_h = (uint16_t)get32(r + 24 + 24);
			}
			if (fl & (1u << 5)) {
				w->max_w = (uint16_t)get32(r + 24 + 28);
				w->max_h = (uint16_t)get32(r + 24 + 32);
			}
			w->has_hints = 1;
			if (trace_on())
				fprintf(stderr, "xshim:   hints 0x%x flags %x "
					"min %ux%u max %ux%u\n", w->id, fl,
					w->min_w, w->min_h, w->max_w, w->max_h);
		}
		break;
	}
	/*
	 * SetSelectionOwner (22) used to be swallowed here. It is implemented
	 * above now, because Qt asks for the owner straight afterwards and
	 * blocks on the answer.
	 *
	 * These seven used to fall through into PutImage, rejected only by its
	 * format and length checks. That was luck, and thinner luck than the
	 * old comment claimed: **opcode 42 is SetInputFocus, whose byte 1 is
	 * revert-to, and RevertToParent is 2 - the same value as ZPixmap**. So
	 * the commonest focus call in X11 passed the format test and reached
	 * the geometry reads, which take iw/ih from r+12 and r+14 on a request
	 * that is only 12 bytes long. Nothing was drawn, because 24 + pad*ih
	 * can never be <= 12, but every such call read past the end of the
	 * request first. GetProperty already answers None unconditionally, so
	 * DeleteProperty genuinely has nothing to do; the rest are server-wide
	 * state this shim does not keep. Ignore them explicitly.
	 */
	case 19:					/* DeleteProperty */
	case 25:					/* SendEvent      */
	case 36:					/* GrabServer     */
	case 37:					/* UngrabServer   */
	case 42:					/* SetInputFocus  */
	case 46:					/* CloseFont      */
	case 109:					/* ChangeHosts    */
		if (trace_on())
			fprintf(stderr, "xshim: ignoring %s (%u)\n",
				opstr(op), op);
		break;
	case 72: {					/* PutImage */
		/*
		 * Accepted and DROPPED until now, which meant any client that
		 * drew an image drew nothing and had no way to find out. It is
		 * the standard way to get pixels onto a drawable, so this was a
		 * hole under every off-the-shelf application, not a slow path.
		 *
		 * Only ZPixmap is handled - the format every toolkit actually
		 * sends - and rows are padded to four bytes, which is what
		 * makes a naive w*h*bpp read tear an image progressively worse
		 * towards the bottom.
		 */
		struct res *d = res_find(get32(r + 4));
		int fmt = r[1], iw = get16(r + 12), ih = get16(r + 14);
		int dx = gets16(r + 16), dy = gets16(r + 18), depth = r[21];
		const uint8_t *src = r + 24;
		int y, x, pad;

		if (getenv("XSHIM_IMGDBG")) {
			/*
			 * Count non-zero source bytes. Geometry being right
			 * tells you the transfer is well formed and nothing
			 * about whether the client computed any colour: a
			 * correct blit of an all-black frame looks identical
			 * to a dropped one on the panel. This is what
			 * separates "we lost it" from "there was nothing".
			 */
			size_t i, nz = 0, tot = (size_t)len > 24 ? len - 24 : 0;

			for (i = 0; i < tot; i++)
				if (r[24 + i])
					nz++;
			fprintf(stderr, "xshim: PutImage nonzero %zu/%zu bytes\n",
				nz, tot);
		}
		if (getenv("XSHIM_IMGDBG"))
			fprintf(stderr,
				"xshim: PutImage dst=0x%x ok=%d fmt=%d %dx%d+%d+%d depth=%d len=%d clip=%d %d,%d-%d,%d dclip=%d,%d-%d,%d ax=%d,%d\n",
				get32(r + 4), drawable_ok(d), fmt, iw, ih,
				dx, dy, depth, len, gcclip_on,
				gcclip_x0, gcclip_y0, gcclip_x1, gcclip_y1,
				d ? d->cx0 : -1, d ? d->cy0 : -1,
				d ? d->cx1 : -1, d ? d->cy1 : -1,
				d ? d->ax : -1, d ? d->ay : -1);
		if (!drawable_ok(d) || fmt != 2 || iw <= 0 || ih <= 0)
			break;
		pad = depth <= 8 ? ((iw + 3) & ~3) :
		      depth <= 16 ? ((iw * 2 + 3) & ~3) : ((iw * 4 + 3) & ~3);
		if (24 + (size_t)pad * ih > (size_t)len)
			break;
		if (op_target(d) && d->buf->bpp == 2) {
			/*
			 * Depth branch hoisted out of the pixel loop, rows
			 * written as runs. An image is the one payload where
			 * per-pixel overhead multiplies by the whole surface.
			 */
			struct res *b = d->buf;

			for (y = 0; y < ih; y++) {
				const uint8_t *row = src + (size_t)y * pad;
				int bx, by, bw, x0;
				uint16_t *out;

				if (!span_clip(d, dx, dy + y, iw,
					       &bx, &by, &bw))
					continue;
				x0 = bx - (dx + d->ax);
				out = b->px + (size_t)by * b->w + bx;
				if (depth <= 8) {
					for (x = 0; x < bw; x++)
						out[x] = row[x0 + x];
				} else if (depth <= 16) {
					memcpy(out, row + (size_t)x0 * 2,
					       (size_t)bw * 2);
				} else {
					const uint8_t *q = row +
						(size_t)x0 * 4;

					for (x = 0; x < bw; x++, q += 4)
						out[x] = (uint16_t)
						    (((q[2] & 0xF8) << 8) |
						     ((q[1] & 0xFC) << 3) |
						     (q[0] >> 3));
				}
			}
		} else if (op_target(d) && d->buf->bpp == 1 && depth <= 8) {
			/*
			 * The same run-per-row treatment for an INDEXED
			 * destination, and for the same reason.
			 *
			 * Without this arm a depth-8 window fell through to
			 * the per-pixel loop below: 64,000 px_set() calls for
			 * one 320x200 frame, each a call plus a clip test plus
			 * an alias check, out of code executing from 80 MHz
			 * XIP flash. Indices are bytes and the destination is
			 * bytes, so the row is a memcpy - which is the whole
			 * point of an indexed visual and was being thrown away
			 * at the last step.
			 */
			struct res *b = d->buf;

			for (y = 0; y < ih; y++) {
				const uint8_t *row = src + (size_t)y * pad;
				int bx, by, bw, x0;

				if (!span_clip(d, dx, dy + y, iw,
					       &bx, &by, &bw))
					continue;
				x0 = bx - (dx + d->ax);
				memcpy((uint8_t *)b->px +
				       (size_t)by * b->w + bx,
				       row + x0, (size_t)bw);
			}
		} else {
			for (y = 0; y < ih; y++) {
				const uint8_t *row = src + (size_t)y * pad;

				int tb = d->buf ? d->buf->bpp : 2;

				for (x = 0; x < iw; x++) {
					uint32_t v;

					if (tb == 4) {
						/*
						 * A 32-bit target keeps
						 * ARGB8888. The alpha byte is
						 * real only for a depth-32
						 * image; a depth-24 one has
						 * padding there, and a
						 * Composite would read 0 as
						 * "invisible".
						 */
						if (depth <= 8)
							v = 0xFF000000u |
							    (row[x] * 0x010101u);
						else if (depth <= 16) {
							unsigned s16 = row[x * 2] |
								(row[x * 2 + 1] << 8);
							v = 0xFF000000u |
							  (((s16 >> 11) << 3) << 16) |
							  ((((s16 >> 5) & 0x3F) << 2) << 8) |
							  ((s16 & 0x1F) << 3);
						} else {
							const uint8_t *q = row + x * 4;
							v = ((uint32_t)(depth >= 32 ?
								q[3] : 0xFF) << 24) |
							    ((uint32_t)q[2] << 16) |
							    ((uint32_t)q[1] << 8) | q[0];
						}
					} else if (depth <= 8) {
						v = row[x];
					} else if (depth <= 16) {
						v = (uint16_t)(row[x * 2] |
						    (row[x * 2 + 1] << 8));
					} else {
						v = (uint16_t)
						 (((row[x * 4 + 2] & 0xF8) << 8) |
						  ((row[x * 4 + 1] & 0xFC) << 3) |
						  (row[x * 4] >> 3));
					}
					px_set(d, dx + x, dy + y, v);
				}
			}
		}
		damage_add(d, dx, dy, iw, ih);
		notify_draw(d);
		break;
	}
	case 93: {					/* CreateCursor */
		/*
		 * Cursor SHAPES are not drawn - lvdesk draws its own pointer -
		 * but a cursor whose mask is all zero draws nothing at all,
		 * and that is how a client hides the pointer (SDL_ShowCursor
		 * off is a 1x1 cursor with an empty mask). Remember which
		 * ones are invisible; the desktop asks before drawing.
		 */
		struct res *rr = res_new(get32(r + 4), R_CURSOR);
		struct res *mk = res_find(get32(r + 12));

		if (!rr)
			break;
		rr->cur_hidden = 0;
		if (mk && mk->type == R_PIXMAP && mk->px) {
			size_t n = (size_t)mk->w * mk->h * mk->bpp, i;
			const uint8_t *b = (const uint8_t *)mk->px;

			for (i = 0; i < n && !b[i]; i++)
				;
			rr->cur_hidden = i == n;
		}
		break;
	}
	case 94:					/* CreateGlyphCursor */
		res_new(get32(r + 4), R_CURSOR);	/* visible */
		break;
	case 95: {					/* FreeCursor */
		struct res *rr = res_find(get32(r + 4));

		if (rr && rr->type == R_CURSOR)
			rr->type = R_FREE;
		break;
	}
	case 127:
		break;					/* NoOperation */

	default:
		/*
		 * Loudly, and never silently: a gap in a protocol
		 * implementation otherwise shows up as a client that hangs
		 * with no clue why, which is the most expensive kind of bug
		 * there is.
		 *
		 * The reply-expecting case is the one that hangs, so it is
		 * called out separately AND answered with an error, which
		 * unblocks the client and makes it print its own diagnosis.
		 */
		if (c->nunimpl[op]++ == 0) {
			fprintf(stderr,
				"xshim: UNIMPLEMENTED %s (opcode %u, detail %u,"
				" len %d)%s\n", opstr(op), op, detail, len,
				expects_reply(op)
					? " - the client is BLOCKED on this"
					: "");
			dump_recent(c);
		}
		send_error(c, X_BAD_IMPLEMENTATION, 0, op);
		break;
	}
}

/* ------------------------------------------------- XFree86-VidMode */
/*
 * Wire layout per xf86vmstr.h, protocol 2.x. A mode record is 48 bytes:
 * dotclock, h{display,syncstart,syncend,total} as CARD16, hskew as CARD32,
 * v{display,syncstart,syncend,total}, pad, flags, three reserved words,
 * privsize. The timings are made up - nothing here has a CRT - but they are
 * consistent, so a client that computes a refresh rate gets 60 Hz.
 */
static void vm_mode_record(uint8_t *m, int i)
{
	unsigned w = vm_modes[i].w, h = vm_modes[i].h;
	unsigned ht = w + 24, vt = h + 8;

	memset(m, 0, 48);
	put32(m + 0, ht * vt * 60 / 1000);	/* dotclock, kHz */
	put16(m + 4, w);
	put16(m + 6, w + 8);
	put16(m + 8, w + 16);
	put16(m + 10, ht);
	put32(m + 12, 0);			/* hskew */
	put16(m + 16, h);
	put16(m + 18, h + 2);
	put16(m + 20, h + 4);
	put16(m + 22, vt);
	put32(m + 28, 0);			/* flags */
	put32(m + 44, 0);			/* privsize */
}

static void vm_switch(int idx, int owner)
{
	if (idx == vm_cur)
		return;
	vm_cur = idx;
	vm_cli = idx ? owner : -1;
	fprintf(stderr, "xshim: video mode %ux%u (client %d)\n",
		vm_modes[idx].w, vm_modes[idx].h, owner);
	if (mode_cb)
		mode_cb(vm_modes[idx].w, vm_modes[idx].h);
}

static void vidmode_request(struct cli *c, const uint8_t *r, int len)
{
	uint8_t minor = r[1];
	uint8_t d24[24];
	static uint8_t modes[sizeof(vm_modes) / sizeof(vm_modes[0]) * 48];
	int i, n = (int)(sizeof(vm_modes) / sizeof(vm_modes[0]));

	memset(d24, 0, sizeof d24);
	switch (minor) {
	case 0:						/* QueryVersion */
		put16(d24 + 0, 2);
		put16(d24 + 2, 2);
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 1: {					/* GetModeLine */
		uint8_t m[48], extra[20];

		vm_mode_record(m, vm_cur);
		/* dotclock @8, then the CARD16 timings packed as the reply wants */
		memcpy(d24 + 0, m + 0, 4);		/* dotclock */
		memcpy(d24 + 4, m + 4, 8);		/* hdisplay..htotal */
		put16(d24 + 12, 0);			/* hskew (CARD16 here) */
		memcpy(d24 + 14, m + 16, 8);		/* vdisplay..vtotal */
		put16(d24 + 22, 0);			/* pad */
		memset(extra, 0, sizeof extra);		/* flags, reserved, privsize */
		send_reply(c, 0, d24, extra, sizeof extra);
		break;
	}
	case 6:						/* GetAllModeLines */
		for (i = 0; i < n; i++)
			vm_mode_record(modes + i * 48, i);
		put32(d24 + 0, n);
		send_reply(c, 0, d24, modes, n * 48);
		break;
	case 10: {					/* SwitchToMode */
		unsigned w = get16(r + 12), h = get16(r + 22);

		for (i = 0; i < n; i++)
			if (vm_modes[i].w == w && vm_modes[i].h == h)
				break;
		if (i == n) {
			send_error(c, X_BAD_VALUE, w << 16 | h, VIDMODE_MAJOR);
			break;
		}
		vm_switch(i, cur_owner);
		break;
	}
	case 9:						/* ValidateModeLine */
		put32(d24 + 0, 0);			/* MODE_OK */
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 11:					/* GetViewPort */
		send_reply(c, 0, d24, NULL, 0);		/* 0,0 */
		break;
	case 16:					/* GetGamma */
		put32(d24 + 0, 10000);			/* 1.0 in the client's units */
		put32(d24 + 4, 10000);
		put32(d24 + 8, 10000);
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 19:					/* GetGammaRampSize: none */
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 4:						/* GetMonitor: nothing known */
		send_reply(c, 0, d24, NULL, 0);
		break;
	case 3: case 5: case 12: case 14: case 15:	/* SwitchMode, Lock, */
		break;			/* SetViewPort, SetClientVersion, SetGamma */
	default:
		if (c->nunimpl[VIDMODE_MAJOR & 127]++ == 0)
			fprintf(stderr, "xshim: VidMode minor %u not implemented\n",
				minor);
		send_error(c, X_BAD_IMPLEMENTATION, 0, VIDMODE_MAJOR);
		break;
	}
	(void)len;
}

/* ------------------------------------------------------------------- API */

int xshim_init(void (*on_window)(uint32_t, int, int),
	       void (*on_draw)(uint32_t), void (*on_close)(uint32_t))
{
	struct sockaddr_un a;
	int i;

	win_cb = on_window;
	draw_cb = on_draw;
	close_cb = on_close;
	pal8_init();
	for (i = 0; i < MAXCLI; i++)
		cli[i].fd = -1;

	mkdir("/tmp/.X11-unix", 0777);
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
 * No compositing pass. Child windows draw straight into the top-level's buffer
 * through px_set()'s offset and clip, so what the client drew IS what we
 * present. The previous version blitted every mapped window in the tree on
 * every draw notification, which for xcalc's 69 windows meant a full
 * re-composite per button repaint.
 */
/*
 * Can this client be resized at all?
 *
 * A client with PMinSize and PMaxSize set to the same extent is fixed by its
 * own declaration - Xt does this for any shell whose geometry is fully
 * constrained, which is why xcalc has never had anything useful to do with a
 * maximise button. Answering 0 lets the desktop remove the affordance instead
 * of offering an operation that can only produce an empty band.
 *
 * Unknown means resizable: a client that sets no hints (xfiles sets none) is
 * making no claim, and X's default is that the window manager decides.
 */
int xshim_window_resizable(uint32_t id)
{
	struct res *r = res_find(id);

	if (!r || r->type != R_WINDOW || !r->has_hints)
		return 1;
	if (!r->min_w || !r->max_w)
		return 1;
	return !(r->min_w == r->max_w && r->min_h == r->max_h);
}

/*
 * Re-home a pixmap's pixels in a memfd so a client can map them.
 *
 * Done on demand rather than for every pixmap: most are never shared, and
 * page-rounding 45 of them would cost more than the traffic it saves. The
 * existing contents are copied across, so this is invisible to anything
 * already drawing into it.
 */
static int px_share(struct res *r)
{
	size_t n = (size_t)r->w * r->h * (r->bpp ? r->bpp : 2);
	void *m;
	int fd;

	if (r->shm_fd >= 0)
		return 1;
	/*
	 * A GEM surface is EXPORTED, not copied: PRIME hands out a dma-buf the
	 * client can map, and XLITE-SHM already passes a descriptor, so the
	 * protocol does not change at all.
	 */
	if (r->gem_src) {
		struct drm_prime_handle ph;
		int dfd = kms_get_fd();

		if (dfd < 0)
			return 0;
		memset(&ph, 0, sizeof ph);
		ph.handle = r->gem_src;
		ph.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(dfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0)
			return 0;
		r->shm_fd = ph.fd;
		r->shm_len = r->gem_len;
		return 1;
	}
	if (!n)
		return 0;
	alias_drop(r);			/* the pixels are about to move */
	fd = memfd_create("xshim-pixmap", 0);
	if (fd < 0)
		return 0;
	if (ftruncate(fd, (off_t)n) < 0) {
		close(fd);
		return 0;
	}
	/*
	 * MAP_POPULATE was tried here and REVERTED, 2026-09-02: maximising
	 * xfiles measured 3149/3623 ms with it against 2549/2722 ms without.
	 * Prefaulting only moves the page-allocation and zeroing cost from
	 * the memcpy's faults into the mmap call; on this board that work is
	 * the expense, not the trap overhead. The one xlite-SHM GetPixmapFd
	 * in a resize still costs 86-355 ms (once 1347 ms) - the fix has to
	 * avoid allocating and copying 733 KB, not fault it more eagerly.
	 */
	m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		close(fd);
		return 0;
	}
	if (r->px)
		memcpy(m, r->px, n);
	else
		memset(m, 0, n);
	free(r->px);
	r->px = m;
	r->shm_fd = fd;
	r->shm_len = n;
	return 1;
}

void xshim_window_resize(uint32_t id, int w, int h)
{
	struct res *r = res_find(id);
	struct cli *c;
	uint8_t d[28];

	if (trace_on())
		fprintf(stderr, "xshim: resize req 0x%x -> %dx%d (res %s, "
			"cur %dx%d, owner %d, hints %d)\n", id, w, h,
			r ? "found" : "MISSING", r ? r->w : -1, r ? r->h : -1,
			r ? r->owner : -1, r ? r->has_hints : -1);
	if (!r || r->type != R_WINDOW || w <= 0 || h <= 0)
		return;
	if (r->w == w && r->h == h)
		return;
	/* Never push a size a client has declared it cannot accept. */
	if (r->has_hints) {
		if (r->max_w && w > r->max_w) w = r->max_w;
		if (r->max_h && h > r->max_h) h = r->max_h;
		if (r->min_w && w < r->min_w) w = r->min_w;
		if (r->min_h && h < r->min_h) h = r->min_h;
		if (r->w == w && r->h == h)
			return;
	}
	if (r->owner < 0 || r->owner >= MAXCLI)
		return;
	c = &cli[r->owner];

	/*
	 * Re-back the top-level at the new size. Only a top-level owns pixels;
	 * children are views into it, so geom_update() re-derives their
	 * offsets and clips against the new buffer.
	 */
	if (r->px) {
		px_release(r);
		r->w = w; r->h = h;
		if (!px_alloc(r, w, h))
			return;
	}
	r->w = w; r->h = h;
	geom_update(r);
	win_fill(r, 0, 0, w, h);

	memset(d, 0, sizeof(d));
	put32(d, r->id); put32(d + 4, r->id);
	put16(d + 12, r->x); put16(d + 14, r->y);
	put16(d + 16, r->w); put16(d + 18, r->h);
	send_event(c, 22, d, 28);		/* ConfigureNotify */
	if (trace_on())
		fprintf(stderr, "xshim: RESIZE 0x%x -> %dx%d, ConfigureNotify "
			"sent to client %d (mask %08x)\n", r->id, r->w, r->h,
			r->owner, r->event_mask);
	if (r->mapped) {
		expose_window(c, r);
		notify_draw(r);
	}
	if (c->fd >= 0)
		out_flush(c);
}

/*
 * The damage accumulated on a window's buffer since the last take, in window
 * coordinates. Returns 1 and clears it, or 0 meaning "unknown - repaint all":
 * a drawing path that never called damage_add() leaves dmg_valid unset, so
 * partial invalidation can only ever under-paint if a path LIES about its
 * rectangle, never because one was forgotten.
 */
int xshim_window_take_damage(uint32_t id, int *x, int *y, int *w, int *h)
{
	struct res *r = res_find(id), *b;

	if (!r || r->type != R_WINDOW)
		return 0;
	b = r->buf;
	if ((!b || !b->dmg_valid) && r->alias) {
		struct res *pm = res_find(r->alias);

		if (pm && pm->dmg_valid)
			b = pm;
	}
	if (!b || !b->dmg_valid)
		return 0;
	*x = b->dmg_x0; *y = b->dmg_y0;
	*w = b->dmg_x1 - b->dmg_x0; *h = b->dmg_y1 - b->dmg_y0;
	b->dmg_valid = 0;
	return 1;
}

/*
 * The raw depth-8 plane, for the direct-expansion path. See xshim.h.
 *
 * This deliberately does NOT touch r->dirty or the shadow: the caller expands
 * on LVGL's own draw pass, which happens exactly when the pixels are needed
 * and already carries the clip rectangle. Keeping the flag out of here means
 * the shadow path stays byte-for-byte what it was, so the two can be compared.
 */
uint32_t xshim_window_gem(uint32_t id)
{
	struct res *r = res_find(id);

	return (r && r->type == R_WINDOW) ? r->gem_src : 0;
}

const uint8_t *xshim_window_indices(uint32_t id, int *w, int *h,
				    int *stride, const uint16_t **pal)
{
	struct res *r = res_find(id);

	if (!r || r->type != R_WINDOW || !r->px || r->bpp != 1)
		return NULL;
	*w = r->w;
	*h = r->h;
	/*
	 * A lone child shares its parent's buffer, so a row steps by the
	 * BUFFER's width, not the window's. Getting this wrong shears the
	 * image by a few pixels a row - which looks like a rendering bug in
	 * the game rather than an arithmetic one here.
	 */
	*stride = r->buf ? r->buf->w : r->w;
	*pal = pal8;
	return (const uint8_t *)r->px;
}

const uint16_t *xshim_window_pixels(uint32_t id, int *w, int *h)
{
	struct res *r = res_find(id);

	if (!r || r->type != R_WINDOW || !r->px)
		return NULL;
	*w = r->w;
	*h = r->h;
	/*
	 * A depth-8 window holds palette INDICES, and the desktop presents
	 * window pixels zero-copy as RGB565 - so an indexed window is expanded
	 * here, into a shadow buffer, through the owning client's palette.
	 *
	 * This is the one place it can happen without the desktop knowing
	 * anything about indexed colour, and it is deliberately the same
	 * buffer the PPA's CLUT would fill: the hardware path replaces the
	 * loop below and nothing else changes. (The S31's PPA has a 256-entry
	 * CLUT and an L8 blend input - verified on silicon; see
	 * docs/sdl-acceleration-plan.md.)
	 *
	 * Only on damage. A window that is not changing costs nothing.
	 */
	/*
	 * A depth-32 window holds ARGB8888. The desktop presents RGB565, so
	 * this converts into the same shadow buffer the palette path uses -
	 * one place, one rule: whatever the client's pixel format, what comes
	 * out of here is what the compositor can blit.
	 *
	 * Alpha is DROPPED, not blended. There is no compositing manager here
	 * and the window is opaque on screen; honouring alpha would mean
	 * reading the desktop underneath every frame.
	 */
	if (r->bpp == 4) {
		const uint32_t *src = (const uint32_t *)r->px;
		size_t n = (size_t)r->w * r->h, i;

		if (!r->shadow) {
			r->shadow = malloc(n * 2);
			if (!r->shadow)
				return NULL;
			r->dirty = 1;
		} else if (!r->dirty) {
			return r->shadow;
		}
		for (i = 0; i < n; i++) {
			uint32_t px = src[i];

			r->shadow[i] = (uint16_t)(((px & 0x00F80000) >> 8) |
						  ((px & 0x0000FC00) >> 5) |
						  ((px & 0x000000F8) >> 3));
		}
		r->dirty = 0;
		return r->shadow;
	}
	if (r->bpp == 1) {
		const uint8_t *src = (const uint8_t *)r->px;
		size_t n = (size_t)r->w * r->h, i;
		const uint16_t *pal;

		if (!r->shadow) {
			r->shadow = malloc(n * 2);
			if (!r->shadow)
				return NULL;
			r->dirty = 1;
		} else if (!r->dirty) {
			return r->shadow;
		}
		pal = pal8;
		/*
		 * EXPANSIONS PER FRAME, printed once every 200 expansions.
		 *
		 * lvdesk measures 48% of the CPU with a zero-copy 320x200
		 * client, where the docs record 18-30% after the buffer share
		 * landed. Each expansion moves ~192 kB of PSRAM (64 kB read,
		 * 128 kB written) and the blit that follows moves another
		 * 256 kB, against a copy ceiling measured at 13.6 MB/s - so
		 * "how many times per frame does this run" decides the frame
		 * rate, and nothing else in this function matters until it is
		 * known. One line per 200 expansions is ~1/s at these rates.
		 */
		{
			static unsigned long nexp;
			static uint64_t t0;
			struct timespec ts;
			uint64_t now;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			now = (uint64_t)ts.tv_sec * 1000ull +
			      (uint64_t)ts.tv_nsec / 1000000ull;
			if (!t0)
				t0 = now;
			if (++nexp % 200 == 0) {
				uint64_t ms = now - t0;

				fprintf(stderr, "xshim: EXPAND %lu total, "
					"200 in %llu ms = %llu/s\n", nexp,
					(unsigned long long)ms,
					(unsigned long long)
					(ms ? 200000ull / ms : 0));
				t0 = now;
			}
		}
		for (i = 0; i < n; i++)
			r->shadow[i] = pal[src[i]];
		/*
		 * CLEAR IT. Without this the early-out above can never fire and
		 * all 64,000 pixels are re-expanded on EVERY repaint pass, not
		 * just the ones carrying new client damage - and repaints are
		 * vblank-quantised at 42 Hz while Doom renders at ~27, so most
		 * passes have nothing new in them. Measured: Doom fell from
		 * 27.2 to 18.6 fps with lvdesk at 38% of the CPU.
		 *
		 * The line was lost on 2026-09-07 in 48a5305, which stripped
		 * the PPA block out of this function; `r->dirty = 0;` sat
		 * directly after that block and went with it. The depth-32 path
		 * added by the SAME commit kept its own, which is why the loss
		 * was asymmetric and survived review.
		 *
		 * Safe because the XLITE-SHM Damaged handler re-arms dirty on
		 * every client damage - see `p->dirty = 1;` there.
		 */
		r->dirty = 0;
		return r->shadow;
	}
	return r->px;
}

/* Device event mask bits we care about. */
#define EV_KEY_PRESS	(1u << 0)
#define EV_KEY_RELEASE	(1u << 1)
#define EV_BTN_PRESS	(1u << 2)
#define EV_BTN_RELEASE	(1u << 3)
#define EV_ENTER	(1u << 4)
#define EV_LEAVE	(1u << 5)
#define EV_MOTION	(1u << 6)
#define EV_FOCUS	(1u << 21)

/*
 * The deepest mapped child containing (x, y), which is where a pointer event
 * belongs. Coordinates come in relative to `w` and are rewritten relative to
 * whatever is returned, because that is what the event carries.
 *
 * Later-created children win, which stands in for stacking order: nothing here
 * restacks, and a toolkit creates its widgets in the order it wants them drawn.
 */
static struct res *hit_test(struct res *w, int *x, int *y)
{
	int i;

	for (;;) {
		struct res *hit = NULL;
		int hx = 0, hy = 0;

		for (i = 0; i < MAXRES; i++) {
			struct res *ch = &res[i];

			if (ch->type != R_WINDOW || ch->parent != w->id ||
			    !ch->mapped)
				continue;
			if (*x < ch->x || *y < ch->y ||
			    *x >= ch->x + ch->w + 2 * ch->bw ||
			    *y >= ch->y + ch->h + 2 * ch->bw)
				continue;
			hit = ch;
			hx = *x - ch->x - ch->bw;
			hy = *y - ch->y - ch->bw;
		}
		if (!hit)
			return w;
		*x = hx; *y = hy;
		w = hit;
	}
}

/*
 * Deliver to the first ancestor that selected this event, which is what X
 * calls propagation. A toolkit selects ButtonPress on the widget window and
 * nothing on the containers around it, so without this every click lands on a
 * window that never asked for one and is dropped.
 */
static uint32_t xshim_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void send_device_event(struct cli *c, uint8_t type, uint8_t detail,
			      struct res *w, int x, int y, uint32_t sel,
			      uint16_t state)
{
	uint8_t d[28];
	uint32_t child = 0;
	int rx = x, ry = y;

	if (trace_on())
		fprintf(stderr, "xshim:   deliver type=%u sel=%08x from 0x%x\n",
			type, sel, w ? w->id : 0);
	while (w && !(w->event_mask & sel)) {
		rx += w->x; ry += w->y;
		child = w->id;
		w = res_find(w->parent);
		if (w && w->type != R_WINDOW)
			return;
	}
	if (!w) {
		if (trace_on())
			fprintf(stderr, "xshim:   DROPPED - nothing selected it\n");
		return;
	}
	if (trace_on())
		fprintf(stderr, "xshim:   -> 0x%x\n", w->id);
	memset(d, 0, sizeof(d));
	/*
	 * A real server time, not 0. SDL's autorepeat filter treats a
	 * KeyRelease followed by a KeyPress of the same key "within 2 ms"
	 * as a repeat and swallows both; with every event stamped 0, three
	 * Enters were one Enter. Milliseconds of CLOCK_MONOTONIC.
	 */
	put32(d + 0, getenv("XSHIM_EVTIME") ? xshim_now_ms() : 0);
	put32(d + 4, ROOT_ID);
	put32(d + 8, w->id);			/* event window */
	put32(d + 12, child);
	put16(d + 16, rx); put16(d + 18, ry);	/* root x/y - no root here */
	put16(d + 20, rx); put16(d + 22, ry);	/* event-relative */
	put16(d + 24, state);
	d[26] = 1;				/* same-screen */
	send_event_d(c, type, detail, d, 28);
}

/*
 * A pointer event from the desktop, in coordinates relative to the top-level
 * whose id is `id`. act: 0 motion, 1 press, 2 release.
 */

static uint32_t ptr_last_top, ptr_last_win;
static int ptr_last_x, ptr_last_y;	/* relative to ptr_last_win */

void xshim_pointer(uint32_t id, int x, int y, int button, int act)
{
#define state ptr_btn_state
	struct res *top = res_find(id), *w;
	struct cli *c;

	if (!top || top->type != R_WINDOW)
		return;
	c = &cli[top->owner];
	if (c->fd < 0)
		return;
	/*
	 * Root-relative position, recorded BEFORE hit_test rewrites x,y into
	 * the deepest child's coordinates. QueryPointer answers in root
	 * coordinates and there is nowhere else to recover them from.
	 */
	ptr_root_x = top->x + x;
	ptr_root_y = top->y + y;

	/*
	 * A grab owns the pointer. Events go to the grab window in its own
	 * coordinates wherever the pointer is - outside it too, which is why
	 * the value can be negative. owner_events (SDL sets it) lets the
	 * grabbing client's OTHER windows take events that fall on them.
	 */
	w = NULL;
	if (grab_win) {
		struct res *g = res_find(grab_win);

		if (g && g->type == R_WINDOW && top_of(g) == top) {
			if (grab_owner_ev && x >= 0 && y >= 0 &&
			    x < top->w && y < top->h)
				w = hit_test(top, &x, &y);
			else {
				x -= g->ax;
				y -= g->ay;
				w = g;
			}
		}
	}
	if (!w)
		w = hit_test(top, &x, &y);
	if (trace_on())
		fprintf(stderr, "xshim: ptr act=%d -> win 0x%x mask=%08x "
			"at %d,%d\n", act, w->id, w->event_mask, x, y);

	/*
	 * Crossing events. EV_ENTER/EV_LEAVE were defined from the start and
	 * never sent, which is why an Xaw button never highlighted under the
	 * pointer: Command's translations bind <EnterWindow>/<LeaveWindow>.
	 * Enter/Leave deliver to the window itself, no propagation; the mode
	 * byte is Normal and the flags byte says same-screen. The pair goes
	 * to each window's OWN owner - with two clients up the old and new
	 * windows are not necessarily the same connection.
	 */
	{
		static uint32_t inside;		/* one pointer, one desktop */

		if (w->id != inside) {
			struct res *ow = res_find(inside);
			uint8_t d[28];

			if (ow && ow->type == R_WINDOW &&
			    (ow->event_mask & EV_LEAVE) &&
			    ow->owner >= 0 && cli[ow->owner].fd >= 0) {
				memset(d, 0, sizeof(d));
				put32(d + 4, ROOT_ID);
				put32(d + 8, ow->id);
				d[26] = 0;		/* NotifyNormal */
				d[27] = 2;		/* same-screen */
				send_event_d(&cli[ow->owner], 8, 0, d, 28);
				out_flush(&cli[ow->owner]);
			}
			if ((w->event_mask & EV_ENTER) &&
			    w->owner >= 0 && cli[w->owner].fd >= 0) {
				memset(d, 0, sizeof(d));
				put32(d + 4, ROOT_ID);
				put32(d + 8, w->id);
				put16(d + 16, x); put16(d + 18, y);
				put16(d + 20, x); put16(d + 22, y);
				d[26] = 0;
				d[27] = 2;
				send_event_d(&cli[w->owner], 7, 0, d, 28);
			}
			inside = w->id;
		}
	}

	if (act == 1) {
		send_device_event(c, 4, button, w, x, y, EV_BTN_PRESS, state);
		state |= 0x100u << (button - 1);	/* Button1Mask.. */
	} else if (act == 2) {
		send_device_event(c, 5, button, w, x, y, EV_BTN_RELEASE, state);
		state &= ~(0x100u << (button - 1));
	} else {
		send_device_event(c, 6, 0, w, x, y, EV_MOTION, state);
	}
	/*
	 * Buttons go out now; motion waits for xshim_flush(), which lvdesk
	 * calls once per loop pass just before it sleeps in poll(). A socket
	 * write is 1-6 ms on this board, and the desktop was paying one per
	 * MotionNotify: 3070 writes and 844 ms of write time in a 25 s
	 * interactive session, ~7% of the core, most of it motion. Batching
	 * costs the client nothing it can see - the flush happens before the
	 * desktop yields the CPU - and a run of motion events coalesces into
	 * one write.
	 */
	if (c->fd >= 0 && act != 0)
		out_flush(c);
	ptr_last_top = id;
	ptr_last_win = w->id;
	ptr_last_x = x;
	ptr_last_y = y;
#undef state
}

/*
 * A key for the client owning top-level `id`. `sym` is already translated by
 * the desktop - a Latin-1 character as itself, or an XLW_ wire code (see
 * xlite/xlite_wirekeys.h). Keys go where the pointer last was inside this
 * top-level, which is the click-to-type model this desktop already uses for
 * focus; if the pointer was last elsewhere they start at the top-level and
 * propagate to whoever selected KeyPress.
 */
void xshim_key(uint32_t id, int sym, int press, unsigned int mods)
{
	struct res *top = res_find(id), *w = NULL;
	struct cli *c;
	int x = 0, y = 0;
	uint16_t st = (uint16_t)((mods & 0x0F) | ptr_btn_state);

	if (!top || top->type != R_WINDOW || sym <= 0 || sym > 255)
		return;
	if (top->owner < 0 || top->owner >= MAXCLI)
		return;
	c = &cli[top->owner];
	if (c->fd < 0)
		return;
	if (ptr_last_top == id) {
		w = res_find(ptr_last_win);
		x = ptr_last_x;
		y = ptr_last_y;
	}
	if (kgrab_win) {
		struct res *g = res_find(kgrab_win);

		if (g && g->type == R_WINDOW && top_of(g) == top) {
			w = g;
			x = y = 0;
		}
	}
	if (!w || w->type != R_WINDOW)
		w = top;
	send_device_event(c, press ? 2 : 3, (uint8_t)sym, w, x, y,
			  press ? EV_KEY_PRESS : EV_KEY_RELEASE, st);
	out_flush(c);
}

int xshim_fds(int *out, int max)
{
	int n = 0, i;

	if (lfd < 0 || max < 1)
		return 0;
	out[n++] = lfd;
	for (i = 0; i < MAXCLI && n < max; i++)
		if (cli[i].fd >= 0)
			out[n++] = cli[i].fd;
	return n;
}

/*
 * Release everything a client owned. `notify` is false when the close came
 * from the consumer (a click on the title bar's X), because it is already
 * tearing the window down and a callback would re-enter it.
 */
static void client_drop(struct cli *c, int notify)
{
	int owner = (int)(c - cli), i;

	free(c->pend);
	c->pend = NULL;
	c->pendn = c->pendcap = 0;
	c->outn = 0;

	if (grab_cli == owner) {
		grab_win = 0;
		grab_cli = -1;
	}
	if (kgrab_cli == owner) {
		kgrab_win = 0;
		kgrab_cli = -1;
	}
	if (vm_cli == owner)
		vm_switch(0, -1);

	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	fprintf(stderr, "xshim: client %d gone after %u requests, "
		"%d answered with an error\n", owner, c->seq, c->nbad);
	if (c->nbad) {
		int j, first = 1;

		fprintf(stderr, "xshim:   unimplemented:");
		for (j = 0; j < 128; j++)
			if (c->nunimpl[j]) {
				fprintf(stderr, "%s %s x%u", first ? "" : ",",
					opstr(j), c->nunimpl[j]);
				first = 0;
			}
		fprintf(stderr, "\n"
			"xshim:   ^ that is the to-do list for this client; "
			"XSHIM_TRACE=1 gives the full request log\n");
	}
	/*
	 * Tell the desktop about EVERY top-level this client owned, mapped or
	 * not.
	 *
	 * The mapped test used to be here, and it leaked a window on every
	 * clean exit: a toolkit unmaps its window while shutting down - SDL
	 * does, and so does Xt - so by the time the socket closes `mapped` is
	 * already 0 and the notification was skipped. lvdesk then kept its
	 * own window object and taskbar entry for a client that no longer
	 * existed: a ghost that could still be raised, dragged and closed,
	 * with no process behind it. prboom leaves one behind every time its
	 * timedemo ends.
	 *
	 * Notifying unconditionally is safe: xwin_on_close() looks the id up
	 * and does nothing if it is not there, so a window that was never
	 * shown costs one failed search.
	 */
	if (notify && close_cb)
		for (i = 0; i < MAXRES; i++)
			if (res[i].type == R_WINDOW && res[i].owner == owner &&
			    res[i].parent == ROOT_ID)
				close_cb(res[i].id);
	for (i = 0; i < MAXRES; i++)
		if (res[i].type != R_FREE && res[i].owner == owner)
			res_free(res[i].id);
	/*
	 * MIT-SHM segments this client attached. ShmDetach is the polite exit
	 * and SDL does send it, but a client that crashes or is killed does
	 * not - and each one then leaks a shmat mapping and one of the eight
	 * slots, so the ninth client to start would silently get no shared
	 * memory at all. Order matters: res_free() above has already handed
	 * back any adopted window buffer, so this only unmaps.
	 */
	for (i = 0; i < MAXSHMSEG; i++)
		if (shmsegs[i].addr && shmsegs[i].owner == owner)
			shmseg_drop(&shmsegs[i]);
	render_drop_client(owner);
}

void xshim_window_close(uint32_t id)
{
	struct res *r = res_find(id);

	if (r && r->type == R_WINDOW)
		client_drop(&cli[r->owner], 0);
}

void xshim_on_title(void (*cb)(uint32_t id))
{
	title_cb = cb;
}

/* FocusIn (9) / FocusOut (10) to whoever in this top-level selected them. */
static void send_focus(struct res *top, int in)
{
	int i;

	for (i = 0; i < MAXRES; i++) {
		struct res *w = &res[i];
		uint8_t d[28];

		if (w->type != R_WINDOW || w->buf != top ||
		    !(w->event_mask & EV_FOCUS) || w->owner < 0 ||
		    cli[w->owner].fd < 0)
			continue;
		memset(d, 0, sizeof(d));
		put32(d + 0, w->id);
		d[4] = 0;			/* mode: NotifyNormal */
		/* detail: NotifyNonlinear, the honest answer between top-levels */
		send_event_d(&cli[w->owner], in ? 9 : 10, 3, d, 28);
		out_flush(&cli[w->owner]);
	}
}

void xshim_focus(uint32_t id)
{
	static uint32_t focused;
	struct res *t;

	if (id == focused)
		return;
	if (focused && (t = res_find(focused)) && t->type == R_WINDOW)
		send_focus(t, 0);
	focused = 0;
	if (id && (t = res_find(id)) && t->type == R_WINDOW) {
		send_focus(t, 1);
		focused = id;
	}
}

void xshim_on_mode(void (*cb)(int w, int h))
{
	mode_cb = cb;
}

uint32_t xshim_mode_window(int w, int h)
{
	int i, fbig = 0;
	uint32_t fits = 0, covering = 0, fallback = 0;

	/*
	 * Which window is the fullscreen one? The client that switched the
	 * mode owns it - wherever it put it. The first rule here was "a
	 * mapped top-level at 0,0 covering the mode", and that is only
	 * SDL's habit at some sizes: `prboom -width 320 -height 240` left
	 * its 320x240 window at 151,81, nothing matched, nothing was ever
	 * presented, and the game ran blind behind a frozen title screen
	 * (2026-09-10). The covering rule stays as the fallback for a client
	 * that switches the mode from a different connection.
	 * The last one created wins: res[] fills in creation order.
	 */
	for (i = 0; i < MAXRES; i++) {
		struct res *t = &res[i];

		if (t->type != R_WINDOW || t->parent != ROOT_ID || !t->mapped)
			continue;
		if (t->x <= 0 && t->y <= 0 && t->x + t->w >= w &&
		    t->y + t->h >= h)
			covering = t->id;
		if (vm_cli >= 0 && t->owner == vm_cli) {
			if (t->w >= w && t->h >= h)
				fits = t->id;
			if (t->w * t->h >= fbig) {
				fbig = t->w * t->h;
				fallback = t->id;
			}
		}
	}
	if (fits)
		return fits;
	if (covering)
		return covering;
	if (fallback)
		fprintf(stderr, "xshim: mode %dx%d: no window of client %d "
			"fits it; presenting its largest, 0x%x\n",
			w, h, vm_cli, fallback);
	return fallback;
}

void xshim_on_warp(void (*cb)(uint32_t top, int x, int y))
{
	warp_cb = cb;
}

uint32_t xshim_grab_top(void)
{
	struct res *g = NULL;

	if (grab_win)
		g = res_find(grab_win);
	else if (kgrab_win)
		g = res_find(kgrab_win);
	g = top_of(g);
	return g ? g->id : 0;
}

void xshim_ungrab_all(void)
{
	if (grab_win || kgrab_win)
		fprintf(stderr, "xshim: grabs released by the desktop\n");
	grab_win = kgrab_win = 0;
	grab_cli = kgrab_cli = -1;
}

int xshim_cursor_hidden(uint32_t top_id, int x, int y)
{
	struct res *top = res_find(top_id), *w, *cur = NULL;
	uint32_t cid = 0;

	if (!top || top->type != R_WINDOW)
		return 0;
	if (grab_win && grab_cursor && top_of(res_find(grab_win)) == top)
		cid = grab_cursor;
	else {
		w = hit_test(top, &x, &y);
		/* None means "inherit from the parent", like the server. */
		while (w && w->type == R_WINDOW && !w->cursor)
			w = res_find(w->parent);
		if (w && w->type == R_WINDOW)
			cid = w->cursor;
	}
	if (cid)
		cur = res_find(cid);
	return cur && cur->type == R_CURSOR && cur->cur_hidden;
}

const char *xshim_window_title(uint32_t id)
{
	struct res *r = res_find(id);

	return (r && r->type == R_WINDOW && r->title[0]) ? r->title : NULL;
}

static void client_data(struct cli *c)
{
	ssize_t n;
	size_t off = 0;
	uint64_t t0 = 0;

	if (xsp_on < 0)
		xsp_on = getenv("XSHIM_PROF") != NULL;
	if (xsp_on)
		t0 = xsp_now();
	{
		/*
		 * recvmsg, not read: a client passing an fd (ShmAttachFd,
		 * ShmCreateSegment) sends it as ancillary data alongside the
		 * request bytes, and a plain read() silently DISCARDS it -
		 * the request then arrives looking perfectly well formed with
		 * no fd behind it.
		 */
		struct msghdr m;
		struct iovec io;
		union {
			struct cmsghdr align;
			char buf[CMSG_SPACE(sizeof(int) * 8)];
		} cm;
		struct cmsghdr *cs;

		io.iov_base = c->in + c->n;
		io.iov_len = sizeof(c->in) - c->n;
		memset(&m, 0, sizeof m);
		m.msg_iov = &io;
		m.msg_iovlen = 1;
		m.msg_control = cm.buf;
		m.msg_controllen = sizeof cm.buf;
		n = recvmsg(c->fd, &m, 0);
		for (cs = CMSG_FIRSTHDR(&m); cs; cs = CMSG_NXTHDR(&m, cs)) {
			int k, cnt;

			if (cs->cmsg_level != SOL_SOCKET ||
			    cs->cmsg_type != SCM_RIGHTS)
				continue;
			cnt = (cs->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (k = 0; k < cnt; k++) {
				int got;

				memcpy(&got, CMSG_DATA(cs) + k * sizeof(int),
				       sizeof got);
				if (c->nrfd < (int)(sizeof(c->rfd) /
						    sizeof(c->rfd[0])))
					c->rfd[c->nrfd++] = got;
				else
					close(got);	/* never leak one */
			}
		}
	}
	if (xsp_on) {
		uint64_t t1 = xsp_now();

		xsp_read += t1 - t0;
		t0 = t1;
	}

	if (n <= 0) {
		client_drop(c, 1);
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

	/*
	 * Still swallowing an oversized request from an earlier read? Discard
	 * before anything is parsed - the bytes in the middle of a request
	 * are not requests, and treating them as such desynchronises the
	 * stream far more destructively than the request itself did.
	 */
	if (c->skip) {
		size_t d = c->skip < c->n ? c->skip : c->n;

		memmove(c->in, c->in + d, c->n - d);
		c->n -= d;
		c->skip -= d;
		if (c->skip)
			return;			/* nothing parseable yet */
	}

	while (c->n - off >= 4) {
		const uint8_t *r = c->in + off;
		int len = get16(r + 2) * 4;

		if (len == 0) {
			fprintf(stderr, "xshim: %s sent a zero-length request "
				"- stream desynchronised, dropping client\n",
				opstr(r[0]));
			dump_recent(c);
			client_drop(c, 1);
			return;
		}
		if ((size_t)len > sizeof(c->in)) {
			/*
			 * Swallow it, do not drop the client.
			 *
			 * The server advertises INBUF/4 as its maximum request
			 * length, but Xlib does not split every request to
			 * respect it: XChangeProperty in particular sends
			 * whatever it is given in one go. SDL2 sets
			 * _NET_WM_ICON from a 128x128 RGBA icon, which is
			 * 65,536 bytes of property plus headers - 65,568, just
			 * over the buffer - and killing the connection for it
			 * took the whole client down at startup.
			 *
			 * Raising INBUF is the wrong trade: it is a per-client
			 * buffer on a board where memory is the binding
			 * constraint, and it only moves the cliff. Discarding
			 * the request keeps the stream in sync at no cost, and
			 * what is lost here is a window icon that lvdesk never
			 * draws - it renders its own chrome.
			 *
			 * A skipped request that expected a reply would leave
			 * its client waiting. Nothing that large has a reply:
			 * the big requests are property and image writes,
			 * which are all one-way.
			 */
			size_t avail = c->n - off;
			size_t drop = (size_t)len < avail ? (size_t)len : avail;

			fprintf(stderr, "xshim: %s is %d bytes, larger than the "
				"%zu-byte input buffer - discarding it\n",
				opstr(r[0]), len, sizeof(c->in));
			c->skip = (size_t)len - drop;
			off += drop;
			continue;
		}
		if (c->n - off < (size_t)len)
			break;
		if (xsp_on) {
			uint64_t th = xsp_now(), dt;
			uint64_t tc = xsp_cpu_now(), dc;
			int key = r[0] < 128 ? r[0] : 128 + (r[1] & 0x7F);

			xshim_px_acc = 0;
			handle(c, r, len);
			dt = xsp_now() - th;
			dc = xsp_cpu_now() - tc;
			xsp_handle += dt;
			xsp_cpu_total += dc;
			xsp_op_ns[key] += dt;
			xsp_op_cpu[key] += dc;
			if (key >= 128)
				xsp_op_major[key] = r[0];
			xsp_op_n[key]++;
			xsp_op_px[key] += xshim_px_acc;
			xsp_calls++;
		} else {
			handle(c, r, len);
		}
		off += len;
	}
	if (off) {
		memmove(c->in, c->in + off, c->n - off);
		c->n -= off;
	}
	out_flush(c);
}

/* Flush every client with buffered output. Called by lvdesk once per loop
 * pass, before it sleeps, so anything deferred (motion) is on the wire
 * before the CPU is yielded. */
void xshim_flush(void)
{
	int i;

	for (i = 0; i < MAXCLI; i++) {
		struct cli *c = &cli[i];

		if (c->fd < 0)
			continue;
		if (c->outn || c->pendn)
			out_flush(c);
		if (c->pendn > PEND_MAX) {
			fprintf(stderr, "xshim: client %d has not read %zu "
				"bytes of events - dropping it\n", i, c->pendn);
			client_drop(c, 1);
		}
	}
}

/*
 * `ready` is the set of our descriptors the CALLER has already found readable.
 *
 * lvdesk's main loop polls every fd it owns, ours included, and then called
 * xshim_poll(), which built its own pollfd array and polled the same
 * descriptors a second time with timeout 0. The second poll cannot learn
 * anything the first did not - no time has passed - and XSHIM_PROF measured it
 * at 13.2 ms/s, ~1.3% of the machine, for nothing. Pass the answer in instead.
 *
 * ready == NULL keeps the old self-polling behaviour, which the standalone
 * build and any other caller still need, and XSHIM_NOPOLLPASS=1 forces it for
 * comparison on one binary.
 */
void xshim_poll_ready(const int *ready, int nready)
{
	struct pollfd p[MAXCLI + 1];
	int map[MAXCLI + 1], n = 0, i;
	static int nopass = -1;

	if (nopass < 0)
		nopass = getenv("XSHIM_NOPOLLPASS") != NULL;
	if (nopass)
		ready = NULL;

	if (lfd < 0)
		return;
	p[n].fd = lfd; p[n].events = POLLIN; map[n] = -1; n++;
	for (i = 0; i < MAXCLI; i++)
		if (cli[i].fd >= 0) {
			p[n].fd = cli[i].fd; p[n].events = POLLIN;
			map[n] = i; n++;
		}
	if (ready) {
		int j, k, any = 0;

		/*
		 * Mark from the caller's list rather than polling. The listen
		 * fd is in this array too, so a new client still connects -
		 * missing that would make new clients silently never appear.
		 */
		for (j = 0; j < n; j++) {
			p[j].revents = 0;
			for (k = 0; k < nready; k++)
				if (ready[k] == p[j].fd) {
					p[j].revents = POLLIN;
					any = 1;
					break;
				}
		}
		if (!any) {
			expose_flush();
			return;
		}
	} else {
	uint64_t tp = 0;
	int pr;

	{
		if (xsp_on > 0)
			tp = xsp_now();
		pr = poll(p, n, 0);
		if (tp) {
			xsp_poll += xsp_now() - tp;
			xsp_dump();
		}
		/*
		 * Still flush. A queued Expose can come from something that is
		 * not client traffic at all - a taskbar click that raises a
		 * window - and this pass then has nothing to poll. Returning
		 * here left the Expose queued until some later pass happened
		 * to have client data, which for a raised-but-idle window is
		 * never: the window stayed on the taskbar and off the screen.
		 */
		if (pr <= 0) {
			expose_flush();
			return;
		}
	}
	}
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
					fprintf(stderr,
						"xshim: client %d connected\n",
						j);
					break;
				}
			if (j == MAXCLI) {
				fprintf(stderr, "xshim: refusing a client, all "
					"%d slots busy - raise MAXCLI\n",
					MAXCLI);
				close(fd);
			}
		} else {
			client_data(&cli[map[i]]);
		}
	}
	expose_flush();
}

void xshim_poll(void)
{
	xshim_poll_ready(NULL, 0);
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
	dump_window(id);
}

int main(void)
{
	int i;

	if (xshim_init(on_window, on_draw, NULL) < 0)
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
