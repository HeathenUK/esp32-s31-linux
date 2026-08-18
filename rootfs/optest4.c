// SPDX-License-Identifier: GPL-2.0-only
/*
 * Same majority-vote fault hunt as optest3, but the arithmetic never touches
 * memory: the whole chain lives in registers. If this is clean while the
 * buffer-reading version faults, the fault is in loads rather than the ALU.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static uint32_t spin_regs(unsigned int n)
{
	uint32_t h = 2166136261u;

	while (n--)
		h = (h ^ (n & 0xff)) * 16777619u;
	return h;
}

int main(int argc, char **argv)
{
	unsigned int rounds = argc > 1 ? (unsigned int)atoi(argv[1]) : 1000, i;
	unsigned int faulty = 0, all3 = 0;

	for (i = 0; i < rounds; i++) {
		uint32_t a = spin_regs(8192);
		uint32_t b = spin_regs(8192);
		uint32_t c = spin_regs(8192);

		if (a == b && b == c)
			continue;
		faulty++;
		if (a != b && b != c && a != c)
			all3++;
	}
	printf("rounds=%u faulty=%u all3=%u\n", rounds, faulty, all3);
	return 0;
}
