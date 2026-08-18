/* Compare read bandwidth from cached PSRAM against its uncached alias.
 *
 * This decides whether a bounce buffer is worth building: DMA into uncached
 * memory needs no cache maintenance, but the CPU then has to copy out of it,
 * and that copy is only a win if uncached reads beat the ~14 MB/s the cache
 * maintenance block sustains.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LEN (1u << 20)		/* 1 MiB window */

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double sweep(volatile uint32_t *p, int reps, size_t len)
{
	double t0 = now();
	uint32_t acc = 0;
	for (int r = 0; r < reps; r++)
		for (unsigned i = 0; i < len / 4; i++)
			acc += p[i];
	double dt = now() - t0;
	if (acc == 0xdeadbeef)
		printf("");
	return (double)len * reps / dt / (1024 * 1024);
}

int main(int argc, char **argv)
{
	int reps = argc > 1 ? atoi(argv[1]) : 4;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	struct { const char *name; unsigned long base; size_t len; } r[] = {
		{ "psram-cached  ", 0x50800000, LEN },
		{ "psram-uncached", 0xC0800000, LEN },
		/* Internal SRAM is uncached on this SoC; the audio DMA pool sits
		 * here, so this is a like-for-like read of on-die memory. */
		{ "sram-uncached ", 0x2f062000, 0x8000 },
	};

	if (fd < 0) { perror("/dev/mem"); return 1; }
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
		void *p = mmap(NULL, r[i].len, PROT_READ, MAP_SHARED, fd,
			       r[i].base);
		if (p == MAP_FAILED) {
			printf("UCBW %s mmap failed\n", r[i].name);
			continue;
		}
		printf("UCBW %s %.2f MiB/s\n", r[i].name,
		       sweep(p, reps, r[i].len));
		munmap(p, r[i].len);
	}
	return 0;
}
