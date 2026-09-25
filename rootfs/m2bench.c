/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * m2bench - GL plan prerequisite M2 (docs/tinygl-hw-accel-assessment-2026-09-25.md
 * section 2.3). Is blendbench's "3-stream 18x on dumb buffers" write-combine,
 * or cache-set aliasing between congruent buffers?
 *
 * The same 3-stream loop (dst = a + b, 16-bit) runs over:
 *   dumb   - three DRM dumb buffers as CMA allocated them
 *   dumbof - the same buffers, b offset by 16 KB and dst by 8 KB
 *   heap   - three plain mallocs
 *   heapal - three heap buffers aligned to 256 KB (congruent, like CMA)
 * All accesses go through volatile pointers, so every arm does the same
 * 16-bit loads and stores. If heapal is slow, it is aliasing, not the mapping.
 *
 *   m2bench [w h reps]    (default 400 300 5)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static double now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void *dumb(int fd, uint32_t w, uint32_t h, uint64_t *size)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 16 };
	struct drm_mode_map_dumb m = { 0 };
	void *p;

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		return NULL;
	m.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		return NULL;
	p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	*size = c.size;
	return p == MAP_FAILED ? NULL : p;
}

static double run(volatile uint16_t *a, volatile uint16_t *b,
		  volatile uint16_t *d, size_t n)
{
	double t0 = now_us();
	size_t i;

	for (i = 0; i < n; i++)
		d[i] = (uint16_t)(a[i] + b[i]);
	return now_us() - t0;
}

int main(int argc, char **argv)
{
	uint32_t w = argc > 2 ? (uint32_t)atoi(argv[1]) : 400;
	uint32_t h = argc > 2 ? (uint32_t)atoi(argv[2]) : 300;
	int reps = argc > 3 ? atoi(argv[3]) : 5, r;
	uint64_t s1, s2, s3;
	size_t n, off = 16384 / 2, off2 = 8192 / 2;
	int fd = open("/dev/dri/card0", O_RDWR);
	uint16_t *d1, *d2, *d3, *h1, *h2, *h3, *a1, *a2, *a3;

	if (fd < 0) { perror("card0"); return 1; }
	d1 = dumb(fd, w, h, &s1); d2 = dumb(fd, w, h, &s2); d3 = dumb(fd, w, h, &s3);
	if (!d1 || !d2 || !d3) { perror("create dumb (is CMA free?)"); return 1; }
	n = s1 / 2 - off;			/* same element count in every arm */
	h1 = malloc(s1); h2 = malloc(s1); h3 = malloc(s1);
	if (posix_memalign((void **)&a1, 262144, s1) ||
	    posix_memalign((void **)&a2, 262144, s1) ||
	    posix_memalign((void **)&a3, 262144, s1) || !h1 || !h2 || !h3) {
		perror("heap"); return 1;
	}
	memset(d1, 1, s1); memset(d2, 2, s1); memset(d3, 0, s1);
	memset(h1, 1, s1); memset(h2, 2, s1); memset(h3, 0, s1);
	memset(a1, 1, s1); memset(a2, 2, s1); memset(a3, 0, s1);
	printf("m2bench %ux%u, %zu px, dumb %p %p %p, heapal %p %p %p\n",
	       w, h, n, (void *)d1, (void *)d2, (void *)d3,
	       (void *)a1, (void *)a2, (void *)a3);
	for (r = 0; r < reps; r++)
		printf("M2 dumb %8.0f  dumbof %8.0f  heap %8.0f  heapal %8.0f us\n",
		       run(d1, d2, d3, n), run(d1, d2 + off, d3 + off2, n),
		       run(h1, h2, h3, n), run(a1, a2, a3, n));
	return 0;
}
