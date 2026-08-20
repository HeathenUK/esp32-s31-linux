// SPDX-License-Identifier: GPL-2.0-only
/*
 * How long does one peripheral register read take?
 *
 * A syscall on this chip costs ~1.4 us, so trap entry is ordinary. Yet an
 * interrupt costs ~95 us, and clock_gettime costs ~2.8 us more than a bare
 * syscall for the sake of a single clocksource read. That points at the
 * peripheral bus rather than the core.
 *
 * Reads only, and of a status register, so this cannot disturb the device.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	unsigned long base = argc > 1 ? strtoul(argv[1], NULL, 0) : 0x20300000UL;
	unsigned long off  = argc > 2 ? strtoul(argv[2], NULL, 0) : 0x14; /* GINTSTS */
	const long n = 20000;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile uint32_t *reg;
	void *map;
	double a, b, empty;
	uint32_t sink = 0;

	if (fd < 0) { perror("open /dev/mem"); return 1; }
	map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, base);
	if (map == MAP_FAILED) { perror("mmap"); return 1; }
	reg = (volatile uint32_t *)((char *)map + off);

	a = now();
	for (long i = 0; i < n; i++)
		__asm__ volatile ("" ::: "memory");
	b = now();
	empty = b - a;

	a = now();
	for (long i = 0; i < n; i++)
		sink += *reg;
	b = now();

	printf("mmio read @0x%lx+0x%lx: %.3f us each (sink %u)\n",
	       base, off, ((b - a) - empty) * 1e6 / n, sink);

	/* A normal DRAM read for comparison, same loop shape. */
	{
		volatile uint32_t *mem = malloc(4096);
		*mem = 1;
		a = now();
		for (long i = 0; i < n; i++)
			sink += *mem;
		b = now();
		printf("ram read:               %.4f us each\n",
		       ((b - a) - empty) * 1e6 / n);
	}
	return 0;
}
