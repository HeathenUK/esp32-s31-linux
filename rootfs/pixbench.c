// SPDX-License-Identifier: GPL-2.0-only
/*
 * What does the client's buffer format cost the compositor?
 *
 * Weston's pixman renderer composites each client surface into the output
 * shadow. weston-terminal hands it ARGB8888 (the toytoolkit hardcodes
 * CAIRO_FORMAT_ARGB32), so every frame pays a per-pixel format conversion down
 * to the RGB565 the panel wants. If the client handed over RGB565 instead the
 * same composite is a straight copy.
 *
 * Times both, for the surface sizes we actually run at.
 */
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double bench(pixman_format_code_t sfmt, pixman_format_code_t dfmt,
		    pixman_op_t op, int w, int h, int iters)
{
	int sbpp = PIXMAN_FORMAT_BPP(sfmt) / 8;
	int dbpp = PIXMAN_FORMAT_BPP(dfmt) / 8;
	void *sbuf = calloc(1, (size_t)w * h * sbpp);
	void *dbuf = calloc(1, (size_t)w * h * dbpp);
	pixman_image_t *src, *dst;
	double t0, t1;
	int i;

	if (!sbuf || !dbuf)
		return -1;
	memset(sbuf, 0x7f, (size_t)w * h * sbpp);

	src = pixman_image_create_bits(sfmt, w, h, sbuf, w * sbpp);
	dst = pixman_image_create_bits(dfmt, w, h, dbuf, w * dbpp);
	if (!src || !dst)
		return -1;

	/* warm the caches so the first pass does not skew a short run */
	pixman_image_composite32(op, src, NULL, dst, 0, 0, 0, 0, 0, 0, w, h);

	t0 = now();
	for (i = 0; i < iters; i++)
		pixman_image_composite32(op, src, NULL, dst, 0, 0, 0, 0, 0, 0,
					 w, h);
	t1 = now();

	pixman_image_unref(src);
	pixman_image_unref(dst);
	free(sbuf);
	free(dbuf);
	return (t1 - t0) * 1000.0 / iters;
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 20;
	struct { int w, h; } sizes[] = { { 800, 480 }, { 640, 384 }, { 400, 240 } };
	unsigned i;

	printf("ms per composite, %d iterations each\n", iters);
	printf("%-10s %12s %12s %12s %12s\n", "size",
	       "a8r8g8b8", "r5g6b5", "OVER a8r8", "OVER r5g6");
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		int w = sizes[i].w, h = sizes[i].h;
		char name[16];

		snprintf(name, sizeof(name), "%dx%d", w, h);
		printf("%-10s %12.2f %12.2f %12.2f %12.2f\n", name,
		       bench(PIXMAN_a8r8g8b8, PIXMAN_r5g6b5, PIXMAN_OP_SRC, w, h, iters),
		       bench(PIXMAN_r5g6b5,   PIXMAN_r5g6b5, PIXMAN_OP_SRC, w, h, iters),
		       bench(PIXMAN_a8r8g8b8, PIXMAN_r5g6b5, PIXMAN_OP_OVER, w, h, iters),
		       bench(PIXMAN_r5g6b5,   PIXMAN_r5g6b5, PIXMAN_OP_OVER, w, h, iters));
	}
	return 0;
}
