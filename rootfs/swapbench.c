// SPDX-License-Identifier: GPL-2.0-only
/*
 * Swap-in cost, measured honestly.
 *
 * An earlier attempt wrote a file to /tmp and read it back. /tmp is tmpfs, so
 * that measured the page cache and reported swap as fast when it had not been
 * touched at all. This allocates anonymous memory instead - the only thing
 * that actually swaps - writes a pattern across all of it so the early pages
 * are evicted while the later ones are written, then reads the whole region
 * back and times that second pass.
 *
 * Per-page cost is what matters here rather than throughput: this board's SD
 * costs ~4.4 ms per REQUEST almost regardless of size (4 KB and 64 KB requests
 * are within 19% of each other), so swap-in speed is set by how many pages the
 * kernel fetches per request - vm.page-cluster - far more than by the card.
 *
 * BIAS WARNING, read before quoting a number from this. Re-reading one large
 * region sequentially is the best possible case for swap readahead, so this
 * tool makes vm.page-cluster look far better than it is: it reports 12.91 s at
 * page-cluster 0 against 3.66 s at 4, a 3.5x spread, while the same setting
 * measured against the real workload (worst of three execs of a library-heavy
 * binary under compositor memory pressure) moves nothing outside run-to-run
 * noise - see /etc/sysctl.d/99-s31-memory.conf. Scattered anonymous pages are
 * not laid out the way this benchmark reads them.
 *
 * Use it to compare swap PATHS - a file against a partition, SD against zram -
 * where the access pattern is held constant. Do not use it to tune readahead.
 *
 * This kernel has CONFIG_VM_EVENT_COUNTERS off, so /proc/vmstat carries no
 * pswpin or pgmajfault and the page counts below read zero. The timings are
 * still sound; the counters are not.
 *
 * Usage: swapbench [megabytes]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned long vmstat_field(const char *name)
{
	char line[256];
	unsigned long v = 0;
	FILE *f = fopen("/proc/vmstat", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, name, strlen(name)) && line[strlen(name)] == ' ') {
			v = strtoul(line + strlen(name), NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

static long meminfo_field(const char *name)
{
	char line[256];
	long v = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, name, strlen(name))) {
			v = strtol(line + strlen(name) + 1, NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 10;
	size_t len = mb * 1024 * 1024;
	long pagesz = sysconf(_SC_PAGESIZE);
	unsigned char *p;
	double t0, t1;
	unsigned long in0, in1, mf0, mf1;
	size_t i;
	volatile unsigned long sink = 0;

	printf("swapbench: %zu MB anonymous, page size %ld, page-cluster %s",
	       mb, pagesz, "(see /proc/sys/vm/page-cluster)\n");
	printf("  SwapFree before: %ld kB   MemAvailable: %ld kB\n",
	       meminfo_field("SwapFree:"), meminfo_field("MemAvailable:"));

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/* Pass 1: dirty every page. Early pages get evicted as later ones land. */
	t0 = now_s();
	for (i = 0; i < len; i += pagesz)
		p[i] = (unsigned char)(i >> 12);
	t1 = now_s();
	printf("  dirty pass:   %6.2f s  (%.0f pages)\n", t1 - t0,
	       (double)(len / pagesz));

	/* Pass 2: read it all back. Whatever was evicted must now swap in. */
	in0 = vmstat_field("pswpin");
	mf0 = vmstat_field("pgmajfault");
	t0 = now_s();
	for (i = 0; i < len; i += pagesz)
		sink += p[i];
	t1 = now_s();
	in1 = vmstat_field("pswpin");
	mf1 = vmstat_field("pgmajfault");
	(void)sink;

	printf("  read-back:    %6.2f s\n", t1 - t0);
	printf("  swapped in:   %lu pages, %lu major faults\n",
	       in1 - in0, mf1 - mf0);
	if (in1 - in0)
		printf("  per page in:  %6.3f ms\n",
		       (t1 - t0) * 1000.0 / (double)(in1 - in0));
	else
		printf("  NOTHING SWAPPED IN - increase the size argument\n");
	printf("  SwapFree after:  %ld kB\n", meminfo_field("SwapFree:"));

	munmap(p, len);
	return 0;
}
