// SPDX-License-Identifier: GPL-2.0-only
/*
 * Repeat a fixed integer computation and check it always gives the same
 * answer. Nothing here touches storage or the network: if the results differ,
 * userspace computation itself is unreliable on this platform.
 *
 * Also exercises unaligned 32-bit loads separately, since those are the usual
 * suspect when one program miscomputes and its neighbours do not.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static unsigned char buf[8192];

static uint32_t fnv(const unsigned char *p, unsigned int len)
{
	uint32_t h = 2166136261u;

	while (len--) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

/* Word-at-a-time, from a deliberately odd offset. */
static uint32_t words(const unsigned char *p, unsigned int len, unsigned int off)
{
	uint32_t h = 0;
	unsigned int i;

	for (i = off; i + 4 <= len; i += 4) {
		uint32_t v;

		memcpy(&v, p + i, sizeof(v));
		h = (h << 1 | h >> 31) ^ v;
	}
	return h;
}

int main(int argc, char **argv)
{
	unsigned int i, iters = argc > 1 ? (unsigned int)atoi(argv[1]) : 2000;
	uint32_t ref_b, ref_w0, ref_w1;
	unsigned int bad_b = 0, bad_w0 = 0, bad_w1 = 0;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 31 + 7);

	ref_b = fnv(buf, sizeof(buf));
	ref_w0 = words(buf, sizeof(buf), 0);
	ref_w1 = words(buf, sizeof(buf), 1);

	for (i = 0; i < iters; i++) {
		if (fnv(buf, sizeof(buf)) != ref_b)
			bad_b++;
		if (words(buf, sizeof(buf), 0) != ref_w0)
			bad_w0++;
		if (words(buf, sizeof(buf), 1) != ref_w1)
			bad_w1++;
	}
	printf("iters=%u bytewise_bad=%u aligned_bad=%u unaligned_bad=%u\n",
	       iters, bad_b, bad_w0, bad_w1);
	return (bad_b || bad_w0 || bad_w1) ? 1 : 0;
}
