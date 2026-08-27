// SPDX-License-Identifier: GPL-2.0-only
/*
 * Time DRM_IOCTL_MODE_DIRTYFB directly, to measure what per-rect damage is
 * actually worth.
 *
 * This deliberately uses no debugfs. The driver's counters are convenient but
 * they are developer diagnostics that compile out, so a measurement that
 * depends on them cannot be repeated on a production kernel - and the thing
 * being changed here is a userspace decision (send a clip list, or send its
 * bounding box), which is measurable from userspace.
 *
 * It takes DRM master, so the desktop has to be stopped first:
 *
 *     /etc/init.d/S40lvdesk stop
 *     dirtybench [reps]
 *
 * The interesting arm is "corners": two small rectangles at opposite ends of
 * the screen. Their bounding box is the whole panel, so the union costs a full
 * 768,000-byte copy while the real damage is a few kilobytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kms.h"

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

/*
 * Touch the pixels the rectangles cover, so the driver has real damage to move
 * and the cache state resembles a genuine repaint rather than a no-op commit.
 */
static void paint(const struct kms_rect *r, int n, unsigned short colour)
{
	int i, x, y;

	for (i = 0; i < n; i++)
		for (y = r[i].y1; y <= r[i].y2; y++) {
			unsigned short *row =
				(unsigned short *)(kms_map + y * kms_pitch);

			for (x = r[i].x1; x <= r[i].x2; x++)
				row[x] = colour;
		}
}

static void run(const char *name, const struct kms_rect *r, int n, int reps,
		int as_union)
{
	long long *t = calloc(reps, sizeof(*t));
	struct kms_rect u = r[0];
	long long total = 0;
	int i;

	for (i = 1; i < n; i++) {
		if (r[i].x1 < u.x1) u.x1 = r[i].x1;
		if (r[i].y1 < u.y1) u.y1 = r[i].y1;
		if (r[i].x2 > u.x2) u.x2 = r[i].x2;
		if (r[i].y2 > u.y2) u.y2 = r[i].y2;
	}

	/* Discard warm-up: the first commits after a modeset page things in. */
	for (i = 0; i < 5; i++) {
		paint(r, n, 0x0000);
		if (as_union)
			kms_dirty(u.x1, u.y1, u.x2, u.y2);
		else
			kms_dirty_rects(r, n);
	}

	for (i = 0; i < reps; i++) {
		long long t0;

		paint(r, n, (i & 1) ? 0xffff : 0x001f);
		t0 = now_us();
		if (as_union)
			kms_dirty(u.x1, u.y1, u.x2, u.y2);
		else
			kms_dirty_rects(r, n);
		t[i] = now_us() - t0;
		total += t[i];
	}

	qsort(t, reps, sizeof(*t), cmp_ll);
	printf("%-22s n=%d %-6s med %6.2f ms  min %6.2f  p90 %6.2f  max %6.2f  mean %6.2f\n",
	       name, n, as_union ? "union" : "rects",
	       (double)t[reps / 2] / 1000.0, (double)t[0] / 1000.0,
	       (double)t[(reps * 9) / 10] / 1000.0, (double)t[reps - 1] / 1000.0,
	       (double)total / reps / 1000.0);
	free(t);
}

int main(int argc, char **argv)
{
	int reps = argc > 1 ? atoi(argv[1]) : 60;
	struct kms_rect corners[2], spread[4], one[1];
	int w, h;

	if (kms_open("/dev/dri/card0") < 0) {
		fprintf(stderr, "dirtybench: no KMS (is lvdesk still running?)\n");
		return 1;
	}
	w = kms_w; h = kms_h;
	memset(kms_map, 0, kms_size);

	/* Two 64x16 patches at opposite corners: the case the union ruins. */
	corners[0] = (struct kms_rect){ 0, 0, 63, 15 };
	corners[1] = (struct kms_rect){ w - 64, h - 16, w - 1, h - 1 };

	/* Four scattered patches, as a window drag plus a clock tick might give. */
	spread[0] = (struct kms_rect){ 0, 0, 63, 15 };
	spread[1] = (struct kms_rect){ w - 64, 0, w - 1, 15 };
	spread[2] = (struct kms_rect){ 0, h - 16, 63, h - 1 };
	spread[3] = (struct kms_rect){ w - 64, h - 16, w - 1, h - 1 };

	/* Control: a single rect, where union and list are the same thing. */
	one[0] = (struct kms_rect){ 0, 0, 63, 15 };

	printf("dirtybench: %ux%u, %d reps\n", kms_w, kms_h, reps);
	run("single 64x16", one, 1, reps, 0);
	run("single 64x16", one, 1, reps, 1);
	run("2 opposite corners", corners, 2, reps, 0);
	run("2 opposite corners", corners, 2, reps, 1);
	run("4 scattered", spread, 4, reps, 0);
	run("4 scattered", spread, 4, reps, 1);
	return 0;
}
