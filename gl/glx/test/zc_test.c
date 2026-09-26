/*
 * zc_test.c - host test of the P2 zero-copy fence protocol (zc_proto.h).
 * MIT. Build and run on any host:
 *     cc -O2 -Wall -I gl/glx -o /tmp/zc_test gl/glx/test/zc_test.c && /tmp/zc_test
 *
 * A discrete-event model of the two halves as they are written:
 *
 *   client (glx_present.c)  frame_begin waits for zc_seq_done(consumed,
 *                           zseq[cur]); failing that it polls, then makes a
 *                           round trip (XSync = the server handles every
 *                           queued request), then gives up and leaves
 *                           zero-copy (zc_off). It then WRITES the buffer
 *                           (partially, with server steps interleaved),
 *                           finishes, and queues PRESENT(cur, ++seq).
 *   server (xshim.c)        handles one queued request per step, at random
 *                           times: a flip (the buffer's fb is not on the
 *                           CRTC; the scale reads it synchronously), a
 *                           same-fb re-present (an async read completed by
 *                           the NEXT PPA op), or a copy (not live;
 *                           synchronous), and publishes
 *                           zc_fence_after(how, seq).
 *
 * The invariant checked at every READ the PPA (or the copy) makes: the
 * buffer holds exactly the frame that was presented from it, fully written -
 * the client never touched a buffer the hardware had not finished with. The
 * run also checks that the client never waits for ever (every wait resolves
 * within one round trip unless the model deliberately breaks alternation),
 * that frame numbers survive the uint32 wrap, that a new allocation seeded
 * from a stale consumed value cannot satisfy its first wait early, and the
 * cache-stagger arithmetic of zc_alloc_rows.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zc_proto.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } } while (0)

/* ------------------------------------------------------------ the model */

enum { WR_IDLE, WR_PARTIAL };

struct buf {
	uint32_t content;	/* frame fully written into it */
	int writing;		/* the client is mid-frame in it */
	uint32_t zseq;		/* last frame presented from it */
	int pending;
};

struct req { int buf; uint32_t seq; };

static struct model {
	struct buf b[ZC_MAXBUF];
	int nbuf, cur;
	uint32_t seq;		/* client's last frame number */
	uint32_t consumed;	/* the control row word */
	int on_crtc;		/* buffer index whose fb is on the CRTC, -1 none */
	int async_buf;		/* an async (same-fb) read still owed, -1 none */
	uint32_t async_seq;
	struct req q[64];
	int qh, qt;
	int live;		/* the desktop flips (else: copy fallback) */
	int zc_off;
	struct zc_fence fence;
	unsigned long reads, flips, sames, copies, waits_poll, waits_sync,
		      waits_stuck, frames;
} m;

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

static void ppa_read(int bi, uint32_t seq, const char *how)
{
	m.reads++;
	CHECK(!m.b[bi].writing,
	      "%s read of buffer %d (frame %u) while the client writes it",
	      how, bi, (unsigned)seq);
	CHECK(m.b[bi].content == seq,
	      "%s read of buffer %d wanted frame %u, holds %u", how, bi,
	      (unsigned)seq, (unsigned)m.b[bi].content);
}

/* Any PPA op first drains the async one in flight (esp32s31_ppa_drain). */
static void ppa_drain(void)
{
	if (m.async_buf >= 0) {
		ppa_read(m.async_buf, m.async_seq, "async");
		m.async_buf = -1;
	}
}

/* One queued request, handled to completion (xshim zc_request). */
static int server_step(void)
{
	struct req r;
	int how;

	if (m.qh == m.qt)
		return 0;
	r = m.q[m.qh++ % 64];
	if (!m.live) {
		/* zc_copy_frame: a CPU memcpy - it drains NO async PPA job */
		ppa_read(r.buf, r.seq, "copy");
		how = ZC_HOW_COPIED;
		m.copies++;
		/* the copy leaves the CRTC on the mode buffer (kms_fs_dirty) */
		m.on_crtc = -1;
	} else if (m.on_crtc == r.buf) {
		ppa_drain();
		m.async_buf = r.buf;		/* PRESENT_MODE_FB, async */
		m.async_seq = r.seq;
		how = ZC_HOW_SAMEFB;
		m.sames++;
	} else {
		ppa_drain();			/* scale_rect drains first */
		ppa_read(r.buf, r.seq, "flip");	/* synchronous */
		m.on_crtc = r.buf;
		how = ZC_HOW_FLIPPED;
		m.flips++;
	}
	m.consumed = zc_fence_after(&m.fence, how, r.seq);
	return 1;
}

