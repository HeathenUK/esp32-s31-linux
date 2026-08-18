// SPDX-License-Identifier: GPL-2.0-only
/*
 * When a repeated computation disagrees with its first result, work out
 * whether the input changed underneath it or the arithmetic went wrong.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned char buf[8192];
static unsigned char ref[8192];

static uint32_t loop_mul(void)
{
	uint32_t h = 2166136261u;
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++)
		h = (h ^ buf[i]) * 16777619u;
	return h;
}

int main(int argc, char **argv)
{
	unsigned int iters = argc > 1 ? (unsigned int)atoi(argv[1]) : 2000, i;
	unsigned int bad = 0, buf_changed = 0, agreed_on_retry = 0;
	uint32_t r0;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 31 + 7);
	memcpy(ref, buf, sizeof(ref));

	r0 = loop_mul();
	for (i = 0; i < iters; i++) {
		uint32_t v = loop_mul();

		if (v == r0)
			continue;
		bad++;
		if (memcmp(buf, ref, sizeof(buf)))
			buf_changed++;
		if (loop_mul() == r0)
			agreed_on_retry++;
	}
	printf("iters=%u bad=%u buf_changed=%u retry_ok=%u\n",
	       iters, bad, buf_changed, agreed_on_retry);
	return 0;
}
