// SPDX-License-Identifier: GPL-2.0-only
/*
 * faultlat - what does ONE swap-in major fault cost, end to end, as the
 * faulting thread sees it?
 *
 * sdlat times a read(2) of the block device; it never enters do_swap_page.
 * The sdtrace ring (dw_mmc) times each request from dw_mci_request() entry
 * to the bio ending. Neither sees the fault entry, the swap-cache and
 * readahead logic, the plug, the folio lock/wake and the return to user.
 * This does: anonymous memory, pushed to swap with MADV_PAGEOUT, then
 * touched one page at a time in random order with CLOCK_MONOTONIC around
 * each touch. mincore() BEFORE each touch (outside the timed window) says
 * whether the page is resident or in the swap cache; only touches of pages
 * that were not are counted as major faults (readahead neighbours become
 * minor faults and are reported separately).
 *
 *   faultlat <MB> <majors> [reset_path] [gap_us] [hog_MB]
 *
 * MADV_PAGEOUT alone is not enough: it writes the pages and unmaps them, but
 * they stay CLEAN IN THE SWAP CACHE until reclaim runs, and a touch then is a
 * swap-cache hit (measured 76-89 us, no SD read). So after the page-out the
 * tool maps and dirties a hog (default 4 MB), which makes reclaim drop those
 * clean folios (the cheapest thing on the LRU), and unmaps it again -
 * repeated until fewer than 5% of the pages are resident.
 *
 * reset_path: written with "0" after the page-out writes have drained and
 * just before the touch phase (use /sys/module/dw_mmc/parameters/sdtrace so
 * the ring then holds exactly the touch phase's requests; <= 60 majors fit
 * its 64 slots). gap_us: sleep between touches (0 = back to back, which
 * keeps the card awake: its idle wake is 5.8 ms after >= ~6-14 ms idle).
 *
 * Output: one MAJ line with every major's microseconds in touch order (for
 * matching against the ring), summary lines, and majflt from getrusage.
 * Every touched page's content is verified (pattern = page index).
 *
 * Written 2026-09-25 for paging plan item 4's first measurement
 * (docs/paging-plan-2026-09-25.md): faultlat major p50 minus the ring's
 * read total p50 is the software share above the driver - the ceiling of
 * the SWP_SYNCHRONOUS_IO fast path.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#define PG 4096

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return x < y ? -1 : x > y;
}

static void stats(const char *tag, long long *v, int n)
{
	long long s = 0;
	int i;

	if (!n) {
		printf("%s n=0\n", tag);
		return;
	}
	for (i = 0; i < n; i++)
		s += v[i];
	qsort(v, n, sizeof(*v), cmp_ll);
	printf("%s n=%d min %lld p50 %lld p90 %lld p99 %lld max %lld mean %lld us\n",
	       tag, n, v[0] / 1000, v[n / 2] / 1000, v[n * 9 / 10] / 1000,
	       v[n * 99 / 100] / 1000, v[n - 1] / 1000, s / n / 1000);
}

static long majflt(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_majflt;
}

static int resident(unsigned char *base, size_t npages, size_t *count)
{
	unsigned char *vec = malloc(npages);
	size_t i, c = 0;

	if (!vec || mincore(base, npages * PG, vec))
		return -1;
	for (i = 0; i < npages; i++)
		c += vec[i] & 1;
	free(vec);
	*count = c;
	return 0;
}

int main(int argc, char **argv)
{
	size_t mb, npages, i, res = 0;
	int want, nmaj = 0, nmin = 0, bad = 0, tries;
	const char *reset = argc > 3 && strcmp(argv[3], "-") ? argv[3] : NULL;
	long gap_us = argc > 4 ? atol(argv[4]) : 0;
	size_t hog_mb = argc > 5 ? (size_t)atol(argv[5]) : 4;
	unsigned char *base;
	size_t *perm;
	long long *maj, *mino, *order;
	long mf0, mf1;
	unsigned int seed;

	if (argc < 3) {
		fprintf(stderr, "usage: faultlat <MB> <majors> [reset_path|-] [gap_us]\n");
		return 2;
	}
	mb = atol(argv[1]);
	want = atoi(argv[2]);
	npages = mb * 1024 * 1024 / PG;
	base = mmap(NULL, npages * PG, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	for (i = 0; i < npages; i++)
		memset(base + i * PG, (int)(i & 0xff) ^ 0x5a, PG);

	for (tries = 0; tries < 4; tries++) {
		if (madvise(base, npages * PG, MADV_PAGEOUT)) {
			perror("madvise(MADV_PAGEOUT)");
			return 1;
		}
		sleep(1);
		if (resident(base, npages, &res))
			return 1;
		if (res * 20 < npages)
			break;
		if (hog_mb) {
			size_t hl = hog_mb * 1024 * 1024, k;
			unsigned char *hog = mmap(NULL, hl, PROT_READ | PROT_WRITE,
						  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			if (hog == MAP_FAILED) {
				perror("mmap hog");
				return 1;
			}
			for (k = 0; k < hl; k += PG)
				hog[k] = 1;
			munmap(hog, hl);
		}
	}
	sleep(2);		/* let the page-out writes drain */
	resident(base, npages, &res);
	printf("PAGES %zu resident_after_pageout %zu tries %d\n", npages, res, tries + 1);

	perm = malloc(npages * sizeof(*perm));
	maj = malloc(npages * sizeof(*maj));
	mino = malloc(npages * sizeof(*mino));
	order = malloc(npages * sizeof(*order));
	if (!perm || !maj || !mino || !order)
		return 1;
	for (i = 0; i < npages; i++)
		perm[i] = i;
	seed = (unsigned int)now_ns();
	for (i = npages - 1; i > 0; i--) {
		size_t j = rand_r(&seed) % (i + 1), t = perm[i];

		perm[i] = perm[j];
		perm[j] = t;
	}

	if (reset) {
		int fd = open(reset, O_WRONLY);

		if (fd < 0 || write(fd, "0", 1) != 1) {
			perror(reset);
			return 1;
		}
		close(fd);
	}

	mf0 = majflt();
	for (i = 0; i < npages && nmaj < want; i++) {
		volatile unsigned char *p = base + perm[i] * PG;
		unsigned char v, in;
		long long t0, t1;

		if (mincore((void *)p, PG, &in))
			return 1;
		t0 = now_ns();
		v = p[PG / 2];
		t1 = now_ns();
		if (v != (unsigned char)(((perm[i] & 0xff)) ^ 0x5a))
			bad++;
		if (in & 1)
			mino[nmin++] = t1 - t0;
		else {
			order[nmaj] = t1 - t0;
			maj[nmaj++] = t1 - t0;
		}
		if (gap_us)
			usleep(gap_us);
	}
	mf1 = majflt();

	printf("MAJ");
	for (i = 0; i < (size_t)nmaj; i++)
		printf(" %lld", order[i] / 1000);
	printf("\n");
	stats("MAJOR", maj, nmaj);
	stats("MINOR", mino, nmin);
	printf("TOUCHED %zu majflt_delta %ld verify_bad %d\n", i, mf1 - mf0, bad);
	return bad ? 1 : 0;
}