static void maybe_server(int pct)
{
	while ((int)(rnd() % 100) < pct && server_step())
		;
}

/* glx_present.c zc_wait(), with the model's server standing in for time */
static void client_wait(struct buf *b)
{
	int i;

	if (!b->pending)
		return;
	if (zc_seq_done(m.consumed, b->zseq)) {
		b->pending = 0;
		return;
	}
	for (i = 0; i < 16; i++) {		/* the 2 ms poll */
		maybe_server(30);
		if (zc_seq_done(m.consumed, b->zseq)) {
			b->pending = 0;
			m.waits_poll++;
			return;
		}
	}
	while (server_step())			/* XSync */
		;
	m.waits_sync++;
	b->pending = 0;
	if (zc_seq_done(m.consumed, b->zseq))
		return;
	/* the 50 ms poll: time passes, the async read physically ends */
	m.waits_stuck++;
	m.zc_off = 1;
	ppa_drain();
}

static void client_frame(int keep_same)
{
	struct buf *b = &m.b[m.cur];
	uint32_t f;

	client_wait(b);
	f = m.seq + 1;
	b->writing = 1;
	maybe_server(40);			/* the frame takes time */
	maybe_server(40);
	b->content = f;
	b->writing = 0;
	m.seq = f;
	b->zseq = f;
	b->pending = 1;
	CHECK(m.qt - m.qh < 64, "queue overflow");
	m.q[m.qt++ % 64] = (struct req){ m.cur, f };
	m.frames++;
	if (!keep_same)
		m.cur = (m.cur + 1) % m.nbuf;
	maybe_server(50);
}

static void model_reset(int nbuf, uint32_t seed_consumed)
{
	memset(&m, 0, sizeof m);
	m.nbuf = nbuf;
	m.on_crtc = -1;
	m.async_buf = -1;
	m.live = 1;
	m.consumed = seed_consumed;
	/* zc_alloc: number frames on from the window's consumed value */
	m.seq = seed_consumed;
}

/* ------------------------------------------------------------- the runs */

static void run(const char *name, int nbuf, uint32_t seed, int frames,
		int toggle_live)
{
	int i;

	model_reset(nbuf, seed);
	for (i = 0; i < frames; i++) {
		if (toggle_live && rnd() % 97 == 0)
			m.live = !m.live;	/* fullscreen entry/refusal */
		client_frame(0);
	}
	while (server_step())
		;
	ppa_drain();
	CHECK(m.reads == m.frames, "%s: %lu frames but %lu reads", name,
	      m.frames, m.reads);
	CHECK(!m.waits_stuck, "%s: %lu stuck waits with alternating buffers",
	      name, m.waits_stuck);
	printf("%-34s nbuf %d: %lu frames, flips %lu same-fb %lu copies %lu, "
	       "waits poll %lu sync %lu stuck %lu\n", name, nbuf, m.frames,
	       m.flips, m.sames, m.copies, m.waits_poll, m.waits_sync,
	       m.waits_stuck);
}

