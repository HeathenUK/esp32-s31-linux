// SPDX-License-Identifier: GPL-2.0-only
/*
 * The inner loops cairo/pixman actually run when a terminal redraws, isolated
 * so the compiler flags that build the rootfs can be compared without
 * rebuilding the rootfs.
 *
 * The rootfs is built -Os and without the Espressif vector extension, even
 * though the toolchain supports it. Neither is a source change to fix.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

__attribute__((optimize("Os"))) static void conv_565_os(uint16_t *d, const uint32_t *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		uint32_t p = s[i];

		d[i] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f);
	}
}

__attribute__((optimize("Os"))) static void over_565_os(uint16_t *d, const uint32_t *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		uint32_t p = s[i];
		unsigned a = p >> 24;
		unsigned sr = (p >> 16) & 0xff, sg = (p >> 8) & 0xff, sb = p & 0xff;
		uint16_t q = d[i];
		unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 0x3f) << 2, db = (q & 0x1f) << 3;
		unsigned ia = 255 - a;

		dr = sr + ((dr * ia) >> 8);
		dg = sg + ((dg * ia) >> 8);
		db = sb + ((db * ia) >> 8);
		d[i] = ((dr & 0xf8) << 8) | ((dg & 0xfc) << 3) | (db >> 3);
	}
}

__attribute__((optimize("Os"))) static void glyph_565_os(uint16_t *d, const uint8_t *m, uint32_t colour, unsigned n)
{
	unsigned sr = (colour >> 16) & 0xff, sg = (colour >> 8) & 0xff, sb = colour & 0xff;
	unsigned i;

	for (i = 0; i < n; i++) {
		unsigned a = m[i], ia = 255 - a;
		uint16_t q = d[i];
		unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 0x3f) << 2, db = (q & 0x1f) << 3;

		dr = ((sr * a) >> 8) + ((dr * ia) >> 8);
		dg = ((sg * a) >> 8) + ((dg * ia) >> 8);
		db = ((sb * a) >> 8) + ((db * ia) >> 8);
		d[i] = ((dr & 0xf8) << 8) | ((dg & 0xfc) << 3) | (db >> 3);
	}
}

__attribute__((optimize("O2"))) static void conv_565_o2(uint16_t *d, const uint32_t *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		uint32_t p = s[i];

		d[i] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f);
	}
}

__attribute__((optimize("O2"))) static void over_565_o2(uint16_t *d, const uint32_t *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		uint32_t p = s[i];
		unsigned a = p >> 24;
		unsigned sr = (p >> 16) & 0xff, sg = (p >> 8) & 0xff, sb = p & 0xff;
		uint16_t q = d[i];
		unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 0x3f) << 2, db = (q & 0x1f) << 3;
		unsigned ia = 255 - a;

		dr = sr + ((dr * ia) >> 8);
		dg = sg + ((dg * ia) >> 8);
		db = sb + ((db * ia) >> 8);
		d[i] = ((dr & 0xf8) << 8) | ((dg & 0xfc) << 3) | (db >> 3);
	}
}

__attribute__((optimize("O2"))) static void glyph_565_o2(uint16_t *d, const uint8_t *m, uint32_t colour, unsigned n)
{
	unsigned sr = (colour >> 16) & 0xff, sg = (colour >> 8) & 0xff, sb = colour & 0xff;
	unsigned i;

	for (i = 0; i < n; i++) {
		unsigned a = m[i], ia = 255 - a;
		uint16_t q = d[i];
		unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 0x3f) << 2, db = (q & 0x1f) << 3;

		dr = ((sr * a) >> 8) + ((dr * ia) >> 8);
		dg = ((sg * a) >> 8) + ((dg * ia) >> 8);
		db = ((sb * a) >> 8) + ((db * ia) >> 8);
		d[i] = ((dr & 0xf8) << 8) | ((dg & 0xfc) << 3) | (db >> 3);
	}
}

__attribute__((optimize("O3"))) static void conv_565_o3(uint16_t *d, const uint32_t *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		uint32_t p = s[i];

		d[i] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f);
	}
}

__attribute__((optimize("O3"))) static void over_565_o3(uint16_t *d, const uint32_t *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		uint32_t p = s[i];
		unsigned a = p >> 24;
		unsigned sr = (p >> 16) & 0xff, sg = (p >> 8) & 0xff, sb = p & 0xff;
		uint16_t q = d[i];
		unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 0x3f) << 2, db = (q & 0x1f) << 3;
		unsigned ia = 255 - a;

		dr = sr + ((dr * ia) >> 8);
		dg = sg + ((dg * ia) >> 8);
		db = sb + ((db * ia) >> 8);
		d[i] = ((dr & 0xf8) << 8) | ((dg & 0xfc) << 3) | (db >> 3);
	}
}

__attribute__((optimize("O3"))) static void glyph_565_o3(uint16_t *d, const uint8_t *m, uint32_t colour, unsigned n)
{
	unsigned sr = (colour >> 16) & 0xff, sg = (colour >> 8) & 0xff, sb = colour & 0xff;
	unsigned i;

	for (i = 0; i < n; i++) {
		unsigned a = m[i], ia = 255 - a;
		uint16_t q = d[i];
		unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 0x3f) << 2, db = (q & 0x1f) << 3;

		dr = ((sr * a) >> 8) + ((dr * ia) >> 8);
		dg = ((sg * a) >> 8) + ((dg * ia) >> 8);
		db = ((sb * a) >> 8) + ((db * ia) >> 8);
		d[i] = ((dr & 0xf8) << 8) | ((dg & 0xfc) << 3) | (db >> 3);
	}
}

int main(int argc, char **argv)
{
	unsigned n = 400 * 240;
	int iters = argc > 1 ? atoi(argv[1]) : 20;
	uint32_t *src = malloc(n * 4);
	uint16_t *dst = malloc(n * 2);
	uint8_t *mask = malloc(n);
	double t;
	int i;

	if (!src || !dst || !mask)
		return 1;
	memset(src, 0x80, n * 4);
	memset(dst, 0x33, n * 2);
	memset(mask, 0x55, n);

	printf("ms per 400x240 pass, %d iterations\n", iters);
	printf("%-12s %8s %8s %8s\n", "loop", "-Os", "-O2", "-O3");

#define TIME(fn) ({ t = now(); for (i = 0; i < iters; i++) fn; (now() - t) * 1000 / iters; })
	printf("%-12s %8.2f %8.2f %8.2f\n", "conv_565",
	       TIME(conv_565_os(dst, src, n)),
	       TIME(conv_565_o2(dst, src, n)),
	       TIME(conv_565_o3(dst, src, n)));
	printf("%-12s %8.2f %8.2f %8.2f\n", "over_565",
	       TIME(over_565_os(dst, src, n)),
	       TIME(over_565_o2(dst, src, n)),
	       TIME(over_565_o3(dst, src, n)));
	printf("%-12s %8.2f %8.2f %8.2f\n", "glyph_565",
	       TIME(glyph_565_os(dst, mask, 0x00c0c0c0, n)),
	       TIME(glyph_565_o2(dst, mask, 0x00c0c0c0, n)),
	       TIME(glyph_565_o3(dst, mask, 0x00c0c0c0, n)));
	return 0;
}
