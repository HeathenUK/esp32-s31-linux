/*
 * zc_proto.h - the P2 zero-copy fullscreen protocol's shared arithmetic
 * (docs/gl-plan-2026-09-25.md section 5; artifacts/gl/phase6/ZEROCOPY.txt).
 * MIT.
 *
 * Included by BOTH halves - the server (lvdesk/xshim.c zc_request) and the
 * client (gl/glx/glx_present.c zc_*) - and by the host test
 * (gl/glx/test/zc_test.c), so the fence rules that must agree are written
 * once. Pure functions of integers: no X, no DRM.
 *
 * The request is XLITE-SHM minor 5, discovered by the name "XLITE-ZC":
 *   CARD8 major, CARD8 5, CARD16 length (4, or 5 for PRESENT),
 *   CARD32 window, CARD32 pixmap, CARD8 op, CARD8 pad[3], [CARD32 seq]
 * op ZC_OP_ALLOC replies BYTE granted, BYTE version, CARD16 pitch,
 * CARD32 control-row offset, CARD32 mapping length; ZC_OP_QUERY (pixmap 0)
 * replies BYTE eligible, BYTE version, CARD16 width, CARD16 height - the
 * buffer size ALLOC would take; ZC_OP_PRESENT has no reply. Eligible: a
 * render-scale grant (panel-size fullscreen; the half size), or the
 * drawable covering a VidMode fullscreen top-level (its own size).
 */
#ifndef ZC_PROTO_H
#define ZC_PROTO_H

#include <stdint.h>

#define ZC_MINOR	5
#define ZC_OP_ALLOC	0
#define ZC_OP_PRESENT	1
#define ZC_OP_QUERY	2	/* would ALLOC be granted, and at what size */
#define ZC_MAXBUF	3
#define ZC_MAGIC	0x5a433031u	/* 'ZC01' */

/* The control row: uint32 words past each buffer's image, written only by
 * the server. */
#define ZC_W_MAGIC	0
#define ZC_W_CONSUMED	1	/* newest seq the PPA has finished reading */
#define ZC_W_REVOKED	2	/* render scale gone: remake at native size */
#define ZC_W_DECLINED	3	/* the desktop would not flip: MIT-SHM */

/* How the server handled a PRESENT (kms_zc_flip's return, 0 = copied). */
#define ZC_HOW_COPIED	0	/* copied into the surface, synchronously */
#define ZC_HOW_SAMEFB	1	/* re-presented async (already on the CRTC) */
#define ZC_HOW_FLIPPED	2	/* flipped: the commit's scale is synchronous */

/* Frame numbers wrap: "consumed covers seq" on the circle. */
static inline int zc_seq_done(uint32_t consumed, uint32_t seq)
{
	return (int32_t)(consumed - seq) >= 0;
}

/*
 * What the server publishes as consumed after handling PRESENT(seq), per
 * window. A flip has read the buffer by the time it returns (the commit's
 * scale is synchronous) and, being a PPA op, first drained any async one.
 * An async re-present of the fb already on the CRTC is only known read at
 * the NEXT PPA op, so it covers the frames before it only - and it stays
 * owed until a flip: a copy (the fallback) is a CPU memcpy, which drains
 * nothing, so it may not advance the fence past the owed frame.
 */
struct zc_fence {
	uint32_t owed_seq;	/* an async read of this frame may be running */
	int owed;
};

static inline uint32_t zc_fence_after(struct zc_fence *f, int how,
				      uint32_t seq)
{
	switch (how) {
	case ZC_HOW_FLIPPED:
		f->owed = 0;
		return seq;
	case ZC_HOW_SAMEFB:
		f->owed = 1;
		f->owed_seq = seq;
		return seq - 1;
	default:		/* ZC_HOW_COPIED */
		return f->owed ? f->owed_seq - 1 : seq;
	}
}

/*
 * Rows to allocate for a buffer of h rows at `pitch` bytes: the image, one
 * control row, and padding to a page count of 2 mod 8, so buffers allocated
 * back to back from CMA start 8 KB apart in the 32 KB ways of the 64 KB
 * 2-way D-cache instead of congruent (memory "3-stream penalty is cache
 * aliasing", M2).
 */
static inline uint32_t zc_alloc_rows(uint32_t pitch, uint32_t h)
{
	uint32_t min = h + 1, rows, max;

	if (!pitch)
		return min;
	/* at most 8 pages of padding; whole rows rarely end on a page, so
	 * test the pages they occupy (the kernel rounds the size up) */
	max = min + (8 * 4096 + pitch - 1) / pitch + 1;
	for (rows = min; rows <= max; rows++)
		if (((rows * pitch + 4095) / 4096) % 8 == 2)
			return rows;
	return min;
}

/* The control row's byte offset (right after the image). */
static inline uint32_t zc_ctl_offset(uint32_t pitch, uint32_t h)
{
	return pitch * h;
}

#endif
