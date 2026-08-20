// SPDX-License-Identifier: GPL-2.0-only
/*
 * Is 2D drawing on this board compute-bound or bandwidth-bound?
 *
 * A terminal scrolls at ~62 lines/s and the client, not the compositor, burns
 * the CPU. Before hand-writing vendor SIMD for pixman it is worth knowing
 * whether pixman is already running at memory speed - if it is, SIMD buys
 * nothing and the answer is to touch fewer pixels instead.
 *
 * Compares raw memcpy/memset against the pixman operations a terminal actually
 * performs, all at panel size, and reports the bytes each one must move so the
 * results can be read against PSRAM's ~90 MB/s.
 */

#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 800
#define H 480

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

#define BENCH(label, bytes, iters, body) do {				\
	double a, b;							\
	int i;								\
	body;			/* warm caches once, untimed */		\
	a = now();							\
	for (i = 0; i < (iters); i++) { body; }				\
	b = now();							\
	printf("  %-30s %7.2f ms  %7.1f MB/s\n", label,			\
	       (b - a) * 1000.0 / (iters),				\
	       (double)(bytes) * (iters) / (b - a) / 1e6);		\
} while (0)

int main(void)
{
	size_t px = (size_t)W * H;
	uint16_t *dst = malloc(px * 2);
	uint16_t *src = malloc(px * 2);
	uint8_t *mask = malloc(px);
	pixman_image_t *pdst, *psrc, *pmask, *solid;
	pixman_color_t white = { 0xffff, 0xffff, 0xffff, 0xffff };

	if (!dst || !src || !mask) { perror("malloc"); return 1; }
	memset(src, 0x5a, px * 2);
	memset(mask, 0x80, px);

	printf("panel %dx%d RGB565, one frame = %zu KB\n", W, H, px * 2 / 1024);
	printf("raw memory:\n");
	BENCH("memset frame", px * 2, 20, memset(dst, 0, px * 2));
	BENCH("memcpy frame (r+w)", px * 4, 20, memcpy(dst, src, px * 2));

	pdst  = pixman_image_create_bits(PIXMAN_r5g6b5, W, H, (uint32_t *)dst, W * 2);
	psrc  = pixman_image_create_bits(PIXMAN_r5g6b5, W, H, (uint32_t *)src, W * 2);
	pmask = pixman_image_create_bits(PIXMAN_a8, W, H, (uint32_t *)mask, W);
	solid = pixman_image_create_solid_fill(&white);

	printf("pixman, full screen:\n");
	BENCH("SRC copy 565->565", px * 4, 20,
	      pixman_image_composite32(PIXMAN_OP_SRC, psrc, NULL, pdst,
				       0, 0, 0, 0, 0, 0, W, H));
	BENCH("OVER solid+a8 mask (glyphs)", px * 4, 20,
	      pixman_image_composite32(PIXMAN_OP_OVER, solid, pmask, pdst,
				       0, 0, 0, 0, 0, 0, W, H));
	{
		pixman_box32_t box = { 0, 0, W, H };
		BENCH("fill rect", px * 2, 20,
		      pixman_image_fill_boxes(PIXMAN_OP_SRC, pdst, &white, 1, &box));
	}

	/* One terminal line: 80 glyphs of 10x18 over the width of the screen. */
	printf("pixman, one 800x18 text line:\n");
	BENCH("OVER solid+a8 mask, 1 line", (size_t)W * 18 * 4, 200,
	      pixman_image_composite32(PIXMAN_OP_OVER, solid, pmask, pdst,
				       0, 0, 0, 0, 0, 0, W, 18));
	return 0;
}
