/*
 * memtest - is PSRAM computing and storing what it should? A pattern test
 * for an over-clocked memory domain: fill N MB with a pseudo-random stream
 * (xorshift, seeded per pass), read it back and count mismatches, several
 * passes with different seeds and a moving-inversions pass. Exit status is
 * the mismatch count (capped). Run pinned to each CPU in turn:
 *   oncpu 1 memtest 6 3   # 6 MB, 3 passes, on CPU0
 * Keep it under the board's free memory or it measures swap instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

static uint32_t xs(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 4;
	int passes = argc > 2 ? atoi(argv[2]) : 3;
	size_t n = mb * 1024 * 1024 / 4;
	uint32_t *buf = mmap(NULL, n * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 1; }
	long bad = 0;
	for (int p = 0; p < passes; p++) {
		uint32_t seed = 0x9e3779b9u * (p + 1), s;
		double t0 = now();
		s = seed; for (size_t i = 0; i < n; i++) buf[i] = xs(&s);
		s = seed; long b = 0; for (size_t i = 0; i < n; i++) if (buf[i] != xs(&s)) b++;
		/* moving inversions: invert every word, verify the inverse */
		for (size_t i = 0; i < n; i++) buf[i] = ~buf[i];
		s = seed; for (size_t i = 0; i < n; i++) if (buf[i] != ~xs(&s)) b++;
		bad += b;
		printf("pass %d: %ld mismatches, %.1f MB/s\n", p, b, (double)(n * 4 * 4) / (now() - t0) / 1e6);
	}
	printf("memtest: %zu MB x %d passes, %ld mismatches\n", mb, passes, bad);
	return bad > 255 ? 255 : (int)bad;
}
