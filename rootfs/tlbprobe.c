/*
 * tlbprobe - where is the TLB knee? A dependent pointer chase over N pages,
 * one load per page, stride = one page, so every load is a new TLB entry.
 * Sweep N; the ns/load steps up where the walker starts missing. That
 * number decides whether 4 MB megapages for the RV32 linear map (C32,
 * docs/perf-plan-2026-09-23.md) can pay: with Sv32, a miss is two
 * uncached PSRAM reads (~0.4-0.5 us) because the walker does not snoop the
 * D-cache. Prints ns per dependent load for each page count.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
	int maxpages = argc > 1 ? atoi(argv[1]) : 512;
	size_t page = 4096;
	char *buf = mmap(NULL, (size_t)maxpages * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 1; }
	memset(buf, 0, (size_t)maxpages * page);	/* fault everything in first */
	for (int n = 4; n <= maxpages; n *= 2) {
		/* chain: page i -> page (i+1) % n, offset rotated by 64 B per page
		 * so the loads do not share a cache set and the D-cache does not
		 * decide the result */
		for (int i = 0; i < n; i++) {
			size_t off = (size_t)i * page + (size_t)((i * 64) % page);
			size_t next = (size_t)((i + 1) % n) * page + (size_t)(((i + 1) % n * 64) % page);
			*(volatile size_t *)(buf + off) = (size_t)(buf + next);
		}
		volatile size_t *p = (volatile size_t *)buf;
		long iters = 200000;
		double t0 = now();
		for (long k = 0; k < iters; k++)
			p = (volatile size_t *)*p;
		double dt = now() - t0;
		printf("pages %5d  %8.1f ns/load  (%d KB)\n", n, dt * 1e9 / iters, n * 4);
		if ((size_t)p == 1) printf("x");	/* keep the chase alive */
	}
	return 0;
}
