// SPDX-License-Identifier: GPL-2.0-only
/*
 * Isolate which arithmetic operation misbehaves. Each loop runs the same
 * number of iterations over the same static data and is compared against its
 * own first result.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned char buf[8192];

static uint32_t loop_mul(void)
{
	uint32_t h = 2166136261u;
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++)
		h = (h ^ buf[i]) * 16777619u;
	return h;
}

static uint32_t loop_add(void)
{
	uint32_t h = 1;
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++)
		h = h + buf[i] + (h << 3);
	return h;
}

static uint32_t loop_rot(void)
{
	uint32_t h = 0x12345678;
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++)
		h = ((h << 7) | (h >> 25)) ^ buf[i];
	return h;
}

static uint32_t loop_load(void)
{
	uint32_t h = 0;
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++)
		h ^= buf[i];
	return h;
}

int main(int argc, char **argv)
{
	unsigned int iters = argc > 1 ? (unsigned int)atoi(argv[1]) : 2000, i;
	unsigned int bm = 0, ba = 0, br = 0, bl = 0;
	uint32_t rm, ra, rr, rl;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 31 + 7);

	rm = loop_mul(); ra = loop_add(); rr = loop_rot(); rl = loop_load();
	for (i = 0; i < iters; i++) {
		if (loop_mul() != rm) bm++;
		if (loop_add() != ra) ba++;
		if (loop_rot() != rr) br++;
		if (loop_load() != rl) bl++;
	}
	printf("iters=%u mul_bad=%u add_bad=%u rot_bad=%u load_bad=%u\n",
	       iters, bm, ba, br, bl);
	return 0;
}
