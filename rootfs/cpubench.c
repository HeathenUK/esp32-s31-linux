// SPDX-License-Identifier: GPL-2.0-only
/*
 * Throughput probe, to decide whether reported CPU load is real.
 *
 * /proc/stat and /proc/uptime both claim about half this core is busy while
 * nothing runs, and they share an accounting path, so they cannot confirm each
 * other. This measures actual work done instead: a dependent add chain retires
 * roughly one iteration per cycle on this core, so iterations per second should
 * approach the clock rate if the core is genuinely free, and land near half
 * that if something is really consuming it.
 */

#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
	const uint64_t iters = 200000000ULL;
	struct timespec a, b;
	volatile uint32_t sink;
	uint32_t acc = 1;
	double secs;

	clock_gettime(CLOCK_MONOTONIC, &a);
	for (uint64_t i = 0; i < iters; i++)
		acc += acc ^ (uint32_t)i;	/* serial dependency, no memory */
	clock_gettime(CLOCK_MONOTONIC, &b);
	sink = acc;
	(void)sink;

	secs = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	printf("%llu iterations in %.3f s = %.1f M/s\n",
	       (unsigned long long)iters, secs, iters / secs / 1e6);
	printf("core is 320 MHz; ~1 iteration/cycle means ~320 M/s when free\n");
	return 0;
}
