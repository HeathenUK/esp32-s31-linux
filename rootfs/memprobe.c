// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hunt intermittent bad loads and say something useful about them.
 *
 * Every word holds its own index, so a wrong value identifies where it came
 * from: a nearby index means a wrong cache line, a stale value means a
 * coherency problem, and a one-bit difference means marginal timing. Each
 * mismatch is re-read immediately to tell transient from persistent.
 *
 *   memprobe <kib> <passes>            - malloc'd memory (PSRAM, cached)
 *   memprobe <kib> <passes> <physaddr> - /dev/mem, e.g. internal SRAM
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 64;
	unsigned long passes = argc > 2 ? strtoul(argv[2], NULL, 0) : 200;
	unsigned long phys = argc > 3 ? strtoul(argv[3], NULL, 0) : 0;
	unsigned long words = kib * 1024 / sizeof(uint32_t);
	unsigned long bad = 0, transient = 0, shown = 0, p, i;
	volatile uint32_t *b;

	if (phys) {
		int fd = open("/dev/mem", O_RDWR | O_SYNC);

		if (fd < 0)
			return 2;
		b = mmap(NULL, kib * 1024, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, phys);
		if (b == MAP_FAILED)
			return 3;
	} else {
		b = malloc(kib * 1024);
		if (!b)
			return 4;
	}

	for (i = 0; i < words; i++)
		b[i] = (uint32_t)i;

	for (p = 0; p < passes; p++) {
		for (i = 0; i < words; i++) {
			uint32_t got = b[i];

			if (got == (uint32_t)i)
				continue;
			bad++;
			if (b[i] == (uint32_t)i)
				transient++;
			if (shown++ < 6)
				printf("  idx=%lu got=%u delta=%ld xor=%#x\n",
				       i, got, (long)got - (long)i,
				       got ^ (uint32_t)i);
		}
	}
	printf("kib=%lu passes=%lu words=%lu bad=%lu transient=%lu phys=%#lx\n",
	       kib, passes, words, bad, transient, phys);
	return 0;
}
