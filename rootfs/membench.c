// SPDX-License-Identifier: GPL-2.0
/*
 * Compare on-chip SRAM against PSRAM for the access patterns that matter to a
 * memory-bound CPU: sequential streaming and pointer-chasing latency.
 *
 * The whole system runs from PSRAM - kernel data, .text.fast, every userspace
 * page - and the CPU measures at ~23% of its theoretical rate, so the question
 * "how much would SRAM buy us" decides whether relocating hot regions is worth
 * the complexity.
 *
 * Usage: membench <hex phys addr> <bytes> <label>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	unsigned long phys = strtoul(argc > 1 ? argv[1] : "0x50c00000", NULL, 0);
	size_t len = argc > 2 ? strtoul(argv[2], NULL, 0) : 16384;
	const char *label = argc > 3 ? argv[3] : "region";
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *p;
	double t0, t1;
	size_t i, reps;
	uint32_t acc = 0;

	if (fd < 0) { perror("open /dev/mem"); return 1; }
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }

	/* Sequential write */
	reps = (16u << 20) / len; if (!reps) reps = 1;
	t0 = now();
	for (size_t r = 0; r < reps; r++)
		for (i = 0; i < len / 4; i++)
			p[i] = (uint32_t)i;
	t1 = now();
	printf("%-8s write  %7.1f MB/s\n", label,
	       (double)len * reps / (t1 - t0) / 1048576.0);

	/* Sequential read */
	t0 = now();
	for (size_t r = 0; r < reps; r++)
		for (i = 0; i < len / 4; i++)
			acc += p[i];
	t1 = now();
	printf("%-8s read   %7.1f MB/s\n", label,
	       (double)len * reps / (t1 - t0) / 1048576.0);

	/*
	 * Pointer chase: each load depends on the previous, so this measures
	 * latency rather than bandwidth. Stride 64 bytes defeats the line.
	 */
	size_t n = len / 64;
	for (i = 0; i < n; i++)
		p[i * 16] = (uint32_t)(((i + 1) % n) * 16);
	size_t idx = 0, hops = 200000;
	t0 = now();
	for (i = 0; i < hops; i++)
		idx = p[idx];
	t1 = now();
	printf("%-8s chase  %7.1f ns/access  (acc %u idx %zu)\n", label,
	       (t1 - t0) * 1e9 / hops, acc, idx);

	munmap((void *)p, len);
	close(fd);
	return 0;
}
