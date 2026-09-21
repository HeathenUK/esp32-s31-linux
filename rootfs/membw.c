/*
 * membw - PSRAM streaming bandwidth and random-access latency, from userspace.
 *
 * Built to answer ONE question: does memory itself differ from boot to boot?
 *
 * ~1 boot in 4 runs the windowed desktop ~17% slower, steady for the whole
 * boot, and by 2026-09-21 everything in software had been eliminated - task
 * placement, client memory, lvdesk's own allocations, the scanout buffer, CMA.
 * What is left is below Linux, and the loader spends ~148 ms on every boot in
 * "MSPI Timing: Enter psram timing tuning" picking delay-line settings that
 * then hold for that whole boot. If the calibration lands differently, memory
 * latency differs, and on a board whose shared ceiling IS PSRAM bandwidth
 * (102 MB/s CPU copy vs 177 MB/s in-cache, docs/accel-plan.md) that moves
 * everything memory-bound at once - exactly the observed shape.
 *
 * No /dev/mem and no physical addresses: every userspace page here IS PSRAM,
 * and the buffer is deliberately far larger than the D-cache so the numbers
 * are memory's, not the cache's.
 *
 * LATENCY is the headline, not bandwidth: a timing-tuning difference shows up
 * in access latency long before it shows up in streaming throughput, and the
 * pointer chase is dependent-load by construction so the CPU cannot hide it.
 *
 *   membw [MB]        default 2 MB, prints one line per metric
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 2;
	size_t n = mb * 1024 * 1024, i, steps;
	unsigned char *a, *b;
	volatile uint32_t sink = 0;
	size_t *chase, idx;
	double t0, dt;

	if (mb < 1)
		mb = 1;
	a = malloc(n);
	b = malloc(n);
	if (!a || !b) {
		fprintf(stderr, "membw: cannot allocate 2x%zu MB\n", mb);
		return 1;
	}
	memset(a, 1, n);
	memset(b, 2, n);

	t0 = now_s();
	memcpy(b, a, n);
	memcpy(a, b, n);
	dt = now_s() - t0;
	printf("membw copy_MBs %.2f\n", (2.0 * n / (1024 * 1024)) / dt);

	t0 = now_s();
	memset(a, 3, n);
	memset(b, 4, n);
	dt = now_s() - t0;
	printf("membw fill_MBs %.2f\n", (2.0 * n / (1024 * 1024)) / dt);

	t0 = now_s();
	for (i = 0; i < n; i += 64)
		sink += a[i];
	dt = now_s() - t0;
	printf("membw read_MBs %.2f\n", ((double)n / (1024 * 1024)) / dt);

	/*
	 * Dependent-load pointer chase over a cache-line-strided cycle: each
	 * load's address comes from the previous load, so nothing prefetches
	 * and nothing overlaps. This is the number a timing change moves.
	 */
	steps = n / sizeof(size_t) / 8;
	chase = (size_t *)a;
	for (i = 0; i < steps; i++)
		chase[i * 8] = ((i + 1) % steps) * 8;
	idx = 0;
	t0 = now_s();
	for (i = 0; i < steps; i++)
		idx = chase[idx];
	dt = now_s() - t0;
	printf("membw chase_ns %.2f\n", dt * 1e9 / steps);
	printf("membw sink %u\n", (unsigned)(sink + idx));
	return 0;
}