int main(void)
{
	int k;

	/* arithmetic */
	CHECK(zc_seq_done(5, 5) && zc_seq_done(6, 5) && !zc_seq_done(4, 5),
	      "seq_done basic");
	CHECK(zc_seq_done(2u, 0xfffffffeu) && !zc_seq_done(0xfffffffeu, 2u),
	      "seq_done across the wrap");
	{
		struct zc_fence f = { 0, 0 };

		CHECK(zc_fence_after(&f, ZC_HOW_FLIPPED, 0) == 0, "flip");
		CHECK(zc_fence_after(&f, ZC_HOW_SAMEFB, 10) == 9, "same-fb");
		CHECK(zc_fence_after(&f, ZC_HOW_COPIED, 11) == 9,
		      "a copy after an async re-present must not pass it");
		CHECK(zc_fence_after(&f, ZC_HOW_FLIPPED, 12) == 12 && !f.owed,
		      "a flip drains the owed read");
		CHECK(zc_fence_after(&f, ZC_HOW_COPIED, 13) == 13, "copy");
		CHECK(zc_fence_after(&f, ZC_HOW_SAMEFB, 0) == 0xffffffffu,
		      "same-fb across the wrap");
	}
	{
		uint32_t rows = zc_alloc_rows(800, 240);
		uint32_t bytes = rows * 800;

		/* 251 rows = 200,800 bytes = 50 pages (48 would be the bare
		 * image + control row) */
		CHECK(rows == 251, "400x240: %u rows (want 251 = 50 pages)", rows);
		CHECK(bytes >= zc_ctl_offset(800, 240) + 16, "control row fits");
		CHECK(((bytes + 4095) / 4096) % 8 == 2,
		      "page count 2 mod 8 (%u)", (bytes + 4095) / 4096);
		/* back-to-back buffers 8 KB apart mod 32 KB */
		CHECK((((bytes + 4095) / 4096 * 4096) % 32768) == 8192,
		      "stagger %u", ((bytes + 4095) / 4096 * 4096) % 32768);
	}
	for (k = 64; k <= 800; k += 16) {
		uint32_t rows = zc_alloc_rows((uint32_t)k * 2, 240);

		CHECK(rows > 240 && (uint64_t)rows * k * 2 >=
		      (uint64_t)k * 2 * 241, "rows for width %d", k);
		CHECK((((uint64_t)rows * k * 2 + 4095) / 4096) % 8 == 2,
		      "pages 2 mod 8 at width %d", k);
		CHECK(rows <= 241 + 8 * 4096 / ((uint32_t)k * 2) + 1,
		      "padding bounded at width %d (%u rows)", k, rows);
	}

	/* the protocol */
	run("two buffers", 2, 0, 20000, 0);
	run("three buffers", 3, 0, 20000, 0);
	run("two buffers, live toggling", 2, 0, 20000, 1);
	run("three buffers, live toggling", 3, 0, 20000, 1);
	run("two buffers across the uint32 wrap", 2, 0xffffff00u, 20000, 1);
	/* a re-allocation seeded from a stale consumed value */
	run("two buffers, seeded at 123456", 2, 123456u, 5000, 1);

	/*
	 * Mixed: occasional same-buffer presents (a hypothetical client) with
	 * the desktop toggling live - the copy-after-async case. Stuck waits
	 * are allowed here (they are the designed way out); torn reads are not.
	 */
	model_reset(2, 0);
	for (k = 0; k < 20000; k++) {
		if (rnd() % 97 == 0)
			m.live = !m.live;
		m.zc_off = 0;
		client_frame(rnd() % 13 == 0);
	}
	while (server_step())
		;
	ppa_drain();
	printf("%-34s nbuf 2: %lu frames, same-fb %lu copies %lu, stuck %lu\n",
	       "mixed same-fb + live toggling", m.frames, m.sames, m.copies,
	       m.waits_stuck);

	/*
	 * Broken alternation (a client presenting ONE buffer twice running -
	 * our libGL never does): the async re-present leaves the fence one
	 * short, so the wait must end in "stuck -> MIT-SHM", not a hang and
	 * not a torn read.
	 */
	model_reset(2, 0);
	for (k = 0; k < 200 && !m.zc_off; k++)
		client_frame(1);
	CHECK(m.zc_off, "same-buffer presents must fall back (zc_off)");
	printf("%-34s nbuf 2: fell back after %lu frames (stuck %lu)\n",
	       "one buffer presented twice", m.frames, m.waits_stuck);

	if (fails) {
		printf("zc_test: %d FAILED\n", fails);
		return 1;
	}
	printf("zc_test: all passed\n");
	return 0;
}
