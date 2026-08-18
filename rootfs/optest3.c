// SPDX-License-Identifier: GPL-2.0-only
/*
 * Measure the rate of transient computation faults without trusting any single
 * result. Each round computes the same pure function three times: the answer is
 * the majority, and a round is faulty if the three do not all agree.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned char buf[8192];
static unsigned char ref[8192];

static uint32_t mix(void)
{
	uint32_t h = 2166136261u;
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++)
		h = (h ^ buf[i]) * 16777619u;
	return h;
}

int main(int argc, char **argv)
{
	unsigned int rounds = argc > 1 ? (unsigned int)atoi(argv[1]) : 1000, i;
	unsigned int disagree = 0, buf_changed = 0, all_three_differ = 0;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 31 + 7);
	memcpy(ref, buf, sizeof(ref));

	for (i = 0; i < rounds; i++) {
		uint32_t a = mix(), b = mix(), c = mix();

		if (a == b && b == c)
			continue;
		disagree++;
		if (a != b && b != c && a != c)
			all_three_differ++;
		if (memcmp(buf, ref, sizeof(buf)))
			buf_changed++;
	}
	printf("rounds=%u faulty=%u all3=%u buf_changed=%u\n",
	       rounds, disagree, all_three_differ, buf_changed);
	return 0;
}
